// Windows implementation of Process. See the class comment in ffmpeg.hpp for
// why this is written directly against Win32 rather than a library.

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "conv/ffmpeg.hpp"
#include "process_common.hpp"

namespace conv {
namespace {

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string last_error_text(DWORD code) {
    LPWSTR buf = nullptr;
    const DWORD n = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string text = "error " + std::to_string(code);
    if (n && buf) {
        std::wstring w(buf, n);
        ::LocalFree(buf);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ')) w.pop_back();
        const int len = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                              nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string u(static_cast<size_t>(len), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), u.data(), len,
                                  nullptr, nullptr);
            text += ": " + u;
        }
    }
    return text;
}

struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    ~Handle() { reset(); }
    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE v = nullptr) {
        if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        h = v;
    }
    bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
};

enum class Stream { Out, Err };

struct Chunk {
    Stream      stream;
    std::string data;
};

// One per pipe. Blocks in ReadFile and hands chunks to the main loop; it never
// touches the callbacks itself, so the "callbacks run on the calling thread"
// contract holds and no locking is needed around the caller's state.
class Reader {
public:
    Reader(Stream stream, HANDLE pipe, std::mutex& m, std::vector<Chunk>& q, HANDLE wake,
           std::atomic_bool& failed, std::string& failure)
        : stream_(stream), pipe_(pipe), mutex_(m), queue_(q), wake_(wake),
          failed_(failed), failure_(failure) {
        thread_ = std::thread([this] { loop(); });
    }

    ~Reader() { join(); }

    bool done() const { return done_.load(std::memory_order_acquire); }

    // Aborts a blocking ReadFile so the thread can exit even if something is
    // still holding the write end open.
    void abort() {
        if (thread_.joinable()) ::CancelSynchronousIo(static_cast<HANDLE>(thread_.native_handle()));
    }

    void join() {
        if (thread_.joinable()) thread_.join();
    }

private:
    void loop() {
        std::vector<char> buf(16 * 1024);
        for (;;) {
            DWORD n = 0;
            const BOOL ok = ::ReadFile(pipe_, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr);
            if (!ok) {
                const DWORD e = ::GetLastError();
                // Broken pipe is the normal EOF: the child closed its end.
                // Anything else is reported so the main loop can decide.
                if (e != ERROR_BROKEN_PIPE && e != ERROR_OPERATION_ABORTED && e != ERROR_NO_DATA) {
                    std::lock_guard lock(mutex_);
                    failure_ = std::string(stream_ == Stream::Out ? "stdout" : "stderr") +
                               " read failed: " + last_error_text(e);
                    failed_.store(true, std::memory_order_release);
                }
                break;
            }
            if (n == 0) break;  // EOF
            {
                std::lock_guard lock(mutex_);
                queue_.push_back({stream_, std::string(buf.data(), n)});
            }
            ::SetEvent(wake_);
        }
        done_.store(true, std::memory_order_release);
        ::SetEvent(wake_);
    }

    Stream              stream_;
    HANDLE              pipe_;
    std::mutex&         mutex_;
    std::vector<Chunk>& queue_;
    HANDLE              wake_;
    std::atomic_bool&   failed_;
    std::string&        failure_;
    std::atomic_bool    done_{false};
    std::thread         thread_;
};

}  // namespace

// ---------------------------------------------------------------------------

struct Process::Impl {
    std::atomic_bool stop_requested{false};
    Outcome          outcome = Outcome::NotStarted;
    std::string      error;
};

Process::Process() : impl_(std::make_unique<Impl>()) {}
Process::~Process() = default;

void Process::request_stop() {
    impl_->stop_requested.store(true, std::memory_order_release);
}

Process::Outcome Process::outcome() const { return impl_->outcome; }
const std::string& Process::error() const { return impl_->error; }

// The rules the Microsoft C runtime applies when it splits a command line back
// into argv. Getting this wrong corrupts any path with a space or a quote, and
// Persian folder names with spaces are the normal case for this app.
std::string Process::quote_windows_command_line(const std::vector<std::string>& args) {
    std::string out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) out.push_back(' ');
        const std::string& a = args[i];

        const bool needs_quotes =
            a.empty() || a.find_first_of(" \t\n\v\"") != std::string::npos;
        if (!needs_quotes) {
            out += a;
            continue;
        }

        out.push_back('"');
        size_t backslashes = 0;
        for (const char c : a) {
            if (c == '\\') {
                ++backslashes;
                continue;
            }
            if (c == '"') {
                // Backslashes before a quote are literal and must be doubled,
                // then the quote itself is escaped.
                out.append(backslashes * 2 + 1, '\\');
                out.push_back('"');
            } else {
                out.append(backslashes, '\\');
                out.push_back(c);
            }
            backslashes = 0;
        }
        // Trailing backslashes precede the closing quote, so they are doubled.
        out.append(backslashes * 2, '\\');
        out.push_back('"');
    }
    return out;
}

std::optional<int> Process::run(const std::vector<std::string>& args,
                                LineHandler on_stdout,
                                LineHandler on_stderr,
                                const std::atomic_bool* cancel,
                                IdleHandler on_idle) {
    impl_->stop_requested.store(false, std::memory_order_release);
    impl_->outcome = Outcome::NotStarted;
    impl_->error.clear();

    if (args.empty()) {
        impl_->error = "no command";
        return std::nullopt;
    }

    // -- pipes -----------------------------------------------------------------
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength        = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    Handle out_read, out_write, err_read, err_write, in_null;
    {
        HANDLE r = nullptr, w = nullptr;
        if (!::CreatePipe(&r, &w, &inheritable, 0)) {
            impl_->error = "CreatePipe(stdout): " + last_error_text(::GetLastError());
            return std::nullopt;
        }
        out_read.reset(r);
        out_write.reset(w);
        if (!::CreatePipe(&r, &w, &inheritable, 0)) {
            impl_->error = "CreatePipe(stderr): " + last_error_text(::GetLastError());
            return std::nullopt;
        }
        err_read.reset(r);
        err_write.reset(w);
    }
    // Our read ends must NOT be inherited, or the child holds a handle to its
    // own output and EOF never arrives.
    ::SetHandleInformation(out_read.h, HANDLE_FLAG_INHERIT, 0);
    ::SetHandleInformation(err_read.h, HANDLE_FLAG_INHERIT, 0);

    // stdin from NUL: ffmpeg polls stdin for interactive keys; an invalid handle
    // there makes some builds log errors every frame.
    in_null.reset(::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &inheritable, OPEN_EXISTING, 0, nullptr));

    // -- restrict inheritance to exactly the three handles ----------------------
    //
    // With bInheritHandles=TRUE, every inheritable handle in this process is
    // copied into the child by default. A stray inheritable write-end from any
    // other code would keep our pipe open after ffmpeg exits. The attribute
    // list pins the set to these three.
    HANDLE inherit_list[3] = {in_null.h, out_write.h, err_write.h};
    SIZE_T attr_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!::InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) ||
        !::UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit_list,
                                     sizeof(inherit_list), nullptr, nullptr)) {
        impl_->error = "proc thread attributes: " + last_error_text(::GetLastError());
        return std::nullopt;
    }

    // -- launch ------------------------------------------------------------------
    std::wstring application = utf8_to_wide(args[0]);
    std::wstring cmdline     = utf8_to_wide(quote_windows_command_line(args));
    cmdline.push_back(L'\0');  // CreateProcessW may modify the buffer in place

    STARTUPINFOEXW si{};
    si.StartupInfo.cb         = sizeof(si);
    si.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput  = in_null.h;
    si.StartupInfo.hStdOutput = out_write.h;
    si.StartupInfo.hStdError  = err_write.h;
    si.lpAttributeList        = attrs;

    PROCESS_INFORMATION pi{};
    const BOOL created = ::CreateProcessW(
        application.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT |
            CREATE_SUSPENDED,
        nullptr, nullptr, &si.StartupInfo, &pi);
    const DWORD create_error = created ? 0 : ::GetLastError();
    ::DeleteProcThreadAttributeList(attrs);

    // Whatever happens next, the child's ends are the child's. Keeping copies
    // here is the classic way to never see EOF.
    out_write.reset();
    err_write.reset();
    in_null.reset();

    if (!created) {
        impl_->error = "CreateProcess: " + last_error_text(create_error);
        return std::nullopt;
    }

    Handle process(pi.hProcess);
    Handle main_thread(pi.hThread);

    // -- job object: the child dies with us --------------------------------------
    //
    // If this application is killed mid-encode (task manager, crash, power
    // user), the ffmpeg it spawned would otherwise carry on for hours as an
    // orphan, holding the temp file open. KILL_ON_JOB_CLOSE ties its lifetime
    // to this handle, which the kernel closes when we die.
    Handle job(::CreateJobObjectW(nullptr, nullptr));
    if (job.valid()) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        // Can fail if we are already inside a job that forbids nesting. Not
        // fatal -- the encode still runs, it just will not be auto-killed.
        ::AssignProcessToJobObject(job.h, process.h);
    }

    ::ResumeThread(main_thread.h);
    main_thread.reset();

    // -- read loop ---------------------------------------------------------------
    std::mutex         queue_mutex;
    std::vector<Chunk> queue;
    Handle             wake(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    std::atomic_bool   reader_failed{false};
    std::string        reader_failure;

    Reader out_reader(Stream::Out, out_read.h, queue_mutex, queue, wake.h, reader_failed, reader_failure);
    Reader err_reader(Stream::Err, err_read.h, queue_mutex, queue, wake.h, reader_failed, reader_failure);

    detail::LineSplitter out_lines(std::move(on_stdout));
    detail::LineSplitter err_lines(std::move(on_stderr));

    const auto drain = [&]() -> bool {
        std::vector<Chunk> local;
        {
            std::lock_guard lock(queue_mutex);
            local.swap(queue);
        }
        for (auto& c : local) {
            if (c.stream == Stream::Out) out_lines.feed(c.data);
            else                          err_lines.feed(c.data);
        }
        return !local.empty();
    };

    const auto should_stop = [&] {
        return impl_->stop_requested.load(std::memory_order_acquire) ||
               (cancel && cancel->load(std::memory_order_acquire));
    };

    constexpr DWORD kTickMs = 150;
    bool    terminated     = false;
    bool    process_exited = false;
    int     grace_ticks    = 0;
    Outcome outcome        = Outcome::Exited;

    const auto terminate_child = [&](Outcome why) {
        if (!terminated) {
            ::TerminateProcess(process.h, 1);
            terminated = true;
            outcome    = why;
        }
    };

    for (;;) {
        if (!terminated && should_stop()) {
            terminate_child(impl_->stop_requested.load(std::memory_order_acquire)
                                ? Outcome::Stopped
                                : Outcome::Cancelled);
        }
        if (!terminated && reader_failed.load(std::memory_order_acquire)) {
            // Never leave a child writing into a pipe nobody reads: it would
            // block on its next write and look exactly like a hang.
            {
                std::lock_guard lock(queue_mutex);
                impl_->error = reader_failure;
            }
            terminate_child(Outcome::Failed);
        }

        const HANDLE waits[2] = {wake.h, process.h};
        const DWORD  r = ::WaitForMultipleObjects(2, waits, FALSE, kTickMs);

        const bool had_data = drain();

        if (r == WAIT_OBJECT_0 + 1) process_exited = true;

        if (process_exited) {
            // The child is gone; its pipe ends closed with it, so the readers
            // are finishing. Give them a moment, then force the issue in case
            // something inherited the write end after all.
            if (out_reader.done() && err_reader.done()) break;
            if (!had_data && ++grace_ticks > static_cast<int>(5000 / kTickMs)) {
                out_reader.abort();
                err_reader.abort();
                break;
            }
            continue;
        }

        if (r == WAIT_TIMEOUT && !had_data && !terminated && on_idle) {
            on_idle();
        }
    }

    out_reader.join();
    err_reader.join();
    drain();
    out_lines.flush();
    err_lines.flush();

    // Exit code. After TerminateProcess the code is whatever we passed (1) and
    // is not meaningful, so it is not returned in that case.
    ::WaitForSingleObject(process.h, 5000);
    DWORD exit_code = 0;
    ::GetExitCodeProcess(process.h, &exit_code);

    impl_->outcome = outcome;
    if (outcome != Outcome::Exited) return std::nullopt;
    return static_cast<int>(exit_code);
}

// ---------------------------------------------------------------------------

CaptureResult capture(const std::vector<std::string>& args) {
    CaptureResult result;
    Process proc;

    const auto code = proc.run(
        args,
        [&](std::string_view line) { result.out.append(line).push_back('\n'); },
        [&](std::string_view line) { result.err.append(line).push_back('\n'); },
        nullptr);

    if (code) {
        result.started   = true;
        result.exit_code = *code;
    } else if (!proc.error().empty()) {
        result.err += proc.error();
    }
    return result;
}

}  // namespace conv
