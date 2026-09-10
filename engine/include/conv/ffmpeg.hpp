// Everything that shells out to ffmpeg / ffprobe.
//
// The app drives the FFmpeg command-line tools as separate processes rather
// than linking libavcodec. That is a deliberate licensing decision, not an
// accident: libx264 and libx265 are GPL, and linking them into this app would
// oblige the whole app to be GPL. Invoking them as separate programs over
// argv and pipes is the arm's-length arrangement that keeps that boundary
// intact. Do not "optimise" this into linked libraries.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "conv/types.hpp"

namespace conv {

// ---------------------------------------------------------------------------
// Locating the bundled tools
// ---------------------------------------------------------------------------

struct ToolPaths {
    std::filesystem::path ffmpeg;
    std::filesystem::path ffprobe;

    bool valid() const { return !ffmpeg.empty() && !ffprobe.empty(); }
};

// Looks for ffmpeg/ffprobe in this order:
//   1. <app_dir>/ffmpeg/            (where the installer puts them)
//   2. <app_dir>/
//   3. PATH
//
// The Java version only ever used PATH, which is why it silently did nothing
// on a machine where ffmpeg had not been installed separately.
ToolPaths locate_tools(const std::filesystem::path& app_dir);

// ---------------------------------------------------------------------------
// Running a child process
// ---------------------------------------------------------------------------

// A cancellable child process with line-oriented output callbacks.
//
// Implemented directly on the OS (CreateProcess + anonymous pipes + a job
// object on Windows; posix_spawn + pipes + poll elsewhere) rather than through a
// library. The previous implementation sat on reproc, which on Windows emulates
// pipes with TCP sockets over loopback so that it can use WSAPoll. A day-long
// encode on a locked-down org PC ran through that socket, hit a transient
// error, and the reader gave up while the child kept writing: the 4 KB buffer
// filled, ffmpeg blocked on write, and the app sat idle for days showing 0%.
// Real pipes, a reader that never abandons the child, and exit detection on the
// process handle itself are the fix.
//
// Both callbacks are invoked on the calling thread, once per complete line.
class Process {
public:
    Process();
    ~Process();

    Process(const Process&)            = delete;
    Process& operator=(const Process&) = delete;

    using LineHandler = std::function<void(std::string_view)>;

    // Called on the calling thread roughly every `idle_tick` while the child is
    // alive but has produced no output. Lets a caller run a watchdog or a
    // heartbeat without a second thread.
    using IdleHandler = std::function<void()>;

    // Why run() returned what it returned.
    enum class Outcome {
        Exited,      // ran to completion; the optional holds the exit code
        NotStarted,  // could not launch (missing binary, bad path)
        Cancelled,   // `cancel` became true; child was terminated
        Stopped,     // request_stop() was called; child was terminated
        Failed,      // an I/O failure on our side; child was terminated
    };

    // Runs to completion and returns the exit code, or std::nullopt if the
    // process did not run to completion -- see outcome() and error() for why.
    //
    // `cancel` is polled between reads. request_stop() may be called from a
    // callback on this thread or from another thread.
    //
    // The child is placed in a kill-on-close job object (Windows), so if this
    // application dies mid-encode the ffmpeg it spawned dies with it instead of
    // grinding on as an orphan.
    std::optional<int> run(const std::vector<std::string>& args,
                           LineHandler on_stdout,
                           LineHandler on_stderr,
                           const std::atomic_bool* cancel = nullptr,
                           IdleHandler on_idle = nullptr);

    // Terminates the running child, if any. Safe to call from another thread.
    void request_stop();

    Outcome            outcome() const;
    const std::string& error() const;  // human-readable detail for the log

    // Builds a single Windows command line from argv using the quoting rules
    // the MSVCRT parser expects. Exposed so it can be unit-tested; a wrong
    // implementation here corrupts every path containing a space or a quote.
    static std::string quote_windows_command_line(const std::vector<std::string>& args);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Convenience: run to completion, capture stdout, discard progress.
struct CaptureResult {
    bool        started = false;
    int         exit_code = -1;
    std::string out;
    std::string err;

    bool ok() const { return started && exit_code == 0; }
};
CaptureResult capture(const std::vector<std::string>& args);

// ---------------------------------------------------------------------------
// Probing
// ---------------------------------------------------------------------------

// Reads duration, resolution and the full audio stream list via a single
// `ffprobe -print_format json` call.
//
// The Java version made a separate bare-duration call per file and parsed the
// first line of stdout, which produced an unhandled divide-by-zero whenever the
// duration was missing or under one second.
MediaInfo probe_media(const ToolPaths& tools, const std::filesystem::path& file);

// Measures mean and peak level for every audio stream, filling in the
// mean_db / peak_db / has_content fields of `info.audio_streams`, then marks
// the ones to keep as `selected`.
//
// This is what replaces the blunt `-map 0`. Studio recordings routinely carry
// six microphone tracks where only one has anyone speaking on it, and which one
// varies per file, so the tracks have to be measured rather than assumed.
//
// Implemented as ONE ffmpeg pass with a volumedetect filter per audio stream,
// sampled at a few points through the file rather than decoding all of it.
bool analyze_audio(const ToolPaths& tools,
                   const std::filesystem::path& file,
                   MediaInfo& info,
                   const AudioDetectionConfig& cfg,
                   const std::atomic_bool* cancel = nullptr);

// Applies the selection rules to already-measured streams. Split out from
// analyze_audio so it can be unit-tested without invoking ffmpeg.
void select_audio_streams(MediaInfo& info, const AudioDetectionConfig& cfg);

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------

// Parses `out_time=HH:MM:SS.mmm` from an `-progress pipe:1` line.
std::optional<double> parse_progress_seconds(std::string_view line);

// One line of `-progress pipe:1` output, decoded.
//
// ffmpeg emits a block of key=value lines every stats period, ending with
// `progress=continue` or `progress=end`. The fields that matter for showing a
// user that a multi-day encode is alive are frame count, encoder fps, the
// speed multiplier and the output timestamp.
struct ProgressLine {
    enum class Kind { Frame, Fps, Speed, OutTime, Bitrate, TotalSize, End, Other };
    Kind   kind  = Kind::Other;
    double value = 0.0;  // frames, fps, x-speed, seconds, kbit/s, bytes
};
ProgressLine parse_progress_line(std::string_view line);

// Number of output audio streams implied by the current selection: every
// selected track counts once, except the follower half of a stereo pair, which
// is folded into its leader.
int count_output_audio_streams(const MediaInfo& info);

}  // namespace conv
