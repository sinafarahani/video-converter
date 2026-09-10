# ============================================================================
#  Video and Audio Compressor and Converter  --  C++ / CEF / Vue rewrite
# ============================================================================
#
#  Open this folder in Visual Studio 2026 ("Open a local folder") and it will
#  configure automatically from CMakePresets.json, then F5 to run.
#
#  Layout:
#    engine/  the conversion core. No CEF, no UI, no Windows-only code in the
#             interfaces. Built with normal C++20 flags (exceptions + RTTI on).
#    app/     the CEF shell: window, JS bridge, asset serving. Built with CEF's
#             flag set, minus the three restrictions we deliberately relax.
#    ui/      the Vue 3 frontend. `npm run build` emits ui/dist, which is
#             embedded into the app at build time.
#
# 3.21 is CEF's own floor. 3.24 is ours: the macOS bundle assembly uses the
# $<TARGET_BUNDLE_DIR_NAME:...> generator expression, which was added there.
cmake_minimum_required(VERSION 3.24)

project(converter
  VERSION 2.1.0
  DESCRIPTION "Video and Audio Compressor and Converter"
  # C as well as C++: on Linux, CEF's cmake runs CHECK_C_COMPILER_FLAG, which
  # fails outright if the C language was never enabled.
  LANGUAGES C CXX)

# main_mac.mm needs Objective-C++ on macOS.
if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 20)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

# ---------------------------------------------------------------------------
# CRT linkage
# ---------------------------------------------------------------------------
#  The x64-windows-static triplet builds every vcpkg package against the static
#  CRT (/MT). CMake defaults our own targets to /MD, and mixing the two fails at
#  link time with a wall of LNK2038 "RuntimeLibrary mismatch" errors.
#
#  Static is also what we want for shipping: no VC++ redistributable, which
#  means the installer needs no administrator rights. It matches CEF's own
#  default too.
if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "-static$")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
set(CEF_ROOT "" CACHE PATH
    "Root of an extracted CEF binary distribution (cef_binary_*). Leave empty to auto-download.")

option(CONVERTER_BUILD_UI "Run the Vue production build as part of the C++ build" ON)

# Builds only the conversion engine, with no CEF and no UI. Useful for working
# on the media pipeline without a 300 MB dependency in the loop, and for CI.
option(CONVERTER_ENGINE_ONLY "Build just the engine library" OFF)

# We render only our own local, trusted assets -- never remote content -- so the
# Chromium renderer sandbox buys us little here, and turning it off means we get
# a normal, brandable, code-signable converter.exe instead of CEF's bootstrap.exe
# plus a client DLL. Flip this to ON if the app ever loads remote URLs.
set(USE_SANDBOX OFF CACHE BOOL "Enable the CEF sandbox (changes the output to bootstrap.exe + DLL)")

# ---------------------------------------------------------------------------
# Locate CEF
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# The Windows installer, the macOS Info.plist and the Linux .desktop file each
# carry a hand-written copy of the engine's extension list. Fail here, on every
# platform and in the engine-only build too, if one has drifted from it.
include(CheckMediaExtensions)

if(CONVERTER_ENGINE_ONLY)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  if(MSVC)
    add_compile_options(/Zc:__cplusplus /utf-8 /permissive- /W4)
  endif()
  add_subdirectory(engine)
  add_subdirectory(tools/convctl)
  return()
endif()

if(NOT CEF_ROOT)
  include(DownloadCEF)
  # Downloads + extracts only if the target directory is not already present.
  download_cef("${CONVERTER_CEF_PLATFORM}" "${CONVERTER_CEF_VERSION}" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cef")
endif()

if(NOT EXISTS "${CEF_ROOT}/cmake/FindCEF.cmake")
  message(FATAL_ERROR
    "CEF_ROOT does not point at a CEF binary distribution: ${CEF_ROOT}\n"
    "Expected to find <CEF_ROOT>/cmake/FindCEF.cmake")
endif()

# Pin the CEF API version rather than building against the experimental
# (unversioned) API, which is tied to one exact libcef.dll. With a pinned
# version a newer CEF binary can be dropped in for a security fix without
# recompiling. CEF's own cmake reads this as `api_version` and validates it.
#
# 15101 is CEF_API_VERSION_LAST for the 151 distribution.
if(NOT DEFINED api_version)
  set(api_version "15101")
endif()

list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)

# ---------------------------------------------------------------------------
# Relax three CEF defaults, consistently across every target we compile
# ---------------------------------------------------------------------------
#  CEF ships with RTTI off, exceptions off and the language pinned to C++20.
#  Chromium needs that; we do not, and our dependencies (nlohmann-json, spdlog)
#  are written against a normal toolchain.
#
#  The important part is CONSISTENCY. Mixing _HAS_EXCEPTIONS=0 and =1
#  translation units in one binary is an MSVC STL ODR violation that shows up as
#  corrupted std::string and impossible-looking crashes. So we strip the flags
#  here, BEFORE libcef_dll_wrapper is added, which means the wrapper is compiled
#  with the same settings as everything else.
#
#  This is safe against libcef.dll itself: that boundary is a pure C ABI, so it
#  neither knows nor cares how our side was compiled.
if(MSVC)
  list(REMOVE_ITEM CEF_COMPILER_FLAGS /GR-)          # re-enable RTTI
  list(REMOVE_ITEM CEF_CXX_COMPILER_FLAGS /std:c++20) # we set the standard per-target
  list(REMOVE_ITEM CEF_COMPILER_DEFINES _HAS_EXCEPTIONS=0)
  # /WX turns any warning in our own code into a hard error. Keep the warnings,
  # drop the escalation -- we are not vendoring Chromium's warning discipline.
  list(REMOVE_ITEM CEF_COMPILER_FLAGS /WX)
else()
  list(REMOVE_ITEM CEF_COMPILER_FLAGS -fno-rtti -fno-exceptions)
  list(REMOVE_ITEM CEF_CXX_COMPILER_FLAGS -fno-rtti -fno-exceptions -std=c++20)
  # CEF builds with -Werror. That is right for Chromium, but it means any
  # warning anywhere in our code -- or in a vcpkg header -- fails the build.
  # Keep the warnings, drop the escalation, exactly as we do with /WX on MSVC.
  list(REMOVE_ITEM CEF_COMPILER_FLAGS -Werror)
  list(REMOVE_ITEM CEF_CXX_COMPILER_FLAGS -Werror)
endif()

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  # /Zc:__cplusplus -- without it MSVC reports __cplusplus as 199711L and
  #   feature detection in third-party headers picks the wrong code paths.
  #
  # /EHsc -- REQUIRED, and easy to miss. CEF compiles with _HAS_EXCEPTIONS=0 and
  #   never adds an exception model; we removed that define so the STL now
  #   declares exceptions enabled, but without /EHsc the compiler still emits no
  #   unwind code. The result is C4530 and, worse, destructors that do not run
  #   while an exception propagates. nlohmann-json throws on malformed input, so
  #   this is a live path, not a theoretical one.
  add_compile_options(/Zc:__cplusplus /utf-8 /permissive- /EHsc)
endif()

# Everything lands in one directory so the app can find CEF's resources, the
# bundled ffmpeg, and the UI assets using paths relative to the executable.
set(CONVERTER_OUT_DIR "${CMAKE_BINARY_DIR}/$<CONFIG>")

# Where a local ffmpeg/ffprobe pair lives, to be staged next to the executable
# for developer builds. The installer supplies its own copy.
set(CONVERTER_FFMPEG_DIR "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH
    "Directory containing ffmpeg/ffprobe to stage into the build output")

add_subdirectory(engine)
if(CONVERTER_BUILD_UI)
  add_subdirectory(ui)
endif()
add_subdirectory(app)

# The CEF distribution ships a sample project we never build.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
