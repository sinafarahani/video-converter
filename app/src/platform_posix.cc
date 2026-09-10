#include "platform.h"

#include <cstdlib>
#include <system_error>
#include <vector>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#else
#  include <limits.h>
#  include <unistd.h>
#endif

namespace converter {
namespace {

std::filesystem::path home_dir() {
    if (const char* home = std::getenv("HOME")) return std::filesystem::path(home);
    return std::filesystem::current_path();
}

}  // namespace

std::filesystem::path executable_dir() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);  // asks for the required length
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::filesystem::current_path();
    }
    std::error_code ec;
    // Resolves the symlink chain, which matters when launched from a bundle.
    auto path = std::filesystem::canonical(std::filesystem::path(buffer.data()), ec);
    if (ec) path = std::filesystem::path(buffer.data());
    return path.parent_path();
#else
    std::error_code ec;
    auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return std::filesystem::current_path();
    return path.parent_path();
#endif
}

std::filesystem::path resources_dir() {
#if defined(__APPLE__)
    // Contents/MacOS/converter -> Contents/Resources
    return executable_dir().parent_path() / "Resources";
#else
    return executable_dir();
#endif
}

std::filesystem::path user_data_dir() {
    std::filesystem::path base;

#if defined(__APPLE__)
    base = home_dir() / "Library" / "Application Support";
#else
    // XDG says fall back to ~/.local/share when XDG_DATA_HOME is unset or is
    // not an absolute path.
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && xdg[0] == '/') {
        base = std::filesystem::path(xdg);
    } else {
        base = home_dir() / ".local" / "share";
    }
#endif

    const auto dir = base / "VideoConverter";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return executable_dir();
    return dir;
}

}  // namespace converter
