#pragma once

#include "include/cef_app.h"
#include "include/wrapper/cef_message_router.h"

namespace converter {

// The custom scheme the Vue bundle is served from.
//
// A real scheme with a real origin, rather than file://, because Vue's ES
// modules, fetch() and anything needing a secure context are all blocked or
// crippled under file://.
inline constexpr char kAppScheme[] = "app";
inline constexpr char kAppHost[]   = "converter";
inline constexpr char kAppOrigin[] = "app://converter/";

// One CefApp serving every process type. With no separate subprocess
// executable configured, CEF re-launches this same binary for the renderer,
// GPU and utility processes, so the browser-side and renderer-side handlers
// both live here.
class ConverterApp : public CefApp,
                     public CefBrowserProcessHandler,
                     public CefRenderProcessHandler {
public:
    ConverterApp();

    // CefApp
    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> command_line) override;
    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler>  GetRenderProcessHandler() override { return this; }

    // CefBrowserProcessHandler
    void OnContextInitialized() override;

    // CefRenderProcessHandler -- these three forward to the message router, and
    // the JS bridge silently stops working if any of them is missed.
    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override;
    void OnContextReleased(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context> context) override;
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override;

private:
    CefRefPtr<CefMessageRouterRendererSide> renderer_router_;

    IMPLEMENT_REFCOUNTING(ConverterApp);
    DISALLOW_COPY_AND_ASSIGN(ConverterApp);
};

}  // namespace converter
