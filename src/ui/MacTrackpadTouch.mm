#include "ui/MacTrackpadTouch.hpp"

#import <Cocoa/Cocoa.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>

// ui/MacTrackpadTouch -- ui/MacTrackpadTouch.hpp's AppKit half. See that
// header for the full "why raw NSTouch, why this needs a real NSWindow, why
// -setNextResponder: and not subclassing SDL3View, why every poll
// re-verifies the splice" reasoning; only implementation-level choices are
// commented here.

namespace np {

namespace {

constexpr size_t kMaxTrackedTouches = 8;

// All touch/window state below is written only from AppKit's touch
// responder methods and read only from the poll functions -- both run on
// the main thread inside the same `[NSApp sendEvent:]` dispatch SDL3's own
// Cocoa pump drives, the identical "no cross-thread access to guard
// against" posture ui/MacTrackpadGestures.mm's own accumulators already
// take.
NSWindow* gWindow = nil;
NSResponder* gResponder = nil;

// Currently-touching fingers, SORTED ASCENDING by `identity` (the
// monotonically-assigned token below, not AppKit's own opaque
// `NSTouch.identity` object) every time it is rebuilt. This is what makes
// `pollTwoFingerTouch()`'s returned pair's "first"/"second" STABLE across
// frames for one continuous two-finger gesture -- `NSSet` iteration order
// (what `-touchesMatchingPhase:inView:` returns) is unspecified and
// observed to vary call to call, and app/TouchGesture.hpp's
// `computeTwoTouchDelta()` computes a SIGNED angle from `curB - curA`: if
// which finger is "A" and which is "B" ever silently swapped between two
// frames of the same gesture, the rotation delta would flip 180 degrees
// with no other symptom. Sorting by a stable per-touch token, assigned once
// per finger and never reassigned while it stays down, closes that off:
// the first finger to touch down is always "first" for the gesture's whole
// lifetime.
int gTouchCount = 0;
std::array<TrackpadTouchPoint, kMaxTrackedTouches> gTouches{};

float gDeviceWidth = 0.0f;
float gDeviceHeight = 0.0f;

// `NSTouch.identity` -> our own uint64_t token, so downstream pure C++
// (app/TouchGesture.hpp, app/TouchGestureSession) never has to hold an
// Objective-C object. Rebuilt (not incrementally edited) on every touch
// event from whatever `-touchesMatchingPhase:NSTouchPhaseTouching` reports
// right now, dropping any identity no longer present -- the same "absolute
// from the event, not accumulated diffs" shape as
// app/TouchGesture.hpp's own "absolute from gesture-start" discipline one
// layer up, and it means a finger that lifts and a DIFFERENT finger that
// later touches down are never confused even if AppKit ever reused an
// `NSTouch.identity` object (Apple's own docs do not guarantee it will not).
NSMutableDictionary<id, NSNumber*>* gIdentityTokens = nil;
uint64_t gNextToken = 1;

}  // namespace

}  // namespace np

// A small, file-owned NSResponder -- see ui/MacTrackpadTouch.hpp's header
// comment for why this is spliced into the responder chain via
// -setNextResponder: rather than SDL3View being subclassed or swizzled.
@interface NPTouchResponder : NSResponder
@end

@implementation NPTouchResponder

- (void)np_handleTouchEvent:(NSEvent*)event {
  NSSet<NSTouch*>* touching = [event touchesMatchingPhase:NSTouchPhaseTouching inView:nil];

  if (touching.count > 0) {
    NSTouch* any = touching.anyObject;
    np::gDeviceWidth = static_cast<float>(any.deviceSize.width);
    np::gDeviceHeight = static_cast<float>(any.deviceSize.height);
  }

  if (np::gIdentityTokens == nil) np::gIdentityTokens = [NSMutableDictionary new];
  NSMutableDictionary<id, NSNumber*>* freshTokens =
      [NSMutableDictionary dictionaryWithCapacity:touching.count];

  std::array<np::TrackpadTouchPoint, np::kMaxTrackedTouches> collected{};
  int count = 0;
  for (NSTouch* t in touching) {
    if (static_cast<size_t>(count) >= np::kMaxTrackedTouches) break;
    NSNumber* token = np::gIdentityTokens[t.identity];
    if (token == nil) token = @(np::gNextToken++);
    freshTokens[t.identity] = token;

    const NSPoint p = t.normalizedPosition;
    np::TrackpadTouchPoint pt;
    pt.identity = token.unsignedLongLongValue;
    pt.x = static_cast<float>(p.x);
    // AppKit's `normalizedPosition` is y-UP; this app's own screen/view
    // convention is y-DOWN (app/WheelInput.hpp's own cross-validated
    // derivation) -- flipped here, the one read site, so nothing
    // downstream needs to know AppKit's convention differs.
    pt.y = 1.0f - static_cast<float>(p.y);
    collected[static_cast<size_t>(count)] = pt;
    ++count;
  }
  np::gIdentityTokens = freshTokens;

  std::sort(collected.begin(), collected.begin() + count,
            [](const np::TrackpadTouchPoint& a, const np::TrackpadTouchPoint& b) {
              return a.identity < b.identity;
            });
  np::gTouchCount = count;
  np::gTouches = collected;
}

- (void)touchesBeganWithEvent:(NSEvent*)event {
  [self np_handleTouchEvent:event];
  [super touchesBeganWithEvent:event];
}

- (void)touchesMovedWithEvent:(NSEvent*)event {
  [self np_handleTouchEvent:event];
  [super touchesMovedWithEvent:event];
}

- (void)touchesEndedWithEvent:(NSEvent*)event {
  [self np_handleTouchEvent:event];
  [super touchesEndedWithEvent:event];
}

- (void)touchesCancelledWithEvent:(NSEvent*)event {
  [self np_handleTouchEvent:event];
  [super touchesCancelledWithEvent:event];
}

@end

namespace np {

namespace {

// Re-verifies (and if needed, repairs) both halves of the splice: the
// content view's `acceptsTouchEvents` opt-in, and this file's responder
// sitting immediately ahead of whatever SDL itself last put in
// `nextResponder`. Called from every poll, not just once at install time --
// see the header comment on why a one-time install cannot be trusted to
// hold.
void ensureSpliced() {
  if (gWindow == nil) return;  // installTrackpadTouchCapture() was never called, or failed
  NSView* view = gWindow.contentView;
  if (view == nil) return;

  // `-setAcceptsTouchEvents:` is the API Apple's own older trackpad-gesture
  // documentation names, but the SDK vendored with this toolchain marks it
  // deprecated since macOS 10.12.2 in favour of `allowedTouchTypes` --
  // `NSTouchTypeMaskIndirect` is the modern equivalent for a TRACKPAD
  // (`NSTouchTypeMaskDirect` is for a touchscreen, which this Mac does not
  // have); confirmed by an actual build against this SDK, not merely by
  // reading a comment, since the two APIs' exact equivalence is a real
  // build-time fact, not something to take on faith.
  if (!(view.allowedTouchTypes & NSTouchTypeMaskIndirect))
    view.allowedTouchTypes = view.allowedTouchTypes | NSTouchTypeMaskIndirect;

  if (gResponder == nil) gResponder = [NPTouchResponder new];
  if (view.nextResponder != gResponder) {
    // Either the very first splice, or SDL has reasserted its own
    // `nextResponder` since we last checked (confirmed to happen at more
    // than one point in SDL's own window lifecycle -- see the header
    // comment) and clobbered ours. Either way: keep whatever is there now
    // as our own continuation, then insert ourselves ahead of it.
    gResponder.nextResponder = view.nextResponder;
    view.nextResponder = gResponder;
  }
}

}  // namespace

void installTrackpadTouchCapture(SDL_Window* window) {
  NSWindow* nsWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
      SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL);
  if (nsWindow == nil) {
    // Loud, not swallowed: a silent failure here would look exactly like
    // "no gesture happened yet" to pollTwoFingerTouch()'s own caller, and
    // the graceful fallback to the existing AppKit-classified pinch/rotate
    // path (ui/MacPaintUI.cpp) means nothing would visibly break -- so this
    // is the one place that degradation gets recorded at all.
    std::fprintf(stderr,
                 "installTrackpadTouchCapture: SDL_GetPointerProperty returned no NSWindow -- "
                 "raw two-finger trackpad capture is unavailable this session; falling back to "
                 "the existing pinch/rotate path\n");
    return;
  }
  gWindow = nsWindow;
  ensureSpliced();
}

std::optional<std::pair<TrackpadTouchPoint, TrackpadTouchPoint>> pollTwoFingerTouch() {
  ensureSpliced();
  if (gTouchCount != 2) return std::nullopt;
  return std::make_pair(gTouches[0], gTouches[1]);
}

TrackpadDeviceSize trackpadDeviceSize() { return TrackpadDeviceSize{gDeviceWidth, gDeviceHeight}; }

bool trackpadNaturalScrolling() {
  // Cached after the first read -- System Settings > Trackpad's own
  // preference, not something that changes mid-gesture or even mid-session
  // in any normal use. `com.apple.swipescrolldirection` is the same key
  // AppKit itself reads when synthesising a scroll-wheel event's sign from
  // raw touches; bypassing that synthesis (the whole reason this file
  // exists) means bypassing that read too, so it is repeated here by hand.
  // Lives in `NSGlobalDomain`, part of `+standardUserDefaults`'s own
  // default search list -- no explicit domain needed. Apple's own default,
  // absent any user override (confirmed on this machine: `defaults read -g
  // com.apple.swipescrolldirection` reports no override present), is
  // natural (`true`).
  static bool cached = true;
  static bool haveCached = false;
  if (!haveCached) {
    id value = [[NSUserDefaults standardUserDefaults] objectForKey:@"com.apple.swipescrolldirection"];
    cached = (value == nil) ? true : [value boolValue];
    haveCached = true;
  }
  return cached;
}

}  // namespace np
