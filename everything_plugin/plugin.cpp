// BetterFileSizesEverything.dll - Everything 1.5 plugin that hosts the
// Better file sizes daemon logic (host/host.h) inside Everything, so no extra
// background program is needed.
//
// Plugin API: official Everything 1.5 Plugin SDK (everything_plugin.h),
// https://www.voidtools.com/forum/viewtopic.php?t=16535
//
// Layout expected in the Everything installation folder:
//   plugins\BetterFileSizesEverything.dll            <- this plugin
//   plugins\BetterFileSizes\BetterFileSizes.dll      <- the mod DLL
//   plugins\BetterFileSizes\BetterFileSizes.ini
// BetterFileSizes.dll is deliberately kept in a sub folder so that Everything
// never tries to load it as a plugin itself.
//
// GNU GPL v3.

#include "../host/host.h"

#include <cstdio>

// The SDK header expects <windows.h> first and uses MSVC-only literal
// suffixes only inside macros we don't use.
#include "../third_party/everything_plugin_sdk/everything_plugin.h"

namespace {

HMODULE g_module;
bfs::Host* g_host;
bool g_started;

void Log(const std::wstring& msg) {
    std::wstring line = L"[BetterFileSizes Everything plugin " +
                        std::wstring(bfs::kArch) + L"] " + msg + L"\n";
    OutputDebugStringW(line.c_str());
}

std::wstring FindModDll() {
    std::wstring dir = bfs::DirName(bfs::ModulePath(g_module));
    std::wstring candidates[] = {
        dir + L"\\BetterFileSizes\\" + bfs::kDllName,
        dir + L"\\" + bfs::kDllName,
    };
    for (auto& c : candidates) {
        if (bfs::FileExists(c)) return c;
    }
    return candidates[0];
}

bool StartHost() {
    if (g_started) return true;
    g_host = new bfs::Host();
    bfs::HostOptions opts;
    opts.dllPath = FindModDll();
    // Everything itself: the mod hooks SHOpenFolderAndSelectItems there when
    // calculateFolderSizes=everything, and Everything's own file dialogs
    // benefit as well. Loaded in-process with LoadLibrary.
    opts.loadIntoSelf = true;
    opts.log = Log;
    std::wstring error;
    if (!g_host->Start(opts, &error)) {
        Log(L"failed to start: " + error);
        delete g_host;
        g_host = nullptr;
        return false;
    }
    g_started = true;
    return true;
}

void StopHost() {
    if (!g_started) return;
    // Bounded, so Everything's shutdown is never held up for long. By
    // default the hooks stay active in the processes they are in (like
    // Windhawk); [Host] disableOnExit=1 turns them off everywhere.
    if (g_host->Stop(4000)) {
        delete g_host;
    } else {
        // Threads still running: leak the object. Our DLL is pinned, so
        // their code stays mapped even after Everything "frees" the plugin.
        Log(L"stop timed out; host threads left running");
    }
    g_host = nullptr;
    g_started = false;
}

void PinSelf() {
    HMODULE m;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (PCWSTR)&PinSelf, &m);
}

}  // namespace

extern "C" __declspec(dllexport) void* EVERYTHING_PLUGIN_API
everything_plugin_proc(DWORD msg, void* data) {
    (void)data;
    switch (msg) {
        case EVERYTHING_PLUGIN_PM_INIT:
            // We don't need any Everything API functions. Pin ourselves so
            // a background thread can never outlive our code.
            PinSelf();
            return (void*)1;

        case EVERYTHING_PLUGIN_PM_GET_PLUGIN_VERSION:
            return (void*)(uintptr_t)EVERYTHING_PLUGIN_VERSION;

        case EVERYTHING_PLUGIN_PM_GET_NAME:
            return (void*)"Better File Sizes";

        case EVERYTHING_PLUGIN_PM_GET_DESCRIPTION:
            return (void*)"Show folder sizes in Explorer and file dialogs";

        case EVERYTHING_PLUGIN_PM_GET_AUTHOR:
            return (void*)"Nurdich";

        case EVERYTHING_PLUGIN_PM_GET_VERSION:
            return (void*)"1.0.0";

        case EVERYTHING_PLUGIN_PM_GET_LINK:
            return (void*)"https://github.com/Nurdich/better-file-sizes-standalone";

        case EVERYTHING_PLUGIN_PM_START:
            return (void*)(uintptr_t)(StartHost() ? 1 : 0);

        case EVERYTHING_PLUGIN_PM_STOP:
            StopHost();
            return (void*)1;

        case EVERYTHING_PLUGIN_PM_KILL:
            // Last message before FreeLibrary; make sure no thread survives.
            StopHost();
            return (void*)1;
    }
    return (void*)0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
