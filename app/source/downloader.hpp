/*
 * SpotiBacan · downloader.hpp
 * Guarda canciones de Spotify en la tarjeta para escucharlas sin internet.
 *
 * Cada cancion queda en sdmc:/music/SpotiBacan/<Artista>/<Album>/<NN - Titulo>.ogg: la biblioteca de la app ya lee
 * sdmc:/music y saca artista y album de esas dos carpetas, asi que lo descargado aparece y suena como cualquier
 * archivo propio. El audio es el Ogg Vorbis de Spotify tal cual (160 kbps), descifrado y sin la cabecera de 167 bytes
 * que Spotify antepone (la misma que salta cspot al reproducir).
 *
 * Usa su propia sesion con el AP de Spotify (las credenciales guardadas al parear), aparte de la del servicio de fondo,
 * y solo mientras la app esta abierta.
 */
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cspot { struct Context; }

struct DlJob {
    std::string uri;     // spotify:track:<base62>
    std::string title, artist, album;
    int trackNo = 0;
};

class Downloader {
public:
    static Downloader& instance();

    /** Pone en cola las canciones que todavia no estan en la tarjeta; devuelve cuantas entraron. */
    int enqueue(const std::vector<DlJob>& jobs);
    /** Vacia la cola (la cancion en curso termina o se corta y se borra su archivo a medias). */
    void cancelAll();
    /** Corta todo y cierra la sesion (al salir de la app). */
    void shutdown();

    struct Status {
        bool active = false;   // hay algo en cola o bajando
        int pending = 0;       // en cola, sin contar la actual
        int done = 0, failed = 0, keyDenied = 0;   // desde que se abrio la app
        std::string current;   // titulo de la que baja
        float progress = 0;    // 0..1 de la actual
        std::string lastError;
    };
    Status status();

    /** Donde queda (o quedaria) una cancion. */
    static std::string pathFor(const DlJob& j);
    static bool isDownloaded(const DlJob& j);

private:
    Downloader() = default;
    ~Downloader();
    void run();
    bool ensureSession(std::string& err);
    void dropSession();
    enum class Result { Ok, KeyDenied, Unavailable, Failed, Cancelled };
    Result fetch(const DlJob& j, std::string& err);

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<DlJob> queue_;
    std::thread worker_, packets_;
    std::atomic<bool> quit_{ false }, cancelCurrent_{ false }, sessionUp_{ false };
    std::shared_ptr<cspot::Context> ctx_;
    Status st_;
    std::atomic<int64_t> got_{ 0 }, total_{ 0 };
};
