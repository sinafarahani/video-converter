// Small, dependency-free helpers: path/UTF-8 conversion, size parsing, and the
// media extension sets.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
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

// The two sets the original app matched on, kept byte-for-byte identical.
//
// Known wart, preserved on purpose pending a decision: four of the audio
// entries are not extensions anyone's files actually use -- "musepack" is
// really .mpc, "atrac" is .oma/.aa3, "wavpack" is .wv, and "alac" is .m4a --
// so those four can never match. Changing them would change which files get
// processed, so they stay until that is agreed.
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

// Case-insensitive path equality. Windows paths are case-insensitive; comparing
// the strings directly would treat C:\A.MP4 and C:\a.mp4 as different files and
// delete the wrong one.
bool same_path(const std::filesystem::path& a, const std::filesystem::path& b);

// Human-readable byte count for log lines, e.g. "1.4 GB".
std::string format_bytes(std::uint64_t bytes);

}  // namespace conv
