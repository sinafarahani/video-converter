#include "conv/job.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <random>
#include <system_error>
#include <thread>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "conv/gpu.hpp"
#include "conv/messages.hpp"
#include "conv/util.hpp"

namespace conv {
namespace {

namespace fs    = std::filesystem;
using     clock = std::chrono::steady_clock;

// Every output audio stream is encoded at this rate. The original hardcoded
// 128 kbps for a single AAC track; keeping the same figure means output quality
// is unchanged for the common one-track case. A stereo pair joined into one
// stream counts as one stream here, so it does not cost twice.
constexpr std::uint64_t kAudioBitsPerSecond = 128'000;

// MP4 muxing overhead is roughly half a percent. Reserving 2% keeps the result
// under the requested size rather than a hair over it, which is the direction
// that matters when a user asks for "at most 1 GB".
constexpr double kContainerOverheadFraction = 0.02;

// Below this the picture is unwatchable and there is no point pretending the
// target is reachable.
constexpr std::uint64_t kMinimumVideoBitrate = 100'000;

#ifdef _WIN32
constexpr const char* kNullSink = "NUL";
#else
constexpr const char* kNullSink = "/dev/null";
#endif

// Liveness thresholds for a long encode.
constexpr auto kHeartbeatLog   = std::chrono::minutes(10);  // run log line
constexpr auto kHeartbeatUi    = std::chrono::minutes(30);  // UI log line
constexpr auto kStallLog       = std::chrono::minutes(5);   // first warning in the run log
constexpr auto kStallUi        = std::chrono::minutes(15);  // warning in the UI
constexpr auto kStallUiRepeat  = std::chrono::minutes(30);

// A short random hex token, used to give temp files and two-pass stats files a
// name that cannot collide between concurrent runs.
std::string random_token() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    return fmt::format("{:012x}", rng() & 0xFFFFFFFFFFFFull);
}

// x264 and x265 leave several files behind for a two-pass run: the stats file,
// an mbtree (x264) or cutree (x265) companion, and .temp variants if the
// process is killed mid-write. The Java version passed no -passlogfile at all,
// so all of this landed in the working directory and stayed there.
//
// `prefix` is built from an ASCII token, so appending these suffixes as narrow
// strings is safe -- no user-supplied filename ever reaches this path.
void remove_pass_logs(const fs::path& prefix) {
    std::error_code ec;
    for (const char* suffix : {"-0.log", "-0.log.mbtree", "-0.log.cutree",
                               "-0.log.temp", "-0.log.mbtree.temp", "-0.log.cutree.temp",
                               ".log", ".log.mbtree", ".log.cutree"}) {
        fs::path p = prefix;
        p += suffix;
        fs::remove(p, ec);
    }
}

bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && fs::is_regular_file(p, ec);
}

std::uint64_t file_size_or_zero(const fs::path& p) {
    std::error_code ec;
    const auto n = fs::file_size(p, ec);
    return ec ? 0ull : n;
}

// The command line as it will be run, for the log. Quoting makes the output
// copy-pasteable into a shell, which is what someone debugging a stuck encode
// from the log actually needs.
std::string command_for_log(const std::vector<std::string>& args) {
    std::string out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) out.push_back(' ');
        const bool plain = args[i].find_first_of(" \t\"'") == std::string::npos && !args[i].empty();
        if (plain) {
            out += args[i];
        } else {
            out.push_back('"');
            for (const char c : args[i]) {
                if (c == '"') out += "\\\"";
                else          out.push_back(c);
            }
            out.push_back('"');
        }
    }
    return out;
}

std::string human_duration(double seconds) {
    if (seconds < 0) return "?";
    const auto s = static_cast<long long>(seconds);
    if (s < 60)   return fmt::format("{}s", s);
    if (s < 3600) return fmt::format("{}m{:02d}s", s / 60, s % 60);
    return fmt::format("{}h{:02d}m", s / 3600, (s % 3600) / 60);
}

// ---------------------------------------------------------------------------
// Temp-file journal
// ---------------------------------------------------------------------------
//
// Every temp output this engine creates is recorded here before ffmpeg starts
// writing it, and removed once it has been renamed into place or deleted. If
// the application dies mid-encode -- killed from Task Manager after appearing
// hung, a crash, a power cut -- the next start sweeps whatever is still listed.
// Without this, a multi-gigabyte half-written file sits in the user's video
// folder indefinitely, which is exactly what happened the first time the app
// was terminated during a stalled encode.
//
// Only paths we created are ever listed, and they all carry the _tmp<token>
// marker, so the sweep cannot touch a user's own file.
class TempJournal {
public:
    explicit TempJournal(fs::path dir) : file_(std::move(dir) / "pending-temp.json") {}

    void add(const fs::path& p) {
        std::lock_guard lock(mutex_);
        auto list = load();
        const std::string s = path_to_utf8(p);
        if (std::find(list.begin(), list.end(), s) == list.end()) list.push_back(s);
        save(list);
    }

    void remove(const fs::path& p) {
        std::lock_guard lock(mutex_);
        auto list = load();
        const std::string s = path_to_utf8(p);
        list.erase(std::remove(list.begin(), list.end(), s), list.end());
        save(list);
    }

    // Deletes every listed file that still exists. Returns what it removed.
    std::vector<std::string> sweep() {
        std::lock_guard lock(mutex_);
        std::vector<std::string> removed;
        for (const auto& s : load()) {
            const fs::path p = path_from_utf8(s);
            // Belt and braces: only our own naming pattern.
            if (s.find("_tmp") == std::string::npos) continue;
            std::error_code ec;
            if (fs::exists(p, ec) && fs::remove(p, ec)) removed.push_back(s);
        }
        save({});
        return removed;
    }

private:
    std::vector<std::string> load() const {
        std::ifstream in(file_);
        if (!in) return {};
        try {
            nlohmann::json j;
            in >> j;
            if (j.is_array()) return j.get<std::vector<std::string>>();
        } catch (...) {
        }
        return {};
    }

    void save(const std::vector<std::string>& list) const {
        std::error_code ec;
        fs::create_directories(file_.parent_path(), ec);
        std::ofstream out(file_, std::ios::trunc);
        if (out) out << nlohmann::json(list).dump();
    }

    fs::path           file_;
    mutable std::mutex mutex_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

std::vector<fs::path> collect_files(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;

    // recursive_directory_iterator with an error_code overload skips entries it
    // cannot read (permission denied, broken junctions) instead of throwing.
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec) return out;

    for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        std::error_code sec;
        if (it->is_regular_file(sec)) out.push_back(it->path());
    }
    return out;
}

fs::path build_output_path(const Settings& s,
                           const fs::path& input_root,
                           const fs::path& input_file) {
    if (s.output_mode == OutputMode::Replace) return input_file;

    std::error_code ec;
    const fs::path relative = fs::relative(input_file, input_root, ec);
    const fs::path target = ec ? (s.output_dir / input_file.filename())
                               : (s.output_dir / relative);

    if (target.has_parent_path()) {
        std::error_code mec;
        fs::create_directories(target.parent_path(), mec);
    }
    return target;
}

BitrateBudget compute_budget(std::uint64_t target_bytes,
                             double duration_seconds,
                             int audio_track_count) {
    BitrateBudget b;
    b.audio_track_count = std::max(0, audio_track_count);
    b.audio_bps_each    = kAudioBitsPerSecond;

    if (duration_seconds <= 0.0 || target_bytes == 0) return b;

    const double usable = static_cast<double>(target_bytes) * (1.0 - kContainerOverheadFraction);
    const double total_bps = usable * 8.0 / duration_seconds;
    const double audio_bps =
        static_cast<double>(kAudioBitsPerSecond) * static_cast<double>(b.audio_track_count);

    const double video_bps = total_bps - audio_bps;
    if (video_bps < static_cast<double>(kMinimumVideoBitrate)) {
        // Even at the floor the audio alone overruns the target. The original
        // clamped video to 2000 bps here and carried on, which produced a file
        // several times the requested size while reporting success.
        b.video_bps = kMinimumVideoBitrate;
        b.feasible  = false;
        return b;
    }

    b.video_bps = static_cast<std::uint64_t>(video_bps);
    b.feasible  = true;
    return b;
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

struct JobRunner::Impl {
    ToolPaths     tools;
    RunLog&       run_log;
    fs::path      cache_dir;
    TempJournal   journal;

    Settings      settings;
    JobCallbacks  cb;
    EncoderChoice encoder;

    std::atomic_bool cancel{false};
    std::atomic_bool running{false};
    std::thread      worker;

    // The process currently encoding, so cancel() can stop it, plus the temp
    // file it is writing so cancel() can remove it.
    std::mutex   active_mutex;
    Process*     active_process = nullptr;
    fs::path     active_temp;

    int file_count = 0;
    int file_index = 0;

    // Per-file progress state, for the richer Progress report.
    Progress          progress;
    clock::time_point file_started{};

    Impl(ToolPaths t, RunLog& l, fs::path cd)
        : tools(std::move(t)), run_log(l), cache_dir(std::move(cd)), journal(cache_dir) {}

    bool cancelled() const { return cancel.load(std::memory_order_relaxed); }

    void ui(std::string_view text) {
        if (cb.log) cb.log(text);
    }
    void file_log(std::string_view text) { run_log.write(text); }

    // -- progress ---------------------------------------------------------------

    void begin_file(const fs::path& current, double duration) {
        progress              = Progress{};
        progress.file_index   = file_index;
        progress.file_count   = file_count;
        progress.current_file = path_to_utf8(current);
        progress.duration     = duration;
        file_started          = clock::now();
    }

    void set_phase(std::string phase, int pass, int pass_count) {
        progress.phase      = std::move(phase);
        progress.pass       = pass;
        progress.pass_count = pass_count;
        progress.fps        = 0.0;
        progress.speed      = 0.0;
        progress.out_time   = 0.0;
        progress.eta_seconds = -1.0;
        emit_progress(progress.file_fraction);
    }

    void emit_progress(double within_file) {
        if (!cb.progress || file_count <= 0) return;
        progress.file_fraction = std::clamp(within_file, 0.0, 1.0);
        progress.fraction = std::clamp(
            (file_index + progress.file_fraction) / static_cast<double>(file_count), 0.0, 1.0);
        progress.elapsed_seconds = static_cast<double>(
            std::chrono::duration_cast<std::chrono::seconds>(clock::now() - file_started).count());
        cb.progress(progress);
    }

    // Runs one ffmpeg invocation, forwarding progress. `base` and `span` place
    // this invocation inside the current file's slice of the progress bar.
    //
    // Also the liveness layer: a heartbeat in the log while things are moving,
    // and an escalating warning when ffmpeg stops reporting while still alive.
    // The first field deployment of this app sat for days on a CPU encode that
    // showed 0% the whole time; whether or not it was actually stuck was
    // unknowable from the outside. It should never be unknowable again.
    std::optional<int> run_ffmpeg(std::vector<std::string> args,
                                  double base,
                                  double span,
                                  int remaining_passes_after_this) {
        Process proc;
        {
            std::lock_guard lock(active_mutex);
            active_process = &proc;
        }

        file_log(fmt::format("exec: {}", command_for_log(args)));
        const auto started = clock::now();

        auto last_output    = started;   // anything on stdout
        auto last_heartbeat = started;
        auto last_ui_beat   = started;
        auto last_stall_ui  = clock::time_point{};
        bool stall_logged   = false;

        const double duration = progress.duration;

        const auto on_stdout = [&](std::string_view line) {
            last_output = clock::now();
            if (stall_logged) {
                file_log("ffmpeg is reporting progress again");
                stall_logged = false;
            }

            const auto p = parse_progress_line(line);
            switch (p.kind) {
                case ProgressLine::Kind::Fps:
                    if (p.value >= 0) progress.fps = p.value;
                    break;
                case ProgressLine::Kind::Speed:
                    if (p.value >= 0) progress.speed = p.value;
                    break;
                case ProgressLine::Kind::OutTime:
                    if (p.value >= 0) progress.out_time = p.value;
                    break;
                case ProgressLine::Kind::End:
                    // The block ends here; update the bar and the ETA once
                    // per block rather than per field.
                    break;
                default:
                    return;
            }
            if (p.kind != ProgressLine::Kind::End && p.kind != ProgressLine::Kind::OutTime) return;
            if (duration <= 0.0) return;

            const double frac = std::clamp(progress.out_time / duration, 0.0, 1.0);
            if (progress.speed > 0.0) {
                const double remaining_here = (duration - progress.out_time) / progress.speed;
                const double remaining_more = remaining_passes_after_this * (duration / progress.speed);
                progress.eta_seconds = remaining_here + remaining_more;
            }
            emit_progress(base + span * frac);

            const auto now = clock::now();
            if (now - last_heartbeat >= kHeartbeatLog) {
                last_heartbeat = now;
                file_log(fmt::format("  ... {} {:.1f}% fps={:.2f} speed={:.4f}x out_time={} eta={}",
                                     progress.phase, progress.file_fraction * 100.0, progress.fps,
                                     progress.speed, human_duration(progress.out_time),
                                     human_duration(progress.eta_seconds)));
            }
            if (now - last_ui_beat >= kHeartbeatUi) {
                last_ui_beat = now;
                ui(msg::still_working(progress.phase, progress.file_fraction * 100.0, progress.fps,
                                      progress.speed));
            }
        };

        const auto on_stderr = [&](std::string_view line) {
            if (!line.empty()) file_log(fmt::format("[ffmpeg] {}", line));
        };

        // Called ~every 150 ms while ffmpeg is alive and silent.
        const auto on_idle = [&]() {
            const auto now    = clock::now();
            const auto silent = now - last_output;

            if (!stall_logged && silent >= kStallLog) {
                stall_logged = true;
                file_log(fmt::format(
                    "WARNING: no progress output from ffmpeg for {} minutes; process still running "
                    "(phase {}, last out_time {})",
                    std::chrono::duration_cast<std::chrono::minutes>(silent).count(), progress.phase,
                    human_duration(progress.out_time)));
            }
            if (silent >= kStallUi) {
                const bool due = (last_stall_ui == clock::time_point{}) ||
                                 (now - last_stall_ui >= kStallUiRepeat);
                if (due) {
                    last_stall_ui = now;
                    ui(msg::stall_warning(
                        static_cast<int>(std::chrono::duration_cast<std::chrono::minutes>(silent).count())));
                }
            }
        };

        const auto code = proc.run(args, on_stdout, on_stderr, &cancel, on_idle);

        {
            std::lock_guard lock(active_mutex);
            active_process = nullptr;
        }

        const auto secs =
            std::chrono::duration_cast<std::chrono::seconds>(clock::now() - started).count();
        if (code) {
            file_log(fmt::format("exit {} after {}", *code, human_duration(static_cast<double>(secs))));
        } else {
            const char* why = "unknown";
            switch (proc.outcome()) {
                case Process::Outcome::NotStarted: why = "could not start"; break;
                case Process::Outcome::Cancelled:  why = "cancelled by user"; break;
                case Process::Outcome::Stopped:    why = "stopped"; break;
                case Process::Outcome::Failed:     why = "I/O failure"; break;
                default: break;
            }
            file_log(fmt::format("ffmpeg did not complete: {} after {}{}", why,
                                 human_duration(static_cast<double>(secs)),
                                 proc.error().empty() ? "" : " -- " + proc.error()));
        }
        return code;
    }

    void set_temp(const fs::path& p) {
        {
            std::lock_guard lock(active_mutex);
            active_temp = p;
        }
        journal.add(p);
    }
    void clear_temp() {
        fs::path p;
        {
            std::lock_guard lock(active_mutex);
            p = active_temp;
            active_temp.clear();
        }
        if (!p.empty()) journal.remove(p);
    }

    // Output must exist, be non-empty, and be readable as media. Same check the
    // original made, but via the probe we already have.
    bool output_valid(const fs::path& p) {
        if (!file_exists(p) || file_size_or_zero(p) == 0) return false;
        const auto info = probe_media(tools, p);
        return info.valid;
    }

    // Moves `temp` into place, honouring Replace vs CopyTo, and reports.
    //
    // This is where the original's most damaging bug lived: in Replace mode it
    // compared the input path against an output path whose extension had
    // already been rewritten to .mp4, decided they were "different files", and
    // so wrote clip.mp4 while leaving the original clip.avi sitting next to it.
    // Only inputs already named .mp4 were genuinely replaced.
    bool finalize(const fs::path& input, const fs::path& output, const fs::path& temp) {
        std::error_code ec;

        if (!same_path(input, output)) {
            if (file_exists(output)) {
                fs::remove(output, ec);
                if (ec || file_exists(output)) {
                    ui(msg::replace_failed(path_to_utf8(output)));
                    file_log(fmt::format("Failed to replace file: {}", path_to_utf8(output)));
                    return false;
                }
            }
            fs::rename(temp, output, ec);
            if (ec) {
                // Falls back to copy+delete when temp and output are on
                // different volumes, which rename cannot cross.
                std::error_code cec;
                fs::copy_file(temp, output, fs::copy_options::overwrite_existing, cec);
                if (cec) {
                    ui(msg::rename_failed(path_to_utf8(output)));
                    file_log(fmt::format("Failed to rename into place: {}", path_to_utf8(output)));
                    return false;
                }
                fs::remove(temp, cec);
            }

            // The fix: in Replace mode the source file is meant to be gone.
            if (settings.output_mode == OutputMode::Replace && file_exists(input)) {
                std::error_code rec;
                fs::remove(input, rec);
                if (rec) {
                    file_log(fmt::format("Converted but could not remove original: {}",
                                         path_to_utf8(input)));
                }
            }
        } else {
            if (file_exists(input)) {
                fs::remove(input, ec);
                if (ec || file_exists(input)) {
                    ui(msg::original_delete_failed(path_to_utf8(input)));
                    file_log(fmt::format("Failed to delete file: {}", path_to_utf8(input)));
                    return false;
                }
            }
            fs::rename(temp, input, ec);
            if (ec) {
                ui(msg::rename_failed(path_to_utf8(input)));
                file_log(fmt::format("Failed to rename file: {}", path_to_utf8(input)));
                return false;
            }
        }

        const auto shown = path_to_utf8(output);
        ui(msg::file_saved(shown));
        file_log(fmt::format("SUCCESS: {}", shown));
        return true;
    }

    void discard_temp(const fs::path& temp) {
        if (!file_exists(temp)) return;
        std::error_code ec;
        fs::remove(temp, ec);
        if (ec) {
            ui(msg::temp_delete_failed(path_to_utf8(temp)));
            file_log(fmt::format("Failed to delete temp file: {}", path_to_utf8(temp)));
        }
    }

    // Logs the audio decision and reports the stereo pairs to the user.
    void report_audio(const MediaInfo& info) {
        if (info.audio_streams.size() <= 1) return;

        const int kept = static_cast<int>(std::count_if(
            info.audio_streams.begin(), info.audio_streams.end(),
            [](const AudioStreamInfo& a) { return a.selected; }));
        ui(msg::audio_selected(kept, static_cast<int>(info.audio_streams.size())));

        for (const auto& a : info.audio_streams) {
            std::string note;
            if (a.is_pair_leader())        note = fmt::format(" (L of pair with {})", a.stereo_partner);
            else if (a.is_pair_follower()) note = fmt::format(" (R of pair with {})", a.stereo_partner);
            file_log(fmt::format("  audio {}: {} ch {}, mean {:.1f} dB, peak {:.1f} dB -> {}{}",
                                 a.audio_index, a.channels, a.codec_name, a.mean_db, a.peak_db,
                                 a.selected ? "KEEP" : "drop", note));
        }
        for (const auto& a : info.audio_streams) {
            if (a.is_pair_leader()) ui(msg::audio_stereo_pair(a.audio_index, a.stereo_partner));
        }
    }

    // -- the converters ----------------------------------------------------------

    FileResult convert_video(const fs::path& input, fs::path output);
    FileResult convert_audio(const fs::path& input, fs::path output);
    FileResult remux_video(const fs::path& input, fs::path output, const MediaInfo& info);

    void run();
};

// ---------------------------------------------------------------------------

FileResult JobRunner::Impl::remux_video(const fs::path& input, fs::path output,
                                        const MediaInfo& info) {
    output = with_extension(output, ".mp4");
    const fs::path temp = make_temp_sibling(output, ".mp4");
    set_temp(temp);
    set_phase("remux", 0, 0);

    std::vector<std::string> args{path_to_utf8(tools.ffmpeg), "-nostdin", "-y", "-v", "error",
                                  "-i", path_to_utf8(input)};

    if (info.has_video) {
        args.push_back("-map");
        args.push_back("0:v:0");
    }
    // Stream copy cannot join two mono tracks into one stereo stream, so a
    // detected pair is carried as two mono streams here. It is a remux: the
    // whole point is not touching the data.
    for (const auto& a : info.audio_streams) {
        if (!a.selected) continue;
        args.push_back("-map");
        args.push_back(fmt::format("0:a:{}", a.audio_index));
    }

    args.insert(args.end(), {"-c", "copy"});
    args.push_back(path_to_utf8(temp));
    args.insert(args.end(), {"-progress", "pipe:1", "-nostats", "-hide_banner"});

    const auto code = run_ffmpeg(args, 0.0, 1.0, 0);
    if (!code) {
        discard_temp(temp);
        clear_temp();
        return FileResult::Cancelled;
    }
    if (*code != 0 || !output_valid(temp)) {
        ui(msg::file_failed(path_to_utf8(input)));
        file_log(fmt::format("Remux failed: {}", path_to_utf8(input)));
        discard_temp(temp);
        clear_temp();
        return FileResult::Failed;
    }

    const bool ok = finalize(input, output, temp);
    clear_temp();
    return ok ? FileResult::Remuxed : FileResult::Failed;
}

FileResult JobRunner::Impl::convert_video(const fs::path& input, fs::path output) {
    ui(msg::file_started(path_to_utf8(input)));
    begin_file(input, 0.0);
    set_phase("probe", 0, 0);

    MediaInfo info = probe_media(tools, input);
    if (!info.valid) {
        ui(msg::file_failed(path_to_utf8(input)));
        file_log(fmt::format("Could not read media: {}", path_to_utf8(input)));
        return FileResult::Failed;
    }
    progress.duration = info.duration_seconds;
    file_log(fmt::format("{}: {:.1f}s, {}x{} {}, {} audio stream(s), {}", path_to_utf8(input),
                         info.duration_seconds, info.width, info.height, info.video_codec_name,
                         info.audio_streams.size(), format_bytes(file_size_or_zero(input))));

    // Which microphone was actually live.
    if (!info.audio_streams.empty()) {
        set_phase("analyze", 0, 0);
        const auto t0 = clock::now();
        analyze_audio(tools, input, info, AudioDetectionConfig{}, &cancel);
        if (cancelled()) return FileResult::Cancelled;
        file_log(fmt::format("audio analysis took {}",
                             human_duration(static_cast<double>(
                                 std::chrono::duration_cast<std::chrono::seconds>(clock::now() - t0)
                                     .count()))));
        report_audio(info);
    }

    const int output_audio = count_output_audio_streams(info);

    // Already small enough? Re-encoding would only make it bigger.
    if (settings.size_mode == SizeMode::Manual) {
        const std::uint64_t current = file_size_or_zero(input);
        if (current > 0 && current <= settings.max_size_bytes) {
            ui(msg::already_small_enough(path_to_utf8(input), current, settings.max_size_bytes));
            file_log(fmt::format("Already within target, remuxing: {}", path_to_utf8(input)));
            return remux_video(input, std::move(output), info);
        }
    }

    output = with_extension(output, ".mp4");
    const fs::path temp = make_temp_sibling(output, ".mp4");
    set_temp(temp);

    const bool two_pass = uses_two_pass(encoder) && settings.size_mode == SizeMode::Manual;

    // Two-pass stats go next to the temp output rather than into %TEMP%.
    // x265's cutree for a two-hour 1080p source runs to gigabytes; the output
    // drive is the one place guaranteed to have room for that, whereas a small
    // system drive fills, x265 fails to write, and the encode dies a day in.
    // ASCII token only, so no Persian ever reaches the narrow-string suffixes.
    const fs::path pass_log =
        temp.parent_path() / path_from_utf8(fmt::format("converter-stats-{}", random_token()));

    BitrateBudget budget;
    if (settings.size_mode == SizeMode::Manual) {
        budget = compute_budget(settings.max_size_bytes, info.duration_seconds, output_audio);
        file_log(fmt::format("budget: target {}, video {} kbps, {} audio stream(s) at {} kbps{}",
                             format_bytes(settings.max_size_bytes), budget.video_bps / 1000,
                             output_audio, budget.audio_bps_each / 1000,
                             budget.feasible ? "" : " -- TARGET UNREACHABLE, encoding at minimum"));
    }

    // Assembles one ffmpeg command line for the given pass.
    const auto build = [&](int pass, const fs::path& dest) {
        std::vector<std::string> a{path_to_utf8(tools.ffmpeg), "-nostdin", "-y", "-v", "error",
                                   "-i", path_to_utf8(input)};

        const bool want_audio = (pass != 1) && output_audio > 0;

        // Stereo pairs are joined with a filter; everything else maps directly.
        std::string filter;
        std::vector<std::string> audio_maps;
        if (want_audio) {
            int pair_no = 0;
            for (const auto& s : info.audio_streams) {
                if (!s.selected || s.is_pair_follower()) continue;
                if (s.is_pair_leader()) {
                    const std::string label = fmt::format("[pair{}]", pair_no++);
                    filter += fmt::format("[0:a:{}][0:a:{}]join=inputs=2:channel_layout=stereo{};",
                                          s.audio_index, s.stereo_partner, label);
                    audio_maps.push_back(label);
                } else {
                    audio_maps.push_back(fmt::format("0:a:{}", s.audio_index));
                }
            }
            if (!filter.empty()) filter.pop_back();  // trailing ';'
        }

        if (!filter.empty()) {
            a.push_back("-filter_complex");
            a.push_back(filter);
        }

        a.push_back("-map");
        a.push_back("0:v:0");
        for (const auto& m : audio_maps) {
            a.push_back("-map");
            a.push_back(m);
        }

        a.push_back("-c:v");
        a.push_back(encoder.ffmpeg_encoder);

        const std::string preset = preset_arg(encoder, settings.speed);
        if (!preset.empty()) {
            a.push_back("-preset");
            a.push_back(preset);
        }

        if (settings.size_mode == SizeMode::Manual) {
            const auto br = bitrate_args(encoder, budget.video_bps);
            a.insert(a.end(), br.begin(), br.end());
        } else {
            const auto q = quality_args(encoder, kOutputCodec, settings.level);
            a.insert(a.end(), q.begin(), q.end());
        }

        const auto compat = compatibility_args(encoder, kOutputCodec);
        a.insert(a.end(), compat.begin(), compat.end());

        if (two_pass) {
            a.push_back("-pass");
            a.push_back(std::to_string(pass));
            a.push_back("-passlogfile");
            a.push_back(path_to_utf8(pass_log));
        }

        if (pass == 1) {
            a.push_back("-an");
            a.push_back("-f");
            a.push_back("null");
            a.push_back(kNullSink);
        } else {
            if (want_audio) {
                a.push_back("-c:a");
                a.push_back("aac");
                a.push_back("-b:a");
                a.push_back(std::to_string(budget.audio_bps_each ? budget.audio_bps_each
                                                                 : kAudioBitsPerSecond));
            } else {
                a.push_back("-an");
            }
            a.push_back(path_to_utf8(dest));
        }

        a.push_back("-progress");
        a.push_back("pipe:1");
        a.push_back("-nostats");
        a.push_back("-hide_banner");
        return a;
    };

    // Pass 1 fills the first half of this file's slice, pass 2 the second --
    // matching the original's progress behaviour. A single-pass hardware encode
    // fills the whole slice.
    if (two_pass) {
        set_phase("pass1", 1, 2);
        const auto code = run_ffmpeg(build(1, {}), 0.0, 0.5, 1);
        if (!code) {
            discard_temp(temp);
            remove_pass_logs(pass_log);
            clear_temp();
            return FileResult::Cancelled;
        }
        if (*code != 0) {
            ui(msg::file_failed_code(path_to_utf8(input), *code));
            file_log(fmt::format("Error processing file: {} (pass 1, exit {})",
                                 path_to_utf8(input), *code));
            remove_pass_logs(pass_log);
            clear_temp();
            return FileResult::Failed;
        }
    }

    const double base = two_pass ? 0.5 : 0.0;
    const double span = two_pass ? 0.5 : 1.0;
    set_phase(two_pass ? "pass2" : "encode", two_pass ? 2 : 0, two_pass ? 2 : 0);
    const auto code = run_ffmpeg(build(2, temp), base, span, 0);

    remove_pass_logs(pass_log);

    if (!code) {
        discard_temp(temp);
        clear_temp();
        return FileResult::Cancelled;
    }
    if (*code != 0) {
        ui(msg::file_failed_code(path_to_utf8(input), *code));
        file_log(fmt::format("Error processing file: {} (exit {})", path_to_utf8(input), *code));
        discard_temp(temp);
        clear_temp();
        return FileResult::Failed;
    }
    if (!output_valid(temp)) {
        ui(msg::file_failed(path_to_utf8(input)));
        file_log(fmt::format("Output failed validation: {}", path_to_utf8(input)));
        discard_temp(temp);
        clear_temp();
        return FileResult::Failed;
    }

    // Report how close we landed; the only way to know the rate control is
    // behaving on real footage rather than synthetic tests.
    if (settings.size_mode == SizeMode::Manual) {
        const auto actual = file_size_or_zero(temp);
        const double delta = settings.max_size_bytes
                                 ? (static_cast<double>(actual) -
                                    static_cast<double>(settings.max_size_bytes)) *
                                       100.0 / static_cast<double>(settings.max_size_bytes)
                                 : 0.0;
        file_log(fmt::format("Size: {} (target {}, {:+.1f}%)", format_bytes(actual),
                             format_bytes(settings.max_size_bytes), delta));
    }

    const bool ok = finalize(input, output, temp);
    clear_temp();
    return ok ? FileResult::Converted : FileResult::Failed;
}

FileResult JobRunner::Impl::convert_audio(const fs::path& input, fs::path output) {
    ui(msg::file_started(path_to_utf8(input)));
    begin_file(input, 0.0);
    set_phase("probe", 0, 0);

    const MediaInfo info = probe_media(tools, input);
    if (!info.valid || info.audio_streams.empty()) {
        ui(msg::file_failed(path_to_utf8(input)));
        file_log(fmt::format("Could not read audio: {}", path_to_utf8(input)));
        return FileResult::Failed;
    }
    progress.duration = info.duration_seconds;

    output = with_extension(output, ".mp3");
    const fs::path temp = make_temp_sibling(output, ".mp3");
    set_temp(temp);
    set_phase("audio", 0, 0);

    const bool already_mp3 = info.audio_streams.front().codec_name == "mp3";

    std::vector<std::string> args{path_to_utf8(tools.ffmpeg), "-nostdin", "-y", "-v", "error",
                                  "-i", path_to_utf8(input), "-vn"};
    if (already_mp3) {
        args.insert(args.end(), {"-c:a", "copy"});
    } else {
        args.insert(args.end(), {"-c:a", "libmp3lame", "-q:a", "0"});
    }
    args.push_back(path_to_utf8(temp));
    args.insert(args.end(), {"-progress", "pipe:1", "-nostats", "-hide_banner"});

    const auto code = run_ffmpeg(args, 0.0, 1.0, 0);
    if (!code) {
        discard_temp(temp);
        clear_temp();
        return FileResult::Cancelled;
    }
    if (*code != 0) {
        ui(msg::file_failed_code(path_to_utf8(input), *code));
        file_log(fmt::format("Error processing file: {} (exit {})", path_to_utf8(input), *code));
        discard_temp(temp);
        clear_temp();
        return FileResult::Failed;
    }
    if (!output_valid(temp)) {
        ui(msg::file_failed(path_to_utf8(input)));
        discard_temp(temp);
        clear_temp();
        return FileResult::Failed;
    }

    const bool ok = finalize(input, output, temp);
    clear_temp();
    return ok ? FileResult::Converted : FileResult::Failed;
}

// ---------------------------------------------------------------------------

void JobRunner::Impl::run() {
    RunSummary summary;
    ui(msg::kStarting);

    std::error_code ec;
    if (!fs::is_directory(settings.input_dir, ec)) {
        ui(msg::kInputDirMissing);
        if (cb.finished) cb.finished(summary);
        running = false;
        return;
    }

    const auto files = collect_files(settings.input_dir);
    if (files.empty()) {
        ui(msg::kNoFilesFound);
        if (cb.finished) cb.finished(summary);
        running = false;
        return;
    }

    // Pick the encoder once. The benchmark is cached, so this is instant after
    // the first run on a given machine.
    encoder = choose_encoder(tools, cache_dir, 2.0, &cancel, settings.force_software_encoder);
    ui(encoder.reason);
    file_log(fmt::format("Encoder: {} ({}) -- {}", encoder.ffmpeg_encoder, encoder.device_name,
                         encoder.detail));

    file_count = static_cast<int>(files.size());
    file_log(fmt::format("{} file(s) in {}", file_count, path_to_utf8(settings.input_dir)));

    for (int i = 0; i < file_count; ++i) {
        if (cancelled()) {
            summary.cancelled = true;
            break;
        }
        file_index = i;

        const fs::path& input = files[static_cast<size_t>(i)];
        const std::string ext = lower_extension(input);
        const bool video = is_video_extension(ext);
        const bool audio = !video && is_audio_extension(ext);

        // Files that are neither stay exactly where they are, and are not
        // copied into the output tree.
        if (!video && !audio) {
            begin_file(input, 0.0);
            emit_progress(1.0);
            continue;
        }

        fs::path output = build_output_path(settings, settings.input_dir, input);
        const fs::path predicted = with_extension(output, video ? ".mp4" : ".mp3");

        if (settings.output_mode == OutputMode::CopyTo &&
            settings.existing_policy == ExistingFilePolicy::Skip && file_exists(predicted)) {
            file_log(fmt::format("Skipped existing file: {}", path_to_utf8(predicted)));
            ++summary.skipped_exists;
            begin_file(input, 0.0);
            emit_progress(1.0);
            continue;
        }

        const FileResult r = video ? convert_video(input, output)
                                   : convert_audio(input, output);
        switch (r) {
            case FileResult::Converted:     ++summary.converted; break;
            case FileResult::Remuxed:       ++summary.remuxed; ++summary.skipped_small; break;
            case FileResult::SkippedExists: ++summary.skipped_exists; break;
            case FileResult::Failed:        ++summary.failed; break;
            case FileResult::Cancelled:     summary.cancelled = true; break;
            default: break;
        }
        if (r == FileResult::Cancelled) break;

        emit_progress(1.0);
    }

    if (summary.cancelled) {
        ui(msg::kCancelled);
        file_log("Process cancelled by user.");
    } else {
        ui(msg::kFinished);
        file_log("All tasks completed.");
    }
    file_log(fmt::format("Summary: {} converted, {} remuxed, {} skipped, {} failed",
                         summary.converted, summary.remuxed,
                         summary.skipped_exists, summary.failed));

    running = false;
    if (cb.finished) cb.finished(summary);
}

// ---------------------------------------------------------------------------
// Public surface
// ---------------------------------------------------------------------------

JobRunner::JobRunner(ToolPaths tools, RunLog& run_log, fs::path cache_dir)
    : impl_(std::make_unique<Impl>(std::move(tools), run_log, std::move(cache_dir))) {
    // Anything a previous, abruptly terminated run left behind.
    for (const auto& removed : impl_->journal.sweep()) {
        impl_->run_log.write("Removed orphaned temp file from a previous run: " + removed);
    }
}

JobRunner::~JobRunner() {
    cancel();
    join();
}

bool JobRunner::running() const {
    return impl_->running.load(std::memory_order_relaxed);
}

bool JobRunner::start(Settings settings, JobCallbacks callbacks) {
    if (running()) return false;
    join();  // reap a previous finished thread

    impl_->settings = std::move(settings);
    impl_->cb       = std::move(callbacks);
    // Paths arrive with whatever separators the caller used; normalising here
    // keeps every later path -- and every log line -- consistent.
    impl_->settings.input_dir  = impl_->settings.input_dir.lexically_normal().make_preferred();
    impl_->settings.output_dir = impl_->settings.output_dir.lexically_normal().make_preferred();
    impl_->cancel.store(false, std::memory_order_relaxed);

    // Validation, in the same order and with the same messages as the original.
    if (impl_->settings.input_dir.empty()) {
        impl_->ui(msg::kInputDirEmpty);
        return false;
    }
    if (!impl_->tools.valid()) {
        impl_->ui(msg::ffmpeg_missing());
        return false;
    }
    if (impl_->settings.size_mode == SizeMode::Manual && impl_->settings.max_size_bytes < 1) {
        impl_->ui(msg::kInvalidSize);
        return false;
    }
    if (impl_->settings.output_mode == OutputMode::CopyTo &&
        impl_->settings.output_dir.empty()) {
        impl_->ui(msg::kOutputDirEmpty);
        return false;
    }

    impl_->running.store(true, std::memory_order_relaxed);
    impl_->run_log.write("Compression process started.");
    impl_->worker = std::thread([p = impl_.get()] { p->run(); });
    return true;
}

void JobRunner::cancel() {
    impl_->cancel.store(true, std::memory_order_relaxed);

    std::lock_guard lock(impl_->active_mutex);
    if (impl_->active_process) impl_->active_process->request_stop();
}

void JobRunner::join() {
    if (impl_->worker.joinable()) impl_->worker.join();
}

}  // namespace conv
