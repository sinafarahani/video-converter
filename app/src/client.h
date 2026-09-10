#pragma once

#include <list>

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
                        public CefLifeSpanHandler,
                        public CefRequestHandler,
                        public CefPermissionHandler,
                        public CefDisplayHandler {
public:
    ConverterClient();
    ~ConverterClient() override;

    // CefClient
    CefRefPtr<CefLifeSpanHandler>  GetLifeSpanHandler() override { return this; }
    CefRefPtr<CefRequestHandler>   GetRequestHandler() override { return this; }
    CefRefPtr<CefPermissionHandler> GetPermissionHandler() override { return this; }
    CefRefPtr<CefDisplayHandler>   GetDisplayHandler() override { return this; }

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
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
    bool DoClose(CefRefPtr<CefBrowser> browser) override;
    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

    // CefRequestHandler
    bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefRequest> request,
                        bool user_gesture,
                        bool is_redirect) override;
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

}  // namespace converter
