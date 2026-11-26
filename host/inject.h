// Loading BetterFileSizes.dll into other processes (and, for the Everything
// plugin, into the current process) and switching its hooks on/off.
//
// Shadow copies: the shipped DLL is never injected. Init() copies
// it to %LOCALAPPDATA%\BetterFileSizes\runtime\<arch>\
// BetterFileSizes-<version>-<hash>.dll and that copy is injected, so the
// installed files (e.g. Everything\plugins\BetterFileSizes\*.dll) are never
// locked by explorer & co. and can be replaced at any time.
//
// The DLL pins itself once it is active, so it is never unloaded; "unload"
// means calling its BfsDisable export (hooks off, code stays mapped).
// Several versions can therefore be mapped in one process; at most one of
// them may have its hooks on (see Inject()). Exports are always resolved
// from the module image in the target's memory, never from a file on disk
// (the file may have been replaced by a newer version meanwhile).
// Every wait here is bounded. GNU GPL v3.
#pragma once

#include "../common/bfs_common.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bfs {

using LogFn = std::function<void(const std::wstring&)>;

class Injector {
   public:
    // shippedDllPath: the installed BetterFileSizes.dll (same bitness as
    // us). Prepares the shadow copy; if that fails the shipped file is used
    // directly and Warning() says why.
    bool Init(const std::wstring& shippedDllPath, std::wstring* error);
    const std::wstring& DllPath() const { return dllPath_; }  // injected file
    const std::wstring& ShippedPath() const { return shipped_; }
    const std::wstring& Warning() const { return warning_; }
    const std::wstring& Version() const { return version_; }

    // Deletes old shadow copies that no process has loaded any more (a
    // loaded DLL file can't be deleted, so locked ones just stay).
    void CleanupShadows(const LogFn& log);

    enum class Result { Loaded, Enabled, AlreadyActive, Failed };
    // Turns old copies off, then loads our copy, or, if it is already
    // loaded (e.g. disabled earlier), calls BfsEnable in it.
    Result Inject(DWORD pid, std::wstring* error, const LogFn& log = nullptr);

    // Calls BfsDisable in every copy in every process, in parallel, and
    // waits at most `timeoutMs` in total. Returns the number of processes.
    size_t DisableAll(const LogFn& log, DWORD timeoutMs = 6000);

    struct Copy {
        HMODULE base = nullptr;
        std::wstring path;
        bool current = false;  // our shadow copy
        int enabled = -1;      // 1/0, -1 unknown (only filled by ListLoaded)
        DWORD rvaEnable = 0, rvaDisable = 0, rvaIsEnabled = 0;
    };
    // Processes that have any copy of the DLL loaded (process name, copies).
    // queryState: also ask each copy whether its hooks are on (slower).
    std::map<DWORD, std::pair<std::wstring, std::vector<Copy>>> ListLoaded(
        bool queryState = false);

   private:
    bool EnsureShadow(std::wstring* error);
    std::vector<Copy> FindCopies(HANDLE process);
    // Runs base+rva(param=null) as a thread in the process; null on error.
    HANDLE StartAt(HANDLE process, DWORD pid, HMODULE base, DWORD rva);
    // Runs and waits; returns the thread's exit code or `fallback`.
    DWORD RunAt(HANDLE process, DWORD pid, HMODULE base, DWORD rva,
                DWORD timeoutMs, DWORD fallback, bool* finished = nullptr);

    std::wstring shipped_;
    std::wstring dllPath_;
    std::wstring warning_;
    std::wstring version_;
    std::wstring hash_;
    std::mutex shadowMutex_;
};

}  // namespace bfs
