#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstdint>
#include "library.hpp"
#include "store.hpp"

class SpotifyApi {
public:
    static SpotifyApi& instance();

    bool init();
    bool hasCredentials() const;
    bool authenticate();
    void logout();

    // Zeroconf Spotify Connect pairing for initial login / re-login
    void startPairing();
    void stopPairing();
    bool isPairing() const { return pairingRunning_.load(); }
    int pairingState() const { return pairingState_.load(); }
    bool consumePairingSuccess() { return pairingJustCompleted_.exchange(false); }

    // SpotiBacan: inicio de sesion con codigo (spotify.com/pair, RFC 8628). Corre a la par del pareo por Wi-Fi;
    // el que termine primero guarda las credenciales y marca pairingJustCompleted_, igual que el otro.
    struct CodeState {
        int state = 0;              // 0 nada, 1 pidiendo codigo, 2 esperando, 3 entrando, 4 listo, 5 error, 6 vencido
        std::string userCode, uri, uriComplete, error;
        double expiresAt = 0;       // segundos (reloj de nowSecondsF)
    };
    void startCodePairing();
    void stopCodePairing();
    CodeState codeState();
    static double nowSecondsF();

    // Fetch methods
    bool fetchLikedTracks(std::vector<Track>& outTracks, std::unordered_map<std::string, std::string>& outCovers, size_t maxTracks = 2000);
    bool fetchUserPlaylists(std::vector<FolderPlaylist>& outPlaylists, std::unordered_map<std::string, std::string>& outCovers);
    bool fetchPlaylistTracks(const std::string& playlistUri, std::vector<Track>& outTracks, std::unordered_map<std::string, std::string>& outCovers, size_t maxTracks = 500);

    const std::string& username() const { return username_; }

    // SpotiBacan: a Web API token for the downloader (storage-resolve); empty when it cannot get one.
    std::string webAccessToken();

private:
    SpotifyApi();
    ~SpotifyApi();
    SpotifyApi(const SpotifyApi&) = delete;
    SpotifyApi& operator=(const SpotifyApi&) = delete;

    bool loadCredentials();
    bool requestClientToken();
    bool requestAccessToken();
    bool pathfinderQuery(const std::string& opName, const std::string& opHash, const std::string& variablesJson, std::string& outJson);
    void pairingLoop();

    std::string username_;
    std::vector<uint8_t> authData_;
    std::string clientToken_;
    std::string accessToken_;
    uint64_t tokenExpiryTime_ = 0;
    bool credentialsLoaded_ = false;
    mutable std::mutex mutex_;

    std::atomic<bool> pairingRunning_{ false };
    std::atomic<bool> pairingQuit_{ false };
    std::atomic<int> pairingState_{ 0 }; // 0=idle, 1=waiting for phone, 2=authenticating, 3=done, 4=error
    std::atomic<bool> pairingJustCompleted_{ false };
    std::thread pairingThread_;
    int listenFd_ = -1;

    void codeLoop();
    std::thread codeThread_;
    std::atomic<bool> codeQuit_{ false };
    std::mutex codeMutex_;
    CodeState code_;
};
