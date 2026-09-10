// Windows entry point.
//
// Built with USE_SANDBOX=OFF, which produces a normal converter.exe rather than
// CEF's bootstrap.exe plus a client DLL. That is the right trade here: the app
// only ever renders its own local assets over the app:// scheme and never loads
// remote content, so the renderer sandbox protects against very little, while a
// plain executable is far easier to brand, version and code-sign for a per-user
// installer. Turn USE_SANDBOX back on in CMake if that ever changes.

#include <windows.h>

#include "app.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "platform.h"

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPTSTR, int) {
    CefMainArgs main_args(hInstance);
    CefRefPtr<converter::ConverterApp> app(new converter::ConverterApp());

    // With no browser_subprocess_path set, CEF relaunches this same executable
    // for the renderer, GPU and utility processes. For those, CefExecuteProcess
    // runs the child's message loop and returns its exit code; for the browser
    // process it returns -1 immediately and we carry on.
    const int exit_code = CefExecuteProcess(main_args, app.get(), nullptr);
    if (exit_code >= 0) return exit_code;

    CefSettings settings;
    settings.no_sandbox = true;

    // Keep Chromium's own state out of the install directory -- it may not be
    // writable, and a per-user install must not need administrator rights.
    const auto cache = converter::user_data_dir() / L"cef-cache";
    CefString(&settings.root_cache_path) = cache.wstring();

    settings.log_severity = LOGSEVERITY_WARNING;
    const auto log_file = converter::user_data_dir() / L"logs" / L"cef.log";
    CefString(&settings.log_file) = log_file.wstring();

    // If an instance is already running, CefInitialize hands our arguments to
    // it and that process raises its window. Windows only lets it take the
    // foreground if we -- launched by Explorer, so allowed to -- pass the right on.
    ::AllowSetForegroundWindow(ASFW_ANY);

    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
        // The hand-off to the running instance is a clean exit, not a failure.
        const int code = CefGetExitCode();
        return code == CEF_RESULT_CODE_NORMAL_EXIT_PROCESS_NOTIFIED ? 0 : code;
    }

    CefRunMessageLoop();
    CefShutdown();
    return 0;
}
