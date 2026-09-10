#include "bridge.h"

#include <atomic>
#include <mutex>

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

// Completes a pickFolder query once the native dialog is dismissed.
//
// CEF's own dialog is used rather than IFileDialog / GTK / NSOpenPanel so that
// one code path covers Windows, Linux and macOS, each still getting its real
// native picker.
class FolderDialogCallback : public CefRunFileDialogCallback {
public:
    explicit FolderDialogCallback(CefRefPtr<CefMessageRouterBrowserSide::Callback> callback)
        : callback_(std::move(callback)) {}

    void OnFileDialogDismissed(const std::vector<CefString>& paths) override {
        json out;
        out["cancelled"] = paths.empty();
        out["path"]      = paths.empty() ? std::string{} : paths.front().ToString();
        callback_->Success(out.dump());
    }

private:
    CefRefPtr<CefMessageRouterBrowserSide::Callback> callback_;

    IMPLEMENT_REFCOUNTING(FolderDialogCallback);
    DISALLOW_COPY_AND_ASSIGN(FolderDialogCallback);
};

}  // namespace

struct Bridge::Impl {
    conv::ToolPaths                  tools;
    std::unique_ptr<conv::RunLog>    run_log;
    std::unique_ptr<conv::JobRunner> runner;

    // The page's persistent event subscription. UI thread only.
    CefRefPtr<Callback> events;
    int64_t             events_query_id = 0;

    // Pushes one event to the page. A persistent query stays open after
    // Success(), so this can be called as many times as there are events.
    //
    // Silently drops events when nothing is subscribed, which happens between
    // the browser being created and the Vue app mounting.
    void Emit(const std::string& payload) {
        if (events) events->Success(payload);
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
    impl_->events = nullptr;
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

    const std::string cmd = req.value("cmd", "");
    const json args = req.contains("args") ? req["args"] : json::object();

    // ---- the event channel -------------------------------------------------
    if (cmd == "subscribe") {
        if (!persistent) {
            callback->Failure(400, "subscribe must be a persistent query");
            return true;
        }
        impl_->events          = callback;
        impl_->events_query_id = query_id;
        return true;  // held open; no Success() until there is an event
    }

    // ---- one-shot commands -------------------------------------------------
    if (cmd == "getState") {
        json out;
        out["ffmpegFound"] = impl_->tools.valid();
        out["ffmpeg"]      = conv::path_to_utf8(impl_->tools.ffmpeg);
        out["running"]     = impl_->runner->running();
        out["logDir"]      = conv::path_to_utf8(user_data_dir() / "logs");
        callback->Success(out.dump());
        return true;
    }

    if (cmd == "pickFolder") {
        if (!browser || !browser->GetHost()) {
            callback->Failure(500, "no browser host");
            return true;
        }
        // Asynchronous: Success() is delivered from OnFileDialogDismissed.
        browser->GetHost()->RunFileDialog(FILE_DIALOG_OPEN_FOLDER,
                                          args.value("title", ""),
                                          args.value("initial", ""),
                                          std::vector<CefString>{},
                                          new FolderDialogCallback(callback));
        return true;
    }

    if (cmd == "parseSize") {
        const auto v = conv::parse_size_input(args.value("text", ""));
        json out;
        out["valid"] = v.has_value();
        out["bytes"] = v ? *v : 0;
        out["human"] = v ? conv::format_bytes(*v) : "";
        callback->Success(out.dump());
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
        s.input_dir       = conv::path_from_utf8(args.value("inputDir", ""));
        s.output_dir      = conv::path_from_utf8(args.value("outputDir", ""));
        s.output_mode     = output_mode_from(args.value("outputMode", "replace"));
        s.existing_policy = policy_from(args.value("existing", "overwrite"));
        s.speed           = speed_from(args.value("speed", "optimal"));
        s.size_mode       = size_mode_from(args.value("sizeMode", "manual"));
        s.level           = level_from(args.value("level", "medium"));

        if (s.size_mode == conv::SizeMode::Manual) {
            const auto bytes = conv::parse_size_input(args.value("maxSize", ""));
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
                                    weak, event.dump()));
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
                      {"cancelled", sum.cancelled}});
        };

        const bool started = impl_->runner->start(std::move(s), std::move(cb));
        json out;
        out["started"] = started;
        callback->Success(out.dump());
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
