// GNU GPL v3. See host.h for the design.
#include "host.h"

#include <tlhelp32.h>

#include <iterator>
#include <unordered_set>
#include <vector>

namespace bfs {

namespace {

constexpr UINT kMsgPause = WM_APP + 1;
constexpr UINT kMsgReload = WM_APP + 2;
constexpr UINT kMsgRescan = WM_APP + 3;
constexpr ULONGLONG kWaitRecheckMs = 1000;

thread_local Host* t_host;

bool GetFileTime(const std::wstring& path, FILETIME* ft) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        *ft = {};
        return false;
    }
    *ft = data.ftLastWriteTime;
    return true;
}

bool IsInteractiveTopLevel(HWND hwnd) {
    return hwnd && GetAncestor(hwnd, GA_ROOT) == hwnd && IsWindowVisible(hwnd);
}

BOOL CALLBACK CollectWindowPids(HWND hwnd, LPARAM param) {
    if (IsInteractiveTopLevel(hwnd)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid) ((std::unordered_set<DWORD>*)param)->insert(pid);
    }
    return TRUE;
}

}  // namespace

Host::~Host() { Stop(); }

std::wstring Host::IniPath() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return iniPath_.empty() ? ResolveIniPath(dllDir_) : iniPath_;
}

void Host::Log(const std::wstring& s) {
    if (opts_.log) opts_.log(s);
}

void Host::SetState(HostState s) {
    state_ = s;
    if (opts_.onState) opts_.onState(s);
}

bool Host::Start(HostOptions options, std::wstring* error) {
    if (hookThread_.joinable()) return true;
    opts_ = std::move(options);
    dllDir_ = DirName(opts_.dllPath);
    if (!injector_.Init(opts_.dllPath, error)) {
        SetState(HostState::Error);
        return false;
    }
    if (!injector_.Warning().empty()) Log(injector_.Warning());
    injector_.CleanupShadows(opts_.log);
    Log(L"injecting " + injector_.DllPath() +
        (injector_.Version().empty() ? L"" : L" (v" + injector_.Version() + L")"));
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    readyEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    hookDone_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    workerDone_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    workerStop_ = false;
    worker_ = std::thread(&Host::WorkerThread, this);
    hookThread_ = std::thread(&Host::HookThread, this);
    WaitForSingleObject(readyEvent_, INFINITE);
    return true;
}

bool Host::Stop(DWORD timeoutMs, bool disableEverywhere) {
    if (!hookThread_.joinable()) return true;
    forceDisable_ = disableEverywhere;
    SetEvent(stopEvent_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        workerStop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    // Bounded: the worker may be waiting (at most a few seconds) on a remote
    // thread in a hung process, the hook thread may be disabling hooks.
    HANDLE done[2] = {hookDone_, workerDone_};
    DWORD r = WaitForMultipleObjects(2, done, TRUE, timeoutMs);
    if (r != WAIT_OBJECT_0) {
        Log(L"stop timed out, leaving threads behind");
        hookThread_.detach();
        worker_.detach();
        return false;
    }
    hookThread_.join();
    worker_.join();
    CloseHandle(stopEvent_);
    CloseHandle(readyEvent_);
    CloseHandle(hookDone_);
    CloseHandle(workerDone_);
    stopEvent_ = readyEvent_ = hookDone_ = workerDone_ = nullptr;
    SetState(HostState::Stopped);
    return true;
}

void Host::SetPaused(bool paused) {
    paused_ = paused;
    if (hookThreadId_) PostThreadMessageW(hookThreadId_, kMsgPause, paused, 0);
}

void Host::Reload() {
    if (hookThreadId_) PostThreadMessageW(hookThreadId_, kMsgReload, 0, 0);
}

void Host::Rescan() {
    if (hookThreadId_) PostThreadMessageW(hookThreadId_, kMsgRescan, 0, 0);
}

void CALLBACK Host::WinEventProc(HWINEVENTHOOK, DWORD, HWND hwnd,
                                 LONG idObject, LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || !t_host) return;
    t_host->OnWindow(hwnd);
}

void Host::OnWindow(HWND hwnd) {
    if (!active_ || !IsInteractiveTopLevel(hwnd)) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid) Enqueue(pid, true);
}

void Host::Enqueue(DWORD pid, bool hasWindow) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(pid);
    if (it != entries_.end()) {
        Entry& e = it->second;
        bool dead = e.process && WaitForSingleObject(e.process, 0) == WAIT_OBJECT_0;
        if (!dead) {
            if (e.queued || e.verdict != Verdict::Wait || !hasWindow) return;
            if (GetTickCount64() - e.lastCheck < kWaitRecheckMs) return;
            e.queued = true;
            jobs_.push_back({pid, hasWindow});
            cv_.notify_one();
            return;
        }
        if (e.process) CloseHandle(e.process);
        entries_.erase(it);
    }
    Entry e;
    e.process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    e.queued = true;
    entries_[pid] = e;
    jobs_.push_back({pid, hasWindow});
    cv_.notify_one();
}

void Host::ClearEntries() {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_.clear();
    for (auto& [pid, e] : entries_) {
        if (e.process) CloseHandle(e.process);
    }
    entries_.clear();
}

std::wstring Host::StatusText() const {
    switch (state_.load()) {
        case HostState::Stopped:
            return L"已停止";
        case HostState::Standby:
            return L"待命（Everything 插件或另一个实例正在工作）";
        case HostState::Paused:
            return L"已暂停（所有程序里已停用）";
        case HostState::Error:
            return L"出错（找不到或无法加载 BetterFileSizes.dll）";
        case HostState::SafetyValve:
            return L"已被安全开关禁用（BetterFileSizes.disable）";
        case HostState::Running:
            break;
    }
    GateStatus g = gate_.Status();
    std::wstring t = g.Text();
    if (g.state == GateState::Ready || g.state == GateState::ReadyTimeout ||
        g.state == GateState::Bypassed) {
        t += L"，已注入 " + std::to_wstring(injected_.load()) + L" 个程序";
    } else if (injected_ > 0) {
        t += L"（新的注入已暂停，已注入 " + std::to_wstring(injected_.load()) +
             L" 个程序）";
    }
    return t;
}

// Called with mutex_ held: move explorer.exe jobs to the front of the queue
// so the shell gets folder sizes first when the gate opens.
void Host::PrioritizeExplorer() {
    std::deque<Job> first, rest;
    for (const Job& j : jobs_) {
        bool isExplorer = false;
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                   j.pid)) {
            wchar_t path[MAX_PATH * 2];
            DWORD n = (DWORD)std::size(path);
            if (QueryFullProcessImageNameW(h, 0, path, &n)) {
                isExplorer = _wcsicmp(BaseName(path).c_str(), L"explorer.exe") == 0;
            }
            CloseHandle(h);
        }
        (isExplorer ? first : rest).push_back(j);
    }
    first.insert(first.end(), rest.begin(), rest.end());
    jobs_.swap(first);
}

void Host::WorkerThread() {
    struct Done {
        HANDLE h;
        ~Done() { SetEvent(h); }
    } done{workerDone_};
    bool wasOpen = false;
    for (;;) {
        Job job;
        FilterConfig cfg;
        unsigned gen;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [&] { return workerStop_ || !jobs_.empty(); });
            if (workerStop_) return;
        }
        // Readiness gate: while closed, jobs just pile up in the queue (they
        // are de-duplicated per pid). The wait is on the worker thread only.
        if (!gate_.IsOpen()) {
            wasOpen = false;
            if (!gate_.WaitOpen(stopEvent_)) return;  // stopping
        }
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (workerStop_) return;
            if (jobs_.empty()) continue;
            if (!wasOpen) {
                wasOpen = true;
                PrioritizeExplorer();
            }
            job = jobs_.front();
            jobs_.pop_front();
            cfg = cfg_;
            gen = generation_;
        }
        if (!active_ || WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) {
            continue;
        }

        bool self = job.pid == GetCurrentProcessId();
        Evaluation ev = EvaluateProcess(cfg, job.pid, job.hasWindow,
                                        self && opts_.loadIntoSelf);
        Verdict final = ev.verdict;
        bool throttle = false;
        std::wstring name = ev.path.empty() ? std::wstring(L"pid")
                                            : BaseName(ev.path);
        std::wstring tag = name + L" (" + std::to_wstring(job.pid) + L")";

        if (ev.verdict == Verdict::Inject) {
            // Deactivate() bumps the generation and then disables; make sure
            // we never inject after it started.
            std::lock_guard<std::mutex> injectLock(injectMutex_);
            if (!active_ || gen != generation_) continue;
            std::wstring err;
            switch (injector_.Inject(job.pid, &err, opts_.log)) {
                case Injector::Result::Loaded:
                    Log(L"loaded: " + tag + L" [" + ev.reason + L"]");
                    injected_++;
                    throttle = true;
                    break;
                case Injector::Result::Enabled:
                    Log(L"enabled: " + tag + L" [" + ev.reason + L"]");
                    injected_++;
                    throttle = true;
                    break;
                case Injector::Result::AlreadyActive:
                    break;
                case Injector::Result::Failed:
                    Log(L"load failed: " + tag + L": " + err);
                    final = Verdict::Skip;
                    break;
            }
        } else if (ev.verdict == Verdict::Skip && job.hasWindow) {
            Log(L"skipped: " + tag + L": " + ev.reason);
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (gen != generation_) continue;
            auto it = entries_.find(job.pid);
            if (it != entries_.end()) {
                it->second.verdict = final;
                it->second.queued = false;
                it->second.lastCheck = GetTickCount64();
            }
        }
        if (throttle) {
            if (opts_.onState) opts_.onState(state_);  // count changed
            // Gradual: one process per injectIntervalMs.
            DWORD gap = injectIntervalMs_;
            if (gap && WaitForSingleObject(stopEvent_, gap) == WAIT_OBJECT_0) {
                return;
            }
        }
    }
}

void Host::Sweep() {
    if (!active_) return;
    FilterConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = cfg_;
    }

    // Live processes; purge entries of processes that are gone.
    std::unordered_set<DWORD> live;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = {sizeof(pe)};
        if (Process32FirstW(snap, &pe)) {
            do {
                live.insert(pe.th32ProcessID);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.begin(); it != entries_.end();) {
            Entry& e = it->second;
            bool dead = !live.count(it->first) ||
                        (e.process &&
                         WaitForSingleObject(e.process, 0) == WAIT_OBJECT_0);
            if (dead && !e.queued) {
                if (e.process) CloseHandle(e.process);
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Processes with a visible top-level window.
    std::unordered_set<DWORD> windowed;
    EnumWindows(CollectWindowPids, (LPARAM)&windowed);

    for (DWORD pid : windowed) Enqueue(pid, true);
    // Windowless processes only matter outside smart mode, or when the user
    // forces some of them with include=.
    if (cfg.mode != Mode::Smart || !cfg.include.empty()) {
        for (DWORD pid : live) {
            if (!windowed.count(pid)) Enqueue(pid, false);
        }
    }
}

bool Host::Activate() {
    std::wstring ini = ResolveIniPath(dllDir_);
    FilterConfig cfg = FilterConfig::Load(ini);
    if (opts_.forceAll && cfg.mode != Mode::All) {
        cfg.mode = Mode::All;
        if (!GetPrivateProfileIntW(L"Process", L"scanInterval", 0, ini.c_str())) {
            cfg.scanIntervalMs = 3000;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        iniPath_ = ini;
        cfg_ = cfg;
    }
    GetFileTime(ini, &iniTime_);
    ClearEntries();
    disableOnStop_ = opts_.disableOnStop ||
                     GetPrivateProfileIntW(L"Host", L"disableOnExit", 0,
                                           ini.c_str()) != 0;

    std::wstring why;
    if (SafetyValveActive(dllDir_, &why)) {
        Log(L"safety valve active (" + why + L"), not injecting anything");
        SetState(HostState::SafetyValve);
        return false;
    }

    hooks_[0] = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr,
                                WinEventProc, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    hooks_[1] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND,
                                EVENT_SYSTEM_FOREGROUND, nullptr, WinEventProc,
                                0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hooks_[0] || !hooks_[1]) {
        Log(L"SetWinEventHook failed, relying on periodic rescans only");
    }

    GateConfig gc = GateConfig::Load(ini);
    injectIntervalMs_ = gc.injectIntervalMs;
    injected_ = 0;
    gate_.Start(gc, opts_.skipGate, gateStartedOnce_, opts_.log, [this] {
        if (opts_.onState) opts_.onState(state_);
    });
    gateStartedOnce_ = true;

    active_ = true;
    SetState(HostState::Running);
    Log(std::wstring(L"watching (") + kArch + L", mode=" +
        FilterConfig::ModeName(cfg.mode) + L", rescan every " +
        std::to_wstring(cfg.scanIntervalMs / 1000) + L"s, ini=" + ini + L")");

    if (opts_.loadIntoSelf) Enqueue(GetCurrentProcessId(), true);
    Sweep();
    scanTimer_ = SetTimer(nullptr, 0, cfg.scanIntervalMs, nullptr);
    return true;
}

void Host::Deactivate(bool unload) {
    active_ = false;
    gate_.Stop(3000);
    for (auto& h : hooks_) {
        if (h) UnhookWinEvent(h);
        h = nullptr;
    }
    if (scanTimer_) KillTimer(nullptr, scanTimer_);
    scanTimer_ = 0;
    generation_++;
    {
        // Wait for an injection that may be in flight.
        std::lock_guard<std::mutex> injectLock(injectMutex_);
    }
    ClearEntries();
    if (unload) {
        size_t n = injector_.DisableAll(opts_.log);
        Log(L"hooks disabled in " + std::to_wstring(n) + L" process(es)");
    }
}

void Host::HookThread() {
    struct Done {
        HANDLE h;
        ~Done() { SetEvent(h); }
    } done{hookDone_};
    t_host = this;
    hookThreadId_ = GetCurrentThreadId();
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // make queue
    SetEvent(readyEvent_);

    // One active host per bitness and session (daemon, Everything plugin and
    // `bfs-injector watch` share this mutex).
    std::wstring mutexName =
        std::wstring(L"Local\\BetterFileSizes.Host.") + kArch;
    HANDLE hostMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    bool owned = false;
    bool quit = false;

    if (hostMutex) {
        DWORD r = WaitForSingleObject(hostMutex, 0);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {
            owned = true;
        } else {
            SetState(HostState::Standby);
            Log(L"another host is active, standing by");
        }
        while (!owned && !quit) {
            HANDLE hs[2] = {stopEvent_, hostMutex};
            r = MsgWaitForMultipleObjects(2, hs, FALSE, INFINITE, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0) {
                quit = true;
            } else if (r == WAIT_OBJECT_0 + 1 || r == WAIT_ABANDONED_0 + 1) {
                owned = true;
            } else {
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    if (msg.message == WM_QUIT) quit = true;
                }
            }
        }
    } else {
        owned = true;  // could not create the mutex; just run
    }

    if (!quit) {
        if (paused_) {
            SetState(HostState::Paused);
        } else {
            Activate();
        }
    }

    // Watch the INI (next to the DLL and in %LOCALAPPDATA%) for edits.
    std::vector<HANDLE> notifies;
    if (!quit) {
        std::wstring dirs[2] = {dllDir_, UserDataDir()};
        for (auto& d : dirs) {
            if (d.empty()) continue;
            HANDLE h = FindFirstChangeNotificationW(
                d.c_str(), FALSE,
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
            if (h != INVALID_HANDLE_VALUE) notifies.push_back(h);
        }
    }

    bool lastValve = SafetyValveActive(dllDir_);
    auto reload = [&] {
        Log(L"reloading settings");
        Deactivate(true);
        if (!paused_) Activate();
    };

    while (!quit) {
        std::vector<HANDLE> hs = {stopEvent_};
        hs.insert(hs.end(), notifies.begin(), notifies.end());
        DWORD r = MsgWaitForMultipleObjects((DWORD)hs.size(), hs.data(), FALSE,
                                            INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        if (r > WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + hs.size()) {
            FindNextChangeNotification(hs[r - WAIT_OBJECT_0]);
            std::wstring ini = ResolveIniPath(dllDir_);
            FILETIME ft;
            GetFileTime(ini, &ft);
            bool valve = SafetyValveActive(dllDir_);
            bool changed = ini != IniPath() ||
                           CompareFileTime(&ft, &iniTime_) != 0 ||
                           valve != lastValve;
            lastValve = valve;
            if (changed && !paused_) {
                // Editors save in several steps; debounce.
                if (reloadTimer_) KillTimer(nullptr, reloadTimer_);
                reloadTimer_ = SetTimer(nullptr, 0, 1000, nullptr);
            }
            continue;
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = true;
                break;
            }
            if (msg.hwnd == nullptr) {
                if (msg.message == WM_TIMER) {
                    if (msg.wParam == scanTimer_) {
                        Sweep();
                    } else if (msg.wParam == reloadTimer_) {
                        KillTimer(nullptr, reloadTimer_);
                        reloadTimer_ = 0;
                        reload();
                    }
                    continue;
                }
                if (msg.message == kMsgPause) {
                    if (msg.wParam && active_) {
                        Log(L"paused");
                        Deactivate(true);
                        SetState(HostState::Paused);
                    } else if (!msg.wParam && !active_) {
                        Log(L"resumed");
                        Activate();
                    }
                    continue;
                }
                if (msg.message == kMsgReload) {
                    reload();
                    continue;
                }
                if (msg.message == kMsgRescan) {
                    Sweep();
                    continue;
                }
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    for (HANDLE h : notifies) FindCloseChangeNotification(h);
    if (reloadTimer_) KillTimer(nullptr, reloadTimer_);
    reloadTimer_ = 0;
    if (owned) {
        Deactivate(disableOnStop_ || forceDisable_);
        if (hostMutex) ReleaseMutex(hostMutex);
    }
    if (hostMutex) CloseHandle(hostMutex);
    hookThreadId_ = 0;
    t_host = nullptr;
}

}  // namespace bfs
