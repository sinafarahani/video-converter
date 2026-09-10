#include "conv/logging.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <system_error>

#include <fmt/format.h>

#include "conv/util.hpp"

namespace conv {
namespace {

std::tm local_time_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

std::string stamp(const char* format) {
    const std::tm tm = local_time_now();
    char buf[64];
    std::strftime(buf, sizeof(buf), format, &tm);
    return buf;
}

}  // namespace

struct RunLog::Impl {
    std::filesystem::path dir;
    std::filesystem::path file;
    std::ofstream         out;
    mutable std::mutex    mutex;
    bool                  opened = false;
    bool                  failed = false;

    // Opens on first use, so an app that is launched and closed without
    // converting anything leaves no file behind.
    void ensure_open() {
        if (opened || failed) return;

        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            failed = true;
            return;
        }

        const std::string ts = stamp("%Y-%m-%d_%H-%M-%S");
        file = dir / path_from_utf8(fmt::format("log_{}.txt", ts));

        out.open(file, std::ios::app | std::ios::binary);
        if (!out) {
            failed = true;
            return;
        }
        opened = true;

        out << fmt::format("=== Log started at {} ===\n", ts);
        out.flush();
    }
};

RunLog::RunLog(std::filesystem::path dir) : impl_(std::make_unique<Impl>()) {
    impl_->dir = std::move(dir);
}

RunLog::~RunLog() {
    close();
}

void RunLog::write(std::string_view message) {
    std::lock_guard lock(impl_->mutex);
    impl_->ensure_open();
    if (!impl_->opened) return;

    impl_->out << fmt::format("[{}] {}\n", stamp("%H:%M:%S"), message);
    // Flushed every line: when a conversion crashes or the machine loses power
    // mid-encode, the log is the only record of how far it got.
    impl_->out.flush();
}

std::filesystem::path RunLog::path() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->file;
}

void RunLog::close() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->opened && impl_->out.is_open()) {
        impl_->out.flush();
        impl_->out.close();
    }
    impl_->opened = false;
}

}  // namespace conv
