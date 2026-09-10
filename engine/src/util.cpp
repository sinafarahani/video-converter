#include "conv/util.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <iterator>
#include <random>
#include <string_view>
#include <system_error>

#include <fmt/format.h>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/stat.h>
#endif

namespace conv {
namespace {

std::string to_upper_ascii(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

std::string trim(std::string_view s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    auto begin = std::find_if(s.begin(), s.end(), not_space);
    auto end   = std::find_if(s.rbegin(), s.rend(), not_space).base();
    return (begin < end) ? std::string(begin, end) : std::string{};
}

// Parses a decimal number that may use '.' or the Persian decimal separator.
std::optional<double> parse_double(std::string_view s) {
    if (s.empty()) return std::nullopt;
    // from_chars for double is available on MSVC and recent libstdc++, but the
    // input here is short and user-typed; strtod with an explicit end check is
    // simpler and behaves identically for our purposes.
    std::string buf(s);
    char*  end = nullptr;
    errno = 0;
    const double v = std::strtod(buf.c_str(), &end);
    if (end == buf.c_str() || errno == ERANGE) return std::nullopt;
    // Anything left over that is not whitespace makes this not a number.
    while (end && *end) {
        if (!std::isspace(static_cast<unsigned char>(*end))) return std::nullopt;
        ++end;
    }
    if (!(v > 0.0) || !std::isfinite(v)) return std::nullopt;
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// UTF-8 <-> native path
// ---------------------------------------------------------------------------

std::filesystem::path path_from_utf8(std::string_view utf8) {
#ifdef _WIN32
    if (utf8.empty()) return {};
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0) return {};
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                          w.data(), wlen);
    return std::filesystem::path(w);
#else
    // POSIX paths are byte strings; UTF-8 passes through unchanged.
    return std::filesystem::path(std::string(utf8));
#endif
}

std::string path_to_utf8(const std::filesystem::path& p) {
#ifdef _WIN32
    const std::wstring& w = p.native();
    if (w.empty()) return {};
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          out.data(), len, nullptr, nullptr);
    return out;
#else
    return p.native();
#endif
}

// ---------------------------------------------------------------------------
// Digits
// ---------------------------------------------------------------------------

std::string normalize_digits(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());

    for (size_t i = 0; i < utf8.size();) {
        const auto b0 = static_cast<unsigned char>(utf8[i]);

        // Extended Arabic-Indic (Persian) digits U+06F0..U+06F9 -> DB B0..B9
        if (b0 == 0xDB && i + 1 < utf8.size()) {
            const auto b1 = static_cast<unsigned char>(utf8[i + 1]);
            if (b1 >= 0xB0 && b1 <= 0xB9) {
                out.push_back(static_cast<char>('0' + (b1 - 0xB0)));
                i += 2;
                continue;
            }
        }
        // Arabic-Indic digits U+0660..U+0669 -> D9 A0..A9
        if (b0 == 0xD9 && i + 1 < utf8.size()) {
            const auto b1 = static_cast<unsigned char>(utf8[i + 1]);
            if (b1 >= 0xA0 && b1 <= 0xA9) {
                out.push_back(static_cast<char>('0' + (b1 - 0xA0)));
                i += 2;
                continue;
            }
        }
        // Arabic decimal separator U+066B -> '.'
        if (b0 == 0xD9 && i + 1 < utf8.size() &&
            static_cast<unsigned char>(utf8[i + 1]) == 0xAB) {
            out.push_back('.');
            i += 2;
            continue;
        }

        out.push_back(utf8[i]);
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Size parsing
// ---------------------------------------------------------------------------

std::optional<std::uint64_t> parse_size_input(std::string_view text) {
    const std::string normalized = normalize_digits(text);
    const std::string s = to_upper_ascii(trim(normalized));
    if (s.empty()) return std::nullopt;

    constexpr std::uint64_t kKiB = 1024ull;
    constexpr std::uint64_t kMiB = kKiB * 1024ull;
    constexpr std::uint64_t kGiB = kMiB * 1024ull;

    const auto scaled = [](double value, std::uint64_t unit) -> std::optional<std::uint64_t> {
        const double bytes = value * static_cast<double>(unit);
        // Guard the cast: a user typing 99999999GB would otherwise wrap.
        if (bytes >= 9.0e18) return std::nullopt;
        const auto v = static_cast<std::uint64_t>(bytes);
        return v > 0 ? std::optional<std::uint64_t>(v) : std::nullopt;
    };

    if (s.ends_with("GB")) {
        const auto v = parse_double(std::string_view(s).substr(0, s.size() - 2));
        return v ? scaled(*v, kGiB) : std::nullopt;
    }
    if (s.ends_with("MB")) {
        const auto v = parse_double(std::string_view(s).substr(0, s.size() - 2));
        return v ? scaled(*v, kMiB) : std::nullopt;
    }
    if (s.ends_with("KB")) {
        const auto v = parse_double(std::string_view(s).substr(0, s.size() - 2));
        return v ? scaled(*v, kKiB) : std::nullopt;
    }

    // Bare number. Preserving the original's rule exactly: under 16 means
    // gigabytes, 16 and over means megabytes. It reads as a hack, but it is
    // what users of this app have been typing for a year, so it stays.
    const auto v = parse_double(s);
    if (!v) return std::nullopt;
    return scaled(*v, (*v < 16.0) ? kGiB : kMiB);
}

// ---------------------------------------------------------------------------
// Extensions
// ---------------------------------------------------------------------------

std::string lower_extension(const std::filesystem::path& p) {
    std::string ext = path_to_utf8(p.extension());
    if (!ext.empty() && ext.front() == '.') ext.erase(ext.begin());
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

// The tables live here, at namespace scope, so the predicates and the lists
// handed to the file picker are the same data and cannot drift apart.
// cmake/CheckMediaExtensions.cmake parses the first two arrays to check the OS
// registrations against them (the pseudo-extensions below are never
// registered): keep one plain string literal per entry, and no comments inside
// the braces.
constexpr std::string_view kVideoExtensions[] = {
    "mp4", "avi", "mkv", "mov", "flv", "wmv", "mxf",
    "gxf", "lxf", "webm", "3gp", "ts", "m2ts", "ogv",
};

// The last five are the corrected spellings of four entries in the original
// list that were codec names rather than extensions:
//     musepack -> mpc          atrac -> oma (also .aa3)
//     wavpack  -> wv           alac  -> m4a
constexpr std::string_view kAudioExtensions[] = {
    "mp3", "aac", "wav", "flac", "ogg", "opus", "wma", "amr", "ape",
    "aiff", "aif", "au", "pcm",
    "mpc", "oma", "aa3", "wv", "m4a",
};

// The original codec-name spellings. Still accepted, since they cost nothing
// and removing them could only ever lose a match, but no file really carries
// them, so they are kept out of audio_extensions().
constexpr std::string_view kAudioPseudoExtensions[] = { "atrac", "musepack", "alac", "wavpack" };

std::span<const std::string_view> video_extensions() { return kVideoExtensions; }
std::span<const std::string_view> audio_extensions() { return kAudioExtensions; }

bool is_video_extension(std::string_view e) {
    return std::find(std::begin(kVideoExtensions), std::end(kVideoExtensions), e) !=
           std::end(kVideoExtensions);
}

bool is_audio_extension(std::string_view e) {
    return std::find(std::begin(kAudioExtensions), std::end(kAudioExtensions), e) !=
               std::end(kAudioExtensions) ||
           std::find(std::begin(kAudioPseudoExtensions), std::end(kAudioPseudoExtensions), e) !=
               std::end(kAudioPseudoExtensions);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

std::filesystem::path with_extension(const std::filesystem::path& p, std::string_view new_ext) {
    std::filesystem::path out = p;
    out.replace_extension(path_from_utf8(new_ext));
    return out;
}

std::filesystem::path make_temp_sibling(const std::filesystem::path& target,
                                        std::string_view new_ext) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const auto token = fmt::format("{:08x}", static_cast<std::uint32_t>(rng()));

    std::filesystem::path out = target;
    const std::string stem = path_to_utf8(target.stem());
    out.replace_filename(path_from_utf8(fmt::format("{}_tmp{}{}", stem, token, new_ext)));
    return out;
}

bool is_temp_sibling(const std::filesystem::path& p) {
    // Mirrors make_temp_sibling above: "<stem>_tmp" + 8 lowercase hex digits,
    // and only ever .mp4 or .mp3 (the two outputs the engine writes).
    constexpr std::string_view kMarker = "_tmp";
    constexpr size_t kTokenDigits = 8;

    const std::string ext = lower_extension(p);
    if (ext != "mp4" && ext != "mp3") return false;

    const std::string stem = path_to_utf8(p.stem());
    if (stem.size() < kMarker.size() + kTokenDigits) return false;
    const std::string_view tail = std::string_view(stem).substr(stem.size() - kTokenDigits);
    const std::string_view marker =
        std::string_view(stem).substr(stem.size() - kTokenDigits - kMarker.size(), kMarker.size());
    return marker == kMarker && std::all_of(tail.begin(), tail.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool same_path(const std::filesystem::path& a, const std::filesystem::path& b) {
    // If both exist, let the filesystem decide -- this handles hardlinks,
    // junctions and differing-but-equivalent spellings correctly.
    std::error_code ec;
    if (std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec)) {
        const bool eq = std::filesystem::equivalent(a, b, ec);
        if (!ec) return eq;
    }
#ifdef _WIN32
    // Fall back to a case-insensitive comparison of the native strings, with
    // the operating system's upper-case table -- what NTFS itself uses, so
    // "Ä.mp4" and "ä.mp4" are one name here as they are on disk. towlower in
    // the C locale folds ASCII only.
    const std::wstring& x = a.native();
    const std::wstring& y = b.native();
    return ::CompareStringOrdinal(x.data(), static_cast<int>(x.size()), y.data(),
                                  static_cast<int>(y.size()), TRUE) == CSTR_EQUAL;
#else
    return a.native() == b.native();
#endif
}

std::string path_key(const std::filesystem::path& p) {
    std::filesystem::path n = p.lexically_normal();
    n.make_preferred();
#ifdef _WIN32
    // Upper case by the file system's rules (LCMapStringEx without
    // LCMAP_LINGUISTIC_CASING), matching same_path's fallback, so the two never
    // disagree and a non-ASCII letter's case cannot make two keys for one file.
    std::wstring w = n.native();
    if (!w.empty()) {
        const int len = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, w.data(),
                                        static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr, 0);
        std::wstring up(static_cast<size_t>(len > 0 ? len : 0), L'\0');
        if (len > 0 && ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, w.data(),
                                       static_cast<int>(w.size()), up.data(), len, nullptr,
                                       nullptr, 0) == len) {
            w = std::move(up);
        } else {
            std::transform(w.begin(), w.end(), w.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(::towupper(c)); });
        }
    }
    return path_to_utf8(std::filesystem::path(std::move(w)));
#else
    std::string s = n.native();
#  ifdef __APPLE__
    // ASCII only: Persian has no case, and folding other scripts the way APFS
    // does would need ICU. A pair that differs only in a non-ASCII letter's
    // case is therefore treated as two names.
    std::transform(s.begin(), s.end(), s.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    });
#  endif
    return s;
#endif
}

std::optional<FileId> file_id(const std::filesystem::path& p) {
#ifdef _WIN32
    // No access rights needed for the identity; share everything so a file
    // another program has open (ffmpeg, a player) can still be asked.
    const HANDLE h = ::CreateFileW(p.c_str(), 0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    std::optional<FileId> id;
    // The 128-bit id is the unique one on ReFS; NTFS fills its low half.
    FILE_ID_INFO full{};
    BY_HANDLE_FILE_INFORMATION basic{};
    if (::GetFileInformationByHandleEx(h, FileIdInfo, &full, sizeof(full))) {
        std::uint64_t high = 0, low = 0;
        static_assert(sizeof(full.FileId.Identifier) == 16);
        std::memcpy(&low, full.FileId.Identifier, 8);
        std::memcpy(&high, full.FileId.Identifier + 8, 8);
        id = FileId{full.VolumeSerialNumber, high, low};
    } else if (::GetFileInformationByHandle(h, &basic)) {
        id = FileId{basic.dwVolumeSerialNumber, 0,
                    (static_cast<std::uint64_t>(basic.nFileIndexHigh) << 32) | basic.nFileIndexLow};
    }
    ::CloseHandle(h);
    return id;
#else
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) return std::nullopt;
    return FileId{static_cast<std::uint64_t>(st.st_dev), 0, static_cast<std::uint64_t>(st.st_ino)};
#endif
}

std::string format_bytes(std::uint64_t bytes) {
    constexpr double kKiB = 1024.0;
    const double b = static_cast<double>(bytes);
    if (b >= kKiB * kKiB * kKiB) return fmt::format("{:.2f} GB", b / (kKiB * kKiB * kKiB));
    if (b >= kKiB * kKiB)        return fmt::format("{:.1f} MB", b / (kKiB * kKiB));
    if (b >= kKiB)               return fmt::format("{:.0f} KB", b / kKiB);
    return fmt::format("{} B", bytes);
}

}  // namespace conv
