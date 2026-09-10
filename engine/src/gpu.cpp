#include "conv/gpu.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <optional>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "conv/messages.hpp"
#include "conv/util.hpp"

#ifdef _WIN32
#  include <dxgi.h>
#  include <wrl/client.h>
#endif

namespace conv {
namespace {

constexpr std::uint32_t kVendorNvidia = 0x10DE;
constexpr std::uint32_t kVendorAmd    = 0x1002;
constexpr std::uint32_t kVendorIntel  = 0x8086;

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

GpuVendor vendor_from_id(std::uint32_t id) {
    switch (id) {
        case kVendorNvidia: return GpuVendor::Nvidia;
        case kVendorAmd:    return GpuVendor::Amd;
        case kVendorIntel:  return GpuVendor::Intel;
        default:            return GpuVendor::Unknown;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

std::vector<GpuInfo> enumerate_gpus() {
    std::vector<GpuInfo> gpus;

#ifdef _WIN32
    using Microsoft::WRL::ComPtr;

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1), &factory))) return gpus;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) !=
                     DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;

        // The Microsoft Basic Render Driver is a software rasteriser; it has no
        // encoder and enumerating it would only confuse the benchmark.
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        GpuInfo g;
        g.vendor_id            = desc.VendorId;
        g.device_id            = desc.DeviceId;
        g.vendor               = vendor_from_id(desc.VendorId);
        g.dedicated_vram_bytes = static_cast<std::uint64_t>(desc.DedicatedVideoMemory);

        // DXGI gives us a wide string; convert without losing non-ASCII names.
        const std::wstring w(desc.Description);
        g.description = path_to_utf8(std::filesystem::path(w));

        // Integrated parts carve their memory out of system RAM, so a large
        // dedicated pool is a solid proxy for "discrete".
        g.is_discrete = g.dedicated_vram_bytes >= (512ull * 1024 * 1024);

        gpus.push_back(std::move(g));
    }
#endif

    return gpus;
}

bool is_encode_candidate(const GpuInfo& gpu) {
    const std::string name = to_lower(gpu.description);

    switch (gpu.vendor) {
        case GpuVendor::Nvidia:
            // Every NVIDIA part with a real framebuffer since Maxwell 2nd gen
            // has an HEVC-capable NVENC. Anything older will simply lose the
            // benchmark, so no model table is needed here.
            return gpu.dedicated_vram_bytes >= (1024ull * 1024 * 1024);

        case GpuVendor::Amd:
            return gpu.dedicated_vram_bytes >= (1024ull * 1024 * 1024);

        case GpuVendor::Intel:
            // Explicitly excluded: the integrated HD and UHD parts. Their
            // encoders exist but the quality at a given bitrate is poor enough
            // that using them is a bad trade even when they are faster.
            if (name.find("hd graphics") != std::string::npos) return false;
            if (name.find("uhd graphics") != std::string::npos) return false;
            // Arc is discrete and genuinely good; Xe-based integrated parts are
            // borderline and are left to the benchmark to judge.
            return gpu.is_discrete || name.find("arc") != std::string::npos;

        default:
            return false;
    }
}

std::string hardware_encoder_name(GpuVendor vendor, VideoCodec codec) {
    const bool hevc = (codec == VideoCodec::H265);
    switch (vendor) {
        case GpuVendor::Nvidia: return hevc ? "hevc_nvenc" : "h264_nvenc";
        case GpuVendor::Intel:  return hevc ? "hevc_qsv"   : "h264_qsv";
        case GpuVendor::Amd:
#ifdef _WIN32
            return hevc ? "hevc_amf" : "h264_amf";
#else
            return hevc ? "hevc_vaapi" : "h264_vaapi";
#endif
        case GpuVendor::Apple:  return hevc ? "hevc_videotoolbox" : "h264_videotoolbox";
        default:                return {};
    }
}

std::vector<std::string> available_encoders(const ToolPaths& tools) {
    std::vector<std::string> names;
    if (tools.ffmpeg.empty()) return names;

    const auto result = capture({path_to_utf8(tools.ffmpeg), "-hide_banner", "-encoders"});
    if (!result.ok()) return names;

    // Lines look like: " V....D hevc_nvenc           NVIDIA NVENC hevc encoder"
    size_t pos = 0;
    while (pos < result.out.size()) {
        const size_t eol = result.out.find('\n', pos);
        std::string_view line(result.out);
        line = line.substr(pos, (eol == std::string::npos ? result.out.size() : eol) - pos);
        pos  = (eol == std::string::npos) ? result.out.size() : eol + 1;

        if (line.size() < 8 || line[0] != ' ') continue;
        const auto flags_end = line.find(' ', 1);
        if (flags_end == std::string_view::npos) continue;

        auto rest = line.substr(flags_end);
        const auto name_start = rest.find_first_not_of(' ');
        if (name_start == std::string_view::npos) continue;
        rest = rest.substr(name_start);
        const auto name_end = rest.find(' ');
        names.emplace_back(rest.substr(0, name_end));
    }
    return names;
}

// ---------------------------------------------------------------------------
// Benchmark
// ---------------------------------------------------------------------------

FpsMeasurement measure_encoder_fps(const ToolPaths& tools,
                                   const std::string& encoder,
                                   const std::vector<std::string>& extra_args,
                                   const std::atomic_bool* cancel) {
    using clock = std::chrono::steady_clock;

    FpsMeasurement m;
    if (tools.ffmpeg.empty()) {
        m.error = "ffmpeg not found";
        return m;
    }

    // Long enough that no encoder runs out of frames before the window closes
    // (3600 frames covers anything up to ~1400 fps), short enough to be safe
    // if request_stop() somehow fails to take effect.
    std::vector<std::string> args{
        path_to_utf8(tools.ffmpeg), "-hide_banner", "-nostdin", "-nostats", "-v", "error",
        "-progress", "pipe:1", "-stats_period", "0.1",
        "-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=30",
        "-t", "120",
        "-c:v", encoder,
    };
    args.insert(args.end(), extra_args.begin(), extra_args.end());
    args.insert(args.end(), {"-an", "-f", "null", "-"});

    // Measurement window: from the first report showing frames > 0 until at
    // least this much wall time has passed. A couple of seconds is plenty to
    // average out x265's lookahead burstiness.
    constexpr auto kWindow = std::chrono::milliseconds(2500);

    struct Sample { clock::time_point t; int frames; };
    std::vector<Sample> samples;
    std::string         stderr_tail;
    Process             proc;
    bool                window_closed = false;

    const auto code = proc.run(
        args,
        [&](std::string_view line) {
            const auto p = parse_progress_line(line);
            if (p.kind != ProgressLine::Kind::Frame || p.value < 0) return;
            const int frames = static_cast<int>(p.value);
            if (frames <= 0 && samples.empty()) return;  // still in startup
            samples.push_back({clock::now(), frames});
            if (!window_closed && samples.back().t - samples.front().t >= kWindow) {
                window_closed = true;
                proc.request_stop();
            }
        },
        [&](std::string_view line) {
            if (stderr_tail.size() < 8192) {
                stderr_tail.append(line).push_back('\n');
            }
        },
        cancel);

    if (cancel && cancel->load(std::memory_order_relaxed)) {
        m.error = "cancelled";
        return m;
    }

    // Ran to completion with an error, or failed to launch: report why.
    const bool launch_failed = !code && proc.outcome() == Process::Outcome::NotStarted;
    const bool io_failed     = !code && proc.outcome() == Process::Outcome::Failed;
    if (launch_failed || io_failed) {
        m.error = proc.error().empty() ? "could not run ffmpeg" : proc.error();
        return m;
    }
    if (code && *code != 0) {
        m.error = stderr_tail.empty() ? fmt::format("ffmpeg exited with code {}", *code)
                                      : stderr_tail;
        return m;
    }

    if (samples.size() < 2) {
        m.error = "too few progress samples to measure";
        if (!stderr_tail.empty()) m.error += "\n" + stderr_tail;
        return m;
    }

    const auto&  a = samples.front();
    const auto&  b = samples.back();
    const double secs =
        std::chrono::duration_cast<std::chrono::milliseconds>(b.t - a.t).count() / 1000.0;
    const int frames = b.frames - a.frames;

    if (secs < 0.3 || frames <= 0) {
        m.error = fmt::format("measurement window too small ({} frames in {:.2f}s)", frames, secs);
        return m;
    }

    m.ok      = true;
    m.frames  = frames;
    m.seconds = secs;
    m.fps     = frames / secs;
    return m;
}

// ---------------------------------------------------------------------------
// The decision, with a cache
// ---------------------------------------------------------------------------

namespace {

std::filesystem::path cache_file(const std::filesystem::path& dir) {
    return dir / "encoder-cache.json";
}

// A cached result is reused only while it is still describing the same thing:
// the same adapter AND the same ffmpeg build. A driver or ffmpeg update can
// turn a failing encoder into a working one, so the ffmpeg version string is
// part of the key.
std::string ffmpeg_version_key(const ToolPaths& tools) {
    const auto r = capture({path_to_utf8(tools.ffmpeg), "-version"});
    if (!r.ok() || r.out.empty()) return "unknown";
    const auto nl = r.out.find('\n');
    std::string first = r.out.substr(0, nl);
    // "ffmpeg version N-125990-g5c395992f9-20260807 Copyright ..." -> the
    // version token only.
    const auto v = first.find("version ");
    if (v != std::string::npos) {
        first = first.substr(v + 8);
        const auto sp = first.find(' ');
        if (sp != std::string::npos) first = first.substr(0, sp);
    }
    return first;
}

struct CachedResult {
    bool   present = false;
    bool   passed  = false;
    double speedup = 0.0;
    double hw_fps  = 0.0;
    double sw_fps  = 0.0;
    std::int64_t when = 0;  // unix seconds
    std::string  error;
};

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

nlohmann::json load_cache(const std::filesystem::path& dir) {
    std::ifstream in(cache_file(dir));
    if (!in) return nlohmann::json::object();
    try {
        nlohmann::json j;
        in >> j;
        return j.is_object() ? j : nlohmann::json::object();
    } catch (...) {
        return nlohmann::json::object();
    }
}

CachedResult read_cached(const std::filesystem::path& dir, const std::string& key) {
    CachedResult c;
    const auto j = load_cache(dir);
    if (!j.contains(key) || !j[key].is_object()) return c;
    const auto& e = j[key];
    c.present = true;
    c.passed  = e.value("passed", false);
    c.speedup = e.value("speedup", 0.0);
    c.hw_fps  = e.value("hw_fps", 0.0);
    c.sw_fps  = e.value("sw_fps", 0.0);
    c.when    = e.value("when", std::int64_t{0});
    c.error   = e.value("error", std::string{});
    return c;
}

void write_cached(const std::filesystem::path& dir, const std::string& key, const CachedResult& c) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    auto j = load_cache(dir);
    j[key] = {
        {"passed", c.passed},   {"speedup", c.speedup}, {"hw_fps", c.hw_fps},
        {"sw_fps", c.sw_fps},   {"when", c.when},       {"error", c.error},
    };
    std::ofstream out(cache_file(dir), std::ios::trunc);
    if (out) out << j.dump(2);
}

// A pass is kept indefinitely: the hardware is not going to get slower. A
// failure is retried after a day, because the most common cause -- an old
// driver -- is something the user may well have fixed since.
constexpr std::int64_t kFailureRetrySeconds = 24 * 3600;

EncoderBackend backend_for(GpuVendor v) {
    switch (v) {
        case GpuVendor::Nvidia: return EncoderBackend::NVENC;
        case GpuVendor::Intel:  return EncoderBackend::QSV;
        case GpuVendor::Amd:
#ifdef _WIN32
            return EncoderBackend::AMF;
#else
            return EncoderBackend::VAAPI;
#endif
        case GpuVendor::Apple:  return EncoderBackend::VideoToolbox;
        default:                return EncoderBackend::Software;
    }
}

// The hardware-side arguments used for the comparison. Constant-quality at
// the app's Medium level, the same thing Automatic mode will actually run.
std::vector<std::string> hw_bench_args(EncoderBackend b) {
    switch (b) {
        case EncoderBackend::NVENC: return {"-rc", "vbr", "-cq", "28", "-preset", "p5"};
        case EncoderBackend::QSV:   return {"-global_quality", "28"};
        case EncoderBackend::AMF:   return {"-rc", "cqp", "-qp_i", "28", "-qp_p", "28"};
        case EncoderBackend::VAAPI: return {"-qp", "28"};
        case EncoderBackend::VideoToolbox: return {"-q:v", "60"};
        default: return {};
    }
}

}  // namespace

EncoderChoice choose_encoder(const ToolPaths& tools,
                             const std::filesystem::path& cache_dir,
                             double min_speedup,
                             const std::atomic_bool* cancel,
                             bool force_software) {
    const std::string sw_encoder =
        (kOutputCodec == VideoCodec::H265) ? "libx265" : "libx264";

    EncoderChoice software;
    software.backend        = EncoderBackend::Software;
    software.ffmpeg_encoder = sw_encoder;
    software.device_name    = "CPU";
    software.speedup        = 1.0;

    if (force_software) {
        software.reason = msg::sw_forced();
        software.detail = "software encoder forced by settings";
        return software;
    }

    const auto encoders = available_encoders(tools);
    const auto has = [&](const std::string& n) {
        return std::find(encoders.begin(), encoders.end(), n) != encoders.end();
    };

    if (!has(sw_encoder)) {
        software.reason = msg::sw_encoder_unavailable(sw_encoder);
        software.detail = sw_encoder + " is not compiled into this ffmpeg";
        return software;
    }

    // Candidates, in order of preference.
    struct Candidate {
        std::string    label;       // what the user sees
        std::string    cache_id;    // stable identity for the cache key
        EncoderBackend backend;
        std::string    encoder;
        // True when no adapter was identified and this is a blind probe of an
        // encoder ffmpeg happens to be built with. A probe that fails to open
        // almost always means the hardware is simply absent -- it must never be
        // reported to the user as "your driver is out of date".
        bool           probed = false;
    };
    std::vector<Candidate> candidates;

    auto gpus = enumerate_gpus();
    std::sort(gpus.begin(), gpus.end(), [](const GpuInfo& a, const GpuInfo& b) {
        return a.dedicated_vram_bytes > b.dedicated_vram_bytes;
    });
    for (const auto& gpu : gpus) {
        if (!is_encode_candidate(gpu)) continue;
        const std::string hw = hardware_encoder_name(gpu.vendor, kOutputCodec);
        if (hw.empty() || !has(hw)) continue;
        candidates.push_back({gpu.description,
                              fmt::format("{:04x}:{:04x}", gpu.vendor_id, gpu.device_id),
                              backend_for(gpu.vendor), hw});
    }

    // Adapter enumeration is DXGI-based and therefore Windows-only. Elsewhere
    // probe the hardware encoders this ffmpeg has and let the benchmark decide
    // -- the same bar, just without the vendor pre-filter.
    if (gpus.empty()) {
        struct Probe { EncoderBackend backend; const char* hevc; const char* h264; const char* label; };
        static constexpr Probe kProbes[] = {
            {EncoderBackend::VideoToolbox, "hevc_videotoolbox", "h264_videotoolbox", "VideoToolbox"},
            {EncoderBackend::NVENC,        "hevc_nvenc",        "h264_nvenc",        "NVENC"},
            {EncoderBackend::QSV,          "hevc_qsv",          "h264_qsv",          "Quick Sync"},
            // VAAPI is deliberately absent: it needs a render device and an
            // hwupload filter in front of the encoder, which the command builder
            // does not produce. Probing it would only ever fail.
        };
        for (const auto& p : kProbes) {
            const std::string hw = (kOutputCodec == VideoCodec::H265) ? p.hevc : p.h264;
            if (has(hw)) candidates.push_back({p.label, std::string("probe:") + hw, p.backend, hw, true});
        }
    }

    if (candidates.empty()) {
        software.reason = msg::sw_no_gpu();
        software.detail = gpus.empty() ? "no display adapters enumerated and no hardware encoder present"
                                       : "no adapter passed the eligibility filter";
        return software;
    }

    const std::string ffver = ffmpeg_version_key(tools);

    // The software number is needed once, for every candidate; measure lazily.
    std::optional<FpsMeasurement> sw;
    const auto software_fps = [&]() -> const FpsMeasurement& {
        if (!sw) sw = measure_encoder_fps(tools, sw_encoder, {"-crf", "20", "-preset", "medium"}, cancel);
        return *sw;
    };

    std::string first_failure_reason;  // remembered so a failure beats "no gpu" in the message
    std::string first_failure_detail;
    std::string probe_notes;           // blind probes that could not open; log only

    for (const auto& c : candidates) {
        if (cancel && cancel->load(std::memory_order_relaxed)) break;

        const std::string key = fmt::format("{}:{}:{}", c.cache_id, c.encoder, ffver);
        CachedResult cached   = read_cached(cache_dir, key);

        const bool use_cached =
            cached.present &&
            (cached.passed || (now_seconds() - cached.when) < kFailureRetrySeconds);

        if (!use_cached) {
            cached = CachedResult{};
            cached.present = true;
            cached.when    = now_seconds();

            const auto hw = measure_encoder_fps(tools, c.encoder, hw_bench_args(c.backend), cancel);
            if (!hw.ok) {
                cached.passed = false;
                cached.error  = hw.error;
            } else {
                const auto& s = software_fps();
                if (!s.ok) {
                    // Cannot compare. Do not cache: this says nothing about
                    // the hardware, and the software failure will be obvious
                    // the moment encoding starts.
                    software.reason = msg::sw_no_gpu();
                    software.detail = "software benchmark failed: " + s.error;
                    return software;
                }
                cached.hw_fps  = hw.fps;
                cached.sw_fps  = s.fps;
                cached.speedup = (s.fps > 0.0) ? hw.fps / s.fps : 0.0;
                cached.passed  = cached.speedup >= min_speedup;
            }
            write_cached(cache_dir, key, cached);
        }

        if (cached.passed) {
            EncoderChoice choice;
            choice.backend        = c.backend;
            choice.ffmpeg_encoder = c.encoder;
            choice.device_name    = c.label;
            choice.speedup        = cached.speedup;
            choice.hw_fps         = cached.hw_fps;
            choice.sw_fps         = cached.sw_fps;
            choice.reason         = msg::using_encoder(c.label, c.encoder, cached.speedup);
            choice.detail = fmt::format("{} on {}: {:.0f} fps vs {} {:.0f} fps = {:.2f}x{}",
                                        c.encoder, c.label, cached.hw_fps, sw_encoder,
                                        cached.sw_fps, cached.speedup,
                                        use_cached ? " (cached)" : "");
            return choice;
        }

        // Did not pass. Remember why, keep looking at the next candidate.
        if (!cached.error.empty() && c.probed) {
            probe_notes += fmt::format("{} not usable here{}\n", c.encoder,
                                       use_cached ? " (cached)" : "");
            continue;
        }
        if (first_failure_reason.empty()) {
            if (!cached.error.empty()) {
                first_failure_reason = msg::sw_gpu_encoder_failed(c.label, c.encoder);
                first_failure_detail = fmt::format("{} on {} failed to run{}:\n{}", c.encoder, c.label,
                                                   use_cached ? " (cached result)" : "", cached.error);
            } else {
                first_failure_reason = msg::sw_gpu_too_slow(c.label, cached.speedup, min_speedup);
                first_failure_detail = fmt::format(
                    "{} on {}: {:.0f} fps vs {} {:.0f} fps = {:.2f}x, below the {:.1f}x bar{}",
                    c.encoder, c.label, cached.hw_fps, sw_encoder, cached.sw_fps, cached.speedup,
                    min_speedup, use_cached ? " (cached)" : "");
            }
        }
    }

    software.reason = first_failure_reason.empty() ? msg::sw_no_gpu() : first_failure_reason;
    if (!first_failure_detail.empty()) {
        software.detail = first_failure_detail;
    } else if (!probe_notes.empty()) {
        software.detail = "no hardware encoder available:\n" + probe_notes;
    } else {
        software.detail = "no candidate passed";
    }
    return software;
}

// ---------------------------------------------------------------------------
// Encoder arguments
// ---------------------------------------------------------------------------

bool uses_two_pass(const EncoderChoice& enc) {
    return enc.backend == EncoderBackend::Software;
}

std::string preset_arg(const EncoderChoice& enc, SpeedPreset speed) {
    switch (enc.backend) {
        case EncoderBackend::Software:
            // Fast deliberately returns empty: the original omitted -preset
            // entirely, leaving the encoder on its own default (medium).
            return speed == SpeedPreset::Optimal ? "veryslow" : "";
        case EncoderBackend::NVENC:
            // p7 is NVENC's slowest/highest quality, p4 its balanced default.
            return speed == SpeedPreset::Optimal ? "p7" : "p4";
        case EncoderBackend::QSV:
            return speed == SpeedPreset::Optimal ? "veryslow" : "medium";
        case EncoderBackend::AMF:
            return speed == SpeedPreset::Optimal ? "quality" : "balanced";
        default:
            return "";
    }
}

std::vector<std::string> quality_args(const EncoderChoice& enc,
                                      VideoCodec codec,
                                      CompressionLevel level) {
    const bool hevc = (codec == VideoCodec::H265);

    switch (enc.backend) {
        case EncoderBackend::Software:
            return {"-crf", std::to_string(kSoftwareQuality.for_level(level))};

        case EncoderBackend::NVENC: {
            const auto& table = hevc ? kHardwareHevcQuality : kHardwareH264Quality;
            return {"-rc", "vbr",
                    "-cq", std::to_string(table.for_level(level)),
                    // Without an explicit bitrate cap NVENC's VBR treats -cq as
                    // advisory and can drift; 0 means "no cap, obey cq".
                    "-b:v", "0",
                    "-multipass", "fullres"};
        }

        case EncoderBackend::QSV: {
            const auto& table = hevc ? kHardwareHevcQuality : kHardwareH264Quality;
            return {"-global_quality", std::to_string(table.for_level(level)),
                    "-look_ahead", "1"};
        }

        case EncoderBackend::AMF: {
            const auto& table = hevc ? kHardwareHevcQuality : kHardwareH264Quality;
            const std::string q = std::to_string(table.for_level(level));
            return {"-rc", "cqp", "-qp_i", q, "-qp_p", q};
        }

        case EncoderBackend::VideoToolbox:
            // hevc_videotoolbox's constant-quality mode. FFmpeg only permits -q:v on
            // Apple Silicon; on an Intel Mac the encoder refuses it, the benchmark
            // (which uses the same flag) fails, and the software encoder is chosen.
            return {"-q:v", std::to_string(kVideoToolboxQuality.for_level(level))};

        default:
            return {"-crf", std::to_string(kSoftwareQuality.for_level(level))};
    }
}

std::vector<std::string> bitrate_args(const EncoderChoice& enc, std::uint64_t video_bps) {
    const std::string bps = std::to_string(video_bps);

    switch (enc.backend) {
        case EncoderBackend::Software:
            // The caller adds -pass/-passlogfile; this is just the target.
            return {"-b:v", bps};

        case EncoderBackend::NVENC:
            // Measured on an RTX 3060 against a deliberately hard synthetic
            // clip: constrained VBR with internal multipass landed +6.5% over
            // the requested size, CBR +1.1%. VBR is kept because it spends bits
            // where they matter, and the overshoot is well inside tolerance.
            return {"-rc", "vbr",
                    "-b:v", bps,
                    "-maxrate", std::to_string(video_bps * 3 / 2),
                    "-bufsize", std::to_string(video_bps * 2),
                    "-multipass", "fullres"};

        case EncoderBackend::QSV:
            return {"-b:v", bps,
                    "-maxrate", std::to_string(video_bps * 3 / 2),
                    "-bufsize", std::to_string(video_bps * 2),
                    "-look_ahead", "1"};

        case EncoderBackend::AMF:
            return {"-rc", "vbr_peak",
                    "-b:v", bps,
                    "-maxrate", std::to_string(video_bps * 3 / 2)};

        default:
            return {"-b:v", bps};
    }
}

std::vector<std::string> compatibility_args(const EncoderChoice& enc, VideoCodec codec) {
    std::vector<std::string> args;

    // 8-bit 4:2:0 is what every hardware decoder handles. A 10-bit or 4:2:2
    // source would otherwise carry its pixel format into the output and lock
    // out exactly the machines this needs to play on.
    args.push_back("-pix_fmt");
    args.push_back("yuv420p");

    if (codec == VideoCodec::H265) {
        // MP4 defaults to the hev1 tag, which QuickTime, Safari, Apple devices
        // and a number of set-top players refuse outright. hvc1 is the same
        // bitstream with the parameter sets in the sample description.
        args.push_back("-tag:v");
        args.push_back("hvc1");

        // Main profile == 8-bit. Explicit so a 10-bit source cannot promote it.
        if (enc.backend == EncoderBackend::Software) {
            args.push_back("-profile:v");
            args.push_back("main");
        }
    } else {
        args.push_back("-profile:v");
        args.push_back("high");
        args.push_back("-level");
        args.push_back("4.1");
    }

    return args;
}

}  // namespace conv
