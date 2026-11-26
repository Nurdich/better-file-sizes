// bfs-daemon.exe: tray daemon that keeps BetterFileSizes.dll injected into
// interactive programs (see host/host.h). Fallback for users without the
// Everything 1.5 plugin.
//
//   bfs-daemon.exe          start (tray icon)
//   bfs-daemon.exe --quit   ask a running daemon to exit (hooks stay active
//                           unless [Host] disableOnExit=1)
//   bfs-daemon.exe --settings  open the settings window (in the running
//                           daemon if there is one, else on its own)
//
// GNU GPL v3. Source is UTF-8; UI strings are wide literals.

#include "../host/host.h"
#include "settings.h"

#include <shellapi.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>

namespace {

constexpr UINT kTrayMsg = WM_APP + 100;
constexpr UINT kStateMsg = WM_APP + 101;
constexpr UINT kStoppedMsg = WM_APP + 102;
constexpr UINT kOpenSettingsMsg = WM_APP + 103;
constexpr UINT_PTR kExitTimer = 1;
constexpr UINT_PTR kClickTimer = 2;  // single click = menu, double = settings
constexpr DWORD kExitHardLimitMs = 8000;
constexpr UINT kTrayId = 1;

enum MenuId : UINT {
    kMenuStatus = 100,
    kMenuSettings,
    kMenuPause,
    kMenuReload,
    kMenuOpenIni,
    kMenuAutostart,
    kMenuExit,
    kMenuDisableAndExit,
};

const std::wstring kWindowClass =
    std::wstring(L"BetterFileSizesDaemon.") + bfs::kArch;
const std::wstring kRunValue =
    std::wstring(L"BetterFileSizes Daemon (") + bfs::kArch + L")";
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

HINSTANCE g_instance;
HWND g_hwnd;
UINT g_taskbarCreatedMsg;
bfs::Host g_host;
std::wstring g_exePath;
std::wstring g_dllPath;
std::wstring g_logFile;
CRITICAL_SECTION g_logLock;

void Log(const std::wstring& msg) {
    std::wstring line =
        L"[bfs-daemon " + std::wstring(bfs::kArch) + L"] " + msg + L"\n";
    OutputDebugStringW(line.c_str());
    if (g_logFile.empty()) return;
    EnterCriticalSection(&g_logLock);
    HANDLE f = CreateFileW(g_logFile.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                                    nullptr, 0, nullptr, nullptr);
        std::string utf8(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                            utf8.data(), n, nullptr, nullptr);
        DWORD written;
        WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        CloseHandle(f);
    }
    LeaveCriticalSection(&g_logLock);
}

bool IsAutostartEnabled() {
    wchar_t value[MAX_PATH * 2];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue.c_str(),
                     RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) {
        return false;
    }
    return StrStrIW(value, g_exePath.c_str()) != nullptr;
}

void SetAutostart(bool enable) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (enable) {
        std::wstring cmd = L"\"" + g_exePath + L"\"";
        RegSetValueExW(key, kRunValue.c_str(), 0, REG_SZ, (const BYTE*)cmd.c_str(),
                       (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue.c_str());
    }
    RegCloseKey(key);
}

void UpdateTray(DWORD action) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = g_hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = (HICON)LoadImageW(g_instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON),
                                  GetSystemMetrics(SM_CYSMICON), 0);
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    std::wstring tip = std::wstring(L"Better File Sizes (") + bfs::kArch +
                       L") - " + g_host.StatusText();
    lstrcpynW(nid.szTip, tip.c_str(), ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(action, &nid);
    if (action == NIM_ADD) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
    DestroyIcon(nid.hIcon);
}

void OpenIni() {
    std::wstring ini = g_host.IniPath();
    if (!bfs::FileExists(ini)) {
        MessageBoxW(g_hwnd, (L"找不到设置文件：\n" + ini).c_str(),
                    L"Better File Sizes", MB_ICONWARNING);
        return;
    }
    if ((INT_PTR)ShellExecuteW(g_hwnd, L"open", ini.c_str(), nullptr, nullptr,
                               SW_SHOWNORMAL) <= 32) {
        ShellExecuteW(g_hwnd, nullptr, L"notepad.exe",
                      (L"\"" + ini + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    }
}

bool g_exiting;
ULONGLONG g_lastDblClick;

void OpenSettings() {
    bfs::SettingsHooks hooks;
    hooks.dllDir = bfs::DirName(g_dllPath);
    hooks.getAutostart = [] { return IsAutostartEnabled(); };
    hooks.setAutostart = [](bool on) { SetAutostart(on); };
    if (g_hwnd) {
        hooks.hostIniPath = [] { return g_host.IniPath(); };
        hooks.reloadHost = [] { g_host.Reload(); };
    }
    bfs::ShowSettings(g_instance, hooks, /*modal=*/g_hwnd == nullptr);
}

bool g_disableOnExit;

DWORD WINAPI StopThread(LPVOID) {
    // Never on the UI thread: the window keeps pumping messages meanwhile.
    g_host.Stop(kExitHardLimitMs - 1000, g_disableOnExit);
    PostMessageW(g_hwnd, kStoppedMsg, 0, 0);
    return 0;
}

// Exit without ever blocking the UI thread. disableEverywhere=true turns
// the hooks off in all processes first (bounded).
void BeginExit(bool disableEverywhere) {
    if (g_exiting) return;
    g_exiting = true;
    UpdateTray(NIM_DELETE);
    g_disableOnExit = disableEverywhere;
    // Hard limit: whatever happens, the daemon is gone after a few seconds.
    SetTimer(g_hwnd, kExitTimer, kExitHardLimitMs, nullptr);
    HANDLE t = CreateThread(nullptr, 0, StopThread, nullptr, 0, nullptr);
    if (t) {
        CloseHandle(t);
    } else {
        DestroyWindow(g_hwnd);
    }
}

void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    bool paused = g_host.Paused();
    std::wstring status = L"状态：" + g_host.StatusText();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuStatus, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"设置…");
    SetMenuDefaultItem(menu, kMenuSettings, FALSE);  // bold: double-click
    AppendMenuW(menu, MF_STRING, kMenuPause, paused ? L"恢复" : L"暂停");
    AppendMenuW(menu, MF_STRING, kMenuReload, L"重新加载设置");
    AppendMenuW(menu, MF_STRING, kMenuOpenIni, L"打开设置文件");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0),
                kMenuAutostart, L"开机自启");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出（已生效的程序保持不变）");
    AppendMenuW(menu, MF_STRING, kMenuDisableAndExit, L"在所有程序里停用后退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                              pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
        case kMenuSettings:
            OpenSettings();
            break;
        case kMenuPause:
            g_host.SetPaused(!paused);
            break;
        case kMenuReload:
            g_host.Reload();
            break;
        case kMenuOpenIni:
            OpenIni();
            break;
        case kMenuAutostart:
            SetAutostart(!IsAutostartEnabled());
            break;
        case kMenuExit:
            BeginExit(false);
            break;
        case kMenuDisableAndExit:
            BeginExit(true);
            break;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == g_taskbarCreatedMsg && g_taskbarCreatedMsg) {
        // explorer.exe restarted: re-add the icon and re-inject right away.
        UpdateTray(NIM_ADD);
        g_host.Rescan();
        return 0;
    }
    switch (msg) {
        case kTrayMsg:
            switch (LOWORD(lParam)) {
                case WM_CONTEXTMENU:
                case NIN_KEYSELECT:
                    KillTimer(hwnd, kClickTimer);
                    ShowMenu();
                    break;
                case NIN_SELECT:
                    // Left click: wait for a possible double-click first.
                    if (GetTickCount64() - g_lastDblClick < GetDoubleClickTime()) {
                        break;  // the click that belongs to a double-click
                    }
                    SetTimer(hwnd, kClickTimer, GetDoubleClickTime(), nullptr);
                    break;
                case WM_LBUTTONDBLCLK:
                    KillTimer(hwnd, kClickTimer);
                    g_lastDblClick = GetTickCount64();
                    OpenSettings();
                    break;
            }
            return 0;
        case kOpenSettingsMsg:
            OpenSettings();
            return 0;
        case kStateMsg:
            UpdateTray(NIM_MODIFY);
            return 0;
        case kStoppedMsg:
            DestroyWindow(hwnd);
            return 0;
        case WM_TIMER:
            if (wParam == kClickTimer) {
                KillTimer(hwnd, kClickTimer);
                ShowMenu();
                return 0;
            }
            if (wParam == kExitTimer) {
                // Stop didn't finish in time: just leave. Nothing we do here
                // can affect other processes.
                ExitProcess(0);
            }
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam) {
                // Logging off / shutting down: do nothing, just go away.
                ExitProcess(0);
            }
            return 0;
        case WM_CLOSE:
            // "bfs-daemon --quit" or someone closing us: normal exit.
            BeginExit(false);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR cmdLine, int) {
    g_instance = instance;
    InitializeCriticalSection(&g_logLock);

    if (cmdLine && (wcsstr(cmdLine, L"--quit") || wcsstr(cmdLine, L"/quit"))) {
        HWND other = FindWindowW(kWindowClass.c_str(), nullptr);
        if (other) PostMessageW(other, WM_CLOSE, 0, 0);
        return 0;
    }

    g_exePath = bfs::ModulePath(nullptr);
    g_dllPath = bfs::DirName(g_exePath) + L"\\" + bfs::kDllName;

    if (cmdLine && (wcsstr(cmdLine, L"--settings") || wcsstr(cmdLine, L"/settings"))) {
        HWND other = FindWindowW(kWindowClass.c_str(), nullptr);
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(other, kOpenSettingsMsg, 0, 0);
        } else {
            OpenSettings();  // no daemon running: modal, then exit
        }
        return 0;
    }

    std::wstring mutexName =
        std::wstring(L"Local\\BetterFileSizes.Daemon.") + bfs::kArch;
    HANDLE single = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Better File Sizes 守护程序已经在运行了（见托盘图标）。",
                    L"Better File Sizes", MB_ICONINFORMATION);
        return 0;
    }

    std::wstring dir = bfs::DirName(g_exePath);
    {
        std::wstring ini = bfs::ResolveIniPath(dir);
        std::wstring lf = bfs::Trim(bfs::IniString(ini, L"Debug", L"logFile", L""));
        if (!lf.empty() && lf.find(L':') == std::wstring::npos &&
            lf.rfind(L"\\\\", 0) != 0) {
            lf = dir + L"\\" + lf;
        }
        g_logFile = lf;
    }

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass.c_str();
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so that it receives
    // TaskbarCreated and WM_ENDSESSION broadcasts.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass.c_str(),
                             L"Better File Sizes", WS_POPUP, 0, 0, 0, 0, nullptr,
                             nullptr, instance, nullptr);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    // Allow the TaskbarCreated broadcast through UIPI if we run elevated.
    ChangeWindowMessageFilterEx(g_hwnd, g_taskbarCreatedMsg, MSGFLT_ALLOW,
                                nullptr);

    bfs::HostOptions opts;
    opts.dllPath = g_dllPath;
    opts.log = Log;
    opts.onState = [](bfs::HostState) {
        if (g_hwnd) PostMessageW(g_hwnd, kStateMsg, 0, 0);
    };
    std::wstring error;
    if (!g_host.Start(opts, &error)) {
        g_exiting = true;
        MessageBoxW(nullptr,
                    (L"无法启动：" + error +
                     L"\n\n请把 bfs-daemon.exe、BetterFileSizes.dll 和 "
                     L"BetterFileSizes.ini 放在同一个文件夹里。")
                        .c_str(),
                    L"Better File Sizes", MB_ICONERROR);
        DestroyWindow(g_hwnd);
        return 1;
    }
    UpdateTray(NIM_ADD);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (bfs::SettingsDialogMessage(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (single) CloseHandle(single);
    return 0;
}
