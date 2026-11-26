// Small helpers shared by BetterFileSizes.dll, the injector, the tray daemon
// and the Everything plugin. Header-only on purpose (each binary is linked
// separately and the DLL must stay self-contained).
//
// GNU GPL v3.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwctype>
#include <string>
#include <vector>

namespace bfs {

inline constexpr wchar_t kDllName[] = L"BetterFileSizes.dll";
inline constexpr wchar_t kIniName[] = L"BetterFileSizes.ini";

#ifdef _WIN64
inline constexpr wchar_t kArch[] = L"x64";
#else
inline constexpr wchar_t kArch[] = L"x86";
#endif

inline std::wstring ModulePath(HMODULE module) {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(module, path.data(), (DWORD)path.size());
        if (len == 0) return L"";
        if (len < path.size()) {
            path.resize(len);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

inline std::wstring DirName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"" : path.substr(0, slash);
}

inline std::wstring BaseName(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

inline bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

// %LOCALAPPDATA%\BetterFileSizes (also holds the symbol cache).
inline std::wstring UserDataDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, ARRAYSIZE(buf));
    if (n == 0 || n >= ARRAYSIZE(buf)) return L"";
    return std::wstring(buf, n) + L"\\BetterFileSizes";
}

// The settings file actually used. A copy in %LOCALAPPDATA%\BetterFileSizes
// wins over the one next to the DLL, so the INI can be edited without admin
// rights when the DLL lives in Program Files (e.g. Everything\plugins).
inline std::wstring ResolveIniPath(const std::wstring& dllDir) {
    std::wstring user = UserDataDir();
    if (!user.empty()) {
        std::wstring candidate = user + L"\\" + kIniName;
        if (FileExists(candidate)) return candidate;
    }
    return dllDir + L"\\" + kIniName;
}

inline std::wstring IniString(const std::wstring& ini, PCWSTR section,
                              PCWSTR key, PCWSTR def) {
    std::wstring buf(4096, L'\0');
    DWORD n = GetPrivateProfileStringW(section, key, def, buf.data(),
                                       (DWORD)buf.size(), ini.c_str());
    buf.resize(n);
    return buf;
}

// Any copy of the mod DLL: the shipped BetterFileSizes.dll and the shadow
// copies BetterFileSizes-<version>-<hash>.dll (not the Everything plugin).
inline bool IsModDllName(const wchar_t* baseName) {
    if (_wcsnicmp(baseName, L"BetterFileSizes", 15) != 0) return false;
    if (_wcsnicmp(baseName, L"BetterFileSizesEverything", 25) == 0) return false;
    size_t n = wcslen(baseName);
    if (n < 4 || _wcsicmp(baseName + n - 4, L".dll") != 0) return false;
    // "BetterFileSizes.dll" or "BetterFileSizes-..."
    return baseName[15] == L'.' || baseName[15] == L'-';
}

// Shadow copies live here (per bitness); see host/shadow.h.
inline std::wstring ShadowDir() {
    std::wstring user = UserDataDir();
    return user.empty() ? std::wstring() : user + L"\\runtime\\" + kArch;
}

// A shadow copy "X.dll" has a sidecar "X.dll.home" holding the folder of the
// installed (shipped) DLL, i.e. where its BetterFileSizes.ini lives.
inline std::wstring ReadHomeSidecar(const std::wstring& dllPath) {
    HANDLE f = CreateFileW((dllPath + L".home").c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    char buf[4096];
    DWORD n = 0;
    BOOL ok = ReadFile(f, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(f);
    if (!ok || !n) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, buf, (int)n, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, buf, (int)n, w.data(), len);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' ||
                          w.back() == L' ' || w.back() == L'\\')) {
        w.pop_back();
    }
    return w;
}

inline std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.front())) s.erase(0, 1);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    return s;
}

inline std::vector<std::wstring> SplitList(const std::wstring& s,
                                           wchar_t sep = L'|') {
    std::vector<std::wstring> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(sep, start);
        if (end == std::wstring::npos) end = s.size();
        std::wstring item = Trim(s.substr(start, end - start));
        if (!item.empty()) out.push_back(item);
        start = end + 1;
    }
    return out;
}

// Case-insensitive '*' / '?' matching.
inline bool WildcardMatch(const wchar_t* pattern, const wchar_t* str) {
    while (*pattern) {
        if (*pattern == L'*') {
            pattern++;
            if (!*pattern) return true;
            for (; *str; str++) {
                if (WildcardMatch(pattern, str)) return true;
            }
            return false;
        }
        if (!*str) return false;
        if (*pattern != L'?' && towlower(*pattern) != towlower(*str)) {
            return false;
        }
        pattern++;
        str++;
    }
    return *str == L'\0';
}

// Patterns containing a backslash match (a suffix of) the full path,
// others match the file name only.
inline bool MatchesProcessPattern(const std::wstring& pattern,
                                  const std::wstring& fullPath) {
    if (pattern.empty()) return false;
    if (pattern.find(L'\\') != std::wstring::npos) {
        std::wstring p = pattern;
        if (p[0] != L'*') p = L"*" + p;
        return WildcardMatch(p.c_str(), fullPath.c_str());
    }
    return WildcardMatch(pattern.c_str(), BaseName(fullPath).c_str());
}

inline bool MatchesAny(const std::vector<std::wstring>& patterns,
                       const std::wstring& fullPath) {
    for (auto& p : patterns) {
        if (MatchesProcessPattern(p, fullPath)) return true;
    }
    return false;
}


// ---------------------------------------------------------------------------
// Processes that must never get the DLL, in any mode, whoever loads it (the
// DLL itself checks this list too). Desktop/shell/input-critical processes:
// freezing their threads (MinHook does that while patching) or crashing them
// can hang the whole desktop. explorer.exe is deliberately NOT here.
inline const std::vector<std::wstring>& CriticalProcesses() {
    static const std::vector<std::wstring> list = {
        // Original mod's @exclude list.
        L"conhost.exe", L"Plex*.exe", L"backgroundTaskHost.exe", L"LockApp.exe",
        L"SearchHost.exe", L"ShellExperienceHost.exe",
        L"StartMenuExperienceHost.exe", L"msedgewebview2.exe", L"windhawk.exe",
        L"windhawk-ui.exe", L"*\\UI\\VSCodium.exe",
        // Our own programs.
        L"bfs-injector*.exe", L"bfs-daemon*.exe",
        // Session / desktop / graphics.
        L"System", L"Registry", L"smss.exe", L"csrss.exe", L"wininit.exe",
        L"winlogon.exe", L"services.exe", L"lsass.exe", L"lsaiso.exe",
        L"LogonUI.exe", L"dwm.exe", L"fontdrvhost.exe", L"sihost.exe",
        L"consent.exe", L"userinit.exe", L"WerFault*.exe", L"wermgr.exe",
        L"audiodg.exe", L"svchost.exe", L"dllhost.exe", L"taskhostw.exe",
        L"RuntimeBroker.exe", L"spoolsv.exe", L"splwow64.exe",
        L"WUDFHost.exe", L"WmiPrvSE.exe", L"SearchIndexer.exe",
        L"SearchProtocolHost.exe", L"SearchFilterHost.exe",
        // Shell UI hosts (Start, taskbar pieces, notifications, lock screen).
        L"ShellHost.exe", L"SearchApp.exe", L"SearchUI.exe",
        L"TextInputHost.exe", L"InputApp.exe", L"ApplicationFrameHost.exe",
        L"ScreenClippingHost.exe", L"CredentialUIBroker.exe",
        L"PickerHost.exe", L"Widgets.exe", L"WidgetService.exe",
        L"PhoneExperienceHost.exe", L"CrossDeviceResume.exe",
        L"UserOOBEBroker.exe", L"smartscreen.exe", L"Taskmgr.exe",
        L"SecurityHealthSystray.exe", L"SecurityHealthService.exe",
        L"MsMpEng.exe", L"NisSrv.exe", L"MpDefenderCoreService.exe",
        L"OpenConsole.exe", L"WindowsTerminal.exe",
        // Text input / IME / accessibility: freezing them stalls typing
        // in every program.
        L"ctfmon.exe", L"TabTip.exe", L"osk.exe", L"ChsIME.exe",
        L"ChtIME.exe", L"*IMEBroker*.exe", L"InputPersonalization.exe",
        L"Sogou*.exe", L"SGTool.exe", L"QQPinyin*.exe", L"QQInput*.exe",
        L"BaiduPinyin*.exe", L"BaiduIME*.exe", L"iFlyIME*.exe", L"WeType*.exe",
        L"Narrator.exe", L"Magnify.exe", L"AtBroker.exe", L"utilman.exe",
        L"VoiceAccess.exe",
        // GPU / audio vendor overlays and hosts.
        L"nvcontainer.exe", L"NVDisplay.Container.exe", L"NVIDIA Overlay.exe",
        L"NVIDIA Share.exe", L"RadeonSoftware.exe", L"AMDRSServ.exe",
        L"GameBar*.exe", L"XboxGameBar*.exe", L"RtkAudUService64.exe",
    };
    return list;
}

// Safety valve: if any of these exist, the DLL does nothing when loaded and
// the hosts don't inject anything:
//   <dll folder>\BetterFileSizes.disable
//   %LOCALAPPDATA%\BetterFileSizes\BetterFileSizes.disable
//   [Debug] disable=1 in the effective INI
inline bool SafetyValveActive(const std::wstring& dllDir,
                              std::wstring* why = nullptr) {
    std::wstring user = UserDataDir();
    std::wstring candidates[] = {
        dllDir + L"\\BetterFileSizes.disable",
        user.empty() ? std::wstring() : user + L"\\BetterFileSizes.disable"};
    for (auto& c : candidates) {
        if (c.empty()) continue;
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (why) *why = c;
            return true;
        }
    }
    std::wstring ini = ResolveIniPath(dllDir);
    if (GetPrivateProfileIntW(L"Debug", L"disable", 0, ini.c_str()) != 0) {
        if (why) *why = ini + L" [Debug] disable=1";
        return true;
    }
    return false;
}

}  // namespace bfs
