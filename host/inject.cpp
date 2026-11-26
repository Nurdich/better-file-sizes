// GNU GPL v3. See inject.h.
#include "inject.h"

#include <psapi.h>
#include <tlhelp32.h>
#include <winver.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace bfs {

namespace {

constexpr DWORD kLoadTimeoutMs = 5000;
constexpr DWORD kEnableTimeoutMs = 10000;
constexpr DWORD kDisableTimeoutMs = 8000;
constexpr DWORD kIsEnabledTimeoutMs = 2000;

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), towlower);
    return s;
}

std::wstring FileVersionString(const std::wstring& path) {
    DWORD unused = 0;
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (!size) return L"0.0.0.0";
    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return L"0.0.0.0";
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", (void**)&ffi, &len) || !ffi) {
        return L"0.0.0.0";
    }
    wchar_t out[64];
    swprintf(out, 64, L"%u.%u.%u.%u",
             HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
             HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    return out;
}

// Short content hash (FNV-1a 32-bit of the whole file), for the shadow name.
std::wstring FileHash8(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"00000000";
    DWORD hash = 2166136261u;
    BYTE buf[8192];
    DWORD n = 0;
    while (ReadFile(f, buf, sizeof(buf), &n, nullptr) && n) {
        for (DWORD i = 0; i < n; i++) {
            hash ^= buf[i];
            hash *= 16777619u;
        }
    }
    CloseHandle(f);
    wchar_t out[16];
    swprintf(out, 16, L"%08x", hash);
    return out;
}

bool WriteHomeSidecar(const std::wstring& shadow, const std::wstring& home) {
    std::wstring path = shadow + L".home";
    int len = WideCharToMultiByte(CP_UTF8, 0, home.c_str(), (int)home.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, home.c_str(), (int)home.size(), utf8.data(),
                        len, nullptr, nullptr);
    utf8.push_back('\n');
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(f);
    return ok != FALSE;
}

bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (CreateDirectoryW(dir.c_str(), nullptr) ||
        GetLastError() == ERROR_ALREADY_EXISTS) {
        return true;
    }
    // Parent may be missing (%LOCALAPPDATA%\BetterFileSizes).
    std::wstring parent = DirName(dir);
    if (parent != dir) EnsureDir(parent);
    return CreateDirectoryW(dir.c_str(), nullptr) ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

// Read an export's RVA from a remote module image (never from a file on disk:
// that file may have been replaced by a newer version).
DWORD RemoteExportRva(HANDLE process, HMODULE base, const char* name) {
    IMAGE_DOS_HEADER dos;
    SIZE_T got = 0;
    if (!ReadProcessMemory(process, base, &dos, sizeof(dos), &got) ||
        got < sizeof(dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    IMAGE_NT_HEADERS nt;
    if (!ReadProcessMemory(process, (BYTE*)base + dos.e_lfanew, &nt, sizeof(nt),
                           &got) ||
        got < sizeof(nt) || nt.Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    auto& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress || !dir.Size) return 0;
    IMAGE_EXPORT_DIRECTORY exp;
    if (!ReadProcessMemory(process, (BYTE*)base + dir.VirtualAddress, &exp,
                           sizeof(exp), &got) ||
        got < sizeof(exp) || !exp.NumberOfNames || !exp.NumberOfFunctions) {
        return 0;
    }
    std::vector<DWORD> names(exp.NumberOfNames);
    std::vector<WORD> ords(exp.NumberOfNames);
    std::vector<DWORD> funcs(exp.NumberOfFunctions);
    if (!ReadProcessMemory(process, (BYTE*)base + exp.AddressOfNames,
                           names.data(), names.size() * sizeof(DWORD), &got))
        return 0;
    if (!ReadProcessMemory(process, (BYTE*)base + exp.AddressOfNameOrdinals,
                           ords.data(), ords.size() * sizeof(WORD), &got))
        return 0;
    if (!ReadProcessMemory(process, (BYTE*)base + exp.AddressOfFunctions,
                           funcs.data(), funcs.size() * sizeof(DWORD), &got))
        return 0;
    char nbuf[64];
    for (DWORD i = 0; i < exp.NumberOfNames; i++) {
        SIZE_T ng = 0;
        if (!ReadProcessMemory(process, (BYTE*)base + names[i], nbuf,
                               sizeof(nbuf) - 1, &ng) ||
            !ng)
            continue;
        nbuf[ng] = 0;
        if (strcmp(nbuf, name) != 0) continue;
        if (ords[i] >= exp.NumberOfFunctions) return 0;
        return funcs[ords[i]];
    }
    return 0;
}

DWORD LocalExportRva(HMODULE base, const char* name) {
    FARPROC p = GetProcAddress(base, name);
    return p ? (DWORD)((BYTE*)p - (BYTE*)base) : 0;
}

void FillRvas(HANDLE process, bool self, Injector::Copy* c) {
    if (self) {
        c->rvaEnable = LocalExportRva(c->base, "BfsEnable");
        c->rvaDisable = LocalExportRva(c->base, "BfsDisable");
        c->rvaIsEnabled = LocalExportRva(c->base, "BfsIsEnabled");
    } else {
        c->rvaEnable = RemoteExportRva(process, c->base, "BfsEnable");
        c->rvaDisable = RemoteExportRva(process, c->base, "BfsDisable");
        c->rvaIsEnabled = RemoteExportRva(process, c->base, "BfsIsEnabled");
    }
}

}  // namespace

bool Injector::EnsureShadow(std::wstring* error) {
    std::wstring dir = ShadowDir();
    if (dir.empty()) {
        if (error) *error = L"%LOCALAPPDATA% not set";
        return false;
    }
    if (!EnsureDir(dir)) {
        if (error) *error = L"cannot create " + dir;
        return false;
    }
    version_ = FileVersionString(shipped_);
    hash_ = FileHash8(shipped_);
    std::wstring dest =
        dir + L"\\BetterFileSizes-" + version_ + L"-" + hash_ + L".dll";
    std::wstring home = DirName(shipped_);

    if (!FileExists(dest)) {
        if (!CopyFileW(shipped_.c_str(), dest.c_str(), TRUE)) {
            // Another host may have just created it.
            if (!FileExists(dest)) {
                if (error) {
                    *error = L"CopyFile to " + dest + L" failed " +
                             std::to_wstring(GetLastError());
                }
                return false;
            }
        }
    }
    // Always refresh the sidecar: the install dir may have moved.
    if (!WriteHomeSidecar(dest, home)) {
        if (error) *error = L"cannot write " + dest + L".home";
        return false;
    }
    dllPath_ = dest;
    return true;
}

bool Injector::Init(const std::wstring& shippedDllPath, std::wstring* error) {
    shipped_ = shippedDllPath;
    warning_.clear();
    if (!FileExists(shipped_)) {
        if (error) *error = shipped_ + L" not found";
        return false;
    }
    // Verify the shipped file has the exports we need (also exercises that
    // it is a real PE of our bitness).
    HMODULE probe =
        LoadLibraryExW(shipped_.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!probe) {
        if (error) {
            *error = L"cannot open " + shipped_ + L" (" +
                     std::to_wstring(GetLastError()) + L")";
        }
        return false;
    }
    bool ok = GetProcAddress(probe, "BfsEnable") &&
              GetProcAddress(probe, "BfsDisable");
    FreeLibrary(probe);
    if (!ok) {
        if (error) {
            *error = L"BfsEnable/BfsDisable exports not found in " + shipped_ +
                     L" (old DLL version?)";
        }
        return false;
    }

    std::wstring shadowErr;
    if (!EnsureShadow(&shadowErr)) {
        // Fall back to injecting the shipped file so the host still works;
        // the install dir will stay locked until those processes exit.
        dllPath_ = shipped_;
        version_ = FileVersionString(shipped_);
        hash_.clear();
        warning_ = L"shadow copy unavailable (" + shadowErr +
                   L"), injecting shipped DLL directly";
    }
    CleanupShadows(nullptr);
    return true;
}

void Injector::CleanupShadows(const LogFn& log) {
    std::wstring dir = ShadowDir();
    if (dir.empty()) return;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        if (!IsModDllName(name.c_str())) continue;
        // Keep our current shadow (and its .home).
        if (!dllPath_.empty() &&
            _wcsicmp(name.c_str(), BaseName(dllPath_).c_str()) == 0) {
            continue;
        }
        std::wstring path = dir + L"\\" + name;
        if (DeleteFileW(path.c_str())) {
            DeleteFileW((path + L".home").c_str());
            if (log) log(L"removed old shadow " + name);
        }
        // Locked by a process: leave it; next start will try again.
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

std::vector<Injector::Copy> Injector::FindCopies(HANDLE process) {
    std::vector<Copy> out;
    HMODULE modules[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(process, modules, sizeof(modules), &needed,
                              LIST_MODULES_DEFAULT)) {
        return out;
    }
    DWORD count = std::min<DWORD>(needed / sizeof(HMODULE), ARRAYSIZE(modules));
    wchar_t name[MAX_PATH];
    wchar_t full[MAX_PATH * 2];
    std::wstring want = Lower(dllPath_);
    for (DWORD i = 0; i < count; i++) {
        if (!GetModuleBaseNameW(process, modules[i], name, ARRAYSIZE(name))) {
            continue;
        }
        if (!IsModDllName(name)) continue;
        Copy c;
        c.base = modules[i];
        DWORD n = GetModuleFileNameExW(process, modules[i], full, ARRAYSIZE(full));
        c.path = std::wstring(full, n);
        c.current = !want.empty() && Lower(c.path) == want;
        out.push_back(std::move(c));
    }
    return out;
}

HANDLE Injector::StartAt(HANDLE process, DWORD pid, HMODULE base, DWORD rva) {
    if (!base || !rva) return nullptr;
    auto start = (LPTHREAD_START_ROUTINE)(void*)((BYTE*)base + rva);
    if (pid == GetCurrentProcessId()) {
        return CreateThread(nullptr, 0, start, nullptr, 0, nullptr);
    }
    return CreateRemoteThread(process, nullptr, 0, start, nullptr, 0, nullptr);
}

DWORD Injector::RunAt(HANDLE process, DWORD pid, HMODULE base, DWORD rva,
                      DWORD timeoutMs, DWORD fallback, bool* finished) {
    HANDLE thread = StartAt(process, pid, base, rva);
    if (!thread) {
        if (finished) *finished = false;
        return fallback;
    }
    DWORD wait = WaitForSingleObject(thread, timeoutMs);
    DWORD code = fallback;
    if (wait == WAIT_OBJECT_0) GetExitCodeThread(thread, &code);
    CloseHandle(thread);
    if (finished) *finished = wait == WAIT_OBJECT_0;
    return code;
}

Injector::Result Injector::Inject(DWORD pid, std::wstring* error,
                                  const LogFn& log) {
    if (GetSystemMetrics(SM_SHUTTINGDOWN)) {
        if (error) *error = L"system is shutting down";
        return Result::Failed;
    }
    bool self = pid == GetCurrentProcessId();
    HANDLE process = nullptr;
    if (self) {
        process = GetCurrentProcess();
    } else {
        process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                  PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                  PROCESS_VM_READ,
                              FALSE, pid);
        if (!process) {
            if (error) {
                *error = L"OpenProcess failed " + std::to_wstring(GetLastError());
            }
            return Result::Failed;
        }
    }

    auto close = [&] {
        if (!self && process) CloseHandle(process);
    };

    std::vector<Copy> copies = FindCopies(process);
    Copy* current = nullptr;
    for (auto& c : copies) {
        FillRvas(process, self, &c);
        if (c.current) current = &c;
    }

    // Turn every other (older) copy's hooks off. MinHook is per-module, so
    // once those hooks are removed the new copy can install its own safely.
    for (auto& c : copies) {
        if (c.current || !c.rvaDisable) continue;
        bool done = false;
        DWORD code =
            RunAt(process, pid, c.base, c.rvaDisable, kDisableTimeoutMs, 1, &done);
        if (log) {
            log((done ? L"disabled old copy: " : L"disable old copy timed out: ") +
                BaseName(c.path) + L" (code " + std::to_wstring(code) + L")");
        }
    }

    if (current) {
        // Our shadow is already mapped (maybe disabled): re-enable it.
        if (!current->rvaEnable) {
            if (error) *error = L"BfsEnable not found in loaded copy";
            close();
            return Result::AlreadyActive;
        }
        bool done = false;
        DWORD code = RunAt(process, pid, current->base, current->rvaEnable,
                           kEnableTimeoutMs, 1, &done);
        close();
        if (!done) {
            if (error) *error = L"BfsEnable still running (timeout)";
            return Result::Failed;
        }
        return code == 0 ? Result::Enabled : Result::Failed;
    }

    // Load our shadow (or the shipped file if shadowing failed).
    if (self) {
        HMODULE m = LoadLibraryW(dllPath_.c_str());
        close();
        if (m) return Result::Loaded;
        if (error) {
            *error = L"LoadLibrary failed " + std::to_wstring(GetLastError());
        }
        return Result::Failed;
    }

    Result result = Result::Failed;
    size_t bytes = (dllPath_.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                  PAGE_READWRITE);
    if (remote &&
        WriteProcessMemory(process, remote, dllPath_.c_str(), bytes, nullptr)) {
        auto loadLibrary = (LPTHREAD_START_ROUTINE)(void*)GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary,
                                           remote, 0, nullptr);
        if (thread) {
            DWORD wait = WaitForSingleObject(thread, kLoadTimeoutMs);
            DWORD code = 0;
            GetExitCodeThread(thread, &code);
            CloseHandle(thread);
            if (wait == WAIT_OBJECT_0 && code != 0) {
                result = Result::Loaded;
            } else if (error) {
                *error = wait == WAIT_OBJECT_0 ? L"LoadLibraryW returned 0"
                                               : L"timeout (process hung?)";
            }
            if (wait != WAIT_OBJECT_0) remote = nullptr;  // still in use
        } else if (error) {
            *error = L"CreateRemoteThread failed " +
                     std::to_wstring(GetLastError());
        }
    } else if (error) {
        *error = L"VirtualAllocEx/WriteProcessMemory failed " +
                 std::to_wstring(GetLastError());
    }
    if (remote) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    close();
    return result;
}

std::map<DWORD, std::pair<std::wstring, std::vector<Injector::Copy>>>
Injector::ListLoaded(bool queryState) {
    std::map<DWORD, std::pair<std::wstring, std::vector<Copy>>> result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W pe = {sizeof(pe)};
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID <= 4) continue;
            bool self = pe.th32ProcessID == GetCurrentProcessId();
            HANDLE process =
                self ? GetCurrentProcess()
                     : OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ |
                                       (queryState ? PROCESS_CREATE_THREAD |
                                                         PROCESS_VM_OPERATION |
                                                         PROCESS_VM_WRITE
                                                   : 0),
                                   FALSE, pe.th32ProcessID);
            if (!process && !self) continue;
            auto copies = FindCopies(self ? GetCurrentProcess() : process);
            if (!copies.empty()) {
                for (auto& c : copies) {
                    FillRvas(self ? GetCurrentProcess() : process, self, &c);
                    if (queryState && c.rvaIsEnabled) {
                        c.enabled = (int)RunAt(
                            self ? GetCurrentProcess() : process,
                            pe.th32ProcessID, c.base, c.rvaIsEnabled,
                            kIsEnabledTimeoutMs, (DWORD)-1, nullptr);
                    }
                }
                result[pe.th32ProcessID] = {pe.szExeFile, std::move(copies)};
            }
            if (!self && process) CloseHandle(process);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return result;
}

size_t Injector::DisableAll(const LogFn& log, DWORD timeoutMs) {
    struct Pending {
        DWORD pid;
        std::wstring name;
        std::wstring path;
        HANDLE thread;
    };
    std::vector<Pending> pending;
    for (auto& [pid, info] : ListLoaded(false)) {
        bool self = pid == GetCurrentProcessId();
        HANDLE process =
            self ? GetCurrentProcess()
                 : OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                   PROCESS_VM_OPERATION | PROCESS_VM_READ |
                                   PROCESS_VM_WRITE,
                               FALSE, pid);
        if (!process && !self) {
            if (log) {
                log(L"disable failed: " + info.first + L" (" +
                    std::to_wstring(pid) + L")");
            }
            continue;
        }
        for (auto& c : info.second) {
            FillRvas(self ? GetCurrentProcess() : process, self, &c);
            HANDLE thread =
                StartAt(self ? GetCurrentProcess() : process, pid, c.base,
                        c.rvaDisable);
            if (thread) {
                pending.push_back({pid, info.first, c.path, thread});
            } else if (log) {
                log(L"disable failed: " + info.first + L" (" +
                    std::to_wstring(pid) + L") " + BaseName(c.path));
            }
        }
        if (!self && process) CloseHandle(process);
    }
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (auto& p : pending) {
        ULONGLONG now = GetTickCount64();
        DWORD left = now < deadline ? (DWORD)(deadline - now) : 0;
        DWORD wait = WaitForSingleObject(p.thread, left);
        if (log) {
            log((wait == WAIT_OBJECT_0 ? L"disabled: " : L"disable still running: ") +
                p.name + L" (" + std::to_wstring(p.pid) + L") " +
                BaseName(p.path));
        }
        CloseHandle(p.thread);
    }
    return pending.size();
}

}  // namespace bfs
