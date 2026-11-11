/*
 * SpotiBacan · downloader.cpp (ver downloader.hpp)
 *
 * Por cancion: metadatos por Mercury (hm://metadata/3/track), clave de audio al AP, enlace del CDN por storage-resolve
 * (los tres pasos son los de cspot, QueuedTrack, los mismos que usa la reproduccion) y despues el archivo entero por
 * curl, descifrado al vuelo con AES-128-CTR. No se usa CDNAudioFile: nunca devuelve su ultimo trozo (readBytes corta
 * cuando position + bytes alcanza el final), y para guardar hace falta el archivo completo.
 */
#include "downloader.hpp"
#include "logger.hpp"
#include "net.hpp"
#include "cores.hpp"
#include "oggtag.hpp"

#include <curl/curl.h>
#include <mbedtls/aes.h>
#include <pb_decode.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "CSpotContext.h"
#include "LoginBlob.h"
#include "MercurySession.h"
#include "TrackQueue.h"
#include "TrackReference.h"
#include "WrappedSemaphore.h"
#include "protobuf/metadata.pb.h"

// En downloader_token.cpp: spotify_api.hpp trae el Track de la biblioteca, que choca con el Track de metadata.pb.h.
std::string downloaderWebToken();

namespace {

constexpr const char* ROOT = "sdmc:/music/SpotiBacan";
constexpr const char* CREDENTIALS[] = { "sdmc:/config/SpotiBacan/credentials.json", "sdmc:/config/Switify/credentials.json" };
constexpr size_t SPOTIFY_HEADER = 167;   // pagina Ogg senuelo que Spotify antepone al Vorbis real
const uint8_t AUDIO_IV[16] = { 0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb, 0xcf, 0x77,
                               0xeb, 0xe8, 0xbc, 0x64, 0x3f, 0x63, 0x0d, 0x93 };

std::string readFile(const char* path) {
    std::string s;
    FILE* f = fopen(path, "rb");
    if (!f) return s;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

/** Un nombre que FAT acepta: sin \ / : * ? " < > |, sin puntos ni espacios al final, y no demasiado largo. */
std::string safeName(std::string s, const char* fallback) {
    std::string out;
    for (char c : s) {
        if (strchr("\\/:*?\"<>|", c) || (unsigned char)c < 0x20) out.push_back('_');
        else out.push_back(c);
    }
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    if (out.size() > 80) {
        out.resize(80);
        // no dejar un caracter UTF-8 cortado a la mitad
        while (!out.empty() && ((unsigned char)out.back() & 0xC0) == 0x80) out.pop_back();
        if (!out.empty() && ((unsigned char)out.back() & 0x80)) out.pop_back();
    }
    return out.empty() ? std::string(fallback) : out;
}

void makeDirs(const std::string& path) {
    for (size_t i = strlen("sdmc:/"); i < path.size(); ++i)
        if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0777);
    mkdir(path.c_str(), 0777);
}

bool exists(const std::string& p) {
    struct stat s;
    return stat(p.c_str(), &s) == 0 && s.st_size > 0;
}

/** Escribe lo descifrado, saltando la cabecera de Spotify. */
struct Sink {
    FILE* f = nullptr;
    mbedtls_aes_context aes;
    uint8_t counter[16];
    uint8_t stream[16];
    size_t ncOff = 0;
    size_t offset = 0;   // bytes cifrados recibidos
    std::atomic<int64_t>* got = nullptr;
    std::atomic<int64_t>* total = nullptr;
    std::atomic<bool>* cancel = nullptr;
    bool writeFailed = false;
    uint8_t buf[16 * 1024];
};

size_t onData(char* data, size_t size, size_t n, void* user) {
    Sink& s = *static_cast<Sink*>(user);
    const size_t len = size * n;
    if (s.cancel && s.cancel->load()) return 0;
    size_t done = 0;
    while (done < len) {
        const size_t chunk = std::min(len - done, sizeof s.buf);
        mbedtls_aes_crypt_ctr(&s.aes, chunk, &s.ncOff, s.counter, s.stream,
                              reinterpret_cast<const uint8_t*>(data) + done, s.buf);
        size_t skip = 0;
        if (s.offset < SPOTIFY_HEADER) skip = std::min(chunk, SPOTIFY_HEADER - s.offset);
        if (chunk > skip && fwrite(s.buf + skip, 1, chunk - skip, s.f) != chunk - skip) {
            s.writeFailed = true;
            return 0;
        }
        s.offset += chunk;
        done += chunk;
    }
    if (s.got) s.got->store(int64_t(s.offset));
    return len;
}

int onProgress(void* user, curl_off_t dlTotal, curl_off_t, curl_off_t, curl_off_t) {
    Sink& s = *static_cast<Sink*>(user);
    if (s.total && dlTotal > 0) s.total->store(int64_t(dlTotal));
    return (s.cancel && s.cancel->load()) ? 1 : 0;
}

/** Espera a que el paso pendiente de QueuedTrack termine (o se pase el tiempo). */
bool waitWhile(cspot::QueuedTrack& qt, cspot::QueuedTrack::State pending, std::mutex& m,
               bell::WrappedSemaphore& sem, int ms, const std::atomic<bool>& quit) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until && !quit.load()) {
        {
            std::scoped_lock l(m);
            if (qt.state != pending) return true;
        }
        sem.twait(200);
    }
    std::scoped_lock l(m);
    return qt.state != pending;
}

}  // namespace

Downloader& Downloader::instance() {
    static Downloader d;
    return d;
}

Downloader::~Downloader() { shutdown(); }

std::string Downloader::pathFor(const DlJob& j) {
    const std::string artist = safeName(j.artist, "Artista desconocido");
    const std::string album = safeName(j.album, "Sin album");
    char num[8] = "";
    if (j.trackNo > 0) snprintf(num, sizeof num, "%02d - ", j.trackNo);
    return std::string(ROOT) + "/" + artist + "/" + album + "/" + num + safeName(j.title, "Cancion") + ".ogg";
}

namespace {
std::string lower(std::string s) {
    for (char& c : s) c = char(tolower((unsigned char)c));
    return s;
}
void walk(const std::string& dir, std::unordered_set<std::string>& out, int depth) {
    if (depth > 4) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        const std::string full = dir + "/" + n;
        if (e->d_type == DT_DIR) walk(full, out, depth + 1);
        else if (n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".ogg") == 0) out.insert(full);
    }
    closedir(d);
}
}  // namespace

bool Downloader::isDownloaded(const DlJob& j) {
    if (!scanStarted_.exchange(true)) {
        scan_ = std::thread([this] {
            std::unordered_set<std::string> found;
            walk(ROOT, found, 0);
            {
                std::scoped_lock l(savedM_);
                for (const auto& f : found) { saved_.insert(lower(f)); savedReal_.insert(f); }
            }
            ++gen_;
        });
    }
    const std::string p = lower(pathFor(j));
    std::scoped_lock l(savedM_);
    return saved_.count(p) != 0;
}

std::vector<std::string> Downloader::savedList() {
    isDownloaded(DlJob{});   // arranca la lectura de la carpeta si no estaba
    std::vector<std::string> out;
    {
        std::scoped_lock l(savedM_);
        out.reserve(savedReal_.size());
        for (const auto& p : savedReal_) out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

void Downloader::queueSnapshot(DlJob& current, std::vector<DlJob>& pending) {
    std::scoped_lock l(m_);
    current = st_.current.empty() ? DlJob{} : current_;
    pending.assign(queue_.begin(), queue_.end());
}

void Downloader::markSaved(const std::string& path) {
    {
        std::scoped_lock l(savedM_);
        saved_.insert(lower(path));
        savedReal_.insert(path);
    }
    ++gen_;
}

int Downloader::enqueue(const std::vector<DlJob>& jobs) {
    int added = 0;
    {
        std::scoped_lock l(m_);
        for (const auto& j : jobs) {
            if (j.uri.rfind("spotify:track:", 0) != 0) continue;   // episodios y archivos locales, no
            if (isDownloaded(j)) continue;
            bool queued = false;
            for (const auto& q : queue_) if (q.uri == j.uri) { queued = true; break; }
            if (queued) continue;
            queue_.push_back(j);
            ++added;
        }
        if (added) st_.active = true;
        st_.pending = int(queue_.size());
        if (added && !worker_.joinable()) {
            quit_ = false;
            worker_ = std::thread([this] { run(); });
        }
    }
    cv_.notify_all();
    LOG("Downloader: %d canciones en cola", added);
    return added;
}

void Downloader::cancelAll() {
    std::scoped_lock l(m_);
    queue_.clear();
    st_.pending = 0;
    cancelCurrent_ = true;
}

void Downloader::shutdown() {
    {
        std::scoped_lock l(m_);
        queue_.clear();
        quit_ = true;
        cancelCurrent_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (scan_.joinable()) scan_.join();
    dropSession();
}

Downloader::Status Downloader::status() {
    std::scoped_lock l(m_);
    Status s = st_;
    const int64_t t = total_.load();
    s.progress = t > 0 ? float(double(got_.load()) / double(t)) : 0.f;
    return s;
}

bool Downloader::ensureSession(std::string& err) {
    if (sessionUp_.load() && ctx_) return true;
    dropSession();
    std::string creds;
    for (const char* p : CREDENTIALS) {
        creds = readFile(p);
        if (!creds.empty()) break;
    }
    if (creds.empty()) {
        err = "Primero vincula tu cuenta de Spotify";
        return false;
    }
    try {
        auto blob = std::make_shared<cspot::LoginBlob>("SpotiBacan");
        blob->loadJson(creds);
        ctx_ = cspot::Context::createFromBlob(blob);
        ctx_->config.audioFormat = AudioFormat_OGG_VORBIS_160;
        ctx_->session->connectWithRandomAp();
        ctx_->config.authData = ctx_->session->authenticate(blob);
        if (ctx_->config.authData.empty()) {
            err = "Spotify no acepto la sesion guardada; probablemente hay que vincular la cuenta de nuevo";
            ctx_.reset();
            return false;
        }
        ctx_->session->startTask();
        sessionUp_ = true;
        auto ctx = ctx_;
        packets_ = std::thread([this, ctx] {
            cores::moveHere(cores::Net);
            while (sessionUp_.load()) {
                try {
                    ctx->session->handlePacket();
                } catch (const std::exception& e) {
                    LOG("Downloader: la sesion se corto (%s)", e.what());
                    sessionUp_ = false;
                } catch (...) {
                    sessionUp_ = false;
                }
            }
        });
        LOG("Downloader: sesion con Spotify lista");
        return true;
    } catch (const std::exception& e) {
        err = std::string("No se pudo conectar con Spotify: ") + e.what();
    } catch (...) {
        err = "No se pudo conectar con Spotify";
    }
    ctx_.reset();
    return false;
}

void Downloader::dropSession() {
    const bool wasUp = sessionUp_.exchange(false);
    if (ctx_ && wasUp) {
        try { ctx_->session->disconnect(); } catch (...) {}
    }
    if (packets_.joinable()) packets_.join();
    ctx_.reset();
}

Downloader::Result Downloader::fetch(const DlJob& j, std::string& err) {
    if (!ensureSession(err)) return Result::Failed;
    // las restricciones por pais de cada cancion se comparan con esto; el AP lo manda poco despues del login
    if (ctx_->config.countryCode.empty()) ctx_->config.countryCode = ctx_->session->getCountryCode();

    // Declarados antes que qt: qt se destruye primero y da de baja sus pedidos pendientes, que los usan.
    std::mutex m;
    auto sem = std::make_shared<bell::WrappedSemaphore>();
    Track pbTrack = Track_init_zero;
    Episode pbEpisode = Episode_init_zero;
    cspot::TrackReference ref;
    ref.uri = j.uri;
    ref.decodeURI();
    auto qt = std::make_shared<cspot::QueuedTrack>(ref, ctx_, 0);
    struct Release {
        Track* t; Episode* e;
        ~Release() { pb_release(Track_fields, t); pb_release(Episode_fields, e); }
    } release{ &pbTrack, &pbEpisode };

    using S = cspot::QueuedTrack::State;
    qt->stepLoadMetadata(&pbTrack, &pbEpisode, m, sem);
    if (!waitWhile(*qt, S::PENDING_META, m, *sem, 15000, quit_)) { err = "Spotify no respondio (metadatos)"; return Result::Failed; }
    { std::scoped_lock l(m); if (qt->state == S::FAILED) { err = "Esta cancion no esta disponible en tu pais o en este formato"; return Result::Unavailable; } }

    qt->stepLoadAudioFile(m, sem);
    if (!waitWhile(*qt, S::PENDING_KEY, m, *sem, 15000, quit_)) { err = "Spotify no respondio (clave de audio)"; return Result::Failed; }
    { std::scoped_lock l(m); if (qt->state == S::FAILED) {
        err = "Spotify nego la clave de audio. Suele pasar con cuentas Premium creadas hace poco; si pasa con todas las canciones, probablemente es eso";
        return Result::KeyDenied; } }

    const std::string token = downloaderWebToken();
    if (token.empty()) { err = "No se pudo obtener el permiso de Spotify (token)"; return Result::Failed; }
    qt->stepLoadCDNUrl(token);
    std::string url;
    std::vector<uint8_t> key;
    cspot::TrackInfo info;
    {
        std::scoped_lock l(m);
        if (qt->state != S::READY) { err = "Spotify no dio el enlace del archivo"; return Result::Failed; }
        url = qt->getCdnUrl();
        info = qt->trackInfo;
        key = qt->getAudioKey();
    }
    if (key.size() != 16) { err = "Clave de audio invalida"; return Result::Failed; }

    const std::string dst = pathFor(j);
    makeDirs(dst.substr(0, dst.find_last_of('/')));
    const std::string tmp = dst + ".part";
    Sink s;
    s.f = fopen(tmp.c_str(), "wb");
    if (!s.f) { err = "No se pudo escribir en la tarjeta"; return Result::Failed; }
    setvbuf(s.f, nullptr, _IOFBF, 256 * 1024);
    mbedtls_aes_init(&s.aes);
    mbedtls_aes_setkey_enc(&s.aes, key.data(), 128);
    memcpy(s.counter, AUDIO_IV, 16);
    s.got = &got_;
    s.total = &total_;
    s.cancel = &cancelCurrent_;
    got_ = 0;
    total_ = 0;

    std::string netErr;
    if (!net::begin(netErr)) { fclose(s.f); remove(tmp.c_str()); mbedtls_aes_free(&s.aes); err = netErr; return Result::Failed; }
    CURL* c = curl_easy_init();
    CURLcode rc = CURLE_FAILED_INIT;
    long status = 0;
    if (c) {
        curl_easy_setopt(c, CURLOPT_URL, url.c_str());
        curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 256L);
        curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, onData);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, onProgress);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, &s);
        curl_easy_setopt(c, CURLOPT_BUFFERSIZE, 128L * 1024);
        rc = curl_easy_perform(c);
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(c);
    }
    mbedtls_aes_free(&s.aes);
    const bool closed = fclose(s.f) == 0;
    if (cancelCurrent_.load()) { remove(tmp.c_str()); return Result::Cancelled; }
    if (rc != CURLE_OK || !closed || s.writeFailed || s.offset <= SPOTIFY_HEADER) {
        remove(tmp.c_str());
        if (s.writeFailed || !closed) err = "No se pudo escribir en la tarjeta (probablemente esta llena)";
        else err = "Fallo la descarga (" + std::string(curl_easy_strerror(rc)) + ", HTTP " + std::to_string(status) + ")";
        return Result::Failed;
    }
    // Etiquetas y caratula, con los datos de Spotify (los de la lista pueden venir incompletos). Si algo sale mal se
    // guarda igual, sin etiquetas: el audio ya esta bien.
    {
        oggtag::Comments tags;
        auto pick = [](const std::string& a, const std::string& b) { return a.empty() ? b : a; };
        tags.emplace_back("TITLE", pick(info.name, j.title));
        tags.emplace_back("ARTIST", pick(info.artist, j.artist));
        tags.emplace_back("ALBUM", pick(info.album, j.album));
        if (j.artist.size()) tags.emplace_back("ALBUMARTIST", j.artist);
        const int num = info.number ? int(info.number) : j.trackNo;
        if (num > 0) tags.emplace_back("TRACKNUMBER", std::to_string(num));
        if (info.discNumber > 0) tags.emplace_back("DISCNUMBER", std::to_string(info.discNumber));
        tags.emplace_back("SPOTIFY_URI", j.uri);
        std::vector<uint8_t> jpeg;
        if (!info.imageUrl.empty()) {
            std::string body, e;
            if (net::get(info.imageUrl, body, e) && body.size() > 4 && (uint8_t)body[0] == 0xFF && (uint8_t)body[1] == 0xD8)
                jpeg.assign(body.begin(), body.end());
        }
        const std::string tagged = dst + ".tag";
        if (oggtag::retag(tmp, tagged, tags, jpeg)) {
            remove(tmp.c_str());
            rename(tagged.c_str(), tmp.c_str());
        } else {
            LOG("Downloader: no se pudieron escribir las etiquetas de %s; se guarda sin ellas", dst.c_str());
        }
    }
    remove(dst.c_str());
    if (rename(tmp.c_str(), dst.c_str()) != 0) { remove(tmp.c_str()); err = "No se pudo escribir en la tarjeta"; return Result::Failed; }
    markSaved(dst);
    LOG("Downloader: guardada %s (%zu bytes)", dst.c_str(), s.offset - SPOTIFY_HEADER);
    return Result::Ok;
}

void Downloader::run() {
    cores::moveHere(cores::Net);
    int keyDeniedInARow = 0;
    while (true) {
        DlJob j;
        {
            std::unique_lock l(m_);
            cv_.wait_for(l, std::chrono::seconds(60), [this] { return quit_.load() || !queue_.empty(); });
            if (quit_.load()) break;
            if (queue_.empty()) {
                // un minuto sin nada que bajar: se suelta la sesion (se abre otra al volver a pedir)
                st_.active = false;
                st_.current.clear();
                l.unlock();
                dropSession();
                continue;
            }
            j = queue_.front();
            queue_.pop_front();
            current_ = j;
            st_.pending = int(queue_.size());
            st_.current = j.title;
            st_.active = true;
            cancelCurrent_ = false;
        }
        got_ = 0;
        total_ = 0;
        std::string err;
        Result r = Result::Failed;
        try {
            r = fetch(j, err);
        } catch (const std::exception& e) {
            err = e.what();
        } catch (...) {
            err = "Error inesperado";
        }
        if (r == Result::Failed && !sessionUp_.load()) {
            // la sesion se cayo a mitad: un reintento con una nueva
            dropSession();
            try { r = fetch(j, err); } catch (...) { r = Result::Failed; }
        }
        std::scoped_lock l(m_);
        switch (r) {
        case Result::Ok: ++st_.done; keyDeniedInARow = 0; break;
        case Result::KeyDenied: ++st_.keyDenied; ++keyDeniedInARow; st_.lastError = err; break;
        case Result::Cancelled: break;
        default: ++st_.failed; st_.lastError = err; break;
        }
        if (r != Result::Ok && r != Result::Cancelled) LOG("Downloader: %s: %s", j.title.c_str(), err.c_str());
        // Si Spotify niega la clave tres veces seguidas es la cuenta, no la cancion: no tiene sentido seguir pidiendo.
        if (keyDeniedInARow >= 3) {
            queue_.clear();
            keyDeniedInARow = 0;
        }
        st_.current.clear();
        st_.pending = int(queue_.size());
        st_.active = !queue_.empty();
    }
    std::scoped_lock l(m_);
    st_.active = false;
    st_.current.clear();
}
