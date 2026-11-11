/*
 * Songate · platform_switch.cpp
 * The platform on the Nintendo Switch: libnx.
 */
#include "platform.hpp"

#include <switch.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#include "gfbg.h"
#include "logger.hpp"

namespace plat {

static_assert(u64(BtnA) == u64(HidNpadButton_A) && u64(BtnB) == u64(HidNpadButton_B) && u64(BtnX) == u64(HidNpadButton_X) && u64(BtnY) == u64(HidNpadButton_Y));
static_assert(u64(BtnStickL) == u64(HidNpadButton_StickL) && u64(BtnStickR) == u64(HidNpadButton_StickR));
static_assert(u64(BtnL) == u64(HidNpadButton_L) && u64(BtnR) == u64(HidNpadButton_R) && u64(BtnZL) == u64(HidNpadButton_ZL) && u64(BtnZR) == u64(HidNpadButton_ZR));
static_assert(u64(BtnPlus) == u64(HidNpadButton_Plus) && u64(BtnMinus) == u64(HidNpadButton_Minus));
static_assert(u64(BtnLeft) == u64(HidNpadButton_Left) && u64(BtnUp) == u64(HidNpadButton_Up) && u64(BtnRight) == u64(HidNpadButton_Right) && u64(BtnDown) == u64(HidNpadButton_Down));

namespace {

constexpr u64 kBgProgram = 0x4200000000535754ull;

PadState pad;
HidSixAxisSensorHandle sixAxisHandles[4]{};
HidSixAxisSensorHandle sixAxisRightSingle{};
bool sixAxisInited = false;
AppletHookCookie cookie{};
LeaveFn leaveFn = nullptr;
void* leaveArg = nullptr;
bool romfsInited = false;
bool plInited = false;
bool psmInited = false;
bool lblInited = false;
bool nifmInited = false;

void onApplet(AppletHookType type, void*) {
    if (leaveFn && (type == AppletHookType_OnExitRequest || (type == AppletHookType_OnFocusState && appletGetFocusState() != AppletFocusState_InFocus)))
        leaveFn(type == AppletHookType_OnExitRequest, leaveArg);
}

/** The console's file system read directly: up to 64 entries with their sizes a request, where stat() asked once a file
 *  (a folder of thousands held the app for many seconds: Details and Delete measure before they show). */
void measureFs(FsFileSystem* fs, const std::string& path, int& files, uint64_t& bytes, int cap) {
    FsDir d;
    char p[FS_MAX_PATH];
    snprintf(p, sizeof p, "%s", path.c_str());
    if (R_FAILED(fsFsOpenDirectory(fs, p, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d))) return;
    static thread_local std::vector<FsDirectoryEntry> buf(64);
    std::vector<std::string> dirs;
    for (;;) {
        s64 n = 0;
        if (files >= cap || R_FAILED(fsDirRead(&d, &n, buf.size(), buf.data())) || n <= 0) break;
        for (s64 i = 0; i < n; i++) {
            const FsDirectoryEntry& e = buf[size_t(i)];
            if (e.type == FsDirEntryType_Dir) dirs.push_back(e.name);
            else { bytes += uint64_t(e.file_size); files++; }
        }
    }
    fsDirClose(&d);
    for (const auto& n : dirs) {
        if (files >= cap) break;
        measureFs(fs, (path == "/" ? "/" : path + "/") + n, files, bytes, cap);
    }
}

/** The same for Storage: stat() asked once per file (thousands of requests for the covers cache, each one queued with
 *  everything else). */
void measureUse(FsFileSystem* fs, const std::string& path, int64_t& bytes, int& files, bool (*keep)(const std::string&), bool top) {
    FsDir d;
    char p[FS_MAX_PATH];
    snprintf(p, sizeof p, "%s", path.c_str());
    if (R_FAILED(fsFsOpenDirectory(fs, p, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d))) {
        FsFile f;   // a file, not a folder
        s64 size = 0;
        if (R_SUCCEEDED(fsFsOpenFile(fs, p, FsOpenMode_Read, &f))) {
            if (R_SUCCEEDED(fsFileGetSize(&f, &size))) { bytes += size; files++; }
            fsFileClose(&f);
        }
        return;
    }
    static thread_local std::vector<FsDirectoryEntry> buf(64);
    std::vector<std::string> dirs;
    for (;;) {
        s64 n = 0;
        if (R_FAILED(fsDirRead(&d, &n, buf.size(), buf.data())) || n <= 0) break;
        for (s64 i = 0; i < n; i++) {
            const FsDirectoryEntry& e = buf[size_t(i)];
            if (top && keep && !keep(e.name)) continue;
            if (e.type == FsDirEntryType_Dir) dirs.push_back(e.name);
            else { bytes += e.file_size; files++; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));   // a breath between batches for the rest of the app
    }
    fsDirClose(&d);
    for (const auto& n : dirs) measureUse(fs, (path == "/" ? "/" : path + "/") + n, bytes, files, keep, false);
}

}  // namespace

// ---- the app's life

void init() {
    Result rc = romfsInit();
    romfsInited = R_SUCCEEDED(rc);
    LOG("plat::init romfsInit: 0x%08X", (unsigned)rc);

    rc = plInitialize(PlServiceType_User);
    plInited = R_SUCCEEDED(rc);
    LOG("plat::init plInitialize: 0x%08X", (unsigned)rc);

    rc = psmInitialize();
    psmInited = R_SUCCEEDED(rc);
    LOG("plat::init psmInitialize: 0x%08X", (unsigned)rc);

    rc = lblInitialize();   // Dim the screen: the handheld backlight (failing is fine: a veil instead)
    lblInited = R_SUCCEEDED(rc);
    LOG("plat::init lblInitialize: 0x%08X", (unsigned)rc);

    rc = nifmInitialize(NifmServiceType_User);
    nifmInited = R_SUCCEEDED(rc);
    LOG("plat::init nifmInitialize: 0x%08X", (unsigned)rc);
}

void quit() {
    LOG("plat::quit start");
    if (sixAxisInited) {
        hidStopSixAxisSensor(sixAxisHandles[0]);
        hidStopSixAxisSensor(sixAxisHandles[1]);
        hidStopSixAxisSensor(sixAxisHandles[2]);
        hidStopSixAxisSensor(sixAxisHandles[3]);
        hidStopSixAxisSensor(sixAxisRightSingle);
        sixAxisInited = false;
    }
    if (nifmInited) { nifmExit(); nifmInited = false; }
    if (lblInited) { lblExit(); lblInited = false; }
    if (psmInited) { psmExit(); psmInited = false; }
    if (plInited) { plExit(); plInited = false; }
    if (romfsInited) { romfsExit(); romfsInited = false; }
    LOG("plat::quit done");
}

bool mainLoop() { return appletMainLoop(); }
void holdExit() {
    if (appletGetAppletType() == AppletType_Application)
        appletLockExit();
}
void releaseExit() {
    if (appletGetAppletType() == AppletType_Application)
        appletUnlockExit();
}

void watchLeave(LeaveFn fn, void* arg) {
    leaveFn = fn;
    leaveArg = arg;
    appletHook(&cookie, onApplet, nullptr);
}
void unwatchLeave() { appletUnhook(&cookie); }

bool inFocus() { return appletGetFocusState() == AppletFocusState_InFocus; }

std::string selfPath(int argc, char** argv) {
    // hbloader gives the .nro's path
    if (argc > 0 && argv[0] && strncmp(argv[0], "sdmc:/", 6) == 0 && strstr(argv[0], ".nro")) return argv[0];
    return "sdmc:/switch/SpotiBacan/SpotiBacan.nro";   // (abierta desde un acceso del menu HOME: no llega la ruta)
}

bool swapsSelf() { return true; }

bool relaunch(const std::string& program) {
    if (!envHasNextLoad()) return false;
    envSetNextLoad(program.c_str(), program.c_str());
    return true;
}

bool legacyData() { return true; }

// ---- input

void padInit() {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    hidGetSixAxisSensorHandles(&sixAxisHandles[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    hidGetSixAxisSensorHandles(&sixAxisHandles[1], 1, HidNpadIdType_No1,      HidNpadStyleTag_NpadFullKey);
    hidGetSixAxisSensorHandles(&sixAxisHandles[2], 2, HidNpadIdType_No1,      HidNpadStyleTag_NpadJoyDual);
    hidGetSixAxisSensorHandles(&sixAxisRightSingle, 1, HidNpadIdType_No1,      HidNpadStyleTag_NpadJoyRight);

    hidStartSixAxisSensor(sixAxisHandles[0]);
    hidStartSixAxisSensor(sixAxisHandles[1]);
    hidStartSixAxisSensor(sixAxisHandles[2]);
    hidStartSixAxisSensor(sixAxisHandles[3]);
    hidStartSixAxisSensor(sixAxisRightSingle);
    sixAxisInited = true;
}
void padRead() { padUpdate(&pad); }
uint64_t padDown() { return padGetButtonsDown(&pad); }
uint64_t padHeld() { return padGetButtons(&pad); }
Stick stick(int which) {
    const HidAnalogStickState s = padGetStickPos(&pad, which);
    return { s.x, s.y };
}

bool isJoyConDetached() {
    const u32 style = padGetStyleSet(&pad);
    if (style & HidNpadStyleTag_NpadHandheld) return false;
    const u32 attr = padGetAttributes(&pad);
    if (attr & HidNpadAttribute_IsRightWired) return false;
    if (style & (HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadJoyRight)) return true;
    if ((attr & HidNpadAttribute_IsRightConnected) && !(style & HidNpadStyleTag_NpadFullKey)) return true;
    return false;
}

bool rightJoyConMotion(float& dx, float& dy) {
    dx = 0.f;
    dy = 0.f;
    if (!sixAxisInited) return false;

    const u32 style = padGetStyleSet(&pad);
    HidSixAxisSensorHandle handle{};
    bool hasHandle = false;
    if (style & HidNpadStyleTag_NpadJoyDual) {
        handle = sixAxisHandles[3];
        hasHandle = true;
    } else if (style & HidNpadStyleTag_NpadJoyRight) {
        handle = sixAxisRightSingle;
        hasHandle = true;
    }
    if (!hasHandle) return false;

    HidSixAxisSensorState state{};
    size_t count = hidGetSixAxisSensorStates(handle, &state, 1);
    if (count == 0) {
        hidStartSixAxisSensor(handle);
        count = hidGetSixAxisSensorStates(handle, &state, 1);
    }
    if (count == 0) return false;

    const float wx = state.angular_velocity.x;
    const float wy = state.angular_velocity.y;
    const float wz = state.angular_velocity.z;

    const float ax = state.acceleration.x;
    const float ay = state.acceleration.y;
    const float az = state.acceleration.z;

    const float glen = std::sqrt(ax * ax + ay * ay + az * az);
    float upY = 1.f;
    float upZ = 0.f;
    if (glen > 0.1f) {
        upY = -ay / glen;
        upZ = -az / glen;
    }

    const float yaw = -(wy * upY + wz * upZ);
    const float pitch = -wx;

    constexpr float kDeadzone = 0.025f;
    const float speed = std::hypot(yaw, pitch);
    if (speed < kDeadzone) {
        return false;
    }

    const float factor = (speed - kDeadzone) / speed;
    dx = yaw * factor;
    dy = pitch * factor;
    return true;
}

// ---- typing

TextResult askText(const char* header, const char* guide, const char* initial, char* out, size_t size) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return TextResult::Unavailable;
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetGuideText(&kbd, guide);
    swkbdConfigSetHeaderText(&kbd, header);
    if (initial) swkbdConfigSetInitialText(&kbd, initial);
    const Result rc = swkbdShow(&kbd, out, size);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) ? TextResult::Done : TextResult::Cancelled;
}

// ---- the machine

void setMediaPlaying(bool on) { appletSetMediaPlaybackState(on); }

void setKeepAwake(bool on) {
    // (1.1 called appletSetAutoSleepTimeAndDimmingTimeEnabled, which only an overlay applet may: from an application it did
    // nothing, so the console dimmed by itself whenever the music was paused.) An application's own ways: the idle
    // time stretched as far as it goes, and auto-sleep off.
    appletSetIdleTimeDetectionExtension(on ? AppletIdleTimeDetectionExtension_ExtendedUnsafe : AppletIdleTimeDetectionExtension_None);
    appletSetAutoSleepDisabled(on);
}

void requestSleep() { appletRequestToSleep(); }

/** Handheld only: docked, the TV is not ours. */
bool dimBacklight(float& saved) {
    if (appletGetOperationMode() != AppletOperationMode_Handheld || R_FAILED(lblGetCurrentBrightnessSetting(&saved)) ||
        R_FAILED(lblSetCurrentBrightnessSetting(std::min(saved, 0.04f))))
        return false;
    lblApplyCurrentBrightnessSettingToBacklight();
    return true;
}

void restoreBacklight(float saved) {
    lblSetCurrentBrightnessSetting(saved);
    lblApplyCurrentBrightnessSettingToBacklight();
}

bool batteryLevel(unsigned& percent) {
    u32 p = 0;
    if (R_FAILED(psmGetBatteryChargePercentage(&p))) return false;
    percent = p;
    return true;
}

bool batteryRaw(double& percent, bool& charging) {
    PsmChargerType ct = PsmChargerType_Unconnected;
    if (R_FAILED(psmGetRawBatteryChargePercentage(&percent)) || R_FAILED(psmGetChargerType(&ct))) return false;
    charging = ct != PsmChargerType_Unconnected;
    return true;
}

bool wifiSignal(int& bars) {
    if (!nifmInited) {
        bars = 0;
        return false;
    }
    NifmInternetConnectionType type{};
    u32 strength = 0;
    NifmInternetConnectionStatus status{};
    if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &status)) ||
        status != NifmInternetConnectionStatus_Connected) {
        bars = 0;
        return false;
    }
    if (type == NifmInternetConnectionType_Ethernet) {
        bars = 3;
        return true;
    }
    bars = std::clamp(int(strength), 1, 3);
    return true;
}

void sysInfo(std::string& firmware, std::string& atmosphere, bool& docked) {
    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) firmware = fw.display_version;
        setsysExit();
    }
    if (R_SUCCEEDED(splInitialize())) {
        u64 v = 0;
        if (R_SUCCEEDED(splGetConfig(SplConfigItem(65000), &v)))   // Exosphère's API version: Atmosphère's own
            atmosphere = std::to_string((v >> 56) & 0xFF) + "." + std::to_string((v >> 48) & 0xFF) + "." + std::to_string((v >> 40) & 0xFF);
        splExit();
    }
    docked = appletGetOperationMode() == AppletOperationMode_Console;
}

long long bootStamp() { return (long long)time(nullptr) - (long long)(armTicksToNs(armGetSystemTick()) / 1000000000ull); }

void placeThread(int core, int priority) {
    AppletType at = appletGetAppletType();
    if (at != AppletType_Application) {
        LOG("placeThread: applet mode %d active; skipping core %d mask to prevent kernel panic", int(at), core);
        return;
    }
    svcSetThreadPriority(CUR_THREAD_HANDLE, u32(priority));
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
}

namespace {
constexpr SocketInitConfig SocketConfig = {
    .tcp_tx_buf_size = 0x800,
    .tcp_rx_buf_size = 0x2000,
    .tcp_tx_buf_max_size = 0x2EE0,
    .tcp_rx_buf_max_size = 0x8000,
    .udp_tx_buf_size = 0x2400,
    .udp_rx_buf_size = 0xA500,
    .sb_efficiency = 2,
    .num_bsd_sessions = 2,
    .bsd_service_type = BsdServiceType_User,
};
}

bool netStart() {
    Result rc = socketInitialize(&SocketConfig);
    LOG("plat::netStart: socketInitialize rc=0x%08X", (unsigned)rc);
    return R_SUCCEEDED(rc);
}
void netStop() {
    socketExit();
    LOG("plat::netStop: socketExit called");
}

// ---- the background player

int bgProtocol() { return GFBG_PROTOCOL; }

bool bgRestart(std::string& err) {
    if (R_FAILED(pmshellInitialize())) { err = "The console did not allow it: restart the console instead"; return false; }
    pmshellTerminateProgram(kBgProgram);   // fails harmlessly when it is not running
    svcSleepThread(300'000'000ull);
    NcmProgramLocation loc{};
    loc.program_id = kBgProgram;
    loc.storageID = NcmStorageId_None;
    u64 pid = 0;
    const Result rc = pmshellLaunchProgram(0, &loc, &pid);
    pmshellExit();
    if (R_FAILED(rc)) { err = "It did not start again: restart the console"; return false; }
    return true;
}

bool bgEnd() {
    const bool pm = R_SUCCEEDED(pmshellInitialize());
    if (pm) {
        pmshellTerminateProgram(kBgProgram);   // fails harmlessly when it is not running
        svcSleepThread(300'000'000ull);
    }
    return pm;
}

void bgStart(bool ended) {
    if (!ended) return;
    NcmProgramLocation loc{};
    loc.program_id = kBgProgram;
    loc.storageID = NcmStorageId_None;
    u64 pid = 0;
    pmshellLaunchProgram(0, &loc, &pid);   // not installed: fails harmlessly
    pmshellExit();
}

// ---- files

std::string assetPath(const char* relative) { return std::string("romfs:/") + relative; }

std::vector<FontData> systemFonts() {
    std::vector<FontData> out;
    for (PlSharedFontType t : { PlSharedFontType_Standard, PlSharedFontType_ChineseSimplified, PlSharedFontType_ExtChineseSimplified,
                                PlSharedFontType_ChineseTraditional, PlSharedFontType_KO }) {
        PlFontData fd;
        if (R_SUCCEEDED(plGetSharedFontByType(&fd, t))) out.push_back({ fd.address, fd.size });
    }
    return out;
}

const char* configParent() { return "sdmc:/config"; }
const char* configDir() { return "sdmc:/config/NXtify"; }
const char* backupDir() { return "sdmc:/config/NXtify-backups"; }
const char* root() { return "sdmc:/"; }
const char* rootName() { return "the SD card"; }
bool isRoot(const std::string& path) { return path.empty() || path == "sdmc:/" || path == "sdmc:"; }

std::string parentDir(const std::string& path) {
    const size_t s = path.find_last_of('/');
    return s == std::string::npos || s <= 5 ? std::string("sdmc:/") : path.substr(0, s);
}

std::string join(const std::string& dir, const std::string& name) { return dir.empty() || dir.back() == '/' ? dir + name : dir + "/" + name; }
size_t rootLen(const std::string& path) { return path.compare(0, 6, "sdmc:/") == 0 ? 6 : 0; }
std::string fromRoot(const std::string& path) { return "sdmc:" + path; }

const std::vector<const char*>& systemFolders() {
    static const std::vector<const char*> v = { "sdmc:/atmosphere", "sdmc:/bootloader", "sdmc:/switch", "sdmc:/Nintendo", "sdmc:/config", "sdmc:/emuMMC" };
    return v;
}

bool measureFiles(const std::string& path, int& files, uint64_t& bytes, int cap) {
    if (path.rfind("sdmc:/", 0) != 0) return false;
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs) return false;
    measureFs(fs, path.substr(5), files, bytes, cap);
    return true;
}

bool measureStorage(const std::string& path, int64_t& bytes, int& files, bool (*keep)(const std::string&)) {
    if (path.rfind("sdmc:/", 0) != 0) return false;
    FsFileSystem* fs = fsdevGetDeviceFileSystem("sdmc");
    if (!fs) return false;
    measureUse(fs, path.substr(5), bytes, files, keep, true);
    return true;
}

}  // namespace plat
