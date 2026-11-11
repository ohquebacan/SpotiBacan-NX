#include "spotibacan_migrate.hpp"
#include "background_backend.hpp"
#include "ipc_self_test.hpp"
#include "switify_service.hpp"

#include "NXConsoleLog.h"

#include <curl/curl.h>
#include <switch.h>

#include <atomic>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <new>
#include <thread>

namespace {

constexpr SocketInitConfig SocketConfig = {
    .tcp_tx_buf_size = 0x800,
    .tcp_rx_buf_size = 0x2000,
    .tcp_tx_buf_max_size = 0x4000,
    .tcp_rx_buf_max_size = 0x8000,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0x2400,
    .sb_efficiency = 2,
    .num_bsd_sessions = 3,
    .bsd_service_type = BsdServiceType_User,
};

Result socketInitResult = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

void abortUnless(Result result) {
  if (R_FAILED(result)) diagAbortWithResult(result);
}

}  // namespace

extern "C" {

uint32_t __nx_applet_type = AppletType_None;
uint32_t __nx_fs_num_sessions = 2;

int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start_routine)(void*), void* arg);

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start_routine)(void*), void* arg) {
  if (!attr) {
    pthread_attr_t boundedAttr;
    if (pthread_attr_init(&boundedAttr) == 0) {
      pthread_attr_setstacksize(&boundedAttr, 0x8000);
      const int rc = __real_pthread_create(thread, &boundedAttr, start_routine, arg);
      pthread_attr_destroy(&boundedAttr);
      return rc;
    }
  }
  return __real_pthread_create(thread, attr, start_routine, arg);
}

void __libnx_initheap(void) {
  // Peak recorded heap across 13h continuous playback in real hardware was
  // 908 KiB used (1119 KiB arena). Setting heap to 1792 KiB (1.75 MiB) leaves
  // ~670 KiB of safe surplus while shrinking process BSS by 1.25 MiB, allowing
  // pmshellLaunchProgram / svcCreateProcess to spawn the service reliably even
  // under heavy memory fragmentation in the Applet pool.
  static char heap[1792 * 1024];
  extern char* fake_heap_start;
  extern char* fake_heap_end;
  fake_heap_start = heap;
  fake_heap_end = heap + sizeof(heap);
}

void __appInit(void) {
  abortUnless(smInitialize());
  abortUnless(fsInitialize());
  abortUnless(fsdevMountSdmc());
  socketInitResult = socketInitialize(&SocketConfig);
}

void __appExit(void) {
  curl_global_cleanup();
  if (R_SUCCEEDED(socketInitResult)) socketExit();
  fsdevUnmountDevice("sdmc");
  fsExit();
  smExit();
}

}  // extern "C"

int main(int, char**) {
  spotibacanMigrateConfig("sdmc:");
  mkdir("/config", 0755);
  mkdir("/config/SpotiBacan", 0755);
  nxlog::installBellLogger();
  nxlog::setLogFile("/config/SpotiBacan/background.log", 512 * 1024);
  nxlog::setQueueEnabled(false);
#ifdef SWITIFY_LIGHTWEIGHT_OVERLAY
  nxlog::push(
      "SpotiBacán Phase 2 background engine 0.5.0-lite1 starting");
#else
  nxlog::push("SpotiBacán Phase 2 background engine 0.5.0 starting");
#endif
  nxlog::push("Compatibility profile: Applet pool, lazy audout, 2 BSD sessions (bsd:u)");
  nxlog::push("Network profile: TCP-only; UDP disabled");
  nxlog::pushHeapUsage("startup");

  // ServiceCore owns the fixed-capacity browsing snapshot and is far larger
  // than the 16 KiB main-thread stack. Keep all long-lived engine state on the
  // process heap so adding browse fields cannot silently overflow that stack.
  auto core = std::unique_ptr<switify::phase2::ServiceCore>(
      new (std::nothrow) switify::phase2::ServiceCore());
  if (!core)
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
  nxlog::pushHeapUsage("service core ready");
  abortUnless(switify::phase2::switch_service::initialize(*core));

  auto backend = std::unique_ptr<switify::phase2::BackgroundBackend>(
      new (std::nothrow) switify::phase2::BackgroundBackend(*core));
  if (!backend)
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));

  std::atomic<bool> backendStarted{false};
  std::atomic<bool> supervisorRunning{true};

  if (R_SUCCEEDED(socketInitResult)) {
    if (backend->start()) {
      backendStarted.store(true);
      nxlog::pushHeapUsage("SpotiBacán backend started");
    }
  }

  std::thread supervisorThread([&core, &backend, &backendStarted, &supervisorRunning]() {
    while (supervisorRunning.load() && !backendStarted.load()) {
      if (R_FAILED(socketInitResult)) {
        socketInitResult = socketInitialize(&SocketConfig);
        if (R_FAILED(socketInitResult)) {
          char detail[40];
          std::snprintf(detail, sizeof(detail), "Red no disponible: 0x%08X",
                        socketInitResult);
          core->publishServiceState(SwitifyServiceState_Connecting, 0);
          core->publishTrack({}, {}, detail, 0);
          for (int i = 0; i < 20 && supervisorRunning.load(); ++i) {
            svcSleepThread(100000000ULL); // 100ms
          }
          continue;
        }
        nxlog::push("Socket initialization succeeded on background retry");
      }
      if (backend->start()) {
        backendStarted.store(true);
        nxlog::pushHeapUsage("SpotiBacán backend started");
        break;
      } else {
        core->publishServiceState(SwitifyServiceState_Connecting, 0);
        core->publishTrack({}, {}, "Conectando a SpotiBacán...", 0);
        for (int i = 0; i < 20 && supervisorRunning.load(); ++i) {
          svcSleepThread(100000000ULL);
        }
      }
    }
  });

  abortUnless(switify::phase2::ipc_self_test::start());

  switify::phase2::switch_service::loop();

  supervisorRunning.store(false);
  if (supervisorThread.joinable()) supervisorThread.join();
  if (backendStarted.load()) backend->stop();
  abortUnless(switify::phase2::switch_service::exit());
  return 0;
}
