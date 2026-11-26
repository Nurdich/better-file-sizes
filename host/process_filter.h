// Process filter shared by bfs-daemon, the Everything plugin and
// bfs-injector: decides which processes get BetterFileSizes.dll.
//
// GNU GPL v3.
#pragma once

#include "../common/bfs_common.h"

#include <string>
#include <vector>

namespace bfs {

enum class Mode {
    Smart,      // interactive GUI processes only, with built-in skip lists
    All,        // every process that technically can be injected
    Whitelist,  // only processes matching [Process] include
};

enum class Trigger {
    Window,  // inject as soon as the process shows a top-level window
    Shell,   // ... and has also loaded windows.storage.dll
};

struct FilterConfig {
    Mode mode = Mode::Smart;
    Trigger trigger = Trigger::Window;
    std::vector<std::wstring> include;  // user, | separated, wildcards
    std::vector<std::wstring> exclude;  // user, always wins
    bool skipGames = true;              // built-in anti-cheat/game list
    unsigned scanIntervalMs = 0;        // safety-net rescan
    bool loadIntoEverything = false;    // calculateFolderSizes=everything

    static FilterConfig Load(const std::wstring& iniPath);
    static PCWSTR ModeName(Mode m);
};

enum class Verdict {
    Inject,
    Skip,  // final for the lifetime of this process
    Wait,  // not yet (no window yet / windows.storage.dll not loaded yet)
};

struct Evaluation {
    Verdict verdict = Verdict::Skip;
    std::wstring path;    // full image path, if known
    std::wstring reason;  // human-readable, for logs
};

// hasWindow: the caller saw a visible top-level window of this process.
// allowSelf: the caller is the Everything plugin evaluating Everything itself.
Evaluation EvaluateProcess(const FilterConfig& cfg, DWORD pid, bool hasWindow,
                           bool allowSelf = false);

// Built-in lists (exposed for README/status output).
const std::vector<std::wstring>& BuiltinHardExcludes();
const std::vector<std::wstring>& BuiltinGameExcludes();

bool ProcessHasModule(HANDLE process, PCWSTR moduleName);

}  // namespace bfs
