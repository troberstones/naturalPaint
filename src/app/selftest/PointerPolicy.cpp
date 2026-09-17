#include "app/selftest/Support.hpp"

#include "app/LongPressGesture.hpp"
#include "app/PointerPolicy.hpp"

namespace np {

// app/PointerPolicy -- the input truth table, and the classifier that decides
// which row of it a gesture is entitled to.
//
// Worth testing headlessly precisely because the bugs it exists to prevent are
// the ones a device test is WORST at catching: every one of them was a single
// frame, or a single misread `which`, inside a gesture that otherwise looked
// entirely normal on screen. The classifier is pure, so the exact event shapes
// measured on the iPad can be replayed here as three integers.
bool runPointerPolicyTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] pointer policy: who owns a gesture, and what it may do\n");

  // --- the classifier, against shapes measured on a real iPad --------------

  // The regression that cost a day: iPadOS reports a mouse click as a touch as
  // well, so the shape below ("which=0 fingers=1") is an ORDINARY MOUSE PRESS,
  // eleven of eleven in the capture. Classifying it as touch is what left a
  // mouse able only to pan.
  check(resolvePointerKind(0, 1, false) == PointerKind::Mouse,
        "mouse click reported with a phantom finger is a mouse");
  check(resolvePointerKind(0, 0, false) == PointerKind::Mouse, "plain mouse press is a mouse");
  check(resolvePointerKind(7, 0, false) == PointerKind::Mouse, "a positive device id is a mouse");

  // SDL's own labels, which are the primary discriminator.
  check(resolvePointerKind(kTouchMouseId, 1, false) == PointerKind::Touch,
        "SDL_TOUCH_MOUSEID is a touch");
  check(resolvePointerKind(kPenMouseId, 0, false) == PointerKind::Pen,
        "SDL_PEN_MOUSEID is a pen");

  // The one case the finger count is allowed to decide: SDL's primary-touch
  // reassignment press, when a second finger lands on a gesture in flight, is
  // NOT labelled with the touch sentinel.
  check(resolvePointerKind(0, 2, false) == PointerKind::Touch,
        "unlabelled press with two fingers down is a touch");
  check(resolvePointerKind(0, 5, false) == PointerKind::Touch,
        "a whole hand on the glass is a touch");

  // Pen contact outranks everything. A hand RESTING on the iPad during a
  // Pencil stroke puts real fingers on the glass; if those reclassified the
  // stroke it would die mid-line.
  check(resolvePointerKind(0, 3, true) == PointerKind::Pen,
        "pen in contact outranks a resting hand's fingers");
  check(resolvePointerKind(kTouchMouseId, 1, true) == PointerKind::Pen,
        "pen in contact outranks even the touch sentinel");

  // The mirrored sentinels are the failure mode a comment cannot catch: if
  // they ever stopped being the top two uint32 values, every touch on the
  // device would classify as a mouse and paint. main.cpp static_asserts them
  // against SDL; this pins the values themselves.
  check(kTouchMouseId == 0xFFFFFFFFu && kPenMouseId == 0xFFFFFFFEu,
        "the mirrored SDL sentinel ids are unchanged");

  // --- the table ------------------------------------------------------------

  // Mouse and pen are identical in every row, and that sameness is the claim
  // -- checked across the whole enum rather than in a few spots, so a future
  // row cannot quietly give the stylus its own behaviour.
  bool penMatchesMouse = true;
  for (int i = 0; i <= static_cast<int>(CanvasInteraction::EyedropperSample); ++i) {
    const auto interaction = static_cast<CanvasInteraction>(i);
    if (!pointerMayDrive(interaction, PointerKind::Mouse) ||
        !pointerMayDrive(interaction, PointerKind::Pen))
      penMatchesMouse = false;
  }
  check(penMatchesMouse, "mouse and pen may drive every interaction alike");

  // Content creation is denied to touch: a finger must never lay down paint,
  // pull a selection, place a vector point or define a crop.
  check(!pointerMayDrive(CanvasInteraction::PaintStroke, PointerKind::Touch),
        "touch may not paint");
  check(!pointerMayDrive(CanvasInteraction::SelectionDefine, PointerKind::Touch),
        "touch may not define a selection");
  check(!pointerMayDrive(CanvasInteraction::VectorPointPlace, PointerKind::Touch),
        "touch may not place vector points");
  check(!pointerMayDrive(CanvasInteraction::PaintBucket, PointerKind::Touch),
        "touch may not fill");
  check(!pointerMayDrive(CanvasInteraction::FlatsFill, PointerKind::Touch),
        "touch may not drive the flats fills");

  // Manipulation is open to touch: moving, transforming and dragging existing
  // handles are exactly what a finger is good at.
  check(pointerMayDrive(CanvasInteraction::MovePixels, PointerKind::Touch),
        "touch may move a layer's pixels");
  check(pointerMayDrive(CanvasInteraction::TransformGizmo, PointerKind::Touch),
        "touch may drive the transform gizmo");
  check(pointerMayDrive(CanvasInteraction::CanvasHandle, PointerKind::Touch),
        "touch may drag an existing canvas handle");
  check(pointerMayDrive(CanvasInteraction::TextFrameMove, PointerKind::Touch),
        "touch may move a text frame");
  check(pointerMayDrive(CanvasInteraction::LayerRowReorder, PointerKind::Touch),
        "touch may reorder layer rows");

  // The phase split, stated as one assertion because it is one idea: the same
  // tool denies the finger while it DEFINES a rect or ramp and admits it
  // afterwards for the handles. A table keyed on the tool could not say this.
  check(!pointerMayDrive(CanvasInteraction::CropDefine, PointerKind::Touch) &&
            !pointerMayDrive(CanvasInteraction::GradientDefine, PointerKind::Touch) &&
            pointerMayDrive(CanvasInteraction::CanvasHandle, PointerKind::Touch),
        "crop/gradient: touch denied while defining, allowed on the handles");

  // A quick tap must not change the colour; touch reaches the eyedropper only
  // through the long press, which does not consult this row.
  check(!pointerMayDrive(CanvasInteraction::EyedropperSample, PointerKind::Touch),
        "a touch TAP does not sample colour");

  // The pan predicate stands down exactly when the tool claims the pointer,
  // which is the generalisation of the transform/pan collision.
  check(toolClaimsSinglePointer(CanvasInteraction::TransformGizmo, PointerKind::Touch),
        "a one-finger transform drag is claimed, so panning stands down");
  check(!toolClaimsSinglePointer(CanvasInteraction::PaintStroke, PointerKind::Touch),
        "a one-finger drag in a paint tool is not claimed by the tool");

  // --- touch grab radius ----------------------------------------------------
  //
  // A fingertip covers the point it is aiming at, so handles sized for a mouse
  // are not merely fiddly under touch -- they are aimed at something the user
  // cannot see.
  check(pointerGrabRadiusPx(PointerKind::Touch) * 2.0f >= 44.0f,
        "grab: a touch target is at least iOS's 44pt minimum, corner to corner");
  check(pointerGrabRadiusPx(PointerKind::Mouse) == kPrecisePointerGrabPx &&
            pointerGrabRadiusPx(PointerKind::Pen) == kPrecisePointerGrabPx,
        "grab: mouse and pen keep their tighter precision");
  check(pointerGrabRadiusPx(PointerKind::Touch) > pointerGrabRadiusPx(PointerKind::Mouse),
        "grab: touch is the widened one, not the other way round");

  // --- the long-press recogniser -------------------------------------------
  //
  // Touch's only route to the eyedropper, and the layer panel's gate for
  // drag-to-reorder. Driven here as a frame sequence, which is the only way to
  // pin the one-shot behaviour and the disqualification rules at all.
  {
    using Phase = LongPressGesture::Phase;
    LongPressGesture g;

    // A clean hold: pending until the threshold, then Fired on exactly one
    // update.
    check(g.update(true, 100.0f, 100.0f, 0.0) == Phase::Pending, "long press: the press edge pends");
    check(g.update(true, 100.0f, 100.0f, kLongPressMs - 1.0) == Phase::Pending,
          "long press: one millisecond short does not fire");
    check(g.update(true, 100.0f, 100.0f, kLongPressMs) == Phase::Fired,
          "long press: fires once the threshold is reached");

    // The expensive bug this prevents: a recogniser that keeps saying yes
    // samples a new colour on every frame of the hold.
    check(g.update(true, 100.0f, 100.0f, kLongPressMs + 100.0) == Phase::Pending &&
              g.update(true, 100.0f, 100.0f, kLongPressMs + 900.0) == Phase::Pending,
          "long press: fires ONCE, not every frame it stays held");

    // The anchor is the press point, not wherever the finger drifted to.
    check(g.anchorX() == 100.0f && g.anchorY() == 100.0f,
          "long press: samples the press point, not the drift");

    // A finger that wanders is a drag, and no amount of time makes a drag into
    // a long press -- otherwise a slow pan would sample a colour partway.
    g.reset();
    g.update(true, 100.0f, 100.0f, 0.0);
    g.update(true, 100.0f + kLongPressSlopPx + 1.0f, 100.0f, 10.0);
    check(g.update(true, 100.0f, 100.0f, 5000.0) == Phase::Pending,
          "long press: a wander past the slop disqualifies the whole hold");

    // ...but jitter inside the slop is still a hold. A finger resting on glass
    // is never perfectly still, so a zero-tolerance rule would recognise
    // nothing on real hardware.
    g.reset();
    g.update(true, 100.0f, 100.0f, 0.0);
    g.update(true, 100.0f + kLongPressSlopPx - 1.0f, 100.0f, 10.0);
    check(g.update(true, 100.0f, 100.0f, kLongPressMs) == Phase::Fired,
          "long press: jitter within the slop still counts as held");

    // Lifting resets, so the next press starts a fresh hold rather than
    // inheriting the last one's clock.
    g.reset();
    g.update(true, 100.0f, 100.0f, 0.0);
    check(g.update(false, 100.0f, 100.0f, 10.0) == Phase::Idle, "long press: lifting returns to idle");
    g.update(true, 400.0f, 400.0f, 20.0);
    check(g.update(true, 400.0f, 400.0f, 20.0 + kLongPressMs - 1.0) == Phase::Pending,
          "long press: a new press restarts the clock, not the old one's");
  }

  return ok;
}

}  // namespace np
