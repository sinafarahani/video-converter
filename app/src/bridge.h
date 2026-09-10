#pragma once

#include <memory>
#include <string>

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

private:
    Bridge();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace converter
