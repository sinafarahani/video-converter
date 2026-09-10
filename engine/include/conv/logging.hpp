// The on-disk run log.
//
// Format is unchanged from the Java version so old and new logs read the same:
//
//   logs/log_2026-08-08_14-22-01.txt
//   === Log started at 2026-08-08_14-22-01 ===
//   [14:22:03] Compression process started.
//   [14:22:41] SUCCESS: H:\media\clip.mp4
//
// One deliberate change: the Java app opened its log file in the controller's
// initialize(), so every launch produced a file even when the user converted
// nothing -- the original project has 39 log files, most containing only the
// header line. Here the file is created lazily on the first message, so an app
// that is opened and closed leaves no trace.
#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace conv {

class RunLog {
public:
    // `dir` is where log files are written; created on demand.
    explicit RunLog(std::filesystem::path dir);
    ~RunLog();

    RunLog(const RunLog&)            = delete;
    RunLog& operator=(const RunLog&) = delete;

    // Thread-safe. The first call opens the file and writes the header.
    void write(std::string_view message);

    // Path of the active log file, or empty if nothing has been written yet.
    std::filesystem::path path() const;

    // Flushes and closes. Safe to call more than once. The Java version never
    // closed its writer at all.
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace conv
