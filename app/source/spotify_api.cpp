#include "spotify_api.hpp"
#include "cJSON.h"
#include "logger.hpp"
#include "net.hpp"
#include "cores.hpp"

#include <curl/curl.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "BellLogger.h"
#define Album CSpotPbAlbum
#define Track CSpotPbTrack
#include "CSpotContext.h"
#include "LoginBlob.h"
#undef Album
#undef Track
#include "MDNSService.h"

namespace {

const std::string& webClientId() {
    static const std::string id = []() {
        FILE* f = fopen("sdmc:/config/SpotiBacan/client_id.txt", "rb");
        if (f) {
            char buf[64] = {};
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = '\0';
            if (n >= 16) return std::string(buf, n);
        }
        static const uint8_t uuid[16] = {
            0x65, 0xb7, 0x08, 0x07, 0x3f, 0xc0, 0x48, 0x0e,
            0xa9, 0x2a, 0x07, 0x72, 0x33, 0xca, 0x87, 0xbd
        };
        char hex[33] = {};
        for (int i = 0; i < 16; ++i) snprintf(hex + i * 2, 3, "%02x", uuid[i]);
        return std::string(hex, 32);
    }();
    return id;
}
#define WEB_CLIENT_ID (webClientId().c_str())
const char* DEVICE_ID = "switch-nxtify-01";
const char* USER_AGENT = "Spotify/127700358 Linux/0 (SpotiBacan)";

// Spotify Pathfinder Query Hashes
const char* HASH_FETCH_LIBRARY_TRACKS = "087278b20b743578a6262c2b0b4bcd20d879c503cc359a2285baf083ef944240";
const char* HASH_LIBRARY_V3 = "390c78e5b951029bad359785e69b07b536a509c581cbcd0aded5e5067f187455";
const char* HASH_FETCH_PLAYLIST = "21ea11a6ac75a5f3e01fd365d29c18e0a4a8b7dc09f89bbb5d73910190c6acaf";

uint64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
}

std::vector<uint8_t> base64Decode(const std::string& in) {
    static const int table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,62,-1,63, // '+' and '-'
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63, // '_'
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        int d = table[c];
        if (d == -1) continue;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

inline std::string jsonString(cJSON* obj, const char* name) {
    if (!obj || !name) return "";
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (item && cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return "";
}

inline double jsonDouble(cJSON* obj, const char* name, double def = 0.0) {
    if (!obj || !name) return def;
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (item && cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    return def;
}

namespace pb {

inline void writeVarint(uint64_t val, std::vector<uint8_t>& out) {
    while (val >= 0x80) {
        out.push_back(static_cast<uint8_t>((val & 0x7F) | 0x80));
        val >>= 7;
    }
    out.push_back(static_cast<uint8_t>(val & 0x7F));
}

inline void writeTag(int fieldNum, int wireType, std::vector<uint8_t>& out) {
    writeVarint((static_cast<uint64_t>(fieldNum) << 3) | static_cast<uint64_t>(wireType), out);
}

inline void writeVarintField(int fieldNum, uint64_t val, std::vector<uint8_t>& out) {
    writeTag(fieldNum, 0, out);
    writeVarint(val, out);
}

inline void writeBytesField(int fieldNum, const uint8_t* data, size_t len, std::vector<uint8_t>& out) {
    writeTag(fieldNum, 2, out);
    writeVarint(len, out);
    if (len > 0 && data) {
        out.insert(out.end(), data, data + len);
    }
}

inline void writeBytesField(int fieldNum, const std::vector<uint8_t>& data, std::vector<uint8_t>& out) {
    writeBytesField(fieldNum, data.data(), data.size(), out);
}

inline void writeStringField(int fieldNum, const std::string& str, std::vector<uint8_t>& out) {
    writeBytesField(fieldNum, reinterpret_cast<const uint8_t*>(str.data()), str.size(), out);
}

struct Field {
    int fieldNum = 0;
    int wireType = 0;
    uint64_t varintVal = 0;
    std::vector<uint8_t> bytesVal;
    std::string asString() const {
        return std::string(reinterpret_cast<const char*>(bytesVal.data()), bytesVal.size());
    }
};

inline std::vector<Field> parse(const uint8_t* data, size_t len) {
    std::vector<Field> fields;
    size_t idx = 0;
    while (idx < len) {
        uint64_t tag = 0;
        int shift = 0;
        while (idx < len) {
            uint8_t b = data[idx++];
            tag |= static_cast<uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) break;
            shift += 7;
            if (shift >= 64) break;
        }
        int fieldNum = static_cast<int>(tag >> 3);
        int wireType = static_cast<int>(tag & 7);
        Field f;
        f.fieldNum = fieldNum;
        f.wireType = wireType;
        if (wireType == 0) {
            uint64_t val = 0;
            int s = 0;
            while (idx < len) {
                uint8_t b = data[idx++];
                val |= static_cast<uint64_t>(b & 0x7F) << s;
                if ((b & 0x80) == 0) break;
                s += 7;
                if (s >= 64) break;
            }
            f.varintVal = val;
            fields.push_back(std::move(f));
        } else if (wireType == 2) {
            uint64_t l = 0;
            int s = 0;
            while (idx < len) {
                uint8_t b = data[idx++];
                l |= static_cast<uint64_t>(b & 0x7F) << s;
                if ((b & 0x80) == 0) break;
                s += 7;
                if (s >= 64) break;
            }
            if (idx + l <= len) {
                f.bytesVal.assign(data + idx, data + idx + l);
                idx += l;
                fields.push_back(std::move(f));
            } else {
                break;
            }
        } else if (wireType == 1) {
            idx += 8;
        } else if (wireType == 5) {
            idx += 4;
        } else {
            break;
        }
    }
    return fields;
}

} // namespace pb

struct MemoryBuffer {
    std::vector<uint8_t> data;
};

size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    MemoryBuffer* buf = static_cast<MemoryBuffer*>(userdata);
    size_t total = size * nmemb;
    buf->data.insert(buf->data.end(), reinterpret_cast<uint8_t*>(ptr), reinterpret_cast<uint8_t*>(ptr) + total);
    return total;
}

} // namespace

SpotifyApi::SpotifyApi() {
}

SpotifyApi::~SpotifyApi() {
    stopCodePairing();
    stopPairing();
}

SpotifyApi& SpotifyApi::instance() {
    static SpotifyApi s_instance;
    return s_instance;
}

bool SpotifyApi::init() {
    if (!credentialsLoaded_) {
        credentialsLoaded_ = loadCredentials();
    }
    return credentialsLoaded_;
}

bool SpotifyApi::hasCredentials() const {
    if (credentialsLoaded_ && !authData_.empty()) return true;
    static const char* paths[] = {
        "sdmc:/config/SpotiBacan/credentials.json",
        "/config/SpotiBacan/credentials.json",
        "sdmc:/config/Switify/credentials.json",
        "/config/Switify/credentials.json",
        nullptr
    };
    for (int i = 0; paths[i]; ++i) {
        FILE* f = fopen(paths[i], "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fclose(f);
            if (sz > 16) return true;
        }
    }
    return false;
}

void SpotifyApi::logout() {
    stopPairing();
    {
        std::scoped_lock lock(mutex_);
        username_.clear();
        authData_.clear();
        clientToken_.clear();
        accessToken_.clear();
        tokenExpiryTime_ = 0;
        credentialsLoaded_ = false;
    }
    remove("sdmc:/config/SpotiBacan/credentials.json");
    remove("/config/SpotiBacan/credentials.json");
    remove("sdmc:/config/SpotiBacan/playback-session.json");
    remove("sdmc:/config/SpotiBacan/playback-session.json.tmp");
    remove("sdmc:/config/SpotiBacan/current-artwork-url");
    remove("sdmc:/config/SpotiBacan/spotify-tracks.tsv");
    remove("sdmc:/config/SpotiBacan/playlists.txt");
    remove("sdmc:/config/SpotiBacan/lists.txt");
    remove("sdmc:/config/SpotiBacan/liked.txt");
    if (FILE* m = fopen("sdmc:/config/SpotiBacan/do-not-import-poc-credentials", "wb")) {
        fputs("SpotiBacán sign-out marker\n", m);
        fclose(m);
    }
    LOG("SpotifyApi::logout: credentials removed and state cleared.");
}

namespace {

std::string urlDecodeZc(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size()) {
            char hex[3] = { in[i + 1], in[i + 2], 0 };
            out += char(strtoul(hex, nullptr, 16));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

std::map<std::string, std::string> parseUrlEncodedZc(const std::string& body) {
    std::map<std::string, std::string> result;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) amp = body.size();
        size_t eq = body.find('=', pos);
        if (eq != std::string::npos && eq < amp) {
            result[urlDecodeZc(body.substr(pos, eq - pos))] = urlDecodeZc(body.substr(eq + 1, amp - eq - 1));
        }
        pos = amp + 1;
    }
    return result;
}

} // namespace

void SpotifyApi::startPairing() {
    if (pairingRunning_.load()) return;
    stopPairing();
    pairingQuit_ = false;
    pairingRunning_ = true;
    pairingState_ = 1;
    pairingThread_ = std::thread(&SpotifyApi::pairingLoop, this);
    startCodePairing();   // SpotiBacan: el codigo, a la par
}

void SpotifyApi::stopPairing() {
    pairingQuit_ = true;
    if (listenFd_ >= 0) {
        shutdown(listenFd_, SHUT_RDWR);
        close(listenFd_);
        listenFd_ = -1;
    }
    if (pairingThread_.joinable()) {
        pairingThread_.join();
    }
    pairingRunning_ = false;
}

void SpotifyApi::pairingLoop() {
    cores::moveHere(cores::Net);
    std::string netErr;
    net::begin(netErr);
    static bool s_bellLogInited = false;
    if (!s_bellLogInited) {
        bell::setDefaultLogger();
        s_bellLogInited = true;
    }

    LOG("SpotifyApi::pairingLoop: starting Zeroconf server on port 7864...");
    auto blob = std::make_shared<cspot::LoginBlob>("SpotiBacan (Switch)");

    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        LOG("SpotifyApi::pairingLoop: failed to create listen socket");
        pairingState_ = 4;
        pairingRunning_ = false;
        return;
    }

    int on = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(7864);

    if (bind(listenFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0 ||
        listen(listenFd_, 4) < 0) {
        LOG("SpotifyApi::pairingLoop: bind/listen failed on port 7864");
        close(listenFd_);
        listenFd_ = -1;
        pairingState_ = 4;
        pairingRunning_ = false;
        return;
    }

    std::shared_ptr<bell::MDNSService> mdns;
    try {
        mdns = bell::MDNSService::registerService(
            blob->getDeviceName(), "_spotify-connect", "_tcp", "", 7864,
            { {"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"} });
    } catch (...) {
        LOG("SpotifyApi::pairingLoop: MDNSService::registerService threw exception");
    }

    bool gotAddUser = false;
    while (!pairingQuit_.load() && !gotAddUser) {
        struct timeval tv{ 0, 250000 }; // 250ms timeout so we can check pairingQuit_
        fd_set rfds;
        FD_ZERO(&rfds);
        if (listenFd_ < 0) break;
        FD_SET(listenFd_, &rfds);
        int sel = select(listenFd_ + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0 || pairingQuit_.load() || listenFd_ < 0) continue;

        struct sockaddr_in client{};
        socklen_t clientLen = sizeof(client);
        int fd = accept(listenFd_, reinterpret_cast<struct sockaddr*>(&client), &clientLen);
        if (fd < 0) continue;

        std::string request;
        char buf[2048];
        size_t headerEnd = std::string::npos;
        while (headerEnd == std::string::npos && request.size() < 32768) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            request.append(buf, size_t(n));
            headerEnd = request.find("\r\n\r\n");
        }
        if (headerEnd != std::string::npos) {
            size_t lineEnd = request.find("\r\n");
            std::string requestLine = request.substr(0, lineEnd);
            size_t sp1 = requestLine.find(' ');
            size_t sp2 = sp1 == std::string::npos ? std::string::npos : requestLine.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos) {
                std::string method = requestLine.substr(0, sp1);
                std::string path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
                size_t q = path.find('?');
                if (q != std::string::npos) path = path.substr(0, q);

                size_t contentLength = 0;
                std::string headers = request.substr(0, headerEnd);
                for (char& c : headers) c = char(tolower((unsigned char)c));
                size_t p = headers.find("content-length:");
                if (p != std::string::npos) contentLength = strtoul(headers.c_str() + p + 15, nullptr, 10);

                std::string body = request.substr(headerEnd + 4);
                while (body.size() < contentLength) {
                    ssize_t n = recv(fd, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    body.append(buf, size_t(n));
                }

                std::string responseBody;
                if (path == "/spotify_info" && method == "GET") {
                    responseBody = blob->buildZeroconfInfo();
                } else if (path == "/spotify_info" && method == "POST") {
                    try {
                        auto query = parseUrlEncodedZc(body);
                        blob->loadZeroconfQuery(query);
                        gotAddUser = true;
                    } catch (...) {
                        LOG("SpotifyApi::pairingLoop: loadZeroconfQuery failed");
                    }
                    responseBody = "{\"status\":101,\"spotifyError\":0,\"statusString\":\"ERROR-OK\"}";
                } else {
                    responseBody = "{}";
                }
                char hdr[256];
                snprintf(hdr, sizeof(hdr),
                         "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                         responseBody.size());
                send(fd, hdr, strlen(hdr), 0);
                send(fd, responseBody.data(), responseBody.size(), 0);
            }
        }
        close(fd);
    }

    if (mdns) {
        try { mdns->unregisterService(); } catch (...) {}
        mdns.reset();
    }
    if (listenFd_ >= 0) {
        close(listenFd_);
        listenFd_ = -1;
    }

    if (gotAddUser && !pairingQuit_.load()) {
        pairingState_ = 2; // Authenticating with Spotify AP
        LOG("SpotifyApi::pairingLoop: received Zeroconf login, authenticating with Spotify AP...");
        try {
            auto ctx = cspot::Context::createFromBlob(blob);
            ctx->config.clientId = WEB_CLIENT_ID;
            ctx->session->connectWithRandomAp();
            ctx->config.authData = ctx->session->authenticate(blob);
            if (!ctx->config.authData.empty()) {
                std::string credJson = ctx->getCredentialsJson();
                mkdir("sdmc:/config", 0777);
                mkdir("sdmc:/config/SpotiBacan", 0777);
                if (FILE* f = fopen("sdmc:/config/SpotiBacan/credentials.json", "wb")) {
                    fwrite(credJson.data(), 1, credJson.size(), f);
                    fclose(f);
                }
                remove("sdmc:/config/SpotiBacan/do-not-import-poc-credentials");
                {
                    std::scoped_lock lock(mutex_);
                    credentialsLoaded_ = loadCredentials();
                }
                pairingState_ = 3;
                pairingJustCompleted_ = true;
                LOG("SpotifyApi::pairingLoop: credentials saved and loaded successfully!");
            } else {
                LOG("SpotifyApi::pairingLoop: authenticate returned empty authData");
                pairingState_ = 4;
            }
        } catch (const std::exception& e) {
            LOG("SpotifyApi::pairingLoop: AP auth exception: %s", e.what());
            pairingState_ = 4;
        } catch (...) {
            LOG("SpotifyApi::pairingLoop: AP auth unknown exception");
            pairingState_ = 4;
        }
    }
    pairingRunning_ = false;
}

bool SpotifyApi::loadCredentials() {
    static const char* candidates[] = {
        "sdmc:/config/SpotiBacan/credentials.json",
        "sdmc:/config/Switify/credentials.json",
        "config/SpotiBacan/credentials.json",
        nullptr
    };

    std::string jsonStr;
    for (int i = 0; candidates[i]; ++i) {
        FILE* f = fopen(candidates[i], "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 1024 * 1024) {
                jsonStr.resize(sz);
                size_t rd = fread(jsonStr.data(), 1, sz, f);
                fclose(f);
                if (rd == size_t(sz)) {
                    LOG("SpotifyApi: found credentials at %s", candidates[i]);
                    break;
                }
            } else {
                fclose(f);
            }
        }
    }

    if (jsonStr.empty()) {
        return false;
    }

    cJSON* root = cJSON_Parse(jsonStr.c_str());
    if (!root) return false;

    std::string user = jsonString(root, "username");
    std::string authB64 = jsonString(root, "authData");
    cJSON_Delete(root);

    if (user.empty() || authB64.empty()) return false;

    username_ = user;
    authData_ = base64Decode(authB64);

    LOG("SpotifyApi: credentials loaded for user '%s' (authData %zu bytes)", username_.c_str(), authData_.size());
    return !authData_.empty();
}

bool SpotifyApi::authenticate() {
    if (!init()) return false;
    if (!accessToken_.empty() && nowSeconds() < tokenExpiryTime_) {
        return true;
    }
    if (clientToken_.empty()) {
        if (!requestClientToken()) return false;
    }
    if (!requestAccessToken()) {
        if (requestClientToken()) {
            return requestAccessToken();
        }
        return false;
    }
    return true;
}

std::string SpotifyApi::webAccessToken() {
    std::scoped_lock lock(mutex_);
    if (!authenticate()) return {};
    return accessToken_;
}

bool SpotifyApi::requestClientToken() {
    std::string netErr;
    if (!net::begin(netErr)) {
        LOG("SpotifyApi: net::begin failed: %s", netErr.c_str());
        return false;
    }

    // connectivity: field 1 platformData (field 5 empty), field 2 device_id
    std::vector<uint8_t> platformData;
    pb::writeBytesField(5, nullptr, 0, platformData);

    std::vector<uint8_t> connectivity;
    pb::writeBytesField(1, platformData, connectivity);
    pb::writeStringField(2, DEVICE_ID, connectivity);

    // clientData: field 1 "0.0.0", field 2 WEB_CLIENT_ID, field 3 connectivity
    std::vector<uint8_t> clientData;
    pb::writeStringField(1, "0.0.0", clientData);
    pb::writeStringField(2, WEB_CLIENT_ID, clientData);
    pb::writeBytesField(3, connectivity, clientData);

    // root: field 1 varint 1, field 2 clientData
    std::vector<uint8_t> reqBody;
    pb::writeVarintField(1, 1, reqBody);
    pb::writeBytesField(2, clientData, reqBody);

    std::lock_guard<std::recursive_mutex> netLock(net::mutex());

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/x-protobuf");
    headers = curl_slist_append(headers, "Content-Type: application/x-protobuf");

    MemoryBuffer respBuf;
    curl_easy_setopt(curl, CURLOPT_URL, "https://clienttoken.spotify.com/v1/clienttoken");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, reqBody.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(reqBody.size()));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBuf);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK || httpCode != 200) {
        LOG("SpotifyApi: clienttoken request failed (curl=%d, http=%ld)", res, httpCode);
        return false;
    }

    auto fields = pb::parse(respBuf.data.data(), respBuf.data.size());
    for (const auto& f : fields) {
        if (f.fieldNum == 2 && f.wireType == 2) { // granted
            auto grantedFields = pb::parse(f.bytesVal.data(), f.bytesVal.size());
            for (const auto& gf : grantedFields) {
                if (gf.fieldNum == 1 && gf.wireType == 2) {
                    clientToken_ = gf.asString();
                    LOG("SpotifyApi: got clientToken (%zu chars)", clientToken_.size());
                    return !clientToken_.empty();
                }
            }
        }
    }

    LOG("SpotifyApi: clientToken not found in response protobuf");
    return false;
}

bool SpotifyApi::requestAccessToken() {
    std::string netErr;
    if (!net::begin(netErr)) return false;

    // clientInfo: field 1 WEB_CLIENT_ID, field 2 DEVICE_ID
    std::vector<uint8_t> clientInfo;
    pb::writeStringField(1, WEB_CLIENT_ID, clientInfo);
    pb::writeStringField(2, DEVICE_ID, clientInfo);

    // storedCredential: field 1 username, field 2 authData
    std::vector<uint8_t> storedCredential;
    pb::writeStringField(1, username_, storedCredential);
    pb::writeBytesField(2, authData_, storedCredential);

    // root: field 1 clientInfo, field 100 storedCredential
    std::vector<uint8_t> reqBody;
    pb::writeBytesField(1, clientInfo, reqBody);
    pb::writeBytesField(100, storedCredential, reqBody);

    std::lock_guard<std::recursive_mutex> netLock(net::mutex());

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept: application/x-protobuf");
    headers = curl_slist_append(headers, "Content-Type: application/x-protobuf");
    std::string ctHeader = "Client-Token: " + clientToken_;
    headers = curl_slist_append(headers, ctHeader.c_str());

    MemoryBuffer respBuf;
    curl_easy_setopt(curl, CURLOPT_URL, "https://login5.spotify.com/v3/login");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, reqBody.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, long(reqBody.size()));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBuf);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK || httpCode != 200) {
        LOG("SpotifyApi: login5 request failed (curl=%d, http=%ld)", res, httpCode);
        return false;
    }

    auto fields = pb::parse(respBuf.data.data(), respBuf.data.size());
    for (const auto& f : fields) {
        if (f.fieldNum == 1 && f.wireType == 2) { // ok
            auto okFields = pb::parse(f.bytesVal.data(), f.bytesVal.size());
            for (const auto& of : okFields) {
                if (of.fieldNum == 2 && of.wireType == 2) {
                    accessToken_ = of.asString();
                    tokenExpiryTime_ = nowSeconds() + 3500;
                    LOG("SpotifyApi: got accessToken (%zu chars)", accessToken_.size());
                    return !accessToken_.empty();
                }
            }
        }
    }

    LOG("SpotifyApi: accessToken not found in login5 response");
    return false;
}

bool SpotifyApi::pathfinderQuery(const std::string& opName, const std::string& opHash, const std::string& variablesJson, std::string& outJson) {
    if (!authenticate()) return false;

    std::string netErr;
    if (!net::begin(netErr)) return false;

    std::lock_guard<std::recursive_mutex> netLock(net::mutex());

    CURL* curl = curl_easy_init();
    if (!curl) return false;

    char* escOp = curl_easy_escape(curl, opName.c_str(), int(opName.size()));
    char* escVars = curl_easy_escape(curl, variablesJson.c_str(), int(variablesJson.size()));
    std::string ext = "{\"persistedQuery\":{\"version\":1,\"sha256Hash\":\"" + opHash + "\"}}";
    char* escExt = curl_easy_escape(curl, ext.c_str(), int(ext.size()));

    std::string url = "https://api-partner.spotify.com/pathfinder/v1/query?operationName=";
    url += escOp ? escOp : "";
    url += "&variables=";
    url += escVars ? escVars : "";
    url += "&extensions=";
    url += escExt ? escExt : "";

    if (escOp) curl_free(escOp);
    if (escVars) curl_free(escVars);
    if (escExt) curl_free(escExt);

    curl_slist* headers = nullptr;
    std::string authHeader = "Authorization: Bearer " + accessToken_;
    std::string ctHeader = "Client-Token: " + clientToken_;
    headers = curl_slist_append(headers, authHeader.c_str());
    headers = curl_slist_append(headers, ctHeader.c_str());
    headers = curl_slist_append(headers, "Spotify-App-Version: 1.2.52.442");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Accept-Language: es-419,es;q=0.9,en-US;q=0.8");
    headers = curl_slist_append(headers, "app-platform: WebPlayer");

    MemoryBuffer respBuf;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBuf);

    LOG("SpotifyApi: pathfinderQuery '%s' variables: %s", opName.c_str(), variablesJson.c_str());
    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    if (res != CURLE_OK || httpCode != 200) {
        LOG("SpotifyApi: pathfinderQuery '%s' failed (curl=%d, http=%ld)", opName.c_str(), res, httpCode);
        if (httpCode == 401) {
            tokenExpiryTime_ = 0; // force reauth on next attempt
        }
        return false;
    }

    LOG("SpotifyApi: pathfinderQuery '%s' success (httpCode=%ld, respLen=%zu)", opName.c_str(), httpCode, respBuf.data.size());

    outJson.assign(reinterpret_cast<const char*>(respBuf.data.data()), respBuf.data.size());
    return true;
}

bool SpotifyApi::fetchLikedTracks(std::vector<Track>& outTracks, std::unordered_map<std::string, std::string>& outCovers, size_t maxTracks) {
    std::scoped_lock lock(mutex_);
    outTracks.clear();
    LOG("SpotifyApi::fetchLikedTracks: starting fetch for up to %zu tracks...", maxTracks);
    size_t offset = 0;
    const size_t limit = 100;

    while (outTracks.size() < maxTracks) {
        std::string vars = "{\"uri\":\"spotify:collection:tracks\",\"offset\":" + std::to_string(offset) + ",\"limit\":" + std::to_string(limit) + "}";
        std::string respJson;
        if (!pathfinderQuery("fetchLibraryTracks", HASH_FETCH_LIBRARY_TRACKS, vars, respJson)) {
            LOG("SpotifyApi::fetchLikedTracks: failed query at offset %zu", offset);
            break;
        }

        cJSON* root = cJSON_Parse(respJson.c_str());
        if (!root) break;

        cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
        cJSON* me = cJSON_GetObjectItemCaseSensitive(data, "me");
        cJSON* library = cJSON_GetObjectItemCaseSensitive(me, "library");
        cJSON* tracksObj = cJSON_GetObjectItemCaseSensitive(library, "tracks");
        cJSON* items = cJSON_GetObjectItemCaseSensitive(tracksObj, "items");

        if (!cJSON_IsArray(items)) {
            cJSON_Delete(root);
            break;
        }

        int count = cJSON_GetArraySize(items);
        if (count == 0) {
            cJSON_Delete(root);
            break;
        }

        for (int i = 0; i < count && outTracks.size() < maxTracks; ++i) {
            cJSON* item = cJSON_GetArrayItem(items, i);
            cJSON* trWrapper = cJSON_GetObjectItemCaseSensitive(item, "track");
            if (!trWrapper) continue;

            // In Pathfinder fetchLibraryTracks, URI is in track._uri
            std::string uri = jsonString(trWrapper, "_uri");
            if (uri.empty()) uri = jsonString(trWrapper, "uri");

            cJSON* tr = cJSON_GetObjectItemCaseSensitive(trWrapper, "data");
            if (!tr) continue;

            if (uri.empty()) {
                uri = jsonString(tr, "uri");
                if (uri.empty()) {
                    std::string id = jsonString(tr, "id");
                    if (!id.empty()) uri = "spotify:track:" + id;
                }
            }

            std::string name = jsonString(tr, "name");
            if (uri.empty() || name.empty()) continue;

            Track t;
            t.title = name;
            t.path = uri;

            // Artists
            cJSON* artistsObj = cJSON_GetObjectItemCaseSensitive(tr, "artists");
            cJSON* aItems = cJSON_GetObjectItemCaseSensitive(artistsObj, "items");
            if (cJSON_IsArray(aItems)) {
                std::string artStr;
                int aCount = cJSON_GetArraySize(aItems);
                for (int a = 0; a < aCount; ++a) {
                    cJSON* artIt = cJSON_GetArrayItem(aItems, a);
                    cJSON* prof = cJSON_GetObjectItemCaseSensitive(artIt, "profile");
                    std::string pName = jsonString(prof, "name");
                    if (!pName.empty()) {
                        if (!artStr.empty()) artStr += ", ";
                        artStr += pName;
                    }
                }
                t.artist = artStr;
                t.albumArtist = artStr;
            }

            // Album
            cJSON* albumObj = cJSON_GetObjectItemCaseSensitive(tr, "albumOfTrack");
            if (albumObj) {
                t.album = jsonString(albumObj, "name");

                // Cover art
                cJSON* coverArt = cJSON_GetObjectItemCaseSensitive(albumObj, "coverArt");
                cJSON* sources = cJSON_GetObjectItemCaseSensitive(coverArt, "sources");
                if (cJSON_IsArray(sources) && cJSON_GetArraySize(sources) > 0) {
                    cJSON* firstSrc = cJSON_GetArrayItem(sources, 0);
                    std::string srcUrl = jsonString(firstSrc, "url");
                    if (!srcUrl.empty()) {
                        outCovers[t.path] = srcUrl;
                    }
                }
            }

            // Duration
            cJSON* durObj = cJSON_GetObjectItemCaseSensitive(tr, "duration");
            if (!durObj) durObj = cJSON_GetObjectItemCaseSensitive(tr, "trackDuration");
            if (durObj) {
                double totalMs = jsonDouble(durObj, "totalMilliseconds");
                if (totalMs > 0) {
                    t.duration = totalMs / 1000.0;
                }
            }

            t.trackNo = int(outTracks.size() + 1);
            t.tagged = true;
            outTracks.push_back(std::move(t));
        }

        cJSON_Delete(root);
        LOG("SpotifyApi::fetchLikedTracks: page offset %zu returned %d items, total tracks now %zu", offset, count, outTracks.size());
        offset += count;
        if (count < int(limit)) break;
    }

    LOG("SpotifyApi::fetchLikedTracks: retrieved %zu tracks", outTracks.size());
    return !outTracks.empty();
}

bool SpotifyApi::fetchUserPlaylists(std::vector<FolderPlaylist>& outPlaylists, std::unordered_map<std::string, std::string>& outCovers) {
    std::scoped_lock lock(mutex_);
    outPlaylists.clear();
    LOG("SpotifyApi::fetchUserPlaylists: querying libraryV3 for playlists...");

    std::string vars = "{\"filters\":[\"Playlists\"],\"order\":null,\"textFilter\":\"\",\"features\":[\"LIKED_SONGS\",\"YOUR_EPISODES\"],\"limit\":100,\"offset\":0,\"flatten\":false,\"expandedFolders\":[],\"folderUri\":null,\"includeFoldersWhenFlattening\":true,\"withCuration\":false}";
    std::string respJson;
    if (!pathfinderQuery("libraryV3", HASH_LIBRARY_V3, vars, respJson)) {
        LOG("SpotifyApi::fetchUserPlaylists: libraryV3 query failed");
        return false;
    }

    cJSON* root = cJSON_Parse(respJson.c_str());
    if (!root) return false;

    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    cJSON* me = cJSON_GetObjectItemCaseSensitive(data, "me");
    cJSON* libV3 = cJSON_GetObjectItemCaseSensitive(me, "libraryV3");
    cJSON* items = cJSON_GetObjectItemCaseSensitive(libV3, "items");

    if (!cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return false;
    }

    int count = cJSON_GetArraySize(items);
    for (int i = 0; i < count; ++i) {
        cJSON* it = cJSON_GetArrayItem(items, i);
        cJSON* itemObj = cJSON_GetObjectItemCaseSensitive(it, "item");
        cJSON* itemData = cJSON_GetObjectItemCaseSensitive(itemObj, "data");
        if (!itemData) continue;

        std::string name = jsonString(itemData, "name");
        std::string uri = jsonString(itemData, "uri");
        if (name.empty() || uri.empty()) continue;

        // Skip non-playlists or collection items
        if (uri.find("spotify:playlist:") != 0 && uri.find("spotify:collection:") != 0) {
            continue;
        }

        // Skip Liked Songs (handled as Tus me gusta)
        if (name == "Liked" || name == "Liked Songs" || name == "Tus me gusta" || uri == "spotify:collection:tracks") {
            continue;
        }

        // Skip AI DJ
        if (name == "DJ" || uri == "spotify:playlist:37i9dQZF1EYkqdzj48dyYq") {
            continue;
        }

        FolderPlaylist pl;
        pl.name = name;
        pl.path = uri; // Stores Spotify playlist URI

        // Images
        cJSON* imagesObj = cJSON_GetObjectItemCaseSensitive(itemData, "images");
        cJSON* imgItems = cJSON_GetObjectItemCaseSensitive(imagesObj, "items");
        if (cJSON_IsArray(imgItems) && cJSON_GetArraySize(imgItems) > 0) {
            cJSON* firstImg = cJSON_GetArrayItem(imgItems, 0);
            cJSON* sources = cJSON_GetObjectItemCaseSensitive(firstImg, "sources");
            if (cJSON_IsArray(sources) && cJSON_GetArraySize(sources) > 0) {
                cJSON* firstSrc = cJSON_GetArrayItem(sources, 0);
                std::string srcUrl = jsonString(firstSrc, "url");
                if (!srcUrl.empty()) {
                    outCovers[pl.name] = srcUrl;
                    outCovers[pl.path] = srcUrl;
                }
            }
        }

        outPlaylists.push_back(std::move(pl));
    }

    cJSON_Delete(root);
    LOG("SpotifyApi::fetchUserPlaylists: found %zu playlists", outPlaylists.size());
    return true;
}

bool SpotifyApi::fetchPlaylistTracks(const std::string& playlistUri, std::vector<Track>& outTracks, std::unordered_map<std::string, std::string>& outCovers, size_t maxTracks) {
    std::scoped_lock lock(mutex_);
    outTracks.clear();
    if (playlistUri.empty()) return false;
    LOG("SpotifyApi::fetchPlaylistTracks: starting fetch for '%s'...", playlistUri.c_str());

    size_t offset = 0;
    const size_t limit = 100;

    while (outTracks.size() < maxTracks) {
        std::string vars = "{\"uri\":\"" + playlistUri + "\",\"offset\":" + std::to_string(offset) + ",\"limit\":" + std::to_string(limit) + ",\"enableWatchFeedEntrypoint\":false}";
        std::string respJson;
        if (!pathfinderQuery("fetchPlaylist", HASH_FETCH_PLAYLIST, vars, respJson)) {
            LOG("SpotifyApi::fetchPlaylistTracks: query failed for '%s' at offset %zu", playlistUri.c_str(), offset);
            break;
        }

        cJSON* root = cJSON_Parse(respJson.c_str());
        if (!root) break;

        cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
        cJSON* plV2 = cJSON_GetObjectItemCaseSensitive(data, "playlistV2");
        cJSON* content = cJSON_GetObjectItemCaseSensitive(plV2, "content");
        cJSON* items = cJSON_GetObjectItemCaseSensitive(content, "items");

        if (!cJSON_IsArray(items)) {
            cJSON_Delete(root);
            break;
        }

        int count = cJSON_GetArraySize(items);
        if (count == 0) {
            cJSON_Delete(root);
            break;
        }

        for (int i = 0; i < count && outTracks.size() < maxTracks; ++i) {
            cJSON* it = cJSON_GetArrayItem(items, i);
            cJSON* itemV2 = cJSON_GetObjectItemCaseSensitive(it, "itemV2");
            if (!itemV2) continue;

            cJSON* tr = cJSON_GetObjectItemCaseSensitive(itemV2, "data");
            if (!tr) continue;

            std::string uri = jsonString(tr, "uri");
            if (uri.empty()) uri = jsonString(itemV2, "_uri");
            if (uri.empty()) {
                std::string id = jsonString(tr, "id");
                if (!id.empty()) uri = "spotify:track:" + id;
            }

            std::string name = jsonString(tr, "name");
            if (uri.empty() || name.empty()) continue;

            Track t;
            t.title = name;
            t.path = uri;

            // Artists
            cJSON* artistsObj = cJSON_GetObjectItemCaseSensitive(tr, "artists");
            cJSON* aItems = cJSON_GetObjectItemCaseSensitive(artistsObj, "items");
            if (cJSON_IsArray(aItems)) {
                std::string artStr;
                int aCount = cJSON_GetArraySize(aItems);
                for (int a = 0; a < aCount; ++a) {
                    cJSON* artIt = cJSON_GetArrayItem(aItems, a);
                    cJSON* prof = cJSON_GetObjectItemCaseSensitive(artIt, "profile");
                    std::string pName = jsonString(prof, "name");
                    if (!pName.empty()) {
                        if (!artStr.empty()) artStr += ", ";
                        artStr += pName;
                    }
                }
                t.artist = artStr;
                t.albumArtist = artStr;
            }

            // Album
            cJSON* albumObj = cJSON_GetObjectItemCaseSensitive(tr, "albumOfTrack");
            if (albumObj) {
                t.album = jsonString(albumObj, "name");

                // Cover art
                cJSON* coverArt = cJSON_GetObjectItemCaseSensitive(albumObj, "coverArt");
                cJSON* sources = cJSON_GetObjectItemCaseSensitive(coverArt, "sources");
                if (cJSON_IsArray(sources) && cJSON_GetArraySize(sources) > 0) {
                    cJSON* firstSrc = cJSON_GetArrayItem(sources, 0);
                    std::string srcUrl = jsonString(firstSrc, "url");
                    if (!srcUrl.empty()) {
                        outCovers[t.path] = srcUrl;
                    }
                }
            }

            // Duration
            cJSON* durObj = cJSON_GetObjectItemCaseSensitive(tr, "trackDuration");
            if (!durObj) durObj = cJSON_GetObjectItemCaseSensitive(tr, "duration");
            if (durObj) {
                double totalMs = jsonDouble(durObj, "totalMilliseconds");
                if (totalMs > 0) {
                    t.duration = totalMs / 1000.0;
                }
            }

            t.trackNo = int(outTracks.size() + 1);
            t.tagged = true;
            outTracks.push_back(std::move(t));
        }

        cJSON_Delete(root);
        LOG("SpotifyApi::fetchPlaylistTracks: page offset %zu returned %d items, total tracks now %zu", offset, count, outTracks.size());
        offset += count;
        if (count < int(limit)) break;
    }

    LOG("SpotifyApi::fetchPlaylistTracks: retrieved %zu tracks for '%s'", outTracks.size(), playlistUri.c_str());
    return !outTracks.empty();
}

// ---------------------------------------------------------------------------------------------------------------
// SpotiBacan: inicio de sesion con codigo (spotify.com/pair). Lo mismo que SpotiBacan 3DS: el client_id de escritorio
// es el unico que Spotify acepta para este flujo (los de Android/iOS responden invalid_scope), los errores llegan como
// JSON con HTTP 400, y un 400 con authorization_pending es "seguir esperando", no un fallo.
// Con el token, el AP acepta el login tipo AUTHENTICATION_SPOTIFY_TOKEN y devuelve credenciales reutilizables: se
// guardan igual que las del pareo por Wi-Fi, asi que el resto de la app no nota la diferencia.
// ---------------------------------------------------------------------------------------------------------------
namespace {

constexpr const char* kPairClientId = "65b708073fc0480ea92a077233ca87bd";
constexpr const char* kPairScopes =
    "streaming playlist-read-private playlist-read-collaborative user-library-read user-library-modify "
    "user-read-email user-read-private";

/** POST de formulario; true si hubo respuesta HTTP (cualquier codigo), con el cuerpo en out. */
bool postForm(const std::string& url, const std::string& body, std::string& out, long& status) {
    std::string netErr;
    if (!net::begin(netErr)) return false;
    CURL* c = curl_easy_init();
    if (!c) return false;
    MemoryBuffer buf;
    curl_slist* headers = curl_slist_append(nullptr, "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, long(body.size()));
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &buf);
    const CURLcode rc = curl_easy_perform(c);
    status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    curl_slist_free_all(headers);
    out.assign(buf.data.begin(), buf.data.end());
    return rc == CURLE_OK;
}

std::string formEncode(const std::string& s) {
    std::string o;
    char h[4];
    for (unsigned char ch : s) {
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') o.push_back(char(ch));
        else { snprintf(h, sizeof h, "%%%02X", ch); o += h; }
    }
    return o;
}

}  // namespace

double SpotifyApi::nowSecondsF() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void SpotifyApi::startCodePairing() {
    {
        std::scoped_lock l(codeMutex_);
        if (code_.state == 1 || code_.state == 2 || code_.state == 3) return;   // ya en marcha
    }
    stopCodePairing();
    codeQuit_ = false;
    {
        std::scoped_lock l(codeMutex_);
        code_ = CodeState{};
        code_.state = 1;
    }
    codeThread_ = std::thread(&SpotifyApi::codeLoop, this);
}

void SpotifyApi::stopCodePairing() {
    codeQuit_ = true;
    if (codeThread_.joinable()) codeThread_.join();
}

SpotifyApi::CodeState SpotifyApi::codeState() {
    std::scoped_lock l(codeMutex_);
    return code_;
}

void SpotifyApi::codeLoop() {
    cores::moveHere(cores::Net);
    auto fail = [this](int st, const std::string& e) {
        std::scoped_lock l(codeMutex_);
        code_.state = st;
        code_.error = e;
        LOG("SpotifyApi::codeLoop: %s", e.c_str());
    };

    std::string out;
    long status = 0;
    if (!postForm("https://accounts.spotify.com/oauth2/device/authorize",
                  std::string("client_id=") + kPairClientId + "&scope=" + formEncode(kPairScopes), out, status)) {
        fail(5, "Sin conexión con Spotify. Revisa el Wi-Fi.");
        return;
    }
    cJSON* root = cJSON_Parse(out.c_str());
    std::string deviceCode = jsonString(root, "device_code");
    const std::string userCode = jsonString(root, "user_code");
    std::string uri = jsonString(root, "verification_uri");
    const std::string uriComplete = jsonString(root, "verification_uri_complete");
    int interval = int(jsonDouble(root, "interval", 5));
    const double expiresIn = jsonDouble(root, "expires_in", 600);
    const std::string err = jsonString(root, "error");
    cJSON_Delete(root);
    if (status != 200 || deviceCode.empty() || userCode.empty()) {
        fail(5, "Spotify no dio un código" + (err.empty() ? std::string() : " (" + err + ")") + ". Prueba con Nuevo código.");
        return;
    }
    if (uri.empty()) uri = "https://www.spotify.com/pair";
    {
        std::scoped_lock l(codeMutex_);
        code_.state = 2;
        code_.userCode = userCode;
        code_.uri = uri;
        code_.uriComplete = uriComplete.empty() ? uri : uriComplete;
        code_.expiresAt = nowSecondsF() + expiresIn;
    }
    LOG("SpotifyApi::codeLoop: codigo listo, esperando a que lo escriban");

    std::string accessToken;
    const double until = nowSecondsF() + expiresIn;
    while (!codeQuit_.load()) {
        // esperar el intervalo, atento a que el usuario se vaya o a que el pareo por Wi-Fi termine antes
        for (int i = 0; i < interval * 10 && !codeQuit_.load(); ++i) {
            if (pairingJustCompleted_.load() || pairingState_.load() == 3) { codeQuit_ = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (codeQuit_.load()) return;
        if (nowSecondsF() > until) { fail(6, "El código venció. Pide uno nuevo."); return; }
        if (!postForm("https://accounts.spotify.com/api/token",
                      std::string("client_id=") + kPairClientId +
                          "&grant_type=urn:ietf:params:oauth:grant-type:device_code&device_code=" + formEncode(deviceCode),
                      out, status))
            continue;   // una caida de red a mitad no invalida el codigo
        cJSON* r = cJSON_Parse(out.c_str());
        const std::string e = jsonString(r, "error");
        accessToken = jsonString(r, "access_token");
        cJSON_Delete(r);
        if (status == 200 && !accessToken.empty()) break;
        if (e == "authorization_pending") continue;
        if (e == "slow_down") { interval += 5; continue; }
        if (e == "access_denied") { fail(5, "Se rechazó el acceso en el celular. Pide un código nuevo."); return; }
        if (e == "expired_token") { fail(6, "El código venció. Pide uno nuevo."); return; }
        fail(5, "Spotify respondió " + (e.empty() ? "HTTP " + std::to_string(status) : e) + ". Pide un código nuevo.");
        return;
    }
    if (codeQuit_.load() || accessToken.empty()) return;

    {
        std::scoped_lock l(codeMutex_);
        code_.state = 3;
    }
    try {
        auto blob = std::make_shared<cspot::LoginBlob>("SpotiBacan (Switch)");
        blob->authType = 3;   // AUTHENTICATION_SPOTIFY_TOKEN
        blob->authData.assign(accessToken.begin(), accessToken.end());
        blob->username = "";
        auto ctx = cspot::Context::createFromBlob(blob);
        ctx->config.clientId = WEB_CLIENT_ID;
        ctx->session->connectWithRandomAp();
        ctx->config.authData = ctx->session->authenticate(blob);
        ctx->config.username = ctx->session->canonicalUsername;
        if (ctx->config.authData.empty() || ctx->config.username.empty()) {
            fail(5, "Spotify no aceptó el inicio de sesión. Probablemente la cuenta no es Premium.");
            return;
        }
        const std::string credJson = ctx->getCredentialsJson();
        mkdir("sdmc:/config", 0777);
        mkdir("sdmc:/config/SpotiBacan", 0777);
        const char* path = "sdmc:/config/SpotiBacan/credentials.json";
        const std::string tmp = std::string(path) + ".tmp";
        bool written = false;
        if (FILE* f = fopen(tmp.c_str(), "wb")) {
            written = fwrite(credJson.data(), 1, credJson.size(), f) == credJson.size();
            written = (fclose(f) == 0) && written;
        }
        if (written) { remove(path); written = rename(tmp.c_str(), path) == 0; }
        if (!written) { fail(5, "No se pudo guardar la sesión en la SD."); return; }
        remove("sdmc:/config/SpotiBacan/do-not-import-poc-credentials");
        {
            std::scoped_lock lock(mutex_);
            credentialsLoaded_ = loadCredentials();
        }
        {
            std::scoped_lock l(codeMutex_);
            code_.state = 4;
        }
        pairingState_ = 3;
        pairingJustCompleted_ = true;
        LOG("SpotifyApi::codeLoop: sesion iniciada con codigo como %s", ctx->config.username.c_str());
        try { ctx->session->disconnect(); } catch (...) {}
    } catch (const std::exception& e) {
        fail(5, std::string("No se pudo entrar a Spotify: ") + e.what());
    } catch (...) {
        fail(5, "No se pudo entrar a Spotify.");
    }
}
