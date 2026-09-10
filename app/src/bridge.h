#pragma once

#include <memory>
#include <string>
#include <vector>

#include "conv/ffmpeg.hpp"
#include "conv/job.hpp"
#include "conv/logging.hpp"
#include "include/wrapper/cef_message_router.h"

namespace converter {

// Routes window.cefQuery calls from the Vue app to the conversion engine, and
// pushes engine events back to the page.
//
// Lifetime note: engine callbacks arrive on a worker thread and are marshalled
// onto the CEF UI thread before touching anything here. Those posted tasks can
// outlive the Bridge if the window closes mid-encode, so they capture a
// weak_ptr and simply do nothing if the Bridge has gone.
//
// Threading: all state lives on the CEF UI thread. SetDragPaths and
// DeliverInputs re-post themselves there when called from any other thread, so
// no lock is needed. Checking paths on disk (inputs, drops, picked files) runs
// on a CEF file thread and hands its result back to the UI thread, so a slow
// network drive never freezes the window.
class Bridge : public CefMessageRouterBrowserSide::Handler,
               public std::enable_shared_from_this<Bridge> {
public:
    static std::shared_ptr<Bridge> Create();
    ~Bridge();

    // CefMessageRouterBrowserSide::Handler
    bool OnQuery(CefRefPtr<CefBrowser> browser,
                 CefRefPtr<CefFrame> frame,
                 int64_t query_id,
                 const CefString& request,
                 bool persistent,
                 CefRefPtr<Callback> callback) override;

    void OnQueryCanceled(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         int64_t query_id) override;

    // Stops any running job. Called when the window is closing so the app does
    // not sit there with an orphaned ffmpeg process.
    void Shutdown();

    // The paths being dragged over the window, from CefDragHandler::OnDragEnter.
    // The page cannot learn them itself -- Chromium gives a dropped DOM File a
    // name but no path -- so on drop it fetches them with takeDroppedPaths.
    // Every drag-enter replaces the previous set; an empty list clears it.
    void SetDragPaths(std::vector<std::string> utf8_paths);

    // Sends paths from outside the page (command line, a second launch, macOS
    // "Open with") to it as an `inputs` event, split into accepted and rejected
    // entries. |source| is "cmdline" | "relaunch" | "openWith" | "drop";
    // |append| adds to the page's list instead of replacing it.
    //
    // Held back until the page subscribes: command-line paths arrive while the
    // page is still loading, and dropping them the way other early events are
    // dropped would lose them.
    void DeliverInputs(std::vector<std::string> utf8_paths, const std::string& source, bool append);

private:
    Bridge();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace converter
