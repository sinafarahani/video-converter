#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "include/cef_command_line.h"

namespace converter {

// Paths the app is asked to open by the operating system rather than by the
// page: command-line arguments on first launch, the arguments a second launch
// forwards to the running instance, and macOS "Open With" Apple Events.
//
// Only the browser process uses any of this.

// Positional (non-switch) arguments of |cl| as absolute UTF-8 paths, resolved
// against |cwd|; file:// URLs are decoded. Entries that do not exist are kept,
// in order: the bridge classifies every path and reports the ones it rejects.
//
// CefCommandLine has already done the switch parsing ("--x", "-x", and "/x" on
// Windows are switches; everything after a bare "--" is an argument), so this
// sees exactly what Chromium sees. Copies everything it needs, so a read-only
// command line handed to OnAlreadyRunningAppRelaunch is not retained.
std::vector<std::string> paths_from_command_line(CefRefPtr<CefCommandLine> cl,
                                                 const std::filesystem::path& cwd);

// Process-wide queue for paths that arrive before the client exists -- a macOS
// Apple Event, or a relaunch, landing before OnContextInitialized. Drained once
// the client is created. UI thread only (on macOS that is the main thread).
struct QueuedPaths {
    std::vector<std::string> paths;
    std::string source;  // "cmdline" | "relaunch" | "openWith"
};

void queue_launch_paths(std::vector<std::string> utf8_paths, std::string source);
std::vector<QueuedPaths> take_queued_launch_paths();

}  // namespace converter
