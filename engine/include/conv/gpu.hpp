// Deciding whether to encode on the GPU, and on which one.
//
// The requirement is "use the GPU only when it is genuinely worth it" -- at
// least twice as fast as software, and not on integrated Intel HD/UHD parts
// where it makes no sense. Rather than maintaining a table of every GPU model
// ever shipped and guessing, this does two things:
//
//   1. A cheap eligibility filter on the adapter (vendor, discrete-ness, VRAM,
//      and an explicit exclusion for the old Intel integrated parts).
//   2. An actual measured benchmark: encode a few seconds of synthetic video
//      with both the hardware and software encoders and compare throughput.
//
// Step 2 is what makes this honest. A speed claim that is measured on the
// machine it will run on cannot be wrong the way a hardcoded model list can.
// The result is cached, keyed by adapter identity, so it costs a few seconds
// once rather than on every run.
#pragma once

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "conv/ffmpeg.hpp"
#include "conv/types.hpp"

namespace conv {

enum class GpuVendor { Unknown, Nvidia, Amd, Intel, Apple };

struct GpuInfo {
    GpuVendor     vendor = GpuVendor::Unknown;
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::string   description;
    std::uint64_t dedicated_vram_bytes = 0;
    bool          is_discrete = false;
};

// Enumerates display adapters. Uses DXGI on Windows; returns an empty list on
// platforms where it is not implemented yet, which simply means "software".
std::vector<GpuInfo> enumerate_gpus();

// Does this adapter clear the bar for even being benchmarked?
//
// Excludes: software adapters (Microsoft Basic Render Driver), anything without
// meaningful dedicated memory, and Intel integrated graphics -- "HD Graphics"
// and "UHD Graphics" parts are specifically ruled out, as requested. Intel Arc
// is discrete and is allowed through to the benchmark.
bool is_encode_candidate(const GpuInfo& gpu);

// Which ffmpeg encoder name corresponds to this vendor on this platform, for
// the configured output codec. Empty when there is no hardware path.
std::string hardware_encoder_name(GpuVendor vendor, VideoCodec codec);

// Which encoders this ffmpeg build actually has compiled in. Some distributions
// omit the vendor SDKs, and asking is cheaper than failing mid-encode.
std::vector<std::string> available_encoders(const ToolPaths& tools);

// Steady-state throughput of one encoder on generated 1080p30 video.
//
// This deliberately does NOT time the whole ffmpeg run. The first version did,
// on a 3-second clip, and 44% of the hardware encoder's wall time turned out
// to be process startup and CUDA context creation rather than encoding. On a
// fast desktop CPU that skew alone pushed a real RTX 3050 under the 2x bar.
//
// Instead the frame counter in ffmpeg's own `-progress` output is sampled, and
// fps is computed from the frames and wall time between the first report and
// the last -- startup is outside that window by construction. The run is cut
// short once a few seconds of steady state have been observed, so this costs
// about three seconds per encoder regardless of how fast it is.
struct FpsMeasurement {
    bool        ok = false;
    double      fps = 0.0;
    int         frames = 0;   // frames inside the measured window
    double      seconds = 0.0;
    std::string error;        // ffmpeg's stderr, or the launch error, when !ok
};
FpsMeasurement measure_encoder_fps(const ToolPaths& tools,
                                   const std::string& encoder,
                                   const std::vector<std::string>& extra_args,
                                   const std::atomic_bool* cancel = nullptr);

// The whole decision, end to end. Enumerates, filters, checks encoder
// availability, benchmarks (or reads the cache), and returns what to use,
// with `reason` set to a user-facing sentence and `detail` to the numbers.
//
// `min_speedup` is the bar the hardware encoder has to clear; below it the
// software encoder wins on quality and the GPU is not worth the tradeoff.
//
// A hardware encoder that FAILS TO RUN -- typically an NVIDIA driver older
// than the NVENC API this ffmpeg was built against -- is reported as exactly
// that, with the ffmpeg error in `detail`, rather than being lumped in with
// "too slow". Those are different problems with different fixes.
EncoderChoice choose_encoder(const ToolPaths& tools,
                             const std::filesystem::path& cache_dir,
                             double min_speedup = 2.0,
                             const std::atomic_bool* cancel = nullptr,
                             bool force_software = false);

// ---------------------------------------------------------------------------
// Encoder arguments
// ---------------------------------------------------------------------------

// The encoder-specific flags for constant-quality (Automatic) mode.
//
// Each backend spells "constant quality" differently: x265 uses -crf, NVENC
// uses -rc vbr with -cq, QSV uses -global_quality, AMF uses -rc cqp with -qp_*.
// The numeric scales are not interchangeable either, which is why the quality
// tables in types.hpp are split by backend.
std::vector<std::string> quality_args(const EncoderChoice& enc,
                                      VideoCodec codec,
                                      CompressionLevel level);

// The encoder-specific flags for a target average bitrate (Manual mode).
//
// Pass number is deliberately not a parameter: -pass/-passlogfile are added by
// the caller only for software encoders, and the hardware encoders take the
// same flags on their single run regardless.
std::vector<std::string> bitrate_args(const EncoderChoice& enc, std::uint64_t video_bps);

// Flags that keep the output broadly decodable: 8-bit 4:2:0, Main profile, and
// -- for HEVC in MP4 -- the hvc1 tag rather than the hev1 default, without
// which Apple software and a number of hardware players refuse the file.
std::vector<std::string> compatibility_args(const EncoderChoice& enc, VideoCodec codec);

// Whether hitting a target size with this encoder needs two separate ffmpeg
// invocations.
//
// Only the software encoders use ffmpeg's -pass 1 / -pass 2 machinery. NVENC,
// QSV and AMF all do their look-ahead internally in a single run (NVENC calls
// it -multipass), so asking them for a second pass would just encode the file
// twice. This changes how per-file progress is apportioned, which is why it is
// exposed rather than buried.
bool uses_two_pass(const EncoderChoice& enc);

// The -preset value for this encoder at the given speed setting. Software
// encoders use x264/x265 preset names; NVENC uses p1..p7.
std::string preset_arg(const EncoderChoice& enc, SpeedPreset speed);

}  // namespace conv
