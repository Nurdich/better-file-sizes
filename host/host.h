// The "daemon" logic shared by bfs-daemon.exe, the Everything plugin and
// `bfs-injector watch`:
//
//  * Out-of-context WinEvent hooks (EVENT_OBJECT_SHOW / EVENT_SYSTEM_FOREGROUND)
//    tell us immediately when any process in our desktop shows a top-level
//    window. No admin rights, no DLL is put into other processes for this.
//  * A worker thread runs the process filter and injects BetterFileSizes.dll.
//  * A slow safety-net rescan (EnumWindows + process snapshot, default every
//    30 s in smart mode) catches anything that was missed and purges dead
//    processes. In all/whitelist mode, windowless processes can only be found
//    by the rescan, so its default interval is 3 s there.
//  * explorer.exe restarts are caught by the same window events (the new
//    taskbar window), and the INI is watched for changes (auto reload).
//  * Only one host per bitness is active at a time (named mutex). A second
//    host waits in standby and takes over when the first one exits.
//  * Readiness gate (readiness.h): nothing is injected until Everything's
//    database is loaded and idle and the disk is quiet; then processes are
//    injected one at a time (explorer.exe first), [Host] injectIntervalMs
//    apart. If Everything restarts / reloads, new injections pause again.
//  * Stop() leaves the mod running (or disables it, see disableOnStop).
//
// GNU GPL v3.
#pragma once

#include "inject.h"
#include "process_filter.h"
#include "readiness.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace bfs {

enum class HostState { Stopped, Standby, Running, Paused, Error, SafetyValve };

struct HostOptions {
    std::wstring dllPath;     // BetterFileSizes.dll
    bool loadIntoSelf = false;  // Everything plugin: also load into our process
    bool forceAll = false;      // bfs-injector watch --all
    // Turn the hooks off everywhere when the host stops. Default false: like
    // Windhawk, exiting the host leaves the mod running in the processes it
    // is already in; use pause / "bfs-injector unload" to turn it off.
    // [Host] disableOnExit=1 in the INI also enables this.
    bool disableOnStop = false;
    bool skipGate = false;    // bfs-injector --now: no readiness gate
    LogFn log;                                 // optional
    std::function<void(HostState)> onState;    // optional, any thread
};

class Host {
   public:
    Host() = default;
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    bool Start(HostOptions options, std::wstring* error);
    // Stops watching (and, if configured, disables the hooks everywhere).
    // Never blocks longer than timeoutMs. Returns false if the threads did
    // not finish in time; they are then detached and the Host object must be
    // leaked (not destroyed) by the caller.
    bool Stop(DWORD timeoutMs = 15000, bool disableEverywhere = false);
    bool Stopped() const { return !hookThread_.joinable(); }

    void SetPaused(bool paused);  // paused = hooks off everywhere, not watching
    void Reload();                // hooks off everywhere, re-read INI, re-enable
    void Rescan();                // e.g. after TaskbarCreated
    HostState State() const { return state_; }
    bool Paused() const { return paused_; }
    std::wstring IniPath() const;
    // Human readable (Chinese) status incl. the readiness gate, e.g.
    // "等待 Everything 数据库加载…" or "已就绪，已注入 3 个程序".
    std::wstring StatusText() const;
    GateStatus Gate() const { return gate_.Status(); }
    unsigned InjectedCount() const { return injected_; }

   private:
    struct Entry {
        HANDLE process = nullptr;  // SYNCHRONIZE, to detect exit / pid reuse
        Verdict verdict = Verdict::Wait;
        bool queued = false;
        ULONGLONG lastCheck = 0;
    };
    struct Job {
        DWORD pid;
        bool hasWindow;
    };

    void HookThread();
    void WorkerThread();
    bool Activate();
    void Deactivate(bool unload);
    void Sweep();
    void OnWindow(HWND hwnd);
    void Enqueue(DWORD pid, bool hasWindow);
    void ClearEntries();
    void SetState(HostState s);
    void Log(const std::wstring& s);
    void PrioritizeExplorer();
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                      LONG idObject, LONG idChild, DWORD,
                                      DWORD);

    HostOptions opts_;
    Injector injector_;
    std::wstring dllDir_;
    std::atomic<HostState> state_{HostState::Stopped};
    std::atomic<bool> paused_{false};
    std::atomic<bool> active_{false};

    std::thread hookThread_;
    DWORD hookThreadId_ = 0;
    HANDLE stopEvent_ = nullptr;
    HANDLE readyEvent_ = nullptr;
    HWINEVENTHOOK hooks_[2] = {};
    UINT_PTR scanTimer_ = 0;
    UINT_PTR reloadTimer_ = 0;
    bool disableOnStop_ = false;
    std::atomic<bool> forceDisable_{false};
    HANDLE hookDone_ = nullptr;
    HANDLE workerDone_ = nullptr;
    FILETIME iniTime_ = {};

    // Filter config + per-process state (guarded by mutex_).
    mutable std::mutex mutex_;
    std::mutex injectMutex_;
    FilterConfig cfg_;
    std::wstring iniPath_;
    std::unordered_map<DWORD, Entry> entries_;
    std::deque<Job> jobs_;
    std::condition_variable cv_;
    bool workerStop_ = false;
    std::thread worker_;
    std::atomic<unsigned> generation_{0};

    ReadinessGate gate_;
    bool gateStartedOnce_ = false;
    std::atomic<unsigned> injected_{0};
    std::atomic<unsigned> injectIntervalMs_{200};
};

}  // namespace bfs
