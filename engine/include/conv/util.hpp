// Small, dependency-free helpers: path/UTF-8 conversion, size parsing, and the
// media extension sets.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace conv {

// ---------------------------------------------------------------------------
// UTF-8 <-> native path
// ---------------------------------------------------------------------------
//
// This matters more than it looks. On MSVC, std::filesystem::path constructed
// from a narrow std::string decodes it using the *active ANSI code page*, not
// UTF-8. Every Persian path arriving from the JavaScript side is UTF-8, so
// constructing a path directly from it silently mangles the name and the file
// is never found -- or worse, is created under a corrupted name.
//
// These two functions are the only sanctioned way to cross that boundary.
// Nothing in the engine should build a path from a std::string directly.
std::filesystem::path path_from_utf8(std::string_view utf8);
std::string           path_to_utf8(const std::filesystem::path& p);

// ---------------------------------------------------------------------------
// Size input parsing
// ---------------------------------------------------------------------------
//
// Reproduces the original rules exactly:
//   "1GB" / "1 gb"   -> 1 * 1024^3
//   "500MB"          -> 500 * 1024^2
//   bare number < 16 -> gigabytes   (so "1" means 1 GB)
//   bare number >= 16-> megabytes   (so "500" means 500 MB)
//
// Note these are binary multipliers (1024-based), matching the Java original.
//
// Differs from the original in one way, deliberately: bad input returns an empty
// optional instead of throwing. In the Java version an empty or non-numeric size
// field threw out of the worker thread, which died silently and left the UI
// wedged in its "running" state with only Cancel to recover.
//
// Persian and Arabic-Indic digits are accepted and normalised, since the UI is
// Persian and a user typing ۵۰۰ would otherwise hit an unexplained failure.
std::optional<std::uint64_t> parse_size_input(std::string_view text);

// Normalises Persian (۰-۹) and Arabic-Indic (٠-٩) digits to ASCII, leaving
// everything else untouched.
std::string normalize_digits(std::string_view utf8);

// ---------------------------------------------------------------------------
// Extensions
// ---------------------------------------------------------------------------

// Lowercased extension without the dot; empty when there is none.
std::string lower_extension(const std::filesystem::path& p);

// The real file extensions of each set, lowercase, without the dot, in the
// original app's order. These are what the file picker filters on and what the
// installers register with the OS, so they list only extensions files actually
// carry.
std::span<const std::string_view> video_extensions();
std::span<const std::string_view> audio_extensions();

// Membership tests on a lower_extension() result.
//
// Four entries in the Java list were codec names rather than extensions --
// "musepack" is really .mpc, "atrac" .oma/.aa3, "wavpack" .wv and "alac" .m4a.
// The real extensions are in audio_extensions(); the codec-name spellings are
// still accepted here, since dropping them could only ever lose a match, but
// they are not advertised anywhere.
bool is_video_extension(std::string_view lower_ext);
bool is_audio_extension(std::string_view lower_ext);

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

// "<stem>.mp4" next to the original, preserving the directory.
std::filesystem::path with_extension(const std::filesystem::path& p, std::string_view new_ext);

// A unique sibling path used while encoding, so a crash never leaves a partial
// file where the real output belongs. Includes a random token so two runs (or
// two files with colliding stems) cannot fight over the same temp name.
std::filesystem::path make_temp_sibling(const std::filesystem::path& target,
                                        std::string_view new_ext);

// True when the file name has exactly the shape make_temp_sibling() gives its
// output, so a temp file orphaned by a crash is never picked up as input.
bool is_temp_sibling(const std::filesystem::path& p);

// Case-insensitive path equality. Windows paths are case-insensitive; comparing
// the strings directly would treat C:\A.MP4 and C:\a.mp4 as different files and
// delete the wrong one. When both exist the file system decides; otherwise
// Windows compares with the file system's own case rules (any script, not only
// ASCII).
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b);

// A comparison key: the UTF-8 of the lexically normal, native-separator path,
// case-folded on Windows (NTFS's rules, every script) and on macOS (ASCII only)
// and left alone on Linux. For de-duplication and collision checks only -- it
// is not a displayable path, and it never touches the disk, so pass a
// weakly_canonical path when links or 8.3 short names must compare equal.
// Where the folding falls short (non-ASCII case on macOS, Unicode
// normalisation), file_id() below is the safety net.
std::string path_key(const std::filesystem::path& p);

// The identity of an existing file: its volume and its file number there.
// Two paths with the same identity are one file, whatever their names -- the
// check path_key() cannot make for every file system.
struct FileId {
    std::uint64_t volume = 0;
    std::uint64_t high   = 0;
    std::uint64_t low    = 0;
    bool operator==(const FileId&) const = default;
};
// nullopt when the file does not exist or cannot be opened.
std::optional<FileId> file_id(const std::filesystem::path& p);

// Human-readable byte count for log lines, e.g. "1.4 GB".
std::string format_bytes(std::uint64_t bytes);

}  // namespace conv
