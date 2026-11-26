// Settings window of bfs-daemon.exe: edits the main INI keys. The INI stays
// the only storage; the host's INI watcher applies the change. GNU GPL v3.
#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace bfs {

struct SettingsHooks {
    std::wstring dllDir;                        // where BetterFileSizes.dll is
    std::function<bool()> getAutostart;         // optional
    std::function<void(bool)> setAutostart;     // optional
    std::function<std::wstring()> hostIniPath;  // INI the host uses (optional)
    std::function<void()> reloadHost;           // optional
};

// Opens the settings window, or brings it to the front if it is open.
// modal=true: runs its own loop and returns when it is closed.
void ShowSettings(HINSTANCE instance, const SettingsHooks& hooks, bool modal);
// For the daemon's message loop (modeless dialog keyboard handling).
bool SettingsDialogMessage(MSG* msg);
// The INI file the settings window writes to (exposed for tests/README):
// the effective INI if it is writable, otherwise the %LOCALAPPDATA% copy.
std::wstring SettingsTargetIni(const std::wstring& dllDir);

}  // namespace bfs
