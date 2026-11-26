cmake_minimum_required(VERSION 3.16)
project(BetterFileSizes C CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)

# MinHook (vendored, BSD-2-Clause)
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
    third_party/minhook/src/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook/include)

# The mod DLL that gets injected. dbghelp/winhttp are NOT linked: they are
# loaded on demand in src/symbols.cpp.
add_library(BetterFileSizes SHARED
    src/mod.cpp
    src/runtime.cpp
    src/symbols.cpp
    src/BetterFileSizes.rc)
target_include_directories(BetterFileSizes PRIVATE shim)
target_link_libraries(BetterFileSizes PRIVATE
    minhook ole32 oleaut32 propsys shell32 shlwapi uuid)
set_target_properties(BetterFileSizes PROPERTIES PREFIX "" OUTPUT_NAME "BetterFileSizes")

# Shared host logic: process filter, injection, window-event watcher.
add_library(bfshost STATIC
    host/process_filter.cpp
    host/inject.cpp
    host/host.cpp
    host/readiness.cpp)
target_link_libraries(bfshost PUBLIC psapi user32 advapi32 pdh version)

add_library(BetterFileSizesEverything SHARED everything_plugin/plugin.cpp)
target_link_libraries(BetterFileSizesEverything PRIVATE bfshost)
set_target_properties(BetterFileSizesEverything PROPERTIES PREFIX ""
    OUTPUT_NAME "BetterFileSizesEverything")

if(MSVC)
    foreach(t BetterFileSizes bfshost BetterFileSizesEverything)
        target_compile_options(${t} PRIVATE /utf-8 /EHsc)
        set_property(TARGET ${t} PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endforeach()
    target_compile_options(BetterFileSizes PRIVATE /permissive-)
    target_link_libraries(BetterFileSizes PRIVATE comsuppw)
    target_sources(BetterFileSizesEverything PRIVATE everything_plugin/plugin.def)
else()
    foreach(t BetterFileSizes bfshost BetterFileSizesEverything)
        target_compile_options(${t} PRIVATE -finput-charset=UTF-8 -fexec-charset=UTF-8)
    endforeach()
    # Fully static runtime so the DLLs have no libstdc++/libgcc dependencies
    # to drag into every process they are loaded into.
    target_link_options(BetterFileSizes PRIVATE -static -static-libgcc -static-libstdc++ -s -Wl,--kill-at)
    target_link_options(BetterFileSizesEverything PRIVATE -static -static-libgcc -static-libstdc++ -s -Wl,--kill-at)
endif()

install(TARGETS BetterFileSizes BetterFileSizesEverything
    RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES config/BetterFileSizes.ini DESTINATION .)
