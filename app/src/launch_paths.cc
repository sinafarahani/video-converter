#include "launch_paths.h"

#include <string_view>
#include <system_error>
#include <utility>

#include "conv/util.hpp"

namespace converter {
namespace {

// Paths that arrived before the client existed. Namespace scope rather than a
// function-local static because the macOS build uses -fno-threadsafe-statics;
// it is only ever touched on the UI thread anyway.
std::vector<QueuedPaths> g_queued;

bool starts_with_icase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != prefix[i]) return false;
    }
    return true;
}

#if defined(_WIN32)
bool is_ascii_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
#endif

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "%20" -> " ". Works on the UTF-8 bytes, so an escaped Persian file name comes
// back intact. A '%' not followed by two hex digits is kept as it is.
std::string percent_decode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            const int hi = hex_value(in[i + 1]);
            const int lo = hex_value(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

// A file URL becomes a path; anything else is returned unchanged. Linux file
// managers can hand over URLs, and a user may paste one into a terminal.
//
//   file:///C:/a%20b.mp4        -> C:/a b.mp4        (Windows)
//   file:///home/u/a.mp4        -> /home/u/a.mp4
//   file://localhost/home/u/x   -> /home/u/x
//   file://server/share/a.mp4   -> //server/share/a.mp4  (a UNC path on Windows;
//                                  elsewhere it simply will not exist and the
//                                  bridge reports it)
//
// Only the "file:/" and "file://" forms count: a relative name that merely
// starts with "file:" is left alone.
std::string file_url_to_path(std::string_view arg) {
    if (!starts_with_icase(arg, "file:/")) return std::string(arg);

    std::string_view rest = arg.substr(5);  // after "file:"
    // A query or fragment is never part of the name; a real '#' arrives as %23.
    rest = rest.substr(0, rest.find_first_of("?#"));

    std::string_view host;
    if (rest.size() >= 2 && rest[0] == '/' && rest[1] == '/') {
        rest.remove_prefix(2);
        const size_t slash = rest.find('/');
        host = rest.substr(0, slash);
        rest = (slash == std::string_view::npos) ? std::string_view{} : rest.substr(slash);
    }

    std::string path = percent_decode(rest);

#if defined(_WIN32)
    // A drive letter in the host slot ("file://C:/x") is malformed but common.
    if (host.size() == 2 && is_ascii_letter(host[0]) && (host[1] == ':' || host[1] == '|')) {
        return std::string(1, host[0]) + ":" + path;
    }
    // "/C:/dir/a.mp4" -> "C:/dir/a.mp4". '|' is the old spelling of the colon.
    if (path.size() >= 3 && path[0] == '/' && is_ascii_letter(path[1]) &&
        (path[2] == ':' || path[2] == '|')) {
        path.erase(0, 1);
        path[1] = ':';
    }
#endif

    if (host.empty() || (host.size() == 9 && starts_with_icase(host, "localhost"))) return path;
    return "//" + std::string(host) + path;
}

}  // namespace

std::vector<std::string> paths_from_command_line(CefRefPtr<CefCommandLine> cl,
                                                 const std::filesystem::path& cwd) {
    std::vector<std::string> out;
    if (!cl || !cl->IsValid() || !cl->HasArguments()) return out;

    CefCommandLine::ArgumentList args;
    cl->GetArguments(args);
    out.reserve(args.size());

    for (const CefString& arg : args) {
        std::string text = arg.ToString();

#if defined(__APPLE__)
        // The process serial number older Finder versions pass ("-psn_0_1234").
        // CEF already reads it as a switch; this only matters after a "--".
        if (text.rfind("-psn_", 0) == 0) continue;
#endif

#if defined(_WIN32)
        // Explorer quotes a drive root as "D:\" and the Windows argument rules
        // read \" as an escaped quote, so it arrives here as D:". A quote can
        // never be part of a Windows file name; it stands for the lost backslash.
        if (!text.empty() && text.back() == '"') text.back() = '\\';
#endif

        if (text.empty()) continue;

        std::filesystem::path p = conv::path_from_utf8(file_url_to_path(text));
        if (p.empty()) continue;

        if (p.is_relative()) {
            if (!cwd.empty()) {
                p = cwd / p;
            } else {
                std::error_code ec;
                auto absolute = std::filesystem::absolute(p, ec);
                if (!ec) p = std::move(absolute);
            }
        }

        out.push_back(conv::path_to_utf8(p.lexically_normal().make_preferred()));
    }
    return out;
}

void queue_launch_paths(std::vector<std::string> utf8_paths, std::string source) {
    if (utf8_paths.empty()) return;
    g_queued.push_back({std::move(utf8_paths), std::move(source)});
}

std::vector<QueuedPaths> take_queued_launch_paths() {
    return std::exchange(g_queued, {});
}

}  // namespace converter
