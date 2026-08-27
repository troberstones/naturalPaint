#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "app/TransformSession.hpp"

namespace np {

// app/TransformSession (docs/reachability-audit.md C1; PRD D14, D16, E10).
// Headless and GPU-free -- nothing here touches ui/ or a device.
//
// This section proves the model, not the engine: ops/Transform.hpp and
// ops/DocumentTransform.hpp already have their own --selftest sections for
// the resampler and its bridge. What is new here is everything a UI would
// otherwise have to get right itself -- handle geometry, hit-testing, drag
// math that reads Shift/Option live, and a commit path that resamples
// exactly once and goes through the same undo funnel every other document
// edit does.
bool runTransformSessionTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] transform session: pure geometry and undo-funnel plumbing, no GPU\n");

  // A distinctive, deterministic RGB fill -- app/selftest/DocumentTransform.cpp's
  // own fixture, reused so a misplacement is obvious rather than statistical.
  auto fillRgb = [](TileStore& store, int32_t w, int32_t h) {
    for (int32_t y = 0; y < h; ++y) {
      for (int32_t x = 0; x < w; ++x) {
        const float fx = static_cast<float>(x) / static_cast<float>(w);
        const float fy = static_cast<float>(y) / static_cast<float>(h);
        const float v = 0.5f + 0.25f * std::sin(fx * 18.0f) * std::cos(fy * 14.0f) +
                        0.15f * std::sin((fx + fy) * 31.0f);
        store.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {v, 0.5f * v, 1.0f - v, 1.0f});
      }
    }
  };
  auto makeDoc = [&](int32_t w, int32_t h) {
    OpenDocument od = makeBlankOpenDocument(w, h, WorkingSpace{});
    fillRgb(*od.document.layers[0].rgbTiles, w, h);
    od.recordEdit("fill fixture", EditKind::Content);
    return od;
  };
  auto tilesBitIdentical = [](const TileStore& a, const TileStore& b, const DocumentRegion& r) {
    const TransformImage ia = imageFromTileStore(a, r.x, r.y, r.width, r.height);
    const TransformImage ib = imageFromTileStore(b, r.x, r.y, r.width, r.height);
    return ia.px.size() == ib.px.size() &&
          std::memcmp(ia.px.data(), ib.px.data(), ia.px.size() * sizeof(float)) == 0;
  };

  // --- 1. Handle geometry, as pure functions --------------------------------
  {
    const DocumentRegion bounds{0, 0, 40u, 20u};
    const TransformHandlePositions h = transformHandlePositions(bounds, mat3Identity());
    check(h.topLeft.x == 0.0f && h.topLeft.y == 0.0f && h.bottomRight.x == 40.0f &&
              h.bottomRight.y == 20.0f && h.center.x == 20.0f && h.center.y == 10.0f,
          "handle positions at identity are the source box's own corners and centre");
    check(h.rotate.x == 20.0f && h.rotate.y < 0.0f,
          "the rotate handle sits above top-centre, outside the box, at identity");

    // A caller-supplied matrix moves every handle through it -- nothing here
    // has any geometry of its own beyond `sourceBounds` and `pending`.
    const Mat3 scaled = mat3Multiply(transformTranslate(100.0f, 0.0f), transformScale(2.0f, 2.0f));
    const TransformHandlePositions h2 = transformHandlePositions(bounds, scaled);
    check(h2.topLeft.x == 100.0f && h2.bottomRight.x == 180.0f && h2.bottomRight.y == 40.0f,
          "handle positions follow an arbitrary pending matrix exactly");

    const TransformHandle hitCorner =
        hitTestTransformHandle(h2, bounds, scaled, Point2{180.0f, 40.0f}, 6.0f);
    check(hitCorner == TransformHandle::BottomRight, "hit test finds a handle within its radius");
    const TransformHandle hitCenter =
        hitTestTransformHandle(h2, bounds, scaled, Point2{140.0f, 20.0f}, 6.0f);
    check(hitCenter == TransformHandle::Move, "hit test resolves the box interior to Move");
    const TransformHandle hitNone =
        hitTestTransformHandle(h2, bounds, scaled, Point2{-500.0f, -500.0f}, 6.0f);
    check(hitNone == TransformHandle::None, "hit test misses everything far outside the box");
  }

  // --- 2. Drag semantics: Move, Option-about-centre, Shift aspect lock -----
  {
    const DocumentRegion bounds{0, 0, 40u, 20u};
    const Mat3 identity = mat3Identity();

    // Move: translate by (cur - start), left-multiplied (destination space).
    const Mat3 moved =
        computeTransformDragMatrix(TransformHandle::Move, bounds, identity, Point2{5.0f, 5.0f},
                                   Point2{15.0f, 1.0f}, false, false);
    const Point2 movedOrigin = mat3MapPoint(moved, Point2{0.0f, 0.0f});
    check(movedOrigin.x == 10.0f && movedOrigin.y == -4.0f,
          "Move drags by exactly (cur - start), regardless of source bounds");

    // BottomRight corner, no modifiers: independent per-axis scale from the
    // OPPOSITE corner (TopLeft).
    const Mat3 corner = computeTransformDragMatrix(TransformHandle::BottomRight, bounds, identity,
                                                   Point2{40.0f, 20.0f}, Point2{80.0f, 30.0f},
                                                   false, false);
    const Point2 br = mat3MapPoint(corner, Point2{40.0f, 20.0f});
    const Point2 tl = mat3MapPoint(corner, Point2{0.0f, 0.0f});
    check(tl.x == 0.0f && tl.y == 0.0f, "an un-anchored corner drag leaves the OPPOSITE corner fixed");
    check(br.x == 80.0f && br.y == 30.0f, "and lands the dragged corner exactly where the cursor is");

    // The same drag with Option held: anchor moves to the box's own centre.
    const Mat3 fromCentre = computeTransformDragMatrix(TransformHandle::BottomRight, bounds,
                                                        identity, Point2{40.0f, 20.0f},
                                                        Point2{80.0f, 30.0f}, false, true);
    const Point2 centreAfter = mat3MapPoint(fromCentre, Point2{20.0f, 10.0f});
    check(std::fabs(centreAfter.x - 20.0f) < 1e-4f && std::fabs(centreAfter.y - 10.0f) < 1e-4f,
          "Option holds the box's OWN centre fixed instead of the opposite corner");
    const Point2 oppositeAfter = mat3MapPoint(fromCentre, Point2{0.0f, 0.0f});
    check(std::fabs(oppositeAfter.x - (-40.0f)) < 1e-3f,
          "and the un-dragged corner now moves too, symmetrically about that centre");

    // Shift on an EDGE handle (only one axis normally active) ties the other
    // axis to it -- the aaspect-lock case app/SelectionDrag's own convention
    // predicts nothing about, decided in this file's header section 6.
    const Mat3 edgeNoShift = computeTransformDragMatrix(TransformHandle::BottomCenter, bounds,
                                                        identity, Point2{20.0f, 20.0f},
                                                        Point2{20.0f, 40.0f}, false, false);
    const Point2 rightNoShift = mat3MapPoint(edgeNoShift, Point2{40.0f, 20.0f});
    check(std::fabs(rightNoShift.x - 40.0f) < 1e-4f,
          "an edge handle alone scales ONE axis -- the other is untouched");
    const Mat3 edgeShift = computeTransformDragMatrix(TransformHandle::BottomCenter, bounds,
                                                       identity, Point2{20.0f, 20.0f},
                                                       Point2{20.0f, 40.0f}, true, false);
    const Point2 rightShift = mat3MapPoint(edgeShift, Point2{40.0f, 20.0f});
    // X was never given an anchor by this handle (only Y was, at the OPPOSITE
    // edge) -- ties it to Y's 2x factor, but about the box's own vertical
    // CENTRELINE (x=20), not the left edge: 20 + 2*(40-20) = 60. Left edge
    // moves the same distance the other way (0 -> -20), so the box grows
    // symmetrically about its centre on the axis Shift newly activated, while
    // Y keeps anchoring at the top edge exactly as it did without Shift.
    check(std::fabs(rightShift.x - 60.0f) < 1e-3f,
          "and Shift on that SAME edge handle ties X to Y's 2x factor too, anchored at the box's "
          "own centreline since X had no edge of its own to anchor from");
  }

  // --- 3. A realistic multi-frame drag: Shift pressed mid-drag, read live --
  //
  // The app/SelectionDrag.hpp lesson: a test that varies the offset while the
  // cursor holds still passes a formula the real call site cannot produce.
  // This drives the loop a real UI runs -- beginDrag, several live-cursor
  // frames, a modifier toggled mid-drag, release -- against ONE fixed
  // {handle, baseMatrix, startCursor} triple, exactly like
  // app/SelectionDrag.cpp's own `computeSelectionDragBox()`.
  {
    TransformSession ts;
    OpenDocument fixture = makeDoc(40, 20);
    check(ts.beginLayer(fixture.document, 0).ok, "multi-frame drag: begin succeeds on a fresh layer");

    const Point2 start{40.0f, 20.0f};  // the BottomRight handle's own position
    ts.beginDrag(TransformHandle::BottomRight, start);

    // Frame 1: no modifier. sx=2, sy=1.5 from this cursor.
    ts.updateDrag(Point2{80.0f, 30.0f}, /*shift=*/false, /*option=*/false);
    const Point2 f1 = mat3MapPoint(ts.pending(), Point2{40.0f, 20.0f});
    check(std::fabs(f1.x - 80.0f) < 1e-3f && std::fabs(f1.y - 30.0f) < 1e-3f,
          "frame 1 (no modifier): both axes move independently");

    // Frame 2: cursor holds still, Shift comes down. The SAME cursor now
    // reads differently because the modifier is live, not latched.
    ts.updateDrag(Point2{80.0f, 30.0f}, /*shift=*/true, /*option=*/false);
    const Point2 f2 = mat3MapPoint(ts.pending(), Point2{40.0f, 20.0f});
    check(std::fabs(f2.x - 80.0f) < 1e-3f && std::fabs(f2.y - 40.0f) < 1e-3f,
          "frame 2 (Shift pressed, cursor unchanged): Y snaps to X's 2x factor");

    // Frame 3: cursor keeps moving, Shift still down.
    ts.updateDrag(Point2{120.0f, 30.0f}, /*shift=*/true, /*option=*/false);
    const Point2 f3 = mat3MapPoint(ts.pending(), Point2{40.0f, 20.0f});
    check(std::fabs(f3.x - 120.0f) < 1e-3f && std::fabs(f3.y - 60.0f) < 1e-3f,
          "frame 3 (Shift held, cursor moves further): the lock keeps tracking, not freezing");

    // Frame 4: Shift released, cursor returns to frame 1's exact position.
    // Recomputed fresh from {start, base} -- NOT accumulated -- so this must
    // reproduce frame 1's result exactly, bit for bit.
    ts.updateDrag(Point2{80.0f, 30.0f}, /*shift=*/false, /*option=*/false);
    const Point2 f4 = mat3MapPoint(ts.pending(), Point2{40.0f, 20.0f});
    check(f4.x == f1.x && f4.y == f1.y,
          "frame 4 (Shift released, cursor back to frame 1): reproduces frame 1 EXACTLY -- proof "
          "there is no accumulated drift across frames, only a fresh recompute each time");

    ts.endDrag();
    ts.cancel();  // nothing was ever written to `fixture` -- see section 7.
  }

  // --- 4. SABOTAGE PROOF 1: accumulation order (rotate-then-scale) ---------
  //
  // Section 6's rule: Move/Rotate are left-multiplied onto the base matrix
  // (applied AFTER it, in destination space); scale is right-multiplied
  // (applied BEFORE it, in source space). Swapping either multiply's operand
  // order changes where a rotated-then-scaled box's corners land, and this
  // asserts it does NOT change for the shipped code, then (recorded in this
  // file's own report, not asserted by the binary) is broken and confirmed
  // to fail.
  {
    TransformSession ts;
    OpenDocument fixture = makeDoc(40, 20);
    check(ts.beginLayer(fixture.document, 0).ok, "accumulation: begin succeeds");

    // Drag 1: rotate the box roughly -90 degrees about its own centre (20,10).
    // The exact angle does not matter -- what matters is that it is a real
    // rotation, so it does not commute with the non-uniform scale below.
    ts.beginDrag(TransformHandle::Rotate, Point2{40.0f, 10.0f});  // centre + (20, 0)
    ts.updateDrag(Point2{20.0f, -10.0f}, false, false);           // centre + (0, -20)
    ts.endDrag();
    const Mat3 r = ts.pending();

    // Drag 2: scale the BottomRight handle (now sitting wherever `r` put it)
    // so that, in the box's own LOCAL frame, sx=2 and sy=1 exactly -- chosen
    // by mapping the intended local start/target points THROUGH `r`, so the
    // cursor positions this drag is fed are the ones a real drag on the
    // already-rotated handle would produce.
    const Point2 startCursor = mat3MapPoint(r, Point2{40.0f, 20.0f});   // BR, local
    const Point2 curCursor = mat3MapPoint(r, Point2{80.0f, 20.0f});     // BR moved to sx=2 in local space
    ts.beginDrag(TransformHandle::BottomRight, startCursor);
    ts.updateDrag(curCursor, false, false);
    ts.endDrag();
    const Mat3 rs = ts.pending();

    const Mat3 s = transformScaleAbout(2.0f, 1.0f, Point2{0.0f, 0.0f});  // hand-known, exact
    const Mat3 expectedRightOrder = mat3Multiply(r, s);  // r * s: scale first (source space), rotate after
    const Mat3 wrongOrder = mat3Multiply(s, r);          // s * r: the swapped-operand bug

    // Compare by mapping a probe point that the two orders send somewhere
    // very different (a corner far from every pivot used above).
    const Point2 probe{40.0f, 0.0f};
    const Point2 got = mat3MapPoint(rs, probe);
    const Point2 right = mat3MapPoint(expectedRightOrder, probe);
    const Point2 wrong = mat3MapPoint(wrongOrder, probe);
    const float errRight = std::hypot(got.x - right.x, got.y - right.y);
    const float errWrong = std::hypot(got.x - wrong.x, got.y - wrong.y);

    // 0.1 units separates float rounding (the rotate handle's angle comes
    // from atan2 of exact inputs, so its own error is on the order of 1e-5
    // degrees; propagated through one more matrix multiply and a point map
    // over an 80-unit span this is many orders of magnitude under 0.1) from
    // a genuinely wrong composition order, which -- see errWrong below --
    // lands multiple units away for this geometry. It is not a resampling
    // tolerance; ops/Transform.hpp's own kernel-error measurements are
    // elsewhere and this is a pure-geometry comparison.
    check(errRight < 0.1f,
          "SABOTAGE 1: rotate-then-scale composes as rotate * scale (matches within float rounding)");
    check(errWrong > 5.0f,
          "SABOTAGE 1: the swapped order really does land somewhere else -- the fixture "
          "discriminates the two orders rather than coincidentally agreeing");
    ts.cancel();
  }

  // --- 5. SABOTAGE PROOF 2: commit() resamples the SOURCE exactly once ----
  //
  // `updateDrag()` cannot resample anything even if sabotaged to try --
  // its signature is `(Point2, bool, bool) -> void`; it never sees a
  // TileStore. What CAN regress is `commit()` itself applying the transform
  // more than once (a duplicated call, a loop over drag history). This
  // proves the actual guarantee: a session driven through several drag
  // frames and then committed produces BIT-IDENTICAL pixels to a single,
  // direct `transformLayer()` call using the session's own final matrix on
  // an identically-fixtured document -- which is only true if the session's
  // commit reads the source exactly once, with the final composed matrix,
  // and not once per frame or twice for any other reason.
  {
    TransformSession ts;
    OpenDocument driven = makeDoc(64, 48);
    check(ts.beginLayer(driven.document, 0).ok, "single-resample: begin succeeds");

    // Several frames of a real drag -- not a single call -- so a per-frame
    // resample (were one accidentally wired in) would have multiple chances
    // to show up as extra blur against the single-shot reference below.
    ts.beginDrag(TransformHandle::Rotate, Point2{72.0f, 24.0f});  // centre(32,24) + (40,0)
    ts.updateDrag(Point2{60.0f, 5.0f}, false, false);
    ts.updateDrag(Point2{45.0f, -2.0f}, false, false);
    ts.updateDrag(Point2{20.0f, 3.0f}, false, false);
    ts.updateDrag(Point2{10.0f, 20.0f}, false, false);
    ts.endDrag();
    const Mat3 finalPending = ts.pending();

    const TransformCommitResult commitResult = ts.commit(driven);
    check(commitResult.ok, "single-resample: commit succeeds");

    OpenDocument reference = makeDoc(64, 48);  // byte-identical starting fixture
    const LayerTransformResult refResult =
        transformLayer(reference.document, 0, finalPending, DocumentTransformParams{});
    check(refResult.ok, "single-resample: the single-shot reference transform succeeds");

    const DocumentRegion cmpRegion = rgbContentRegion(*reference.document.layers[0].rgbTiles);
    check(!cmpRegion.empty() &&
              tilesBitIdentical(*driven.document.layers[0].rgbTiles,
                                *reference.document.layers[0].rgbTiles, cmpRegion),
          "SABOTAGE 2: a multi-frame-driven commit is BIT-IDENTICAL to one direct transformLayer() "
          "call with the final matrix -- proof commit() resampled the source exactly once, not "
          "once per drag frame and not twice");
  }

  // --- 6. SABOTAGE PROOF 3: exactRemapKind() is honoured through a drag ---
  {
    TransformSession ts;
    OpenDocument fixture = makeDoc(50, 30);
    check(ts.beginLayer(fixture.document, 0).ok, "exact path: begin succeeds");

    // An integer-pixel Move: this is PRD D15's no-resample path, reached
    // through the session exactly as it is reached anywhere else --
    // exactRemapKind() classifies the composed matrix, not the gesture that
    // built it.
    ts.beginDrag(TransformHandle::Move, Point2{0.0f, 0.0f});
    ts.updateDrag(Point2{7.0f, -3.0f}, false, false);
    ts.endDrag();
    check(ts.pendingExactRemap() == ExactRemap::Identity,
          "an integer-pixel Move classifies as the exact (Identity-permutation) path BEFORE commit");

    const DocumentRegion before = rgbContentRegion(*fixture.document.layers[0].rgbTiles);
    TransformImage originalPixels =
        imageFromTileStore(*fixture.document.layers[0].rgbTiles, before.x, before.y, before.width,
                           before.height);

    const TransformCommitResult r = ts.commit(fixture);
    check(r.ok && r.exact == ExactRemap::Identity && r.reconstructionPasses == 0,
          "SABOTAGE 3: commit reports the exact path and ZERO reconstruction passes for an "
          "integer-pixel Move");

    const DocumentRegion after = rgbContentRegion(*fixture.document.layers[0].rgbTiles);
    const TransformImage movedPixels = imageFromTileStore(*fixture.document.layers[0].rgbTiles,
                                                           after.x, after.y, after.width, after.height);
    check(after.x == before.x + 7 && after.y == before.y - 3,
          "and the content region landed exactly at the requested integer offset");
    check(originalPixels.px.size() == movedPixels.px.size() &&
              std::memcmp(originalPixels.px.data(), movedPixels.px.data(),
                          originalPixels.px.size() * sizeof(float)) == 0,
          "SABOTAGE 3: and the moved pixels are BIT-IDENTICAL to the originals -- no reconstruction "
          "kernel touched a single value");
  }

  // --- 7. Cancel leaves nothing behind -------------------------------------
  {
    TransformSession ts;
    OpenDocument fixture = makeDoc(20, 20);
    const uint64_t revBefore = fixture.revision;
    const size_t cursorBefore = fixture.history.cursor();
    check(ts.beginLayer(fixture.document, 0).ok, "cancel: begin succeeds");
    ts.beginDrag(TransformHandle::BottomRight, Point2{20.0f, 20.0f});
    ts.updateDrag(Point2{60.0f, 60.0f}, false, false);
    ts.endDrag();
    check(ts.active(), "cancel: the session is active with a real pending transform before cancel");
    ts.cancel();
    check(!ts.active(), "cancel: the session reports inactive afterwards");
    check(fixture.revision == revBefore && fixture.history.cursor() == cursorBefore,
          "cancel: the document's revision and history are untouched -- nothing was ever written, "
          "so there is nothing to restore");
  }

  // --- 8. Commit records exactly one undoable structural edit --------------
  {
    TransformSession ts;
    OpenDocument fixture = makeDoc(20, 20);
    const uint64_t revBefore = fixture.revision;
    const size_t cursorBefore = fixture.history.cursor();
    ts.beginLayer(fixture.document, 0);
    ts.beginDrag(TransformHandle::Move, Point2{0.0f, 0.0f});
    ts.updateDrag(Point2{3.0f, 3.0f}, false, false);
    ts.endDrag();
    const TransformCommitResult r = ts.commit(fixture);
    check(r.ok && !r.editLabel.empty(), "commit: succeeds and names the edit");
    check(fixture.revision == revBefore + 1, "commit: bumps the revision exactly once");
    check(fixture.history.cursor() == cursorBefore + 1,
          "commit: appends exactly ONE history entry, so undo takes the whole transform back in "
          "one step");
    check(!ts.active(), "commit: the session is no longer active afterwards");
  }

  // --- 9. An identity commit is a no-op: nothing written, nothing recorded -
  {
    TransformSession ts;
    OpenDocument fixture = makeDoc(20, 20);
    const uint64_t revBefore = fixture.revision;
    const size_t cursorBefore = fixture.history.cursor();
    ts.beginLayer(fixture.document, 0);
    // No drag at all: pending() is still the identity beginLayer() set it to.
    const TransformCommitResult r = ts.commit(fixture);
    check(r.ok && r.exact == ExactRemap::Identity,
          "identity commit: reports ok and Identity without touching a store");
    check(fixture.revision == revBefore && fixture.history.cursor() == cursorBefore,
          "identity commit: a transform tool opened and released with no edit records NOTHING -- "
          "an undo step for a no-op gesture would be noise");
  }

  // --- 10. Begin refusals, each with the reason a UI would surface --------
  {
    OpenDocument fixture = makeDoc(20, 20);
    TransformSession ts;

    TransformBeginResult outOfRange = ts.beginLayer(fixture.document, 5);
    check(!outOfRange.ok && outOfRange.error.find("out of range") != std::string::npos,
          "beginLayer refuses an out-of-range index, by name");

    fixture.document.layers[0].locked = true;
    TransformBeginResult locked = ts.beginLayer(fixture.document, 0);
    check(!locked.ok && locked.error.find("locked") != std::string::npos,
          "beginLayer refuses a locked layer, by name");
    fixture.document.layers[0].locked = false;

    Layer adjustment;
    adjustment.kind = LayerKind::Adjustment;
    fixture.document.layers.push_back(adjustment);
    TransformBeginResult noPixels = ts.beginLayer(fixture.document, 1);
    check(!noPixels.ok && noPixels.error.find("Adjustment") != std::string::npos,
          "beginLayer refuses a layer kind that holds no pixels, naming the kind");

    Layer emptyRgb;
    emptyRgb.kind = LayerKind::RGB;
    emptyRgb.rgbTiles.emplace();  // engaged, but genuinely empty
    fixture.document.layers.push_back(emptyRgb);
    TransformBeginResult empty = ts.beginLayer(fixture.document, 2);
    check(!empty.ok && empty.error.find("no content") != std::string::npos,
          "beginLayer refuses an engaged-but-empty layer, distinctly from 'no pixel storage'");

    // Selection-pixels: an empty selection, and a Pigment layer refused BY
    // NAME while the SAME layer is transformable as a whole (section 4).
    Selection nothing = selectRectangle(0.0f, 0.0f, 0.0f, 0.0f);
    TransformBeginResult emptySel = ts.beginSelectionPixels(fixture.document, nothing, 0);
    check(!emptySel.ok && emptySel.error.find("no pixels") != std::string::npos,
          "beginSelectionPixels refuses a selection that covers nothing");

    Layer pigment;
    pigment.kind = LayerKind::Pigment;
    pigment.pigmentTiles.emplace();
    pigment.pigmentTiles->getOrCreate(TileCoord{0, 0})
        .writeTexel(PixelCoord{5, 5}, PigmentTexel{Latent{{0.2f, 0.2f, 0.2f}, {}}, 1.0f});
    fixture.document.layers.push_back(pigment);
    const size_t pigmentIdx = fixture.document.layers.size() - 1;
    Selection overPigment = selectRectangle(0.0f, 0.0f, 20.0f, 20.0f);
    TransformBeginResult pigSel = ts.beginSelectionPixels(fixture.document, overPigment, pigmentIdx);
    check(!pigSel.ok && pigSel.error.find("Pigment") != std::string::npos &&
              pigSel.error.find("Kubelka-Munk") != std::string::npos,
          "beginSelectionPixels refuses a Pigment layer BY NAME, with the reason (section 4)");
    TransformBeginResult pigWhole = ts.beginLayer(fixture.document, pigmentIdx);
    check(pigWhole.ok,
          "and beginLayer accepts that SAME Pigment layer for a whole-layer transform -- the "
          "engine's own mass-weighted path, inherited rather than refused");
    ts.cancel();
  }

  // --- 11. Selection-pixels commit: the selection moves WITH the pixels ---
  {
    // Two distinct flat-colour blocks side by side, so "did the right pixels
    // move, and did the vacated spot really clear" is a colour comparison,
    // not a statistical one.
    OpenDocument od = makeBlankOpenDocument(60, 30, WorkingSpace{});
    TileStore& tiles = *od.document.layers[0].rgbTiles;
    for (int32_t y = 0; y < 30; ++y) {
      for (int32_t x = 0; x < 20; ++x)
        tiles.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {1.0f, 0.0f, 0.0f, 1.0f});  // red block
      for (int32_t x = 40; x < 60; ++x)
        tiles.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {0.0f, 0.0f, 1.0f, 1.0f});  // blue block
    }
    od.recordEdit("fill fixture", EditKind::Content);
    const uint64_t revBefore = od.revision;

    Selection redBlock = selectRectangle(0.0f, 0.0f, 20.0f, 30.0f);
    od.selection = redBlock;

    TransformSession ts;
    check(ts.beginSelectionPixels(od.document, redBlock, 0).ok,
          "selection-pixels: begin succeeds over the red block");

    // Integer-pixel Move by (20, 0): red block's new home is [20,40)x[0,30)
    // -- the exact gap between its own old spot and the blue block (which
    // starts at x=40) -- so a bug that let the moved content clobber the
    // blue block would show up as a colour, not a coordinate typo to notice.
    ts.beginDrag(TransformHandle::Move, Point2{0.0f, 0.0f});
    ts.updateDrag(Point2{20.0f, 0.0f}, false, false);
    ts.endDrag();
    const TransformCommitResult r = ts.commit(od);
    check(r.ok, "selection-pixels: commit succeeds");
    check(od.revision == revBefore + 1, "selection-pixels: bumps the revision exactly once");

    const TransformImage vacated =
        imageFromTileStore(*od.document.layers[0].rgbTiles, 0, 0, 20, 30);
    bool vacatedClear = true;
    for (size_t i = 0; i + 3 < vacated.px.size(); i += 4)
      if (vacated.px[i + 3] != 0.0f) vacatedClear = false;
    check(vacatedClear,
          "the red block's OLD location is fully cleared -- cutThroughSelection()'s coverage-"
          "weighted erase, not left as a ghost");

    const TransformImage movedTo = imageFromTileStore(*od.document.layers[0].rgbTiles, 20, 0, 20, 30);
    bool movedIsRed = true;
    for (size_t i = 0; i + 3 < movedTo.px.size(); i += 4)
      if (movedTo.px[i] != 1.0f || movedTo.px[i + 1] != 0.0f || movedTo.px[i + 2] != 0.0f ||
          movedTo.px[i + 3] != 1.0f)
        movedIsRed = false;
    check(movedIsRed, "and the red block reappears, bit-exact, at the destination (integer "
                      "translate, exact path)");

    const TransformImage untouchedBlue =
        imageFromTileStore(*od.document.layers[0].rgbTiles, 40, 0, 20, 30);
    bool blueIntact = true;
    for (size_t i = 0; i + 3 < untouchedBlue.px.size(); i += 4)
      if (untouchedBlue.px[i] != 0.0f || untouchedBlue.px[i + 2] != 1.0f) blueIntact = false;
    check(blueIntact, "the blue block, outside the selection, is untouched");

    check(od.selection.has_value(), "SECTION 3: the active selection survives the commit");
    if (od.selection.has_value()) {
      const DocumentRegion movedSel = selectionContentRegion(*od.selection);
      check(movedSel.x == 20 && movedSel.width == 20u,
            "SECTION 3: and it moved WITH the pixels -- the marquee now bounds the block's NEW "
            "location, not its old one");
    }
  }

  return ok;
}

}  // namespace np
