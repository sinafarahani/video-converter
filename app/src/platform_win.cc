#include "platform.h"

#include <windows.h>

#include <shlobj.h>

#include <array>

namespace converter {

std::filesystem::path executable_dir() {
    // MAX_PATH is not the real limit on modern Windows; a generous buffer
    // avoids truncating a deeply nested per-user install path.
    std::array<wchar_t, 32768> buffer{};
    const DWORD n =
        ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (n == 0 || n >= buffer.size()) return std::filesystem::current_path();
    return std::filesystem::path(std::wstring(buffer.data(), n)).parent_path();
}

std::filesystem::path resources_dir() {
    // Everything ships flat beside converter.exe.
    return executable_dir();
}

std::filesystem::path user_data_dir() {
    PWSTR raw = nullptr;
    // LocalAppData is writable without administrator rights, which is the whole
    // point for a per-user install on a locked-down machine.
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) {
        std::filesystem::path base(raw);
        ::CoTaskMemFree(raw);

        const auto dir = base / L"VideoConverter";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (!ec) return dir;
    }
    return executable_dir();
}

}  // namespace converter
