# The CEF shell: window, JavaScript bridge, and asset serving.
#
# This is the only target that sees CEF. All of the actual work lives in
# conv_engine, which knows nothing about browsers.

# CEF's wrapper is shipped as source and has to be compiled by us. Because the
# top-level CMakeLists already stripped /GR-, _HAS_EXCEPTIONS=0, /std:c++20 and
# the warnings-as-errors flags from CEF's flag lists, the wrapper is built with
# the same settings as our own code -- which is what keeps the MSVC STL from
# tripping over ODR.
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" libcef_dll_wrapper)

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
#
# The entry point and the two directory lookups are the only platform-specific
# code. Folder picking goes through CefBrowserHost::RunFileDialog, which is
# native everywhere, so there is no per-platform dialog code at all.
# launch_paths turns command-line arguments (and macOS Apple Events, via
# main_mac.mm) into input paths for the page.
set(CONVERTER_SOURCES
  src/app.cc
  src/app.h
  src/client.cc
  src/client.h
  src/bridge.cc
  src/bridge.h
  src/launch_paths.cc
  src/launch_paths.h
  src/platform.h
)

if(OS_WINDOWS)
  # res/converter.rc carries the application icon (lifted from the JavaFX
  # installer) and the VERSIONINFO block.
  list(APPEND CONVERTER_SOURCES src/main_win.cc src/platform_win.cc res/converter.rc)
elseif(OS_MAC)
  # main_mac.mm provides the CefAppProtocol NSApplication CEF requires on macOS.
  # The .icns is generated from the same converter.ico as the Windows build;
  # listing it with a package location copies it into Contents/Resources,
  # where CFBundleIconFile in Info.plist.in expects it.
  list(APPEND CONVERTER_SOURCES src/main_mac.mm src/platform_posix.cc res/converter.icns)
  set_source_files_properties(res/converter.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
else()
  list(APPEND CONVERTER_SOURCES src/main_posix.cc src/platform_posix.cc)
endif()

# ADD_LOGICAL_TARGET wraps CEF's import library in an imported target carrying
# the right per-configuration path. macOS has no import library -- the framework
# is loaded at runtime by CefScopedLibraryLoader -- so it is skipped there.
if(NOT OS_MAC)
  ADD_LOGICAL_TARGET(libcef_lib "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
endif()

# Must come BEFORE add_executable: on Ninja and Makefile generators this macro
# sets CMAKE_RUNTIME_OUTPUT_DIRECTORY, which only applies to targets defined
# after it. Called afterwards, the exe lands one directory above libcef.dll and
# fails to start.
SET_CEF_TARGET_OUT_DIR()

# ===========================================================================
#  macOS: an .app bundle plus one helper bundle per subprocess type
# ===========================================================================
if(OS_MAC)
  set(CONVERTER_BUNDLE_ID "com.sina0.videoconverter")
  set(CONVERTER_APP "${CEF_TARGET_OUT_DIR}/converter.app")

  # Writes a concrete Info.plist for one bundle.
  #
  # MACOSX_BUNDLE_INFO_PLIST is expanded with whatever the global ${...}
  # variables hold at the END of this directory. With six bundles, every one of
  # them would get the last helper's name and identifier, and the Xcode-only
  # ${EXECUTABLE_NAME} would come out empty -- leaving the app with no
  # CFBundleExecutable, which macOS refuses to launch. Substituting by hand,
  # per bundle, avoids both.
  function(converter_write_plist template out exe_name product_name bundle_id)
    file(READ "${template}" _p)
    string(REPLACE "\${EXECUTABLE_NAME}"           "${exe_name}"        _p "${_p}")
    string(REPLACE "\${PRODUCT_NAME}"              "${product_name}"    _p "${_p}")
    string(REPLACE "\${PRODUCT_BUNDLE_IDENTIFIER}" "${bundle_id}"       _p "${_p}")
    string(REPLACE "\${CONVERTER_VERSION}"         "${PROJECT_VERSION}" _p "${_p}")
    string(REPLACE "\${CEF_TARGET_SDK}"            "${CEF_TARGET_SDK}"  _p "${_p}")
    file(WRITE "${out}" "${_p}")
  endfunction()

  # --- helper bundles ------------------------------------------------------
  #
  # macOS cannot relaunch the main executable for subprocesses, so each type
  # needs its own bundle. CEF_HELPER_APP_SUFFIXES supplies the list in the form
  # "<name suffix>:<target suffix>:<plist suffix>" -- five entries: the plain
  # helper plus Alerts, GPU, Plugin and Renderer. CEF finds them by name, as
  # "<main executable> Helper (<suffix>).app" inside Contents/Frameworks.
  set(CONVERTER_HELPER_TARGETS "")

  foreach(_suffix_pair ${CEF_HELPER_APP_SUFFIXES})
    string(REPLACE ":" ";" _suffix_list ${_suffix_pair})
    list(GET _suffix_list 0 _name_suffix)     # e.g. " (GPU)"
    list(GET _suffix_list 1 _target_suffix)   # e.g. "_gpu"
    list(GET _suffix_list 2 _plist_suffix)    # e.g. ".gpu"

    set(_helper_target "converter_helper${_target_suffix}")
    set(_helper_output "converter Helper${_name_suffix}")
    set(_helper_plist  "${CMAKE_CURRENT_BINARY_DIR}/helper-Info${_target_suffix}.plist")

    converter_write_plist("${CMAKE_CURRENT_SOURCE_DIR}/mac/helper-Info.plist.in" "${_helper_plist}"
                          "${_helper_output}" "${_helper_output}"
                          "${CONVERTER_BUNDLE_ID}.helper${_plist_suffix}")

    # app.cc is shared with the main app, and it calls into launch_paths, so
    # the helpers link that too even though they never open a path.
    add_executable(${_helper_target} MACOSX_BUNDLE
      src/main_mac_helper.cc src/app.cc src/app.h src/client.cc src/client.h
      src/bridge.cc src/bridge.h src/launch_paths.cc src/launch_paths.h
      src/platform_posix.cc src/platform.h)
    SET_EXECUTABLE_TARGET_PROPERTIES(${_helper_target})
    target_include_directories(${_helper_target} PRIVATE "${CEF_ROOT}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${_helper_target} PRIVATE conv_engine libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_compile_features(${_helper_target} PRIVATE cxx_std_20)

    set_target_properties(${_helper_target} PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST "${_helper_plist}"
      OUTPUT_NAME "${_helper_output}"
      FOLDER "converter/helpers"
    )

    list(APPEND CONVERTER_HELPER_TARGETS ${_helper_target})
  endforeach()

  # --- the app itself ------------------------------------------------------
  converter_write_plist("${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist.in"
                        "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
                        "converter" "Video Converter" "${CONVERTER_BUNDLE_ID}")

  add_executable(converter MACOSX_BUNDLE ${CONVERTER_SOURCES})
  SET_EXECUTABLE_TARGET_PROPERTIES(converter)
  add_dependencies(converter ${CONVERTER_HELPER_TARGETS})

  target_include_directories(converter PRIVATE "${CEF_ROOT}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(converter PRIVATE conv_engine libcef_dll_wrapper ${CEF_STANDARD_LIBS})
  target_compile_features(converter PRIVATE cxx_std_20)

  set_target_properties(converter PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    OUTPUT_NAME "converter"
    FOLDER "converter"
  )

  # The framework first, then the helpers into the Frameworks directory it
  # created. CEF's paks and locales live inside the framework on macOS, so
  # there is no separate resource copy as on the other platforms.
  COPY_MAC_FRAMEWORK(converter "${CEF_BINARY_DIR}" "${CONVERTER_APP}")

  foreach(_helper_target ${CONVERTER_HELPER_TARGETS})
    add_custom_command(TARGET converter POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "$<TARGET_BUNDLE_DIR:${_helper_target}>"
              "${CONVERTER_APP}/Contents/Frameworks/$<TARGET_BUNDLE_DIR_NAME:${_helper_target}>"
      VERBATIM)
  endforeach()

  set(CONVERTER_STAGE_DIR "${CONVERTER_APP}/Contents/Resources")

# ===========================================================================
#  Windows and Linux: a plain executable with everything beside it
# ===========================================================================
else()
  # WIN32 suppresses the console window on Windows and is ignored elsewhere.
  add_executable(converter WIN32 ${CONVERTER_SOURCES})
  SET_EXECUTABLE_TARGET_PROPERTIES(converter)

  target_include_directories(converter PRIVATE
    "${CEF_ROOT}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/res"
  )

  if(OS_WINDOWS)
    # version.h carries the project() version into the VERSIONINFO block, so
    # the exe's Properties dialog always matches the release it came from.
    configure_file(res/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)
    target_include_directories(converter PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    # So the .rc can #include "resource.h" and find converter.ico beside it.
    set_source_files_properties(res/converter.rc PROPERTIES
      INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/res;${CMAKE_CURRENT_BINARY_DIR}/generated")
  endif()

  target_link_libraries(converter PRIVATE
    conv_engine
    libcef_lib
    libcef_dll_wrapper
    ${CEF_STANDARD_LIBS}
  )

  target_compile_features(converter PRIVATE cxx_std_20)

  set_target_properties(converter PROPERTIES
    OUTPUT_NAME "converter"
    FOLDER "converter"
  )

  if(OS_LINUX)
    # libcef.so sits beside the executable, not on the system library path.
    # Without an $ORIGIN rpath the app only starts when LD_LIBRARY_PATH happens
    # to be set, which is exactly the kind of thing that works for the developer
    # and fails for everyone else.
    set_target_properties(converter PROPERTIES
      BUILD_WITH_INSTALL_RPATH TRUE
      INSTALL_RPATH "$ORIGIN"
    )
  endif()

  COPY_FILES(converter "${CEF_BINARY_FILES}"   "${CEF_BINARY_DIR}"   "${CEF_TARGET_OUT_DIR}")
  COPY_FILES(converter "${CEF_RESOURCE_FILES}" "${CEF_RESOURCE_DIR}" "${CEF_TARGET_OUT_DIR}")

  set(CONVERTER_STAGE_DIR "${CEF_TARGET_OUT_DIR}")
endif()

# ---------------------------------------------------------------------------
# Stage the UI, ffmpeg and the licences
# ---------------------------------------------------------------------------
#
# Runs as its own ALL target rather than as POST_BUILD on `converter`. A
# POST_BUILD command only fires when the executable actually relinks, so
# changing only the UI, or dropping a new ffmpeg build in place, would leave the
# staged copy stale -- which is exactly how the FFmpeg licence files went
# missing from a package once. The *_if_different copies make running this on
# every build essentially free.
set(STAGE_COMMANDS
  COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
          "${CMAKE_SOURCE_DIR}/ui/dist" "${CONVERTER_STAGE_DIR}/ui"
  # Licences that must travel with every package: ours, the third-party
  # summary, and CEF's own licence plus the Chromium credits it requires.
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/LICENSE" "${CONVERTER_STAGE_DIR}/LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CMAKE_SOURCE_DIR}/THIRD-PARTY-NOTICES.md" "${CONVERTER_STAGE_DIR}/THIRD-PARTY-NOTICES.md"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CEF_ROOT}/LICENSE.txt" "${CONVERTER_STAGE_DIR}/CEF-LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${CEF_ROOT}/CREDITS.html" "${CONVERTER_STAGE_DIR}/CEF-CREDITS.html")

if(EXISTS "${CONVERTER_FFMPEG_DIR}")
  if(OS_MAC)
    # The executables go in Contents/MacOS, the only bundle location code
    # signing accepts executables in; anywhere else Gatekeeper reports the app
    # as "damaged". Their licence and source offer go in Contents/Resources.
    # Both licence files are copied by name, so a package missing either one
    # fails to build rather than shipping without them.
    list(APPEND STAGE_COMMANDS
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${CONVERTER_FFMPEG_DIR}/ffmpeg" "${CONVERTER_FFMPEG_DIR}/ffprobe"
              "${CONVERTER_APP}/Contents/MacOS"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${CONVERTER_STAGE_DIR}/ffmpeg"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${CONVERTER_FFMPEG_DIR}/LICENSE.txt" "${CONVERTER_FFMPEG_DIR}/FFMPEG-SOURCE-OFFER.txt"
              "${CONVERTER_STAGE_DIR}/ffmpeg")
  else()
    list(APPEND STAGE_COMMANDS
      COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
              "${CONVERTER_FFMPEG_DIR}" "${CONVERTER_STAGE_DIR}/ffmpeg")
  endif()
else()
  message(WARNING
    "CONVERTER_FFMPEG_DIR does not exist: ${CONVERTER_FFMPEG_DIR}\n"
    "The build will run but the app will report that ffmpeg is missing.\n"
    "Run scripts/fetch-ffmpeg.sh <platform> to download the build that ships "
    "with the app, or point -DCONVERTER_FFMPEG_DIR at your own.")
endif()

if(OS_LINUX)
  # Desktop integration: the .desktop entry, its icon, and the per-user
  # install.sh. Configured into the binary dir first because the stage dir
  # contains a $<CONFIG> generator expression, which configure_file would take
  # literally and create a directory named "$<CONFIG>". Appended BEFORE
  # converter_stage exists -- a list(APPEND) afterwards would be ignored.
  set(CONVERTER_VERSION "${PROJECT_VERSION}")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/linux/converter.desktop.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/linux/videoconverter.desktop" @ONLY)
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/linux/install.sh.in"
                 "${CMAKE_CURRENT_BINARY_DIR}/linux/install.sh" @ONLY
                 FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                                  GROUP_READ GROUP_EXECUTE
                                  WORLD_READ WORLD_EXECUTE)
  list(APPEND STAGE_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/res/videoconverter.png"
            "${CMAKE_CURRENT_BINARY_DIR}/linux/videoconverter.desktop"
            "${CMAKE_CURRENT_BINARY_DIR}/linux/install.sh"
            "${CONVERTER_STAGE_DIR}")
endif()

add_custom_target(converter_stage ALL
  ${STAGE_COMMANDS}
  COMMENT "Staging the UI, ffmpeg and licences"
  VERBATIM)

add_dependencies(converter_stage converter)
if(CONVERTER_BUILD_UI)
  add_dependencies(converter_stage converter_ui)
endif()
set_target_properties(converter_stage PROPERTIES FOLDER "converter")
