#include "logger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <sys/stat.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

// SpotiBacan: el log ya no escribe a la SD en cada linea. Antes cada LOG hacia fprintf + fflush bajo un mutex global
// que tambien toma el hilo de la interfaz: con la sincronizacion y las caratulas escribiendo, la pantalla esperaba a la
// tarjeta y se perdian cuadros (el puntero iba a tirones). Ahora LOG solo agrega al bufer y un hilo lo escribe una vez
// por segundo (o antes si crece mucho). Lo que se pierde ante un cierre brusco es, como mucho, el ultimo segundo.

namespace {
FILE* s_logFile = nullptr;
std::mutex s_logMutex;
std::condition_variable s_cv;
std::string s_pending;
std::thread s_flusher;
std::atomic<bool> s_quit{ false };
auto s_start = std::chrono::steady_clock::now();
constexpr size_t kFlushAt = 32 * 1024;

void flushLoop() {
    std::string out;
    std::unique_lock<std::mutex> lk(s_logMutex);
    while (true) {
        s_cv.wait_for(lk, std::chrono::seconds(1), [] { return s_quit.load() || s_pending.size() >= kFlushAt; });
        out.swap(s_pending);
        FILE* f = s_logFile;
        const bool quit = s_quit.load();
        lk.unlock();
        if (f && !out.empty()) {
            std::fwrite(out.data(), 1, out.size(), f);
            std::fflush(f);
        }
        out.clear();
        lk.lock();
        if (quit && s_pending.empty()) break;
    }
}
}  // namespace

void logInit() {
    std::lock_guard<std::mutex> lk(s_logMutex);
#ifdef __SWITCH__
    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/SpotiBacan", 0777);
    const char* logPath = "sdmc:/config/SpotiBacan/app.log";
    const char* prevPath = "sdmc:/config/SpotiBacan/app.log.previous";
#else
    const char* logPath = "app.log";
    const char* prevPath = "app.log.previous";
#endif
    std::remove(prevPath);
    std::rename(logPath, prevPath);
    s_logFile = std::fopen(logPath, "w");
    s_start = std::chrono::steady_clock::now();
    s_pending = "=== SpotiBacan UI Log Started ===\n";
    if (!s_flusher.joinable()) {
        s_quit = false;
        s_flusher = std::thread(flushLoop);
    }
}

void logWrite(const char* fmt, ...) {
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_start).count();

    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    char line[2100];
    const int n = snprintf(line, sizeof line, "[%08lld ms] %s\n", static_cast<long long>(ms), buf);
    bool big = false;
    {
        std::lock_guard<std::mutex> lk(s_logMutex);
        s_pending.append(line, n > 0 ? std::min<size_t>(size_t(n), sizeof line - 1) : 0);
        big = s_pending.size() >= kFlushAt;
    }
    if (big) s_cv.notify_one();
#ifdef GF_DEV
    std::printf("%s", line);
#endif
}

void logClose() {
    {
        std::lock_guard<std::mutex> lk(s_logMutex);
        s_pending += "=== SpotiBacan UI Log Closed ===\n";
        s_quit = true;
    }
    s_cv.notify_one();
    if (s_flusher.joinable()) s_flusher.join();
    std::lock_guard<std::mutex> lk(s_logMutex);
    if (s_logFile) {
        std::fclose(s_logFile);
        s_logFile = nullptr;
    }
}
