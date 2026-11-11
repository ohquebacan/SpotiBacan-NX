/*
 * Songate · bgclient_switch.cpp
 * Switify Spotify background sysmodule IPC client integration.
 */
#include "bgclient.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <SDL2/SDL.h>
#include <switch.h>

#include "switify/ipc_client.h"
#include "logger.hpp"

namespace {
std::mutex g_ipcMutex;
std::mutex g_browseOpMutex;
std::atomic<bool> g_ipcBusy = false;

struct IpcBusyScope {
    IpcBusyScope() { g_ipcBusy.store(true); }
    ~IpcBusyScope() { g_ipcBusy.store(false); }
};
constexpr u64 kSysmoduleProgramId = 0x4200000000535754ull;
const char* const kReadyPath = "sdmc:/config/SpotiBacan/background-ready.log";
const char* const kReadyPathLegacy = "sdmc:/config/Switify/background-ready.log";

bool isSysmoduleRunning(uint64_t* out_pid) {
    if (R_FAILED(pmshellInitialize())) return false;
    uint64_t pid = 0;
    Result rc = pmshellGetProcessId(&pid, kSysmoduleProgramId);
    pmshellExit();
    if (out_pid) *out_pid = pid;
    return R_SUCCEEDED(rc) && pid != 0;
}

bool isSysmoduleReadyAt(const char* path, uint64_t pid) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[128] = {};
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return false;
    }
    fclose(f);
    unsigned long long markerPid = 0;
    unsigned result = UINT32_MAX;
    const int parsed = std::sscanf(line, "pass-v043 %llX 0x%X", &markerPid, &result);
    return parsed == 2 && markerPid == pid && result == 0;
}

bool isSysmoduleReady(uint64_t pid) {
    return isSysmoduleReadyAt(kReadyPath, pid) || isSysmoduleReadyAt(kReadyPathLegacy, pid);
}

double bgClock() { return SDL_GetTicks64() / 1000.0; }
}  // namespace

bool BgClient::connect() {
    if (connected_) return true;

    uint64_t pid = 0;
    if (!isSysmoduleRunning(&pid)) {
        LOG("BgClient::connect: sysmodule (0x%016llX) is not running", static_cast<unsigned long long>(kSysmoduleProgramId));
        // Launch sysmodule in the background without blocking
        if (R_SUCCEEDED(pmshellInitialize())) {
            NcmProgramLocation loc{};
            loc.program_id = kSysmoduleProgramId;
            loc.storageID = NcmStorageId_None;
            uint64_t new_pid = 0;
            Result rc = pmshellLaunchProgram(0, &loc, &new_pid);
            pmshellExit();
            LOG("BgClient::connect: launched sysmodule rc=0x%08X, new_pid=%llu", static_cast<unsigned>(rc), static_cast<unsigned long long>(new_pid));
        }
        return false;
    }

    if (!isSysmoduleReady(pid)) {
        LOG("BgClient::connect: sysmodule pid=%llu is starting up (waiting for readiness marker)...", static_cast<unsigned long long>(pid));
        return false;
    }

    LOG("BgClient::connect: sysmodule pid=%llu is verified ready, calling switifyIpcInitialize()...", static_cast<unsigned long long>(pid));
    Result rc = 0;
    {
        std::scoped_lock lock(g_ipcMutex);
        rc = switifyIpcInitialize();
    }
    connected_ = R_SUCCEEDED(rc);
    LOG("BgClient::connect: switifyIpcInitialize: 0x%08X (connected=%d)", static_cast<unsigned>(rc), connected_ ? 1 : 0);
    return connected_;
}

void BgClient::disconnect() {
    if (connected_) {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcExit();
        connected_ = false;
    }
}

void BgClient::checkArtwork() {
    FILE* f = fopen("sdmc:/config/SpotiBacan/current-artwork-url", "r");
    if (!f) f = fopen("sdmc:/config/Switify/current-artwork-url", "r");
    if (f) {
        char buf[256] = {};
        if (fgets(buf, sizeof(buf), f)) {
            size_t len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n' || buf[len - 1] == ' ')) {
                buf[--len] = '\0';
            }
            if (len > 0 && artworkUrl_ != buf) {
                artworkUrl_ = buf;
                uint64_t hash = UINT64_C(14695981039346656037);
                for (size_t i = 0; i < len; ++i) {
                    hash ^= static_cast<unsigned char>(buf[i]);
                    hash *= UINT64_C(1099511628211);
                }
                char hex[17] = {};
                snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
                std::string primary = std::string("sdmc:/config/SpotiBacan/artwork-cache/") + hex + ".img";
                FILE* tf = fopen(primary.c_str(), "rb");
                if (tf) {
                    fclose(tf);
                    artworkCachePath_ = primary;
                } else {
                    std::string legacy = std::string("sdmc:/config/Switify/artwork-cache/") + hex + ".img";
                    FILE* lf = fopen(legacy.c_str(), "rb");
                    if (lf) {
                        fclose(lf);
                        artworkCachePath_ = legacy;
                    } else {
                        artworkCachePath_ = primary;
                    }
                }
            }
        }
        fclose(f);
    }
}

bool BgClient::poll(double) {
    if (g_ipcBusy.load()) return true;
    const double now = bgClock();
    if (connected_ && now - polledAt_ < 0.1 && now >= polledAt_) return true;   // SpotiBacan: no en cada cuadro
    if (!connected_) {
        if (!connect()) return false;
    }
    SwitifyIpcStatus s{};
    Result statusRc = 0;
    {
        std::scoped_lock lock(g_ipcMutex);
        statusRc = switifyIpcGetStatus(&s);
    }
    if (R_FAILED(statusRc)) {
        LOG("BgClient::poll: switifyIpcGetStatus failed, disconnecting");
        disconnect();
        return false;
    }

    switch (s.playback_state) {
        case SwitifyPlaybackState_Playing: st_.status = BgState::Playing; break;
        case SwitifyPlaybackState_Paused: st_.status = BgState::Paused; break;
        default: st_.status = BgState::Stopped; break;
    }

    st_.position = double(s.position_ms) / 1000.0;
    st_.duration = double(s.duration_ms) / 1000.0;
    st_.volume = float(s.volume_permille) / 1000.0f;
    st_.pos = int(s.queue_index);
    st_.track = int(s.queue_index);
    st_.queueCount = std::max(1u, s.queue_count);

    if (title_ != s.title || artist_ != s.artist || album_ != s.album) {
        title_ = s.title;
        artist_ = s.artist;
        album_ = s.album;
        st_.trackGen++;
        artUrlAtChange_ = artworkUrl_;
        artRecheckUntil_ = now + 8.0;
        artNextAt_ = now + 0.4;
        checkArtwork();
        if (artworkUrl_ != artUrlAtChange_) artRecheckUntil_ = 0;
    } else if (now < artRecheckUntil_ && now >= artNextAt_) {
        // la caratula de la cancion nueva todavia no estaba: cada 0,4 s hasta 8 s, hasta que cambie
        artNextAt_ = now + 0.4;
        checkArtwork();
        if (artworkUrl_ != artUrlAtChange_) artRecheckUntil_ = 0;
    } else if (artworkCachePath_.empty()) {
        static double s_artAt = -10;
        if (now - s_artAt > 2.0) { s_artAt = now; checkArtwork(); }
    }

    if (now < expectUntil_) {
        if (expectStatus_ >= 0) st_.status = expectStatus_;
        if (expectPos_ >= 0) st_.position = expectPos_;
    } else {
        expectStatus_ = -1;
        expectPos_ = -1;
    }
    polledAt_ = now;
    return true;
}

double BgClient::position(double) const {
    const double now = bgClock();
    double p = st_.position;
    if (st_.status == BgState::Playing) p += std::clamp(now - polledAt_, 0.0, 0.25);
    return st_.duration > 0 ? std::min(p, st_.duration) : p;
}

bool BgClient::setQueue(const BgQueue& q) {
    st_.queueCount = std::max(1, int(q.paths.size()));
    st_.pos = q.pos;
    return true;
}

bool BgClient::getQueue(BgQueue& q) {
    q.name = "SpotiBacán";
    q.pos = st_.pos;
    q.mode = 0;
    q.paths = { "spotify:track:current" };
    q.order = { 0 };
    return true;
}

void BgClient::playAt(int pos, bool paused, double start) {
    if (!connected_) return;
    {
        std::scoped_lock lock(g_ipcMutex);
        if (paused) switifyIpcPause();
        else switifyIpcPlay();
        if (start > 0) switifyIpcSeek(uint32_t(start * 1000.0));
    }
    st_.pos = pos;
    expectStatus_ = paused ? BgState::Paused : BgState::Playing;
    expectPos_ = start;
    expectUntil_ = polledAt_ + 0.3;
}

void BgClient::toggle(double) {
    const double now = bgClock();
    if (!connected_) return;
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcPlayPause();
    }
    expectStatus_ = st_.status == BgState::Playing ? BgState::Paused : BgState::Playing;
    expectUntil_ = now + 0.3;
}

void BgClient::pause(double) {
    const double now = bgClock();
    if (!connected_) return;
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcPause();
    }
    expectStatus_ = BgState::Paused;
    expectUntil_ = now + 0.3;
}

void BgClient::stop() {
    if (connected_) {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcPause();
    }
}

void BgClient::next() {
    if (connected_) {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcNext();
    }
}

void BgClient::prev() {
    if (connected_) {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcPrevious();
    }
}

void BgClient::seek(double t, double) {
    const double now = bgClock();
    if (!connected_) return;
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcSeek(uint32_t(t * 1000.0));
    }
    expectPos_ = t;
    expectUntil_ = now + 0.3;
    st_.position = t;
    polledAt_ = now;
}

void BgClient::setVolume(float v) {
    if (!connected_ || std::fabs(v - sentVolume_) < 0.0005f) return;
    sentVolume_ = v;
    std::scoped_lock lock(g_ipcMutex);
    switifyIpcSetVolume(uint32_t(std::clamp(v, 0.0f, 1.0f) * 1000.0f));
}

void BgClient::setStopAfter(bool on) {
    st_.stopAfter = on;
}

void BgClient::setLevelMode(int) {}
void BgClient::setSleep(float) {}
void BgClient::setCrossfade(float, bool) {}
void BgClient::setAutomixModel(int, const std::string&) {}
bool BgClient::eq(GfbgEq&) { return false; }
void BgClient::setEq(const GfbgEq&) {}
bool BgClient::pcm(float* out, int n) {
    if (!connected_ || st_.status != BgState::Playing || !out || n <= 0) return false;
    const double now = bgClock();
    static double s_lastAt = 0.0;
    if (now - s_lastAt < 0.033) return false;   // SpotiBacan: 30 por segundo, para que se mueva con soltura
    s_lastAt = now;

    const double pos = position(now);
    uint32_t seed = 2166136261u;
    for (unsigned char c : title_) { seed ^= c; seed *= 16777619u; }
    for (unsigned char c : artist_) { seed ^= c; seed *= 16777619u; }

    const double bpm = 110.0 + double(seed % 28);
    const double beat = pos * (bpm / 60.0);
    const double twoPi = 6.283185307179586;

    // Kick & bass envelope on quarter/eighth beats
    const double kickEnv = std::pow(0.5 + 0.5 * std::cos(beat * twoPi), 3.5);
    const double snareEnv = std::pow(0.5 + 0.5 * std::cos((beat - 0.5) * twoPi), 4.0);
    const double hatEnv = std::pow(0.5 + 0.5 * std::sin(beat * 2.0 * twoPi), 2.0);

    // SpotiBacan: senal inventada mas viva. Antes 28 bandas con modulacion lenta: muchas barras del espectro (sobre
    // todo en el Canvas con puntos LED) caian entre dos bandas y quedaban quietas. Ahora 64 bandas que cubren todo el
    // ancho, golpes de bombo/caja/platillo con ataque rapido y caida suave, y un poco de ruido propio en cada banda.
    static constexpr int kBands = 64;
    static double s_phase[kBands] = {};
    static float s_env[kBands] = {};
    static float s_noise[kBands] = {};
    static uint32_t s_rng = 0x9e3779b9u;
    auto rnd = [] { s_rng = s_rng * 1664525u + 1013904223u; return float(s_rng >> 8) / float(1u << 24); };
    double freqs[kBands];
    float amps[kBands];
    const double eighth = std::pow(0.5 + 0.5 * std::cos(beat * 2.0 * twoPi), 6.0);   // platillos en corcheas
    for (int k = 0; k < kBands; ++k) {
        const double frac = double(k) / double(kBands - 1);
        freqs[k] = 45.0 * std::pow(14500.0 / 45.0, frac);
        s_noise[k] = s_noise[k] * 0.55f + rnd() * 0.45f;
        const double wobble = 0.5 + 0.5 * std::sin(pos * (2.3 + (seed >> (k % 16) & 7) * 0.5) + k * 0.9);
        double drive;
        if (frac < 0.18) drive = 0.75 * kickEnv + 0.25 * s_noise[k];                                   // graves: el bombo
        else if (frac < 0.62) drive = 0.35 * snareEnv + 0.25 * kickEnv + 0.25 * s_noise[k] + 0.15 * wobble;   // medios
        else drive = 0.45 * std::max(hatEnv, eighth) + 0.35 * s_noise[k] + 0.2 * snareEnv;             // agudos
        const double base = 0.30 * (1.0 - 0.55 * frac);
        const float target = float(base * (0.12 + 0.88 * drive));
        // ataque rapido, caida suave: se nota cada golpe
        s_env[k] = target > s_env[k] ? s_env[k] * 0.25f + target * 0.75f : s_env[k] * 0.72f + target * 0.28f;
        amps[k] = s_env[k];
    }
    (void)snareEnv;

    std::memset(out, 0, sizeof(float) * size_t(n));
    const double invSr = twoPi / 48000.0;
    for (int k = 0; k < kBands; ++k) {
        // SpotiBacan: seno por rotacion (cos/sin una vez por banda, despues solo multiplicaciones), no std::sin por muestra
        const double step = freqs[k] * invSr;
        const float a = amps[k];
        float c = float(std::cos(s_phase[k])), s = float(std::sin(s_phase[k]));
        const float dc = float(std::cos(step)), ds = float(std::sin(step));
        for (int i = 0; i < n; ++i) {
            out[i] += a * s;
            const float nc = c * dc - s * ds;
            s = s * dc + c * ds;
            c = nc;
        }
        s_phase[k] = std::fmod(s_phase[k] + step * n, twoPi);
    }
    return true;
}
bool BgClient::config(BgConfig& out) {
    out = BgConfig{};
    return true;
}
void BgClient::setConfig(const BgConfig&) {}

void BgClient::browseHome() {
    if (!connected_) {
        if (!connect()) return;
    }
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcBrowseHome();
    }
    browsePage_.state = 1; // Loading
}

void BgClient::browseLibrary() {
    if (!connected_) {
        if (!connect()) return;
    }
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcBrowseLibrary();
    }
    browsePage_.state = 1; // Loading
}

void BgClient::browseSearch(const std::string& query) {
    if (!connected_) {
        if (!connect()) return;
    }
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcBrowseSearch(query.c_str());
    }
    browsePage_.state = 1; // Loading
}

void BgClient::browseOpen(uint64_t generation, uint32_t index) {
    if (!connected_) return;
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcBrowseOpen(generation, index);
    }
    browsePage_.state = 1; // Loading
}

void BgClient::browseBack(uint64_t generation) {
    if (!connected_) return;
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcBrowseBack(generation);
    }
    browsePage_.state = 1; // Loading
}

void BgClient::browsePlay(uint64_t generation, uint32_t index) {
    if (!connected_) return;
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcBrowsePlay(generation, index);
    }
}

static bool waitForBrowse(uint64_t oldGen, SwitifyIpcBrowseHeader& outH, int timeoutMs = 2000) {
    int elapsed = 0;
    while (elapsed < timeoutMs) {
        {
            std::scoped_lock lock(g_ipcMutex);
            if (R_SUCCEEDED(switifyIpcGetBrowseHeader(&outH))) {
                if (outH.generation != oldGen && outH.state == SwitifyBrowseState_Ready) {
                    return true;
                }
            }
        }
        svcSleepThread(10000000); // 10ms
        elapsed += 10;
    }
    return false;
}

void BgClient::playTrack(int trackIndex, const std::string& trackTitle, const std::string& playlistName) {
    if (!connected_) {
        if (!connect()) return;
    }
    std::scoped_lock browseLock(g_browseOpMutex);
    IpcBusyScope busy;
    LOG("BgClient::playTrack: trackIndex=%d, title='%s', playlist='%s'", 
        trackIndex, trackTitle.c_str(), playlistName.c_str());

    // 0. Dispatch reload session to sysmodule! App::loadTrack already wrote the newest playback-session.json.
    Result rc = 0;
    {
        std::scoped_lock lock(g_ipcMutex);
        rc = switifyIpcReloadSession();
    }
    LOG("BgClient::playTrack: switifyIpcReloadSession() returned 0x%08X", rc);
    if (R_SUCCEEDED(rc)) {
        return;
    }

    // 1. Try playing via BrowseCurrentPlaylist (fast, in-memory, no Spotify AP network roundtrips)
    SwitifyIpcBrowseHeader curH{};
    uint64_t oldGen = 0;
    {
        std::scoped_lock lock(g_ipcMutex);
        if (R_SUCCEEDED(switifyIpcGetBrowseHeader(&curH))) oldGen = curH.generation;
        switifyIpcBrowseCurrentPlaylist();
    }
    SwitifyIpcBrowseHeader h{};
    bool ok = waitForBrowse(oldGen, h, 1000) && h.item_count > 0;
    if (ok) {
        uint32_t targetIndex = static_cast<uint32_t>(std::clamp(trackIndex, 0, int(h.item_count) - 1));
        bool found = false;
        if (!trackTitle.empty()) {
            for (uint32_t i = 0; i < h.item_count; ++i) {
                SwitifyIpcBrowseItem it{};
                bool got = false;
                {
                    std::scoped_lock lock(g_ipcMutex);
                    got = R_SUCCEEDED(switifyIpcGetBrowseItem(i, &it));
                }
                if (got && it.kind == SwitifyBrowseItemKind_Track && it.name == trackTitle) {
                    targetIndex = i;
                    found = true;
                    break;
                }
            }
        }
        if (found) {
            LOG("BgClient::playTrack: playing current playlist targetIndex=%u on gen=%llu",
                targetIndex, static_cast<unsigned long long>(h.generation));
            {
                std::scoped_lock lock(g_ipcMutex);
                switifyIpcBrowsePlay(h.generation, targetIndex);
                switifyIpcPlay();
            }
            return;
        }
    }

    // 2. Open Library -> Section 0 (Playlists) -> find playlistName -> play targetIndex!
    oldGen = 0;
    {
        std::scoped_lock lock(g_ipcMutex);
        if (R_SUCCEEDED(switifyIpcGetBrowseHeader(&curH))) oldGen = curH.generation;
        switifyIpcBrowseLibrary();
    }
    if (waitForBrowse(oldGen, h, 1500) && h.item_count > 0) {
        // Open Section 0
        oldGen = h.generation;
        {
            std::scoped_lock lock(g_ipcMutex);
            switifyIpcBrowseOpen(h.generation, 0);
        }
        if (waitForBrowse(oldGen, h, 1500) && h.item_count > 0) {
            uint32_t plColIdx = UINT32_MAX;
            for (uint32_t i = 0; i < h.item_count; ++i) {
                SwitifyIpcBrowseItem it{};
                bool got = false;
                {
                    std::scoped_lock lock(g_ipcMutex);
                    got = R_SUCCEEDED(switifyIpcGetBrowseItem(i, &it));
                }
                if (got && (it.name == playlistName || (playlistName == "Tus me gusta" && i == 0))) {
                    plColIdx = i;
                    break;
                }
            }
            if (plColIdx != UINT32_MAX) {
                // Open that playlist
                oldGen = h.generation;
                {
                    std::scoped_lock lock(g_ipcMutex);
                    switifyIpcBrowseOpen(h.generation, plColIdx);
                }
                if (waitForBrowse(oldGen, h, 2000) && h.item_count > 0) {
                    uint32_t targetIndex = static_cast<uint32_t>(std::clamp(trackIndex, 0, int(h.item_count) - 1));
                    if (!trackTitle.empty()) {
                        for (uint32_t i = 0; i < h.item_count; ++i) {
                            SwitifyIpcBrowseItem it{};
                            bool got = false;
                            {
                                std::scoped_lock lock(g_ipcMutex);
                                got = R_SUCCEEDED(switifyIpcGetBrowseItem(i, &it));
                            }
                            if (got && it.kind == SwitifyBrowseItemKind_Track && it.name == trackTitle) {
                                targetIndex = i;
                                break;
                            }
                        }
                    }
                    LOG("BgClient::playTrack: playing from playlist '%s' targetIndex=%u on gen=%llu",
                        playlistName.c_str(), targetIndex, static_cast<unsigned long long>(h.generation));
                    {
                        std::scoped_lock lock(g_ipcMutex);
                        switifyIpcBrowsePlay(h.generation, targetIndex);
                        switifyIpcPlay();
                    }
                    return;
                }
            }
        }
    }

    // 3. Fallback: Search for the track title via switifyIpcBrowseSearch
    if (!trackTitle.empty()) {
        LOG("BgClient::playTrack: attempting search playback for '%s'", trackTitle.c_str());
        oldGen = 0;
        {
            std::scoped_lock lock(g_ipcMutex);
            if (R_SUCCEEDED(switifyIpcGetBrowseHeader(&curH))) oldGen = curH.generation;
            switifyIpcBrowseSearch(trackTitle.c_str());
        }
        if (waitForBrowse(oldGen, h, 1500) && h.item_count > 0) {
            // Open Section 0: Canciones
            oldGen = h.generation;
            {
                std::scoped_lock lock(g_ipcMutex);
                switifyIpcBrowseOpen(h.generation, 0);
            }
            if (waitForBrowse(oldGen, h, 1500) && h.item_count > 0) {
                uint32_t searchTarget = 0;
                for (uint32_t i = 0; i < h.item_count; ++i) {
                    SwitifyIpcBrowseItem it{};
                    bool got = false;
                    {
                        std::scoped_lock lock(g_ipcMutex);
                        got = R_SUCCEEDED(switifyIpcGetBrowseItem(i, &it));
                    }
                    if (got && it.kind == SwitifyBrowseItemKind_Track && it.name == trackTitle) {
                        searchTarget = i;
                        break;
                    }
                }
                LOG("BgClient::playTrack: playing search result targetIndex=%u on gen=%llu",
                    searchTarget, static_cast<unsigned long long>(h.generation));
                {
                    std::scoped_lock lock(g_ipcMutex);
                    switifyIpcBrowsePlay(h.generation, searchTarget);
                    switifyIpcPlay();
                }
                return;
            }
        }
    }

    LOG("BgClient::playTrack: fallback to switifyIpcPlay()");
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcPlay();
    }
}

bool BgClient::fetchLibraryPlaylists(std::vector<std::pair<std::string, std::string>>& outPlaylists) {
    if (!connected_) {
        if (!connect()) return false;
    }
    std::scoped_lock browseLock(g_browseOpMutex);
    IpcBusyScope busy;
    SwitifyIpcBrowseHeader curH{};
    uint64_t oldGen = 0;
    {
        std::scoped_lock lock(g_ipcMutex);
        if (R_SUCCEEDED(switifyIpcGetBrowseHeader(&curH))) oldGen = curH.generation;
        switifyIpcBrowseLibrary();
    }
    SwitifyIpcBrowseHeader h{};
    if (!waitForBrowse(oldGen, h, 1500) || h.item_count == 0) return false;

    // Open item 0: Playlists y canciones que te gustan
    oldGen = h.generation;
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcBrowseOpen(h.generation, 0);
    }
    if (!waitForBrowse(oldGen, h, 1500) || h.item_count == 0) return false;

    outPlaylists.clear();
    outPlaylists.reserve(h.item_count);
    for (uint32_t i = 0; i < h.item_count; ++i) {
        SwitifyIpcBrowseItem it{};
        bool got = false;
        {
            std::scoped_lock lock(g_ipcMutex);
            got = R_SUCCEEDED(switifyIpcGetBrowseItem(i, &it));
        }
        if (got) {
            if (it.kind == SwitifyBrowseItemKind_Playlist) {
                outPlaylists.push_back({ it.name, it.subtitle });
            }
        }
    }
    LOG("BgClient::fetchLibraryPlaylists: found %zu playlists", outPlaylists.size());
    return !outPlaylists.empty();
}

bool BgClient::fetchCollectionTracks(uint32_t collectionIndex, std::vector<BgBrowseItem>& outTracks) {
    if (!connected_) {
        if (!connect()) return false;
    }
    std::scoped_lock browseLock(g_browseOpMutex);
    IpcBusyScope busy;
    SwitifyIpcBrowseHeader curH{};
    uint64_t oldGen = 0;
    {
        std::scoped_lock lock(g_ipcMutex);
        if (R_SUCCEEDED(switifyIpcGetBrowseHeader(&curH))) oldGen = curH.generation;
        switifyIpcBrowseLibrary();
    }
    SwitifyIpcBrowseHeader h{};
    if (!waitForBrowse(oldGen, h, 1500) || h.item_count == 0) return false;

    // Open section 0 (Playlists)
    oldGen = h.generation;
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcBrowseOpen(h.generation, 0);
    }
    if (!waitForBrowse(oldGen, h, 1500) || collectionIndex >= h.item_count) return false;

    // Open collection
    oldGen = h.generation;
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcBrowseOpen(h.generation, collectionIndex);
    }
    if (!waitForBrowse(oldGen, h, 2000) || h.item_count == 0) return false;

    outTracks.clear();
    bool hasNext = true;
    while (hasNext) {
        hasNext = false;
        uint32_t nextIdx = UINT32_MAX;
        for (uint32_t i = 0; i < h.item_count; ++i) {
            SwitifyIpcBrowseItem it{};
            bool got = false;
            {
                std::scoped_lock lock(g_ipcMutex);
                got = R_SUCCEEDED(switifyIpcGetBrowseItem(i, &it));
            }
            if (got) {
                if (it.kind == SwitifyBrowseItemKind_Track) {
                    BgBrowseItem bi;
                    bi.kind = it.kind;
                    bi.index = it.index;
                    bi.durationMs = it.duration_ms;
                    bi.flags = it.flags;
                    bi.name = it.name;
                    bi.subtitle = it.subtitle;
                    bi.detail = it.detail;
                    outTracks.push_back(std::move(bi));
                } else if (it.kind == SwitifyBrowseItemKind_Section) {
                    std::string sName = it.name;
                    if (sName.find("Siguientes") != std::string::npos) {
                        hasNext = true;
                        nextIdx = i;
                    }
                }
            }
        }
        if (hasNext && nextIdx != UINT32_MAX) {
            uint64_t oldGen = h.generation;
            {
                std::scoped_lock lock(g_ipcMutex);
                switifyIpcBrowseOpen(h.generation, nextIdx);
            }
            if (!waitForBrowse(oldGen, h, 2500) || h.item_count == 0) break;
        } else {
            break;
        }
    }
    LOG("BgClient::fetchCollectionTracks: fetched %zu tracks for collection %u", outTracks.size(), collectionIndex);
    return !outTracks.empty();
}

bool BgClient::fetchPlaylistTracks(const std::string& playlistName, std::vector<BgBrowseItem>& outTracks) {
    if (!connected_) {
        if (!connect()) return false;
    }
    std::scoped_lock browseLock(g_browseOpMutex);
    IpcBusyScope busy;
    SwitifyIpcBrowseHeader curH{};
    uint64_t oldGen = 0;
    {
        std::scoped_lock lock(g_ipcMutex);
        if (R_SUCCEEDED(switifyIpcGetBrowseHeader(&curH))) oldGen = curH.generation;
        switifyIpcBrowseLibrary();
    }
    SwitifyIpcBrowseHeader h{};
    if (!waitForBrowse(oldGen, h, 1500) || h.item_count == 0) return false;

    // Open section 0 (Playlists y canciones que te gustan)
    oldGen = h.generation;
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcBrowseOpen(h.generation, 0);
    }
    if (!waitForBrowse(oldGen, h, 1500) || h.item_count == 0) return false;

    uint32_t targetIndex = 0;
    bool isLiked = (playlistName.empty() || playlistName == "Liked" || playlistName == "Tus me gusta" || playlistName == "Canciones que te gustan");
    if (!isLiked) {
        bool found = false;
        for (uint32_t i = 0; i < h.item_count; ++i) {
            SwitifyIpcBrowseItem it{};
            bool got = false;
            {
                std::scoped_lock lock(g_ipcMutex);
                got = R_SUCCEEDED(switifyIpcGetBrowseItem(i, &it));
            }
            if (got && playlistName == it.name) {
                targetIndex = i;
                found = true;
                break;
            }
        }
        if (!found) {
            LOG("BgClient::fetchPlaylistTracks: playlist '%s' not found in library", playlistName.c_str());
            return false;
        }
    }

    // Open collection / playlist
    oldGen = h.generation;
    {
        std::scoped_lock lock(g_ipcMutex);
        switifyIpcBrowseOpen(h.generation, targetIndex);
    }
    if (!waitForBrowse(oldGen, h, 2000) || h.item_count == 0) return false;

    outTracks.clear();
    bool hasNext = true;
    while (hasNext) {
        hasNext = false;
        uint32_t nextIdx = UINT32_MAX;
        for (uint32_t i = 0; i < h.item_count; ++i) {
            SwitifyIpcBrowseItem it{};
            bool got = false;
            {
                std::scoped_lock lock(g_ipcMutex);
                got = R_SUCCEEDED(switifyIpcGetBrowseItem(i, &it));
            }
            if (got) {
                if (it.kind == SwitifyBrowseItemKind_Track) {
                    BgBrowseItem bi;
                    bi.kind = it.kind;
                    bi.index = it.index;
                    bi.durationMs = it.duration_ms;
                    bi.flags = it.flags;
                    bi.name = it.name;
                    bi.subtitle = it.subtitle;
                    bi.detail = it.detail;
                    outTracks.push_back(std::move(bi));
                } else if (it.kind == SwitifyBrowseItemKind_Section) {
                    std::string sName = it.name;
                    if (sName.find("Siguientes") != std::string::npos) {
                        hasNext = true;
                        nextIdx = i;
                    }
                }
            }
        }
        if (hasNext && nextIdx != UINT32_MAX) {
            uint64_t oldGen = h.generation;
            {
                std::scoped_lock lock(g_ipcMutex);
                switifyIpcBrowseOpen(h.generation, nextIdx);
            }
            if (!waitForBrowse(oldGen, h, 2500) || h.item_count == 0) break;
        } else {
            break;
        }
    }
    LOG("BgClient::fetchPlaylistTracks: fetched %zu tracks for playlist '%s'", outTracks.size(), playlistName.c_str());
    return !outTracks.empty();
}

bool BgClient::pollBrowse(BgBrowsePage& out) {
    if (g_ipcBusy.load()) {
        out = browsePage_;
        return true;
    }
    if (!connected_) {
        out = browsePage_;
        return false;
    }
    SwitifyIpcBrowseHeader h{};
    Result headerRc = 0;
    {
        std::scoped_lock lock(g_ipcMutex);
        headerRc = switifyIpcGetBrowseHeader(&h);
    }
    if (R_FAILED(headerRc)) {
        out = browsePage_;
        return false;
    }
    if (h.struct_size != sizeof(SwitifyIpcBrowseHeader) ||
        h.abi_version != SWITIFY_IPC_BROWSE_ABI_VERSION) {
        out = browsePage_;
        return false;
    }

    browsePage_.generation = h.generation;
    browsePage_.state = h.state;
    browsePage_.depth = h.depth;
    browsePage_.itemCount = h.item_count;
    browsePage_.title = h.title;
    browsePage_.subtitle = h.subtitle;
    browsePage_.message = h.message;

    if (h.state == SwitifyBrowseState_Ready) {
        if (browsePage_.items.size() != h.item_count || browsePageGeneration_ != h.generation) {
            browsePageGeneration_ = h.generation;
            browsePage_.items.clear();
            browsePage_.items.reserve(h.item_count);
            for (uint32_t i = 0; i < h.item_count; ++i) {
                SwitifyIpcBrowseItem it{};
                bool itemOk = false;
                {
                    std::scoped_lock lock(g_ipcMutex);
                    itemOk = R_SUCCEEDED(switifyIpcGetBrowseItem(i, &it));
                }
                if (itemOk && it.generation == h.generation) {
                    BgBrowseItem bi;
                    bi.kind = it.kind;
                    bi.index = it.index;
                    bi.durationMs = it.duration_ms;
                    bi.flags = it.flags;
                    bi.name = it.name;
                    bi.subtitle = it.subtitle;
                    bi.detail = it.detail;
                    browsePage_.items.push_back(std::move(bi));
                }
            }
        }
    } else if (h.state != SwitifyBrowseState_Loading) {
        browsePage_.items.clear();
    }

    out = browsePage_;
    return true;
}
