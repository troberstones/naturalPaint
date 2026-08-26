#include "ui/MacTrackpadGestures.hpp"

#import <Cocoa/Cocoa.h>

// ui/MacTrackpadGestures -- ui/MacTrackpadGestures.hpp's AppKit half. See
// that header for why this file exists and why a local event monitor rather
// than a responder method on SDL's own content view.

namespace np {

namespace {
// Accumulated `NSEvent.magnification` since the last `pollPinchMagnification()`
// call. Written only from the monitor's handler block and read only from
// `pollPinchMagnification()` -- both run on the main thread, inside the same
// `[NSApp sendEvent:]` dispatch SDL3's own Cocoa pump drives
// (`Cocoa_PumpEventsUntilDate()`), so there is no cross-thread access here to
// guard against.
float gPinchAccumulator = 0.0f;
bool gMonitorInstalled = false;
}  // namespace

float pollPinchMagnification() {
  if (!gMonitorInstalled) {
    // The same "no-op if NSApp is somehow still nil" posture
    // `ui/MacNativeMenu.mm`'s `installNativeMenuBar()` takes, for the same
    // reason: this is polled from the very first frame of the main loop,
    // and asserting on a window system that has not finished initialising
    // is a worse failure than trying again next frame.
    if (NSApp == nil) return 0.0f;
    // Local, not global: this observes events already being dispatched to
    // THIS app via `-sendEvent:` (which is exactly what SDL3's Cocoa pump
    // calls for every event -- see this file's header comment) rather than
    // installing a system-wide tap, which would need Accessibility
    // permission this app has no other reason to ask for.
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskMagnify
                                           handler:^NSEvent* (NSEvent* event) {
                                             gPinchAccumulator +=
                                                 static_cast<float>(event.magnification);
                                             // Returning the event unmodified: this
                                             // is an observer, not a filter, and
                                             // nothing else in the app has any
                                             // reason to also want a magnify event.
                                             return event;
                                           }];
    gMonitorInstalled = true;
  }
  const float out = gPinchAccumulator;
  gPinchAccumulator = 0.0f;
  return out;
}

}  // namespace np
