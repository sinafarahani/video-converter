// convctl -- a command-line harness for the conversion engine.
//
// Not shipped. It exists so the media pipeline can be exercised and verified
// without going through the UI, which makes it possible to test the parts that
// are genuinely hard to get right -- audio track detection, encoder selection,
// size targeting, process handling -- against real files.
//
//   convctl gpu                               adapters, encoder choice, measured fps
//   convctl bench <encoder> [ffmpeg args...]  raw throughput of one encoder
//   convctl quote                             self-test of Windows argv quoting
//   convctl size [values...]                  size parsing (built-in table if none)
//   convctl probe <file>                      duration, streams, per-track loudness
//   convctl run <input-dir> [--out <dir>] [--size 1GB | --level medium]
//              [--fast] [--skip] [--cpu]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "conv/ffmpeg.hpp"
#include "conv/gpu.hpp"
#include "conv/job.hpp"
#include "conv/logging.hpp"
#include "conv/util.hpp"

namespace {

std::filesystem::path exe_dir() {
    std::error_code ec;
    auto p = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path(".") : p;
}

int cmd_gpu(const conv::ToolPaths& tools) {
    fmt::print("ffmpeg : {}\n", conv::path_to_utf8(tools.ffmpeg));
    fmt::print("ffprobe: {}\n\n", conv::path_to_utf8(tools.ffprobe));

    const auto gpus = conv::enumerate_gpus();
    fmt::print("adapters ({}):\n", gpus.size());
    for (const auto& g : gpus) {
        fmt::print("  {:<42} vendor={:04x} device={:04x} vram={:>8} discrete={} candidate={}\n",
                   g.description, g.vendor_id, g.device_id,
                   conv::format_bytes(g.dedicated_vram_bytes),
                   g.is_discrete ? "yes" : "no ",
                   conv::is_encode_candidate(g) ? "YES" : "no");
    }

    const auto encoders = conv::available_encoders(tools);
    fmt::print("\nrelevant encoders present: ");
    for (const char* n : {"libx265", "libx264", "hevc_nvenc", "hevc_qsv", "hevc_amf"}) {
        const bool have = std::find(encoders.begin(), encoders.end(), n) != encoders.end();
        if (have) fmt::print("{} ", n);
    }
    fmt::print("\n\nmeasuring (a few seconds per encoder, cached afterwards)...\n");

    const auto choice = conv::choose_encoder(tools, exe_dir() / "cache", 2.0, nullptr);
    fmt::print("\nCHOSEN: {} on {}\n", choice.ffmpeg_encoder, choice.device_name);
    fmt::print("  speedup : {:.2f}x  (hw {:.0f} fps, sw {:.0f} fps)\n", choice.speedup,
               choice.hw_fps, choice.sw_fps);
    fmt::print("  user    : {}\n", choice.reason);
    fmt::print("  detail  : {}\n", choice.detail);
    fmt::print("  two-pass: {}\n", conv::uses_two_pass(choice) ? "yes" : "no");
    return 0;
}

int cmd_bench(const conv::ToolPaths& tools, const std::vector<std::string>& args) {
    const std::string encoder = args[2];
    std::vector<std::string> extra(args.begin() + 3, args.end());

    fmt::print("measuring {} ...\n", encoder);
    const auto m = conv::measure_encoder_fps(tools, encoder, extra, nullptr);
    if (!m.ok) {
        fmt::print("FAILED: {}\n", m.error);
        return 1;
    }
    fmt::print("{}: {:.1f} fps  ({} frames in {:.2f}s of steady state)\n", encoder, m.fps,
               m.frames, m.seconds);
    return 0;
}

int cmd_quote() {
    // Each case: argv in, the exact command line the MSVCRT parser must see.
    struct Case { std::vector<std::string> in; std::string want; };
    const Case cases[] = {
        {{"ffmpeg", "-i", "plain.mp4"},                 "ffmpeg -i plain.mp4"},
        {{"a", "has space"},                            "a \"has space\""},
        {{"a", ""},                                     "a \"\""},
        {{"a", "quote\"inside"},                        "a \"quote\\\"inside\""},
        {{"a", "trailing\\"},                           "a trailing\\"},
        {{"a", "trailing with space\\"},                "a \"trailing with space\\\\\""},
        {{"a", "back\\\\slash \"q\""},                  "a \"back\\\\slash \\\"q\\\"\""},
        {{"a", "H:\\deps\\پوشه با فاصله\\clip.mkv"},    "a \"H:\\deps\\پوشه با فاصله\\clip.mkv\""},
        {{"a", "C:\\path\\"},                           "a C:\\path\\"},
        {{"a", "C:\\dir name\\"},                       "a \"C:\\dir name\\\\\""},
    };

    int failures = 0;
    for (const auto& c : cases) {
        const auto got = conv::Process::quote_windows_command_line(c.in);
        const bool ok = got == c.want;
        if (!ok) ++failures;
        fmt::print("{}  {}\n      want: {}\n", ok ? "ok  " : "FAIL", got, c.want);
    }
    fmt::print("\n{} failure(s)\n", failures);
    return failures ? 1 : 0;
}

int cmd_size(const std::vector<std::string>& args) {
    std::vector<std::string> inputs(args.begin() + std::min<size_t>(2, args.size()), args.end());

    // With no arguments, run a built-in table. This exists because Windows hands
    // main() its argv in the active ANSI code page, so Persian text typed on a
    // command line arrives as question marks and cannot be tested that way.
    // These literals are UTF-8 in the source (the file is compiled /utf-8),
    // which is exactly how strings arrive from the JavaScript bridge.
    if (inputs.empty()) {
        inputs = {
            "1GB", "500MB", "1", "500", "15.9", "16", "2.5gb", "  700 MB  ",
            "۵۰۰MB",   // Persian digits
            "۱GB",     // Persian digit + unit
            "٢GB",     // Arabic-Indic digits
            "۱٫۵GB",   // Persian decimal separator
            "", "abc", "0", "-5",
        };
    }

    for (const auto& in : inputs) {
        const auto v = conv::parse_size_input(in);
        if (v) {
            fmt::print("{:>14} -> {:>10} ({} bytes)\n", "\"" + in + "\"",
                       conv::format_bytes(*v), *v);
        } else {
            fmt::print("{:>14} -> REJECTED\n", "\"" + in + "\"");
        }
    }
    return 0;
}

int cmd_probe(const conv::ToolPaths& tools, const std::string& file) {
    const auto path = conv::path_from_utf8(file);
    auto info = conv::probe_media(tools, path);

    fmt::print("valid    : {}\n", info.valid);
    fmt::print("duration : {:.3f} s\n", info.duration_seconds);
    fmt::print("video    : {} {}x{} ({})\n", info.has_video ? "yes" : "no", info.width,
               info.height, info.video_codec_name);
    fmt::print("audio    : {} stream(s)\n", info.audio_streams.size());

    if (info.audio_streams.empty()) return 0;

    fmt::print("\nanalysing loudness...\n");
    if (!conv::analyze_audio(tools, path, info, conv::AudioDetectionConfig{}, nullptr)) {
        fmt::print("analysis FAILED\n");
        return 1;
    }

    fmt::print("\n  {:<4} {:<10} {:>4} {:>10} {:>10}  {:<6} {}\n", "idx", "codec", "ch", "mean dB",
               "peak dB", "keep", "pair");
    for (const auto& a : info.audio_streams) {
        std::string pair;
        if (a.is_pair_leader())        pair = fmt::format("L with {}", a.stereo_partner);
        else if (a.is_pair_follower()) pair = fmt::format("R with {}", a.stereo_partner);
        fmt::print("  {:<4} {:<10} {:>4} {:>10.1f} {:>10.1f}  {:<6} {}\n", a.audio_index,
                   a.codec_name, a.channels, a.mean_db, a.peak_db, a.selected ? "KEEP" : "drop",
                   pair);
    }
    fmt::print("\noutput audio streams: {}\n", conv::count_output_audio_streams(info));
    return 0;
}

int cmd_run(const conv::ToolPaths& tools, const std::vector<std::string>& args) {
    conv::Settings s;
    int cancel_after = 0;  // seconds; 0 = never. Exercises JobRunner::cancel() like the UI does.
    s.input_dir = conv::path_from_utf8(args[2]);
    s.size_mode = conv::SizeMode::Automatic;
    s.level     = conv::CompressionLevel::Medium;

    for (size_t i = 3; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--out" && i + 1 < args.size()) {
            s.output_mode = conv::OutputMode::CopyTo;
            s.output_dir  = conv::path_from_utf8(args[++i]);
        } else if (a == "--size" && i + 1 < args.size()) {
            const auto v = conv::parse_size_input(args[++i]);
            if (!v) {
                fmt::print("bad size\n");
                return 2;
            }
            s.size_mode      = conv::SizeMode::Manual;
            s.max_size_bytes = *v;
        } else if (a == "--level" && i + 1 < args.size()) {
            const std::string l = args[++i];
            s.size_mode = conv::SizeMode::Automatic;
            s.level = l == "low"     ? conv::CompressionLevel::Low
                      : l == "high"  ? conv::CompressionLevel::High
                      : l == "extreme" ? conv::CompressionLevel::Extreme
                                       : conv::CompressionLevel::Medium;
        } else if (a == "--fast") {
            s.speed = conv::SpeedPreset::Fast;
        } else if (a == "--skip") {
            s.existing_policy = conv::ExistingFilePolicy::Skip;
        } else if (a == "--cpu") {
            s.force_software_encoder = true;
        } else if (a == "--cancel-after" && i + 1 < args.size()) {
            cancel_after = std::stoi(args[++i]);
        }
    }

    conv::RunLog log(exe_dir() / "logs");
    conv::JobRunner runner(tools, log, exe_dir() / "cache");

    conv::JobCallbacks cb;
    cb.log = [](std::string_view m) { fmt::print("\n[ui] {}\n", m); };
    cb.progress = [](const conv::Progress& p) {
        fmt::print("\r[{:>5.1f}%] {}/{} {:<8} file {:>5.1f}%  fps {:>6.1f}  speed {:>7.3f}x  "
                   "eta {:>7}  ",
                   p.fraction * 100.0, p.file_index + 1, p.file_count, p.phase,
                   p.file_fraction * 100.0, p.fps, p.speed,
                   p.eta_seconds < 0 ? std::string("?") : fmt::format("{:.0f}s", p.eta_seconds));
        std::fflush(stdout);
    };
    cb.finished = [&](const conv::RunSummary& sum) {
        fmt::print("\n\ndone: {} converted, {} remuxed, {} skipped, {} failed, cancelled={}\n",
                   sum.converted, sum.remuxed, sum.skipped_exists, sum.failed, sum.cancelled);
    };

    if (!runner.start(s, cb)) {
        fmt::print("refused to start\n");
        return 1;
    }

    // --cancel-after: the same JobRunner::cancel() the UI's button calls.
    std::thread canceller;
    if (cancel_after > 0) {
        canceller = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::seconds(cancel_after));
            fmt::print("\n[test] cancelling\n");
            runner.cancel();
        });
    }
    runner.join();
    if (canceller.joinable()) canceller.join();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv, argv + argc);
    if (args.size() < 2) {
        fmt::print("usage: convctl <gpu|bench|quote|size|probe|run> ...\n");
        return 2;
    }

    const auto tools = conv::locate_tools(exe_dir());
    const std::string& cmd = args[1];

    if (cmd == "size")  return cmd_size(args);
    if (cmd == "quote") return cmd_quote();

    if (!tools.valid()) {
        fmt::print("ffmpeg/ffprobe not found next to convctl or on PATH\n");
        return 1;
    }

    if (cmd == "gpu") return cmd_gpu(tools);
    if (cmd == "bench" && args.size() >= 3) return cmd_bench(tools, args);
    if (cmd == "probe" && args.size() >= 3) return cmd_probe(tools, args[2]);
    if (cmd == "run" && args.size() >= 3) return cmd_run(tools, args);

    fmt::print("unknown command\n");
    return 2;
}
