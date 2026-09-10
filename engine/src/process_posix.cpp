// Linux / macOS implementation of Process. See the class comment in ffmpeg.hpp.
//
// Written without a host to compile on. It sticks to the plain POSIX calls --
// pipe, fork, execvp, poll, waitpid, kill -- and nothing clever, precisely so
// the first real build has as little as possible to go wrong.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#  include <sys/prctl.h>
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "conv/ffmpeg.hpp"
#include "process_common.hpp"

namespace conv {
namespace {

void set_cloexec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

}  // namespace

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

// Only meaningful on Windows; provided so callers and tests link everywhere.
std::string Process::quote_windows_command_line(const std::vector<std::string>& args) {
    std::string out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) out.push_back(' ');
        out += args[i];
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

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (::pipe(out_pipe) != 0) {
        impl_->error = std::string("pipe(stdout): ") + std::strerror(errno);
        return std::nullopt;
    }
    if (::pipe(err_pipe) != 0) {
        impl_->error = std::string("pipe(stderr): ") + std::strerror(errno);
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return std::nullopt;
    }
    // The read ends are ours alone. The write ends are dup2'd onto 1 and 2 in
    // the child and the originals closed there, so they must not leak into
    // any other child either.
    set_cloexec(out_pipe[0]);
    set_cloexec(err_pipe[0]);
    set_cloexec(out_pipe[1]);
    set_cloexec(err_pipe[1]);

    // argv must be built before fork(): only async-signal-safe calls are
    // allowed between fork and exec in a multithreaded process, and std::string
    // allocation is not one of them.
    std::vector<std::string> storage(args);
    std::vector<char*>       argv;
    argv.reserve(storage.size() + 1);
    for (auto& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        impl_->error = std::string("fork: ") + std::strerror(errno);
        ::close(out_pipe[0]); ::close(out_pipe[1]);
        ::close(err_pipe[0]); ::close(err_pipe[1]);
        return std::nullopt;
    }

    if (pid == 0) {
        // ---- child -------------------------------------------------------
#if defined(__linux__)
        // Die with the parent. This is the Linux stand-in for the Windows job
        // object: if the app is killed mid-encode, ffmpeg goes with it instead
        // of running on for hours holding the temp file.
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        // The parent may already have died between fork and prctl.
        if (::getppid() == 1) ::_exit(127);
#endif
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::close(devnull);
        }
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        // CLOEXEC closes the originals and the read ends at exec.
        ::execvp(argv[0], argv.data());
        ::_exit(127);
    }

    // ---- parent ----------------------------------------------------------
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);

    detail::LineSplitter out_lines(std::move(on_stdout));
    detail::LineSplitter err_lines(std::move(on_stderr));

    const auto should_stop = [&] {
        return impl_->stop_requested.load(std::memory_order_acquire) ||
               (cancel && cancel->load(std::memory_order_acquire));
    };

    constexpr int kTickMs = 150;
    std::vector<char> buf(16 * 1024);

    int  fds[2]        = {out_pipe[0], err_pipe[0]};
    bool exited        = false;
    int  status        = 0;
    bool terminated    = false;
    Outcome outcome    = Outcome::Exited;
    int  grace_ticks   = 0;
    std::chrono::steady_clock::time_point term_at{};

    const auto reap = [&]() -> bool {
        if (exited) return true;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) exited = true;
        return exited;
    };

    const auto terminate_child = [&](Outcome why) {
        if (terminated) return;
        terminated = true;
        outcome    = why;
        term_at    = std::chrono::steady_clock::now();
        ::kill(pid, SIGTERM);
    };

    for (;;) {
        if (!terminated && should_stop()) {
            terminate_child(impl_->stop_requested.load(std::memory_order_acquire)
                                ? Outcome::Stopped
                                : Outcome::Cancelled);
        }
        if (terminated && !exited) {
            // SIGTERM lets ffmpeg close files; if it ignores that, insist.
            const auto waited = std::chrono::steady_clock::now() - term_at;
            if (waited > std::chrono::milliseconds(1500)) ::kill(pid, SIGKILL);
        }

        pollfd pfd[2];
        int    n = 0;
        for (int i = 0; i < 2; ++i) {
            if (fds[i] >= 0) {
                pfd[n].fd      = fds[i];
                pfd[n].events  = POLLIN;
                pfd[n].revents = 0;
                ++n;
            }
        }

        bool had_data = false;
        if (n > 0) {
            const int r = ::poll(pfd, static_cast<nfds_t>(n), kTickMs);
            if (r < 0 && errno != EINTR) {
                impl_->error = std::string("poll: ") + std::strerror(errno);
                terminate_child(Outcome::Failed);
            } else if (r > 0) {
                for (int i = 0; i < n; ++i) {
                    if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                    const ssize_t got = ::read(pfd[i].fd, buf.data(), buf.size());
                    if (got > 0) {
                        had_data = true;
                        const std::string_view chunk(buf.data(), static_cast<size_t>(got));
                        if (pfd[i].fd == fds[0]) out_lines.feed(chunk);
                        else                     err_lines.feed(chunk);
                    } else if (got == 0 || (got < 0 && errno != EINTR && errno != EAGAIN)) {
                        // EOF (or a genuine error): done with this stream.
                        const int which = (pfd[i].fd == fds[0]) ? 0 : 1;
                        ::close(fds[which]);
                        fds[which] = -1;
                    }
                }
            }
        } else {
            // Both streams closed; just wait for the exit status.
            std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
        }

        reap();

        if (fds[0] < 0 && fds[1] < 0 && exited) break;

        if (exited && n > 0) {
            // Child gone but a pipe is still open -- something inherited the
            // write end. Drain briefly, then stop waiting on it.
            if (!had_data && ++grace_ticks > (5000 / kTickMs)) {
                for (int& fd : fds) {
                    if (fd >= 0) { ::close(fd); fd = -1; }
                }
                break;
            }
        }

        if (!had_data && !exited && !terminated && on_idle) on_idle();
    }

    for (int& fd : fds) {
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
    out_lines.flush();
    err_lines.flush();

    if (!exited) {
        // Blocking wait is safe here: the child has been signalled (or has
        // closed both pipes, which for ffmpeg means it is exiting).
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        exited = true;
    }

    impl_->outcome = outcome;
    if (outcome != Outcome::Exited) return std::nullopt;

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
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
