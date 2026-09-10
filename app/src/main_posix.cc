// Linux and macOS entry point.
//
// Linux is complete. macOS is NOT: a CEF app there has to live inside a .app
// bundle with a separate "Helper" bundle per subprocess type, and the framework
// has to be loaded before any CEF call. The library-loading half of that is
// handled below; the bundle layout is a packaging job that has not been done
// yet. See the note at the bottom of this file.

#include "app.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "platform.h"

#if defined(__APPLE__)
#  include "include/wrapper/cef_library_loader.h"
#endif

int main(int argc, char* argv[]) {
#if defined(__APPLE__)
    // On macOS the framework is loaded dynamically and this must happen before
    // any other CEF call, including CefExecuteProcess.
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInMain()) return 1;
#endif

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
        return CefGetExitCode();
    }

    CefRunMessageLoop();
    CefShutdown();
    return 0;
}

// --- Remaining macOS work -------------------------------------------------
//
//  1. Package as Converter.app with Contents/Frameworks/"Chromium Embedded
//     Framework.framework" and four helper bundles (plain, GPU, Plugin,
//     Renderer), each with its own Info.plist and LSUIElement=1.
//  2. The helpers need their own tiny main() calling CefExecuteProcess after
//     CefScopedLibraryLoader::LoadInHelper().
//  3. Sign every nested bundle inside-out, and notarise, or Gatekeeper refuses
//     to launch it.
//
// CEF ships a working example of this layout in tests/cefsimple; the CMake
// there is the reference to copy.
