// Readiness gate: injection only starts once the system is quiet.
//
//  * calculateFolderSizes=everything (and [Host] waitForEverything=1):
//      Everything must be running, its database loaded and not busy
//      (Everything IPC: EVERYTHING_IPC_IS_DB_LOADED / EVERYTHING_IPC_IS_DB_BUSY,
//      folder-size indexing checked with EVERYTHING_IPC_IS_FILE_INFO_INDEXED),
//      Everything's own disk I/O and CPU must be low, the system disk must be
//      mostly idle for [Host] idleSeconds in a row, and a real folder-size
//      query (the same SDK3 pipe / IPC the mod uses) must answer in < 500 ms.
//  * other modes: a short startup delay + the same disk-idle window.
//
// The gate runs on its own thread; nothing here ever blocks a UI thread or
// Everything's main thread (all IPC uses SendMessageTimeout / cancellable
// pipe I/O with short timeouts). GNU GPL v3.
#pragma once

#include "../common/bfs_common.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace bfs {

struct GateConfig {
    bool everythingMode = false;      // calculateFolderSizes=everything
    bool waitForEverything = true;    // [Host] waitForEverything
    unsigned startupDelaySec = 5;     // [Host] startupDelay
    unsigned idleSeconds = 10;        // [Host] idleSeconds
    unsigned maxDiskBusyPercent = 30; // [Host] maxDiskBusyPercent
    double maxEverythingIoMBps = 2;   // [Host] maxEverythingIoMBps
    unsigned maxEverythingCpuPercent = 25;  // [Host] maxEverythingCpuPercent
    unsigned readyTimeoutSec = 0;     // [Host] readyTimeout, 0 = wait forever
    unsigned injectIntervalMs = 200;  // [Host] injectIntervalMs

    static GateConfig Load(const std::wstring& iniPath);
    bool UsesEverything() const { return everythingMode && waitForEverything; }
};

enum class GateState {
    Off,             // gate not running
    Starting,        // startup delay
    WaitEverything,  // Everything not running
    WaitDbLoaded,
    WaitDbBusy,
    WaitIdle,        // disk / Everything I/O
    WaitTestQuery,
    Ready,
    ReadyTimeout,    // readyTimeout reached, injecting anyway
    Bypassed,        // --now
};

struct GateStatus {
    GateState state = GateState::Off;
    unsigned idleSoFar = 0;
    unsigned idleNeeded = 0;
    int diskBusyPercent = -1;      // -1 = unknown (PDH unavailable)
    double everythingIoMBps = -1;  // -1 = unknown
    int everythingCpuPercent = -1;
    bool everythingRunning = false;
    bool ipcDenied = false;        // Everything elevated, IPC blocked by UIPI
    int folderSizeIndexed = -1;    // -1 unknown, 0 no, 1 yes
    int lastQueryMs = -1;
    unsigned secondsLeft = 0;      // Starting
    std::wstring Text() const;     // Chinese, for tray/status
};

// One-shot snapshot of Everything's state (used by the gate and
// `bfs-injector status`). Never blocks for more than ~2 s.
struct EverythingProbe {
    bool running = false;
    bool ipcOk = false;
    bool ipcDenied = false;
    bool dbLoaded = false;
    bool dbBusy = false;
    int folderSizeIndexed = -1;
    DWORD pid = 0;
    std::wstring instance;  // L"" or L"1.5a"
};
EverythingProbe ProbeEverything();

// Folder-size test query for `folder` through the SDK3 pipe (Everything
// 1.5) or the IPC v2 WM_COPYDATA query (1.4). Returns elapsed ms, or -1 on
// failure / timeout. Must be called on a thread that may pump messages.
int TestFolderSizeQuery(const std::wstring& folder, DWORD timeoutMs,
                        const std::wstring& instance);

// Sample of system disk busy %, via PDH "\PhysicalDisk(_Total)\% Idle Time"
// (English counter name, works on localized Windows).
class DiskMonitor {
   public:
    DiskMonitor();
    ~DiskMonitor();
    int SampleBusyPercent();  // -1 if unavailable
   private:
    void* query_ = nullptr;
    void* counter_ = nullptr;
};

class ReadinessGate {
   public:
    using LogFn = std::function<void(const std::wstring&)>;
    ReadinessGate();
    ~ReadinessGate();
    ReadinessGate(const ReadinessGate&) = delete;
    ReadinessGate& operator=(const ReadinessGate&) = delete;

    // bypass=true opens immediately (bfs-injector --now).
    void Start(const GateConfig& cfg, bool bypass, bool skipStartupDelay,
               LogFn log, std::function<void()> onChange);
    // Bounded; returns false if the thread had to be left behind.
    bool Stop(DWORD timeoutMs = 4000);

    bool IsOpen() const;
    // Waits until open or until `abortEvent` is signaled. True if open.
    bool WaitOpen(HANDLE abortEvent) const;
    GateStatus Status() const;
    const GateConfig& Config() const { return cfg_; }

   private:
    void Run(unsigned gen, HANDLE stopEvent, HANDLE doneEvent, LogFn log);
    void Set(const GateStatus& s, bool open, unsigned gen);

    GateConfig cfg_;
    bool skipStartupDelay_ = false;
    LogFn log_;
    std::function<void()> onChange_;
    HANDLE stopEvent_ = nullptr;
    HANDLE openEvent_ = nullptr;  // manual reset, signaled = open
    HANDLE doneEvent_ = nullptr;
    std::thread thread_;
    // Bumped by Start/Stop; a gate thread that was left behind (Stop
    // timeout) can then never touch the state of a newer one.
    std::atomic<unsigned> gen_{0};
    mutable std::mutex mutex_;
    GateStatus status_;
};

}  // namespace bfs
