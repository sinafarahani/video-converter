// macOS entry point for the main application.
//
// macOS needs four things the other platforms do not:
//
//  1. The CEF framework is loaded at runtime (CefScopedLibraryLoader) rather
//     than linked, and this must happen before any other CEF call.
//  2. The NSApplication instance must implement CefAppProtocol, so CEF can tell
//     when it is inside -sendEvent: and avoid re-entering the Chromium message
//     loop. Without it CEF aborts during initialisation.
//  3. Subprocesses are separate helper bundles (main_mac_helper.cc), so this
//     entry point never calls CefExecuteProcess.
//  4. Finder's "Open With", files dropped on the Dock icon and Dock clicks
//     arrive as Apple Events at the application delegate, never on argv.
//     Paths typed after the binary in a terminal do arrive on argv; those are
//     read by ConverterApp::OnContextInitialized, as on the other platforms.
//
// Built without ARC (CEF's cmake does not enable it), but nothing below depends
// on either mode.

#import <Cocoa/Cocoa.h>

#include <string>
#include <utility>
#include <vector>

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

// The app while the message loop runs, for the delegate below. main() owns it
// and clears this before CefShutdown. Main thread only, which on macOS is the
// CEF UI thread.
converter::ConverterApp* g_app = nullptr;

}  // namespace

// Receives the Apple Events described at the top of this file.
//
// CEF or Chrome may already have installed an application delegate by the time
// ours goes in. Replacing it outright would silently drop whatever it handles,
// so every message this class does not implement itself is forwarded to it.
@interface ConverterAppDelegate : NSObject <NSApplicationDelegate> {
 @private
  id<NSApplicationDelegate> previous_;
}
// The delegate that was installed before this one, if any. Set it BEFORE
// installing this object: AppKit asks which messages a delegate answers at the
// moment it is set.
@property(nonatomic, strong) id<NSApplicationDelegate> previous;
@end

@implementation ConverterAppDelegate

@synthesize previous = previous_;

- (BOOL)respondsToSelector:(SEL)selector {
  return [super respondsToSelector:selector] || [previous_ respondsToSelector:selector];
}

- (id)forwardingTargetForSelector:(SEL)selector {
  if ([previous_ respondsToSelector:selector]) return previous_;
  return [super forwardingTargetForSelector:selector];
}

// Finder "Open With" and Dock drops, cold start included: AppKit holds the
// launch-time event until the run loop starts, and this delegate is in place
// before that. Implementing this method also means AppKit never calls
// application:openFiles: on the previous delegate.
- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
  std::vector<std::string> paths;
  for (NSURL* url in urls) {
    if (!url.isFileURL) continue;
    // A file reference URL (file:///.file/id=...) has no usable path until it
    // is turned into a path URL; nil if the file has gone.
    NSURL* file = url.filePathURL;
    const char* path = file ? file.path.fileSystemRepresentation : nullptr;
    if (path && *path) paths.emplace_back(path);
  }
  if (paths.empty() || !g_app) return;

  // Queued by the app if the client does not exist yet.
  g_app->DeliverLaunchPaths(std::move(paths), "openWith");
  g_app->BringWindowToFront();
}

// A click on the Dock icon. The default, and Chrome's delegate, would open a
// new window when none is visible; this app only ever has the one.
- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)flag {
  if (g_app) g_app->BringWindowToFront();
  return NO;
}

// The Dock icon's menu. Implemented here, so it is never forwarded: Chrome's
// delegate would add "New Window" and "New Incognito Window", which open a
// Chrome window directly, past every guard on the app's own browser. nil keeps
// the standard items (Options, Show All Windows, Hide, Quit).
- (NSMenu*)applicationDockMenu:(NSApplication*)sender {
  return nil;
}

@end

namespace {

// NSApp.delegate does not retain its delegate, so this reference is what keeps
// ours alive. Never released: it lives as long as the process.
ConverterAppDelegate* g_delegate = nil;

void install_app_delegate() {
  g_delegate = [[ConverterAppDelegate alloc] init];
  g_delegate.previous = NSApp.delegate;
  NSApp.delegate = g_delegate;
}

// An application menu with Quit, and an Edit menu. The Edit menu is not
// decoration: on macOS, Cmd-C / Cmd-V / Cmd-A in a text field are delivered
// through these menu items, so without it nothing can be pasted into the text
// fields (the input path, the output folder, the size).
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
      // An instance was already running and CefInitialize handed it our
      // arguments. That hand-off is a clean exit, not a failure.
      if (exit_code == CEF_RESULT_CODE_NORMAL_EXIT_PROCESS_NOTIFIED) exit_code = 0;
    } else {
      // After CefInitialize, so that a delegate CEF or Chrome installs is the
      // one we wrap; before the run loop, so no Apple Event is missed.
      g_app = app.get();
      install_app_delegate();

      [NSApp activateIgnoringOtherApps:YES];
      CefRunMessageLoop();

      g_app = nullptr;
      CefShutdown();
    }
  }
  return exit_code;
}
