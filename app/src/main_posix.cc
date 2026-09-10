// Linux entry point.
//
// macOS has its own, main_mac.mm: the framework is loaded at runtime there,
// subprocesses are separate helper bundles, and NSApplication needs CEF's
// protocol. CMake only builds this file on Linux.

#include "app.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "platform.h"

int main(int argc, char* argv[]) {
    CefMainArgs main_args(argc, argv);
    CefRefPtr<converter::ConverterApp> app(new converter::ConverterApp());

    // Subprocesses re-enter here; CefExecuteProcess runs their loop and returns
    // an exit code. The browser process gets -1 and carries on.
    const int exit_code = CefExecuteProcess(main_args, app.get(), nullptr);
    if (exit_code >= 0) return exit_code;

    CefSettings settings;
    settings.no_sandbox = true;

    const auto data = converter::user_data_dir();
    CefString(&settings.root_cache_path) = (data / "cef-cache").string();
    settings.log_severity                = LOGSEVERITY_WARNING;
    CefString(&settings.log_file)        = (data / "logs" / "cef.log").string();

    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
        // An instance was already running and CefInitialize handed it our
        // arguments. That hand-off is a clean exit, not a failure.
        const int code = CefGetExitCode();
        return code == CEF_RESULT_CODE_NORMAL_EXIT_PROCESS_NOTIFIED ? 0 : code;
    }

    CefRunMessageLoop();
    CefShutdown();
    return 0;
}
