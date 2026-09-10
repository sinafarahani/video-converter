#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "include/cef_app.h"
#include "include/views/cef_window.h"
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

class ConverterClient;

// One CefApp serving every process type. With no separate subprocess
// executable configured, CEF re-launches this same binary for the renderer,
// GPU and utility processes, so the browser-side and renderer-side handlers
// both live here.
class ConverterApp : public CefApp,
                     public CefBrowserProcessHandler,
                     public CefRenderProcessHandler {
public:
    ConverterApp();
    // Out of line: ConverterClient is only forward-declared here.
    ~ConverterApp() override;

    // CefApp
    void OnBeforeCommandLineProcessing(const CefString& process_type,
                                       CefRefPtr<CefCommandLine> command_line) override;
    void OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) override;

    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override { return this; }
    CefRefPtr<CefRenderProcessHandler>  GetRenderProcessHandler() override { return this; }

    // CefBrowserProcessHandler
    void OnContextInitialized() override;
    // A second launch with the same root_cache_path lands here, in the running
    // process, carrying the new process's arguments. Always returns true.
    bool OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line,
                                     const CefString& current_directory) override;
    CefRefPtr<CefClient> GetDefaultClient() override;

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

    // Paths the operating system asks us to open -- "cmdline", "relaunch" or
    // "openWith" -- go to the page, or wait in the launch queue until the
    // client exists. A delivery that follows the previous one within 1.5 s is
    // appended to it rather than replacing it. Browser process UI thread only
    // (on macOS, the main thread).
    void DeliverLaunchPaths(std::vector<std::string> utf8_paths, const std::string& source);

    // Restores, shows and activates the window, if it exists yet.
    void BringWindowToFront();

private:
    // Told about the window when it is created, and given nullptr when it is
    // destroyed.
    void OnWindowChanged(CefRefPtr<CefWindow> window);

    CefRefPtr<CefMessageRouterRendererSide> renderer_router_;

    // Browser process only. Both are set while the window exists.
    CefRefPtr<ConverterClient> client_;
    CefRefPtr<CefWindow>       window_;
    std::optional<std::chrono::steady_clock::time_point> last_launch_delivery_;

    IMPLEMENT_REFCOUNTING(ConverterApp);
    DISALLOW_COPY_AND_ASSIGN(ConverterApp);
};

}  // namespace converter
