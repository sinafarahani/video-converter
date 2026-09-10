// Shared pieces of the two Process implementations (process_win.cpp and
// process_posix.cpp). Not part of the public engine interface.
#pragma once

#include <string>
#include <string_view>

#include "conv/ffmpeg.hpp"

namespace conv::detail {

// Turns a byte stream into complete lines, holding a partial trailing line
// until the rest of it arrives. ffmpeg's `-progress pipe:1` output only makes
// sense line by line, and a 4 KB read almost never lands on a line boundary.
class LineSplitter {
public:
    explicit LineSplitter(Process::LineHandler handler) : handler_(std::move(handler)) {}

    void feed(std::string_view chunk) {
        if (!handler_) return;
        buffer_.append(chunk);

        size_t start = 0;
        for (;;) {
            const size_t nl = buffer_.find('\n', start);
            if (nl == std::string::npos) break;

            size_t end = nl;
            if (end > start && buffer_[end - 1] == '\r') --end;  // CRLF
            handler_(std::string_view(buffer_).substr(start, end - start));
            start = nl + 1;
        }
        if (start > 0) buffer_.erase(0, start);

        // A process that never emits a newline must not be allowed to grow this
        // without bound. 1 MiB of line is not a line; hand it over as-is.
        if (buffer_.size() > (1u << 20)) flush();
    }

    // Emits whatever is left, for processes that do not end with a newline.
    void flush() {
        if (handler_ && !buffer_.empty()) {
            handler_(buffer_);
            buffer_.clear();
        }
    }

private:
    Process::LineHandler handler_;
    std::string          buffer_;
};

}  // namespace conv::detail
