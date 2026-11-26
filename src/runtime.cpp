// Standalone runtime: replaces the Windhawk engine for this mod.
//
// Life cycle (designed so that nothing can hang or crash the host process):
//  * DllMain only records paths and starts an init thread (no heavy work
//    under the loader lock).
//  * The init thread checks the exclude list and the safety valve. If the
//    process is excluded, the DLL frees itself (no hook was ever created).
//    Otherwise the DLL PINS itself (GetModuleHandleEx PIN): from then on it
//    is never unmapped, so a thread that is still inside a hook function or
//    a MinHook trampoline can never return into freed memory.
//  * BfsDisable disables all hooks (one MinHook freeze), then waits a bounded
//    time for hook calls in flight and runs Wh_ModUninit on a helper thread
//    with a bounded wait. Trampolines are never freed.
//  * BfsEnable re-reads the INI and runs Wh_ModInit again; hooks that already
//    exist are simply re-enabled.
//  * BfsUnload is kept as an alias of BfsDisable for older hosts.
//  * Wh_Log is off by default (like Windhawk): hot hooks call it constantly
//    and OutputDebugString is serialized system-wide while a debugger such as
//    DebugView listens. Life-cycle messages are always logged.
//
// Original mod: "Better file sizes in Explorer details" by m417z, GPLv3.
// This runtime is also distributed under the GNU GPL v3.

#define WH_STANDALONE_RUNTIME
#include "windhawk_utils.h"

#include <MinHook.h>

#include "../common/bfs_common.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

BOOL WINAPI Wh_Mod_DllMain(HINSTANCE hinstDLL, DWORD fdwReason,
                           LPVOID lpReserved);

// From src/mod.cpp (global variables there).
extern std::atomic<int> g_hookRefCount;

namespace {

constexpr DWORD kControlLockTimeoutMs = 20000;
constexpr DWORD kRefCountWaitMs = 3000;
constexpr DWORD kUninitWaitMs = 5000;

HMODULE g_module;
std::wstring g_dllDir;
std::wstring g_iniPath;
std::wstring g_logFilePath;
bool g_verboseLog;
SRWLOCK g_logLock = SRWLOCK_INIT;

// Serializes Enable/Disable/Init. A mutex handle so waits can be bounded.
HANDLE g_controlMutex;
bool g_pinned;
bool g_minHookInitialized;
std::atomic<bool> g_enabled{false};
// The host's latest wish; checked again right before hooks are applied.
std::atomic<bool> g_wantEnabled{true};
// Wh_ModUninit didn't finish in time (it keeps running on its own thread).
HANDLE g_uninitThread;

struct HookInfo {
    void* detour;
    void* trampoline;
};
std::unordered_map<void*, HookInfo> g_hooks;  // every hook ever created
std::vector<void*> g_activeTargets;           // hooks of the current session

// Thread that runs init/enable/disable: logs from it are always written.
thread_local bool t_lifecycle;

struct SettingDefault {
    const wchar_t* name;
    const wchar_t* value;
};

// Defaults copied from the mod's WindhawkModSettings block.
const SettingDefault kSettingDefaults[] = {
    {L"calculateFolderSizes", L"everything"},
    {L"sortSizesMixFolders", L"1"},
    {L"disableKbOnlySizes", L"1"},
    {L"useIecTerms", L"0"},
};

std::wstring FormatV(PCWSTR format, va_list args) {
    wchar_t buf[2048];
    int n = _vsnwprintf(buf, ARRAYSIZE(buf) - 1, format, args);
    buf[ARRAYSIZE(buf) - 1] = L'\0';
    if (n < 0) n = (int)wcslen(buf);
    return std::wstring(buf, n);
}

void WriteLogLine(const char* func, const std::wstring& msg) {
    wchar_t prefix[256];
    _snwprintf(prefix, ARRAYSIZE(prefix) - 1, L"[BetterFileSizes %u] [%S] ",
               GetCurrentProcessId(), func ? func : "");
    prefix[ARRAYSIZE(prefix) - 1] = L'\0';
    std::wstring line = prefix + msg + L"\n";
    OutputDebugStringW(line.c_str());

    if (g_logFilePath.empty()) return;
    AcquireSRWLockExclusive(&g_logLock);
    HANDLE file = CreateFileW(g_logFilePath.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
                                        (int)line.size(), nullptr, 0, nullptr,
                                        nullptr);
        std::string utf8(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                            utf8.data(), bytes, nullptr, nullptr);
        DWORD written;
        WriteFile(file, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        CloseHandle(file);
    }
    ReleaseSRWLockExclusive(&g_logLock);
}

void RtLog(PCWSTR format, ...) {
    va_list args;
    va_start(args, format);
    std::wstring msg = FormatV(format, args);
    va_end(args);
    WriteLogLine("runtime", msg);
}

// Re-read the INI location and the [Debug] options.
void LoadRuntimeSettings() {
    g_iniPath = bfs::ResolveIniPath(g_dllDir);
    std::wstring lf =
        bfs::Trim(bfs::IniString(g_iniPath, L"Debug", L"logFile", L""));
    if (!lf.empty() && lf.find(L':') == std::wstring::npos &&
        lf.rfind(L"\\\\", 0) != 0) {
        lf = g_dllDir + L"\\" + lf;
    }
    g_logFilePath = lf;
    g_verboseLog = GetPrivateProfileIntW(L"Debug", L"verbose", 0,
                                         g_iniPath.c_str()) != 0;
}

std::wstring ReadIniString(PCWSTR section, PCWSTR key, PCWSTR def) {
    return bfs::IniString(g_iniPath, section, key, def);
}

const wchar_t* DefaultFor(const std::wstring& name) {
    for (const auto& d : kSettingDefaults) {
        if (name == d.name) return d.value;
    }
    return L"";
}

bool IsCurrentProcessExcluded() {
    std::wstring path = bfs::ModulePath(nullptr);
    if (path.empty()) return false;
    if (bfs::MatchesAny(bfs::CriticalProcesses(), path)) return true;
    // Extra user excludes: [Process] exclude=a.exe|b*.exe
    return bfs::MatchesAny(
        bfs::SplitList(ReadIniString(L"Process", L"exclude", L"")), path);
}

bool LockControl() {
    DWORD r = WaitForSingleObject(g_controlMutex, kControlLockTimeoutMs);
    return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
}

void UnlockControl() { ReleaseMutex(g_controlMutex); }

// Cancels hooks that were queued but not applied (e.g. Wh_ModInit failed).
void CancelQueuedHooks() {
    for (void* target : g_activeTargets) MH_QueueDisableHook(target);
    MH_ApplyQueued();
    g_activeTargets.clear();
}

DWORD WINAPI UninitThread(LPVOID) {
    t_lifecycle = true;
    Wh_ModUninit();  // has its own (unbounded) wait for hooks in flight
    return 0;
}

// Caller holds the control lock.
DWORD EnableLocked() {
    if (g_enabled) return 0;
    if (g_uninitThread) {
        // A previous Wh_ModUninit is still running; give it a last chance.
        if (WaitForSingleObject(g_uninitThread, kUninitWaitMs) != WAIT_OBJECT_0) {
            RtLog(L"Previous uninit still running, not enabling");
            return 3;
        }
        CloseHandle(g_uninitThread);
        g_uninitThread = nullptr;
    }
    LoadRuntimeSettings();
    std::wstring why;
    if (bfs::SafetyValveActive(g_dllDir, &why)) {
        RtLog(L"Safety valve active (%s), staying disabled", why.c_str());
        return 4;
    }
    if (!g_minHookInitialized) {
        if (MH_Initialize() != MH_OK) {
            RtLog(L"MH_Initialize failed");
            return 1;
        }
        g_minHookInitialized = true;
    }
    g_activeTargets.clear();
    if (!Wh_ModInit()) {
        RtLog(L"Wh_ModInit failed");
        CancelQueuedHooks();
        return 1;
    }
    if (!g_wantEnabled) {
        // Disable was requested while we were initializing.
        CancelQueuedHooks();
        Wh_ModUninit();
        RtLog(L"Enable cancelled");
        return 2;
    }
    // One MinHook freeze for all hooks.
    MH_STATUS status = MH_ApplyQueued();
    if (status != MH_OK) RtLog(L"MH_ApplyQueued: %S", MH_StatusToString(status));
    g_enabled = true;
    RtLog(L"Enabled, %u hook(s) active", (unsigned)g_activeTargets.size());
    return 0;
}

// Caller holds the control lock.
DWORD DisableLocked() {
    if (!g_enabled) return 0;
    // 1. Stop new calls from entering hooks (one MinHook freeze).
    for (void* target : g_activeTargets) MH_QueueDisableHook(target);
    MH_STATUS status = MH_ApplyQueued();
    if (status != MH_OK) RtLog(L"MH_ApplyQueued: %S", MH_StatusToString(status));
    g_activeTargets.clear();
    g_enabled = false;

    // 2. Bounded wait for calls still in flight (counted hooks only; the
    //    others are short and the DLL is pinned anyway).
    ULONGLONG start = GetTickCount64();
    while (g_hookRefCount > 0 && GetTickCount64() - start < kRefCountWaitMs) {
        Sleep(20);
    }

    // 3. Wh_ModUninit can wait forever (hook ref count, Everything thread),
    //    so run it on its own thread and never wait longer than a few
    //    seconds. Code stays mapped (pinned), so letting it finish later is
    //    safe.
    HANDLE thread = CreateThread(nullptr, 0, UninitThread, nullptr, 0, nullptr);
    if (thread) {
        if (WaitForSingleObject(thread, kUninitWaitMs) == WAIT_OBJECT_0) {
            CloseHandle(thread);
        } else {
            RtLog(L"Uninit still busy, continuing in the background");
            g_uninitThread = thread;
        }
    }
    RtLog(L"Disabled (DLL stays loaded, hooks off)");
    return 0;
}

void Pin() {
    if (g_pinned) return;
    HMODULE m;
    g_pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_PIN,
                                  (PCWSTR)&Pin, &m) != FALSE;
}

DWORD WINAPI InitThread(LPVOID) {
    t_lifecycle = true;
    LoadRuntimeSettings();
    std::wstring why;
    bool valve = bfs::SafetyValveActive(g_dllDir, &why);
    if (valve || IsCurrentProcessExcluded()) {
        if (valve) RtLog(L"Safety valve active (%s), not loading", why.c_str());
        // Nothing was hooked: it is safe to drop our own reference.
        FreeLibraryAndExitThread(g_module, 0);
    }

    // From here on the DLL is never unmapped.
    Pin();
    RtLog(L"Loaded into process %u (pinned=%d)", GetCurrentProcessId(),
          g_pinned ? 1 : 0);

    if (!LockControl()) return 1;
    DWORD result = g_wantEnabled ? EnableLocked() : 0;
    UnlockControl();
    return result;
}

}  // namespace

extern "C" {

void Wh_Log_Impl(const char* func, PCWSTR format, ...) {
    if (!g_verboseLog && !t_lifecycle) return;
    va_list args;
    va_start(args, format);
    std::wstring msg = FormatV(format, args);
    va_end(args);
    WriteLogLine(func, msg);
}

int Wh_GetIntSetting(PCWSTR valueName, ...) {
    va_list args;
    va_start(args, valueName);
    std::wstring name = FormatV(valueName, args);
    va_end(args);

    std::wstring value = ReadIniString(L"Settings", name.c_str(),
                                       DefaultFor(name));
    if (_wcsicmp(value.c_str(), L"true") == 0 ||
        _wcsicmp(value.c_str(), L"yes") == 0 ||
        _wcsicmp(value.c_str(), L"on") == 0) {
        return 1;
    }
    if (_wcsicmp(value.c_str(), L"false") == 0 ||
        _wcsicmp(value.c_str(), L"no") == 0 ||
        _wcsicmp(value.c_str(), L"off") == 0) {
        return 0;
    }
    return _wtoi(value.c_str());
}

PCWSTR Wh_GetStringSetting(PCWSTR valueName, ...) {
    va_list args;
    va_start(args, valueName);
    std::wstring name = FormatV(valueName, args);
    va_end(args);

    std::wstring value = ReadIniString(L"Settings", name.c_str(),
                                       DefaultFor(name));
    wchar_t* result = new wchar_t[value.size() + 1];
    wcscpy(result, value.c_str());
    return result;
}

void Wh_FreeStringSetting(PCWSTR string) {
    delete[] string;
}

BOOL Wh_SetFunctionHook(void* targetFunction, void* hookFunction,
                        void** originalFunction) {
    if (!targetFunction || !hookFunction) return FALSE;

    auto it = g_hooks.find(targetFunction);
    if (it != g_hooks.end()) {
        // Created in an earlier session: reuse it (trampolines are never
        // freed, see the top of this file).
        if (it->second.detour != hookFunction) {
            Wh_Log(L"Hook %p already exists with another detour", targetFunction);
            return FALSE;
        }
        if (originalFunction) *originalFunction = it->second.trampoline;
    } else {
        void* trampoline = nullptr;
        MH_STATUS status =
            MH_CreateHook(targetFunction, hookFunction, &trampoline);
        if (status != MH_OK) {
            Wh_Log(L"MH_CreateHook(%p) failed: %S", targetFunction,
                   MH_StatusToString(status));
            return FALSE;
        }
        g_hooks[targetFunction] = {hookFunction, trampoline};
        if (originalFunction) *originalFunction = trampoline;
    }
    MH_STATUS status = MH_QueueEnableHook(targetFunction);
    if (status != MH_OK) {
        Wh_Log(L"MH_QueueEnableHook(%p) failed: %S", targetFunction,
               MH_StatusToString(status));
        return FALSE;
    }
    g_activeTargets.push_back(targetFunction);
    return TRUE;
}

BOOL Wh_RemoveFunctionHook(void* targetFunction) {
    return MH_QueueDisableHook(targetFunction) == MH_OK;
}

BOOL Wh_ApplyHookOperations(void) {
    MH_STATUS status = MH_ApplyQueued();
    if (status != MH_OK) {
        Wh_Log(L"MH_ApplyQueued failed: %S", MH_StatusToString(status));
        return FALSE;
    }
    return TRUE;
}

// Effective settings file (used by symbols.cpp for the [Symbols] section).
const wchar_t* BfsGetIniPath(void) {
    return g_iniPath.c_str();
}

// ---- Exported thread procs, called by the hosts via CreateRemoteThread ----
// Return values: 0 ok, 1 failed, 2 cancelled, 3 busy, 4 safety valve,
// 5 control lock timeout. None of them ever unloads the DLL.

__declspec(dllexport) DWORD WINAPI BfsEnable(LPVOID) {
    t_lifecycle = true;
    g_wantEnabled = true;
    if (!LockControl()) return 5;
    DWORD r = EnableLocked();
    UnlockControl();
    return r;
}

__declspec(dllexport) DWORD WINAPI BfsDisable(LPVOID) {
    t_lifecycle = true;
    g_wantEnabled = false;
    if (!LockControl()) return 5;
    DWORD r = DisableLocked();
    UnlockControl();
    return r;
}

// Compatibility with hosts from the first version: same as BfsDisable (the
// DLL is no longer freed).
__declspec(dllexport) DWORD WINAPI BfsUnload(LPVOID param) {
    return BfsDisable(param);
}

__declspec(dllexport) DWORD WINAPI BfsReload(LPVOID) {
    t_lifecycle = true;
    g_wantEnabled = true;
    if (!LockControl()) return 5;
    DisableLocked();
    DWORD r = EnableLocked();
    UnlockControl();
    return r;
}

// 1 = hooks active, 0 = loaded but disabled.
__declspec(dllexport) DWORD WINAPI BfsIsEnabled(LPVOID) {
    return g_enabled ? 1 : 0;
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    switch (fdwReason) {
        case DLL_PROCESS_ATTACH: {
            g_module = hinstDLL;
            DisableThreadLibraryCalls(hinstDLL);
            // Shadow copies (BetterFileSizes-<ver>-<hash>.dll) live under
            // %LOCALAPPDATA%\BetterFileSizes\runtime\; the install dir
            // (where BetterFileSizes.ini and the shipped DLL live) is in the
            // sidecar <shadow>.home. Fall back to the DLL's own folder.
            {
                std::wstring self = bfs::ModulePath(hinstDLL);
                std::wstring home = bfs::ReadHomeSidecar(self);
                g_dllDir = !home.empty() ? home : bfs::DirName(self);
            }
            g_controlMutex = CreateMutexW(nullptr, FALSE, nullptr);
            Wh_Mod_DllMain(hinstDLL, fdwReason, lpReserved);
            HANDLE thread =
                CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
            if (thread) CloseHandle(thread);
            break;
        }

        case DLL_PROCESS_DETACH:
            // Only reached on process exit (we are pinned) or when the init
            // thread decided not to hook anything in this process.
            Wh_Mod_DllMain(hinstDLL, fdwReason, lpReserved);
            if (!lpReserved && g_controlMutex) {
                CloseHandle(g_controlMutex);
                g_controlMutex = nullptr;
            }
            break;
    }
    return TRUE;
}
