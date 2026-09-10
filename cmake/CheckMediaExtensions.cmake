# Keeps the OS file-type registrations in step with the engine.
#
# The extensions the engine converts are defined once, in engine/src/util.cpp
# (kVideoExtensions and kAudioExtensions). The OS integration needs them too,
# written out by hand in three other formats:
#
#   installer/converter.nsi          "Open with" on Windows (_ForEachMediaExt)
#   app/mac/Info.plist.in            UTIs imported for types macOS does not know
#   app/linux/converter.desktop.in   the MimeType= line
#
# Nothing else ties those copies to the engine, so this runs at configure time
# and fails with a message naming the file to fix when one has fallen behind.
# Plain text and regexes only, so it behaves the same on every platform. It
# also runs on its own, without a configure:
#
#   cmake -P cmake/CheckMediaExtensions.cmake

function(_converter_check_media_extensions root)
  get_filename_component(root "${root}" ABSOLUTE)

  # Accepted by the engine but deliberately not registered with the OS: raw
  # PCM has no header, so ffprobe cannot identify a .pcm file.
  set(not_registered pcm)

  # macOS: extensions a type macOS itself declares already covers
  # (public.mpeg-4, com.apple.quicktime-movie, public.avi, public.3gpp,
  # public.mp3, public.mpeg-4-audio, com.microsoft.waveform-audio,
  # public.aiff-audio, public.ulaw-audio). All of them conform to public.movie
  # or public.audio, which the plist lists. Every other extension needs an
  # imported com.sina0.videoconverter.<ext> type.
  set(mac_system_types mp4 mov avi 3gp mp3 m4a wav aiff aif au)

  # Linux: the MIME types shared-mime-info 2.4 gives each extension, all of
  # which MimeType= must carry ("-" where it has none, so nothing can be
  # listed). MimeType= may list more (aliases, sub-classes). A new engine
  # extension needs a row here before the configure passes again.
  set(linux_mime_types
    "mp4=video/mp4"
    "avi=video/vnd.avi,video/x-msvideo"
    "mkv=video/x-matroska"
    "mov=video/quicktime"
    "flv=video/x-flv"
    "wmv=video/x-ms-wmv"
    "mxf=application/mxf"
    "gxf=-"
    "lxf=-"
    "webm=video/webm"
    "3gp=video/3gpp"
    "ts=video/mp2t"
    "m2ts=video/mp2t"
    "ogv=video/ogg"
    "mp3=audio/mpeg"
    "aac=audio/aac"
    "wav=audio/vnd.wave,audio/x-wav"
    "flac=audio/flac"
    "ogg=audio/ogg"
    "opus=audio/x-opus+ogg"
    "wma=audio/x-ms-wma"
    "amr=audio/AMR"
    "ape=audio/x-ape"
    "aiff=audio/x-aiff"
    "aif=audio/x-aiff"
    "au=audio/basic"
    "mpc=audio/x-musepack"
    "oma=-"
    "aa3=-"
    "wv=audio/x-wavpack"
    "m4a=audio/mp4")

  set(util_cpp "${root}/engine/src/util.cpp")
  set(nsi      "${root}/installer/converter.nsi")
  set(plist    "${root}/app/mac/Info.plist.in")
  set(desktop  "${root}/app/linux/converter.desktop.in")
  foreach(f IN ITEMS "${util_cpp}" "${nsi}" "${plist}" "${desktop}")
    if(NOT EXISTS "${f}")
      message(FATAL_ERROR "Media extension check: ${f} does not exist.")
    endif()
  endforeach()

  set(problems "")

  # --- the engine's tables ---------------------------------------------------
  #   constexpr std::string_view kVideoExtensions[] = { "mp4", "avi", ... };
  file(READ "${util_cpp}" text)
  set(registered "")
  foreach(table kVideoExtensions kAudioExtensions)
    if(NOT text MATCHES "string_view[ \t]+${table}[ \t]*\\[[ \t]*\\][ \t\r\n]*=[ \t\r\n]*{([^}]*)}")
      message(FATAL_ERROR
        "Media extension check: ${util_cpp} has no array of the form\n"
        "  constexpr std::string_view ${table}[] = { \"ext\", ... };\n"
        "which is the shape this check parses. If the table was moved or "
        "renamed, update cmake/CheckMediaExtensions.cmake to match.")
    endif()
    # A commented-out entry is not accepted. [*], because \* is not a valid
    # escape in a quoted CMake argument.
    string(REGEX REPLACE "/[*]([^*]|[*]+[^*/])*[*]+/" "" body "${CMAKE_MATCH_1}")
    string(REGEX REPLACE "//[^\n]*" "" body "${body}")
    string(REGEX MATCHALL "\"[^\"]*\"" literals "${body}")
    if(NOT literals)
      message(FATAL_ERROR "Media extension check: ${table} in ${util_cpp} has no entries.")
    endif()
    foreach(literal IN LISTS literals)
      string(REGEX REPLACE "^\"(.*)\"$" "\\1" ext "${literal}")
      if(NOT ext MATCHES "^[a-z0-9]+$")
        list(APPEND problems "util.cpp: \"${ext}\" in ${table} is not a lowercase extension without a dot")
      elseif(NOT ext IN_LIST not_registered)
        list(APPEND registered "${ext}")
      endif()
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES registered)

  # --- Windows: installer/converter.nsi --------------------------------------
  file(READ "${nsi}" bom LIMIT 3 HEX)
  if(NOT bom STREQUAL "efbbbf")
    list(APPEND problems "converter.nsi: must be saved as UTF-8 with a byte-order mark. Without one makensis reads it in the ANSI code page and garbles the Persian verb label.")
  endif()
  file(READ "${nsi}" text)
  string(FIND "${text}" "!macro _ForEachMediaExt" start)
  if(start LESS 0)
    list(APPEND problems "converter.nsi: no \"!macro _ForEachMediaExt\" block")
  else()
    string(SUBSTRING "${text}" ${start} -1 block)
    string(FIND "${block}" "!macroend" end)
    string(SUBSTRING "${block}" 0 ${end} block)
    # makensis ignores /* */ blocks and everything from ; or # to the line end,
    # so an entry commented out that way registers nothing. The block holds no
    # string that could contain ; or #.
    string(REGEX REPLACE "/[*]([^*]|[*]+[^*/])*[*]+/" "" block "${block}")
    string(REGEX REPLACE "[;#][^\n]*" "" block "${block}")
    string(REGEX MATCHALL "\"\\.[A-Za-z0-9]+\"" quoted "${block}")
    set(nsi_exts "")
    foreach(q IN LISTS quoted)
      string(REGEX REPLACE "^\"\\.(.*)\"$" "\\1" ext "${q}")
      string(TOLOWER "${ext}" ext)
      list(APPEND nsi_exts "${ext}")
    endforeach()
    foreach(ext IN LISTS registered)
      if(NOT ext IN_LIST nsi_exts)
        list(APPEND problems "converter.nsi: .${ext} is missing from _ForEachMediaExt")
      endif()
    endforeach()
    foreach(ext IN LISTS nsi_exts)
      if(NOT ext IN_LIST registered)
        list(APPEND problems "converter.nsi: _ForEachMediaExt registers .${ext}, which the engine does not accept or which must not be registered")
      endif()
    endforeach()
  endif()

  # --- macOS: app/mac/Info.plist.in ------------------------------------------
  file(READ "${plist}" text)
  # A type inside an XML comment is declared nowhere.
  string(REGEX REPLACE "<!--([^-]|-[^-])*-->" "" text "${text}")
  foreach(parent public.movie public.audio)
    string(FIND "${text}" "<string>${parent}</string>" at)
    if(at LESS 0)
      list(APPEND problems "Info.plist.in: ${parent} is missing from CFBundleDocumentTypes")
    endif()
  endforeach()
  foreach(ext IN LISTS registered)
    if(ext IN_LIST mac_system_types)
      continue()
    endif()
    # Once as UTTypeIdentifier, once in the media entry's LSItemContentTypes.
    string(REGEX MATCHALL "<string>com\\.sina0\\.videoconverter\\.${ext}</string>" ids "${text}")
    list(LENGTH ids n)
    string(FIND "${text}" "<string>${ext}</string>" tag)
    if(n LESS 2 OR tag LESS 0)
      list(APPEND problems "Info.plist.in: .${ext} has no system type, so com.sina0.videoconverter.${ext} must be declared in UTImportedTypeDeclarations (tagged with ${ext}) and listed in LSItemContentTypes")
    endif()
  endforeach()
  string(REGEX MATCHALL "com\\.sina0\\.videoconverter\\.[a-z0-9]+" ids "${text}")
  if(ids)
    list(REMOVE_DUPLICATES ids)
  endif()
  foreach(id IN LISTS ids)
    string(REGEX REPLACE "^com\\.sina0\\.videoconverter\\." "" ext "${id}")
    if(NOT ext IN_LIST registered)
      list(APPEND problems "Info.plist.in: declares ${id}, which the engine does not accept or which must not be registered")
    endif()
  endforeach()

  # --- Linux: app/linux/converter.desktop.in ---------------------------------
  file(READ "${desktop}" text)
  if(NOT text MATCHES "(^|\n)MimeType=([^\r\n]*)")
    list(APPEND problems "converter.desktop.in: no MimeType= line")
  else()
    set(mime "${CMAKE_MATCH_2}")   # ';'-separated, so already a CMake list
    list(REMOVE_ITEM mime "")
    foreach(row IN LISTS linux_mime_types)
      if(row MATCHES "^([a-z0-9]+)=(.+)$")
        set(types_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
      endif()
    endforeach()
    foreach(ext IN LISTS registered)
      if(NOT DEFINED types_${ext})
        list(APPEND problems "CheckMediaExtensions.cmake: no Linux MIME type is known for .${ext}. Look it up in shared-mime-info, add a row to linux_mime_types (\"-\" if it has none) and add the type to MimeType= in converter.desktop.in")
      elseif(NOT types_${ext} STREQUAL "-")
        string(REPLACE "," ";" wanted "${types_${ext}}")
        foreach(type IN LISTS wanted)
          if(NOT type IN_LIST mime)
            list(APPEND problems "converter.desktop.in: MimeType= lacks ${type} (for .${ext})")
          endif()
        endforeach()
      endif()
    endforeach()
    if("inode/directory" IN_LIST mime)
      list(APPEND problems "converter.desktop.in: MimeType= must not list inode/directory. Wherever no default folder handler is set, the desktop would then open this app for every folder.")
    endif()
  endif()

  if(problems)
    list(LENGTH problems n)
    string(JOIN "\n  " details ${problems})
    message(FATAL_ERROR
      "The OS file-type registrations disagree with the engine's extension "
      "tables in engine/src/util.cpp (${n} problem(s)):\n  ${details}\n"
      "Every extension the engine accepts, except .pcm, is registered on all "
      "three platforms.")
  endif()
  list(LENGTH registered n)
  message(STATUS "Media extensions: ${n} registered with the OS; converter.nsi, Info.plist.in and converter.desktop.in agree with the engine")
endfunction()

_converter_check_media_extensions("${CMAKE_CURRENT_LIST_DIR}/..")
