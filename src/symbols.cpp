// Standalone replacement for WindhawkUtils::HookSymbols.
//
// Resolves C++ function names (in the undecorated form the mod uses) inside a
// system DLL by downloading the matching PDB from the Microsoft symbol server
// and reading it with DbgHelp. Resolved addresses are cached per module
// version in %LOCALAPPDATA%\BetterFileSizes\symbols.ini, so the PDB is only
// needed the first time a given DLL version is seen.
//
// GNU GPL v3, like the original mod.

#define WH_STANDALONE_RUNTIME
#include "windhawk_utils.h"

#include <dbghelp.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#ifndef UNDNAME_NO_PTR64
#define UNDNAME_NO_PTR64 0x20000
#endif

// dbghelp.dll and winhttp.dll are loaded on demand (LoadLibrary +
// GetProcAddress) instead of being linked, so processes this DLL is injected
// into only pay for them when a PDB really has to be downloaded/parsed
// (normally once per Windows build, after that the RVA cache is used).
extern "C" const wchar_t* BfsGetIniPath(void);

namespace lz {

#define BFS_LAZY_DECL(name) decltype(&::name) name = nullptr;
BFS_LAZY_DECL(SymSetOptions)
BFS_LAZY_DECL(SymInitializeW)
BFS_LAZY_DECL(SymLoadModuleExW)
BFS_LAZY_DECL(SymGetModuleInfoW64)
BFS_LAZY_DECL(SymEnumSymbolsW)
BFS_LAZY_DECL(SymUnloadModule64)
BFS_LAZY_DECL(SymCleanup)
BFS_LAZY_DECL(UnDecorateSymbolNameW)
BFS_LAZY_DECL(WinHttpCrackUrl)
BFS_LAZY_DECL(WinHttpOpen)
BFS_LAZY_DECL(WinHttpConnect)
BFS_LAZY_DECL(WinHttpOpenRequest)
BFS_LAZY_DECL(WinHttpSendRequest)
BFS_LAZY_DECL(WinHttpReceiveResponse)
BFS_LAZY_DECL(WinHttpQueryHeaders)
BFS_LAZY_DECL(WinHttpReadData)
BFS_LAZY_DECL(WinHttpCloseHandle)
#undef BFS_LAZY_DECL

HMODULE LoadSystemDll(PCWSTR name) {
    // System32 only, never the application directory (DLL planting).
    HMODULE m = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m && GetLastError() == ERROR_INVALID_PARAMETER) {
        // LOAD_LIBRARY_SEARCH_* unsupported (very old Windows 7 w/o KB2533623).
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH - 32);
        if (n) {
            std::wstring full = std::wstring(path, n) + L"\\" + name;
            m = LoadLibraryW(full.c_str());
        }
    }
    if (!m) Wh_Log(L"Failed to load %s: %u", name, GetLastError());
    return m;
}

template <typename T>
bool Resolve(HMODULE m, T& fn, const char* name) {
    fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    if (!fn) Wh_Log(L"Missing export %S", name);
    return fn != nullptr;
}

// RAII: loads the library, resolves pointers, unloads + clears on scope exit.
class DbgHelp {
   public:
    DbgHelp() {
        m_ = LoadSystemDll(L"dbghelp.dll");
        ok_ = m_ && Resolve(m_, SymSetOptions, "SymSetOptions") &&
              Resolve(m_, SymInitializeW, "SymInitializeW") &&
              Resolve(m_, SymLoadModuleExW, "SymLoadModuleExW") &&
              Resolve(m_, SymGetModuleInfoW64, "SymGetModuleInfoW64") &&
              Resolve(m_, SymEnumSymbolsW, "SymEnumSymbolsW") &&
              Resolve(m_, SymUnloadModule64, "SymUnloadModule64") &&
              Resolve(m_, SymCleanup, "SymCleanup") &&
              Resolve(m_, UnDecorateSymbolNameW, "UnDecorateSymbolNameW");
    }
    ~DbgHelp() {
        SymSetOptions = nullptr; SymInitializeW = nullptr;
        SymLoadModuleExW = nullptr; SymGetModuleInfoW64 = nullptr;
        SymEnumSymbolsW = nullptr; SymUnloadModule64 = nullptr;
        SymCleanup = nullptr; UnDecorateSymbolNameW = nullptr;
        if (m_) FreeLibrary(m_);
    }
    bool ok() const { return ok_; }

   private:
    HMODULE m_ = nullptr;
    bool ok_ = false;
};

class WinHttp {
   public:
    WinHttp() {
        m_ = LoadSystemDll(L"winhttp.dll");
        ok_ = m_ && Resolve(m_, WinHttpCrackUrl, "WinHttpCrackUrl") &&
              Resolve(m_, WinHttpOpen, "WinHttpOpen") &&
              Resolve(m_, WinHttpConnect, "WinHttpConnect") &&
              Resolve(m_, WinHttpOpenRequest, "WinHttpOpenRequest") &&
              Resolve(m_, WinHttpSendRequest, "WinHttpSendRequest") &&
              Resolve(m_, WinHttpReceiveResponse, "WinHttpReceiveResponse") &&
              Resolve(m_, WinHttpQueryHeaders, "WinHttpQueryHeaders") &&
              Resolve(m_, WinHttpReadData, "WinHttpReadData") &&
              Resolve(m_, WinHttpCloseHandle, "WinHttpCloseHandle");
    }
    ~WinHttp() {
        WinHttpCrackUrl = nullptr; WinHttpOpen = nullptr;
        WinHttpConnect = nullptr; WinHttpOpenRequest = nullptr;
        WinHttpSendRequest = nullptr; WinHttpReceiveResponse = nullptr;
        WinHttpQueryHeaders = nullptr; WinHttpReadData = nullptr;
        WinHttpCloseHandle = nullptr;
        if (m_) FreeLibrary(m_);
    }
    bool ok() const { return ok_; }

   private:
    HMODULE m_ = nullptr;
    bool ok_ = false;
};

}  // namespace lz

namespace {

std::mutex g_dbghelpMutex;

struct CodeViewInfo {
    GUID guid;
    DWORD age;
    std::string pdbName;  // File name only.
};

struct RSDS_DEBUG_FORMAT {
    DWORD signature;  // 'RSDS'
    GUID guid;
    DWORD age;
    char path[1];
};

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

bool GetCodeViewInfo(HMODULE module, CodeViewInfo* info) {
    auto base = (BYTE*)module;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!dir.VirtualAddress || !dir.Size) return false;
    auto entries = (IMAGE_DEBUG_DIRECTORY*)(base + dir.VirtualAddress);
    size_t count = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (size_t i = 0; i < count; i++) {
        if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
            !entries[i].AddressOfRawData) {
            continue;
        }
        auto rsds = (RSDS_DEBUG_FORMAT*)(base + entries[i].AddressOfRawData);
        if (rsds->signature != 0x53445352) continue;  // 'RSDS'
        info->guid = rsds->guid;
        info->age = rsds->age;
        std::string path = rsds->path;
        size_t slash = path.find_last_of("\\/");
        info->pdbName =
            slash == std::string::npos ? path : path.substr(slash + 1);
        return !info->pdbName.empty();
    }
    return false;
}

std::wstring GuidAgeString(const CodeViewInfo& cv) {
    wchar_t buf[64];
    _snwprintf(buf, ARRAYSIZE(buf),
               L"%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X",
               (unsigned)cv.guid.Data1, cv.guid.Data2, cv.guid.Data3,
               cv.guid.Data4[0], cv.guid.Data4[1], cv.guid.Data4[2],
               cv.guid.Data4[3], cv.guid.Data4[4], cv.guid.Data4[5],
               cv.guid.Data4[6], cv.guid.Data4[7], (unsigned)cv.age);
    return buf;
}

std::wstring GetDataDir() {
    wchar_t* localAppData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                       &localAppData))) {
        dir = localAppData;
        CoTaskMemFree(localAppData);
    } else {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(ARRAYSIZE(tmp), tmp);
        dir = tmp;
    }
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    dir += L"BetterFileSizes";
    return dir;
}

void CreateDirs(const std::wstring& path) {
    for (size_t i = 3; i <= path.size(); i++) {
        if (i == path.size() || path[i] == L'\\') {
            CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
        }
    }
}

bool HttpDownload(const std::wstring& url, const std::wstring& destPath) {
    // Downloads are serialized by the PDB mutex in EnsurePdb, and the
    // pointers are process-global, so guard against concurrent use.
    static std::mutex s_httpMutex;
    std::lock_guard<std::mutex> httpLock(s_httpMutex);
    lz::WinHttp winhttp;
    if (!winhttp.ok()) return false;

    URL_COMPONENTS uc = {sizeof(uc)};
    wchar_t host[256], urlPath[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = urlPath;
    uc.dwUrlPathLength = ARRAYSIZE(urlPath);
    if (!lz::WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        Wh_Log(L"Bad URL: %s", url.c_str());
        return false;
    }

    bool ok = false;
    HINTERNET session = lz::WinHttpOpen(L"BetterFileSizes/1.0",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        // Windows 7/8 don't support automatic proxy.
        session = lz::WinHttpOpen(L"BetterFileSizes/1.0",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                              0);
    }
    HINTERNET connect = nullptr, request = nullptr;
    std::wstring tmpPath = destPath + L".part";
    HANDLE file = INVALID_HANDLE_VALUE;

    do {
        if (!session) break;
        connect = lz::WinHttpConnect(session, host, uc.nPort, 0);
        if (!connect) break;
        DWORD flags =
            uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        request = lz::WinHttpOpenRequest(connect, L"GET", urlPath, nullptr,
                                     WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request) break;
        if (!lz::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !lz::WinHttpReceiveResponse(request, nullptr)) {
            Wh_Log(L"HTTP request failed: %u", GetLastError());
            break;
        }
        DWORD status = 0, size = sizeof(status);
        lz::WinHttpQueryHeaders(request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            Wh_Log(L"HTTP status %u for %s", status, url.c_str());
            break;
        }
        file = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) break;

        std::vector<BYTE> buf(1 << 16);
        bool failed = false;
        for (;;) {
            DWORD read = 0;
            if (!lz::WinHttpReadData(request, buf.data(), (DWORD)buf.size(),
                                 &read)) {
                failed = true;
                break;
            }
            if (read == 0) break;
            DWORD written;
            if (!WriteFile(file, buf.data(), read, &written, nullptr)) {
                failed = true;
                break;
            }
        }
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        if (failed) break;
        ok = MoveFileExW(tmpPath.c_str(), destPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING);
    } while (false);

    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!ok) DeleteFileW(tmpPath.c_str());
    if (request) lz::WinHttpCloseHandle(request);
    if (connect) lz::WinHttpCloseHandle(connect);
    if (session) lz::WinHttpCloseHandle(session);
    return ok;
}

std::wstring GetSymbolServer() {
    // Optional override: [Symbols] server=https://... in BetterFileSizes.ini.
    std::wstring ini = BfsGetIniPath();
    wchar_t server[512];
    GetPrivateProfileStringW(L"Symbols", L"server",
                             L"https://msdl.microsoft.com/download/symbols",
                             server, ARRAYSIZE(server), ini.c_str());
    std::wstring s = server;
    while (!s.empty() && s.back() == L'/') s.pop_back();
    return s;
}

// Returns the folder holding the PDB, downloading it if needed.
std::wstring EnsurePdb(const CodeViewInfo& cv) {
    std::wstring pdbName = Utf8ToWide(cv.pdbName);
    std::wstring guidAge = GuidAgeString(cv);
    std::wstring dir = GetDataDir() + L"\\pdb\\" + pdbName + L"\\" + guidAge;
    std::wstring pdbPath = dir + L"\\" + pdbName;

    if (GetFileAttributesW(pdbPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return dir;
    }

    // Many processes may start at once; let only one download at a time.
    std::wstring mutexName = L"Local\\BetterFileSizes_pdb_" + guidAge;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (mutex) WaitForSingleObject(mutex, 5 * 60 * 1000);

    if (GetFileAttributesW(pdbPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirs(dir);
        std::wstring url =
            GetSymbolServer() + L"/" + pdbName + L"/" + guidAge + L"/" + pdbName;
        Wh_Log(L"Downloading %s", url.c_str());
        if (!HttpDownload(url, pdbPath)) {
            Wh_Log(L"PDB download failed");
        }
    }

    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }

    return GetFileAttributesW(pdbPath.c_str()) != INVALID_FILE_ATTRIBUTES
               ? dir
               : L"";
}

std::wstring Normalize(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s.compare(i, 7, L"__ptr64") == 0) {
            i += 6;
            continue;
        }
        if (!iswspace(s[i])) out += s[i];
    }
    return out;
}

std::wstring HashKey(std::wstring_view s) {
    // FNV-1a 64-bit, used as an INI key for the cache.
    unsigned long long h = 1469598103934665603ULL;
    for (wchar_t c : s) {
        h ^= (unsigned short)c;
        h *= 1099511628211ULL;
    }
    wchar_t buf[32];
    _snwprintf(buf, ARRAYSIZE(buf), L"%016I64X", h);
    return buf;
}

struct EnumContext {
    std::vector<std::wstring> wantedExact;
    std::vector<std::wstring> wantedNorm;
    std::vector<ULONG64> found;  // RVA, 0 if not found.
    ULONG64 base;
};

BOOL CALLBACK EnumSymbolsCallback(PSYMBOL_INFOW sym, ULONG, PVOID ctxPtr) {
    auto ctx = (EnumContext*)ctxPtr;
    std::wstring decorated(sym->Name, sym->NameLen);
    wchar_t undecorated[4096];
    DWORD len = lz::UnDecorateSymbolNameW(decorated.c_str(), undecorated,
                                      ARRAYSIZE(undecorated),
                                      UNDNAME_COMPLETE | UNDNAME_NO_PTR64);
    std::wstring und = len ? std::wstring(undecorated, len) : decorated;
    std::wstring undNorm;
    bool computedNorm = false;

    for (size_t i = 0; i < ctx->wantedExact.size(); i++) {
        if (ctx->found[i]) continue;
        bool match = und == ctx->wantedExact[i] ||
                     decorated == ctx->wantedExact[i];
        if (!match) {
            if (!computedNorm) {
                undNorm = Normalize(und);
                computedNorm = true;
            }
            match = undNorm == ctx->wantedNorm[i];
        }
        if (match) {
            ctx->found[i] = sym->Address - ctx->base;
        }
    }
    return TRUE;
}

bool ResolveWithPdb(HMODULE module, const std::wstring& modulePath,
                    const std::wstring& pdbDir, EnumContext* ctx) {
    std::lock_guard<std::mutex> lock(g_dbghelpMutex);
    lz::DbgHelp dbghelp;
    if (!dbghelp.ok()) return false;

    // A private pseudo-handle so we don't clash with the host's DbgHelp use.
    static int s_dummy;
    HANDLE process = (HANDLE)&s_dummy;

    lz::SymSetOptions(SYMOPT_PUBLICS_ONLY | SYMOPT_FAIL_CRITICAL_ERRORS |
                  SYMOPT_NO_PROMPTS | SYMOPT_EXACT_SYMBOLS);
    if (!lz::SymInitializeW(process, pdbDir.c_str(), FALSE)) {
        Wh_Log(L"SymInitialize failed: %u", GetLastError());
        return false;
    }

    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)module +
                                  ((IMAGE_DOS_HEADER*)module)->e_lfanew);
    ctx->base = (ULONG64)(ULONG_PTR)module;
    DWORD64 loaded = lz::SymLoadModuleExW(process, nullptr, modulePath.c_str(),
                                      nullptr, ctx->base,
                                      nt->OptionalHeader.SizeOfImage, nullptr,
                                      0);
    bool ok = false;
    if (loaded) {
        IMAGEHLP_MODULEW64 mi = {sizeof(mi)};
        if (lz::SymGetModuleInfoW64(process, loaded, &mi) &&
            mi.SymType == SymPdb) {
            ok = lz::SymEnumSymbolsW(process, loaded, L"*", EnumSymbolsCallback,
                                 ctx);
        } else {
            Wh_Log(L"PDB was not loaded (SymType %d)", (int)mi.SymType);
        }
        lz::SymUnloadModule64(process, loaded);
    } else {
        Wh_Log(L"SymLoadModuleEx failed: %u", GetLastError());
    }
    lz::SymCleanup(process);
    return ok;
}

}  // namespace

namespace WindhawkUtils {

bool HookSymbols(HMODULE module,
                 const SYMBOL_HOOK* symbolHooks,
                 size_t symbolHooksCount,
                 const HOOK_SYMBOLS_OPTIONS* options) {
    (void)options;
    if (!module) return false;
    if (symbolHooksCount == 0) return true;

    wchar_t pathBuf[MAX_PATH];
    DWORD pathLen = GetModuleFileNameW(module, pathBuf, ARRAYSIZE(pathBuf));
    if (!pathLen || pathLen >= ARRAYSIZE(pathBuf)) return false;
    std::wstring modulePath(pathBuf, pathLen);
    std::wstring moduleName = modulePath.substr(modulePath.find_last_of(L'\\') + 1);

    auto nt = (IMAGE_NT_HEADERS*)((BYTE*)module +
                                  ((IMAGE_DOS_HEADER*)module)->e_lfanew);
    wchar_t section[MAX_PATH + 64];
    _snwprintf(section, ARRAYSIZE(section), L"%s_%08X_%08X_%s",
               moduleName.c_str(), (unsigned)nt->FileHeader.TimeDateStamp,
               (unsigned)nt->OptionalHeader.SizeOfImage,
#ifdef _WIN64
               L"x64"
#else
               L"x86"
#endif
    );

    std::wstring dataDir = GetDataDir();
    CreateDirs(dataDir);
    std::wstring cachePath = dataDir + L"\\symbols.ini";

    // Each hook may list several alternative names; flatten them.
    struct Wanted {
        size_t hookIndex;
        std::wstring name;
    };
    std::vector<Wanted> wanted;
    for (size_t i = 0; i < symbolHooksCount; i++) {
        for (auto name : symbolHooks[i].symbols) {
            wanted.push_back({i, std::wstring(name)});
        }
    }

    std::vector<ULONG64> rvas(wanted.size(), 0);
    std::vector<bool> cached(wanted.size(), false);
    bool allCached = true;
    for (size_t i = 0; i < wanted.size(); i++) {
        wchar_t value[64];
        GetPrivateProfileStringW(section, HashKey(wanted[i].name).c_str(), L"",
                                 value, ARRAYSIZE(value), cachePath.c_str());
        if (*value) {
            cached[i] = true;
            rvas[i] = wcscmp(value, L"none") == 0
                          ? 0
                          : wcstoull(value, nullptr, 16);
        } else {
            allCached = false;
        }
    }

    if (!allCached) {
        CodeViewInfo cv;
        if (!GetCodeViewInfo(module, &cv)) {
            Wh_Log(L"No CodeView info in %s", moduleName.c_str());
            return false;
        }
        std::wstring pdbDir = EnsurePdb(cv);
        if (pdbDir.empty()) {
            return false;
        }

        EnumContext ctx;
        for (auto& w : wanted) {
            ctx.wantedExact.push_back(w.name);
            ctx.wantedNorm.push_back(Normalize(w.name));
        }
        ctx.found.assign(wanted.size(), 0);
        if (!ResolveWithPdb(module, modulePath, pdbDir, &ctx)) {
            return false;
        }

        for (size_t i = 0; i < wanted.size(); i++) {
            rvas[i] = ctx.found[i];
            wchar_t value[64];
            if (rvas[i]) {
                _snwprintf(value, ARRAYSIZE(value), L"%I64X", rvas[i]);
            } else {
                wcscpy(value, L"none");
            }
            WritePrivateProfileStringW(section, HashKey(wanted[i].name).c_str(),
                                       value, cachePath.c_str());
        }
    }

    // Pick the first resolved alternative for every hook.
    std::vector<void*> addresses(symbolHooksCount, nullptr);
    for (size_t i = 0; i < wanted.size(); i++) {
        size_t h = wanted[i].hookIndex;
        if (!addresses[h] && rvas[i]) {
            addresses[h] = (BYTE*)module + rvas[i];
        }
    }

    for (size_t i = 0; i < symbolHooksCount; i++) {
        if (!addresses[i] && !symbolHooks[i].optional) {
            Wh_Log(L"Symbol not found: %.*s",
                   symbolHooks[i].symbols.empty()
                       ? 0
                       : (int)symbolHooks[i].symbols[0].size(),
                   symbolHooks[i].symbols.empty()
                       ? L""
                       : symbolHooks[i].symbols[0].data());
            return false;
        }
    }

    for (size_t i = 0; i < symbolHooksCount; i++) {
        const auto& hook = symbolHooks[i];
        if (!addresses[i]) {
            Wh_Log(L"Optional symbol not found, skipping");
            continue;
        }
        if (hook.hookFunction) {
            if (!Wh_SetFunctionHook(addresses[i], hook.hookFunction,
                                    hook.pOriginalFunction)) {
                return false;
            }
        } else if (hook.pOriginalFunction) {
            *hook.pOriginalFunction = addresses[i];
        }
    }
    return true;
}

}  // namespace WindhawkUtils
