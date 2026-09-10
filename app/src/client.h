#pragma once

#include <list>
#include <memory>
#include <string>
#include <vector>

#include "include/cef_client.h"
#include "include/cef_scheme.h"
#include "include/wrapper/cef_message_router.h"

namespace converter {

class Bridge;

// Serves the Vue production build from <exe dir>/ui over the app:// scheme.
//
// Assets are read from disk rather than embedded so that a UI fix can be
// shipped without relinking, and so `npm run dev` output can be dropped in
// during development.
class AssetSchemeHandlerFactory : public CefSchemeHandlerFactory {
public:
    // Explicit, because DISALLOW_COPY_AND_ASSIGN below declares a deleted copy
    // constructor, and any user-declared constructor suppresses the implicit
    // default one.
    AssetSchemeHandlerFactory() = default;

    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser> browser,
                                         CefRefPtr<CefFrame> frame,
                                         const CefString& scheme_name,
                                         CefRefPtr<CefRequest> request) override;

private:
    IMPLEMENT_REFCOUNTING(AssetSchemeHandlerFactory);
    DISALLOW_COPY_AND_ASSIGN(AssetSchemeHandlerFactory);
};

class ConverterClient : public CefClient,
                        public CefCommandHandler,
                        public CefContextMenuHandler,
                        public CefDragHandler,
                        public CefLifeSpanHandler,
                        public CefRequestHandler,
                        public CefPermissionHandler,
                        public CefDisplayHandler {
public:
    ConverterClient();
    ~ConverterClient() override;

    // Hands paths from outside the page -- the command line, a second launch,
    // macOS "Open with" -- to the page as its input list. Meant for the UI
    // thread; from any other thread the bridge re-posts it there.
    //
    // |utf8_paths| are absolute; |source| is "cmdline" | "relaunch" |
    // "openWith" | "drop". |append| adds to the current input list instead of
    // replacing it. Returns at once: the paths are checked on a file thread.
    void DeliverInputs(std::vector<std::string> utf8_paths, const std::string& source, bool append);

    // CefClient
    CefRefPtr<CefCommandHandler>   GetCommandHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefDragHandler>      GetDragHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler>  GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefRequestHandler>   GetRequestHandler() override { return this; }
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
    CefRefPtr<CefDisplayHandler>   GetDisplayHandler() override { return this; }

    // CefCommandHandler -- suppresses the Chrome commands that would open
    // Chrome's own UI (New Tab Page, history, settings, online help...). Chrome
    // style only, so a guard for now: the app's browser is Alloy style (see
    // app.cc), which has no Chrome commands. See client.cc for the list.
    bool OnChromeCommand(CefRefPtr<CefBrowser> browser,
                         int command_id,
                         cef_window_open_disposition_t disposition) override;

    // CefContextMenuHandler -- the right-click menu keeps its edit items (undo,
    // cut, copy, paste, select all...) and nothing else: no Back/Forward,
    // Print or View Source on the page, no search, translate or spell-check
    // entries that would open a window or reach the network.
    void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                             CefRefPtr<CefFrame> frame,
                             CefRefPtr<CefContextMenuParams> params,
                             CefRefPtr<CefMenuModel> model) override;
    bool OnContextMenuCommand(CefRefPtr<CefBrowser> browser,
                              CefRefPtr<CefFrame> frame,
                              CefRefPtr<CefContextMenuParams> params,
                              int command_id,
                              EventFlags event_flags) override;

    // CefDragHandler -- captures the paths of files dragged onto the window,
    // which the page cannot see, and refuses dragged links. Only ever called
    // for an Alloy-style browser.
    bool OnDragEnter(CefRefPtr<CefBrowser> browser,
                     CefRefPtr<CefDragData> dragData,
                     DragOperationsMask mask) override;

    // CefPermissionHandler -- this app needs no web permissions at all, so
    // every request is denied outright rather than being allowed to surface a
    // prompt. Without this, Chromium probing the Windows location service makes
    // the OS pop up a "Location has been turned off" dialog over our window on
    // first launch.
    bool OnRequestMediaAccessPermission(CefRefPtr<CefBrowser> browser,
                                        CefRefPtr<CefFrame> frame,
                                        const CefString& requesting_origin,
                                        uint32_t requested_permissions,
                                        CefRefPtr<CefMediaAccessCallback> callback) override;

    bool OnShowPermissionPrompt(CefRefPtr<CefBrowser> browser,
                                uint64_t prompt_id,
                                const CefString& requesting_origin,
                                uint32_t requested_permissions,
                                CefRefPtr<CefPermissionPromptCallback> callback) override;

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override;

    // CefLifeSpanHandler
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       int popup_id,
                       const CefString& target_url,
                       const CefString& target_frame_name,
                       cef_window_open_disposition_t target_disposition,
                       bool user_gesture,
                       const CefPopupFeatures& popupFeatures,
                       CefWindowInfo& windowInfo,
                       CefRefPtr<CefClient>& client,
                       CefBrowserSettings& settings,
                       CefRefPtr<CefDictionaryValue>& extra_info,
                       bool* no_javascript_access) override;
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    bool DoClose(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    // CefRequestHandler
    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefRequest> request,
                        bool user_gesture,
                        bool is_redirect) override;
    bool OnOpenURLFromTab(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          const CefString& target_url,
                          cef_window_open_disposition_t target_disposition,
                          bool user_gesture) override;
    void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                   TerminationStatus status,
                                   int error_code,
                                   const CefString& error_string) override;

private:
    CefRefPtr<CefMessageRouterBrowserSide> router_;
    std::shared_ptr<Bridge>                bridge_;
    std::list<CefRefPtr<CefBrowser>>       browsers_;

    IMPLEMENT_REFCOUNTING(ConverterClient);
    DISALLOW_COPY_AND_ASSIGN(ConverterClient);
};

// The client for a browser window that Chrome UI opens by itself
// (ConverterApp::GetDefaultClient) -- never the app's own window. It closes
// the window as soon as it exists and lets it do nothing meanwhile: no
// navigation, no popups, no commands, no menu. It holds no state and shares
// nothing with ConverterClient, so such a window can never keep the app alive,
// hold up its shutdown, or reach the bridge.
class StrayBrowserClient : public CefClient,
                           public CefCommandHandler,
                           public CefContextMenuHandler,
                           public CefLifeSpanHandler,
                           public CefRequestHandler {
public:
    StrayBrowserClient() = default;

    // CefClient
    CefRefPtr<CefCommandHandler>     GetCommandHandler() override { return this; }
    CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override { return this; }
    CefRefPtr<CefLifeSpanHandler>    GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefRequestHandler>     GetRequestHandler() override { return this; }

    // CefCommandHandler
    bool OnChromeCommand(CefRefPtr<CefBrowser> browser,
                         int command_id,
                         cef_window_open_disposition_t disposition) override;

    // CefContextMenuHandler
    void OnBeforeContextMenu(CefRefPtr<CefBrowser> browser,
                             CefRefPtr<CefFrame> frame,
                             CefRefPtr<CefContextMenuParams> params,
                             CefRefPtr<CefMenuModel> model) override;

    // CefLifeSpanHandler
    bool OnBeforePopup(CefRefPtr<CefBrowser> browser,
                       CefRefPtr<CefFrame> frame,
                       int popup_id,
                       const CefString& target_url,
                       const CefString& target_frame_name,
                       cef_window_open_disposition_t target_disposition,
                       bool user_gesture,
                       const CefPopupFeatures& popupFeatures,
                       CefWindowInfo& windowInfo,
                       CefRefPtr<CefClient>& client,
                       CefBrowserSettings& settings,
                       CefRefPtr<CefDictionaryValue>& extra_info,
                       bool* no_javascript_access) override;
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;

    // CefRequestHandler
    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefRequest> request,
                        bool user_gesture,
                        bool is_redirect) override;
    bool OnOpenURLFromTab(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          const CefString& target_url,
                          cef_window_open_disposition_t target_disposition,
                          bool user_gesture) override;

private:
    IMPLEMENT_REFCOUNTING(StrayBrowserClient);
    DISALLOW_COPY_AND_ASSIGN(StrayBrowserClient);
};

}  // namespace converter
