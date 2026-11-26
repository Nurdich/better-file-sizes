// GNU GPL v3. See readiness.h.
#include "readiness.h"

#include <pdh.h>
#include <pdhmsg.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

// The reduced Everything SDK3 client + Everything_IPC.h definitions, copied
// verbatim from the original mod (src/mod.cpp is never modified).
namespace bfs_es {
#include "everything_sdk.inc"
}  // namespace bfs_es

// Everything IPC status queries, from the official SDK
// (https://www.voidtools.com/Everything-SDK.zip, ipc/everything_ipc.h).
#ifndef EVERYTHING_WM_IPC
#define EVERYTHING_WM_IPC (WM_USER)
#endif
#define BFS_EVERYTHING_IPC_GET_MAJOR_VERSION 0
#define BFS_EVERYTHING_IPC_IS_DB_LOADED 401
#define BFS_EVERYTHING_IPC_IS_DB_BUSY 402
#define BFS_EVERYTHING_IPC_IS_FILE_INFO_INDEXED 411
#define BFS_EVERYTHING_IPC_FILE_INFO_FOLDER_SIZE 2

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace bfs {

namespace {

constexpr DWORD kIpcTimeoutMs = 700;

unsigned IniUInt(const std::wstring& ini, PCWSTR key, unsigned def) {
    return GetPrivateProfileIntW(L"Host", key, (INT)def, ini.c_str());
}

double IniDouble(const std::wstring& ini, PCWSTR key, double def) {
    std::wstring s = Trim(IniString(ini, L"Host", key, L""));
    if (s.empty()) return def;
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    return (end && end != s.c_str() && v >= 0) ? v : def;
}

std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 511, fmt, ap);
    va_end(ap);
    buf[511] = 0;
    return buf;
}

// Returns true and sets *result if the message was delivered.
bool IpcQuery(HWND hwnd, WPARAM command, LPARAM param, LRESULT* result,
              bool* denied) {
    DWORD_PTR r = 0;
    SetLastError(0);
    if (!SendMessageTimeoutW(hwnd, EVERYTHING_WM_IPC, command, param,
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, kIpcTimeoutMs, &r)) {
        if (denied && GetLastError() == ERROR_ACCESS_DENIED) *denied = true;
        return false;
    }
    *result = (LRESULT)r;
    return true;
}

ULONGLONG FileTimeToU64(const FILETIME& ft) {
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

// ---- IPC v2 folder-size query (Everything 1.4), same request as the mod ----

struct Ipc2Reply {
    DWORD id = 0;
    bool done = false;
    bool found = false;
};
thread_local Ipc2Reply* t_reply;

LRESULT CALLBACK ReplyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COPYDATA && t_reply) {
        auto* cds = (COPYDATASTRUCT*)lp;
        if (cds && cds->dwData == t_reply->id &&
            cds->cbData >= sizeof(bfs_es::EVERYTHING_IPC_LIST2)) {
            auto* list = (bfs_es::EVERYTHING_IPC_LIST2*)cds->lpData;
            t_reply->found = list->numitems != 0;
            t_reply->done = true;
        }
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int Ipc2FolderSizeQuery(HWND everything, const std::wstring& folder,
                        DWORD timeoutMs) {
    static const wchar_t kClass[] = L"BetterFileSizes.GateReply";
    static std::once_flag once;
    std::call_once(once, [] {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = ReplyWndProc;
        wc.hInstance = (HINSTANCE)&__ImageBase;
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
    });
    HWND wnd = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                               nullptr, (HINSTANCE)&__ImageBase, nullptr);
    if (!wnd) return -1;

    static std::atomic<DWORD> counter{0x42460000};
    Ipc2Reply reply;
    reply.id = ++counter;
    t_reply = &reply;

    std::wstring search = L"folder:wfn:\"" + folder + L"\"";
    DWORD size = (DWORD)(sizeof(bfs_es::EVERYTHING_IPC_QUERY2) +
                         (search.size() + 1) * sizeof(wchar_t));
    std::vector<BYTE> buf(size, 0);
    auto* q = (bfs_es::EVERYTHING_IPC_QUERY2*)buf.data();
    q->reply_hwnd = (DWORD)(DWORD_PTR)wnd;
    q->reply_copydata_message = reply.id;
    q->search_flags = EVERYTHING_IPC_MATCHCASE | EVERYTHING_IPC_MATCHDIACRITICS;
    q->offset = 0;
    q->max_results = 1;
    q->request_flags = EVERYTHING_IPC_QUERY2_REQUEST_SIZE;
    q->sort_type = EVERYTHING_IPC_SORT_NAME_ASCENDING;
    memcpy(q + 1, search.c_str(), (search.size() + 1) * sizeof(wchar_t));
    COPYDATASTRUCT cds = {EVERYTHING_IPC_COPYDATA_QUERY2W, size, q};

    ULONGLONG start = GetTickCount64();
    int result = -1;
    DWORD_PTR r = 0;
    // SMTO_NORMAL: our thread keeps handling incoming sent messages (the
    // reply may arrive while we wait).
    if (SendMessageTimeoutW(everything, WM_COPYDATA, (WPARAM)wnd, (LPARAM)&cds,
                            SMTO_NORMAL | SMTO_ABORTIFHUNG, timeoutMs, &r) &&
        r) {
        for (;;) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (reply.done) break;
            ULONGLONG el = GetTickCount64() - start;
            if (el >= timeoutMs) break;
            MsgWaitForMultipleObjects(0, nullptr, FALSE,
                                      (DWORD)(timeoutMs - el), QS_ALLINPUT);
        }
        if (reply.done && reply.found) result = (int)(GetTickCount64() - start);
    }
    t_reply = nullptr;
    DestroyWindow(wnd);
    return result;
}

// ---- SDK3 pipe folder-size query (Everything 1.5), same calls as the mod ---

struct Sdk3Job {
    std::wstring instance;
    std::wstring folder;
    std::mutex lock;  // guards client's lifetime against ShutdownClient
    bfs_es::EVERYTHING3_CLIENT* client = nullptr;
    std::atomic<int> state{0};  // 0 running, 1 ok, 2 failed, 3 no pipe
    std::atomic<int> refs{2};
    void Release() {
        if (refs.fetch_sub(1) == 1) delete this;
    }
};

DWORD WINAPI Sdk3Thread(void* p) {
    auto* job = (Sdk3Job*)p;
    bfs_es::EVERYTHING3_CLIENT* c = bfs_es::Everything3_ConnectW(
        job->instance.empty() ? nullptr : job->instance.c_str());
    if (!c) {
        job->state = 3;
    } else {
        {
            std::lock_guard<std::mutex> g(job->lock);
            job->client = c;
        }
        EVERYTHING3_UINT64 size =
            bfs_es::Everything3_GetFolderSizeFromFilenameW(c,
                                                           job->folder.c_str());
        job->state = size == EVERYTHING3_UINT64_MAX ? 2 : 1;
        {
            std::lock_guard<std::mutex> g(job->lock);
            job->client = nullptr;
        }
        bfs_es::Everything3_DestroyClient(c);
    }
    job->Release();
    return 0;
}

// -1 failed/timeout, -2 no pipe (try IPC v2), else elapsed ms.
int Sdk3FolderSizeQuery(const std::wstring& instance,
                        const std::wstring& folder, DWORD timeoutMs) {
    auto* job = new Sdk3Job;
    job->instance = instance;
    job->folder = folder;
    ULONGLONG start = GetTickCount64();
    HANDLE th = CreateThread(nullptr, 64 * 1024, Sdk3Thread, job, 0, nullptr);
    if (!th) {
        delete job;
        return -1;
    }
    DWORD w = WaitForSingleObject(th, timeoutMs);
    int elapsed = (int)(GetTickCount64() - start);
    int result;
    if (w == WAIT_OBJECT_0) {
        int st = job->state;
        result = st == 1 ? elapsed : st == 3 ? -2 : -1;
    } else {
        // Too slow: cancel the pending pipe I/O (documented SDK3 way), give
        // the thread a moment, then leave it (it owns its own job object).
        {
            std::lock_guard<std::mutex> g(job->lock);
            if (job->client) bfs_es::Everything3_ShutdownClient(job->client);
        }
        WaitForSingleObject(th, 2000);
        result = -1;
    }
    CloseHandle(th);
    job->Release();
    return result;
}

const wchar_t* StateName(GateState s) {
    switch (s) {
        case GateState::Off: return L"off";
        case GateState::Starting: return L"startup-delay";
        case GateState::WaitEverything: return L"wait-everything";
        case GateState::WaitDbLoaded: return L"wait-db-loaded";
        case GateState::WaitDbBusy: return L"wait-db-busy";
        case GateState::WaitIdle: return L"wait-idle";
        case GateState::WaitTestQuery: return L"wait-test-query";
        case GateState::Ready: return L"ready";
        case GateState::ReadyTimeout: return L"ready-timeout";
        case GateState::Bypassed: return L"bypassed";
    }
    return L"?";
}

}  // namespace

// ------------------------------------------------------------------ config

GateConfig GateConfig::Load(const std::wstring& ini) {
    GateConfig c;
    std::wstring calc = Trim(IniString(ini, L"Settings", L"calculateFolderSizes",
                                       L"everything"));
    c.everythingMode = _wcsicmp(calc.c_str(), L"everything") == 0;
    c.waitForEverything = IniUInt(ini, L"waitForEverything", 1) != 0;
    c.startupDelaySec = IniUInt(ini, L"startupDelay", 5);
    c.idleSeconds = IniUInt(ini, L"idleSeconds", 10);
    c.maxDiskBusyPercent = IniUInt(ini, L"maxDiskBusyPercent", 30);
    if (c.maxDiskBusyPercent > 100) c.maxDiskBusyPercent = 100;
    c.maxEverythingIoMBps = IniDouble(ini, L"maxEverythingIoMBps", 2.0);
    c.maxEverythingCpuPercent = IniUInt(ini, L"maxEverythingCpuPercent", 25);
    c.readyTimeoutSec = IniUInt(ini, L"readyTimeout", 0);
    c.injectIntervalMs = IniUInt(ini, L"injectIntervalMs", 200);
    if (c.injectIntervalMs > 10000) c.injectIntervalMs = 10000;
    return c;
}

// ------------------------------------------------------------------ status

std::wstring GateStatus::Text() const {
    std::wstring t;
    switch (state) {
        case GateState::Off:
            t = L"未启动";
            break;
        case GateState::Starting:
            t = Fmt(L"启动延迟中（还剩 %u 秒）", secondsLeft);
            break;
        case GateState::WaitEverything:
            t = L"等待 Everything 启动…";
            break;
        case GateState::WaitDbLoaded:
            t = L"等待 Everything 数据库加载…";
            break;
        case GateState::WaitDbBusy:
            t = L"等待 Everything 完成索引…";
            break;
        case GateState::WaitIdle: {
            t = Fmt(L"等待磁盘空闲…（%u/%u 秒", idleSoFar, idleNeeded);
            if (diskBusyPercent >= 0) t += Fmt(L"，磁盘忙 %d%%", diskBusyPercent);
            if (everythingIoMBps >= 0)
                t += Fmt(L"，Everything %.1f MB/s", everythingIoMBps);
            t += L"）";
            break;
        }
        case GateState::WaitTestQuery:
            t = L"等待 Everything 响应文件夹大小查询…";
            break;
        case GateState::Ready:
            t = L"已就绪";
            break;
        case GateState::ReadyTimeout:
            t = L"等待超时，已开始注入";
            break;
        case GateState::Bypassed:
            t = L"已跳过就绪检查（--now）";
            break;
    }
    if (ipcDenied) t += L"［Everything 以管理员身份运行，无法查询其数据库状态］";
    if (folderSizeIndexed == 0)
        t += L"［Everything 未开启“索引文件夹大小”］";
    return t;
}

// ------------------------------------------------------------------ probe

EverythingProbe ProbeEverything() {
    EverythingProbe p;
    // Same window lookup order as the mod: the 1.5a instance first.
    HWND hwnd = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION_(1.5a)", nullptr);
    if (hwnd) {
        p.instance = L"1.5a";
    } else {
        hwnd = FindWindowW(L"EVERYTHING_TASKBAR_NOTIFICATION", nullptr);
    }
    if (!hwnd) return p;
    p.running = true;
    GetWindowThreadProcessId(hwnd, &p.pid);

    LRESULT r = 0;
    bool denied = false;
    if (!IpcQuery(hwnd, BFS_EVERYTHING_IPC_IS_DB_LOADED, 0, &r, &denied)) {
        p.ipcDenied = denied;
        return p;
    }
    p.ipcOk = true;
    p.dbLoaded = r != 0;
    if (IpcQuery(hwnd, BFS_EVERYTHING_IPC_IS_DB_BUSY, 0, &r, nullptr)) {
        p.dbBusy = r != 0;
    } else {
        p.dbBusy = true;  // not answering within 1 s: treat as busy
    }
    if (p.dbLoaded &&
        IpcQuery(hwnd, BFS_EVERYTHING_IPC_IS_FILE_INFO_INDEXED,
                 BFS_EVERYTHING_IPC_FILE_INFO_FOLDER_SIZE, &r, nullptr)) {
        p.folderSizeIndexed = r != 0 ? 1 : 0;
    }
    return p;
}

int TestFolderSizeQuery(const std::wstring& folder, DWORD timeoutMs,
                        const std::wstring& instance) {
    // Like the mod: SDK3 pipe (unnamed instance, then "1.5a"), else IPC v2.
    int r = Sdk3FolderSizeQuery(L"", folder, timeoutMs);
    if (r == -2) r = Sdk3FolderSizeQuery(L"1.5a", folder, timeoutMs);
    if (r != -2) return r;
    HWND hwnd = FindWindowW(instance == L"1.5a"
                                ? L"EVERYTHING_TASKBAR_NOTIFICATION_(1.5a)"
                                : L"EVERYTHING_TASKBAR_NOTIFICATION",
                            nullptr);
    if (!hwnd) return -1;
    return Ipc2FolderSizeQuery(hwnd, folder, timeoutMs);
}

// ------------------------------------------------------------------ disk

DiskMonitor::DiskMonitor() {
    PDH_HQUERY q = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &q) != ERROR_SUCCESS) return;
    PDH_HCOUNTER c = nullptr;
    // English name so it also works on Chinese / other localized Windows.
    if (PdhAddEnglishCounterW(q, L"\\PhysicalDisk(_Total)\\% Idle Time", 0,
                              &c) != ERROR_SUCCESS) {
        PdhCloseQuery(q);
        return;
    }
    PdhCollectQueryData(q);  // first sample (rate counters need two)
    query_ = q;
    counter_ = c;
}

DiskMonitor::~DiskMonitor() {
    if (query_) PdhCloseQuery((PDH_HQUERY)query_);
}

int DiskMonitor::SampleBusyPercent() {
    if (!query_) return -1;
    if (PdhCollectQueryData((PDH_HQUERY)query_) != ERROR_SUCCESS) return -1;
    PDH_FMT_COUNTERVALUE v;
    DWORD type = 0;
    if (PdhGetFormattedCounterValue((PDH_HCOUNTER)counter_,
                                    PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &type,
                                    &v) != ERROR_SUCCESS ||
        (v.CStatus != PDH_CSTATUS_VALID_DATA &&
         v.CStatus != PDH_CSTATUS_NEW_DATA)) {
        return -1;
    }
    double idle = v.doubleValue;
    if (idle > 100) idle = 100;
    if (idle < 0) idle = 0;
    return (int)(100.0 - idle + 0.5);
}

// ------------------------------------------------------------------ gate

ReadinessGate::ReadinessGate()
    : openEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

ReadinessGate::~ReadinessGate() {
    if (Stop() && openEvent_) CloseHandle(openEvent_);
}

void ReadinessGate::Start(const GateConfig& cfg, bool bypass,
                          bool skipStartupDelay, LogFn log,
                          std::function<void()> onChange) {
    Stop();
    cfg_ = cfg;
    skipStartupDelay_ = skipStartupDelay;
    unsigned gen;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        log_ = std::move(log);
        onChange_ = std::move(onChange);
        gen = ++gen_;
        ResetEvent(openEvent_);
    }
    if (bypass) {
        GateStatus s;
        s.state = GateState::Bypassed;
        Set(s, true, gen);
        return;
    }
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    doneEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread(&ReadinessGate::Run, this, gen, stopEvent_, doneEvent_,
                          log_);
}

bool ReadinessGate::Stop(DWORD timeoutMs) {
    bool ok = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++gen_;
        ResetEvent(openEvent_);
    }
    if (thread_.joinable()) {
        SetEvent(stopEvent_);
        if (WaitForSingleObject(doneEvent_, timeoutMs) == WAIT_OBJECT_0) {
            thread_.join();
            CloseHandle(stopEvent_);
            CloseHandle(doneEvent_);
        } else {
            // Should not happen (every wait in Run is short); never hang.
            // The thread keeps its own event handles (leaked on purpose) and
            // its generation no longer matches, so it cannot open the gate.
            thread_.detach();
            ok = false;
        }
        stopEvent_ = doneEvent_ = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = GateStatus{};
    return ok;
}

bool ReadinessGate::IsOpen() const {
    return openEvent_ && WaitForSingleObject(openEvent_, 0) == WAIT_OBJECT_0;
}

bool ReadinessGate::WaitOpen(HANDLE abortEvent) const {
    if (!openEvent_) return false;
    HANDLE hs[2] = {abortEvent, openEvent_};
    return WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0 + 1;
}

GateStatus ReadinessGate::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void ReadinessGate::Set(const GateStatus& s, bool open, unsigned gen) {
    bool changed;
    GateState old;
    LogFn log;
    std::function<void()> onChange;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (gen != gen_) return;  // stale thread (Stop timed out earlier)
        old = status_.state;
        changed = s.Text() != status_.Text();
        status_ = s;
        if (open) {
            SetEvent(openEvent_);
        } else {
            ResetEvent(openEvent_);
        }
        log = log_;
        onChange = onChange_;
    }
    if (old != s.state && log) {
        log(std::wstring(L"gate: ") + StateName(s.state) + L" - " + s.Text());
    }
    if (changed && onChange) onChange();
}

void ReadinessGate::Run(unsigned gen, HANDLE stopEvent, HANDLE doneEvent,
                        LogFn log) {
    struct Done {
        HANDLE h;
        ~Done() { SetEvent(h); }
    } done{doneEvent};
    // Own copies: a newer Start() may overwrite the members if this thread
    // was ever left behind.
    const GateConfig cfg = cfg_;
    const bool skipDelay = skipStartupDelay_;

    const bool useEv = cfg.UsesEverything();
    DiskMonitor disk;
    if (log) {
        log(Fmt(L"gate: started (everything=%d, idle %us, disk busy <= %u%%, "
                 L"Everything io <= %.1f MB/s, timeout %us)",
                 useEv ? 1 : 0, cfg.idleSeconds, cfg.maxDiskBusyPercent,
                 cfg.maxEverythingIoMBps, cfg.readyTimeoutSec));
    }

    wchar_t winDir[MAX_PATH] = L"C:\\Windows";
    GetWindowsDirectoryW(winDir, MAX_PATH);

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const double ncpu = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;

    ULONGLONG start = GetTickCount64();
    ULONGLONG timeoutStart = start;  // readyTimeout counts from here
    if (skipDelay) start -= (ULONGLONG)cfg.startupDelaySec * 1000;
    bool open = false;
    bool openedByTimeout = false;  // then Everything is not watched any more
    unsigned idle = 0;
    unsigned bad = 0;
    unsigned queryFails = 0;

    // Everything process counters (I/O bytes, CPU time).
    DWORD evPid = 0;
    HANDLE evProc = nullptr;
    ULONGLONG lastIo = 0, lastCpu = 0, lastTick = 0;

    GateStatus s;
    s.idleNeeded = cfg.idleSeconds;

    for (DWORD wait = 0;; wait = 1000) {
        if (WaitForSingleObject(stopEvent, wait) == WAIT_OBJECT_0) break;
        ULONGLONG now = GetTickCount64();

        int busy = disk.SampleBusyPercent();
        EverythingProbe ev;
        if (useEv) ev = ProbeEverything();

        // Everything I/O + CPU rates.
        double ioMBps = -1;
        int cpu = -1;
        if (useEv && ev.pid != evPid) {
            if (evProc) CloseHandle(evProc);
            evProc = ev.pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                          FALSE, ev.pid)
                            : nullptr;
            evPid = ev.pid;
            lastTick = 0;
        }
        if (evProc) {
            IO_COUNTERS io;
            FILETIME c, e, k, u;
            if (GetProcessIoCounters(evProc, &io) &&
                GetProcessTimes(evProc, &c, &e, &k, &u)) {
                ULONGLONG ioBytes = io.ReadTransferCount + io.WriteTransferCount;
                ULONGLONG cpu100ns = FileTimeToU64(k) + FileTimeToU64(u);
                if (lastTick && now > lastTick) {
                    double sec = (now - lastTick) / 1000.0;
                    ioMBps = (ioBytes - lastIo) / sec / (1024.0 * 1024.0);
                    cpu = (int)((cpu100ns - lastCpu) / 1e7 / sec / ncpu * 100 +
                                0.5);
                }
                lastIo = ioBytes;
                lastCpu = cpu100ns;
                lastTick = now;
            }
        }

        s.diskBusyPercent = busy;
        s.everythingIoMBps = ioMBps;
        s.everythingCpuPercent = cpu;
        s.everythingRunning = ev.running;
        s.ipcDenied = ev.ipcDenied;
        if (ev.folderSizeIndexed >= 0 || !ev.running)
            s.folderSizeIndexed = ev.folderSizeIndexed;

        if (open) {
            // Re-gate if Everything goes away or reloads its database. A few
            // seconds of grace so a short busy moment does not flap.
            if (useEv && !openedByTimeout && (!ev.running ||
                          (ev.ipcOk && (!ev.dbLoaded || ev.dbBusy)))) {
                if (++bad >= 3) {
                    open = false;
                    idle = 0;
                    queryFails = 0;
                    // readyTimeout and the startup delay restart from here.
                    start = now - (ULONGLONG)cfg.startupDelaySec * 1000;
                    timeoutStart = now;
                    s.state = !ev.running         ? GateState::WaitEverything
                              : !ev.dbLoaded      ? GateState::WaitDbLoaded
                                                  : GateState::WaitDbBusy;
                    if (log) log(L"gate: Everything restarted / busy again, "
                                   L"pausing new injections");
                    Set(s, false, gen);
                }
            } else {
                bad = 0;
                Set(s, true, gen);  // refresh numbers / warnings
            }
            continue;
        }

        ULONGLONG elapsedMs = now - start;
        if (cfg.readyTimeoutSec &&
            now - timeoutStart >= (ULONGLONG)cfg.readyTimeoutSec * 1000) {
            s.state = GateState::ReadyTimeout;
            open = true;
            openedByTimeout = true;
            bad = 0;
            Set(s, true, gen);
            continue;
        }

        if (elapsedMs < (ULONGLONG)cfg.startupDelaySec * 1000) {
            s.state = GateState::Starting;
            s.secondsLeft = (unsigned)(((ULONGLONG)cfg.startupDelaySec * 1000 -
                                        elapsedMs + 999) / 1000);
            Set(s, false, gen);
            continue;
        }

        if (useEv) {
            if (!ev.running) {
                idle = 0;
                s.state = GateState::WaitEverything;
                Set(s, false, gen);
                continue;
            }
            if (ev.ipcOk && !ev.dbLoaded) {
                idle = 0;
                s.state = GateState::WaitDbLoaded;
                Set(s, false, gen);
                continue;
            }
            if (ev.ipcOk && ev.dbBusy) {
                idle = 0;
                s.state = GateState::WaitDbBusy;
                Set(s, false, gen);
                continue;
            }
        }

        bool quiet = busy < 0 || busy <= (int)cfg.maxDiskBusyPercent;
        if (useEv && evProc) {  // unknown if we may not open Everything
            if (ioMBps < 0 || ioMBps > cfg.maxEverythingIoMBps) quiet = false;
            if (cpu < 0 || cpu > (int)cfg.maxEverythingCpuPercent) quiet = false;
        }
        idle = quiet ? idle + 1 : 0;
        s.idleSoFar = idle < cfg.idleSeconds ? idle : cfg.idleSeconds;
        if (idle < cfg.idleSeconds) {
            s.state = GateState::WaitIdle;
            Set(s, false, gen);
            continue;
        }

        // Final check: a real folder-size query must answer fast. Skipped if
        // Everything's IPC is blocked (elevated Everything) or folder sizes
        // are not indexed at all (the query could never succeed).
        if (useEv && ev.ipcOk && ev.folderSizeIndexed != 0) {
            if (WaitForSingleObject(stopEvent, 0) == WAIT_OBJECT_0) break;
            int ms = TestFolderSizeQuery(winDir, 500, ev.instance);
            s.lastQueryMs = ms;
            if (ms < 0) {
                s.state = GateState::WaitTestQuery;
                Set(s, false, gen);
                // Keep most of the idle window; retry in ~2 s.
                idle = cfg.idleSeconds > 2 ? cfg.idleSeconds - 2 : 0;
                if (++queryFails % 10 == 1 && log)
                    log(L"gate: folder-size test query failed / too slow");
                continue;
            }
            if (log) log(Fmt(L"gate: test query %ls took %d ms", winDir, ms));
        }
        s.state = GateState::Ready;
        open = true;
        bad = 0;
        Set(s, true, gen);
    }
    if (evProc) CloseHandle(evProc);
}

}  // namespace bfs
