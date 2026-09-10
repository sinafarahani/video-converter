// Core value types for the conversion engine.
//
// Everything here is plain data with no CEF, no Win32 and no I/O, so the engine
// can be unit-tested and reused from a different frontend.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace conv {

// ---------------------------------------------------------------------------
// User-facing settings (mirrors the controls in the UI)
// ---------------------------------------------------------------------------

// Where converted files go.
enum class OutputMode {
    Replace,  // جاگزین کردن -- write next to the input and remove the original
    CopyTo,   // کپی به آدرس  -- mirror the input tree(s) under an output directory
};

// What to do when the destination file already exists. Only meaningful in
// CopyTo mode -- Replace mode has nothing to collide with.
enum class ExistingFilePolicy {
    Overwrite,  // جاگزین کردن
    Skip,       // پرش
};

// Encoder effort. Fast leaves the encoder on its own default preset (medium),
// exactly as the Java version did by simply omitting -preset.
enum class SpeedPreset {
    Fast,     // سریع
    Optimal,  // بهینه -- maps to `-preset veryslow`
};

// How the target bitrate is decided.
enum class SizeMode {
    Manual,     // دستی   -- hit a specific output size, via two-pass ABR
    Automatic,  // خودکار -- constant-quality, size falls where it falls
};

// Only meaningful when SizeMode::Automatic.
enum class CompressionLevel {
    Low,      // کم       -- largest files, best quality
    Medium,   // متوسط
    High,     // زیاد
    Extreme,  // خیلی زیاد -- smallest files
};

struct Settings {
    std::vector<std::filesystem::path> inputs;  // folders and/or files, in the order given
    std::filesystem::path              output_dir;

    OutputMode         output_mode     = OutputMode::Replace;
    ExistingFilePolicy existing_policy = ExistingFilePolicy::Overwrite;
    SpeedPreset        speed           = SpeedPreset::Optimal;
    SizeMode           size_mode       = SizeMode::Manual;

    // SizeMode::Manual only. Bytes.
    std::uint64_t max_size_bytes = 0;

    // SizeMode::Automatic only.
    CompressionLevel level = CompressionLevel::Medium;

    // Not exposed in the UI. Forces the software encoder regardless of what
    // hardware is present -- for testing the two-pass path on a machine that
    // would otherwise pick its GPU, and as an escape hatch if a driver
    // misbehaves in the field.
    bool force_software_encoder = false;
};

// ---------------------------------------------------------------------------
// Codec selection
// ---------------------------------------------------------------------------

enum class VideoCodec { H265, H264 };

// The single switch that decides what every output file contains.
//
// H.265 was chosen because the target audience plays videos in VLC, PotPlayer
// and KMPlayer -- all three ship their own HEVC decoder, so nothing needs to be
// installed -- and because Intel iGPUs have decoded HEVC in hardware since
// Skylake (2015), so even machines with no discrete GPU decode it cheaply.
//
// If a real machine ever turns out to struggle, changing this one line to
// VideoCodec::H264 switches the entire app: encoder names, quality values and
// the container tag all follow from it. Nothing else needs to be touched.
inline constexpr VideoCodec kOutputCodec = VideoCodec::H265;

// Which piece of silicon does the encoding.
enum class EncoderBackend {
    Software,        // libx265 / libx264
    NVENC,           // NVIDIA   (Windows + Linux)
    QSV,             // Intel    (Windows + Linux)
    AMF,             // AMD      (Windows only)
    VAAPI,           // AMD/Intel on Linux
    VideoToolbox,    // macOS
};

// Resolved once per run: which encoder to actually use, and why.
struct EncoderChoice {
    EncoderBackend backend = EncoderBackend::Software;
    std::string    ffmpeg_encoder;  // e.g. "libx265", "hevc_nvenc"
    std::string    device_name;     // for the log: "NVIDIA GeForce RTX 4070"
    std::string    reason;          // one sentence for the user, in Persian
    std::string    detail;          // the numbers behind it, for the run log
    double         speedup = 1.0;   // measured vs software, 1.0 when unknown
    double         hw_fps  = 0.0;   // measured throughput of the chosen encoder
    double         sw_fps  = 0.0;   // measured throughput of the software encoder
};

// ---------------------------------------------------------------------------
// Quality tables
// ---------------------------------------------------------------------------
//
// Two separate scales, because they are not comparable:
//
//  * Software encoders take a true CRF. HEVC and H.264 happen to use the same
//    numeric range here because the user tunes them the same way in practice.
//
//  * Hardware encoders take a constant-quality parameter that behaves like CRF
//    but is calibrated differently per vendor. The HEVC values sit ~8 higher
//    than the software ones, which is the offset the user arrived at from
//    experience and matches how NVENC's -cq scale behaves for HEVC.
//
// The H.264 hardware row is an ESTIMATE and has not been validated against real
// footage -- it only matters if kOutputCodec is ever switched to H264. NVENC's
// H.264 mode tracks x264's CRF scale far more closely than its HEVC mode does,
// so the +8 offset would be badly wrong there; +2 is the conservative guess.
struct QualityTable {
    int low, medium, high, extreme;

    constexpr int for_level(CompressionLevel l) const {
        switch (l) {
            case CompressionLevel::Low:     return low;
            case CompressionLevel::Medium:  return medium;
            case CompressionLevel::High:    return high;
            case CompressionLevel::Extreme: return extreme;
        }
        return medium;
    }
};

inline constexpr QualityTable kSoftwareQuality{ .low = 14, .medium = 20, .high = 26, .extreme = 30 };
inline constexpr QualityTable kHardwareHevcQuality{ .low = 22, .medium = 28, .high = 34, .extreme = 38 };
inline constexpr QualityTable kHardwareH264Quality{ .low = 16, .medium = 22, .high = 28, .extreme = 32 };

// hevc_videotoolbox's -q:v runs the other way: 1-100, higher is better. These
// values are an ESTIMATE meant to sit roughly level with the hardware HEVC row
// above; they have not yet been compared against real footage on a Mac.
inline constexpr QualityTable kVideoToolboxQuality{ .low = 70, .medium = 60, .high = 50, .extreme = 42 };

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

// One audio stream as reported by ffprobe, plus what our loudness analysis
// concluded about it.
struct AudioStreamInfo {
    int         index = 0;        // absolute stream index within the file
    int         audio_index = 0;  // index among audio streams only (the N in 0:a:N)
    std::string codec_name;
    std::string language;         // from stream tags, may be empty
    std::string title;            // from stream tags, may be empty
    int         channels = 0;

    // Filled in by the loudness pass. Decibels relative to full scale; ffmpeg
    // reports -91.0 for digital silence.
    double mean_db = -91.0;
    double peak_db = -91.0;

    bool has_content = false;  // survived the "is anything actually on this track" test
    bool selected    = false;  // will be muxed into the output

    // audio_index of the other half of a detected stereo pair, or -1.
    //
    // Broadcast MXF stores audio as discrete mono tracks, so a stereo
    // recording arrives as two adjacent tracks that both carry content. Left
    // as separate streams they play as two mono tracks and double the audio
    // bitrate; recognised as a pair they are joined into one stereo stream.
    // Set on both halves; the lower index is the one that gets mapped.
    int stereo_partner = -1;

    bool is_pair_leader() const { return stereo_partner > audio_index; }
    bool is_pair_follower() const { return stereo_partner >= 0 && stereo_partner < audio_index; }
};

// Thresholds for deciding whether an audio track carries real content.
//
// The motivating case: studio recordings with ~6 microphone tracks where only
// one has someone speaking on it, and which one changes from file to file. A
// dead mic sits at its noise floor, typically -60 dB and below; a live mic
// carrying speech averages around -25 to -35 dB. The gap is wide, so these
// thresholds are not delicate -- but they are named constants precisely so they
// can be retuned once we have measurements from real footage.
struct AudioDetectionConfig {
    // Above this average level, a track is considered to have content.
    double mean_floor_db = -50.0;

    // A track that is mostly silent but has clear peaks -- someone speaking
    // briefly in an otherwise quiet room -- still counts.
    double peak_floor_db = -35.0;

    // ...but it must also be within this much of the loudest track, so that a
    // room-tone track does not get promoted just because it has a door slam.
    double relative_gap_db = 20.0;

    // Analysing every second of a two-hour file is wasteful when we only need
    // to know which mic is live. Sample this many seconds from each of several
    // points spread through the file. Zero means analyse the whole thing.
    int sample_seconds_per_probe = 30;
    int probe_count              = 3;

    // Two adjacent mono tracks that both have content and sit within this
    // many dB of each other are treated as the left and right of one stereo
    // source. A genuine L/R pair is usually within 3 dB; two unrelated live
    // microphones can differ by far more.
    double stereo_pair_max_gap_db = 8.0;

    // Whether to pair at all. Off means every selected track stays mono.
    bool   detect_stereo_pairs = true;
};

// ---------------------------------------------------------------------------
// Probe results
// ---------------------------------------------------------------------------

struct MediaInfo {
    bool   valid = false;
    double duration_seconds = 0.0;
    bool   has_video = false;

    int    video_stream_index = -1;
    int    width  = 0;
    int    height = 0;
    std::string video_codec_name;

    std::vector<AudioStreamInfo> audio_streams;
};

// ---------------------------------------------------------------------------
// Per-file outcome, for the log and the summary
// ---------------------------------------------------------------------------

enum class FileResult {
    Converted,
    Remuxed,        // already small enough; container-swapped without re-encoding
    SkippedExists,  // destination present and the policy said skip
    SkippedNotMedia,
    Failed,
    Cancelled,
};

}  // namespace conv
