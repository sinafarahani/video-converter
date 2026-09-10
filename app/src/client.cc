#include "client.h"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

#include "app.h"
#include "bridge.h"
#include "conv/util.hpp"
#include "include/base/cef_callback.h"
#include "include/cef_id_mappers.h"
#include "include/cef_parser.h"
#include "include/cef_stream.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
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

// Chrome commands that open Chrome's own UI rather than acting on our page.
// A Chrome-style browser keeps Chrome's keyboard shortcuts (Ctrl+T, Ctrl+H,
// F1...), and each of these would open a window or tab we do not manage, most
// of them pages that talk to Google (New Tab Page, help, feedback, Lens,
// search). The app's browser is Alloy style, which has none of these commands,
// so this list is a guard should that ever change; the context menu is
// filtered separately (OnBeforeContextMenu), because Chrome runs its menu
// items without asking OnChromeCommand. A deny-list rather than an
// allow-list, so editing, zoom, reload and DevTools would keep working.
constexpr const char* kBlockedCommands[] = {
    "IDC_NEW_TAB",
    "IDC_NEW_WINDOW",
    "IDC_NEW_INCOGNITO_WINDOW",
    "IDC_RESTORE_TAB",
    "IDC_OPEN_FILE",
    "IDC_HELP_PAGE_VIA_KEYBOARD",
    "IDC_HELP_PAGE_VIA_MENU",
    "IDC_FEEDBACK",
    "IDC_SHOW_HISTORY",
    "IDC_SHOW_BOOKMARK_MANAGER",
    "IDC_SHOW_DOWNLOADS",
    "IDC_CLEAR_BROWSING_DATA",
    "IDC_OPTIONS",
    "IDC_ABOUT",
    "IDC_MANAGE_EXTENSIONS",
    "IDC_HOME",
    "IDC_TASK_MANAGER",
    "IDC_VIEW_SOURCE",
    "IDC_SAVE_PAGE",
    "IDC_OPEN_GLIC",
    "IDC_PRINT",
    "IDC_CONTENT_CONTEXT_SEARCHWEBFOR",
    "IDC_CONTENT_CONTEXT_SEARCHWEBFORNEWTAB",
    "IDC_CONTENT_CONTEXT_SEARCHWEBFORIMAGE",
    "IDC_CONTENT_CONTEXT_GOTOURL",
    "IDC_CONTENT_CONTEXT_TRANSLATE",
    "IDC_CONTENT_CONTEXT_LENS_REGION_SEARCH",
    "IDC_CONTENT_CONTEXT_LENS_OVERLAY",
    "IDC_CONTENT_CONTEXT_OPENLINKNEWTAB",
    "IDC_CONTENT_CONTEXT_OPENLINKNEWWINDOW",
    "IDC_CONTENT_CONTEXT_OPENLINKOFFTHERECORD",
    "IDC_CONTENT_CONTEXT_LANGUAGE_SETTINGS",
    "IDC_CONTENT_CONTEXT_SPELLING_TOGGLE",
    "IDC_SPELLCHECK_MENU",
    "IDC_CHECK_SPELLING_WHILE_TYPING",
};

// The numeric IDC values change between Chromium versions and the names do
// not, so the ids are looked up by name once, at first use. A name this libcef
// does not know maps to -1 and is dropped.
std::vector<int> command_ids(std::span<const char* const> names) {
    std::vector<int> ids;
    for (const char* name : names) {
        const int id = cef_id_for_command_id_name(name);
        if (id != -1) ids.push_back(id);
    }
    return ids;
}

const std::vector<int>& blocked_command_ids() {
    static const std::vector<int> ids = command_ids(kBlockedCommands);
    return ids;
}

// The context-menu items that are kept, in both id families: an Alloy-style
// browser's menu (the app's) uses CEF's MENU_ID_* values, a Chrome-style one
// Chrome's own IDC_CONTENT_CONTEXT_* ids.
constexpr const char* kEditCommandNames[] = {
    "IDC_CONTENT_CONTEXT_UNDO",
    "IDC_CONTENT_CONTEXT_REDO",
    "IDC_CONTENT_CONTEXT_CUT",
    "IDC_CONTENT_CONTEXT_COPY",
    "IDC_CONTENT_CONTEXT_PASTE",
    "IDC_CONTENT_CONTEXT_PASTE_AND_MATCH_STYLE",
    "IDC_CONTENT_CONTEXT_DELETE",
    "IDC_CONTENT_CONTEXT_SELECTALL",
};

bool is_edit_command(int command_id) {
    static const std::vector<int> ids = [] {
        std::vector<int> v = command_ids(kEditCommandNames);
        v.insert(v.end(), {MENU_ID_UNDO, MENU_ID_REDO, MENU_ID_CUT, MENU_ID_COPY, MENU_ID_PASTE,
                           MENU_ID_PASTE_MATCH_STYLE, MENU_ID_DELETE, MENU_ID_SELECT_ALL});
        return v;
    }();
    return std::find(ids.begin(), ids.end(), command_id) != ids.end();
}

// Removes every item but the edit commands, submenus (spell-check, writing
// direction) included, then the separators left at either end or next to each
// other. A menu left empty is simply not shown.
void keep_edit_items(CefRefPtr<CefMenuModel> model) {
    // Backwards, so a removal never shifts an item still to be visited.
    for (size_t i = model->GetCount(); i-- > 0;) {
        const cef_menu_item_type_t type = model->GetTypeAt(i);
        if (type == MENUITEMTYPE_SEPARATOR) continue;
        if (type == MENUITEMTYPE_SUBMENU || !is_edit_command(model->GetCommandIdAt(i))) {
            model->RemoveAt(i);
        }
    }
    for (size_t i = model->GetCount(); i-- > 1;) {
        if (model->GetTypeAt(i) == MENUITEMTYPE_SEPARATOR &&
            model->GetTypeAt(i - 1) == MENUITEMTYPE_SEPARATOR) {
            model->RemoveAt(i);
        }
    }
    while (model->GetCount() > 0 && model->GetTypeAt(0) == MENUITEMTYPE_SEPARATOR) {
        model->RemoveAt(0);
    }
    while (model->GetCount() > 0 &&
           model->GetTypeAt(model->GetCount() - 1) == MENUITEMTYPE_SEPARATOR) {
        model->RemoveAt(model->GetCount() - 1);
    }
}

// The window only ever shows our own page. DevTools is the one exception: its
// browser inherits this client, and its front end is served from devtools://
// out of CEF's own resources.
bool is_allowed_url(const std::string& url) {
    return url.starts_with(kAppOrigin) || url.starts_with("devtools://");
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

void ConverterClient::DeliverInputs(std::vector<std::string> utf8_paths,
                                    const std::string& source,
                                    bool append) {
    if (bridge_) bridge_->DeliverInputs(std::move(utf8_paths), source, append);
}

bool ConverterClient::OnChromeCommand(CefRefPtr<CefBrowser>,
                                      int command_id,
                                      cef_window_open_disposition_t) {
    CEF_REQUIRE_UI_THREAD();
    const std::vector<int>& blocked = blocked_command_ids();
    return std::find(blocked.begin(), blocked.end(), command_id) != blocked.end();
}

void ConverterClient::OnBeforeContextMenu(CefRefPtr<CefBrowser>,
                                          CefRefPtr<CefFrame>,
                                          CefRefPtr<CefContextMenuParams>,
                                          CefRefPtr<CefMenuModel> model) {
    CEF_REQUIRE_UI_THREAD();
    keep_edit_items(model);
}

bool ConverterClient::OnContextMenuCommand(CefRefPtr<CefBrowser>,
                                           CefRefPtr<CefFrame>,
                                           CefRefPtr<CefContextMenuParams>,
                                           int command_id,
                                           EventFlags) {
    CEF_REQUIRE_UI_THREAD();
    // Second line behind the filter: anything but an edit command is claimed
    // as handled, so its default action never runs.
    return !is_edit_command(command_id);
}

bool ConverterClient::OnDragEnter(CefRefPtr<CefBrowser>,
                                  CefRefPtr<CefDragData> dragData,
                                  DragOperationsMask) {
    CEF_REQUIRE_UI_THREAD();

    // Captured now, on enter, because this is the only point where the paths
    // are visible: the page's drop event gets File objects with names only.
    // Returning false lets the page still see dragover/drop and show its drop
    // zone; on drop it asks for these with takeDroppedPaths. CEF calls this
    // for Alloy-style browsers only -- the reason the app's browser is Alloy.
    if (dragData->IsFile()) {
        std::vector<CefString> paths;
        std::vector<std::string> utf8;
        if (dragData->GetFilePaths(paths)) {
            utf8.reserve(paths.size());
            for (const CefString& p : paths) utf8.push_back(p.ToString());
        }
        bridge_->SetDragPaths(std::move(utf8));
        return false;
    }

    // Anything else clears the set, so a stale one never answers a later drop.
    bridge_->SetDragPaths({});

    // A dropped link would navigate the window away from the app. OnBeforeBrowse
    // stops that too; refusing here also shows the user a no-drop cursor.
    return dragData->IsLink();
}

bool ConverterClient::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                               CefRefPtr<CefFrame> frame,
                                               CefProcessId source_process,
                                               CefRefPtr<CefProcessMessage> message) {
    CEF_REQUIRE_UI_THREAD();
    return router_->OnProcessMessageReceived(browser, frame, source_process, message);
}

bool ConverterClient::OnBeforePopup(CefRefPtr<CefBrowser>,
                                    CefRefPtr<CefFrame>,
                                    int,
                                    const CefString&,
                                    const CefString&,
                                    cef_window_open_disposition_t,
                                    bool,
                                    const CefPopupFeatures&,
                                    CefWindowInfo&,
                                    CefRefPtr<CefClient>&,
                                    CefBrowserSettings&,
                                    CefRefPtr<CefDictionaryValue>&,
                                    bool*) {
    CEF_REQUIRE_UI_THREAD();
    // The app is one window. A popup (window.open, target=_blank) would be a
    // second browser outside it, able to load anything.
    return true;
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

    // Anything but our own page -- a file dropped where the page did not catch
    // it, a dragged or clicked link, a redirect -- is cancelled: it would
    // replace the UI, and a web URL would be network access the app promises
    // never to make.
    if (!is_allowed_url(request->GetURL().ToString())) return true;

    // Only for navigations that go ahead (cef_message_router.h). Told about a
    // cancelled one, the router would drop the page's live event subscription.
    router_->OnBeforeBrowse(browser, frame);
    return false;
}

bool ConverterClient::OnOpenURLFromTab(CefRefPtr<CefBrowser>,
                                       CefRefPtr<CefFrame>,
                                       const CefString& target_url,
                                       cef_window_open_disposition_t target_disposition,
                                       bool) {
    CEF_REQUIRE_UI_THREAD();
    // A URL meant for a new tab or window (a middle click, Ctrl+click, a menu
    // item) would create a browser outside the app; cancelled before it exists.
    // One for this window still goes through OnBeforeBrowse.
    return target_disposition != CEF_WOD_CURRENT_TAB ||
           !is_allowed_url(target_url.ToString());
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

// ---------------------------------------------------------------------------

bool StrayBrowserClient::OnChromeCommand(CefRefPtr<CefBrowser>,
                                         int,
                                         cef_window_open_disposition_t) {
    return true;  // nothing runs in the moment before the window closes
}

void StrayBrowserClient::OnBeforeContextMenu(CefRefPtr<CefBrowser>,
                                             CefRefPtr<CefFrame>,
                                             CefRefPtr<CefContextMenuParams>,
                                             CefRefPtr<CefMenuModel> model) {
    model->Clear();
}

bool StrayBrowserClient::OnBeforePopup(CefRefPtr<CefBrowser>,
                                       CefRefPtr<CefFrame>,
                                       int,
                                       const CefString&,
                                       const CefString&,
                                       cef_window_open_disposition_t,
                                       bool,
                                       const CefPopupFeatures&,
                                       CefWindowInfo&,
                                       CefRefPtr<CefClient>&,
                                       CefBrowserSettings&,
                                       CefRefPtr<CefDictionaryValue>&,
                                       bool*) {
    return true;
}

void StrayBrowserClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
    CEF_REQUIRE_UI_THREAD();
    // Posted rather than closed from inside the creation callback, so Chrome
    // finishes setting the window up first. Forced: no unload handler may
    // keep it open.
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b) { b->GetHost()->CloseBrowser(true); },
                            browser));
}

bool StrayBrowserClient::OnBeforeBrowse(CefRefPtr<CefBrowser>,
                                        CefRefPtr<CefFrame>,
                                        CefRefPtr<CefRequest>,
                                        bool,
                                        bool) {
    // Every URL, app:// included: this window must never load the UI, whose
    // subscription would take the bridge's event channel from the real one.
    return true;
}

bool StrayBrowserClient::OnOpenURLFromTab(CefRefPtr<CefBrowser>,
                                          CefRefPtr<CefFrame>,
                                          const CefString&,
                                          cef_window_open_disposition_t,
                                          bool) {
    return true;
}

}  // namespace converter
