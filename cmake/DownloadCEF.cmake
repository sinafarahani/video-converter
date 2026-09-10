# Downloads and extracts a CEF binary distribution if it is not already present.
#
# Adapted from the upstream cef-project template. Only runs when CEF_ROOT was
# not supplied by the preset, so on a machine that already has a distribution
# this file does nothing.

# The distribution to fetch. Bump these two together.
#
# 151.3.24 is pinned because it is the newest 151 patch AND the first one
# published for all four targets: 151.3.15 had no macOS Intel build at all.
# Staying on the 151 branch keeps CEF_API_VERSION 15101 valid; moving to 152
# means re-checking the API version in the top-level CMakeLists.txt.
set(CONVERTER_CEF_VERSION "151.3.24+g2384915+chromium-151.0.7922.174"
    CACHE STRING "CEF version to download when CEF_ROOT is not set")

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(CONVERTER_CEF_PLATFORM "windows64")
  else()
    set(CONVERTER_CEF_PLATFORM "windows32")
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  # Keyed on the TARGET architecture so an Intel build can be cross-compiled
  # on an Apple Silicon machine; falls back to the host when none is given.
  set(_mac_arch "${CMAKE_OSX_ARCHITECTURES}")
  if(NOT _mac_arch)
    set(_mac_arch "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  if(_mac_arch MATCHES "arm64|aarch64")
    set(CONVERTER_CEF_PLATFORM "macosarm64")
  else()
    set(CONVERTER_CEF_PLATFORM "macosx64")
  endif()
else()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(CONVERTER_CEF_PLATFORM "linuxarm64")
  else()
    set(CONVERTER_CEF_PLATFORM "linux64")
  endif()
endif()

function(download_cef platform version download_dir)
  # The CDN encodes '+' literally in the filename but it must be percent-encoded
  # in the URL, otherwise the CDN reads it as a space and returns 404.
  string(REPLACE "+" "%2B" _url_version "${version}")
  set(_name "cef_binary_${version}_${platform}_minimal")
  set(_root "${download_dir}/${_name}")

  if(EXISTS "${_root}/cmake/FindCEF.cmake")
    message(STATUS "CEF already present: ${_root}")
    set(CEF_ROOT "${_root}" CACHE PATH "" FORCE)
    return()
  endif()

  set(_archive "${download_dir}/${_name}.tar.bz2")
  set(_url "https://cef-builds.spotifycdn.com/cef_binary_${_url_version}_${platform}_minimal.tar.bz2")

  file(MAKE_DIRECTORY "${download_dir}")

  # Fetch the published SHA1 first so the payload can be verified. A truncated
  # 160 MB download that silently half-extracts is a miserable thing to debug.
  if(NOT EXISTS "${_archive}")
    message(STATUS "Downloading CEF ${version} (~163 MB) ...")
    file(DOWNLOAD "${_url}.sha1" "${_archive}.sha1" STATUS _s TIMEOUT 60)
    list(GET _s 0 _code)
    if(NOT _code EQUAL 0)
      list(GET _s 1 _msg)
      message(FATAL_ERROR "Could not fetch CEF checksum from ${_url}.sha1 : ${_msg}")
    endif()
    file(READ "${_archive}.sha1" _sha1)
    string(STRIP "${_sha1}" _sha1)

    file(DOWNLOAD "${_url}" "${_archive}"
         EXPECTED_HASH SHA1=${_sha1}
         SHOW_PROGRESS STATUS _s)
    list(GET _s 0 _code)
    if(NOT _code EQUAL 0)
      list(GET _s 1 _msg)
      file(REMOVE "${_archive}")
      message(FATAL_ERROR "CEF download failed: ${_msg}")
    endif()
  endif()

  message(STATUS "Extracting CEF ...")
  execute_process(COMMAND ${CMAKE_COMMAND} -E tar xjf "${_archive}"
                  WORKING_DIRECTORY "${download_dir}"
                  RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Failed to extract ${_archive}")
  endif()

  if(NOT EXISTS "${_root}/cmake/FindCEF.cmake")
    message(FATAL_ERROR "CEF extracted but ${_root} does not look like a distribution")
  endif()

  # The extracted tree is all that is needed from here on; keeping the 160 MB
  # archive as well would double every cache and CI artifact that holds it.
  file(REMOVE "${_archive}" "${_archive}.sha1")

  set(CEF_ROOT "${_root}" CACHE PATH "" FORCE)
  message(STATUS "CEF ready: ${_root}")
endfunction()
