// The conversion pipeline: expand the inputs into media files, convert each
// one, report progress, and stop promptly when cancelled.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "conv/ffmpeg.hpp"
#include "conv/logging.hpp"
#include "conv/types.hpp"

namespace conv {

// Everything the UI needs to show that a multi-day encode is alive and where
// it is. The first version reported only the overall fraction, which for a
// folder of two hundred files rounds to "0%" for the first day and looks
// exactly like a hang.
struct Progress {
    double      fraction      = 0.0;  // 0..1 across the whole run
    double      file_fraction = 0.0;  // 0..1 within the current file
    int         file_index    = 0;    // 0-based
    int         file_count    = 0;
    std::string current_file;         // UTF-8

    // "probe" | "analyze" | "pass1" | "pass2" | "encode" | "remux" | "audio"
    std::string phase;
    int         pass       = 0;  // 1 or 2 during a two-pass encode, else 0
    int         pass_count = 0;

    // Straight from ffmpeg's progress output; 0 when not yet known.
    double fps      = 0.0;
    double speed    = 0.0;   // multiple of realtime
    double out_time = 0.0;   // seconds of output produced
    double duration = 0.0;   // seconds of input

    double elapsed_seconds = 0.0;  // since this file started
    double eta_seconds     = -1.0; // for this file; -1 when unknown
};

struct RunSummary {
    int converted = 0;
    int remuxed = 0;
    int skipped_exists = 0;
    int skipped_small = 0;
    int failed = 0;
    int skipped_not_media = 0;  // files given directly that are not audio/video
    int missing = 0;            // inputs that did not exist
    bool cancelled = false;
};

// Callbacks are invoked from the worker thread. The CEF layer marshals them
// onto the UI thread; nothing here touches the UI directly.
struct JobCallbacks {
    std::function<void(std::string_view)>   log;       // Persian, for the log pane
    std::function<void(const Progress&)>    progress;
    std::function<void(const RunSummary&)>  finished;
};

class JobRunner {
public:
    JobRunner(ToolPaths tools, RunLog& run_log, std::filesystem::path cache_dir);
    ~JobRunner();

    JobRunner(const JobRunner&)            = delete;
    JobRunner& operator=(const JobRunner&) = delete;

    // Validates the settings and, if they are usable, starts the worker thread.
    // Returns false and reports the reason through the log callback when the
    // settings are rejected -- matching the original's behaviour of refusing to
    // start on an empty input directory, an unparseable size, and so on.
    bool start(Settings settings, JobCallbacks callbacks);

    // Asks the run to stop. Kills the active ffmpeg process and removes the
    // temp file it was writing. Returns immediately; `finished` still fires.
    void cancel();

    bool running() const;

    // Blocks until the worker thread has exited. Called by the destructor.
    void join();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Pieces exposed for testing
// ---------------------------------------------------------------------------

// Recursively collects every regular file under `dir`, sorted by path, so the
// processing order -- and therefore any " (2)" names -- is the same on every
// OS rather than whatever order the file system enumerates in. Links to
// folders (symlinks, junctions) are not followed. A folder that cannot be
// listed is skipped and added to `unreadable` (when given); the walk goes on
// with the rest.
std::vector<std::filesystem::path> collect_files(const std::filesystem::path& dir,
                                                 std::vector<std::filesystem::path>* unreadable = nullptr);

// One media file the run will process.
struct PlannedFile {
    std::filesystem::path input;   // absolute
    std::filesystem::path root;    // folder it was found under, or input.parent_path() for a loose file
    bool video = false;
    std::filesystem::path output;  // final path incl. .mp4/.mp3 and any " (2)" suffix
    bool renamed = false;          // true when a " (n)" suffix was added
    // True when other files of this run map to the same output name (a.avi and
    // a.mkv -> a.mp4, a (2).mp4). Which of them an existing file came from
    // cannot be known, so the Skip policy says so instead of a plain skip.
    bool name_clash = false;
};

// Everything the run needs to know about its inputs, worked out before any
// file is touched.
struct InputPlan {
    std::vector<PlannedFile> files;                 // media only, in processing order
    std::vector<std::filesystem::path> missing;     // inputs that do not exist
    std::vector<std::filesystem::path> not_media;   // files given directly that are not audio/video
    int duplicates = 0;                             // same file reached twice
    int ignored_in_folders = 0;                     // non-media files inside folders (silent)
    int excluded_output_subtree = 0;                // files skipped because they sit in the output folder
    int found_inputs = 0;                           // inputs that exist (media or not), counted per entry
    std::vector<std::filesystem::path> unreadable_folders;  // folders the walk could not list
};

// Expands `s.inputs` (folders and/or files) into the list of media files to
// process and decides every output name up front, so that no output can land
// on an input of the same run or on another output. Reads the disk (existence,
// folder walks) but never writes to it.
InputPlan plan_inputs(const Settings& s);

// Pure: no disk writes. CopyTo target before the extension is applied.
std::filesystem::path output_target(const Settings& s, const std::filesystem::path& root,
                                    const std::filesystem::path& file);

// output_target() plus creating the target's parent directories.
std::filesystem::path build_output_path(const Settings& s,
                                        const std::filesystem::path& input_root,
                                        const std::filesystem::path& input_file);

// Splits the size budget between video and the selected audio tracks.
//
// The Java version reserved a flat 131072 bps for one AAC track and clamped the
// video to 2000 bps whenever the budget was tight -- which meant a small target
// still produced a file several times larger than requested, because the audio
// floor alone exceeded the budget. This accounts for every selected track and
// for container overhead, and reports when the target is simply not reachable.
struct BitrateBudget {
    std::uint64_t video_bps = 0;
    std::uint64_t audio_bps_each = 0;
    int           audio_track_count = 0;
    bool          feasible = false;  // false when even minimum quality overshoots
};
BitrateBudget compute_budget(std::uint64_t target_bytes,
                             double duration_seconds,
                             int audio_track_count);

}  // namespace conv
