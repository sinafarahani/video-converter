#include "app.h"

#if defined(_WIN32)
#  include <windows.h>
#  include "resource.h"
#endif

#include "client.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_scheme.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_helpers.h"

namespace converter {
namespace {

// Puts the application icon on the window.
//
// CefWindowDelegate has no icon hook in CEF 151, and the icon compiled into the
// executable only governs how Explorer draws the *file* -- the window CEF
// creates still shows Chromium's default. Setting it directly on the native
// handle fixes the title bar, the taskbar button and Alt-Tab.
void apply_window_icon(CefRefPtr<CefWindow> window) {
#if defined(_WIN32)
    const HWND hwnd = window->GetWindowHandle();
    if (!hwnd) return;

    const HINSTANCE instance = ::GetModuleHandleW(nullptr);

    // Two sizes: the small one is the title bar and Alt-Tab, the large one the
    // taskbar. Letting Windows scale a single size looks visibly rough.
    const auto load = [&](int cx, int cy) -> HICON {
        return static_cast<HICON>(::LoadImageW(instance, MAKEINTRESOURCEW(IDI_APPICON),
                                               IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR));
    };

    if (HICON small_icon = load(::GetSystemMetrics(SM_CXSMICON),
                                ::GetSystemMetrics(SM_CYSMICON))) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    }
    if (HICON big_icon = load(::GetSystemMetrics(SM_CXICON),
                              ::GetSystemMetrics(SM_CYICON))) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
    }
    // The icons are owned by the window for its lifetime and are released when
    // the process exits; nothing to free here.
#else
    (void)window;  // handled by the .desktop entry / bundle on other platforms
#endif
}

// Gives the window its title bar text and sensible bounds. The original was
// 80% of the screen with a 720x600 minimum, which is preserved here.
class WindowDelegate : public CefWindowDelegate {
public:
    explicit WindowDelegate(CefRefPtr<CefBrowserView> browser_view)
        : browser_view_(std::move(browser_view)) {}

    void OnWindowCreated(CefRefPtr<CefWindow> window) override {
        window->AddChildView(browser_view_);
        window->SetTitle("Video and Audio Compressor and Converter");

        const CefRect screen = window->GetDisplay()->GetWorkArea();
        const int w = static_cast<int>(screen.width * 0.8);
        const int h = static_cast<int>(screen.height * 0.8);
        window->SetBounds({screen.x + (screen.width - w) / 2,
                           screen.y + (screen.height - h) / 2, w, h});

        apply_window_icon(window);

        window->Show();
        browser_view_->RequestFocus();
    }

    void OnWindowDestroyed(CefRefPtr<CefWindow>) override { browser_view_ = nullptr; }

    CefSize GetMinimumSize(CefRefPtr<CefView>) override { return CefSize(720, 600); }

    bool CanClose(CefRefPtr<CefWindow>) override {
        CefRefPtr<CefBrowser> browser = browser_view_ ? browser_view_->GetBrowser() : nullptr;
        return browser ? browser->GetHost()->TryCloseBrowser() : true;
    }

private:
    CefRefPtr<CefBrowserView> browser_view_;

    IMPLEMENT_REFCOUNTING(WindowDelegate);
    DISALLOW_COPY_AND_ASSIGN(WindowDelegate);
};

}  // namespace

ConverterApp::ConverterApp() = default;

void ConverterApp::OnBeforeCommandLineProcessing(const CefString& process_type,
                                                 CefRefPtr<CefCommandLine> command_line) {
    if (!process_type.empty()) return;  // browser process only

    // A renderer that gets throttled while the window is minimised would stall
    // the progress bar and log updates during a long encode.
    command_line->AppendSwitch("disable-renderer-backgrounding");

    // None of these are meaningful for a local single-window tool, and each one
    // is a background network call we would rather not make.
    command_line->AppendSwitch("disable-background-networking");
    command_line->AppendSwitch("disable-component-update");
    command_line->AppendSwitch("no-first-run");
}

void ConverterApp::OnRegisterCustomSchemes(CefRawPtr<CefSchemeRegistrar> registrar) {
    // Must happen in EVERY process, which is why it lives on CefApp rather than
    // the browser process handler.
    //
    // STANDARD gives a real scheme://host origin (needed for module scripts and
    // history), SECURE makes it a secure context, CORS_ENABLED and
    // FETCH_ENABLED let the page fetch its own assets, and DISPLAY_ISOLATED
    // stops other schemes linking into it.
    registrar->AddCustomScheme(kAppScheme,
                               CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
                                   CEF_SCHEME_OPTION_CORS_ENABLED |
                                   CEF_SCHEME_OPTION_FETCH_ENABLED |
                                   CEF_SCHEME_OPTION_DISPLAY_ISOLATED);
}

void ConverterApp::OnContextInitialized() {
    CEF_REQUIRE_UI_THREAD();

    CefRegisterSchemeHandlerFactory(kAppScheme, kAppHost, new AssetSchemeHandlerFactory());

    CefRefPtr<ConverterClient> client(new ConverterClient());

    CefBrowserSettings browser_settings;
    // The page paints its own dark background; without this there is a white
    // flash on every launch.
    browser_settings.background_color = CefColorSetARGB(255, 15, 42, 68);

    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(
        client, std::string(kAppOrigin) + "index.html", browser_settings, nullptr, nullptr,
        nullptr);

    CefWindow::CreateTopLevelWindow(new WindowDelegate(browser_view));
}

void ConverterApp::OnContextCreated(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    CefRefPtr<CefV8Context> context) {
    if (!renderer_router_) {
        CefMessageRouterConfig config;  // defaults to window.cefQuery
        renderer_router_ = CefMessageRouterRendererSide::Create(config);
    }
    renderer_router_->OnContextCreated(browser, frame, context);
}

void ConverterApp::OnContextReleased(CefRefPtr<CefBrowser> browser,
                                     CefRefPtr<CefFrame> frame,
                                     CefRefPtr<CefV8Context> context) {
    if (renderer_router_) renderer_router_->OnContextReleased(browser, frame, context);
}

bool ConverterApp::OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                            CefRefPtr<CefFrame> frame,
                                            CefProcessId source_process,
                                            CefRefPtr<CefProcessMessage> message) {
    if (renderer_router_) {
        return renderer_router_->OnProcessMessageReceived(browser, frame, source_process, message);
    }
    return false;
}

}  // namespace converter
