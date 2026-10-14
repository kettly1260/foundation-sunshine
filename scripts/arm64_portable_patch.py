#!/usr/bin/env python3
"""Experimental ARM64 CMake bootstrap for AlkaidLab/foundation-sunshine.

Only run on a clean checkout at the known 2026-10-13 source layout.
Fails closed when expected source fragments are absent.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def edit_file(relpath, replacements):
    path = ROOT / relpath
    original = path.read_text(encoding="utf-8")
    updated = original
    for before, after in replacements:
        n = updated.count(before)
        if n != 1:
            raise RuntimeError(f"{relpath}: expected 1 match, found {n}: {before[:90]!r}")
        updated = updated.replace(before, after, 1)
    if updated != original:
        path.write_text(updated, encoding="utf-8", newline="\n")
        print(f"Patched {relpath}")


# Based on the native Windows ARM64 MinHook variant in upstream LizardByte/Sunshine.
edit_file("cmake/dependencies/windows.cmake", [(
    """# Make sure MinHook is installed
find_library(MINHOOK_LIBRARY libMinHook.a REQUIRED)
find_path(MINHOOK_INCLUDE_DIR MinHook.h PATH_SUFFIXES include REQUIRED)

add_library(minhook::minhook STATIC IMPORTED)
set_property(TARGET minhook::minhook PROPERTY IMPORTED_LOCATION ${MINHOOK_LIBRARY})
target_include_directories(minhook::minhook INTERFACE ${MINHOOK_INCLUDE_DIR})
""",
    """# For Windows ARM64, use upstream's minhook-detours compatibility shim.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
  include(FetchContent)
  FetchContent_Declare(minhook-detours
    URL https://github.com/m417z/minhook-detours/releases/download/v1.0.6/minhook-detours-1.0.6.zip
    URL_HASH SHA256=E719959D824511E27395A82AEDA994CAAD53A67EE5894BA5FC2F4BF1FA41E38E)
  FetchContent_MakeAvailable(minhook-detours)
  add_library(minhook::minhook SHARED IMPORTED GLOBAL)
  set_target_properties(minhook::minhook PROPERTIES
    IMPORTED_LOCATION "${minhook-detours_SOURCE_DIR}/Release/minhook-detours.ARM64.Release.dll"
    IMPORTED_IMPLIB "${minhook-detours_SOURCE_DIR}/Release/minhook-detours.ARM64.Release.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${minhook-detours_SOURCE_DIR}/src")
else()
  find_library(MINHOOK_LIBRARY libMinHook.a REQUIRED)
  find_path(MINHOOK_INCLUDE_DIR MinHook.h PATH_SUFFIXES include REQUIRED)
  add_library(minhook::minhook STATIC IMPORTED)
  set_property(TARGET minhook::minhook PROPERTY IMPORTED_LOCATION ${MINHOOK_LIBRARY})
  target_include_directories(minhook::minhook INTERFACE ${MINHOOK_INCLUDE_DIR})
endif()
"""
)])

# Link through the imported CMake target, instead of assuming x64's -lMinHook.
edit_file("cmake/dependencies/common.cmake", [(
    "set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid vpl MinHook)",
    "set(FFMPEG_PLATFORM_LIBRARIES mfplat ole32 strmiids mfuuid vpl)"
)])

# Avoid GCC-specific statically linked libraries and windres flags on CLANGARM64.
edit_file("cmake/compile_definitions/windows.cmake", [(
    """enable_language(RC)
set(CMAKE_RC_COMPILER windres)
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} --use-temp-file")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
""",
    """# The ARM64 runner passes CMAKE_RC_COMPILER before project().
# Reassigning it here forces CMake to clear its compiler cache and configure forever.
if(NOT CMAKE_RC_COMPILER)
  message(FATAL_ERROR "ARM64 resource compiler not configured before project()")
endif()
enable_language(RC)
"""
), (
    """        libssp.a
        libstdc++.a
        libwinpthread.a
""",
    """"""
), (
    """        wtsapi32
)
""",
    """        wtsapi32
)
if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
  list(APPEND PLATFORM_LIBRARIES libssp.a libstdc++.a libwinpthread.a)
endif()
"""
)])
# The Foundation tree pins NVENC API headers by SDK version (1301/1300/1200/1100),
# but its generic include root does not itself contain ffnvcodec/nvEncodeAPI.h.
# Expose the latest pinned headers for general compilation (e.g. config.cpp).
# Per-version NVENC factory sources retain their explicitly pinned includes.
edit_file("cmake/compile_definitions/windows.cmake", [(
    'include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nvenc-headers")',
    'include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nvenc-headers/1301/include")\n'
    'include_directories(SYSTEM "${CMAKE_SOURCE_DIR}/third-party/nvenc-headers")'
)])

# AMD AMF SDK headers must be included at global scope. The stock source
# includes them *inside* namespace config::amd, which nests C++ declarations
# like std::optional in config::amd::std under clang/LLVM and breaks build.
edit_file("src/config.cpp", [(
    '#include "globals.h"\n\n',
    '#include "globals.h"\n\n'
    '#ifdef _WIN32\n'
    '  #ifdef _GLIBCXX_USE_C99_INTTYPES\n'
    '    #undef _GLIBCXX_USE_C99_INTTYPES\n'
    '  #endif\n'
    '  #include <AMF/components/VideoEncoderAV1.h>\n'
    '  #include <AMF/components/VideoEncoderHEVC.h>\n'
    '  #include <AMF/components/VideoEncoderVCE.h>\n'
    '#endif\n\n'
), (
    '#else\n'
    '  #ifdef _GLIBCXX_USE_C99_INTTYPES\n'
    '    #undef _GLIBCXX_USE_C99_INTTYPES\n'
    '  #endif\n'
    '  #include <AMF/components/VideoEncoderAV1.h>\n'
    '  #include <AMF/components/VideoEncoderHEVC.h>\n'
    '  #include <AMF/components/VideoEncoderVCE.h>\n'
    '#endif',
    '#endif'
)])

# MSYS2 CLANGARM64's libc++ rejects std::atomic<shared_ptr<T>>.
# Use the C++ shared_ptr atomic free functions to retain acquire/release
# synchronization and immutable snapshots across config publishers/readers.
edit_file("src/webhook/webhook.cpp", [(
    "std::atomic<std::shared_ptr<const configuration_t>> active_configuration {",
    "std::shared_ptr<const configuration_t> active_configuration {"
), (
    """active_configuration.store(
      std::move(configuration.configuration_),
      std::memory_order_release
    );""",
    """std::atomic_store_explicit(
      &active_configuration,
      std::move(configuration.configuration_),
      std::memory_order_release
    );"""
), (
    "const auto configuration = active_configuration.load(std::memory_order_acquire);",
    "const auto configuration = std::atomic_load_explicit(&active_configuration, std::memory_order_acquire);"
)])

# libc++ in CLANGARM64 rejects atomic<shared_ptr<T>>. Preserve log cache
# snapshot CAS semantics via the standard shared_ptr atomic free functions.
edit_file("src/confighttp.cpp", [(
    "static std::atomic<std::shared_ptr<const LogCacheSnapshot>> log_cache;",
    "static std::shared_ptr<const LogCacheSnapshot> log_cache;"
), (
    "auto snapshot = log_cache.load();",
    "auto snapshot = std::atomic_load(&log_cache);"
), (
    "if (!log_cache.compare_exchange_strong(snapshot, new_snap)) {",
    "if (!std::atomic_compare_exchange_strong(&log_cache, &snapshot, std::shared_ptr<const LogCacheSnapshot>(new_snap))) {"
)])

print("ARM64 compatibility bootstrap applied; native build still requires CI validation.")
