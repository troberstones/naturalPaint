#pragma once

#include <optional>
#include <utility>

#include "app/TouchGesture.hpp"  // TrackpadTouchPoint

struct SDL_Window;

namespace np {

// ui/MacTrackpadTouch -- the raw NSTouch capture layer app/TouchGesture.hpp's
// own header comment names: bypasses AppKit's magnify/rotate gesture
// classifier (which "might switch its interpretation from one gesture to
// another" mid-motion, per Apple's own `LightTable` sample documentation --
// the root cause item 4 was filed against) by reading the two fingers' raw
// positions directly, every frame.
//
// **Why this needs a real NSWindow, unlike ui/MacTrackpadGestures.hpp's
// pollers.** `NSEventTypeMagnify`/`NSEventTypeRotate` are delivered through
// `-[NSApplication sendEvent:]`, which a LOCAL EVENT MONITOR observes
// app-wide with no view involved at all -- that is why
// `pollPinchMagnification()`/`pollRotationDegrees()` need nothing but
// `NSApp`. Raw touches are NOT delivered that way: Apple's "Handling
// Trackpad Events" documentation is explicit that touches go only to a VIEW
// that has called `-setAcceptsTouchEvents:YES` and happens to be under the
// mouse pointer at first touch-down -- a view-level opt-in and dispatch
// target, invisible to any app-wide monitor. So this needs SDL's own
// content view specifically, which means it needs the `NSWindow` SDL
// created it inside.
//
// **Why this reaches into SDL's own view rather than adding a new one.**
// SDL3's vendored Cocoa backend (`src/video/cocoa/SDL_cocoawindow.m`) never
// calls `setAcceptsTouchEvents:` and implements none of the four touch
// responder methods on `SDL3View`, its real content view class -- confirmed
// by reading that file directly on this build's vendored 3.2.24. Touches
// are gated on the VIEW under the pointer, not on whichever object is
// merely first in the responder chain, so there is no way to observe them
// without touching the view AppKit will actually deliver to: SDL's own.
//
// **Why this is `-setNextResponder:`, not subclassing or swizzling
// `SDL3View`.** `ui/MacTrackpadGestures.hpp`'s own header comment already
// rejected exactly that for the pinch/rotate monitor, for a reason that
// applies here just as much: replacing or patching a class this app does
// not own "is exactly the kind of thing that breaks silently the next time
// SDL's own view implementation changes." Instead: `setAcceptsTouchEvents:
// YES` (a single, public, idempotent property set -- the one unavoidable
// reach into SDL's view, and the whole reason this needs the real
// `NSWindow`) turns delivery ON; a small `NSResponder` subclass owned
// entirely by this file is spliced into the responder chain via the public
// `-setNextResponder:` API, ahead of whatever SDL itself already put there
// (`SDL3Cocoa_WindowListener`, confirmed via the same source read). Because
// `SDL3View` implements none of the four touch methods, AppKit's own
// documented default `NSResponder` behaviour -- forwarding an unhandled
// event up `nextResponder` -- carries every touch straight to this file's
// responder without one line of `SDL3View` ever being touched.
//
// **Why every poll re-verifies the splice.** SDL re-asserts
// `sdlContentView.nextResponder` at more than one point in its own window
// lifecycle (window creation, and again around a later refocus codepath --
// both confirmed by reading `SetupWindowData()` and its later call sites).
// A ONE-TIME insertion could be silently overwritten by SDL's own later
// reassignment, and unlike a monitor (installed once against `NSApp`, which
// nothing else reassigns), there is no way to be notified when this
// happens -- so this checks and, if needed, re-splices on every single
// poll rather than trusting a one-time install to hold. Self-healing, not
// self-congratulatory: cheap (`==` and maybe two `-setNextResponder:`
// calls) against a real, previously-observed failure mode, not a
// hypothetical one.
#if defined(__APPLE__)

// Call once, right after `SDL_CreateWindow()` (mirrors
// `setFileDialogParentWindow()`'s own placement and reasoning in
// `main.cpp`) -- opts SDL's real content view into touch delivery and
// installs this file's responder ahead of it. Safe to call again (e.g. if
// a future window-recreation path needs it); idempotent.
void installTrackpadTouchCapture(SDL_Window* window);

// Exactly two touches down on the trackpad right now, matched by identity
// across frames -- `std::nullopt` for any other count (zero, one, or three
// or more; a third finger joining, same as a finger lifting, both fall out
// of "tracking a pair"). Re-verifies the responder splice every call (see
// the header comment above) and is otherwise cheap: reads a small, already
// -maintained touch table, no AppKit traversal of its own.
//
// `TrackpadTouchPoint::x`/`y` are ALREADY converted here from
// `NSTouch.normalizedPosition`'s own y-UP convention to this app's y-DOWN
// one -- see `app/TouchGesture.hpp`'s own struct comment for why that
// conversion belongs at this single read site and nowhere downstream.
std::optional<std::pair<TrackpadTouchPoint, TrackpadTouchPoint>> pollTwoFingerTouch();

// The trackpad's own physical surface size in points -- `NSTouch.
// deviceSize`, Apple's own public bridge from `normalizedPosition`'s
// dimensionless [0,1] fraction to a physical distance, which is what lets
// `TwoTouchDelta::panDx`/`panDy` (themselves dimensionless, per
// `app/TouchGesture.hpp`) be converted to screen points: `panDx *
// trackpadDeviceSize().width`. {0, 0} until at least one touch has been
// observed -- there is nothing to report before that.
struct TrackpadDeviceSize {
  float width = 0.0f;
  float height = 0.0f;
};
TrackpadDeviceSize trackpadDeviceSize();

// This Mac's current System Settings > Trackpad > "natural" scrolling
// preference (`true` == natural/default, content tracks the fingers;
// `false` == traditional) -- read once from `NSUserDefaults`'s
// `com.apple.swipescrolldirection` (in `NSGlobalDomain`, part of
// `+standardUserDefaults`'s own default search list -- confirmed against
// `defaults read -g com.apple.swipescrolldirection` on this machine) and
// cached: a real System Settings default, read directly, rather than an
// invented one. This exists because AppKit itself applies this preference
// when it SYNTHESISES
// a scroll-wheel event from raw touches -- bypassing that synthesis (the
// whole reason this file exists) means bypassing that preference's own
// application too, so a caller applying the touch-pan component
// (`TwoTouchDelta::panDx`/`panDy`) must apply it by hand. Zoom and rotate
// have no such preference (Apple defines "natural" only for a directional
// scroll/pan), so this affects the pan component alone.
bool trackpadNaturalScrolling();

#else

inline void installTrackpadTouchCapture(SDL_Window*) {}
inline std::optional<std::pair<TrackpadTouchPoint, TrackpadTouchPoint>> pollTwoFingerTouch() {
  return std::nullopt;
}
struct TrackpadDeviceSize {
  float width = 0.0f;
  float height = 0.0f;
};
inline TrackpadDeviceSize trackpadDeviceSize() { return TrackpadDeviceSize{}; }
inline bool trackpadNaturalScrolling() { return true; }

#endif

}  // namespace np
