// Standalone replacement for Windhawk's windhawk_api.h / windhawk_utils.h.
// Lets the original mod source (src/mod.cpp) compile unchanged without
// Windhawk. Implementations live in src/runtime.cpp and src/symbols.cpp.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <initializer_list>
#include <string_view>
#include <type_traits>
#include <vector>

#ifdef __cplusplus
extern "C" {
#endif
void Wh_Log_Impl(const char* func, PCWSTR format, ...);
int Wh_GetIntSetting(PCWSTR valueName, ...);
PCWSTR Wh_GetStringSetting(PCWSTR valueName, ...);
void Wh_FreeStringSetting(PCWSTR string);
BOOL Wh_SetFunctionHook(void* targetFunction, void* hookFunction,
                        void** originalFunction);
BOOL Wh_RemoveFunctionHook(void* targetFunction);
BOOL Wh_ApplyHookOperations(void);
#ifdef __cplusplus
}
#endif

// Values Windhawk normally predefines for the mod.
#define WH_MOD_ID L"explorer-details-better-file-sizes"
#define WH_MOD_VERSION L"1.6"

#define Wh_Log(format, ...) Wh_Log_Impl(__FUNCTION__, format, ##__VA_ARGS__)

// The mod defines its own DllMain. The standalone runtime owns the real
// DllMain and forwards to the mod's one under this name.
#ifndef WH_STANDALONE_RUNTIME
#define DllMain Wh_Mod_DllMain
#endif

// Entry points the mod implements.
BOOL Wh_ModInit();
void Wh_ModUninit();
BOOL Wh_ModSettingsChanged(BOOL* bReload);

namespace WindhawkUtils {

template <typename T>
struct identity {
    using type = T;
};

template <typename Prototype>
inline BOOL SetFunctionHook(Prototype* targetFunction,
                            typename identity<Prototype*>::type hookFunction,
                            Prototype** originalFunction) {
    return Wh_SetFunctionHook((void*)targetFunction, (void*)hookFunction,
                              (void**)originalFunction);
}

struct SYMBOL_HOOK {
    std::vector<std::wstring_view> symbols;
    void** pOriginalFunction = nullptr;
    void* hookFunction = nullptr;
    bool optional = false;

    SYMBOL_HOOK() = default;

    template <typename Prototype>
    SYMBOL_HOOK(std::initializer_list<std::wstring_view> symbols,
                Prototype** originalFunction,
                typename identity<Prototype*>::type hookFunction = nullptr,
                bool optional = false)
        : symbols(symbols),
          pOriginalFunction((void**)originalFunction),
          hookFunction((void*)hookFunction),
          optional(optional) {}
};

struct HOOK_SYMBOLS_OPTIONS {
    PCWSTR symbolServer = nullptr;
    bool noUndecoratedSymbols = false;
    bool onlineCacheUrl = false;
};

bool HookSymbols(HMODULE module,
                 const SYMBOL_HOOK* symbolHooks,
                 size_t symbolHooksCount,
                 const HOOK_SYMBOLS_OPTIONS* options = nullptr);

}  // namespace WindhawkUtils
