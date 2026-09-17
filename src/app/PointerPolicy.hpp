#pragma once

#include <cstdint>

namespace np {

// app/PointerPolicy -- WHO is driving this gesture, and WHAT they are allowed
// to do with it. The approved input truth table, in one declarative place.
//
// ==========================================================================
// 1. Why this exists
// ==========================================================================
//
// Before this file, device identity was three unrelated booleans consulted at
// four call sites out of roughly twenty-five canvas handlers. The other
// twenty-one were identity-blind: a finger drove the transform gizmo, the
// selection tools, crop, text and the vector pen exactly as a mouse did. A
// truth table spread across twenty-one `if`s is a table that drifts, so the
// rule is stated ONCE here, as data, and every handler asks.
//
// ==========================================================================
// 2. Identity: `which`, not the finger count
// ==========================================================================
//
// SDL labels its synthesised mouse events: `SDL_TOUCH_MOUSEID` for a finger,
// `SDL_PEN_MOUSEID` for the pencil, any other id for a real mouse or trackpad.
// That label is the primary discriminator, and it is reliable -- measured on
// an iPad, eleven real mouse presses all carried `which=0` while the session's
// one genuine finger press was the only `which=-1`.
//
// The raw finger count is NOT a substitute, and this is the trap that cost a
// day: **iPadOS reports a mouse click as a touch as well.** Every one of those
// eleven mouse presses arrived as `which=0 fingers=1`, the finger appearing
// with the click and vanishing on release. A rule of the form "a finger is
// down, so this is touch" therefore classifies a mouse as a finger, which is
// precisely what stopped a mouse painting on iPad.
//
// The count earns its keep in exactly one case: SDL's primary-touch
// REASSIGNMENT press (a second finger landing on a gesture already in flight)
// is not labelled `SDL_TOUCH_MOUSEID`, but it happens with two fingers already
// on the glass. So `fingers >= 2` means touch; one finger's worth never
// overrules `which`.
//
// ==========================================================================
// 3. Resolved once per gesture, not per frame
// ==========================================================================
//
// Identity is decided at the press and held for the gesture's whole life,
// because the facts it is derived from move underneath it: ImGui's input
// trickling reports the button held for one frame after the fingers are gone,
// and a per-frame verdict flips to "not touch" in that window -- one frame
// that looks exactly like a mouse press, which is one deposited dab. Deciding
// at the press and clearing only when ImGui itself agrees the button is up
// closes that window by construction.
enum class PointerKind : uint8_t {
  Mouse,
  Pen,
  Touch,
};

// The distinct things a canvas gesture can be asking to do. Keyed on the
// INTERACTION rather than on the tool, because several tools are two different
// interactions depending on phase -- Crop, Gradient and Frame/Slice each
// DEFINE a new rect or ramp (content editing, denied to touch) and then let you
// drag ITS HANDLES afterwards (manipulation, allowed to touch). A table keyed
// on the tool alone could not express that split, and the handler always knows
// which phase it is in.
enum class CanvasInteraction : uint8_t {
  // --- content creation / editing: mouse and pen only ---------------------
  PaintStroke,       // brush, eraser, airbrush, smudge, pencil, dodge/burn
  SelectionDefine,   // marquee, ellipse, lasso, polygon lasso, magic wand
  ShapeDefine,       // the shape tool's drag-out
  VectorPointPlace,  // pen/curve tool placing new path points
  PaintBucket,
  GradientDefine,   // dragging out the ramp itself
  CloneStamp,       // both the anchor and the painting
  CropDefine,       // dragging out a NEW crop/frame/slice rect
  MeasureDefine,    // drawing a new measure line
  FlatsFill,        // the flats bridge/fill tools

  // --- manipulation: every device, touch included -------------------------
  MovePixels,       // the Move tool dragging a layer's pixels
  TransformGizmo,   // the affine gizmo and the warp control net
  CanvasHandle,     // gradient stops, crop/frame handles, radial-blur, gnomon
  TextFrameMove,    // moving or repositioning an existing text box
  LayerRowReorder,  // the layer panel's drag-to-reorder

  // --- special ------------------------------------------------------------
  // Mouse and pen sample on an ordinary click; touch samples only through a
  // LONG PRESS, never a quick tap, so a stray tap cannot silently change the
  // colour. The long-press recogniser is what supplies touch's permission
  // here -- see `pointerMayDrive()`'s own note.
  EyedropperSample,
};

// The table itself. `true` if a gesture driven by `kind` may drive
// `interaction`.
//
// Pen is identical to mouse in every row: there is no interaction where a
// stylus should behave differently from a pointing device, and encoding them
// separately would only invite them to drift apart.
//
// Touch is denied every content-creation row. A denied touch is fully INERT --
// it does not fall through to panning, and it shows no cursor or tooltip
// cue. One-finger touch is never navigation in any tool; navigation is
// two-finger pan/pinch/rotate, the mouse's middle-drag and the wheel, and
// those are reserved in every tool without exception.
//
// `EyedropperSample` answers `false` for touch here on purpose: a quick tap
// must not sample. Touch reaches the eyedropper only through the global
// long-press gesture, which calls the sampler directly rather than asking this
// table for a tap's permission.
constexpr bool pointerMayDrive(CanvasInteraction interaction, PointerKind kind) noexcept {
  if (kind != PointerKind::Touch) return true;  // mouse and pen: everything
  switch (interaction) {
    case CanvasInteraction::MovePixels:
    case CanvasInteraction::TransformGizmo:
    case CanvasInteraction::CanvasHandle:
    case CanvasInteraction::TextFrameMove:
    case CanvasInteraction::LayerRowReorder: return true;

    case CanvasInteraction::PaintStroke:
    case CanvasInteraction::SelectionDefine:
    case CanvasInteraction::ShapeDefine:
    case CanvasInteraction::VectorPointPlace:
    case CanvasInteraction::PaintBucket:
    case CanvasInteraction::GradientDefine:
    case CanvasInteraction::CloneStamp:
    case CanvasInteraction::CropDefine:
    case CanvasInteraction::MeasureDefine:
    case CanvasInteraction::FlatsFill:
    case CanvasInteraction::EyedropperSample: return false;
  }
  return false;
}

// True when a single-pointer gesture of this kind is CLAIMED by the tool, so
// the pan predicate must stand down for it.
//
// This generalises what was first found as a Transform-only collision: the pan
// predicate has no per-tool exclusion and runs AFTER the tool handlers, so a
// one-finger drag could grab a transform handle and pan the canvas out from
// under it in the same frame. Every manipulation row has the same shape, so
// the rule belongs here rather than in five handlers.
//
// Navigation is untouched by this: it speaks only about the single-pointer
// gesture. Two-finger pan/pinch/rotate and the mouse's middle-drag are decided
// elsewhere and are reserved in every tool.
constexpr bool toolClaimsSinglePointer(CanvasInteraction interaction,
                                       PointerKind kind) noexcept {
  return pointerMayDrive(interaction, kind);
}

// SDL's two sentinel mouse ids, mirrored here so this header stays free of
// SDL and can be exercised by the selftest without a window. `main.cpp`
// static_asserts these against the real `SDL_TOUCH_MOUSEID` /
// `SDL_PEN_MOUSEID` at the one place it converts, so the mirror cannot drift
// silently.
inline constexpr uint32_t kTouchMouseId = 0xFFFFFFFFu;  // SDL_TOUCH_MOUSEID
inline constexpr uint32_t kPenMouseId = 0xFFFFFFFEu;    // SDL_PEN_MOUSEID

// Decide WHO owns a gesture, from the press event alone. Pure, so the truth
// table's foundation is testable without a device.
//
// `which` is SDL's device id for the (possibly synthesised) mouse event.
// `fingerCount` is the raw count from IOSTouchTracker -- our own UIKit-level
// tally, not SDL's. `penInContact` is the pencil-tip-down state.
//
// Order matters, and each step is load-bearing:
//
//   1. A pen in contact wins outright. The pencil generates a synthesised
//      mouse event whose `which` is not always the pen sentinel, and contact
//      is the ground truth for "the tip is on the glass".
//   2. The sentinels decide next, because they are SDL's own label and were
//      measured correct on every press.
//   3. Only then does `fingerCount >= 2` reclassify to touch, catching SDL's
//      primary-touch reassignment press, which carries a real-looking id.
//      Note this is BELOW the sentinel checks but ABOVE the mouse default, so
//      it can promote an unlabelled event to touch yet never demote a labelled
//      one.
//   4. Anything left is a real mouse or trackpad. Crucially this includes
//      `which=0 fingers=1`, which is what iPadOS reports for an ordinary mouse
//      click -- treating that as touch is the bug that stopped a mouse
//      painting, so the default here must be Mouse, not Touch.
constexpr PointerKind resolvePointerKind(uint32_t which, int fingerCount,
                                         bool penInContact) noexcept {
  if (penInContact) return PointerKind::Pen;
  if (which == kPenMouseId) return PointerKind::Pen;
  if (which == kTouchMouseId) return PointerKind::Touch;
  if (fingerCount >= 2) return PointerKind::Touch;
  return PointerKind::Mouse;
}

// How big a handle's GRAB area is, in screen pixels of radius -- which is not
// the same as how big it is drawn.
//
// A mouse pointer is one pixel with a visible hotspot, so a tight radius is
// honest precision. A fingertip is a contact patch roughly a centimetre across
// whose centre the user cannot see, because their own finger is covering it.
// Handles sized for the mouse are therefore not "slightly fiddly" under touch,
// they are aimed at a point the user has no way to observe.
//
// iOS Human Interface Guidelines put the minimum tap target at 44x44 points, so
// touch gets a 22pt radius while mouse and pen keep the 9px they have always
// had. The drawn handle does NOT change size: a 44pt dot on every crop corner
// would bury the picture the handles sit on, and the visual is already clear
// enough to aim at -- it is the tolerance around it that was wrong.
//
// Radius is in SCREEN pixels, so callers divide by zoom to reach texels. That
// keeps the target the same size under the finger at every zoom level, which is
// the property that actually matters: a handle must not get harder to hit
// because the user zoomed out.
inline constexpr float kPrecisePointerGrabPx = 9.0f;
inline constexpr float kTouchTargetPt = 44.0f;
inline constexpr float kTouchPointerGrabPx = kTouchTargetPt * 0.5f;

constexpr float pointerGrabRadiusPx(PointerKind kind) noexcept {
  return kind == PointerKind::Touch ? kTouchPointerGrabPx : kPrecisePointerGrabPx;
}

// How long a finger must be held still before it counts as a long press, and
// how far it may wander while doing so. Shared by the global colour sampler and
// the layer panel's press-and-hold reorder, so the two gestures cannot disagree
// about what "held" means.
inline constexpr float kLongPressMs = 450.0f;
inline constexpr float kLongPressSlopPx = 12.0f;

}  // namespace np
