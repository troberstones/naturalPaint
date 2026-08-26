#pragma once

// ui/MacTrackpadGestures -- the AppKit backend for pinch-to-zoom.
//
// **Why this file exists at all, rather than a field on `SDL_MouseWheelEvent`
// this app could just read.** track10/input's brief asked, explicitly, that
// pinch not be faked or invented against an API the vendored library does
// not actually have -- so this is what was checked, on this build's own
// vendored SDL 3.2.24 (CMake-fetched into `_deps/sdl3-src`), before writing
// a single line here:
//
//  - SDL2's `SDL_MULTIGESTURE` event, the one place a pinch could arrive, is
//    gone in SDL3. `SDL3/SDL_events.h` in this vendored copy does not merely
//    lack a replacement -- it explicitly reserves the old event-type values:
//    "0x800, 0x801, and 0x802 were the Gesture events from SDL2. Do not
//    reuse these values! sdl2-compat needs them!" That is a library that
//    dropped the feature on purpose, not one waiting for an app to ask.
//  - The vendored Cocoa backend (`src/video/cocoa/`) has no handler anywhere
//    for `NSEventTypeMagnify`, `NSEventTypeRotate` or `NSEventTypeSwipe` --
//    grepped the whole directory for "pinch", "magnif" and "gesture"; the
//    only hits are an unrelated comment and an unrelated selector name.
//  - `SDL_MouseWheelEvent` (`SDL3/SDL_events.h`) carries exactly what a
//    two-finger SCROLL reports -- `x`/`y`/`integer_x`/`integer_y` -- and
//    nothing about finger separation. Pinch is not a wheel event under a
//    different name; it is a gesture SDL3 does not deliver at all here.
//
// So this app cannot get a pinch through SDL on this vendored version, full
// stop, and the honest alternative -- the one the brief asked for by name --
// is to reach past SDL for it, the same way `ui/MacNativeMenu.mm` already
// reaches past SDL for the menu bar: an `NSEvent` local monitor for
// `NSEventTypeMagnify`, installed once `NSApp` exists.
//
// **Why a local monitor and not `-magnifyWithEvent:` on some NSView.** SDL3
// owns the window's content view and its responder chain; subclassing or
// swizzling it to add a gesture method is exactly the kind of thing that
// breaks silently the next time SDL's own view implementation changes.
// `+[NSEvent addLocalMonitorForEventsMatchingMask:handler:]` asks nothing of
// SDL's view at all -- it observes events as `-[NSApplication sendEvent:]`
// dispatches them, which is the exact call SDL3's own Cocoa pump already
// makes for every event (`SDL_cocoaevents.m`'s `Cocoa_PumpEventsUntilDate()`:
// `[NSApp nextEventMatchingMask:...]` then `[NSApp sendEvent:event]`), so
// this runs on the very same pump, same thread, no new run loop and no
// polling of its own.
//
// Off Apple these are an inline no-op rather than an `#if` at the call site,
// the same shape `ui/MacNativeMenu.hpp` already uses and for the same
// reason: the one call site in `ui/MacPaintUI.cpp` should not have to know
// which platform it is running on.
namespace np {

#if defined(__APPLE__)

// Drains and returns the total `NSEvent.magnification` accumulated by
// pinch gestures since the last call, installing the local event monitor on
// first use. Called every frame regardless of whether the canvas is
// hovered -- the caller decides whether a nonzero result should DO
// anything (only when the canvas was hovered when it arrived), but the
// accumulator itself is drained unconditionally so a pinch that started
// over a side panel cannot leave a stale sample to be misread as freshly
// started the next time the cursor happens to be over the canvas.
//
// 0.0f when nothing has happened, which is indistinguishable from "no
// monitor could be installed" (no `NSApp` yet) -- both are "nothing to do
// this frame", and the caller already no-ops on an exact zero the same way
// it does for the wheel's `MouseWheel == 0.0f`.
float pollPinchMagnification();

#else

inline float pollPinchMagnification() { return 0.0f; }

#endif

}  // namespace np
