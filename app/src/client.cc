#include "client.h"

#include <algorithm>
#include <string>

#include "app.h"
#include "bridge.h"
#include "conv/util.hpp"
#include "include/cef_parser.h"
#include "include/cef_stream.h"
#include "include/wrapper/cef_helpers.h"
#include "include/wrapper/cef_stream_resource_handler.h"
#include "platform.h"

namespace converter {
namespace {

std::string mime_for(const std::string& path) {
    const auto dot = path.rfind('.');
    const std::string ext = (dot == std::string::npos) ? "" : path.substr(dot + 1);

    if (ext == "html" || ext == "htm") return "text/html";
    if (ext == "js" || ext == "mjs")   return "text/javascript";
    if (ext == "css")                  return "text/css";
    if (ext == "json")                 return "application/json";
    if (ext == "svg")                  return "image/svg+xml";
    if (ext == "png")                  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "webp")                 return "image/webp";
    if (ext == "woff2")                return "font/woff2";
    if (ext == "woff")                 return "font/woff";
    if (ext == "ttf")                  return "font/ttf";
    if (ext == "ico")                  return "image/x-icon";
    return "application/octet-stream";
}

// Turns app://converter/assets/index.js into <exe>/ui/assets/index.js, refusing
// anything that tries to escape the ui directory.
std::optional<std::filesystem::path> resolve_asset(const std::string& url) {
    CefURLParts parts;
    if (!CefParseURL(url, parts)) return std::nullopt;

    std::string path = CefString(&parts.path).ToString();
    if (path.empty() || path == "/") path = "/index.html";

    // Strip a query string or fragment if one survived.
    const auto cut = path.find_first_of("?#");
    if (cut != std::string::npos) path = path.substr(0, cut);

    // Reject traversal before touching the filesystem.
    if (path.find("..") != std::string::npos) return std::nullopt;
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    if (path.empty()) return std::nullopt;

    const std::filesystem::path root = (resources_dir() / "ui").lexically_normal();
    const std::filesystem::path full = (root / conv::path_from_utf8(path)).lexically_normal();

    // Belt and braces: the resolved path must still be under the ui directory.
    const auto root_str = root.native();
    const auto full_str = full.native();
    if (full_str.size() < root_str.size() ||
        full_str.compare(0, root_str.size(), root_str) != 0) {
        return std::nullopt;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(full, ec)) return std::nullopt;
    return full;
}

}  // namespace

CefRefPtr<CefResourceHandler> AssetSchemeHandlerFactory::Create(CefRefPtr<CefBrowser>,
                                                               CefRefPtr<CefFrame>,
                                                               const CefString&,
                                                               CefRefPtr<CefRequest> request) {
    const auto file = resolve_asset(request->GetURL().ToString());
    if (!file) return nullptr;  // 404

    CefRefPtr<CefStreamReader> stream =
        CefStreamReader::CreateForFile(CefString(file->native()));
    if (!stream) return nullptr;

    return new CefStreamResourceHandler(mime_for(conv::path_to_utf8(*file)), stream);
}

// ---------------------------------------------------------------------------

ConverterClient::ConverterClient() {
    CefMessageRouterConfig config;  // window.cefQuery / window.cefQueryCancel
    router_ = CefMessageRouterBrowserSide::Create(config);

    bridge_ = Bridge::Create();
    router_->AddHandler(bridge_.get(), /*first=*/false);
}

ConverterClient::~ConverterClient() {
    if (router_ && bridge_) router_->RemoveHandler(bridge_.get());
}

bool ConverterClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                               CefRefPtr<CefFrame> frame,
                                               CefProcessId source_process,
                                               CefRefPtr<CefProcessMessage> message) {
    CEF_REQUIRE_UI_THREAD();
    return router_->OnProcessMessageReceived(browser, frame, source_process, message);
}

void ConverterClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    browsers_.push_back(browser);
}

bool ConverterClient::DoClose(CefRefPtr<CefBrowser>) {
    CEF_REQUIRE_UI_THREAD();
    return false;  // let CEF close the window normally
}

void ConverterClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();

    router_->OnBeforeClose(browser);

    browsers_.remove_if([&](const CefRefPtr<CefBrowser>& b) {
        return b->IsSame(browser);
    });

    if (browsers_.empty()) {
        // Stop any encode in flight before the message loop goes away, so we do
        // not leave an orphaned ffmpeg process behind.
        if (bridge_) bridge_->Shutdown();
        CefQuitMessageLoop();
    }
}

bool ConverterClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefFrame> frame,
                                     CefRefPtr<CefRequest> request,
                                     bool,
                                     bool) {
    CEF_REQUIRE_UI_THREAD();
    router_->OnBeforeBrowse(browser, frame);
    return false;
}

bool ConverterClient::OnRequestMediaAccessPermission(CefRefPtr<CefBrowser>,
                                                     CefRefPtr<CefFrame>,
                                                     const CefString&,
                                                     uint32_t,
                                                     CefRefPtr<CefMediaAccessCallback> callback) {
    // No camera, no microphone, no screen capture -- the UI is ten controls.
    callback->Cancel();
    return true;
}

bool ConverterClient::OnShowPermissionPrompt(CefRefPtr<CefBrowser>,
                                             uint64_t,
                                             const CefString&,
                                             uint32_t,
                                             CefRefPtr<CefPermissionPromptCallback> callback) {
    callback->Continue(CEF_PERMISSION_RESULT_DENY);
    return true;
}

void ConverterClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                                TerminationStatus,
                                                int,
                                                const CefString&) {
    CEF_REQUIRE_UI_THREAD();
    router_->OnRenderProcessTerminated(browser);
}

}  // namespace converter
