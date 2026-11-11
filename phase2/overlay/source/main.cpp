// SPDX-License-Identifier: GPL-2.0-only

#define TESLA_INIT_IMPL

#include "artwork_loader.hpp"
#include "switify/ipc_client.h"

#include <tesla.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

#ifdef SWITIFY_LIGHTWEIGHT_OVERLAY
constexpr const char* OverlayVersion = "v0.5.0";
#else
constexpr const char* OverlayVersion = "v0.5.0";
#endif

constexpr uint64_t SwitifyProgramId = UINT64_C(0x4200000000535754);
constexpr const char* ReadyPath =
    "sdmc:/config/SpotiBacan/background-ready.log";
constexpr const char* ReadyPathLegacy =
    "sdmc:/config/SpotiBacan/background-ready.log";
constexpr const char* ArtworkUrlPath =
    "sdmc:/config/SpotiBacan/current-artwork-url";
constexpr const char* ArtworkUrlPathLegacy =
    "sdmc:/config/SpotiBacan/current-artwork-url";
constexpr const char* OverlayLogPath =
    "sdmc:/config/SpotiBacan/overlay.log";
constexpr auto RefreshInterval = std::chrono::milliseconds(500);
constexpr auto BrowseRefreshInterval = std::chrono::milliseconds(200);
constexpr auto InputClaimInterval = std::chrono::milliseconds(200);
constexpr unsigned AutomaticLaunchAttempts = 4;
constexpr auto StartupReadinessTimeout = std::chrono::seconds(6);
constexpr auto StartupRelaunchDelay = std::chrono::milliseconds(750);

enum class ConnectionState {
  Dormant,
  Starting,
  Connected,
  Incompatible,
  Failed,
};

ConnectionState connectionState = ConnectionState::Dormant;
SwitifyIpcStatus currentStatus{};
uint64_t capabilities = 0;
uint64_t runningPid = 0;
uint64_t connectedPid = 0;
std::string serviceText = "Inactivo";
std::string feedbackText = "Se inicia automaticamente con SpotiBacán";
std::string readyText = "No iniciado";
bool automaticLaunchActive = false;
unsigned automaticLaunchAttempt = 0;
std::chrono::steady_clock::time_point nextAutomaticLaunch{};
uint64_t automaticObservedPid = 0;
std::chrono::steady_clock::time_point automaticObservedAt{};
std::chrono::steady_clock::time_point nextInputClaim{};
std::chrono::steady_clock::time_point nextServiceSupervision{};

void logOverlay(const char* fmt, ...) {
  mkdir("sdmc:/config", 0777);
  mkdir("sdmc:/config/SpotiBacan", 0777);
  FILE* file = std::fopen(OverlayLogPath, "a");
  if (!file) return;

  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  if (size > 256 * 1024) {
    std::fclose(file);
    file = std::fopen(OverlayLogPath, "w");
    if (!file) return;
    std::fprintf(file, "[OVERLAY LOG REINICIADO]\n");
  }

  static const auto logStart = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - logStart).count();
  std::fprintf(file, "[%5u.%03u] ", static_cast<unsigned>(elapsed / 1000),
               static_cast<unsigned>(elapsed % 1000));

  va_list args;
  va_start(args, fmt);
  std::vfprintf(file, fmt, args);
  va_end(args);
  std::fprintf(file, "\n");
  std::fflush(file);
  std::fclose(file);
}

void claimExclusiveInput(bool force = false) {
  const auto now = std::chrono::steady_clock::now();
  if (!force && now < nextInputClaim) return;
  tsl::hlp::requestForeground(true, false);
  nextInputClaim = now + InputClaimInterval;
}

std::string readLastLine(const char* path, const char* unavailable) {
  FILE* file = std::fopen(path, "r");
  if (!file) return unavailable;

  char line[96] = {};
  char last[96] = {};
  while (std::fgets(line, sizeof(line), file)) {
    std::snprintf(last, sizeof(last), "%s", line);
  }
  std::fclose(file);

  std::string value = last[0] ? last : "Empty";
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r'))
    value.pop_back();
  return value;
}

std::string readTextFile(const char* path, size_t maximum = 1024) {
  FILE* file = std::fopen(path, "rb");
  if (!file) return {};
  std::string value;
  value.resize(maximum);
  const size_t length = std::fread(value.data(), 1, value.size(), file);
  std::fclose(file);
  value.resize(length);
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r' ||
          value.back() == '\0'))
    value.pop_back();
  return value;
}

std::string resultText(Result result) {
  char text[64];
  if (result == KERNELRESULT(OutOfResource)) {
    std::snprintf(text, sizeof(text), "Recursos agotados: 0x%08X", result);
    return text;
  }
  std::snprintf(text, sizeof(text), "0x%08X (M%u D%u)", result,
                R_MODULE(result), R_DESCRIPTION(result));
  return text;
}

std::string formatTime(uint32_t milliseconds) {
  const uint32_t seconds = milliseconds / 1000;
  char text[24];
  std::snprintf(text, sizeof(text), "%u:%02u", seconds / 60, seconds % 60);
  return text;
}

std::string boundedText(const char* value, size_t capacity,
                        const char* fallback = "-") {
  const size_t length = strnlen(value, capacity);
  return length == 0 ? fallback : std::string(value, length);
}

bool processIsRunning(uint64_t* pidOut = nullptr) {
  uint64_t pid = 0;
  const Result result = pmdmntGetProcessId(&pid, SwitifyProgramId);
  if (R_FAILED(result) || pid == 0) return false;
  if (pidOut) *pidOut = pid;
  return true;
}

bool internalIpcIsReady(uint64_t pid) {
  unsigned long long markerPid = 0;
  unsigned result = UINT32_MAX;
  const int parsed = std::sscanf(readyText.c_str(), "pass-v043 %llX 0x%X",
                                 &markerPid, &result);
  return parsed == 2 && markerPid == pid && result == 0;
}

void disconnectIpc() {
  switifyIpcExit();
  connectedPid = 0;
  capabilities = 0;
  std::memset(&currentStatus, 0, sizeof(currentStatus));
}

std::string engineStateText(const SwitifyIpcStatus& status) {
  switch (status.service_state) {
    case SwitifyServiceState_Booting:
      return "Iniciando motor de SpotiBacán";
    case SwitifyServiceState_WaitingForCredentials:
      return "Inicia sesion en la app SpotiBacán primero";
    case SwitifyServiceState_Pairing:
      return "Esperando a SpotiBacán Connect";
    case SwitifyServiceState_Connecting:
      return "Conectando a SpotiBacán";
    case SwitifyServiceState_Ready:
      return (status.flags & SwitifyStatusFlag_Connected) ? "En linea"
                                                         : "Listo";
    case SwitifyServiceState_Error:
      return "Error en motor de SpotiBacán";
    case SwitifyServiceState_Stopping:
      return "Deteniendo";
    default:
      return "Estado desconocido";
  }
}

std::string queueText(const SwitifyIpcStatus& status) {
  if (status.queue_count == 0 ||
      status.queue_index == SWITIFY_IPC_INVALID_INDEX)
    return "-";
  return std::to_string(status.queue_index + 1) + " de " +
         std::to_string(status.queue_count);
}

bool controlsAvailable() {
  return connectionState == ConnectionState::Connected &&
         currentStatus.service_state == SwitifyServiceState_Ready &&
         (capabilities & SwitifyIpcCapability_PlaybackControl) != 0;
}

void refreshServiceState(bool updateFeedback = true) {
  static ConnectionState lastLoggedState = ConnectionState::Dormant;
  static uint64_t lastLoggedPid = 0;
  static uint32_t lastLoggedServiceState = UINT32_MAX;

  uint64_t pid = 0;
  if (!processIsRunning(&pid)) {
    if (runningPid != 0 || connectionState != ConnectionState::Dormant) {
      logOverlay("refreshServiceState: proceso no encontrado (era pid=%llu)",
                 static_cast<unsigned long long>(runningPid));
    }
    disconnectIpc();
    runningPid = 0;
    connectionState = ConnectionState::Dormant;
    serviceText = "Inactivo";
    readyText = "No iniciado";
    if (updateFeedback) feedbackText = "El servicio en segundo plano no esta activo";
    lastLoggedState = ConnectionState::Dormant;
    lastLoggedPid = 0;
    lastLoggedServiceState = UINT32_MAX;
    return;
  }

  if (connectedPid != 0 && connectedPid != pid) {
    logOverlay("refreshServiceState: pid cambio de %llu a %llu",
               static_cast<unsigned long long>(connectedPid),
               static_cast<unsigned long long>(pid));
    disconnectIpc();
  }
  runningPid = pid;
  readyText = readLastLine(ReadyPath, "");
  if (!internalIpcIsReady(pid)) {
    readyText = readLastLine(ReadyPathLegacy, "Esperando comprobacion");
  }
  if (!internalIpcIsReady(pid)) {
    disconnectIpc();
    connectionState = ConnectionState::Starting;
    serviceText = "Iniciando";
    if (updateFeedback) feedbackText = readyText;
    if (lastLoggedState != ConnectionState::Starting || lastLoggedPid != pid) {
      logOverlay("refreshServiceState: iniciando pid=%llu marker=%s",
                 static_cast<unsigned long long>(pid), readyText.c_str());
      lastLoggedState = ConnectionState::Starting;
      lastLoggedPid = pid;
    }
    return;
  }

  // Atmosphere can defer GetService while a service is absent. The readiness
  // marker contains this process's PID and is written only after an internal
  // Ping succeeds, so this call cannot freeze the Ultrahand host.
  if (!switifyIpcIsActive()) {
    logOverlay("refreshServiceState: conectando a switify IPC (pid=%llu)...",
               static_cast<unsigned long long>(pid));
    const Result connectResult = switifyIpcInitialize();
    if (R_FAILED(connectResult)) {
      logOverlay("refreshServiceState: switifyIpcInitialize fallo: 0x%08X (M%u D%u)",
                 connectResult, R_MODULE(connectResult), R_DESCRIPTION(connectResult));
      connectionState = ConnectionState::Failed;
      serviceText = "Fallo conexion IPC";
      if (updateFeedback) feedbackText = resultText(connectResult);
      return;
    }
    connectedPid = pid;
    logOverlay("refreshServiceState: conexion IPC establecida con pid=%llu",
               static_cast<unsigned long long>(pid));
  }

  uint32_t version = 0;
  SwitifyIpcStatus status{};
  Result result = switifyIpcPing();
  if (R_SUCCEEDED(result)) result = switifyIpcGetApiVersion(&version);
  if (R_SUCCEEDED(result)) result = switifyIpcGetCapabilities(&capabilities);
  if (R_SUCCEEDED(result)) result = switifyIpcGetStatus(&status);
  if (R_FAILED(result)) {
    logOverlay("refreshServiceState: consulta IPC fallo: 0x%08X (M%u D%u)",
               result, R_MODULE(result), R_DESCRIPTION(result));
    disconnectIpc();
    connectionState = ConnectionState::Failed;
    serviceText = "Fallo solicitud IPC";
    if (updateFeedback) feedbackText = resultText(result);
    return;
  }

  if (version != SWITIFY_IPC_API_VERSION ||
      status.struct_size != sizeof(status) ||
      status.abi_version != SWITIFY_IPC_STATUS_ABI_VERSION ||
      !(capabilities & SwitifyIpcCapability_Status)) {
    logOverlay("refreshServiceState: version incompatible (v=%u esperado %u)",
               version, SWITIFY_IPC_API_VERSION);
    connectionState = ConnectionState::Incompatible;
    serviceText = "Version incompatible";
    if (updateFeedback) feedbackText = "Las versiones del overlay y servicio difieren";
    return;
  }

  currentStatus = status;
  connectionState = ConnectionState::Connected;
  serviceText = engineStateText(status);

  if (lastLoggedState != ConnectionState::Connected ||
      lastLoggedServiceState != static_cast<uint32_t>(status.service_state)) {
    logOverlay("refreshServiceState: conectado, estado del motor=%s (%u)",
               serviceText.c_str(), static_cast<unsigned>(status.service_state));
    lastLoggedState = ConnectionState::Connected;
    lastLoggedServiceState = static_cast<uint32_t>(status.service_state);
  }

  if (status.service_state == SwitifyServiceState_Error && status.album[0] != '\0') {
    feedbackText = boundedText(status.album, sizeof(status.album));
  } else if (status.service_state == SwitifyServiceState_Connecting && status.album[0] != '\0') {
    feedbackText = boundedText(status.album, sizeof(status.album));
  } else if (status.service_state == SwitifyServiceState_WaitingForCredentials) {
    feedbackText = "Inicia sesion en la app SpotiBacán primero";
  } else if (updateFeedback) {
    feedbackText = "Conexion en segundo plano activa";
  }
}

void launchService() {
  logOverlay("launchService: iniciando peticion de arranque");
  refreshServiceState(false);
  if (runningPid != 0) {
    logOverlay("launchService: servicio ya activo con pid=%llu",
               static_cast<unsigned long long>(runningPid));
    feedbackText = "La reproduccion en segundo plano ya esta activa";
    return;
  }

  std::remove(ReadyPath);
  Result result = pmshellInitialize();
  if (R_FAILED(result)) {
    logOverlay("launchService: pmshellInitialize fallo: 0x%08X (M%u D%u)",
               result, R_MODULE(result), R_DESCRIPTION(result));
    serviceText = "Fallo al iniciar";
    feedbackText = "pm:shell " + resultText(result);
    return;
  }

  NcmProgramLocation location{};
  location.program_id = SwitifyProgramId;
  location.storageID = NcmStorageId_None;
  uint64_t pid = 0;
  result = pmshellLaunchProgram(0, &location, &pid);
  pmshellExit();
  if (R_FAILED(result) || pid == 0) {
    logOverlay("launchService: pmshellLaunchProgram fallo: 0x%08X (M%u D%u), pid=%llu",
               result, R_MODULE(result), R_DESCRIPTION(result),
               static_cast<unsigned long long>(pid));
    serviceText = "Fallo al iniciar";
    feedbackText = resultText(result);
    return;
  }

  logOverlay("launchService: programa iniciado exitosamente, nuevo pid=%llu",
             static_cast<unsigned long long>(pid));
  runningPid = pid;
  connectionState = ConnectionState::Starting;
  serviceText = "Iniciando";
  feedbackText = "Iniciando SpotiBacán en segundo plano";
  readyText = "Esperando comprobacion";
}

void beginAutomaticStartup() {
  automaticLaunchActive = true;
  const auto now = std::chrono::steady_clock::now();
  // A process discovered without a working readiness marker is an existing
  // startup attempt, not proof that startup succeeded. The old state machine
  // returned forever merely because a PID existed.
  automaticLaunchAttempt = runningPid == 0 ? 0 : 1;
  automaticObservedPid = runningPid;
  automaticObservedAt = now;
  nextAutomaticLaunch = now;
}

bool terminateServiceForAutomaticRecovery() {
  logOverlay("terminateServiceForAutomaticRecovery: deteniendo programa");
  disconnectIpc();
  Result result = pmshellInitialize();
  if (R_FAILED(result)) {
    logOverlay("terminateServiceForAutomaticRecovery: pmshellInitialize fallo: 0x%08X", result);
    feedbackText = "Recuperacion automatica: pm:shell " + resultText(result);
    return false;
  }
  result = pmshellTerminateProgram(SwitifyProgramId);
  pmshellExit();
  if (R_FAILED(result)) {
    logOverlay("terminateServiceForAutomaticRecovery: pmshellTerminateProgram fallo: 0x%08X", result);
    feedbackText = "Fallo al detener para recuperacion: " + resultText(result);
    return false;
  }

  logOverlay("terminateServiceForAutomaticRecovery: detenido exitosamente");
  runningPid = 0;
  automaticObservedPid = 0;
  connectionState = ConnectionState::Dormant;
  serviceText = "Reiniciando";
  readyText = "Reiniciando tras tiempo de espera";
  return true;
}

void maintainAutomaticStartup() {
  if (!automaticLaunchActive) return;
  const auto now = std::chrono::steady_clock::now();
  if (connectionState == ConnectionState::Connected) {
    // Connected is the healthy state of a persistent supervisor, not the end
    // of automatic startup. Reset its retry budget so an unexpected service
    // exit during Home/Library/Search can be relaunched immediately.
    automaticLaunchAttempt = 0;
    automaticObservedPid = runningPid;
    automaticObservedAt = now;
    nextAutomaticLaunch = now;
    return;
  }
  if (runningPid != 0) {
    if (automaticObservedPid != runningPid) {
      automaticObservedPid = runningPid;
      automaticObservedAt = now;
      if (automaticLaunchAttempt == 0) automaticLaunchAttempt = 1;
      return;
    }
    if (automaticObservedAt.time_since_epoch().count() == 0)
      automaticObservedAt = now;
    if (now - automaticObservedAt < StartupReadinessTimeout) return;

    logOverlay("maintainAutomaticStartup: tiempo de espera de readiness agotado (intento %u de %u)",
               automaticLaunchAttempt, AutomaticLaunchAttempts);
    const bool exhausted = automaticLaunchAttempt >= AutomaticLaunchAttempts;
    if (!terminateServiceForAutomaticRecovery()) {
      automaticLaunchActive = false;
      serviceText = "Fallo recuperacion";
      return;
    }
    if (exhausted) {
      automaticLaunchActive = false;
      serviceText = "Fallo al iniciar";
      feedbackText = "Tiempo de espera agotado tras reintentos";
      logOverlay("maintainAutomaticStartup: reintentos automaticos agotados");
      return;
    }
    feedbackText = "Tiempo agotado; reintentando automaticamente";
    nextAutomaticLaunch = now + StartupRelaunchDelay;
    return;
  }

  if (now < nextAutomaticLaunch) return;
  if (automaticLaunchAttempt >= AutomaticLaunchAttempts) {
    automaticLaunchActive = false;
    feedbackText = "Inicio automatico fallido; abre Ajustes para reintentar";
    logOverlay("maintainAutomaticStartup: limite de reintentos alcanzado");
    return;
  }

  ++automaticLaunchAttempt;
  logOverlay("maintainAutomaticStartup: intento de arranque %u de %u",
             automaticLaunchAttempt, AutomaticLaunchAttempts);
  launchService();
  if (runningPid != 0) {
    automaticObservedPid = runningPid;
    automaticObservedAt = now;
    return;
  }

  const std::string failure = feedbackText;
  if (automaticLaunchAttempt >= AutomaticLaunchAttempts) {
    automaticLaunchActive = false;
    feedbackText = failure + "; reintentos agotados";
    logOverlay("maintainAutomaticStartup: reintentos agotados tras fallo al lanzar");
    return;
  }
  const unsigned delaySeconds = 1U << (automaticLaunchAttempt - 1);
  nextAutomaticLaunch = now + std::chrono::seconds(delaySeconds);
  feedbackText = failure + "; reintentando";
}

void superviseAutomaticService(bool force = false) {
  const auto now = std::chrono::steady_clock::now();
  if (!force && now < nextServiceSupervision) return;
  refreshServiceState(false);
  maintainAutomaticStartup();
  nextServiceSupervision = now + RefreshInterval;
}

void stopService() {
  logOverlay("stopService: deteniendo servicio");
  automaticLaunchActive = false;
  automaticObservedPid = 0;
  disconnectIpc();
  if (!processIsRunning()) {
    logOverlay("stopService: el proceso ya no estaba en ejecucion");
    runningPid = 0;
    connectionState = ConnectionState::Dormant;
    serviceText = "Inactivo";
    feedbackText = "La reproduccion en segundo plano ya esta detenida";
    return;
  }

  Result result = pmshellInitialize();
  if (R_FAILED(result)) {
    logOverlay("stopService: pmshellInitialize fallo: 0x%08X", result);
    serviceText = "Fallo al detener";
    feedbackText = "pm:shell " + resultText(result);
    return;
  }
  result = pmshellTerminateProgram(SwitifyProgramId);
  pmshellExit();
  if (R_FAILED(result)) {
    logOverlay("stopService: pmshellTerminateProgram fallo: 0x%08X", result);
    serviceText = "Fallo al detener";
    feedbackText = resultText(result);
    return;
  }

  logOverlay("stopService: detenido exitosamente");
  runningPid = 0;
  connectionState = ConnectionState::Dormant;
  serviceText = "Inactivo";
  feedbackText = "Reproduccion en segundo plano detenida";
}

template <typename Command>
bool dispatchControl(const char* description, Command command) {
  if (!controlsAvailable()) {
    feedbackText = "Los controles de reproduccion no estan listos";
    return false;
  }
  const Result result = command();
  feedbackText = R_SUCCEEDED(result) ? description : resultText(result);
  return R_SUCCEEDED(result);
}

std::string clipped(std::string value, size_t maximum) {
  if (value.size() <= maximum) return value;
  if (maximum < 4) return value.substr(0, maximum);
  value.resize(maximum - 3);
  return value + "...";
}

std::string browseText(const char* value, size_t capacity) {
  return boundedText(value, capacity, "");
}

template <typename Action>
tsl::elm::ListItem* actionItem(const std::string& text,
                               const std::string& value, Action action) {
  auto* item = new tsl::elm::ListItem(text, value);
  item->setClickListener([action = std::move(action)](uint64_t keys) {
    if (!(keys & HidNpadButton_A)) return false;
    action();
    return true;
  });
  return item;
}

void drawOutline(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                 s32 height, tsl::Color color, s32 thickness = 2) {
  renderer->drawRect(x, y, width, thickness, color);
  renderer->drawRect(x, y + height - thickness, width, thickness, color);
  renderer->drawRect(x, y, thickness, height, color);
  renderer->drawRect(x + width - thickness, y, thickness, height, color);
}

class PlayerElement final : public tsl::elm::Element {
 public:
  static constexpr s32 Height = 475;

  tsl::elm::Element* requestFocus(tsl::elm::Element*,
                                  tsl::FocusDirection) override {
    return this;
  }

  void layout(u16, u16, u16, u16) override {
    setBoundaries(getX(), getY(), getWidth(), Height);
  }

  void sync() {
    std::string next = readTextFile(ArtworkUrlPath);
    if (next.empty()) next = readTextFile(ArtworkUrlPathLegacy);
    if (next != artworkUrl) artworkUrl = next;

    // Starting artwork networking here rather than in initServices() leaves
    // the launch path free of the artwork BSD session and worker thread.  A
    // repeated request is cheap and lets artwork start as soon as a previously
    // booting service becomes healthy.
    if (connectionState == ConnectionState::Connected &&
        currentStatus.service_state == SwitifyServiceState_Ready &&
        !artworkUrl.empty()) {
      switify::artwork::request(artworkUrl, ArtSize);
    }
  }

  bool onClick(u64 keys) override {
    if (keys & HidNpadButton_Up) {
      if (focus < 5) {
        focus = 5;
        return true;
      }
      if (focus == 6) {
        focus = 2;
        return true;
      }
      return false;
    }
    if (keys & HidNpadButton_Down) {
      if (focus == 5) {
        focus = 2;
        return true;
      }
      if (focus < 5) {
        focus = 6;
        return true;
      }
      return false;
    }
    if (focus == 5) {
      if (keys & HidNpadButton_Left) {
        seekBy(-15000);
        return true;
      }
      if (keys & HidNpadButton_Right) {
        seekBy(15000);
        return true;
      }
      return false;
    }
    if (focus == 6) {
      if (keys & HidNpadButton_Left) {
        changeVolume(-50);
        return true;
      }
      if (keys & HidNpadButton_Right) {
        changeVolume(50);
        return true;
      }
      return false;
    }
    if (keys & HidNpadButton_Left) {
      if (focus > 0) --focus;
      return true;
    }
    if (keys & HidNpadButton_Right) {
      if (focus < 4) ++focus;
      return true;
    }
    if (keys & HidNpadButton_A) {
      activate(focus);
      return true;
    }
    if (keys & HidNpadButton_X) {
      toggleRepeat();
      return true;
    }
    if (keys & HidNpadButton_Y) {
      toggleShuffle();
      return true;
    }
    return false;
  }

  void draw(tsl::gfx::Renderer* renderer) override {
    const tsl::Color green = a({0x1, 0xD, 0x6, 0xF});
    const tsl::Color dimGreen = a({0x1, 0x8, 0x5, 0xA});
    const tsl::Color text = a(tsl::style::color::ColorText);
    const tsl::Color dim = a(tsl::style::color::ColorHeaderBar);
    const tsl::Color panel = a(tsl::style::color::ColorFrame);
    const s32 x = getX();
    const s32 y = getY();
    const s32 width = getWidth();
    const s32 center = x + width / 2;
    const s32 artX = center - ArtSize / 2;
    const s32 artY = y + 14;

    renderer->drawRect(x + 4, y + 4, width - 8, Height - 8, panel);
    drawOutline(renderer, x + 4, y + 4, width - 8, Height - 8,
                hasFocus() ? green : dimGreen, hasFocus() ? 3 : 2);

    const uint8_t* pixels = nullptr;
    int artWidth = 0;
    int artHeight = 0;
    switify::artwork::borrow(&pixels, &artWidth, &artHeight);
    if (pixels && artWidth > 0 && artHeight > 0) {
      const s32 decodedX = artX + (ArtSize - artWidth) / 2;
      const s32 decodedY = artY + (ArtSize - artHeight) / 2;
      renderer->drawBitmap(decodedX, decodedY, artWidth, artHeight, pixels);
    } else {
      renderer->drawRect(artX, artY, ArtSize, ArtSize,
                         a({0x1, 0x2, 0x2, 0xF}));
      drawOutline(renderer, artX, artY, ArtSize, ArtSize, dimGreen);
      renderer->drawLine(center + 18, artY + 70, center + 18,
                         artY + 142, green);
      renderer->drawLine(center + 18, artY + 70, center + 58,
                         artY + 61, green);
      renderer->drawCircle(center + 3, artY + 148, 15, true, green);
    }

    const bool hasTrack = currentStatus.flags & SwitifyStatusFlag_HasTrack;
    const std::string title = clipped(
        hasTrack ? boundedText(currentStatus.title, sizeof(currentStatus.title))
                 : "Elige algo para reproducir",
        31);
    const std::string artist = clipped(
        hasTrack ? boundedText(currentStatus.artist,
                               sizeof(currentStatus.artist))
                 : serviceText,
        36);
    centeredText(renderer, title, center, artY + ArtSize + 31, 23, text);
    centeredText(renderer, artist, center, artY + ArtSize + 54, 17, dim);

    const s32 barX = x + 20;
    const s32 barY = artY + ArtSize + 78;
    const s32 barWidth = width - 40;
    const float fraction = currentStatus.duration_ms == 0
                               ? 0.0f
                               : std::clamp(
                                     static_cast<float>(currentStatus.position_ms) /
                                         currentStatus.duration_ms,
                                     0.0f, 1.0f);
    if (hasFocus() && focus == 5)
      renderer->drawRect(barX - 2, barY - 4, barWidth + 4, 11,
                         a({0x1, 0xD, 0x6, 0x7}));
    renderer->drawRect(barX, barY, barWidth, 3, a({0xA, 0xA, 0xA, 0xA}));
    const s32 fill = static_cast<s32>(barWidth * fraction);
    if (fill > 0) renderer->drawRect(barX, barY - 2, fill, 7, green);
    renderer->drawCircle(barX + fill, barY + 1, 6, true, green);
    renderer->drawString(formatTime(currentStatus.position_ms), false, barX,
                         barY + 26, 16, text);
    const std::string total = formatTime(currentStatus.duration_ms);
    const auto [totalWidth, totalHeight] = renderer->drawString(
        total, false, 0, 0, 16, tsl::style::color::ColorTransparent);
    (void)totalHeight;
    renderer->drawString(total, false, barX + barWidth - totalWidth,
                         barY + 26, 16, text);

    const s32 controlsY = artY + ArtSize + 139;
    const s32 spacing = (width - 70) / 4;
    for (int index = 0; index < 5; ++index) {
      const s32 controlX = x + 35 + spacing * index;
      if (hasFocus() && focus == index)
        renderer->drawRect(controlX - 20, controlsY - 20, 40, 40,
                           a({0x1, 0xD, 0x6, 0x5}));
      drawControl(renderer, index, controlX, controlsY,
                  index == 0 &&
                          (currentStatus.flags &
                           (SwitifyStatusFlag_RepeatAll |
                            SwitifyStatusFlag_RepeatOne))
                      ? green
                      : (index == 4 &&
                                 (currentStatus.flags &
                                  SwitifyStatusFlag_Shuffle)
                             ? green
                             : text));
    }

    const s32 volumePercentY = controlsY + 43;
    const s32 volumeBarX = x + 112;
    const s32 volumeBarY = controlsY + 66;
    const s32 volumeBarWidth = width - 138;
    const uint32_t volume = std::min(UINT32_C(1000),
                                     currentStatus.volume_permille);
    // Keep the label on the slider's row. Only the percentage belongs above.
    renderer->drawString("Volumen", false, x + 20, volumeBarY + 6, 16, text);
    const std::string volumeText = std::to_string(volume / 10) + "%";
    const auto [volumeTextWidth, volumeTextHeight] = renderer->drawString(
        volumeText, false, 0, 0, 16, tsl::style::color::ColorTransparent);
    (void)volumeTextHeight;
    renderer->drawString(volumeText, false, x + width - 20 - volumeTextWidth,
                         volumePercentY, 16, text);
    if (hasFocus() && focus == 6)
      renderer->drawRect(volumeBarX - 3, volumeBarY - 5,
                         volumeBarWidth + 6, 13, a({0x1, 0xD, 0x6, 0x7}));
    renderer->drawRect(volumeBarX, volumeBarY, volumeBarWidth, 3,
                       a({0xA, 0xA, 0xA, 0xA}));
    const s32 volumeFill = static_cast<s32>(volumeBarWidth * volume / 1000.0f);
    if (volumeFill > 0)
      renderer->drawRect(volumeBarX, volumeBarY - 2, volumeFill, 7, green);
    renderer->drawCircle(volumeBarX + volumeFill, volumeBarY + 1, 6, true,
                         green);
  }

 private:
  static constexpr s32 ArtSize = 176;

  void centeredText(tsl::gfx::Renderer* renderer, const std::string& value,
                    s32 center, s32 baseline, u32 size,
                    tsl::Color color) const {
    const auto [width, height] = renderer->drawString(
        value, false, 0, 0, size, tsl::style::color::ColorTransparent);
    (void)height;
    renderer->drawString(value, false, center - width / 2, baseline, size,
                         color);
  }

  void drawControl(tsl::gfx::Renderer* renderer, int index, s32 x, s32 y,
                   tsl::Color color) const {
    if (index == 0) {
      renderer->drawLine(x - 13, y - 7, x + 11, y - 7, color);
      renderer->drawLine(x + 11, y - 7, x + 6, y - 12, color);
      renderer->drawLine(x + 11, y - 7, x + 6, y - 2, color);
      renderer->drawLine(x + 13, y + 7, x - 11, y + 7, color);
      renderer->drawLine(x - 11, y + 7, x - 6, y + 2, color);
      renderer->drawLine(x - 11, y + 7, x - 6, y + 12, color);
      if (currentStatus.flags & SwitifyStatusFlag_RepeatOne)
        renderer->drawString("1", false, x + 8, y + 2, 12, color);
    } else if (index == 1 || index == 3) {
      const bool next = index == 3;
      renderer->drawRect(x + (next ? 10 : -12), y - 12, 3, 24, color);
      for (s32 line = 0; line <= 20; ++line) {
        const s32 half = next ? (20 - line) * 12 / 20
                              : line * 12 / 20;
        const s32 px = x - 10 + line;
        renderer->drawLine(px, y - half, px, y + half, color);
      }
    } else if (index == 2) {
      if (currentStatus.playback_state == SwitifyPlaybackState_Playing) {
        renderer->drawRect(x - 9, y - 13, 6, 26, color);
        renderer->drawRect(x + 4, y - 13, 6, 26, color);
      } else {
        for (s32 line = 0; line <= 24; ++line) {
          const s32 half = (24 - line) * 13 / 24;
          renderer->drawLine(x - 10 + line, y - half, x - 10 + line,
                             y + half, color);
        }
      }
    } else {
      renderer->drawLine(x - 14, y - 9, x - 5, y - 9, color);
      renderer->drawLine(x - 5, y - 9, x + 7, y + 9, color);
      renderer->drawLine(x + 7, y + 9, x + 14, y + 9, color);
      renderer->drawLine(x + 14, y + 9, x + 9, y + 4, color);
      renderer->drawLine(x + 14, y + 9, x + 9, y + 14, color);
      renderer->drawLine(x - 14, y + 9, x - 5, y + 9, color);
      renderer->drawLine(x - 5, y + 9, x + 7, y - 9, color);
      renderer->drawLine(x + 7, y - 9, x + 14, y - 9, color);
      renderer->drawLine(x + 14, y - 9, x + 9, y - 14, color);
      renderer->drawLine(x + 14, y - 9, x + 9, y - 4, color);
    }
  }

  void seekBy(int32_t delta) {
    if (!(currentStatus.flags & SwitifyStatusFlag_CanSeek)) return;
    const int64_t target = std::clamp<int64_t>(
        static_cast<int64_t>(currentStatus.position_ms) + delta, 0,
        currentStatus.duration_ms);
    if (dispatchControl("Posicion cambiada", [target] {
          return switifyIpcSeek(static_cast<uint32_t>(target));
        }))
      currentStatus.position_ms = static_cast<uint32_t>(target);
  }

  void changeVolume(int32_t delta) {
    const int32_t value = std::clamp<int32_t>(
        static_cast<int32_t>(currentStatus.volume_permille) + delta, 0, 1000);
    if (dispatchControl("Volumen cambiado", [value] {
          return switifyIpcSetVolume(static_cast<uint32_t>(value));
        }))
      currentStatus.volume_permille = static_cast<uint32_t>(value);
  }

  void toggleRepeat() {
    SwitifyRepeatMode mode = SwitifyRepeatMode_All;
    const char* message = "Repetir todo";
    if (currentStatus.flags & SwitifyStatusFlag_RepeatAll) {
      mode = SwitifyRepeatMode_One;
      message = "Repetir una";
    } else if (currentStatus.flags & SwitifyStatusFlag_RepeatOne) {
      mode = SwitifyRepeatMode_Off;
      message = "Repetir desactivado";
    }
    if (dispatchControl(message,
                        [mode] { return switifyIpcSetRepeatMode(mode); })) {
      currentStatus.flags &= ~(SwitifyStatusFlag_RepeatAll |
                               SwitifyStatusFlag_RepeatOne);
      if (mode == SwitifyRepeatMode_All)
        currentStatus.flags |= SwitifyStatusFlag_RepeatAll;
      else if (mode == SwitifyRepeatMode_One)
        currentStatus.flags |= SwitifyStatusFlag_RepeatOne;
    }
  }

  void toggleShuffle() {
    const bool enabled = !(currentStatus.flags & SwitifyStatusFlag_Shuffle);
    if (dispatchControl(enabled ? "Aleatorio activado" : "Aleatorio desactivado",
                        [enabled] { return switifyIpcSetShuffle(enabled); })) {
      if (enabled)
        currentStatus.flags |= SwitifyStatusFlag_Shuffle;
      else
        currentStatus.flags &= ~SwitifyStatusFlag_Shuffle;
    }
  }

  void activate(int index) {
    switch (index) {
      case 0:
        toggleRepeat();
        break;
      case 1:
        dispatchControl("Cancion anterior", switifyIpcPrevious);
        break;
      case 2: {
        const bool playing = currentStatus.playback_state ==
                             SwitifyPlaybackState_Playing;
        if (dispatchControl(playing ? "En pausa" : "Reproduciendo",
                            playing ? switifyIpcPause : switifyIpcPlay)) {
          currentStatus.playback_state =
              playing ? SwitifyPlaybackState_Paused
                      : SwitifyPlaybackState_Playing;
        }
        break;
      }
      case 3:
        dispatchControl("Siguiente cancion", switifyIpcNext);
        break;
      case 4:
        toggleShuffle();
        break;
    }
  }

  int focus = 2;
  std::string artworkUrl;
};

uint64_t currentBrowseGeneration() {
  SwitifyIpcBrowseHeader header{};
  return R_SUCCEEDED(switifyIpcGetBrowseHeader(&header)) ? header.generation
                                                         : 0;
}

class BrowseGui final : public tsl::Gui {
 public:
  explicit BrowseGui(uint64_t previousGeneration = 0)
      : previousGeneration(previousGeneration) {}

  tsl::elm::Element* createUI() override {
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame("SpotiBacán", "Explorador de SpotiBacán");
    auto* list = new tsl::elm::List();
    Result result = switifyIpcGetBrowseHeader(&header);
    if (R_FAILED(result) ||
        header.struct_size != sizeof(SwitifyIpcBrowseHeader) ||
        header.abi_version != SWITIFY_IPC_BROWSE_ABI_VERSION) {
      renderedUnavailable = true;
      list->addItem(new tsl::elm::CategoryHeader("Explorador no disponible"));
      list->addItem(new tsl::elm::ListItem(
          automaticLaunchActive ? "El servicio se esta reiniciando"
                                : "No se pudo leer el contenido de SpotiBacán",
          resultText(result)));
    } else if (header.generation == previousGeneration &&
               header.state != SwitifyBrowseState_Loading) {
      list->addItem(new tsl::elm::CategoryHeader("Abriendo SpotiBacán"));
      list->addItem(new tsl::elm::ListItem("Cargando...", "Espera un momento"));
    } else {
      const std::string title = browseText(header.title, sizeof(header.title));
      const std::string subtitle =
          browseText(header.subtitle, sizeof(header.subtitle));
      list->addItem(new tsl::elm::CategoryHeader(
          title.empty() ? "SpotiBacán" : title));
      if (!subtitle.empty())
        list->addItem(
            new tsl::elm::CategoryHeader(clipped(subtitle, 42)));
      if (header.state == SwitifyBrowseState_Loading) {
        list->addItem(new tsl::elm::ListItem("Cargando contenido de SpotiBacán...", ""));
      } else if (header.state == SwitifyBrowseState_Error) {
        list->addItem(new tsl::elm::ListItem(
            clipped(browseText(header.message, sizeof(header.message)), 44),
            "B Atras"));
      } else if (header.state == SwitifyBrowseState_Ready) {
        populateItems(list);
      } else {
        list->addItem(new tsl::elm::ListItem("Nada que mostrar aun", ""));
      }
    }
    renderedGeneration = header.generation;
    renderedState = header.state;
    renderedCount = header.item_count;
    frame->setContent(list);
    nextRefresh = std::chrono::steady_clock::now() + BrowseRefreshInterval;
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
    const auto now = std::chrono::steady_clock::now();
    if (now < nextRefresh) return;
    if (connectionState != ConnectionState::Connected) {
      if (!renderedUnavailable) {
        tsl::swapTo<BrowseGui>(previousGeneration);
        return;
      }
      nextRefresh = now + BrowseRefreshInterval;
      return;
    }
    if (renderedUnavailable) {
      tsl::swapTo<BrowseGui>(previousGeneration);
      return;
    }
    SwitifyIpcBrowseHeader latest{};
    if (R_SUCCEEDED(switifyIpcGetBrowseHeader(&latest)) &&
        (latest.generation != renderedGeneration ||
         latest.state != renderedState || latest.item_count != renderedCount)) {
      tsl::swapTo<BrowseGui>(previousGeneration);
      return;
    }
    nextRefresh = now + BrowseRefreshInterval;
  }

  bool handleInput(u64 keysDown, u64, const HidTouchState&,
                   HidAnalogStickState, HidAnalogStickState) override {
    if (keysDown & HidNpadButton_B) {
      // A network request must never trap the user inside Switify. Returning
      // false lets libtesla pop this GUI immediately without waiting for IPC.
      // Re-entering Home/Library/Search starts a fresh root request.
      if (renderedState == SwitifyBrowseState_Loading) return false;

      // The service can advance the browse generation while a large collection
      // is loading or being paged.  Never send Back with the GUI's cached
      // generation: that turns a perfectly valid B press into a silent no-op.
      SwitifyIpcBrowseHeader latest{};
      const Result headerResult = switifyIpcGetBrowseHeader(&latest);
      if (R_FAILED(headerResult) ||
          latest.struct_size != sizeof(SwitifyIpcBrowseHeader) ||
          latest.abi_version != SWITIFY_IPC_BROWSE_ABI_VERSION) {
        feedbackText = resultText(headerResult);
        return false;
      }
      if (latest.depth == 0) return false;

      const uint64_t old = latest.generation;
      const Result result = switifyIpcBrowseBack(old);
      if (R_SUCCEEDED(result)) {
        previousGeneration = old;
        nextRefresh = std::chrono::steady_clock::now();
      } else {
        feedbackText = resultText(result);
        return false;
      }
      return true;
    }
    return false;
  }

 private:
  void populateItems(tsl::elm::List* list) {
    if (header.item_count == 0) {
      list->addItem(new tsl::elm::ListItem("Sin resultados", "B Atras"));
      return;
    }
    const uint32_t count = std::min<uint32_t>(header.item_count, 128);
    for (uint32_t index = 0; index < count; ++index) {
      SwitifyIpcBrowseItem item{};
      if (R_FAILED(switifyIpcGetBrowseItem(index, &item)) ||
          item.generation != header.generation)
        continue;
      std::string name = browseText(item.name, sizeof(item.name));
      std::string value = browseText(item.subtitle, sizeof(item.subtitle));
      if (item.kind == SwitifyBrowseItemKind_Track) {
        if (!value.empty()) name += " - " + value;
        name = clipped(name, 42);
        value = item.duration_ms > 0 ? formatTime(item.duration_ms) : "";
      } else {
        name = clipped(name, 36);
        value = clipped(value, 18);
      }
      auto* row = actionItem(name.empty() ? "Sin titulo" : name, value,
                             [this, index, kind = item.kind] {
        const uint64_t old = header.generation;
        const Result result =
            kind == SwitifyBrowseItemKind_Track
                ? switifyIpcBrowsePlay(old, index)
                : switifyIpcBrowseOpen(old, index);
        if (R_FAILED(result)) {
          feedbackText = resultText(result);
        } else if (kind != SwitifyBrowseItemKind_Track) {
          previousGeneration = old;
          nextRefresh = std::chrono::steady_clock::now();
        }
      });
      list->addItem(row);
    }
  }

  uint64_t previousGeneration = 0;
  SwitifyIpcBrowseHeader header{};
  uint64_t renderedGeneration = 0;
  uint32_t renderedState = SwitifyBrowseState_Idle;
  uint32_t renderedCount = 0;
  bool renderedUnavailable = false;
  std::chrono::steady_clock::time_point nextRefresh{};
};

class SearchKeyboardElement final : public tsl::elm::Element {
 public:
  tsl::elm::Element* requestFocus(tsl::elm::Element*,
                                  tsl::FocusDirection) override {
    return this;
  }

  void layout(u16, u16, u16, u16) override {
    setBoundaries(getX(), getY(), getWidth(), getHeight());
  }

  bool onClick(u64 keys) override {
    if (keys & HidNpadButton_AnyUp) {
      if (row > 0) --row;
      clampColumn();
      return true;
    }
    if (keys & HidNpadButton_AnyDown) {
      if (row + 1 < static_cast<int>(Rows.size())) ++row;
      clampColumn();
      return true;
    }
    if (keys & HidNpadButton_AnyLeft) {
      if (column > 0) --column;
      return true;
    }
    if (keys & HidNpadButton_AnyRight) {
      if (column + 1 < static_cast<int>(Rows[row].size())) ++column;
      return true;
    }
    if (keys & HidNpadButton_X) {
      if (!query.empty()) query.pop_back();
      return true;
    }
    if (keys & HidNpadButton_A) {
      activate(Rows[row][column]);
      return true;
    }
    return false;
  }

  void draw(tsl::gfx::Renderer* renderer) override {
    const s32 x = getX();
    const s32 y = getY();
    const s32 width = getWidth();
    const tsl::Color green = a({0x1, 0xD, 0x6, 0xF});
    renderer->drawRect(x + 10, y + 14, width - 20, 43,
                       a(tsl::style::color::ColorFrame));
    renderer->drawString(query.empty() ? "Buscar en SpotiBacán" : clipped(query, 34),
                         false, x + 20, y + 43, 21,
                         a(tsl::style::color::ColorText));
    const s32 top = y + 76;
    const s32 rowHeight = 45;
    for (size_t r = 0; r < Rows.size(); ++r) {
      const auto& keys = Rows[r];
      const s32 cellWidth = (width - 20) / static_cast<s32>(keys.size());
      for (size_t c = 0; c < keys.size(); ++c) {
        const s32 keyX = x + 10 + static_cast<s32>(c) * cellWidth;
        const s32 keyY = top + static_cast<s32>(r) * rowHeight;
        renderer->drawRect(
            keyX + 2, keyY + 2, cellWidth - 4, rowHeight - 5,
            r == static_cast<size_t>(row) && c == static_cast<size_t>(column)
                ? green
                : a(tsl::style::color::ColorFrame));
        const std::string label = keys[c] == "SHIFT" && shifted
                                      ? "shift"
                                      : keys[c];
        const auto [textWidth, textHeight] = renderer->drawString(
            label, false, 0, 0, 16,
            tsl::style::color::ColorTransparent);
        (void)textHeight;
        renderer->drawString(label, false,
                             keyX + (cellWidth - textWidth) / 2, keyY + 27,
                             16, a(tsl::style::color::ColorText));
      }
    }
  }

 private:
  inline static const std::vector<std::vector<std::string>> Rows = {
      {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
      {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
      {"a", "s", "d", "f", "g", "h", "j", "k", "l"},
      {"z", "x", "c", "v", "b", "n", "m", "-"},
      {"SHIFT", "SPACE", "DEL", "SEARCH"}};

  void clampColumn() {
    column = std::clamp(column, 0, static_cast<int>(Rows[row].size()) - 1);
  }

  void activate(const std::string& key) {
    if (key == "SHIFT") {
      shifted = !shifted;
    } else if (key == "SPACE") {
      if (query.size() < SWITIFY_IPC_BROWSE_QUERY_SIZE - 1) query += ' ';
    } else if (key == "DEL") {
      if (!query.empty()) query.pop_back();
    } else if (key == "SEARCH") {
      if (query.empty()) return;
      const uint64_t old = currentBrowseGeneration();
      const Result result = switifyIpcBrowseSearch(query.c_str());
      if (R_SUCCEEDED(result))
        tsl::swapTo<BrowseGui>(old);
      else
        feedbackText = resultText(result);
    } else if (query.size() < SWITIFY_IPC_BROWSE_QUERY_SIZE - 1) {
      char character = key[0];
      if (shifted)
        character = static_cast<char>(
            std::toupper(static_cast<unsigned char>(character)));
      query += character;
      shifted = false;
    }
  }

  std::string query;
  int row = 1;
  int column = 0;
  bool shifted = false;
};

class SearchGui final : public tsl::Gui {
 public:
  tsl::elm::Element* createUI() override {
    // Foreground applications can reclaim controller input after Tesla's
    // initial hand-off. Search must own HID for its entire lifetime so typing
    // never repeats A/D-pad input in the application underneath.
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame(
        "Buscar", "A escribir   X borrar   B cancelar");
    frame->setContent(new SearchKeyboardElement());
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
  }
};

void openBrowser(Result (*request)()) {
  superviseAutomaticService(true);
  if (connectionState != ConnectionState::Connected ||
      currentStatus.service_state != SwitifyServiceState_Ready ||
      !(capabilities & SwitifyIpcCapability_Browsing)) {
    if (connectionState == ConnectionState::Connected &&
        currentStatus.service_state == SwitifyServiceState_Error) {
      feedbackText = currentStatus.album[0] != '\0'
                         ? boundedText(currentStatus.album, sizeof(currentStatus.album))
                         : "Error en motor de SpotiBacán";
    } else {
      feedbackText = "SpotiBacán aun se esta iniciando";
    }
    return;
  }
  const uint64_t old = currentBrowseGeneration();
  const Result result = request();
  if (R_SUCCEEDED(result))
    tsl::changeTo<BrowseGui>(old);
  else
    feedbackText = resultText(result);
}

class ServiceGui final : public tsl::Gui {
 public:
  tsl::elm::Element* createUI() override {
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame("SpotiBacán", "Servicio y ajustes");
    auto* list = new tsl::elm::List();
    list->addItem(new tsl::elm::CategoryHeader("Reproduccion en segundo plano"));
    serviceItem = new tsl::elm::ListItem("Servicio", serviceText);
    feedbackItem = new tsl::elm::ListItem("Detalles", clipped(feedbackText, 36));
    list->addItem(serviceItem);
    list->addItem(feedbackItem);
    retryItem = actionItem("Reintentar inicio automatico", "A", [this] {
      if (runningPid != 0 && currentStatus.service_state == SwitifyServiceState_Error) {
        terminateServiceForAutomaticRecovery();
      }
      beginAutomaticStartup();
      maintainAutomaticStartup();
      sync();
    });
    stopItem = actionItem("Detener reproduccion en segundo plano", "A", [this] {
      stopService();
      sync();
    });
    list->addItem(retryItem);
    list->addItem(stopItem);
    list->addItem(actionItem("Actualizar conexion", "A", [this] {
      refreshServiceState();
      sync();
    }));
    list->addItem(new tsl::elm::CategoryHeader("Audio"));
    qualityItem = actionItem("Calidad de streaming", qualityText(), [this] {
      if (connectionState != ConnectionState::Connected ||
          !(capabilities & SwitifyIpcCapability_AudioSettings)) {
        feedbackText = "Actualiza el overlay y el servicio juntos";
        sync();
        return;
      }
      const bool high =
          !(currentStatus.flags & SwitifyStatusFlag_HighQuality);
      const Result result = switifyIpcSetAudioQuality(high);
      if (R_SUCCEEDED(result)) {
        if (high)
          currentStatus.flags |= SwitifyStatusFlag_HighQuality;
        else
          currentStatus.flags &= ~SwitifyStatusFlag_HighQuality;
        feedbackText = high ? "Calidad alta guardada; aplica al reiniciar"
                            : "Calidad normal guardada; aplica al reiniciar";
      } else {
        feedbackText = resultText(result);
      }
      sync();
    });
    list->addItem(qualityItem);
    list->addItem(new tsl::elm::ListItem(
        "Normal 160 kbps / Alta 320 kbps", "Reiniciar para aplicar"));
    list->addItem(new tsl::elm::CategoryHeader(
        "Inicia sesion una vez en la app SpotiBacán"));
    list->addItem(new tsl::elm::CategoryHeader("Registros (Logs)"));
    list->addItem(new tsl::elm::ListItem("Log overlay", "/config/SpotiBacan/overlay.log"));
    list->addItem(new tsl::elm::ListItem("Log motor", "/config/SpotiBacan/background.log"));
    list->addItem(new tsl::elm::CategoryHeader("Creditos"));
    list->addItem(new tsl::elm::ListItem("Port moderno y motor", "noesio582-boop"));
    list->addItem(new tsl::elm::ListItem("Repo base antiguo", "HiroshiYamauchi"));
    list->addItem(new tsl::elm::ListItem("GitHub", "noesio582-boop"));
    list->addItem(new tsl::elm::ListItem("SpotiBacán Connect", "Activado (Nintendo Switch)"));
    frame->setContent(list);
    sync();
    nextRefresh = std::chrono::steady_clock::now() + RefreshInterval;
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
    const auto now = std::chrono::steady_clock::now();
    if (now < nextRefresh) return;
    sync();
    nextRefresh = now + RefreshInterval;
  }

 private:
  void sync() {
    if (!serviceItem) return;
    serviceItem->setValue(serviceText);
    feedbackItem->setValue(clipped(feedbackText, 36));
    retryItem->isLocked =
        (runningPid != 0 &&
         currentStatus.service_state != SwitifyServiceState_Error);
    stopItem->isLocked = runningPid == 0;
    if (qualityItem) qualityItem->setValue(qualityText());
  }

  std::string qualityText() const {
    return (currentStatus.flags & SwitifyStatusFlag_HighQuality)
               ? "Alta - 320 kbps"
               : "Normal - 160 kbps";
  }

  std::chrono::steady_clock::time_point nextRefresh{};
  tsl::elm::ListItem* serviceItem = nullptr;
  tsl::elm::ListItem* feedbackItem = nullptr;
  tsl::elm::ListItem* retryItem = nullptr;
  tsl::elm::ListItem* stopItem = nullptr;
  tsl::elm::ListItem* qualityItem = nullptr;
};

class NowPlayingGui final : public tsl::Gui {
 public:
  tsl::elm::Element* createUI() override {
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame("SpotiBacán", "Reproduciendo");
    auto* list = new tsl::elm::List();
    player = new PlayerElement();
    list->addItem(player, PlayerElement::Height);
    list->addItem(new tsl::elm::CategoryHeader("Sesion de reproduccion"));
    list->addItem(actionItem("Playlist", queueText(currentStatus), [] {
      openBrowser(switifyIpcBrowseCurrentPlaylist);
    }));
    list->addItem(actionItem("Cola", "A continuacion", [] {
      openBrowser(switifyIpcBrowseQueue);
    }));
    frame->setContent(list);
    player->sync();
    nextRefresh = std::chrono::steady_clock::now() + RefreshInterval;
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
    const auto now = std::chrono::steady_clock::now();
    if (now < nextRefresh) return;
    if (player) player->sync();
    nextRefresh = now + RefreshInterval;
  }

 private:
  PlayerElement* player = nullptr;
  std::chrono::steady_clock::time_point nextRefresh{};
};

class MainGui final : public tsl::Gui {
 public:
  tsl::elm::Element* createUI() override {
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame("SpotiBacán", OverlayVersion);
    auto* list = new tsl::elm::List();
    player = new PlayerElement();
    list->addItem(player, PlayerElement::Height);
    list->addItem(new tsl::elm::CategoryHeader("Explorar SpotiBacán"));
    list->addItem(actionItem("Inicio", "Para ti", [] {
      openBrowser(switifyIpcBrowseHome);
    }));
    list->addItem(actionItem("Buscar", "Canciones y mas", [] {
      if (connectionState == ConnectionState::Connected &&
          currentStatus.service_state == SwitifyServiceState_Ready &&
          (capabilities & SwitifyIpcCapability_Search)) {
        // Consume the A press that opens Search before changing screens.
        claimExclusiveInput(true);
        tsl::changeTo<SearchGui>();
      } else {
        if (connectionState == ConnectionState::Connected &&
            currentStatus.service_state == SwitifyServiceState_Error) {
          feedbackText = currentStatus.album[0] != '\0'
                             ? boundedText(currentStatus.album, sizeof(currentStatus.album))
                             : "Error en motor de SpotiBacán";
        } else {
          feedbackText = "SpotiBacán aun se esta iniciando";
        }
      }
    }));
    list->addItem(actionItem("Tu biblioteca", "Musica guardada", [] {
      openBrowser(switifyIpcBrowseLibrary);
    }));
    list->addItem(actionItem("Reproduciendo", "Playlist y cola", [] {
      tsl::changeTo<NowPlayingGui>();
    }));
    settingsItem = actionItem("Ajustes", serviceText, [] {
      tsl::changeTo<ServiceGui>();
    });
    list->addItem(settingsItem);
    frame->setContent(list);
    player->sync();
    nextRefresh = std::chrono::steady_clock::now() + RefreshInterval;
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
    const auto now = std::chrono::steady_clock::now();
    if (now < nextRefresh) return;
    if (player) player->sync();
    if (settingsItem) settingsItem->setValue(serviceText);
    nextRefresh = now + RefreshInterval;
  }

 private:
  PlayerElement* player = nullptr;
  tsl::elm::ListItem* settingsItem = nullptr;
  std::chrono::steady_clock::time_point nextRefresh{};
};

class NXtifyOverlay final : public tsl::Overlay {
 public:
  void initServices() override {
    logOverlay("SpotiBacánOverlay: initServices (version %s)", OverlayVersion);
    refreshServiceState(false);
    if (connectionState != ConnectionState::Connected &&
        connectionState != ConnectionState::Incompatible) {
      beginAutomaticStartup();
      maintainAutomaticStartup();
    }
  }

  void exitServices() override {
    logOverlay("SpotiBacánOverlay: exitServices");
    switify::artwork::shutdown();
    disconnectIpc();
  }

  void onHide() override {
    logOverlay("SpotiBacánOverlay: onHide");
    // The playback engine is a separate Atmosphere process and continues
    // normally. Release every overlay-owned worker, BSD session, and IPC
    // handle before Ultrahand returns to the game or HOME menu.
    switify::artwork::shutdown();
    disconnectIpc();
  }

  void onShow() override {
    logOverlay("SpotiBacánOverlay: onShow");
    refreshServiceState(false);
    if (connectionState != ConnectionState::Connected &&
        connectionState != ConnectionState::Incompatible) {
      beginAutomaticStartup();
      maintainAutomaticStartup();
    }
  }

  std::unique_ptr<tsl::Gui> loadInitialGui() override {
    return initially<MainGui>();
  }
};

}  // namespace

int main(int argc, char** argv) {
  return tsl::loop<NXtifyOverlay>(argc, argv);
}
