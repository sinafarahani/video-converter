#pragma once

#include <filesystem>

namespace converter {

// Directory containing the running executable. Everything the app needs --
// CEF's resources, the Vue bundle, the bundled ffmpeg -- is located relative
// to this rather than the working directory, which is whatever the shell
// happened to be in when the user launched us.
std::filesystem::path executable_dir();

// Where the bundled resources live: the Vue build in ui/ and the bundled
// ffmpeg in ffmpeg/.
//
// On Windows and Linux this is simply the executable's directory. On macOS the
// executable sits in Contents/MacOS while resources belong in
// Contents/Resources, so the two are NOT the same -- using executable_dir()
// there would look for ffmpeg inside Contents/MacOS and never find it.
std::filesystem::path resources_dir();

// Per-user writable location for logs, the encoder benchmark cache and CEF's
// own cache.
//
// Deliberately NOT next to the executable: the app installs per-user without
// administrator rights, but it may still land somewhere the user cannot write
// to, and the Java version's habit of writing `logs/` into the working
// directory scattered log files wherever it happened to be started from.
//
//   Windows  %LOCALAPPDATA%\VideoConverter
//   Linux    $XDG_DATA_HOME/VideoConverter, else ~/.local/share/VideoConverter
//   macOS    ~/Library/Application Support/VideoConverter
std::filesystem::path user_data_dir();

// Folder picking is NOT here on purpose. CefBrowserHost::RunFileDialog with
// FILE_DIALOG_OPEN_FOLDER gives a native picker on all three platforms through
// one asynchronous call, so there is no reason to hand-write IFileDialog on
// Windows, GTK on Linux and NSOpenPanel on macOS. See bridge.cc.

}  // namespace converter
