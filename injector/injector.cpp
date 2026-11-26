// bfs-injector: command-line tool to load/unload BetterFileSizes.dll.
//
// Usage:
//   bfs-injector load   [--auto | --all | process.exe ...]
//   bfs-injector unload            (= disable: hooks off in every process)
//   bfs-injector reload [--auto | --all | process.exe ...]
//   bfs-injector watch  [--all]
//   bfs-injector status
//   bfs-injector check  process.exe ...
//   add --now to load/reload/watch to skip the readiness gate
//
// The DLL is never really unloaded (it pins itself, so no thread can ever
// return into freed code); "unload" switches its hooks off and "load" on an
// already loaded process switches them back on.
// Without targets, load/reload use explorer.exe. --auto applies the
// [Process] rules from BetterFileSizes.ini (smart mode by default) to all
// running processes; --all forces mode=all. `watch` runs the same daemon
// logic as bfs-daemon.exe / the Everything plugin in this console.
// load/reload/watch first wait for the readiness gate (Everything database
// loaded and idle, disk quiet; see host/readiness.h) unless --now is given.
//
// GNU GPL v3.

#include "../host/host.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

std::wstring g_dir;
std::wstring g_dllPath;
HANDLE g_stopEvent;

// Chinese-safe output: WriteConsoleW on a console, UTF-8 when redirected.
void Print(const std::wstring& s) {
    fflush(stdout);
    std::wstring line = s + L"\n";
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD n;
    if (GetConsoleMode(out, &n)) {
        WriteConsoleW(out, line.c_str(), (DWORD)line.size(), &n, nullptr);
        return;
    }
    int len = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string u(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), u.data(),
                        len, nullptr, nullptr);
    WriteFile(out, u.data(), (DWORD)u.size(), &n, nullptr);
}

BOOL CALLBACK CollectPids(HWND hwnd, LPARAM param) {
    if (GetAncestor(hwnd, GA_ROOT) == hwnd && IsWindowVisible(hwnd)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        ((std::set<DWORD>*)param)->insert(pid);
    }
    return TRUE;
}

std::set<DWORD> WindowedPids() {
    std::set<DWORD> pids;
    EnumWindows(CollectPids, (LPARAM)&pids);
    return pids;
}

template <typename F>
void ForEachProcess(F f) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = {sizeof(pe)};
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID > 4 && pe.th32ProcessID != GetCurrentProcessId()) {
                f(pe.th32ProcessID, pe.szExeFile);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

void InjectOne(bfs::Injector& inj, DWORD pid, const std::wstring& name) {
    std::wstring err;
    switch (inj.Inject(pid, &err)) {
        case bfs::Injector::Result::Loaded:
            wprintf(L"  %ls (%u): loaded\n", name.c_str(), pid);
            break;
        case bfs::Injector::Result::Enabled:
            wprintf(L"  %ls (%u): enabled (was already loaded)\n", name.c_str(), pid);
            break;
        case bfs::Injector::Result::AlreadyActive:
            wprintf(L"  %ls (%u): already loaded (old DLL version?)\n", name.c_str(), pid);
            break;
        case bfs::Injector::Result::Failed:
            wprintf(L"  %ls (%u): failed: %ls\n", name.c_str(), pid, err.c_str());
            break;
    }
}

// Blocks until the readiness gate opens (or Ctrl+C). False = cancelled.
bool WaitForGate() {
    bfs::GateConfig cfg = bfs::GateConfig::Load(bfs::ResolveIniPath(g_dir));
    bfs::ReadinessGate gate;
    Print(L"Waiting for the readiness gate (Ctrl+C to cancel, --now to skip)...");
    std::wstring last;
    // No startup delay for a manual load; Everything / disk checks still run.
    gate.Start(cfg, false, true, nullptr, nullptr);
    for (;;) {
        HANDLE hs[1] = {g_stopEvent};
        if (WaitForMultipleObjects(1, hs, FALSE, 500) == WAIT_OBJECT_0) {
            gate.Stop();
            Print(L"Cancelled.");
            return false;
        }
        std::wstring t = gate.Status().Text();
        if (t != last) {
            Print(L"  " + t);
            last = t;
        }
        if (gate.IsOpen()) break;
    }
    gate.Stop();
    return true;
}

unsigned InjectInterval() {
    return bfs::GateConfig::Load(bfs::ResolveIniPath(g_dir)).injectIntervalMs;
}

void DoLoad(bfs::Injector& inj, bool autoMode, bool all,
            const std::vector<std::wstring>& names) {
    // Collect targets first (explorer.exe first), then inject one at a time.
    std::vector<std::pair<DWORD, std::wstring>> targets;
    if (!autoMode && !all) {
        ForEachProcess([&](DWORD pid, const wchar_t* exe) {
            for (auto& n : names) {
                if (bfs::WildcardMatch(n.c_str(), exe)) {
                    targets.emplace_back(pid, exe);
                    break;
                }
            }
        });
    } else {
        bfs::FilterConfig cfg =
            bfs::FilterConfig::Load(bfs::ResolveIniPath(g_dir));
        if (all) cfg.mode = bfs::Mode::All;
        wprintf(L"mode=%ls\n", bfs::FilterConfig::ModeName(cfg.mode));
        std::set<DWORD> windowed = WindowedPids();
        ForEachProcess([&](DWORD pid, const wchar_t* exe) {
            bfs::Evaluation ev =
                bfs::EvaluateProcess(cfg, pid, windowed.count(pid) != 0);
            if (ev.verdict == bfs::Verdict::Inject) targets.emplace_back(pid, exe);
        });
    }
    std::stable_partition(targets.begin(), targets.end(), [](const auto& t) {
        return _wcsicmp(t.second.c_str(), L"explorer.exe") == 0;
    });
    unsigned gap = InjectInterval();
    for (size_t i = 0; i < targets.size(); i++) {
        if (i && gap && WaitForSingleObject(g_stopEvent, gap) == WAIT_OBJECT_0) {
            Print(L"Cancelled.");
            return;
        }
        InjectOne(inj, targets[i].first, targets[i].second);
    }
}

const wchar_t* YesNo(int v) { return v < 0 ? L"?" : v ? L"yes" : L"no"; }

void DoStatus(bfs::Injector& inj) {
    std::wstring ini = bfs::ResolveIniPath(g_dir);
    bfs::GateConfig cfg = bfs::GateConfig::Load(ini);
    Print(L"ini: " + ini);
    Print(L"shipped: " + inj.ShippedPath());
    Print(L"shadow:  " + inj.DllPath() +
          (inj.Version().empty() ? L"" : L" (v" + inj.Version() + L")"));
    if (!inj.Warning().empty()) Print(L"warning: " + inj.Warning());

    wprintf(L"gate: everything=%ls waitForEverything=%d idleSeconds=%u "
            L"maxDiskBusyPercent=%u maxEverythingIoMBps=%.1f readyTimeout=%u "
            L"injectIntervalMs=%u\n",
            cfg.everythingMode ? L"yes" : L"no", cfg.waitForEverything ? 1 : 0,
            cfg.idleSeconds, cfg.maxDiskBusyPercent, cfg.maxEverythingIoMBps,
            cfg.readyTimeoutSec, cfg.injectIntervalMs);

    bfs::DiskMonitor disk;
    Sleep(1000);
    int busy = disk.SampleBusyPercent();
    if (busy < 0) {
        wprintf(L"disk busy: unknown (PDH counter unavailable)\n");
    } else {
        wprintf(L"disk busy: %d%% (limit %u%%)\n", busy, cfg.maxDiskBusyPercent);
    }

    bfs::EverythingProbe ev = bfs::ProbeEverything();
    wprintf(L"Everything: running=%ls", YesNo(ev.running));
    if (ev.running) {
        wprintf(L" pid=%u instance=%ls ipc=%ls db_loaded=%ls db_busy=%ls "
                L"folder_size_indexed=%ls",
                ev.pid, ev.instance.empty() ? L"(default)" : ev.instance.c_str(),
                ev.ipcOk ? L"ok" : ev.ipcDenied ? L"denied (elevated?)" : L"no reply",
                YesNo(ev.ipcOk ? ev.dbLoaded : -1),
                YesNo(ev.ipcOk ? ev.dbBusy : -1), YesNo(ev.folderSizeIndexed));
    }
    wprintf(L"\n");
    if (ev.running && ev.ipcOk && ev.dbLoaded) {
        wchar_t winDir[MAX_PATH] = L"C:\\Windows";
        GetWindowsDirectoryW(winDir, MAX_PATH);
        int ms = bfs::TestFolderSizeQuery(winDir, 500, ev.instance);
        if (ms < 0) {
            wprintf(L"test query (%ls): failed or > 500 ms\n", winDir);
        } else {
            wprintf(L"test query (%ls): %d ms\n", winDir, ms);
        }
    }

    // One-shot verdict (the live gate also needs idleSeconds of quiet).
    std::wstring verdict;
    if (cfg.UsesEverything()) {
        if (!ev.running) verdict = L"等待 Everything 启动…";
        else if (ev.ipcOk && !ev.dbLoaded) verdict = L"等待 Everything 数据库加载…";
        else if (ev.ipcOk && ev.dbBusy) verdict = L"等待 Everything 完成索引…";
    }
    if (verdict.empty() && busy >= 0 && busy > (int)cfg.maxDiskBusyPercent) {
        verdict = L"等待磁盘空闲…";
    }
    if (verdict.empty()) verdict = L"此刻满足条件（实际注入前还需连续空闲 " +
                                   std::to_wstring(cfg.idleSeconds) + L" 秒）";
    Print(L"gate: " + verdict);

    std::wstring mutexName = std::wstring(L"Local\\BetterFileSizes.Host.") +
                             bfs::kArch;
    if (HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, mutexName.c_str())) {
        CloseHandle(m);
        Print(L"host: a daemon / Everything plugin / watch is running (its "
              L"tray tooltip shows its live gate state)");
    } else {
        Print(L"host: none running");
    }

    Print(L"processes with the DLL:");
    for (auto& [pid, info] : inj.ListLoaded(true)) {
        wprintf(L"  %ls (%u)\n", info.first.c_str(), pid);
        for (auto& c : info.second) {
            const wchar_t* st = c.enabled < 0 ? L"?"
                                 : c.enabled ? L"enabled" : L"disabled";
            wprintf(L"    %ls  %ls%ls\n", st, c.path.c_str(),
                    c.current ? L"  [current]" : L"");
        }
    }
}

void Usage() {
    wprintf(
        L"Usage: bfs-injector <command> [options]\n"
        L"  load   [--auto|--all|name.exe ...]  load the DLL (default: explorer.exe)\n"
        L"  unload                              turn the hooks off in every process\n"
        L"                                      (the DLL stays loaded but inactive)\n"
        L"  reload [--auto|--all|name.exe ...]  unload, then load (after editing the INI)\n"
        L"  watch  [--all]                      run the daemon logic in this console;\n"
        L"                                      Ctrl+C leaves the hooks active\n"
        L"  status                              readiness gate state + processes\n"
        L"                                      that have the DLL\n"
        L"  check  name.exe ...                 explain the filter decision\n"
        L"  --auto  use [Process] mode from BetterFileSizes.ini (default smart)\n"
        L"  --all   every process of the same bitness that can be injected\n"
        L"  --now   skip the readiness gate (Everything DB / disk idle wait)\n");
}

BOOL WINAPI CtrlHandler(DWORD) {
    SetEvent(g_stopEvent);
    // Windows kills a console process ~5 s after the close button; main()
    // stops within 4 s.
    Sleep(4500);
    return TRUE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        Usage();
        return 1;
    }
    std::wstring cmd = argv[1];
    bool all = false, autoMode = false, now = false;
    std::vector<std::wstring> names;
    for (int i = 2; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--all") == 0) all = true;
        else if (_wcsicmp(argv[i], L"--auto") == 0) autoMode = true;
        else if (_wcsicmp(argv[i], L"--now") == 0) now = true;
        else names.push_back(argv[i]);
    }
    if (!all && !autoMode && names.empty()) names.push_back(L"explorer.exe");

    g_dir = bfs::DirName(bfs::ModulePath(nullptr));
    g_dllPath = g_dir + L"\\" + bfs::kDllName;
    bfs::Injector inj;
    std::wstring err;
    if (!inj.Init(g_dllPath, &err)) {
        Print(err);
        return 1;
    }
    auto log = [](const std::wstring& s) { Print(L"  " + s); };
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    if (cmd == L"load" || cmd == L"reload") {
        SetConsoleCtrlHandler(CtrlHandler, TRUE);
        if (cmd == L"reload") {
            inj.DisableAll(log);
            Sleep(300);
        }
        if (!now && !WaitForGate()) return 2;
        DoLoad(inj, autoMode, all, names);
    } else if (cmd == L"unload") {
        inj.DisableAll(log);
    } else if (cmd == L"status") {
        DoStatus(inj);
    } else if (cmd == L"check") {
        bfs::FilterConfig cfg =
            bfs::FilterConfig::Load(bfs::ResolveIniPath(g_dir));
        if (all) cfg.mode = bfs::Mode::All;
        std::set<DWORD> windowed = WindowedPids();
        ForEachProcess([&](DWORD pid, const wchar_t* exe) {
            for (auto& n : names) {
                if (!bfs::WildcardMatch(n.c_str(), exe)) continue;
                bool hasWindow = windowed.count(pid) != 0;
                bfs::Evaluation ev = bfs::EvaluateProcess(cfg, pid, hasWindow);
                PCWSTR v = ev.verdict == bfs::Verdict::Inject ? L"INJECT"
                           : ev.verdict == bfs::Verdict::Wait ? L"WAIT"
                                                              : L"SKIP";
                wprintf(L"  %ls (%u) window=%d: %ls - %ls\n", exe, pid,
                        hasWindow ? 1 : 0, v, ev.reason.c_str());
                break;
            }
        });
    } else if (cmd == L"watch") {
        SetConsoleCtrlHandler(CtrlHandler, TRUE);
        bfs::Host host;
        bfs::HostOptions opts;
        opts.dllPath = g_dllPath;
        opts.log = log;
        opts.forceAll = all;
        opts.skipGate = now;
        if (!host.Start(opts, &err)) {
            Print(err);
            return 1;
        }
        Print(L"Watching. Press Ctrl+C to stop (hooks stay active; run "
              L"'bfs-injector unload' to turn them off).");
        WaitForSingleObject(g_stopEvent, INFINITE);
        Print(L"Stopping...");
        if (!host.Stop(4000)) ExitProcess(0);  // never hang on exit
        return 0;
    } else {
        Usage();
        return 1;
    }
    return 0;
}
