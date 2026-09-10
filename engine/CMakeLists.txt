# The conversion engine.
#
# Deliberately free of CEF and of any UI dependency, so it builds with a normal
# toolchain and can be exercised from a test harness.

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)

set(CONV_ENGINE_SOURCES
  src/util.cpp
  src/messages.cpp
  src/logging.cpp
  src/ffmpeg.cpp
  src/gpu.cpp
  src/job.cpp
  src/process_common.hpp
)

# Child processes are driven directly through the OS. The previous reproc-based
# layer emulated pipes with loopback TCP sockets on Windows and could abandon a
# running ffmpeg after a transient socket error, which left encodes idle for
# days. See the Process class comment in include/conv/ffmpeg.hpp.
if(WIN32)
  list(APPEND CONV_ENGINE_SOURCES src/process_win.cpp)
else()
  list(APPEND CONV_ENGINE_SOURCES src/process_posix.cpp)
endif()

add_library(conv_engine STATIC ${CONV_ENGINE_SOURCES})

target_include_directories(conv_engine
  PUBLIC  include
  PRIVATE src
)

target_link_libraries(conv_engine
  PUBLIC  nlohmann_json::nlohmann_json
  PRIVATE spdlog::spdlog fmt::fmt
)

target_compile_features(conv_engine PUBLIC cxx_std_20)

if(WIN32)
  # dxgi for adapter enumeration, shell32/ole32 for known-folder lookups.
  target_link_libraries(conv_engine PRIVATE dxgi shell32 ole32)
  target_compile_definitions(conv_engine PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE _UNICODE
    # std::getenv is standard C++; MSVC deprecates it in favour of a
    # Microsoft-only replacement we cannot use in cross-platform code.
    _CRT_SECURE_NO_WARNINGS
  )
else()
  find_package(Threads REQUIRED)
  target_link_libraries(conv_engine PRIVATE Threads::Threads)
endif()

set_target_properties(conv_engine PROPERTIES FOLDER "converter")
