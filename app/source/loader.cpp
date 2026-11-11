/*
 * Gatefold NX · loader.cpp
 */
#include "loader.hpp"
#include "coverio.hpp"
#include "net.hpp"

#include <SDL2/SDL_image.h>
#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "cores.hpp"
#include "gfx.hpp"
#include "library.hpp"
#ifdef GF_WINDOWED
std::vector<uint8_t> mediaJpeg(SDL_Surface* s);   // metacache.cpp
#endif

namespace {

std::string urlEncodeStr(const std::string& value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string escaped;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            escaped += c;
        } else if (c == ' ') {
            escaped += "+";
        } else {
            escaped += '%';
            escaped += hex[(c >> 4) & 0xF];
            escaped += hex[c & 0xF];
        }
    }
    return escaped;
}

std::string extractJsonField(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    size_t p = json.find(needle);
    if (p == std::string::npos) {
        needle = "\"" + key + "\" : \"";
        p = json.find(needle);
        if (p == std::string::npos) return "";
    }
    size_t start = p + needle.size();
    size_t end = start;
    while (end < json.size()) {
        if (json[end] == '\\') { end += 2; continue; }
        if (json[end] == '"') break;
        end++;
    }
    std::string raw = json.substr(start, end - start);
    std::string res;
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            char n = raw[++i];
            if (n == 'n') res += '\n';
            else if (n == 'r') res += '\r';
            else if (n == 't') res += '\t';
            else if (n == '"') res += '"';
            else if (n == '\\') res += '\\';
            else if (n == 'u' && i + 4 < raw.size()) {
                unsigned int cp = 0;
                for (int h = 0; h < 4; h++) {
                    char c = raw[++i];
                    cp <<= 4;
                    if (c >= '0' && c <= '9') cp |= (c - '0');
                    else if (c >= 'a' && c <= 'f') cp |= (c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F') cp |= (c - 'A' + 10);
                }
                if (cp < 0x80) res += char(cp);
                else if (cp < 0x800) {
                    res += char(0xC0 | ((cp >> 6) & 0x1F));
                    res += char(0x80 | (cp & 0x3F));
                } else {
                    res += char(0xE0 | ((cp >> 12) & 0x0F));
                    res += char(0x80 | ((cp >> 6) & 0x3F));
                    res += char(0x80 | (cp & 0x3F));
                }
            } else res += n;
        } else {
            res += raw[i];
        }
    }
    return res;
}

std::string fetchOnlineLyrics(const std::string& artist, const std::string& title, double duration) {
    if (title.empty()) return "";
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char b : artist) { hash ^= b; hash *= UINT64_C(1099511628211); }
    hash ^= '\t'; hash *= UINT64_C(1099511628211);
    for (const unsigned char b : title) { hash ^= b; hash *= UINT64_C(1099511628211); }
    char hex[17] = {};
    snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));

    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/SpotiBacan", 0777);
    mkdir("sdmc:/config/SpotiBacan/lyrics-cache", 0777);
    std::string cachePath = std::string("sdmc:/config/SpotiBacan/lyrics-cache/") + hex + ".lrc";

    if (FILE* f = fopen(cachePath.c_str(), "rb")) {
        char buf[8192];
        std::string cached;
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) cached.append(buf, n);
        fclose(f);
        if (!cached.empty()) return cached;
    }

    std::string netErr;
    if (!net::begin(netErr)) return "";

    std::string url = "https://lrclib.net/api/get?artist_name=" + urlEncodeStr(artist) +
                      "&track_name=" + urlEncodeStr(title);
    if (duration > 0) url += "&duration=" + std::to_string(int(duration));

    std::string json;
    bool ok = net::get(url, json, netErr, nullptr);
    if (!ok || json.empty()) {
        std::string searchUrl = "https://lrclib.net/api/search?q=" + urlEncodeStr(artist + " " + title);
        ok = net::get(searchUrl, json, netErr, nullptr);
    }
    if (!ok || json.empty()) return "";

    std::string lyrics = extractJsonField(json, "syncedLyrics");
    if (lyrics.empty()) lyrics = extractJsonField(json, "plainLyrics");
    if (!lyrics.empty()) {
        if (FILE* f = fopen(cachePath.c_str(), "wb")) {
            fwrite(lyrics.data(), 1, lyrics.size(), f);
            fclose(f);
        }
    }
    return lyrics;
}

}  // namespace

void TrackLoader::Result::release() {
    if (cover) SDL_FreeSurface(cover);
    if (blur) SDL_FreeSurface(blur);
    if (frost) SDL_FreeSurface(frost);
#ifdef GF_WINDOWED
    if (mediaCover) SDL_FreeSurface(mediaCover);
    mediaCover = nullptr;
#endif
    cover = blur = frost = nullptr;
}

namespace {
/**
 * The cover frosted (Automix's join, Now playing): the 64×64 sample blurred by three passes of a box blur 9 texels wide each
 * way (close to a Gaussian of σ ≈ 4.5 texels), edges held. Stretched over the cover with linear filtering it is smooth;
 * the backdrop's 12×12 showed its grid there (the user, 2026-10-28: "frosted should be more like blur").
 */
SDL_Surface* frostOf(const SDL_Surface* thumb) {
    constexpr int N = 64, R = 4;
    SDL_Surface* out = SDL_CreateRGBSurfaceWithFormat(0, N, N, 32, SDL_PIXELFORMAT_RGBA32);
    if (!out) return nullptr;
    std::vector<float> a(N * N * 3), b(N * N * 3);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++)
            for (int c = 0; c < 3; c++) a[(y * N + x) * 3 + c] = static_cast<const uint8_t*>(thumb->pixels)[y * thumb->pitch + x * 4 + c];
    for (int pass = 0; pass < 3; pass++)
        for (int dir = 0; dir < 2; dir++) {   // across, then down
            for (int y = 0; y < N; y++)
                for (int x = 0; x < N; x++)
                    for (int c = 0; c < 3; c++) {
                        float s = 0;
                        for (int k = -R; k <= R; k++) {
                            const int xx = std::clamp(dir ? x : x + k, 0, N - 1), yy = std::clamp(dir ? y + k : y, 0, N - 1);
                            s += a[(yy * N + xx) * 3 + c];
                        }
                        b[(y * N + x) * 3 + c] = s / (2 * R + 1);
                    }
            a.swap(b);
        }
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            uint8_t* p = static_cast<uint8_t*>(out->pixels) + y * out->pitch + x * 4;
            for (int c = 0; c < 3; c++) p[c] = uint8_t(std::clamp(int(std::lround(a[(y * N + x) * 3 + c])), 0, 255));
            p[3] = 255;
        }
    return out;
}
}  // namespace

void TrackLoader::start() {
    if (thread_.joinable()) return;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
}

void TrackLoader::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    if (hasDone_) done_.release();
    hasDone_ = hasReq_ = false;
}

void TrackLoader::request(Request r) {
    {
        std::lock_guard<std::mutex> g(m_);
        if (hasReq_ && req_.readInfo && !r.readInfo) {
            r.readInfo = true;
            if (r.title.empty()) r.title = req_.title;
            if (r.artist.empty()) r.artist = req_.artist;
            if (r.album.empty()) r.album = req_.album;
            if (r.duration <= 0) r.duration = req_.duration;
        }
        req_ = std::move(r);
        hasReq_ = true;
    }
    cv_.notify_one();
}

bool TrackLoader::take(Result& out) {
    std::lock_guard<std::mutex> g(m_);
    if (!hasDone_) return false;
    out = std::move(done_);
    done_ = Result{};
    hasDone_ = false;
    return true;
}

void TrackLoader::run() {
    cores::moveHere(cores::Loader);
    for (;;) {
        Request r;
        {
            std::unique_lock<std::mutex> g(m_);
            cv_.wait(g, [&] { return quit_ || hasReq_; });
            if (quit_) return;
            r = std::move(req_);
            hasReq_ = false;
        }
        Result res;
        work(r, res);
        std::lock_guard<std::mutex> g(m_);
        if (hasDone_) done_.release();   // not taken yet, and already old
        done_ = std::move(res);
        hasDone_ = true;
    }
}

void TrackLoader::work(const Request& r, Result& out) {
    out.gen = r.gen;
    out.readInfo = r.readInfo;
    std::vector<uint8_t> bytes;
    if (!r.myCover.empty()) {
        std::string localFile = r.myCover;
        if (r.myCover.rfind("http://", 0) == 0 || r.myCover.rfind("https://", 0) == 0) {
            uint64_t hash = UINT64_C(14695981039346656037);
            for (const unsigned char byte : r.myCover) {
                hash ^= byte;
                hash *= UINT64_C(1099511628211);
            }
            char hex[17] = {};
            snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
            mkdir("sdmc:/config", 0777);
            mkdir("sdmc:/config/SpotiBacan", 0777);
            mkdir("sdmc:/config/SpotiBacan/artwork-cache", 0777);
            localFile = std::string("sdmc:/config/SpotiBacan/artwork-cache/") + hex + ".img";
            FILE* test = fopen(localFile.c_str(), "rb");
            if (!test) {
                std::string err;
                net::begin(err);
                net::download(r.myCover, localFile, err, nullptr);
            } else {
                fclose(test);
            }
        }
        if (FILE* f = fopen(localFile.c_str(), "rb")) {
            char buf[16384];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
            fclose(f);
        }
    }
    if (r.readInfo) {
        std::string art = r.artist;
        std::string tit = r.title;
        if (r.path.rfind("spotify:", 0) != 0) {
            readTrackInfo(r.path, out.info, bytes.empty(), true);
            if (bytes.empty()) bytes = std::move(out.info.cover);
            out.info.cover.clear();
            out.info.cover.shrink_to_fit();
            if (art.empty()) art = out.info.artist;
            if (tit.empty()) tit = out.info.title;
            if (out.lyrics.empty()) out.lyrics = std::move(out.info.lyrics);
            out.info.lyrics.clear();
        }
        if (out.lyrics.empty() && !tit.empty()) {
            static std::string s_cachedKey;
            static std::string s_cachedLyrics;
            const std::string key = art + "\t" + tit;
            if (!s_cachedKey.empty() && key == s_cachedKey) {
                out.lyrics = s_cachedLyrics;
            } else {
                out.lyrics = fetchOnlineLyrics(art, tit, r.duration > 0 ? r.duration : out.info.duration);
                s_cachedKey = key;
                s_cachedLyrics = out.lyrics;
            }
            if (!out.lyrics.empty()) {
                out.lyricsProvider = "LRCLIB";
                out.sidecar = false;
            }
        }
    } else if (bytes.empty() && r.path.rfind("spotify:", 0) != 0) {
        TrackInfo info;
        readTrackInfo(r.path, info, true, false);
        bytes = std::move(info.cover);
    }
    if (bytes.empty()) bytes = findFolderCover(r.folder);
    if (bytes.empty()) return;

    SDL_Surface* rgba = nullptr;
    if (SDL_Surface* src = loadImage(bytes, r.path.c_str())) {
        rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_RGBA32, 0);
        SDL_FreeSurface(src);
    }
    if (!rgba) return;
    // The cover is drawn at most ~450 px (the Canvas); 512 keeps it sharp without aliasing while it floats and zooms.
#ifdef GF_WINDOWED
    out.cover = Gfx::shrink(rgba, 1536);   // … three times that on a big window
    out.mediaCover = Gfx::shrink(rgba, 256);
    if (out.mediaCover) out.mediaJpeg = mediaJpeg(out.mediaCover);
#else
    out.cover = Gfx::shrink(rgba, 512);
#endif
    // 64×64 for the palette (as Gatefold samples it), 12×12 stretched with linear filtering for the backdrop blur.
    SDL_Surface* thumb = SDL_CreateRGBSurfaceWithFormat(0, 64, 64, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_Surface* tiny = SDL_CreateRGBSurfaceWithFormat(0, 12, 12, 32, SDL_PIXELFORMAT_RGBA32);
    if (thumb && tiny && SDL_SoftStretchLinear(rgba, nullptr, thumb, nullptr) == 0) {
        out.px.resize(64 * 64 * 4);
        for (int y = 0; y < 64; y++) memcpy(&out.px[y * 256], static_cast<uint8_t*>(thumb->pixels) + y * thumb->pitch, 256);
        out.pal = paletteFromPixels(out.px.data(), 64, 64, r.opts);
        if (SDL_SoftStretchLinear(thumb, nullptr, tiny, nullptr) == 0) { out.blur = tiny; tiny = nullptr; }
        out.frost = frostOf(thumb);
    }
    if (thumb) SDL_FreeSurface(thumb);
    if (tiny) SDL_FreeSurface(tiny);
    SDL_FreeSurface(rgba);
}
