#include "app/selftest/Support.hpp"

#include <cstring>

#include "app/MoveTool.hpp"
#include "app/StrokeSession.hpp"  // the four gates Move must NOT be answered by
#include "app/ZoomAndSize.hpp"    // toolZoomsView(), the fifth
#include "ui/AtelierChrome.hpp"

namespace np {

// app/MoveTool -- `Tool::Move`, the palette's `V` cell.
//
// What this section proves is deliberately NOT the resampler: ops/Transform,
// ops/DocumentTransform and app/TransformSession each have their own
// sections, and the Move tool's whole design premise is that it adds no
// second translation path to disagree with them (app/MoveTool.hpp section 1).
// What is new, and therefore what is asserted here, is the four decisions
// that header records -- the target rule, the refusal being prose rather than
// silence, the nudge, and the seventh canvas gate -- plus the property those
// decisions exist to protect: **an integer move is lossless, and a move that
// was not committed changed nothing at all.**
//
// Headless and GPU-free: nothing below touches a device, and the tool's UI
// half (ui/MacPaintUI.cpp's two delimited Move blocks) is out of reach here
// for the same reason every other canvas block is -- which is exactly why the
// decisions were lifted into app/MoveTool in the first place.
bool runMoveToolTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] move tool: target rule, lossless integer moves, refusals, nudge\n");

  // A deterministic, high-frequency fill: every texel differs from its
  // neighbours, so "the pixels came back identical" is a real claim rather
  // than one a flat fill would satisfy by accident.
  auto fillRgb = [](TileStore& store, int32_t x0, int32_t y0, int32_t w, int32_t h) {
    for (int32_t y = y0; y < y0 + h; ++y) {
      for (int32_t x = x0; x < x0 + w; ++x) {
        const float fx = static_cast<float>(x - x0) / static_cast<float>(w);
        const float fy = static_cast<float>(y - y0) / static_cast<float>(h);
        const float v = 0.5f + 0.25f * std::sin(fx * 23.0f) * std::cos(fy * 17.0f) +
                        0.15f * std::sin((fx + fy) * 37.0f);
        store.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {v, 0.5f * v, 1.0f - v, 1.0f});
      }
    }
  };
  // The whole canvas, read straight out of the layer's store, as raw floats.
  // `imageFromTileStore()` answers transparent black for an absent tile, so
  // this covers "content moved" and "content vanished" with one comparison.
  auto snapshot = [](const OpenDocument& od, int32_t w, int32_t h) {
    return imageFromTileStore(*od.document.layers[0].rgbTiles, 0, 0, static_cast<uint32_t>(w),
                              static_cast<uint32_t>(h));
  };
  auto samePixels = [](const TransformImage& a, const TransformImage& b) {
    return a.px.size() == b.px.size() &&
           std::memcmp(a.px.data(), b.px.data(), a.px.size() * sizeof(float)) == 0;
  };

  // --- 1. The seventh gate (app/MoveTool.hpp section 5) --------------------
  //
  // The palette's `implemented` flag and the canvas's handler have to agree,
  // and `runEyedropperTest()` asserts that for every tool. What is asserted
  // HERE is the half that assertion cannot see: that Move is handled through
  // its OWN predicate and not by widening somebody else's -- the same shape
  // of claim the Zoom row in that file makes, for the same reason.
  {
    check(toolMovesPixels(Tool::Move), "gate: toolMovesPixels() is true for Tool::Move");
    bool othersFalse = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      if (t != Tool::Move && toolMovesPixels(t)) othersFalse = false;
    }
    check(othersFalse,
          "gate: and false for every other tool -- Move is the only one that repositions "
          "existing pixels by dragging them");
    check(toolImplemented(Tool::Move) && toolHasCanvasHandler(Tool::Move) &&
              toolNoHandlerException(Tool::Move) == nullptr,
          "gate: Move is implemented, has a canvas handler and needs no recorded exception");
    check(!toolWritesRgbPixels(Tool::Move) && !toolPansView(Tool::Move) &&
              !toolDrawsSelection(Tool::Move) && !toolSamplesCanvas(Tool::Move) &&
              !toolZoomsView(Tool::Move) && !toolBeginsStroke(Tool::Move),
          "gate: and through NO other gate -- the assertion that catches Move being wired by "
          "widening the bucket's or the hand's predicate instead of adding the seventh");
  }

  // --- 2. The target rule (app/MoveTool.hpp section 2) ---------------------
  {
    OpenDocument od = makeBlankOpenDocument(64, 48, WorkingSpace{});
    fillRgb(*od.document.layers[0].rgbTiles, 8, 8, 24, 24);
    od.recordEdit("fill fixture", EditKind::Content);

    check(moveTargetFor(od) == MoveTarget::WholeLayer,
          "target: with no selection a Move acts on the whole layer");
    TransformSession whole;
    check(beginMove(whole, od).ok && whole.target() == TransformTarget::Layer,
          "target: and beginMove() really opens a whole-layer session, not just says so");
    whole.cancel();

    // Deliberately a SMALLER box than the layer's content bounds (which are
    // (8,8)+24x24 from the fill above), so "which region did the session
    // take" has two different answers to choose between and the assertion
    // below can only pass for one of them.
    od.selection = selectRectangle(12.0f, 12.0f, 28.0f, 28.0f);
    check(moveTargetFor(od) == MoveTarget::SelectionPixels,
          "target: with a selection a Move acts on the selected pixels");
    TransformSession sel;
    check(beginMove(sel, od).ok && sel.target() == TransformTarget::SelectionPixels,
          "target: and beginMove() opens a selection-pixels session for it");
    check(sel.sourceBounds().x == 12 && sel.sourceBounds().width == 16u,
          "target: whose source bounds are the SELECTION's region (12,+16), not the layer's "
          "content bounds (8,+24) -- which one was used is a fact, not an inference");
    sel.cancel();
  }

  // --- 3. There and back again: an integer move is BIT-identical -----------
  //
  // The property the whole design exists for. A move of (dx, dy) followed by
  // a move of (-dx, -dy) must return the layer to exactly the bits it
  // started with -- not "visually the same", not "within a tolerance".
  // `memcmp` over the whole canvas is the only comparison that can say that,
  // and a path that resampled through Catmull-Rom would fail it on the first
  // texel of the first pass. Both commits are additionally required to report
  // `reconstructionPasses == 0`, so a future change that got the pixels right
  // by luck (a kernel whose weights happen to sum to a delta) still reddens.
  {
    OpenDocument od = makeBlankOpenDocument(96, 64, WorkingSpace{});
    fillRgb(*od.document.layers[0].rgbTiles, 20, 12, 40, 30);
    od.recordEdit("fill fixture", EditKind::Content);
    const TransformImage before = snapshot(od, 96, 64);

    TransformSession out;
    check(beginMove(out, od).ok, "round trip: the outward move begins");
    setMoveTranslation(out, 17.0f, -9.0f);
    check(out.pendingExactRemap() == ExactRemap::Identity,
          "round trip: a whole-pixel drag classifies as PRD D15's exact path BEFORE it commits, "
          "so a UI can promise losslessness while the pointer is still down");
    const TransformCommitResult r1 = out.commit(od);
    check(r1.ok && r1.exact == ExactRemap::Identity && r1.reconstructionPasses == 0,
          "round trip: and the outward commit really took it -- ZERO reconstruction passes");

    const TransformImage moved = snapshot(od, 96, 64);
    check(!samePixels(before, moved),
          "round trip: the outward move actually moved something -- the guard against a "
          "'lossless' claim that is only true because nothing happened");

    TransformSession back;
    check(beginMove(back, od).ok, "round trip: the return move begins");
    setMoveTranslation(back, -17.0f, 9.0f);
    const TransformCommitResult r2 = back.commit(od);
    check(r2.ok && r2.exact == ExactRemap::Identity && r2.reconstructionPasses == 0,
          "round trip: and the return commit takes the exact path too");
    check(samePixels(before, snapshot(od, 96, 64)),
          "round trip: (dx,dy) then (-dx,-dy) leaves the layer BIT-IDENTICAL -- an integer "
          "move must be exactly lossless, or dragging a layer around costs picture quality");
  }

  // --- 4. A selection move leaves everything outside the selection alone ---
  //
  // Two flat blocks with a gap between them: the red one is selected and
  // moved into the gap, so a bug that moved the whole layer, or that let the
  // resample's footprint spill past the selection, shows up as the blue block
  // having changed -- a comparison of bits, not of a coordinate.
  {
    OpenDocument od = makeBlankOpenDocument(80, 32, WorkingSpace{});
    TileStore& tiles = *od.document.layers[0].rgbTiles;
    for (int32_t y = 0; y < 32; ++y) {
      for (int32_t x = 0; x < 20; ++x)
        tiles.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {1.0f, 0.0f, 0.0f, 1.0f});
      for (int32_t x = 56; x < 76; ++x)
        tiles.getOrCreate(tileCoordAt(PixelCoord{x, y}))
            .writePixel(tileLocalOffset(PixelCoord{x, y}), {0.0f, 0.0f, 1.0f, 1.0f});
    }
    od.recordEdit("fill fixture", EditKind::Content);
    const TransformImage blueBefore =
        imageFromTileStore(*od.document.layers[0].rgbTiles, 56, 0, 20u, 32u);

    od.selection = selectRectangle(0.0f, 0.0f, 20.0f, 32.0f);
    TransformSession sel;
    check(beginMove(sel, od).ok, "selection move: begins over the red block");
    setMoveTranslation(sel, 24.0f, 0.0f);
    check(sel.commit(od).ok, "selection move: commits");

    const TransformImage arrived =
        imageFromTileStore(*od.document.layers[0].rgbTiles, 24, 0, 20u, 32u);
    bool arrivedIsRed = true;
    for (size_t i = 0; i + 3 < arrived.px.size(); i += 4)
      if (arrived.px[i] != 1.0f || arrived.px[i + 1] != 0.0f || arrived.px[i + 2] != 0.0f ||
          arrived.px[i + 3] != 1.0f)
        arrivedIsRed = false;
    check(arrivedIsRed, "selection move: the selected block arrives, bit-exact, 24 px to the right");

    const TransformImage blueAfter =
        imageFromTileStore(*od.document.layers[0].rgbTiles, 56, 0, 20u, 32u);
    check(samePixels(blueBefore, blueAfter),
          "selection move: and every texel OUTSIDE the selection is untouched -- the whole "
          "point of a selection scoping the move");
  }

  // --- 5. A cancelled move writes nothing at all ---------------------------
  //
  // app/TransformSession's "cancel needs no restore step" seen from the Move
  // tool's side: the drag path composes a matrix and never reads or writes a
  // texel, so an abandoned gesture is not undone -- it never happened. Three
  // separate witnesses, because a write can hide in any one of them: the
  // pixels, the revision the composite caches by, and the history the user
  // would otherwise find an entry in.
  {
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{});
    fillRgb(*od.document.layers[0].rgbTiles, 10, 10, 30, 30);
    od.recordEdit("fill fixture", EditKind::Content);
    const TransformImage before = snapshot(od, 64, 64);
    const uint64_t revBefore = od.revision;
    const size_t cursorBefore = od.history.cursor();

    TransformSession ts;
    check(beginMove(ts, od).ok, "cancel: the move begins");
    setMoveTranslation(ts, 25.0f, -13.0f);
    check(ts.active(), "cancel: and carries a real pending translation before it is abandoned");
    ts.cancel();
    check(!ts.active(), "cancel: the session is inactive afterwards");
    check(od.revision == revBefore && od.history.cursor() == cursorBefore &&
              samePixels(before, snapshot(od, 64, 64)),
          "cancel: a cancelled move writes NOTHING -- same pixels, same revision, no history "
          "entry; there was never anything to unwind");
  }

  // --- 6. A zero-distance move is not an edit ------------------------------
  //
  // The pen-down-pen-up with no travel that every pointing device produces by
  // accident. It must not cost a history entry, or the panel PRD O2 says is
  // scanned to find an edit to undo fills with rows that did nothing.
  {
    OpenDocument od = makeBlankOpenDocument(32, 32, WorkingSpace{});
    fillRgb(*od.document.layers[0].rgbTiles, 4, 4, 20, 20);
    od.recordEdit("fill fixture", EditKind::Content);
    const uint64_t revBefore = od.revision;
    const size_t cursorBefore = od.history.cursor();

    const TransformCommitResult r = nudgeMove(od, 0.0f, 0.0f);
    check(r.ok, "no-op move: a zero-distance move succeeds rather than erroring");
    check(od.revision == revBefore && od.history.cursor() == cursorBefore,
          "no-op move: and records NOTHING -- no revision bump, no history entry for a "
          "gesture that moved the picture zero pixels");
  }

  // --- 7. The nudge (app/MoveTool.hpp section 4) ---------------------------
  {
    OpenDocument od = makeBlankOpenDocument(64, 48, WorkingSpace{});
    fillRgb(*od.document.layers[0].rgbTiles, 20, 15, 16, 12);
    od.recordEdit("fill fixture", EditKind::Content);
    const DocumentRegion before = rgbContentRegion(*od.document.layers[0].rgbTiles);
    const TransformImage pixelsBefore = snapshot(od, 64, 48);
    const size_t cursorBefore = od.history.cursor();

    const TransformCommitResult r = nudgeMove(od, 3.0f, -2.0f);
    check(r.ok && r.exact == ExactRemap::Identity && r.reconstructionPasses == 0,
          "nudge: an arrow-key nudge is always on the exact path -- whole pixels, no kernel");
    const DocumentRegion after = rgbContentRegion(*od.document.layers[0].rgbTiles);
    check(after.x == before.x + 3 && after.y == before.y - 2,
          "nudge: and lands the content exactly (+3, -2) from where it was -- the axes are "
          "not swapped and the sign is not flipped");
    check(od.history.cursor() == cursorBefore + 1,
          "nudge: each press is exactly ONE undo step, never zero and never two");

    // Six nudges out and six back. Repeated nudging is the gesture this tool
    // is used for, and it is precisely where a per-step resample would rot
    // the picture invisibly -- one pass is imperceptible, twelve are not.
    for (int i = 0; i < 5; ++i) check(nudgeMove(od, 3.0f, -2.0f).ok, "nudge: repeats commit");
    for (int i = 0; i < 6; ++i) check(nudgeMove(od, -3.0f, 2.0f).ok, "nudge: and reverse");
    check(samePixels(pixelsBefore, snapshot(od, 64, 48)),
          "nudge: twelve nudges out and back leave the layer BIT-IDENTICAL -- repeated "
          "nudging cannot accumulate resampling damage");
  }

  // --- 8. Refusals arrive as prose, and leave the session inactive ---------
  //
  // app/MoveTool.hpp section 3. Each of these is a target a user can actually
  // be pointed at when they press the mouse, and each must produce a sentence
  // the status band can show rather than a drag that quietly commits nothing.
  {
    OpenDocument od = makeBlankOpenDocument(32, 32, WorkingSpace{});
    fillRgb(*od.document.layers[0].rgbTiles, 4, 4, 20, 20);
    od.recordEdit("fill fixture", EditKind::Content);

    od.document.layers[0].locked = true;
    TransformSession ts;
    const TransformBeginResult locked = beginMove(ts, od);
    check(!locked.ok && locked.error.find("locked") != std::string::npos && !ts.active(),
          "refusal: a locked layer refuses BY NAME and leaves no session behind");
    const uint64_t revBefore = od.revision;
    const TransformCommitResult lockedNudge = nudgeMove(od, 1.0f, 0.0f);
    check(!lockedNudge.ok && od.revision == revBefore,
          "refusal: and the keyboard form refuses it too, without touching the document -- "
          "an arrow key swallowed by a locked layer is the same silent no-op as a dead drag");
    od.document.layers[0].locked = false;

    // An empty layer: nothing to move, and saying so beats a gesture that
    // appears to pick up a picture that is not there.
    OpenDocument blank = makeBlankOpenDocument(32, 32, WorkingSpace{});
    TransformSession ts2;
    const TransformBeginResult empty = beginMove(ts2, blank);
    check(!empty.ok && empty.error.find("no content") != std::string::npos && !ts2.active(),
          "refusal: a layer with no content refuses by name rather than moving nothing");

    // A Pigment layer WITH a selection: refused, because splicing moved
    // pigment back needs Kubelka-Munk mixing and not a straight alpha over
    // (app/TransformSession.hpp section 4). The same layer with no selection
    // is NOT refused -- a whole-layer pigment move is supported -- and
    // asserting both halves is what stops a fix for one becoming a blanket
    // refusal of the other.
    OpenDocument pig = makeBlankOpenDocument(32, 32, WorkingSpace{});
    pig.document.layers[0].kind = LayerKind::Pigment;
    pig.document.layers[0].rgbTiles.reset();
    pig.document.layers[0].pigmentTiles.emplace();
    for (int32_t y = 4; y < 20; ++y) {
      for (int32_t x = 4; x < 20; ++x) {
        const PixelCoord at{x, y};
        pig.document.layers[0].pigmentTiles->getOrCreate(tileCoordAt(at)).writeTexel(
            tileLocalOffset(at), PigmentTexel{Latent{{0.2f, 0.3f, 0.4f}, {}}, 0.5f});
      }
    }
    pig.recordEdit("pigment fixture", EditKind::Content);

    TransformSession ts3;
    check(beginMove(ts3, pig).ok && ts3.target() == TransformTarget::Layer,
          "refusal: a Pigment layer with NO selection moves as a whole layer, unrefused");
    ts3.cancel();

    pig.selection = selectRectangle(4.0f, 4.0f, 16.0f, 16.0f);
    TransformSession ts4;
    const TransformBeginResult pigSel = beginMove(ts4, pig);
    check(!pigSel.ok && pigSel.error.find("Pigment") != std::string::npos && !ts4.active(),
          "refusal: but a Pigment layer WITH a selection refuses by name -- the moved paint "
          "would have to be re-mixed, not alpha-blended, back onto what is left");
  }

  std::printf("[selftest] move tool %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
