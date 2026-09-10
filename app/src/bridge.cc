#include "bridge.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "conv/gpu.hpp"
#include "conv/util.hpp"
#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "platform.h"

using json = nlohmann::json;

namespace converter {
namespace {

conv::OutputMode output_mode_from(const std::string& s) {
    return s == "copy" ? conv::OutputMode::CopyTo : conv::OutputMode::Replace;
}
conv::ExistingFilePolicy policy_from(const std::string& s) {
    return s == "skip" ? conv::ExistingFilePolicy::Skip : conv::ExistingFilePolicy::Overwrite;
}
conv::SpeedPreset speed_from(const std::string& s) {
    return s == "fast" ? conv::SpeedPreset::Fast : conv::SpeedPreset::Optimal;
}
conv::SizeMode size_mode_from(const std::string& s) {
    return s == "auto" ? conv::SizeMode::Automatic : conv::SizeMode::Manual;
}
conv::CompressionLevel level_from(const std::string& s) {
    if (s == "low") return conv::CompressionLevel::Low;
    if (s == "high") return conv::CompressionLevel::High;
    if (s == "extreme") return conv::CompressionLevel::Extreme;
    return conv::CompressionLevel::Medium;
}

// Reads one string argument. nlohmann's value() throws type_error when the key
// holds another type, and nothing catches an exception inside OnQuery, so every
// argument goes through here instead.
std::string str_arg(const json& obj, const char* key, const char* fallback = "") {
    const auto it = obj.find(key);  // end() when obj is not an object at all
    return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string(fallback);
}

// dump() throws on a string that is not valid UTF-8, which a Linux file name is
// allowed to be. Replacing the bad bytes beats losing the whole event.
std::string dump(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool is_media_file(const std::filesystem::path& p) {
    const std::string ext = conv::lower_extension(p);
    return conv::is_video_extension(ext) || conv::is_audio_extension(ext);
}

struct Classified {
    json paths    = json::array();
    json rejected = json::array();
};

// The one rule shared by drop, the file picker, the command line and a second
// launch: an existing folder, or an existing file with an audio/video
// extension, is kept. Anything else -- missing, not media, a device -- comes
// back as rejected, so the page can say what it ignored.
//
// Touches the disk, so never call it on the UI thread: use classify_async.
Classified classify(const std::vector<std::string>& utf8_paths) {
    Classified out;
    for (const std::string& s : utf8_paths) {
        if (s.empty()) continue;
        const std::filesystem::path p = conv::path_from_utf8(s);
        std::error_code ec;
        const auto st   = std::filesystem::status(p, ec);
        const bool keep = !ec && (std::filesystem::is_directory(st) ||
                                  (std::filesystem::is_regular_file(st) && is_media_file(p)));
        (keep ? out.paths : out.rejected).push_back(s);
    }
    return out;
}

// Runs classify() on a CEF file thread and |done| with the result back on the
// UI thread.
//
// One status() call on a sleeping network drive or an unreachable share can
// block for the SMB timeout. On the UI thread that would freeze the window,
// and during a relaunch also the launching process, which waits on the
// singleton hand-off until the running app returns. The file runner is a
// single thread and the UI thread runs its tasks in order, so results arrive
// in the order the lists were handed in.
void classify_async(std::vector<std::string> utf8_paths,
                    base::OnceCallback<void(Classified)> done) {
    CefPostTask(TID_FILE_USER_VISIBLE,
                base::BindOnce(
                    [](std::vector<std::string> paths, base::OnceCallback<void(Classified)> cb) {
                        CefPostTask(TID_UI, base::BindOnce(std::move(cb), classify(paths)));
                    },
                    std::move(utf8_paths), std::move(done)));
}

// {paths, rejected} -- the reply to takeDroppedPaths and, with "cancelled"
// added, to pickFiles.
json paths_reply(Classified c) {
    json out;
    out["paths"]    = std::move(c.paths);
    out["rejected"] = std::move(c.rejected);
    return out;
}

// The file picker deliberately gets no accept_filters, so it shows every
// file; the reply re-checks the selection and reports non-media files as
// rejected. CEF 151 offers no usable media filter: it drops the documented
// "description|.ext;.ext" form, and a list of plain extensions becomes one
// dialog entry per extension with the first pre-selected -- the picker would
// open showing .mp4 files only and hide everything else. (Checked on Windows by
// reading the dialog's file-type list, and through the Linux portal.)
std::vector<CefString> media_accept_filters() {
    return {};
}

// Completes a pickFolder or pickFiles query once the native dialog is
// dismissed.
//
// CEF's own dialog is used rather than IFileDialog / GTK / NSOpenPanel so that
// one code path covers Windows, Linux and macOS, each still getting its real
// native picker.
class PathsDialogCallback : public CefRunFileDialogCallback {
public:
    // |media_only|: the pickFiles reply, {cancelled, paths, rejected}, with the
    // selection re-checked. Otherwise the pickFolder reply, {cancelled, path,
    // paths}, passed through as chosen.
    PathsDialogCallback(CefRefPtr<CefMessageRouterBrowserSide::Callback> callback, bool media_only)
        : callback_(std::move(callback)), media_only_(media_only) {}

    void OnFileDialogDismissed(const std::vector<CefString>& paths) override {
        std::vector<std::string> utf8;
        utf8.reserve(paths.size());
        for (const CefString& p : paths) utf8.push_back(p.ToString());

        const bool cancelled = utf8.empty();
        if (media_only_) {
            // The filter only narrows what the dialog lists; a typed name or an
            // "all files" entry still gets through, so check again.
            classify_async(std::move(utf8),
                           base::BindOnce(
                               [](CefRefPtr<CefMessageRouterBrowserSide::Callback> cb,
                                  bool none, Classified c) {
                                   json out         = paths_reply(std::move(c));
                                   out["cancelled"] = none;
                                   cb->Success(dump(out));
                               },
                               callback_, cancelled));
            return;
        }

        json out;
        out["cancelled"] = cancelled;
        out["path"]      = cancelled ? std::string{} : utf8.front();
        out["paths"]     = utf8;
        callback_->Success(dump(out));
    }

private:
    CefRefPtr<CefMessageRouterBrowserSide::Callback> callback_;
    bool                                             media_only_;

    IMPLEMENT_REFCOUNTING(PathsDialogCallback);
    DISALLOW_COPY_AND_ASSIGN(PathsDialogCallback);
};

}  // namespace

struct Bridge::Impl {
    conv::ToolPaths                  tools;
    std::unique_ptr<conv::RunLog>    run_log;
    std::unique_ptr<conv::JobRunner> runner;

    // The page's persistent event subscription. UI thread only.
    CefRefPtr<Callback> events;
    int64_t             events_query_id = 0;

    // `inputs` events waiting for the page to subscribe, oldest first. UI
    // thread only.
    std::vector<std::string> pending_events;

    // Paths from the last OnDragEnter, waiting for the page's drop. UI thread
    // only.
    std::vector<std::string> drag_paths;

    // Set by Shutdown, so an `inputs` event still being checked on the file
    // thread is dropped instead of queued for a page that has gone. UI thread
    // only.
    bool shut_down = false;

    // Pushes one event to the page. A persistent query stays open after
    // Success(), so this can be called as many times as there are events.
    //
    // Silently drops events when nothing is subscribed, which happens between
    // the browser being created and the Vue app mounting. Engine events cannot
    // occur then; inputs go through EmitOrBuffer instead.
    void Emit(const std::string& payload) {
        if (events) events->Success(payload);
    }

    // Like Emit, but keeps the event until the page is listening. Also queues
    // while an earlier backlog is still waiting to be flushed, so events always
    // reach the page in the order they were raised.
    void EmitOrBuffer(std::string payload) {
        if (events && pending_events.empty()) {
            events->Success(payload);
        } else {
            pending_events.push_back(std::move(payload));
        }
    }

    void FlushPending() {
        if (!events) return;  // unsubscribed again before this ran; keep them
        std::vector<std::string> queued;
        queued.swap(pending_events);
        for (const std::string& payload : queued) events->Success(payload);
    }

    Impl()
        // Next to the executable. On macOS that is Contents/MacOS, the only place
        // code signing accepts executables -- ffmpeg in Contents/Resources makes
        // Gatekeeper report the whole app as damaged.
        : tools(conv::locate_tools(executable_dir())),
          run_log(std::make_unique<conv::RunLog>(user_data_dir() / "logs")),
          runner(std::make_unique<conv::JobRunner>(tools, *run_log, user_data_dir() / "cache")) {}
};

// ---------------------------------------------------------------------------

Bridge::Bridge() : impl_(std::make_unique<Impl>()) {}

Bridge::~Bridge() = default;

std::shared_ptr<Bridge> Bridge::Create() {
    // enable_shared_from_this needs the object to be owned by a shared_ptr, and
    // the constructor is private, so construction goes through here.
    return std::shared_ptr<Bridge>(new Bridge());
}

void Bridge::Shutdown() {
    if (impl_->runner) {
        impl_->runner->cancel();
        impl_->runner->join();
    }
    impl_->shut_down = true;
    impl_->events    = nullptr;
    impl_->pending_events.clear();
    impl_->drag_paths.clear();
}

void Bridge::SetDragPaths(std::vector<std::string> utf8_paths) {
    if (!CefCurrentlyOn(TID_UI)) {
        std::weak_ptr<Bridge> weak = shared_from_this();
        CefPostTask(TID_UI, base::BindOnce(
                                [](std::weak_ptr<Bridge> w, std::vector<std::string> paths) {
                                    if (auto self = w.lock()) self->SetDragPaths(std::move(paths));
                                },
                                weak, std::move(utf8_paths)));
        return;
    }
    impl_->drag_paths = std::move(utf8_paths);
}

void Bridge::DeliverInputs(std::vector<std::string> utf8_paths,
                           const std::string& source,
                           bool append) {
    if (!CefCurrentlyOn(TID_UI)) {
        std::weak_ptr<Bridge> weak = shared_from_this();
        CefPostTask(TID_UI, base::BindOnce(
                                [](std::weak_ptr<Bridge> w, std::vector<std::string> paths,
                                   std::string src, bool add) {
                                    if (auto self = w.lock()) {
                                        self->DeliverInputs(std::move(paths), src, add);
                                    }
                                },
                                weak, std::move(utf8_paths), source, append));
        return;
    }

    // Checked off the UI thread, so a relaunch hand-off returns at once. Every
    // delivery takes this same route, in order, so a burst's replace-then-
    // append sequence survives; |append| was decided on arrival and travels
    // with the paths.
    std::weak_ptr<Bridge> weak = shared_from_this();
    classify_async(std::move(utf8_paths),
                   base::BindOnce(
                       [](std::weak_ptr<Bridge> w, std::string src, bool add, Classified c) {
                           auto self = w.lock();
                           if (!self || self->impl_->shut_down) return;
                           const json event{{"type", "inputs"},
                                            {"paths", std::move(c.paths)},
                                            {"rejected", std::move(c.rejected)},
                                            {"source", src},
                                            {"append", add}};
                           self->impl_->EmitOrBuffer(dump(event));
                       },
                       weak, source, append));
}

bool Bridge::OnQuery(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefFrame> frame,
                     int64_t query_id,
                     const CefString& request,
                     bool persistent,
                     CefRefPtr<Callback> callback) {
    CEF_REQUIRE_UI_THREAD();

    json req;
    try {
        req = json::parse(request.ToString());
    } catch (const std::exception& e) {
        callback->Failure(400, std::string("bad request: ") + e.what());
        return true;
    }
    if (!req.is_object()) {
        callback->Failure(400, "bad request: not an object");
        return true;
    }

    const std::string cmd = str_arg(req, "cmd");
    const auto args_it    = req.find("args");
    const json args = (args_it != req.end() && args_it->is_object()) ? *args_it : json::object();

    // ---- the event channel -------------------------------------------------
    if (cmd == "subscribe") {
        if (!persistent) {
            callback->Failure(400, "subscribe must be a persistent query");
            return true;
        }
        impl_->events          = callback;
        impl_->events_query_id = query_id;

        // Hand over whatever arrived before the page was listening. Posted
        // rather than sent from here, so Success() is never called on this
        // query from inside its own OnQuery.
        if (!impl_->pending_events.empty()) {
            std::weak_ptr<Bridge> weak = shared_from_this();
            CefPostTask(TID_UI, base::BindOnce(
                                    [](std::weak_ptr<Bridge> w) {
                                        if (auto self = w.lock()) self->impl_->FlushPending();
                                    },
                                    weak));
        }
        return true;  // held open; no Success() until there is an event
    }

    // ---- one-shot commands -------------------------------------------------
    if (cmd == "getState") {
        json out;
        out["ffmpegFound"] = impl_->tools.valid();
        out["ffmpeg"]      = conv::path_to_utf8(impl_->tools.ffmpeg);
        out["running"]     = impl_->runner->running();
        out["logDir"]      = conv::path_to_utf8(user_data_dir() / "logs");
        callback->Success(dump(out));
        return true;
    }

    if (cmd == "pickFolder" || cmd == "pickFiles") {
        if (!browser || !browser->GetHost()) {
            callback->Failure(500, "no browser host");
            return true;
        }
        // There is no dialog mode that takes files and folders together, hence
        // two commands. Asynchronous: Success() is delivered from
        // OnFileDialogDismissed.
        const bool files = (cmd == "pickFiles");
        browser->GetHost()->RunFileDialog(
            files ? FILE_DIALOG_OPEN_MULTIPLE : FILE_DIALOG_OPEN_FOLDER,
            str_arg(args, "title"),
            str_arg(args, "initial"),
            files ? media_accept_filters() : std::vector<CefString>{},
            new PathsDialogCallback(callback, /*media_only=*/files));
        return true;
    }

    if (cmd == "takeDroppedPaths") {
        // Taken, not peeked: a later drop that somehow skipped OnDragEnter must
        // never be answered with this set.
        std::vector<std::string> dropped;
        dropped.swap(impl_->drag_paths);

        // Answered once the paths are checked, off the UI thread.
        classify_async(std::move(dropped),
                       base::BindOnce(
                           [](CefRefPtr<Callback> cb, Classified c) {
                               cb->Success(dump(paths_reply(std::move(c))));
                           },
                           callback));
        return true;
    }

    if (cmd == "parseSize") {
        const auto v = conv::parse_size_input(str_arg(args, "text"));
        json out;
        out["valid"] = v.has_value();
        out["bytes"] = v ? *v : 0;
        out["human"] = v ? conv::format_bytes(*v) : "";
        callback->Success(dump(out));
        return true;
    }

    if (cmd == "cancel") {
        impl_->runner->cancel();
        callback->Success("{}");
        return true;
    }

    if (cmd == "start") {
        if (impl_->runner->running()) {
            callback->Failure(409, "already running");
            return true;
        }

        conv::Settings s;
        // Folders and/or files, in the order the page lists them. `inputDir` is
        // the single-folder form an older page sends. What is missing or not
        // media is the engine's to report, per path, in the log.
        if (const auto it = args.find("inputs"); it != args.end() && it->is_array()) {
            for (const json& v : *it) {
                if (v.is_string()) s.inputs.push_back(conv::path_from_utf8(v.get<std::string>()));
            }
        } else if (const std::string dir = str_arg(args, "inputDir"); !dir.empty()) {
            s.inputs.push_back(conv::path_from_utf8(dir));
        }
        s.output_dir      = conv::path_from_utf8(str_arg(args, "outputDir"));
        s.output_mode     = output_mode_from(str_arg(args, "outputMode", "replace"));
        s.existing_policy = policy_from(str_arg(args, "existing", "overwrite"));
        s.speed           = speed_from(str_arg(args, "speed", "optimal"));
        s.size_mode       = size_mode_from(str_arg(args, "sizeMode", "manual"));
        s.level           = level_from(str_arg(args, "level", "medium"));

        if (s.size_mode == conv::SizeMode::Manual) {
            const auto bytes = conv::parse_size_input(str_arg(args, "maxSize"));
            s.max_size_bytes = bytes.value_or(0);
        }

        // Engine callbacks land on the worker thread. Hop to the UI thread
        // before touching the CEF Callback, and hold only a weak reference so
        // a task queued as the window closes cannot touch a dead Bridge.
        std::weak_ptr<Bridge> weak = shared_from_this();

        const auto post = [weak](json event) {
            CefPostTask(TID_UI, base::BindOnce(
                                    [](std::weak_ptr<Bridge> w, std::string payload) {
                                        if (auto self = w.lock()) self->impl_->Emit(payload);
                                    },
                                    weak, dump(event)));
        };

        conv::JobCallbacks cb;
        cb.log = [post](std::string_view text) {
            post(json{{"type", "log"}, {"text", std::string(text)}});
        };
        cb.progress = [post](const conv::Progress& p) {
            post(json{{"type", "progress"},
                      {"fraction", p.fraction},
                      {"fileFraction", p.file_fraction},
                      {"index", p.file_index},
                      {"count", p.file_count},
                      {"file", p.current_file},
                      {"phase", p.phase},
                      {"pass", p.pass},
                      {"passCount", p.pass_count},
                      {"fps", p.fps},
                      {"speed", p.speed},
                      {"outTime", p.out_time},
                      {"duration", p.duration},
                      {"elapsed", p.elapsed_seconds},
                      {"eta", p.eta_seconds}});
        };
        cb.finished = [post](const conv::RunSummary& sum) {
            post(json{{"type", "finished"},
                      {"converted", sum.converted},
                      {"remuxed", sum.remuxed},
                      {"skipped", sum.skipped_exists},
                      {"failed", sum.failed},
                      {"notMedia", sum.skipped_not_media},
                      {"missing", sum.missing},
                      {"cancelled", sum.cancelled}});
        };

        const bool started = impl_->runner->start(std::move(s), std::move(cb));
        json out;
        out["started"] = started;
        callback->Success(dump(out));
        return true;
    }

    callback->Failure(404, "unknown command: " + cmd);
    return true;
}

void Bridge::OnQueryCanceled(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int64_t query_id) {
    CEF_REQUIRE_UI_THREAD();
    if (query_id == impl_->events_query_id) {
        impl_->events          = nullptr;
        impl_->events_query_id = 0;
    }
}

}  // namespace converter
