// GNU GPL v3.
#include "process_filter.h"

#include <psapi.h>

namespace bfs {

namespace {

// Smart mode only: programs in the Windows system folders are skipped,
// except these (they show normal shell file views/dialogs).
const std::vector<std::wstring> kSystemDirAllow = {
    L"notepad.exe", L"mspaint.exe", L"write.exe", L"mmc.exe", L"regedit.exe",
    L"charmap.exe", L"SnippingTool.exe", L"mstsc.exe", L"msinfo32.exe",
    L"eventvwr.exe", L"WFS.exe", L"xpsrchvw.exe", L"wordpad.exe",
};

bool IsInSystemDir(const std::wstring& path) {
    wchar_t win[MAX_PATH];
    UINT n = GetWindowsDirectoryW(win, ARRAYSIZE(win));
    if (!n || n >= ARRAYSIZE(win)) return false;
    std::wstring w(win, n);
    std::wstring prefixes[] = {w + L"\\System32\\", w + L"\\SysWOW64\\",
                               w + L"\\SystemApps\\",
                               w + L"\\ImmersiveControlPanel\\"};
    for (auto& p : prefixes) {
        if (path.size() > p.size() &&
            _wcsnicmp(path.c_str(), p.c_str(), p.size()) == 0) {
            return true;
        }
    }
    return false;
}

// Smart mode only: games, launchers and anti-cheat components where DLL
// injection can get the user kicked/banned or crash the game. None of them
// needs folder sizes. Override with [Process] include=... if you really want.
const std::vector<std::wstring> kGameExcludes = {
    // Easy Anti-Cheat (Epic)
    L"EasyAntiCheat*.exe", L"EACLauncher.exe", L"start_protected_game.exe",
    L"*_EAC.exe", L"*_EAC_EOS.exe",
    // BattlEye
    L"BEService*.exe", L"BattlEye*.exe", L"*_BE.exe",
    // Riot Vanguard / Riot games
    L"vgc.exe", L"vgtray.exe", L"vgm.exe", L"RiotClient*.exe",
    L"VALORANT*.exe", L"League of Legends.exe", L"LeagueClient*.exe",
    // FACEIT / ESEA
    L"FACEIT*.exe", L"faceit*.exe", L"ESEA*.exe",
    // EA Javelin anti-cheat
    L"EAAntiCheat*.exe",
    // Tencent ACE (AntiCheatExpert) / TenProtect
    L"SGuard*.exe", L"ACE-*.exe", L"TenSafe*.exe", L"TPHelper.exe",
    // nProtect GameGuard, XIGNCODE3, PunkBuster, HackShield, Ricochet
    L"GameMon*.exe", L"npggsvc.exe", L"xmag.exe", L"xcorona*.exe",
    L"PnkBstr*.exe", L"pbsvc.exe", L"EhSvc.exe", L"HSUpdate.exe",
    L"atvi-*.exe",
    // Well-known protected game executables
    L"cs2.exe", L"csgo.exe", L"dota2.exe", L"r5apex*.exe", L"TslGame*.exe",
    L"ExecPubg.exe", L"Fortnite*.exe", L"RainbowSix*.exe", L"Overwatch*.exe",
    L"destiny2*.exe", L"EscapeFromTarkov*.exe", L"BsgLauncher.exe",
    L"cod.exe", L"sp*-cod.exe", L"ModernWarfare*.exe",
    L"BlackOps*.exe", L"GTA5*.exe", L"PlayGTAV.exe", L"RustClient.exe",
    L"GenshinImpact.exe", L"YuanShen.exe", L"StarRail.exe",
    L"ZenlessZoneZero.exe", L"RobloxPlayer*.exe", L"Warframe*.exe",
    L"eldenring.exe", L"Wow.exe", L"WowClassic.exe", L"Diablo*.exe", L"Hearthstone.exe",
    // Unreal Engine shipping builds = games
    L"*-Win64-Shipping*.exe", L"*-WinGDK-Shipping*.exe",
    // Game library folders (path patterns)
    L"*\\steamapps\\common\\*", L"*\\Epic Games\\*", L"*\\Riot Games\\*",
    L"*\\XboxGames\\*",
};

bool QueryImagePath(HANDLE process, std::wstring* path) {
    std::wstring buf(1024, L'\0');
    DWORD size = (DWORD)buf.size();
    if (!QueryFullProcessImageNameW(process, 0, buf.data(), &size)) {
        return false;
    }
    buf.resize(size);
    *path = buf;
    return true;
}

bool IsSameBitness(HANDLE process) {
    BOOL selfWow = FALSE, targetWow = FALSE;
    IsWow64Process(GetCurrentProcess(), &selfWow);
    if (!IsWow64Process(process, &targetWow)) return false;
    return selfWow == targetWow;
}

DWORD SelfIntegrity() {
    static DWORD s_level = [] {
        DWORD level = SECURITY_MANDATORY_MEDIUM_RID;
        HANDLE token;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            BYTE buf[256];
            DWORD len;
            if (GetTokenInformation(token, TokenIntegrityLevel, buf,
                                    sizeof(buf), &len)) {
                auto til = (TOKEN_MANDATORY_LABEL*)buf;
                level = *GetSidSubAuthority(
                    til->Label.Sid,
                    *GetSidSubAuthorityCount(til->Label.Sid) - 1);
            }
            CloseHandle(token);
        }
        return level;
    }();
    return s_level;
}

// Returns false (and a reason) for sandboxed / elevated / AppContainer.
bool CheckToken(HANDLE process, std::wstring* reason) {
    HANDLE token;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        *reason = L"cannot open token (elevated or protected)";
        return false;
    }
    bool ok = true;
    BYTE buf[256];
    DWORD len;
    if (GetTokenInformation(token, TokenIntegrityLevel, buf, sizeof(buf),
                            &len)) {
        auto til = (TOKEN_MANDATORY_LABEL*)buf;
        DWORD level = *GetSidSubAuthority(
            til->Label.Sid, *GetSidSubAuthorityCount(til->Label.Sid) - 1);
        if (level < SECURITY_MANDATORY_MEDIUM_RID) {
            *reason = L"low/untrusted integrity (sandbox)";
            ok = false;
        } else if (level > SelfIntegrity()) {
            *reason = L"higher integrity than us (elevated)";
            ok = false;
        }
    }
    DWORD isAppContainer = 0;
    if (ok && GetTokenInformation(token, TokenIsAppContainer, &isAppContainer,
                                  sizeof(isAppContainer), &len) &&
        isAppContainer) {
        *reason = L"AppContainer (UWP/sandbox)";
        ok = false;
    }
    CloseHandle(token);
    return ok;
}

bool CheckProtectionAndMitigations(HANDLE process, std::wstring* reason) {
    PROCESS_PROTECTION_LEVEL_INFORMATION ppl = {};
    if (GetProcessInformation(process, ProcessProtectionLevelInfo, &ppl,
                              sizeof(ppl)) &&
        ppl.ProtectionLevel != PROTECTION_LEVEL_NONE) {
        *reason = L"protected process";
        return false;
    }
    // Only Microsoft-signed DLLs allowed -> LoadLibrary would fail.
    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sig = {};
    if (GetProcessMitigationPolicy(process, ProcessSignaturePolicy, &sig,
                                   sizeof(sig)) &&
        (sig.MicrosoftSignedOnly || sig.StoreSignedOnly)) {
        *reason = L"signed-DLLs-only mitigation";
        return false;
    }
    // Arbitrary Code Guard -> MinHook cannot allocate trampolines.
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dyn = {};
    if (GetProcessMitigationPolicy(process, ProcessDynamicCodePolicy, &dyn,
                                   sizeof(dyn)) &&
        dyn.ProhibitDynamicCode) {
        *reason = L"dynamic code prohibited (ACG)";
        return false;
    }
    return true;
}

}  // namespace

const std::vector<std::wstring>& BuiltinHardExcludes() { return CriticalProcesses(); }
const std::vector<std::wstring>& BuiltinGameExcludes() { return kGameExcludes; }

PCWSTR FilterConfig::ModeName(Mode m) {
    switch (m) {
        case Mode::Smart: return L"smart";
        case Mode::All: return L"all";
        case Mode::Whitelist: return L"whitelist";
    }
    return L"?";
}

FilterConfig FilterConfig::Load(const std::wstring& ini) {
    FilterConfig cfg;
    std::wstring mode = Trim(IniString(ini, L"Process", L"mode", L"smart"));
    if (_wcsicmp(mode.c_str(), L"all") == 0) {
        cfg.mode = Mode::All;
    } else if (_wcsicmp(mode.c_str(), L"whitelist") == 0) {
        cfg.mode = Mode::Whitelist;
    }
    std::wstring trigger =
        Trim(IniString(ini, L"Process", L"trigger", L"window"));
    if (_wcsicmp(trigger.c_str(), L"shell") == 0) cfg.trigger = Trigger::Shell;
    cfg.include = SplitList(IniString(ini, L"Process", L"include", L""));
    cfg.exclude = SplitList(IniString(ini, L"Process", L"exclude", L""));
    cfg.skipGames =
        GetPrivateProfileIntW(L"Process", L"skipGames", 1, ini.c_str()) != 0;
    int scan = (int)GetPrivateProfileIntW(L"Process", L"scanInterval", 0,
                                          ini.c_str());
    if (scan <= 0) scan = cfg.mode == Mode::Smart ? 30 : 3;
    if (scan < 1) scan = 1;
    cfg.scanIntervalMs = (unsigned)scan * 1000;
    std::wstring calc = Trim(IniString(ini, L"Settings", L"calculateFolderSizes",
                                       L"everything"));
    cfg.loadIntoEverything = _wcsicmp(calc.c_str(), L"everything") == 0;
    return cfg;
}

bool ProcessHasModule(HANDLE process, PCWSTR moduleName) {
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(process, modules, sizeof(modules), &needed,
                              LIST_MODULES_DEFAULT)) {
        return false;
    }
    DWORD count = needed / sizeof(HMODULE);
    if (count > ARRAYSIZE(modules)) count = ARRAYSIZE(modules);
    wchar_t name[MAX_PATH];
    for (DWORD i = 0; i < count; i++) {
        if (GetModuleBaseNameW(process, modules[i], name, ARRAYSIZE(name)) &&
            _wcsicmp(name, moduleName) == 0) {
            return true;
        }
    }
    return false;
}

Evaluation EvaluateProcess(const FilterConfig& cfg, DWORD pid, bool hasWindow,
                           bool allowSelf) {
    Evaluation ev;
    bool self = pid == GetCurrentProcessId();
    if (pid <= 4 || (self && !allowSelf)) {
        ev.reason = L"system or self";
        return ev;
    }

    DWORD session = 0, ourSession = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &ourSession);
    if (!ProcessIdToSessionId(pid, &session) || session != ourSession) {
        ev.reason = L"other session";
        return ev;
    }

    HANDLE process = OpenProcess(
        PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        ev.reason = L"cannot open process";
        return ev;
    }

    struct Closer {
        HANDLE h;
        ~Closer() { CloseHandle(h); }
    } closer{process};

    if (!QueryImagePath(process, &ev.path)) {
        ev.reason = L"no image path";
        return ev;
    }

    if (MatchesAny(cfg.exclude, ev.path)) {
        ev.reason = L"[Process] exclude";
        return ev;
    }
    if (MatchesAny(CriticalProcesses(), ev.path)) {
        ev.reason = L"built-in never-inject list (system/shell/input)";
        return ev;
    }
    bool included = MatchesAny(cfg.include, ev.path);
    bool everythingSelf = self && cfg.loadIntoEverything;

    if (cfg.mode == Mode::Whitelist && !included && !everythingSelf) {
        ev.reason = L"not in whitelist";
        return ev;
    }

    if (!self) {
        if (!IsSameBitness(process)) {
            ev.reason = L"different bitness";
            return ev;
        }
        if (!CheckToken(process, &ev.reason) ||
            !CheckProtectionAndMitigations(process, &ev.reason)) {
            return ev;
        }
    }

    if (cfg.mode == Mode::Smart && !included && !everythingSelf) {
        if (IsInSystemDir(ev.path) && !MatchesAny(kSystemDirAllow, ev.path)) {
            ev.reason = L"Windows system program";
            return ev;
        }
        if (cfg.skipGames && MatchesAny(kGameExcludes, ev.path)) {
            ev.reason = L"game / anti-cheat list";
            return ev;
        }
        if (!hasWindow) {
            ev.verdict = Verdict::Wait;
            ev.reason = L"no visible window yet";
            return ev;
        }
        if (cfg.trigger == Trigger::Shell &&
            !ProcessHasModule(process, L"windows.storage.dll")) {
            ev.verdict = Verdict::Wait;
            ev.reason = L"windows.storage.dll not loaded yet";
            return ev;
        }
    }

    ev.verdict = Verdict::Inject;
    ev.reason = included ? L"[Process] include" : L"ok";
    return ev;
}

}  // namespace bfs
