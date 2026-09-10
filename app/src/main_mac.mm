// macOS entry point for the main application.
//
// macOS needs three things the other platforms do not:
//
//  1. The CEF framework is loaded at runtime (CefScopedLibraryLoader) rather
//     than linked, and this must happen before any other CEF call.
//  2. The NSApplication instance must implement CefAppProtocol, so CEF can tell
//     when it is inside -sendEvent: and avoid re-entering the Chromium message
//     loop. Without it CEF aborts during initialisation.
//  3. Subprocesses are separate helper bundles (main_mac_helper.cc), so this
//     entry point never calls CefExecuteProcess.

#import <Cocoa/Cocoa.h>

#include "app.h"
#include "include/cef_application_mac.h"
#include "include/wrapper/cef_library_loader.h"
#include "platform.h"

@interface ConverterApplication : NSApplication <CefAppProtocol> {
 @private
  BOOL handlingSendEvent_;
}
@end

@implementation ConverterApplication

- (BOOL)isHandlingSendEvent {
  return handlingSendEvent_;
}

- (void)setHandlingSendEvent:(BOOL)handlingSendEvent {
  handlingSendEvent_ = handlingSendEvent;
}

- (void)sendEvent:(NSEvent*)event {
  CefScopedSendingEvent sendingEventScoper;
  [super sendEvent:event];
}

// Cmd-Q and the Quit menu item. Letting NSApplication call exit() would tear
// the process down underneath CEF. Instead every window is asked to close,
// which runs the normal browser shutdown (stopping any encode in progress) and
// ends the message loop from ConverterClient::OnBeforeClose.
- (void)terminate:(id)sender {
  for (NSWindow* window in [self windows]) {
    [window performClose:nil];
  }
}

@end

namespace {

// An application menu with Quit, and an Edit menu. The Edit menu is not
// decoration: on macOS, Cmd-C / Cmd-V / Cmd-A in a text field are delivered
// through these menu items, so without it the folder fields cannot be pasted
// into.
void build_main_menu() {
  NSMenu* bar = [[NSMenu alloc] init];

  NSMenuItem* app_item = [[NSMenuItem alloc] init];
  [bar addItem:app_item];
  NSMenu* app_menu = [[NSMenu alloc] init];
  [app_menu addItemWithTitle:@"Quit Video Converter"
                      action:@selector(terminate:)
               keyEquivalent:@"q"];
  [app_item setSubmenu:app_menu];

  NSMenuItem* edit_item = [[NSMenuItem alloc] init];
  [bar addItem:edit_item];
  NSMenu* edit_menu = [[NSMenu alloc] initWithTitle:@"Edit"];
  [edit_menu addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
  [edit_menu addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"Z"];
  [edit_menu addItem:[NSMenuItem separatorItem]];
  [edit_menu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
  [edit_menu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
  [edit_menu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
  [edit_menu addItemWithTitle:@"Select All" action:@selector(selectAll:) keyEquivalent:@"a"];
  [edit_item setSubmenu:edit_menu];

  [NSApp setMainMenu:bar];
}

}  // namespace

int main(int argc, char* argv[]) {
  CefScopedLibraryLoader library_loader;
  if (!library_loader.LoadInMain()) return 1;

  int exit_code = 0;
  @autoreleasepool {
    // Must be created before CefInitialize so that it is the shared instance.
    [ConverterApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    build_main_menu();

    CefMainArgs main_args(argc, argv);

    CefSettings settings;
    settings.no_sandbox = true;

    const auto data = converter::user_data_dir();
    CefString(&settings.root_cache_path) = (data / "cef-cache").string();
    settings.log_severity                = LOGSEVERITY_WARNING;
    CefString(&settings.log_file)        = (data / "logs" / "cef.log").string();

    CefRefPtr<converter::ConverterApp> app(new converter::ConverterApp());
    if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
      exit_code = CefGetExitCode();
    } else {
      [NSApp activateIgnoringOtherApps:YES];
      CefRunMessageLoop();
      CefShutdown();
    }
  }
  return exit_code;
}
