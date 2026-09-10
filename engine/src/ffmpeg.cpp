#include "conv/ffmpeg.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <system_error>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "conv/util.hpp"

namespace conv {
namespace {

#ifdef _WIN32
constexpr const char* kExeSuffix = ".exe";
#else
constexpr const char* kExeSuffix = "";
#endif

std::filesystem::path find_on_path(const std::string& stem) {
    const char* raw = std::getenv("PATH");
    if (!raw) return {};

#ifdef _WIN32
    constexpr char kSep = ';';
#else
    constexpr char kSep = ':';
#endif
    const std::string path_var(raw);
    const std::string filename = stem + kExeSuffix;

    size_t start = 0;
    while (start <= path_var.size()) {
        const size_t end = path_var.find(kSep, start);
        const std::string dir =
            path_var.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            std::error_code ec;
            const auto candidate = path_from_utf8(dir) / path_from_utf8(filename);
            if (std::filesystem::exists(candidate, ec)) return candidate;
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

std::filesystem::path locate_one(const std::filesystem::path& app_dir, const std::string& stem) {
    std::error_code ec;
    const std::string filename = stem + kExeSuffix;

    // Where the installer puts them.
    auto candidate = app_dir / "ffmpeg" / path_from_utf8(filename);
    if (std::filesystem::exists(candidate, ec)) return candidate;

    // Beside the executable.
    candidate = app_dir / path_from_utf8(filename);
    if (std::filesystem::exists(candidate, ec)) return candidate;

    // Whatever the machine already has.
    return find_on_path(stem);
}

double json_double(const nlohmann::json& j, const char* key, double fallback = 0.0) {
    if (!j.contains(key)) return fallback;
    const auto& v = j.at(key);
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

std::string json_string(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) return {};
    const auto& v = j.at(key);
    return v.is_string() ? v.get<std::string>() : std::string{};
}

// Pulls "mean_volume: -23.4 dB" style values out of a volumedetect stderr line,
// returning the filter instance index and the value.
struct VolumeLine {
    int    instance = -1;
    double mean_db = 0.0;
    double peak_db = 0.0;
    bool   is_mean = false;
    bool   is_peak = false;
};

std::optional<VolumeLine> parse_volumedetect(std::string_view line) {
    // [Parsed_volumedetect_2 @ 0000018f] mean_volume: -68.3 dB
    constexpr std::string_view kTag = "Parsed_volumedetect_";
    const size_t tag = line.find(kTag);
    if (tag == std::string_view::npos) return std::nullopt;

    size_t p = tag + kTag.size();
    int index = 0;
    bool any_digit = false;
    while (p < line.size() && std::isdigit(static_cast<unsigned char>(line[p]))) {
        index = index * 10 + (line[p] - '0');
        ++p;
        any_digit = true;
    }
    if (!any_digit) return std::nullopt;

    const bool is_mean = line.find("mean_volume:") != std::string_view::npos;
    const bool is_peak = line.find("max_volume:") != std::string_view::npos;
    if (!is_mean && !is_peak) return std::nullopt;

    const size_t colon = line.find(':', p);
    if (colon == std::string_view::npos) return std::nullopt;

    // ffmpeg prints "-inf dB" for a stream that is bit-exact silence.
    const std::string tail(line.substr(colon + 1));
    double value = -91.0;
    if (tail.find("inf") == std::string::npos) {
        try {
            value = std::stod(tail);
        } catch (...) {
            return std::nullopt;
        }
    }

    VolumeLine out;
    out.instance = index;
    out.is_mean  = is_mean;
    out.is_peak  = is_peak;
    if (is_mean) out.mean_db = value;
    if (is_peak) out.peak_db = value;
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------

ToolPaths locate_tools(const std::filesystem::path& app_dir) {
    ToolPaths t;
    t.ffmpeg  = locate_one(app_dir, "ffmpeg");
    t.ffprobe = locate_one(app_dir, "ffprobe");
    return t;
}

// ---------------------------------------------------------------------------

MediaInfo probe_media(const ToolPaths& tools, const std::filesystem::path& file) {
    MediaInfo info;
    if (tools.ffprobe.empty()) return info;

    const auto result = capture({
        path_to_utf8(tools.ffprobe),
        "-v", "error",
        "-print_format", "json",
        "-show_format",
        "-show_streams",
        path_to_utf8(file),
    });

    if (!result.ok() || result.out.empty()) return info;

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(result.out);
    } catch (const std::exception&) {
        return info;
    }

    if (root.contains("format")) {
        info.duration_seconds = json_double(root["format"], "duration", 0.0);
    }

    if (root.contains("streams") && root["streams"].is_array()) {
        int audio_ordinal = 0;
        for (const auto& s : root["streams"]) {
            const std::string type = json_string(s, "codec_type");

            if (type == "video" && !info.has_video) {
                // Cover art in an audio file shows up as a video stream; it has
                // no real duration and must not make us treat the file as video.
                const std::string disp = s.contains("disposition") &&
                                                 s["disposition"].value("attached_pic", 0) == 1
                                             ? "attached"
                                             : "";
                if (disp.empty()) {
                    info.has_video          = true;
                    info.video_stream_index = static_cast<int>(json_double(s, "index", -1));
                    info.width              = static_cast<int>(json_double(s, "width", 0));
                    info.height             = static_cast<int>(json_double(s, "height", 0));
                    info.video_codec_name   = json_string(s, "codec_name");
                }
            } else if (type == "audio") {
                AudioStreamInfo a;
                a.index       = static_cast<int>(json_double(s, "index", 0));
                a.audio_index = audio_ordinal++;
                a.codec_name  = json_string(s, "codec_name");
                a.channels    = static_cast<int>(json_double(s, "channels", 0));
                if (s.contains("tags")) {
                    a.language = json_string(s["tags"], "language");
                    a.title    = json_string(s["tags"], "title");
                }
                info.audio_streams.push_back(std::move(a));
            }
        }
    }

    // A duration of zero means we cannot compute a bitrate for a size target.
    // The Java version divided by it anyway and threw ArithmeticException.
    info.valid = info.duration_seconds > 0.0;
    return info;
}

// ---------------------------------------------------------------------------

void select_audio_streams(MediaInfo& info, const AudioDetectionConfig& cfg) {
    for (auto& a : info.audio_streams) {
        a.has_content = false;
        a.selected    = false;
    }
    if (info.audio_streams.empty()) return;

    // The loudest track sets the reference. A dead microphone sits at its noise
    // floor, typically 30-50 dB below a live one, so the gap is decisive.
    const auto loudest = std::max_element(
        info.audio_streams.begin(), info.audio_streams.end(),
        [](const AudioStreamInfo& l, const AudioStreamInfo& r) { return l.mean_db < r.mean_db; });
    const double best_mean = loudest->mean_db;

    for (auto& a : info.audio_streams) {
        const bool above_floor = a.mean_db >= cfg.mean_floor_db || a.peak_db >= cfg.peak_floor_db;
        const bool near_best   = a.mean_db >= best_mean - cfg.relative_gap_db;
        a.has_content = above_floor && near_best;
        a.selected    = a.has_content;
    }

    // Never produce a silent video because the detector was too strict. If
    // nothing qualified, keep the loudest track regardless.
    const bool any = std::any_of(info.audio_streams.begin(), info.audio_streams.end(),
                                 [](const AudioStreamInfo& a) { return a.selected; });
    if (!any) loudest->selected = true;

    // -- stereo pairs ----------------------------------------------------------
    //
    // The first real MXF this was run on had 8 mono tracks, of which 2 carried
    // sound: the left and right of one stereo source. Broadcast convention
    // (EBU R48 / SMPTE 377) lays pairs out on aligned track numbers -- (1,2),
    // (3,4), ... -- so aligned neighbours are tried first and only then the
    // unaligned ones, so that a live mic on track 2 and another on track 3
    // are not mistaken for a pair when tracks 1 and 4 are dead.
    for (auto& a : info.audio_streams) a.stereo_partner = -1;
    if (!cfg.detect_stereo_pairs) return;

    auto& tracks = info.audio_streams;
    const auto can_pair = [&](size_t i, size_t j) {
        if (i >= tracks.size() || j >= tracks.size()) return false;
        const auto& x = tracks[i];
        const auto& y = tracks[j];
        if (!x.selected || !y.selected) return false;
        if (x.stereo_partner >= 0 || y.stereo_partner >= 0) return false;
        if (x.channels != 1 || y.channels != 1) return false;  // already stereo, or more
        return std::abs(x.mean_db - y.mean_db) <= cfg.stereo_pair_max_gap_db;
    };
    const auto make_pair = [&](size_t i, size_t j) {
        tracks[i].stereo_partner = tracks[j].audio_index;
        tracks[j].stereo_partner = tracks[i].audio_index;
    };

    // Pass 1: aligned pairs (0,1), (2,3), ...
    for (size_t i = 0; i + 1 < tracks.size(); i += 2) {
        if (can_pair(i, i + 1)) make_pair(i, i + 1);
    }
    // Pass 2: whatever adjacent selected mono tracks remain.
    for (size_t i = 0; i + 1 < tracks.size(); ++i) {
        if (can_pair(i, i + 1)) make_pair(i, i + 1);
    }
}

int count_output_audio_streams(const MediaInfo& info) {
    int n = 0;
    for (const auto& a : info.audio_streams) {
        if (a.selected && !a.is_pair_follower()) ++n;
    }
    return n;
}

bool analyze_audio(const ToolPaths& tools,
                   const std::filesystem::path& file,
                   MediaInfo& info,
                   const AudioDetectionConfig& cfg,
                   const std::atomic_bool* cancel) {
    if (tools.ffmpeg.empty() || info.audio_streams.empty()) return false;

    const int    n        = static_cast<int>(info.audio_streams.size());
    const double duration = info.duration_seconds;

    // Where to sample. Decoding a 50 GB source end to end just to find out which
    // microphone was live would take longer than the encode; a few windows
    // spread through the file answer the question just as well.
    std::vector<double> offsets;
    const bool sample = cfg.sample_seconds_per_probe > 0 && cfg.probe_count > 0 &&
                        duration > cfg.sample_seconds_per_probe * cfg.probe_count * 1.5;
    if (sample) {
        for (int i = 0; i < cfg.probe_count; ++i) {
            const double frac = (i + 1.0) / (cfg.probe_count + 1.0);  // 0.25, 0.5, 0.75 ...
            offsets.push_back(duration * frac);
        }
    } else {
        offsets.push_back(-1.0);  // whole file
    }

    // Accumulate across probe windows: the loudest window wins, because a mic
    // that is live for only part of the recording is still a live mic.
    std::vector<double> mean_db(n, -91.0);
    std::vector<double> peak_db(n, -91.0);
    bool got_any = false;

    for (const double offset : offsets) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;

        // One volumedetect instance per audio stream, all in a single decode.
        std::string filter;
        std::vector<std::string> maps;
        for (int i = 0; i < n; ++i) {
            filter += fmt::format("[0:a:{}]volumedetect[vd{}];", i, i);
            maps.push_back("-map");
            maps.push_back(fmt::format("[vd{}]", i));
            maps.push_back("-f");
            maps.push_back("null");
            maps.push_back("-");
        }
        if (!filter.empty()) filter.pop_back();  // trailing ';'

        std::vector<std::string> args{path_to_utf8(tools.ffmpeg), "-hide_banner", "-nostdin",
                                      "-nostats"};
        if (offset >= 0.0) {
            // Input seeking: fast even on very large files.
            args.push_back("-ss");
            args.push_back(fmt::format("{:.3f}", offset));
            args.push_back("-t");
            args.push_back(std::to_string(cfg.sample_seconds_per_probe));
        }
        args.push_back("-i");
        args.push_back(path_to_utf8(file));
        args.push_back("-vn");
        args.push_back("-filter_complex");
        args.push_back(filter);
        args.insert(args.end(), maps.begin(), maps.end());

        std::vector<double> win_mean(n, -91.0);
        std::vector<double> win_peak(n, -91.0);

        Process proc;
        const auto code = proc.run(
            args,
            nullptr,
            [&](std::string_view line) {
                const auto parsed = parse_volumedetect(line);
                if (!parsed) return;
                if (parsed->instance < 0 || parsed->instance >= n) return;
                if (parsed->is_mean) win_mean[parsed->instance] = parsed->mean_db;
                if (parsed->is_peak) win_peak[parsed->instance] = parsed->peak_db;
            },
            cancel);

        if (!code) return false;  // cancelled or failed to start

        for (int i = 0; i < n; ++i) {
            mean_db[i] = std::max(mean_db[i], win_mean[i]);
            peak_db[i] = std::max(peak_db[i], win_peak[i]);
        }
        got_any = true;
    }

    if (!got_any) return false;

    for (int i = 0; i < n; ++i) {
        info.audio_streams[i].mean_db = mean_db[i];
        info.audio_streams[i].peak_db = peak_db[i];
    }
    select_audio_streams(info, cfg);
    return true;
}

// ---------------------------------------------------------------------------

std::optional<double> parse_progress_seconds(std::string_view line) {
    // Preferred: microseconds, no parsing of colons required.
    constexpr std::string_view kUs = "out_time_us=";
    if (line.starts_with(kUs)) {
        const std::string v(line.substr(kUs.size()));
        if (v.find("N/A") != std::string::npos) return std::nullopt;
        try {
            return std::stoll(v) / 1'000'000.0;
        } catch (...) {
            return std::nullopt;
        }
    }

    constexpr std::string_view kTime = "out_time=";
    if (!line.starts_with(kTime)) return std::nullopt;

    // HH:MM:SS.mmmmmm -- parsed by hand rather than with sscanf, which is
    // locale-sensitive for the decimal point and would misread "1.5" under a
    // locale that uses a comma.
    std::string_view v = line.substr(kTime.size());
    if (v.find("N/A") != std::string_view::npos) return std::nullopt;

    const auto take_int = [&v](char delimiter) -> std::optional<long> {
        const size_t pos = v.find(delimiter);
        if (pos == std::string_view::npos) return std::nullopt;
        long value = 0;
        for (size_t i = 0; i < pos; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(v[i]))) return std::nullopt;
            value = value * 10 + (v[i] - '0');
        }
        v.remove_prefix(pos + 1);
        return value;
    };

    const auto hours = take_int(':');
    if (!hours) return std::nullopt;
    const auto minutes = take_int(':');
    if (!minutes) return std::nullopt;

    double seconds = 0.0;
    double scale   = 0.0;
    for (const char c : v) {
        if (c == '.') {
            if (scale != 0.0) break;
            scale = 0.1;
            continue;
        }
        if (!std::isdigit(static_cast<unsigned char>(c))) break;
        const int digit = c - '0';
        if (scale == 0.0) {
            seconds = seconds * 10.0 + digit;
        } else {
            seconds += digit * scale;
            scale /= 10.0;
        }
    }

    return static_cast<double>(*hours) * 3600.0 + static_cast<double>(*minutes) * 60.0 + seconds;
}

}  // namespace conv

namespace conv {

ProgressLine parse_progress_line(std::string_view line) {
    ProgressLine out;
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) return out;

    const std::string_view key = line.substr(0, eq);
    std::string_view       val = line.substr(eq + 1);
    while (!val.empty() && val.front() == ' ') val.remove_prefix(1);
    while (!val.empty() && val.back() == ' ') val.remove_suffix(1);

    // Strips a trailing unit such as the 'x' in "speed=0.031x" or the
    // "kbits/s" in bitrate before converting.
    const auto number = [](std::string_view v) -> std::optional<double> {
        if (v.empty() || v == "N/A") return std::nullopt;
        size_t n = 0;
        while (n < v.size() && (std::isdigit(static_cast<unsigned char>(v[n])) || v[n] == '.' ||
                                v[n] == '-' || v[n] == '+')) {
            ++n;
        }
        if (n == 0) return std::nullopt;
        try {
            return std::stod(std::string(v.substr(0, n)));
        } catch (...) {
            return std::nullopt;
        }
    };

    if (key == "frame") {
        out.kind = ProgressLine::Kind::Frame;
    } else if (key == "fps") {
        out.kind = ProgressLine::Kind::Fps;
    } else if (key == "speed") {
        out.kind = ProgressLine::Kind::Speed;
    } else if (key == "bitrate") {
        out.kind = ProgressLine::Kind::Bitrate;
    } else if (key == "total_size") {
        out.kind = ProgressLine::Kind::TotalSize;
    } else if (key == "out_time_us" || key == "out_time") {
        out.kind  = ProgressLine::Kind::OutTime;
        out.value = parse_progress_seconds(line).value_or(-1.0);
        return out;
    } else if (key == "progress") {
        out.kind = (val == "end") ? ProgressLine::Kind::End : ProgressLine::Kind::Other;
        return out;
    } else {
        return out;
    }

    out.value = number(val).value_or(-1.0);
    return out;
}

}  // namespace conv
