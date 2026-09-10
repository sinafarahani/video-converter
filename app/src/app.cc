#include "app.h"

#if defined(_WIN32)
#  include <windows.h>
#  include "resource.h"
#endif

#include <algorithm>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "client.h"
#include "conv/util.hpp"
#include "include/base/cef_logging.h"
#include "include/cef_browser.h"
#include "include/cef_command_line.h"
#include "include/cef_preference.h"
#include "include/cef_request_context.h"
#include "include/cef_scheme.h"
#include "include/cef_values.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_helpers.h"
#include "launch_paths.h"

namespace converter {
namespace {

// ===========================================================================
//  Offline: why each entry exists
// ===========================================================================
//
// The app makes no network contact of its own. The UI comes from disk over
// app:// and ffmpeg is a separate process, so none of this costs a feature.
// Each entry names what it stops; the evidence (Chromium 151 source, and the
// Google contacts found in this app's own profile) is in the 2.1.0 research
// notes. The layers overlap on purpose: switches and features act from process
// start, preferences only once the profile has loaded, and the resolver rule
// catches whatever the lists miss.

// Plain switches, browser process only; Chromium passes on to its children the
// ones they need.
constexpr const char* kOfflineSwitches[] = {
    "disable-background-networking",  // Safe Browsing updates, translate, variations,
                                      // intranet-redirect probes, extension updates
    "disable-component-update",       // component updater: 21 update checks on first launch
    "no-first-run",                   // first-run UI and imports
    "no-default-browser-check",
    "disable-default-apps",
    "disable-component-extensions-with-background-pages",
    "disable-domain-reliability",     // Domain Reliability uploads
    "disable-sync",
    "no-pings",                       // hyperlink-auditing pings
    "no-proxy-server",                // WPAD/PAC auto-detection, which the OS resolver would
                                      // do outside Chromium; the app needs no proxy
};

// Every hostname lookup inside Chromium fails. The page never needs DNS -- app://
// is served by a CefSchemeHandlerFactory -- so this is the catch-all, e.g. for
// the GAIA cookie-jar request to accounts.google.com, which has no switch of its
// own. Caret, not tilde: Chromium 151 only knows ^NOTFOUND. The switch is copied
// to the network-service process. It cannot stop IP-literal connections or LAN
// multicast (mDNS/SSDP); the features below cover those.
constexpr char kHostResolverRules[] = "MAP * ^NOTFOUND";

// MERGED into --disable-features, never written over it: CEF and the real
// command line may already carry entries there, and appending the switch again
// replaces the old value outright (the docs/DEVELOPMENT.md gotcha).
constexpr const char* kDisabledFeatures[] = {
    "NetworkTimeServiceQuerying",  // http://clients2.google.com/time at startup; not gated
                                   // by disable-background-networking
    "AimEnabled",                  // AI Mode eligibility: www.google.com/async/folae
    "AimServerRequestOnStartupEnabled",
    "AimServerRequestOnIdentityChangeEnabled",
    "OptimizationHints",           // Optimization Guide hints and model downloads
    "OptimizationTargetPrediction",
    "OptimizationGuideModelExecution",
    "OptimizationGuideOnDeviceModel",
    "MediaRouter",                 // Cast/DIAL discovery: mDNS and SSDP multicast on the
    "DialMediaRouteProvider",      // LAN, and the macOS "accept incoming connections"
    "CastMediaRouteProvider",      // prompt
    "AutofillServerCommunication", // Autofill form-type queries
    "PreloadTopChromeWebUI",       // hidden Chrome WebUI contents behind the "Timeout of
    "WebUIOmniboxPopup",           // new browser info response" lines in cef.log
};

// A preference value small enough for a constexpr table.
struct Pref {
    enum class Kind { Off, Int, String, EmptyList };
    const char* name;
    Kind        kind;
    int         number;  // Kind::Int
    const char* text;    // Kind::String
};

constexpr Pref off(const char* name) { return {name, Pref::Kind::Off, 0, nullptr}; }
constexpr Pref int_pref(const char* name, int v) { return {name, Pref::Kind::Int, v, nullptr}; }
constexpr Pref str_pref(const char* name, const char* v) { return {name, Pref::Kind::String, 0, v}; }
constexpr Pref empty_list(const char* name) { return {name, Pref::Kind::EmptyList, 0, nullptr}; }

// Profile preferences (CefRequestContext::GetGlobalContext). That is the
// on-disk Default profile under root_cache_path, so the values persist; they
// are still set on every launch, so a 2.0-era profile, a CEF update or a pref
// reset cannot leave any of them on.
constexpr Pref kProfilePrefs[] = {
    off("browser.enable_spellchecking"),  // Hunspell dictionary download from
    empty_list("spellcheck.dictionaries"),  // redirector.gvt1.com (Linux mainly)
    off("spellcheck.use_spelling_service"),
    off("safebrowsing.enabled"),          // safebrowsing.googleapis.com lookups
    off("search.suggest_enabled"),        // omnibox zero-suggest prefetch
    off("alternate_error_pages.enabled"),
    off("translate.enabled"),             // translate.googleapis.com
    int_pref("net.network_prediction_options", 2),  // 2 = no DNS prefetch / preconnect
    off("url_keyed_anonymized_data_collection.enabled"),
    off("media_router.enable_media_router"),
    off("credentials_enable_service"),    // password manager and its leak check
    off("profile.password_manager_leak_detection"),
    off("autofill.profile_enabled"),
    off("autofill.credit_card_enabled"),
    off("signin.allowed"),
    off("privacy_sandbox.m1.topics_enabled"),
    off("privacy_sandbox.m1.fledge_enabled"),
    off("privacy_sandbox.m1.ad_measurement_enabled"),
};

// Local-state preferences (CefPreferenceManager::GetGlobalPreferenceManager).
// Persisted, so later launches start with them off.
constexpr Pref kLocalStatePrefs[] = {
    off("network_time.network_time_queries_enabled"),    // second layer behind the feature
    off("component_updates.component_updates_enabled"),  // second layer behind the switch
    str_pref("dns_over_https.mode", "off"),              // no automatic Secure DNS probes
};

CefRefPtr<CefValue> to_value(const Pref& pref) {
    CefRefPtr<CefValue> value = CefValue::Create();
    switch (pref.kind) {
        case Pref::Kind::Off:       value->SetBool(false); break;
        case Pref::Kind::Int:       value->SetInt(pref.number); break;
        case Pref::Kind::String:    value->SetString(pref.text); break;
        case Pref::Kind::EmptyList: value->SetList(CefListValue::Create()); break;
    }
    return value;
}

// A preference Chromium does not know or will not change is logged to cef.log
// and skipped: the switches and features still cover the same ground, and a
// CEF update renaming one must not stop the app from starting.
void apply_prefs(CefRefPtr<CefPreferenceManager> manager, std::span<const Pref> prefs,
                 const char* store) {
    if (!manager) {
        LOG(WARNING) << "offline: no " << store << " preference manager";
        return;
    }
    for (const Pref& pref : prefs) {
        if (!manager->CanSetPreference(pref.name)) {
            LOG(WARNING) << "offline: " << store << " preference cannot be set: " << pref.name;
            continue;
        }
        CefString error;
        if (!manager->SetPreference(pref.name, to_value(pref), error)) {
            LOG(WARNING) << "offline: setting " << store << " preference " << pref.name
                         << " failed: " << error.ToString();
        }
    }
}

// Feature names already in a --disable-features value. Entries may carry a
// field-trial suffix ("Name<Study") and stray spaces.
std::vector<std::string> feature_names(const std::string& list) {
    std::vector<std::string> names;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(',', start);
        if (end == std::string::npos) end = list.size();
        std::string name = list.substr(start, end - start);
        name = name.substr(0, name.find('<'));
        const auto first = name.find_first_not_of(" \t");
        const auto last  = name.find_last_not_of(" \t");
        if (first != std::string::npos) names.push_back(name.substr(first, last - first + 1));
        start = end + 1;
    }
    return names;
}

void merge_disabled_features(CefRefPtr<CefCommandLine> command_line) {
    constexpr char kSwitch[] = "disable-features";
    const bool present = command_line->HasSwitch(kSwitch);

    std::string merged = present ? command_line->GetSwitchValue(kSwitch).ToString() : std::string();
    std::vector<std::string> names = feature_names(merged);

    for (const char* feature : kDisabledFeatures) {
        if (std::find(names.begin(), names.end(), feature) != names.end()) continue;
        if (!merged.empty() && merged.back() != ',') merged += ',';
        merged += feature;
        names.emplace_back(feature);
    }

    if (present) command_line->RemoveSwitch(kSwitch);
    command_line->AppendSwitchWithValue(kSwitch, merged);
}

// Several processes launched together -- Explorer starts one per selected file
// -- reach the running window as separate relaunches within moments of each
// other. Deliveries this close together form one list.
constexpr std::chrono::milliseconds kLaunchBurst{1500};

// ===========================================================================
//  The window
// ===========================================================================

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

// The UI browser is Alloy style, inside the Chrome runtime.
//
// Chrome style cannot take a file drop: CefDragHandler::OnDragEnter is only
// ever called from the Alloy browser host (AlloyBrowserHostImpl::CanDragEnter);
// a Chrome-style browser's drag goes to Chromium's Browser, which never asks
// CEF. The page cannot learn a dropped file's path itself, so drag-and-drop
// needs Alloy. Alloy also brings no Chrome keyboard shortcuts, menus or tabs,
// and its context menu is CEF's own (filtered in ConverterClient). The process
// singleton and OnAlreadyRunningAppRelaunch belong to the runtime, not the
// style, and work the same.
class BrowserViewDelegate : public CefBrowserViewDelegate {
public:
    BrowserViewDelegate() = default;

    cef_runtime_style_t GetBrowserRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }

private:
    IMPLEMENT_REFCOUNTING(BrowserViewDelegate);
    DISALLOW_COPY_AND_ASSIGN(BrowserViewDelegate);
};

// Gives the window its title bar text and sensible bounds. The original was
// 80% of the screen with a 720x600 minimum, which is preserved here.
class WindowDelegate : public CefWindowDelegate {
public:
    // |on_window| gets the window once it exists and nullptr once it is
    // destroyed, so the app can bring it to the front on a relaunch.
    using WindowObserver = std::function<void(CefRefPtr<CefWindow>)>;

    WindowDelegate(CefRefPtr<CefBrowserView> browser_view, WindowObserver on_window)
        : browser_view_(std::move(browser_view)), on_window_(std::move(on_window)) {}

    // Alloy, like the browser view it hosts (see BrowserViewDelegate). A Chrome
    // window could host it too, but Alloy for both is the tested combination.
    cef_runtime_style_t GetWindowRuntimeStyle() override { return CEF_RUNTIME_STYLE_ALLOY; }

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

        if (on_window_) on_window_(window);
    }

    void OnWindowDestroyed(CefRefPtr<CefWindow>) override {
        browser_view_ = nullptr;
        if (on_window_) on_window_(nullptr);
    }

    CefSize GetMinimumSize(CefRefPtr<CefView>) override { return CefSize(720, 600); }

    bool CanClose(CefRefPtr<CefWindow>) override {
        CefRefPtr<CefBrowser> browser = browser_view_ ? browser_view_->GetBrowser() : nullptr;
        return browser ? browser->GetHost()->TryCloseBrowser() : true;
    }

private:
    CefRefPtr<CefBrowserView> browser_view_;
    WindowObserver            on_window_;

    IMPLEMENT_REFCOUNTING(WindowDelegate);
    DISALLOW_COPY_AND_ASSIGN(WindowDelegate);
};

}  // namespace

ConverterApp::ConverterApp() = default;
ConverterApp::~ConverterApp() = default;

void ConverterApp::OnBeforeCommandLineProcessing(const CefString& process_type,
                                                 CefRefPtr<CefCommandLine> command_line) {
    if (!process_type.empty()) return;  // browser process only

    // A renderer that gets throttled while the window is minimised would stall
    // the progress bar and log updates during a long encode.
    command_line->AppendSwitch("disable-renderer-backgrounding");

    // Offline: see the tables at the top of this file.
    for (const char* name : kOfflineSwitches) command_line->AppendSwitch(name);
    command_line->AppendSwitchWithValue("host-resolver-rules", kHostResolverRules);
    // Linux: keep Chromium away from the desktop keyring. With the default
    // password store it asks gnome-keyring or KWallet for a key at start-up,
    // which pops up "Choose password for new keyring" wherever no keyring is
    // unlocked. The app stores no passwords or cookies worth encrypting.
    command_line->AppendSwitchWithValue("password-store", "basic");
    merge_disabled_features(command_line);
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

    // Offline preferences, before any browser exists. OnRegisterCustomPreferences
    // cannot override built-in preferences, so they are set here instead.
    apply_prefs(CefRequestContext::GetGlobalContext(), kProfilePrefs, "profile");
    apply_prefs(CefPreferenceManager::GetGlobalPreferenceManager(), kLocalStatePrefs,
                "local state");

    CefRegisterSchemeHandlerFactory(kAppScheme, kAppHost, new AssetSchemeHandlerFactory());

    client_ = new ConverterClient();

    CefBrowserSettings browser_settings;
    // The page paints its own dark background; without this there is a white
    // flash on every launch.
    browser_settings.background_color = CefColorSetARGB(255, 15, 42, 68);

    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(
        client_, std::string(kAppOrigin) + "index.html", browser_settings, nullptr, nullptr,
        new BrowserViewDelegate());

    CefWindow::CreateTopLevelWindow(new WindowDelegate(
        browser_view, [this](CefRefPtr<CefWindow> window) { OnWindowChanged(window); }));

    // Paths on our own command line: Explorer "Open with", the Linux %F, a
    // terminal. Chrome does not open positional arguments itself on first
    // launch, so this is their only consumer. The bridge holds them until the
    // page subscribes.
    std::error_code ec;
    DeliverLaunchPaths(paths_from_command_line(CefCommandLine::GetGlobalCommandLine(),
                                               std::filesystem::current_path(ec)),
                       "cmdline");

    // Then whatever arrived before the client existed -- a macOS Apple Event, or
    // an early relaunch.
    for (QueuedPaths& queued : take_queued_launch_paths()) {
        DeliverLaunchPaths(std::move(queued.paths), queued.source);
    }
}

bool ConverterApp::OnAlreadyRunningAppRelaunch(CefRefPtr<CefCommandLine> command_line,
                                               const CefString& current_directory) {
    CEF_REQUIRE_UI_THREAD();

    // Everything is copied out now: |command_line| must not outlive this call.
    std::error_code ec;
    const std::filesystem::path cwd = current_directory.empty()
                                          ? std::filesystem::current_path(ec)
                                          : conv::path_from_utf8(current_directory.ToString());
    DeliverLaunchPaths(paths_from_command_line(command_line, cwd), "relaunch");
    BringWindowToFront();

    // ALWAYS handled. Returning false makes Chrome open a default-styled window
    // of its own in this process -- unmanaged, able to browse, with a New Tab
    // Page that talks to Google and a "Restore pages?" bubble.
    return true;
}

CefRefPtr<CefClient> ConverterApp::GetDefaultClient() {
    // Only used when Chrome UI opens a browser window by itself (a Chrome
    // menu, the macOS Dock menu), never for the app's own window. Such a
    // window gets a client that closes it at once and shares nothing with the
    // app: were it given client_, it would join the app's browser list, keep
    // an encode and the process alive after the app window closed, and could
    // load the UI a second time on the same bridge. A new one per call, so
    // nothing app-side holds it past CefShutdown; never nullptr, which would
    // make the window unmanaged and block shutdown.
    return new StrayBrowserClient();
}

void ConverterApp::DeliverLaunchPaths(std::vector<std::string> utf8_paths,
                                      const std::string& source) {
    CEF_REQUIRE_UI_THREAD();

    // A launch with no paths (the app started again, a Dock click) only raises
    // the window; delivering an empty list would clear the page's inputs.
    if (utf8_paths.empty()) return;

    if (!client_) {
        queue_launch_paths(std::move(utf8_paths), source);
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool append = last_launch_delivery_ && now - *last_launch_delivery_ < kLaunchBurst;
    last_launch_delivery_ = now;
    client_->DeliverInputs(std::move(utf8_paths), source, append);
}

void ConverterApp::BringWindowToFront() {
    CEF_REQUIRE_UI_THREAD();
    if (!window_) return;
    if (window_->IsMinimized()) window_->Restore();
    window_->Show();
    window_->Activate();
    window_->BringToTop();
}

void ConverterApp::OnWindowChanged(CefRefPtr<CefWindow> window) {
    CEF_REQUIRE_UI_THREAD();
    window_ = window;
    // The window is gone, so the browser is closing: drop the client too. This
    // object outlives CefShutdown, and nothing CEF-side may be released after it.
    if (!window_) client_ = nullptr;
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
