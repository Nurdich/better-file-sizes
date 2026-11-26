// GNU GPL v3. See settings.h.
#include "settings.h"

#include "../common/bfs_common.h"
#include "../host/readiness.h"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>

#include <memory>
#include <thread>

namespace bfs {

namespace {

constexpr UINT kMsgProbe = WM_APP + 1;  // wParam = probe result code

struct Choice {
    const wchar_t* label;
    const wchar_t* value;
};

const Choice kFolderChoices[] = {
    {L"关闭", L"disabled"},
    {L"通过 Everything（推荐）", L"everything"},
    {L"手动计算（可能较慢）", L"always"},
    {L"按住 Shift 时手动计算", L"withShiftKey"},
};
const Choice kUnitChoices[] = {
    {L"Windows 默认", L"0"},
    {L"大文件显示 MB/GB", L"1"},
    {L"始终显示 KB", L"alwaysKb"},
};
const Choice kModeChoices[] = {
    {L"智能（推荐）：有窗口的普通程序", L"smart"},
    {L"所有能注入的程序", L"all"},
    {L"只注入白名单（include）", L"whitelist"},
};

// Defaults are the same as the DLL's / host's defaults.
constexpr wchar_t kDefFolder[] = L"everything";
constexpr wchar_t kDefUnits[] = L"1";
constexpr wchar_t kDefMode[] = L"smart";

struct Values {
    std::wstring folder, units, mode;
    bool iec = false, mix = true, waitEv = true, autostart = false;
};

struct State {
    HINSTANCE instance = nullptr;
    SettingsHooks hooks;
    Values loaded;
    std::wstring effectiveIni;
    bool modal = false;
    HFONT font = nullptr;
    HICON iconBig = nullptr, iconSmall = nullptr;
    unsigned probeSeq = 0;
};

HWND g_dlg;

bool IsWritableFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

bool IsWritableDir(const std::wstring& dir) {
    wchar_t tmp[MAX_PATH];
    if (!GetTempFileNameW(dir.c_str(), L"bfs", 0, tmp)) return false;
    DeleteFileW(tmp);
    return true;
}

bool CanWrite(const std::wstring& ini) {
    return FileExists(ini) ? IsWritableFile(ini) : IsWritableDir(DirName(ini));
}

std::wstring OverrideIni() {
    std::wstring user = UserDataDir();
    return user.empty() ? std::wstring() : user + L"\\" + kIniName;
}

// Makes sure the target INI exists and is writable. If the INI next to the
// DLL can't be written (e.g. Program Files), the full file (comments and all
// other keys) is copied to %LOCALAPPDATA%\BetterFileSizes first.
std::wstring PrepareTargetIni(const std::wstring& dllDir, std::wstring* err) {
    std::wstring eff = ResolveIniPath(dllDir);
    if (CanWrite(eff)) return eff;
    std::wstring ov = OverrideIni();
    if (ov.empty()) {
        *err = L"无法写入 " + eff + L"，并且找不到 %LOCALAPPDATA%。";
        return L"";
    }
    CreateDirectoryW(DirName(ov).c_str(), nullptr);
    if (!FileExists(ov) && FileExists(eff) &&
        !CopyFileW(eff.c_str(), ov.c_str(), TRUE)) {
        *err = L"无法复制设置文件到 " + ov;
        return L"";
    }
    if (FileExists(ov)) {
        // The copy keeps the read-only attribute of the original.
        DWORD a = GetFileAttributesW(ov.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
            SetFileAttributesW(ov.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
        }
    }
    if (!CanWrite(ov)) {
        *err = L"无法写入 " + ov;
        return L"";
    }
    return ov;
}

int FindChoice(const Choice* c, int n, const std::wstring& v, PCWSTR def) {
    for (int i = 0; i < n; i++) {
        if (_wcsicmp(c[i].value, v.c_str()) == 0) return i;
    }
    for (int i = 0; i < n; i++) {
        if (_wcsicmp(c[i].value, def) == 0) return i;
    }
    return 0;
}

void FillCombo(HWND dlg, int id, const Choice* c, int n, int sel) {
    HWND cb = GetDlgItem(dlg, id);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < n; i++) {
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)c[i].label);
    }
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

int ComboSel(HWND dlg, int id) {
    LRESULT r = SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0);
    return r < 0 ? 0 : (int)r;
}

Values ReadValues(const std::wstring& ini, const SettingsHooks& hooks) {
    Values v;
    v.folder = Trim(IniString(ini, L"Settings", L"calculateFolderSizes", kDefFolder));
    v.units = Trim(IniString(ini, L"Settings", L"disableKbOnlySizes", kDefUnits));
    v.iec = GetPrivateProfileIntW(L"Settings", L"useIecTerms", 0, ini.c_str()) != 0;
    v.mix = GetPrivateProfileIntW(L"Settings", L"sortSizesMixFolders", 1,
                                  ini.c_str()) != 0;
    v.mode = Trim(IniString(ini, L"Process", L"mode", kDefMode));
    v.waitEv = GetPrivateProfileIntW(L"Host", L"waitForEverything", 1,
                                     ini.c_str()) != 0;
    v.autostart = hooks.getAutostart ? hooks.getAutostart() : false;
    // Normalize to the combo values (unknown values show the default).
    v.folder = kFolderChoices[FindChoice(kFolderChoices, 4, v.folder, kDefFolder)].value;
    v.units = kUnitChoices[FindChoice(kUnitChoices, 3, v.units, kDefUnits)].value;
    v.mode = kModeChoices[FindChoice(kModeChoices, 3, v.mode, kDefMode)].value;
    return v;
}

Values CollectValues(HWND dlg) {
    Values v;
    v.folder = kFolderChoices[ComboSel(dlg, IDC_FOLDER)].value;
    v.units = kUnitChoices[ComboSel(dlg, IDC_UNITS)].value;
    v.mode = kModeChoices[ComboSel(dlg, IDC_MODE)].value;
    v.iec = IsDlgButtonChecked(dlg, IDC_IEC) == BST_CHECKED;
    v.mix = IsDlgButtonChecked(dlg, IDC_MIX) == BST_CHECKED;
    v.waitEv = IsDlgButtonChecked(dlg, IDC_WAITEV) == BST_CHECKED;
    v.autostart = IsDlgButtonChecked(dlg, IDC_AUTOSTART) == BST_CHECKED;
    return v;
}

void SetCheck(HWND dlg, int id, bool on) {
    CheckDlgButton(dlg, id, on ? BST_CHECKED : BST_UNCHECKED);
}

// ---- Everything hint (never on the UI thread: IPC may take ~2 s) --------

enum ProbeResult : WPARAM {
    kProbeNotRunning = 1,
    kProbeNoIpc,
    kProbeDbLoading,
    kProbeNoFolderIndex,
    kProbeOk,
};

void StartProbe(HWND dlg, State* st) {
    unsigned seq = ++st->probeSeq;
    SetDlgItemTextW(dlg, IDC_HINT, L"正在检查 Everything…");
    std::thread([dlg, seq] {
        EverythingProbe p = ProbeEverything();
        WPARAM r = !p.running              ? kProbeNotRunning
                   : !p.ipcOk              ? kProbeNoIpc
                   : !p.dbLoaded           ? kProbeDbLoading
                   : p.folderSizeIndexed == 0 ? kProbeNoFolderIndex
                                              : kProbeOk;
        // If the dialog is gone the message is simply dropped.
        PostMessageW(dlg, kMsgProbe, r, (LPARAM)seq);
    }).detach();
}

void UpdateHint(HWND dlg, State* st) {
    if (_wcsicmp(kFolderChoices[ComboSel(dlg, IDC_FOLDER)].value,
                 L"everything") == 0) {
        StartProbe(dlg, st);
    } else {
        ++st->probeSeq;
        SetDlgItemTextW(dlg, IDC_HINT, L"");
    }
}

const wchar_t* ProbeText(WPARAM r) {
    switch (r) {
        case kProbeNotRunning:
            return L"注意：没有检测到正在运行的 Everything。可以先保存，"
                   L"Everything 启动并就绪后才会注入。";
        case kProbeNoIpc:
            return L"注意：无法查询 Everything（可能以管理员身份运行）。";
        case kProbeDbLoading:
            return L"Everything 数据库正在加载…";
        case kProbeNoFolderIndex:
            return L"注意：Everything 未开启“索引文件夹大小”：工具 → 选项 → "
                   L"索引 里勾选后才会显示文件夹大小。";
        case kProbeOk:
            return L"Everything 已就绪，已开启文件夹大小索引。";
    }
    return L"";
}

// ---- saving --------------------------------------------------------------

// Minimal INI line editor. WritePrivateProfileString keeps comments on
// Windows, but other implementations (e.g. Wine) rewrite the whole file, and
// it can't tell UTF-8 comments from ANSI. So we change exactly one line:
// the file is read as bytes (each byte mapped 1:1 to a wchar_t, so UTF-8 /
// GBK comments round-trip untouched) or as UTF-16 if it has a BOM.
struct IniText {
    std::wstring text;
    bool utf16 = false;
};

bool LoadIniText(const std::wstring& path, IniText* out) {
    out->text.clear();
    out->utf16 = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    std::string bytes;
    char buf[8192];
    DWORD n;
    while (ReadFile(f, buf, sizeof(buf), &n, nullptr) && n) bytes.append(buf, n);
    CloseHandle(f);
    if (bytes.size() >= 2 && (unsigned char)bytes[0] == 0xFF &&
        (unsigned char)bytes[1] == 0xFE) {
        out->utf16 = true;
        out->text.assign((const wchar_t*)(bytes.data() + 2),
                         (bytes.size() - 2) / sizeof(wchar_t));
    } else {
        out->text.reserve(bytes.size());
        for (char c : bytes) out->text.push_back((wchar_t)(unsigned char)c);
    }
    return true;
}

bool SaveIniText(const std::wstring& path, const IniText& in) {
    std::string bytes;
    if (in.utf16) {
        bytes = "\xFF\xFE";
        bytes.append((const char*)in.text.data(), in.text.size() * sizeof(wchar_t));
    } else {
        bytes.reserve(in.text.size());
        for (wchar_t c : in.text) bytes.push_back((char)(unsigned char)c);
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    bool ok = WriteFile(f, bytes.data(), (DWORD)bytes.size(), &n, nullptr) &&
              n == bytes.size();
    CloseHandle(f);
    return ok;
}

std::wstring TrimWs(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r");
    return s.substr(a, b - a + 1);
}

// Sets key=value in [section]; everything else stays byte-identical.
void SetIniValue(IniText* ini, PCWSTR section, PCWSTR key,
                 const std::wstring& value) {
    std::wstring& t = ini->text;
    const std::wstring nl = t.find(L"\r\n") != std::wstring::npos ? L"\r\n" : L"\n";
    bool inSection = false;
    size_t insertAt = std::wstring::npos;  // after the section's last entry
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t eol = t.find(L'\n', pos);
        size_t lineEnd = eol == std::wstring::npos ? t.size() : eol;
        size_t next = eol == std::wstring::npos ? t.size() + 1 : eol + 1;
        std::wstring line = t.substr(pos, lineEnd - pos);
        std::wstring tl = TrimWs(line);
        if (!tl.empty() && tl[0] == L'[') {
            size_t close = tl.find(L']');
            std::wstring name = TrimWs(tl.substr(1, close == std::wstring::npos
                                                        ? std::wstring::npos
                                                        : close - 1));
            if (inSection) break;  // section ended without the key
            inSection = _wcsicmp(name.c_str(), section) == 0;
            if (inSection) insertAt = next;
        } else if (inSection && !tl.empty() && tl[0] != L';' && tl[0] != L'#') {
            size_t eq = line.find(L'=');
            if (eq != std::wstring::npos &&
                _wcsicmp(TrimWs(line.substr(0, eq)).c_str(), key) == 0) {
                // Keep "key=" (and its spacing), replace the rest of the line.
                size_t vstart = pos + eq + 1;  // absolute indexes
                size_t vend = lineEnd;
                while (vstart < vend && (t[vstart] == L' ' || t[vstart] == L'\t'))
                    vstart++;  // keep "key = " spacing
                if (vend > vstart && t[vend - 1] == L'\r') vend--;
                t.replace(vstart, vend - vstart, value);
                return;
            }
            insertAt = next;
        }
        pos = next;
    }
    std::wstring entry = std::wstring(key) + L"=" + value + nl;
    if (insertAt != std::wstring::npos) {
        if (insertAt > t.size()) {  // section's last entry is the last line
            t += nl;
            insertAt = t.size();
        }
        t.insert(insertAt, entry);
        return;
    }
    if (!t.empty() && t.back() != L'\n') t += nl;
    t += nl + L"[" + section + L"]" + nl + entry;
}

bool Save(HWND dlg, State* st) {
    Values v = CollectValues(dlg);
    const Values& o = st->loaded;
    bool iniChanged = v.folder != o.folder || v.units != o.units ||
                      v.iec != o.iec || v.mix != o.mix || v.mode != o.mode ||
                      v.waitEv != o.waitEv;
    std::wstring err;
    if (iniChanged) {
        std::wstring ini = PrepareTargetIni(st->hooks.dllDir, &err);
        if (ini.empty()) {
            MessageBoxW(dlg, err.c_str(), L"Better File Sizes", MB_ICONERROR);
            return false;
        }
        IniText text;
        if (!LoadIniText(ini, &text)) {
            MessageBoxW(dlg, (L"无法读取 " + ini).c_str(), L"Better File Sizes",
                        MB_ICONERROR);
            return false;
        }
        // Only the keys that changed; comments and other keys stay as they are.
        if (v.folder != o.folder)
            SetIniValue(&text, L"Settings", L"calculateFolderSizes", v.folder);
        if (v.units != o.units)
            SetIniValue(&text, L"Settings", L"disableKbOnlySizes", v.units);
        if (v.iec != o.iec)
            SetIniValue(&text, L"Settings", L"useIecTerms", v.iec ? L"1" : L"0");
        if (v.mix != o.mix)
            SetIniValue(&text, L"Settings", L"sortSizesMixFolders", v.mix ? L"1" : L"0");
        if (v.mode != o.mode)
            SetIniValue(&text, L"Process", L"mode", v.mode);
        if (v.waitEv != o.waitEv)
            SetIniValue(&text, L"Host", L"waitForEverything", v.waitEv ? L"1" : L"0");
        if (!SaveIniText(ini, text)) {
            MessageBoxW(dlg, (L"无法写入 " + ini + L"（错误 " +
                              std::to_wstring(GetLastError()) + L"）").c_str(),
                        L"Better File Sizes", MB_ICONERROR);
            return false;
        }
        // The running host watches its INI and reloads ~1 s after a save.
        // Only if it is using another file (we just created the
        // %LOCALAPPDATA% copy, whose folder it may not be watching) do we ask
        // it to reload explicitly - never both.
        if (st->hooks.hostIniPath && st->hooks.reloadHost) {
            std::wstring used = st->hooks.hostIniPath();
            if (_wcsicmp(used.c_str(), ini.c_str()) != 0) st->hooks.reloadHost();
        }
    }
    if (v.autostart != o.autostart && st->hooks.setAutostart) {
        st->hooks.setAutostart(v.autostart);
    }
    return true;
}

// ---- dialog --------------------------------------------------------------

void InitDialog(HWND dlg, State* st) {
    SetWindowTextW(dlg, L"Better File Sizes 设置");
    SetDlgItemTextW(dlg, IDC_GRP_DISPLAY, L"显示");
    SetDlgItemTextW(dlg, IDC_LBL_FOLDER, L"显示文件夹大小：");
    SetDlgItemTextW(dlg, IDC_LBL_UNITS, L"文件大小单位：");
    SetDlgItemTextW(dlg, IDC_IEC, L"使用 IEC 单位（KiB/MiB/GiB）");
    SetDlgItemTextW(dlg, IDC_MIX, L"按大小排序时文件和文件夹混排");
    SetDlgItemTextW(dlg, IDC_GRP_INJECT, L"注入与启动");
    SetDlgItemTextW(dlg, IDC_LBL_MODE, L"注入范围：");
    SetDlgItemTextW(dlg, IDC_WAITEV, L"等待 Everything 就绪后再注入");
    SetDlgItemTextW(dlg, IDC_AUTOSTART, L"开机自启（托盘守护程序）");
    SetDlgItemTextW(dlg, IDC_LBL_INI, L"设置文件：");
    SetDlgItemTextW(dlg, IDC_OPENINI, L"打开 INI 文件…");
    SetDlgItemTextW(dlg, IDOK, L"保存");
    SetDlgItemTextW(dlg, IDCANCEL, L"取消");

    st->effectiveIni = ResolveIniPath(st->hooks.dllDir);
    st->loaded = ReadValues(st->effectiveIni, st->hooks);
    const Values& v = st->loaded;
    FillCombo(dlg, IDC_FOLDER, kFolderChoices, 4,
              FindChoice(kFolderChoices, 4, v.folder, kDefFolder));
    FillCombo(dlg, IDC_UNITS, kUnitChoices, 3,
              FindChoice(kUnitChoices, 3, v.units, kDefUnits));
    FillCombo(dlg, IDC_MODE, kModeChoices, 3,
              FindChoice(kModeChoices, 3, v.mode, kDefMode));
    SetCheck(dlg, IDC_IEC, v.iec);
    SetCheck(dlg, IDC_MIX, v.mix);
    SetCheck(dlg, IDC_WAITEV, v.waitEv);
    SetCheck(dlg, IDC_AUTOSTART, v.autostart);
    if (!st->hooks.setAutostart) EnableWindow(GetDlgItem(dlg, IDC_AUTOSTART), FALSE);

    std::wstring pathText = st->effectiveIni;
    if (!CanWrite(st->effectiveIni)) {
        std::wstring ov = OverrideIni();
        if (!ov.empty()) pathText += L"（只读，保存时会复制到 " + ov + L"）";
    }
    SetDlgItemTextW(dlg, IDC_INIPATH, pathText.c_str());

    st->iconBig = (HICON)LoadImageW(st->instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                    GetSystemMetrics(SM_CXICON),
                                    GetSystemMetrics(SM_CYICON), 0);
    st->iconSmall = (HICON)LoadImageW(st->instance, MAKEINTRESOURCEW(1),
                                      IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                      GetSystemMetrics(SM_CYSMICON), 0);
    if (st->iconBig) SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)st->iconBig);
    if (st->iconSmall) SendMessageW(dlg, WM_SETICON, ICON_SMALL, (LPARAM)st->iconSmall);
    UpdateHint(dlg, st);
}

void OpenIniFile(HWND dlg, State* st) {
    std::wstring ini = ResolveIniPath(st->hooks.dllDir);
    if (!FileExists(ini)) {
        MessageBoxW(dlg, (L"找不到设置文件：\n" + ini).c_str(),
                    L"Better File Sizes", MB_ICONWARNING);
        return;
    }
    if ((INT_PTR)ShellExecuteW(dlg, L"open", ini.c_str(), nullptr, nullptr,
                               SW_SHOWNORMAL) <= 32) {
        ShellExecuteW(dlg, nullptr, L"notepad.exe", (L"\"" + ini + L"\"").c_str(),
                      nullptr, SW_SHOWNORMAL);
    }
}

void Close(HWND dlg, State* st) {
    if (st->modal) {
        EndDialog(dlg, 0);
    } else {
        DestroyWindow(dlg);
    }
}

INT_PTR CALLBACK DlgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = (State*)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
        case WM_INITDIALOG:
            st = (State*)lp;
            SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)st);
            g_dlg = dlg;
            InitDialog(dlg, st);
            return TRUE;
        case kMsgProbe:
            if (st && (unsigned)lp == st->probeSeq) {
                SetDlgItemTextW(dlg, IDC_HINT, ProbeText(wp));
            }
            return TRUE;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case IDC_FOLDER:
                    if (HIWORD(wp) == CBN_SELCHANGE) UpdateHint(dlg, st);
                    return TRUE;
                case IDC_OPENINI:
                    OpenIniFile(dlg, st);
                    return TRUE;
                case IDOK:
                    if (Save(dlg, st)) Close(dlg, st);
                    return TRUE;
                case IDCANCEL:
                    Close(dlg, st);
                    return TRUE;
            }
            break;
        case WM_CLOSE:
            Close(dlg, st);
            return TRUE;
        case WM_DESTROY:
            if (g_dlg == dlg) g_dlg = nullptr;
            return TRUE;
        case WM_NCDESTROY:
            if (st) {
                if (st->iconBig) DestroyIcon(st->iconBig);
                if (st->iconSmall) DestroyIcon(st->iconSmall);
                delete st;
                SetWindowLongPtrW(dlg, DWLP_USER, 0);
            }
            return FALSE;
    }
    return FALSE;
}

}  // namespace

std::wstring SettingsTargetIni(const std::wstring& dllDir) {
    std::wstring eff = ResolveIniPath(dllDir);
    if (CanWrite(eff)) return eff;
    std::wstring ov = OverrideIni();
    return ov.empty() ? eff : ov;
}

bool SettingsDialogMessage(MSG* msg) {
    return g_dlg && IsDialogMessageW(g_dlg, msg);
}

void ShowSettings(HINSTANCE instance, const SettingsHooks& hooks, bool modal) {
    if (g_dlg) {
        // Single instance: bring the open one to the front.
        if (IsIconic(g_dlg)) ShowWindow(g_dlg, SW_RESTORE);
        SetForegroundWindow(g_dlg);
        return;
    }
    static bool initCc = [] {
        INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES};
        return InitCommonControlsEx(&icc) != FALSE;
    }();
    (void)initCc;
    auto* st = new State;
    st->instance = instance;
    st->hooks = hooks;
    st->modal = modal;
    if (modal) {
        DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr,
                        DlgProc, (LPARAM)st);
        return;
    }
    HWND dlg = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS),
                                  nullptr, DlgProc, (LPARAM)st);
    if (!dlg) {
        delete st;
        return;
    }
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
}

}  // namespace bfs
