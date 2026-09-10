// macOS helper-process entry point.
//
// On macOS, CEF cannot relaunch the main executable for subprocesses the way it
// does on Windows and Linux -- each subprocess type has to be its own bundle
// inside Contents/Frameworks. Those bundles all share this entry point.

#include "app.h"
#include "include/cef_app.h"
#include "include/wrapper/cef_library_loader.h"

int main(int argc, char* argv[]) {
    // Load the CEF framework from the enclosing .app bundle. Must happen before
    // any other CEF call.
    CefScopedLibraryLoader library_loader;
    if (!library_loader.LoadInHelper()) return 1;

    // ConverterApp is passed here, not nullptr, and both reasons matter:
    //
    //   * OnRegisterCustomSchemes has to run in EVERY process. Skip it in the
    //     renderer and the app:// scheme is unknown there, so the Vue bundle
    //     fails to load with no obvious error.
    //   * The renderer-side message router lives on ConverterApp. Without it
    //     window.cefQuery is never injected and the entire JS bridge is silently
    //     absent -- the UI renders and then does nothing.
    //
    // ConverterApp::OnContextInitialized only runs in the browser process, so
    // nothing here tries to create a window.
    CefRefPtr<converter::ConverterApp> app(new converter::ConverterApp());

    CefMainArgs main_args(argc, argv);
    return CefExecuteProcess(main_args, app.get(), nullptr);
}
