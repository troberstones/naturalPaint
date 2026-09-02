#include "app/selftest/Support.hpp"

#include "app/AppState.hpp"
#include "app/StrokeSession.hpp"
#include "brush/CloneStamp.hpp"
#include "brush/Deposit.hpp"
#include "core/SelectionShapes.hpp"
#include "ui/AtelierChrome.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The clone stamp on a plain RGB layer (brush/CloneStamp), the routing that
// made it exist at all (app/StrokeSession §1b), and the Option+click gesture
// that gives it a source (AppState::CloneSourceState).
//
// See app/SelfTest.hpp for the section's own contents list, and the two headers
// for every decision this file only checks. Three things are worth saying here
// because they are what the section is *for*:
//
//   * **The clone stamp did nothing.** `Tool::CloneStamp` sat in
//     `strokeRouteFor()`'s not-built list beside the other name/icon/slot-only
//     palette cells, so a drag with it reached no layer and said nothing.
//   * **The order-independence claim (section 2) is the one this module exists
//     for**, and it is only testable as a *pair*. A loop that read the live
//     store gets a rightward one-texel shift exactly right and turns the
//     leftward one into a single column smeared across the whole dab, so
//     either direction on its own passes against the broken implementation.
//     Section 2 runs both, over a per-column pattern, across a tile boundary.
//   * **The unset-source refusal (section 4) is about an INVISIBLE failure.**
//     With no anchor the offset is (0,0), every texel's source is itself, and a
//     full-opacity composite of a texel onto itself is a perfect no-op. The
//     wrong version of this tool is not one that draws the wrong thing; it is
//     one that draws nothing while every tier of the chrome says it is working.
// ---------------------------------------------------------------------------
bool runCloneStampTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // --- Tolerances -------------------------------------------------------
  //
  // Most of this file asserts at **exactly zero**, and that is a property of
  // the arithmetic rather than luck: at full flow over an opaque source the
  // composite's keep factor is `1 - src[3] * a` with `src[3] == a == 1`, i.e.
  // exactly `0.0f`, so the stored texel is `src * 1.0f + dst * 0.0f` -- both
  // operations exact in IEEE-754 -- and it round-trips through binary16
  // unchanged because it *is* a binary16 value that was read out of a tile.
  // That exactness is the whole meaning of the word "clone", so it is asserted
  // rather than approximated.
  //
  // Sections 5 and 6 do need a bound. `kHalfRel`/`kHalfFloor` are binary16's,
  // derived from the storage exactly as `runRgbEraseTest()` derives them for
  // the identical storage: an 11-bit significand gives at most 2^-11 relative
  // for a normal value, plus half a subnormal ulp, 2^-25, absolute. Over a
  // stroke of N *writing* dabs the drift is at most `N * kHalfRel * value`,
  // because each write rounds once and every earlier error is damped by a later
  // dab's `(1 - a)` factor, which is at most 1.
  constexpr float kHalfRel = 4.8828125e-04f;    // 2^-11
  constexpr float kHalfFloor = 2.9802322e-08f;  // 2^-25

  auto readAt = [](const TileStore& store, int32_t x, int32_t y) -> std::array<float, 4> {
    const Tile* tile = store.find(tileCoordAt(PixelCoord{x, y}));
    if (tile == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
    return tile->readPixel(tileLocalOffset(PixelCoord{x, y}));
  };

  // The fixture is written straight into the store rather than painted with
  // brush/RgbDeposit first, for `runRgbEraseTest()`'s stated reason: this
  // section's subject is what the clone does to texels that are already there,
  // and building the "already there" out of another module would make every
  // number below depend on two arithmetics instead of one.
  auto fillRect = [](TileStore& store, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     const std::array<float, 4>& premultiplied) {
    for (int32_t y = y0; y <= y1; ++y) {
      for (int32_t x = x0; x <= x1; ++x) {
        const PixelCoord p{x, y};
        store.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), premultiplied);
      }
    }
  };

  // A per-COLUMN pattern, which is what makes a one-texel horizontal shift
  // visible at all: a flat fill would be indistinguishable from a smear, from
  // the identity, and from a shift in the wrong direction. `k / 64` for
  // `k` in 1..13 is exactly representable in binary16 (64 is a power of two and
  // 13 needs four significand bits), so the fixture survives storage unchanged
  // and every comparison below can be an exact one.
  auto columnValue = [](int32_t x) {
    return static_cast<float>(((x % 13) + 13) % 13 + 1) / 64.0f;
  };
  auto columnTexel = [&](int32_t x) {
    const float v = columnValue(x);
    return std::array<float, 4>{v, v, v, 1.0f};
  };
  auto fillColumns = [&](TileStore& store, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    for (int32_t y = y0; y <= y1; ++y)
      for (int32_t x = x0; x <= x1; ++x) {
        const PixelCoord p{x, y};
        store.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), columnTexel(x));
      }
  };

  // A hard disc, so `dabCoverage()` is exactly 1.0f over the whole disc --
  // `singleTipCoverage()` returns `1.0f` outright whenever `d <= hardness`, and
  // `d < 1` always holds inside -- and every number below is about the clone
  // rather than about the falloff.
  auto discTip = [](float radius, float flow) {
    BrushTip t;
    t.radius = radius;
    t.hardness = 1.0f;
    t.flow = flow;
    t.opacity = 1.0f;
    return t;
  };

  auto makeRgbDoc = [](int32_t w, int32_t h) {
    return makeBlankOpenDocument(w, h, WorkingSpace{}, "clone stamp");
  };

  std::printf("[selftest] clone stamp (brush/CloneStamp, app/StrokeSession §1b)\n");

  // ======================================================================
  // 0. The premise every exactness assertion below rests on
  // ======================================================================
  {
    const BrushTip t = discTip(10.0f, 1.0f);
    check(dabCoverage(t, 0.0f, 0.0f) == 1.0f && dabCoverage(t, 6.5f, 0.0f) == 1.0f &&
              dabCoverage(t, 9.99f, 0.0f) == 1.0f && dabCoverage(t, 10.0f, 0.0f) == 0.0f,
          "premise: a hardness-1 tip covers its whole disc at EXACTLY 1.0 -- without that "
          "every 'reproduces the source exactly' claim below would be a claim about the "
          "falloff instead");
    check(columnTexel(7) != columnTexel(8) && columnValue(20) == columnValue(33),
          "premise: the column pattern really does differ column to column -- a flat fixture "
          "cannot tell a shift from a smear from the identity");
  }

  // ======================================================================
  // 1. A clone reproduces the source texels EXACTLY (brush/CloneStamp §1)
  // ======================================================================
  //
  // The word in the tool's name is "clone". At full flow over an opaque source
  // the keep factor `1 - src[3] * a` is exactly zero, so this is a copy and not
  // an approximation -- asserted on all four premultiplied channels, over a
  // destination that already held *different* paint, so "unchanged" and
  // "correct" cannot both be true of a broken implementation.
  {
    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    // Every component exactly representable in binary16, so "bit for bit" is a
    // claim about the clone and not about the fixture surviving storage: k/2^n
    // with a four-bit k is exact in an 11-bit significand, and 0.8 and 0.4 --
    // the obvious first choice -- are not.
    const std::array<float, 4> a{0.75f, 0.375f, 0.25f, 1.0f};   // the source paint
    const std::array<float, 4> b{0.125f, 0.125f, 0.5f, 1.0f};   // what is already there
    fillRect(store, 20, 20, 70, 70, a);
    fillRect(store, 80, 80, 130, 130, b);

    CloneStampStroke s;
    s.begin(store, Vec2{-60.0f, -60.0f}, 1.0f, false);
    const DepositCount c =
        s.cloneDab(store, discTip(10.0f, 1.0f), Vec2{100.5f, 100.5f}, 256, 256, nullptr, nullptr);

    bool exact = true;
    for (int32_t y = 94; y <= 106; ++y)
      for (int32_t x = 94; x <= 106; ++x)
        if (readAt(store, x, y) != a) exact = false;

    std::printf("  [measured] one dab at offset (-60,-60): %zu texels, %zu tiles; centre now "
                "(%.6f %.6f %.6f %.6f)\n",
                c.texels, c.tiles, static_cast<double>(readAt(store, 100, 100)[0]),
                static_cast<double>(readAt(store, 100, 100)[1]),
                static_cast<double>(readAt(store, 100, 100)[2]),
                static_cast<double>(readAt(store, 100, 100)[3]));

    check(c.texels > 0 && exact,
          "exact: every texel in the dab's core holds the SOURCE texel bit for bit, on all "
          "four premultiplied channels -- at full flow the keep factor is exactly 0, so a "
          "clone is a copy rather than a close approximation");
    check(readAt(store, 40, 40) == a && readAt(store, 30, 65) == a,
          "exact: and the SOURCE region is untouched -- a clone that moved what it read would "
          "be a smear wearing this tool's name");
    check(readAt(store, 82, 82) == b && readAt(store, 128, 128) == b,
          "exact: with the rest of the destination rectangle bit-identical -- the dab wrote "
          "its own footprint and nothing else");
    s.end();
  }

  // ======================================================================
  // 2. The result is INDEPENDENT of iteration order (brush/CloneStamp §2)
  // ======================================================================
  //
  // The module's whole subject. Source and destination are two windows onto one
  // store, so a loop reading the live store feeds its own output back in and the
  // answer depends on the direction the texel and tile loops happen to run:
  //
  //   offset -1, ascending x   every texel reads the one before it -- ONE COLUMN
  //                            smeared across the whole dab
  //   offset +1, ascending x   reads texels not yet written -- the correct
  //                            shifted copy
  //
  // Same magnitude, opposite sign, two different KINDS of answer, and nothing in
  // the picture says which happened. Asserting both directions is therefore the
  // test: no live-reading implementation can satisfy them at once.
  //
  // The dab is centred on x = 128, which is a tile boundary (kTileSize = 128),
  // so the (y, x)-ascending TILE loop is split by the same argument at the same
  // time as the texel loop.
  {
    auto shiftOnce = [&](int32_t dxOffset) {
      OpenDocument od = makeRgbDoc(256, 256);
      TileStore& store = *od.document.layers[0].rgbTiles;
      fillColumns(store, 0, 60, 255, 140);
      CloneStampStroke s;
      s.begin(store, Vec2{static_cast<float>(dxOffset), 0.0f}, 1.0f, false);
      const BrushTip t = discTip(20.0f, 1.0f);
      s.cloneDab(store, t, Vec2{128.5f, 100.5f}, 256, 256, nullptr, nullptr);
      s.end();

      // Only the texels the disc covers at coverage exactly 1 -- which for a
      // hardness-1 tip is every texel strictly inside it -- are claims about
      // the clone; the rest are claims about `dabCoverage()`.
      int32_t checked = 0;
      int32_t wrong = 0;
      for (int32_t y = 82; y <= 118; ++y) {
        for (int32_t x = 109; x <= 147; ++x) {
          const float ddx = (static_cast<float>(x) + 0.5f) - 128.5f;
          const float ddy = (static_cast<float>(y) + 0.5f) - 100.5f;
          if (!(ddx * ddx + ddy * ddy < 20.0f * 20.0f)) continue;
          ++checked;
          if (readAt(store, x, y) != columnTexel(x + dxOffset)) ++wrong;
        }
      }
      return std::pair<int32_t, int32_t>{checked, wrong};
    };

    const auto left = shiftOnce(-1);
    const auto right = shiftOnce(+1);
    std::printf("  [measured] one-texel shift across the x=128 tile boundary: -1 -> %d/%d "
                "texels wrong; +1 -> %d/%d wrong\n",
                left.second, left.first, right.second, right.first);

    check(left.first > 500 && right.first == left.first,
          "order: both directions really do exercise hundreds of texels over the same disc -- "
          "the premise the pair below rests on");
    check(right.second == 0,
          "order: a source one texel to the RIGHT reproduces the shifted columns exactly -- "
          "the direction a live-reading loop also gets right, asserted so the pair is a pair");
    check(left.second == 0,
          "order: and a source one texel to the LEFT does too, at zero tolerance -- this is "
          "the one a live-reading loop turns into a single column smeared across the whole "
          "dab, and it is why the source is snapshotted at pen-down");

    // The same hazard one granularity up: a STROKE whose dabs pass over their
    // own source. Snapshotting per DAB instead of per stroke -- the other
    // tempting design, and the one brush/CloneStamp §2 records as rejected --
    // is correct for one dab and wrong here: dab N would read dab N-1's output
    // wherever the stroke has already crossed its source, giving `in(x-8)`
    // where `in(x-4)` is meant.
    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillColumns(store, 0, 88, 255, 112);
    std::vector<Vec2> dabs;
    for (int32_t k = 0; k <= 46; ++k)
      dabs.push_back(Vec2{60.0f + 3.0f * static_cast<float>(k), 100.5f});
    CloneStampStroke s;
    s.begin(store, Vec2{-4.0f, 0.0f}, 1.0f, false);
    const StrokeDeposit sd = s.cloneDabs(store, discTip(8.0f, 1.0f), dabs, 256, 256, nullptr);
    s.end();

    int32_t strokeWrong = 0;
    int32_t strokeChecked = 0;
    for (int32_t x = 80; x <= 180; ++x) {
      ++strokeChecked;
      if (readAt(store, x, 100) != columnTexel(x - 4)) ++strokeWrong;
    }
    std::printf("  [measured] a %zu-dab stroke at offset (-4,0) over its own source: %d/%d "
                "texels wrong on the centre line\n",
                sd.dabs, strokeWrong, strokeChecked);
    check(sd.dabs == dabs.size() && strokeChecked == 101 && strokeWrong == 0,
          "order: a whole STROKE dragged along its own source is the same shifted copy, dab "
          "for dab -- a per-DAB snapshot is right for one dab and reads its own output here");
  }

  // ======================================================================
  // 3. The keep factor is `1 - src[3] * a`, not `1 - a`
  // ======================================================================
  //
  // brush/CloneStamp §1's one genuinely new line. Reusing brush/RgbDeposit's
  // opaque-source keep factor would make a clone whose source is blank canvas
  // *erase* the paint under the tip -- a tool doing the opposite of its name,
  // invisibly, and only over whichever parts of the source happen to be empty.
  {
    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    const std::array<float, 4> b{0.5f, 0.25f, 0.75f, 1.0f};
    fillRect(store, 80, 80, 130, 130, b);
    const size_t tilesBefore = store.occupiedTileCount();

    CloneStampStroke s;
    s.begin(store, Vec2{-60.0f, -60.0f}, 1.0f, false);  // source: never written
    const DepositCount c =
        s.cloneDab(store, discTip(10.0f, 1.0f), Vec2{100.5f, 100.5f}, 256, 256, nullptr, nullptr);
    s.end();

    bool untouched = true;
    for (int32_t y = 90; y <= 110; ++y)
      for (int32_t x = 90; x <= 110; ++x)
        if (readAt(store, x, y) != b) untouched = false;

    check(untouched,
          "transparent source: cloning from blank canvas leaves the destination BIT-IDENTICAL "
          "-- `1 - a` instead of `1 - src[3] * a` would cut a hole in the paint under the tip");
    check(c.texels == 0 && c.tiles == 0 && store.occupiedTileCount() == tilesBefore,
          "transparent source: and it writes no texel, unshares no tile and reports none -- "
          "the skip is arithmetic (dst * (1 - 0) IS dst), not an optimisation");

    // **The keep factor's own assertion, and it needs a PARTIALLY covered
    // source.** An all-zero source never reaches the composite at all -- §4's
    // skip returns before it -- so the fully-transparent case above proves the
    // skip and not the arithmetic. A source at alpha 0.5 over an opaque
    // destination is where `1 - src[3] * a` and `1 - a` give visibly different
    // answers: the correct one is a source-over that leaves the texel opaque,
    // and the wrong one REPLACES the destination with a half-transparent texel,
    // punching the layer's alpha down to the source's.
    {
      OpenDocument od2 = makeRgbDoc(256, 256);
      TileStore& st2 = *od2.document.layers[0].rgbTiles;
      fillRect(st2, 0, 0, 255, 60, {0.25f, 0.25f, 0.25f, 0.5f});   // half-covered source
      fillRect(st2, 0, 80, 255, 160, {0.0f, 0.0f, 0.0f, 1.0f});    // opaque destination
      CloneStampStroke half;
      half.begin(st2, Vec2{0.0f, -100.0f}, 1.0f, false);
      half.cloneDab(st2, discTip(20.0f, 1.0f), Vec2{120.0f, 120.0f}, 256, 256, nullptr, nullptr);
      half.end();
      const std::array<float, 4> got = readAt(st2, 120, 120);
      std::printf("  [measured] an alpha-0.5 source over an opaque destination: "
                  "(%.6f %.6f %.6f %.6f); `1 - a` would give (0.25 0.25 0.25 0.50)\n",
                  static_cast<double>(got[0]), static_cast<double>(got[1]),
                  static_cast<double>(got[2]), static_cast<double>(got[3]));
      check(got == std::array<float, 4>{0.25f, 0.25f, 0.25f, 1.0f},
            "partial source: a half-covered source composites OVER the destination and leaves "
            "it opaque -- `1 - a` instead of `1 - src[3] * a` would replace it outright and "
            "drop the layer's alpha to the source's");
    }

    // The other half of §4, the deliberate inverse of the skip's own test: a
    // texel holding colour at alpha 0 is MALFORMED rather than absent, so it is
    // cloned like anything else rather than quietly declared empty here.
    TileStore odd;
    const PixelCoord sp{10, 10};
    odd.getOrCreate(tileCoordAt(sp)).writePixel(tileLocalOffset(sp), {0.375f, 0.0f, 0.0f, 0.0f});
    CloneStampStroke probe;
    probe.begin(odd, Vec2{-20.0f, 0.0f}, 1.0f, false);
    const DepositCount oc =
        probe.cloneDab(odd, discTip(1.2f, 1.0f), Vec2{30.5f, 10.5f}, 64, 64, nullptr, nullptr);
    probe.end();
    check(oc.texels > 0 &&
              readAt(odd, 30, 10) == std::array<float, 4>{0.375f, 0.0f, 0.0f, 0.0f},
          "transparent source: a MALFORMED source texel (colour at alpha 0) is cloned rather "
          "than skipped -- the skip tests all four channels, so 'nothing there' means nothing");
  }

  // ======================================================================
  // 3b. Alpha lock freezes the alpha and lets the colour through
  // ======================================================================
  //
  // The row where this tool and the eraser deliberately disagree
  // (app/StrokeSession §1b): `alphaLocked` exists to permit colour changes and
  // refuse alpha ones, so a clone -- which paints -- goes through, and
  // brush/CloneStamp §1's colour-only composite is what honours the lock. That
  // composite is derived by un-premultiplying the source and watching the
  // `src[3]` cancel, so the assertion worth making is that it neither divides
  // by a transparent source's alpha nor lets the destination's own alpha move.
  {
    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    // A HALF-covered destination and an opaque source, so an implementation
    // that quietly wrote `src` through would be caught by the alpha alone.
    fillRect(store, 0, 0, 255, 60, {1.0f, 1.0f, 1.0f, 1.0f});
    fillRect(store, 0, 80, 255, 160, {0.0f, 0.0f, 0.0f, 0.5f});

    CloneStampStroke s;
    s.begin(store, Vec2{0.0f, -100.0f}, 1.0f, /*alphaLocked=*/true);
    s.cloneDab(store, discTip(20.0f, 1.0f), Vec2{120.0f, 120.0f}, 256, 256, nullptr, nullptr);
    s.end();

    const std::array<float, 4> got = readAt(store, 120, 120);
    std::printf("  [measured] a clone onto an ALPHA-LOCKED texel of alpha 0.5: "
                "(%.6f %.6f %.6f %.6f)\n",
                static_cast<double>(got[0]), static_cast<double>(got[1]),
                static_cast<double>(got[2]), static_cast<double>(got[3]));
    check(got[3] == 0.5f,
          "alpha lock: the destination's alpha is FROZEN -- copied through rather than "
          "recomputed, so there is no expression a later dab could climb");
    check(got[0] == 0.5f && got[1] == 0.5f && got[2] == 0.5f,
          "alpha lock: while the colour moves all the way to the source, premultiplied by the "
          "destination's own alpha -- an unlocked clone would have written alpha 1.0 here, and "
          "a refusal would have written nothing at all");
  }

  // ======================================================================
  // 4. An unset source writes NOTHING, and says so (app/StrokeSession §1b)
  // ======================================================================
  //
  // The failure this refusal exists to prevent is silent: with no anchor the
  // offset is (0,0), every texel's source is itself, and a full-opacity copy of
  // a texel onto itself is a perfect no-op. The tool would draw nothing with the
  // palette cell lit, the cursor correct, and not one word anywhere.
  {
    AppState::CloneSourceState clone;
    check(!clone.haveAnchor && !clone.haveOffset && !cloneSourceRefusal(clone).empty() &&
              contains(cloneSourceRefusal(clone), "Option-click"),
          "unset: a fresh clone state refuses, and the sentence names the Option+click that "
          "fixes it -- a modifier that does nothing visible is not discoverable by trying");
    check(!latchCloneOffset(clone, Vec2{100.0f, 100.0f}) && !clone.haveOffset,
          "unset: pen-down with no anchor latches nothing and reports no usable source -- it "
          "does NOT invent an offset of (0,0), which is the invisible version of this bug");

    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillColumns(store, 0, 60, 255, 140);
    const size_t entries = od.history.entries().size();
    const uint64_t rev = od.revision;
    const std::array<float, 4> before = readAt(store, 100, 100);

    StrokeSession s;
    std::string why;
    const bool began = s.begin(od, 0, discTip(10.0f, 1.0f), Tool::CloneStamp, &why, nullptr,
                               DynamicInputs{}, &clone);
    check(!began && contains(why, "no source set") && contains(why, "Option-click"),
          "unset: StrokeSession::begin() REFUSES a clone with no source, in the same errorOut "
          "sentence every other refusal uses -- so the options bar prints it unchanged");
    std::string why2;
    check(!s.begin(od, 0, discTip(10.0f, 1.0f), Tool::CloneStamp, &why2, nullptr,
                   DynamicInputs{}, nullptr) &&
              why2 == why,
          "unset: and a NULL clone state takes the identical branch -- 'the UI forgot to pass "
          "it' and 'the user has not set a source' have the same correct answer");
    // The refusal is only worth anything if nothing happened. Driven through
    // addPoint()/end() as well, because a session that refused at begin() but
    // still accepted points would be the same defect one call later.
    s.addPoint(100.0f, 100.0f);
    s.addPoint(120.0f, 100.0f);
    s.end();
    check(readAt(store, 100, 100) == before && od.history.entries().size() == entries &&
              od.revision == rev && s.texelsWritten() == 0,
          "unset: and the layer, the revision and the history are all untouched -- a refusal "
          "that still wrote an undo step would be worse than the silent no-op it replaced");

    // The gesture's own arithmetic, which is where the sign convention lives.
    setCloneAnchor(clone, Vec2{40.0f, 30.0f});
    check(clone.haveAnchor && !clone.haveOffset,
          "gesture: an Option+click sets the anchor and leaves the offset UNLATCHED -- the "
          "next stroke derives it from its own pen-down");
    check(latchCloneOffset(clone, Vec2{100.0f, 90.0f}) && clone.haveOffset &&
              clone.offset.x == -60.0f && clone.offset.y == -60.0f,
          "gesture: pen-down latches offset = anchor - penDown, so the stroke's first dab "
          "reads exactly the anchor -- the one sign convention the whole feature agrees on");
    check(latchCloneOffset(clone, Vec2{7.0f, 9.0f}) && clone.offset.x == -60.0f &&
              clone.offset.y == -60.0f,
          "gesture: a SECOND stroke keeps the same offset -- this build clones aligned, so a "
          "series of short dabs continues one copy rather than restarting it at the anchor");
    setCloneAnchor(clone, Vec2{200.0f, 200.0f});
    check(!clone.haveOffset && !cloneSourceRefusal(clone).empty(),
          "gesture: a new Option+click DISCARDS the latched offset -- one that survived would "
          "leave the click looking like it worked while the copy carried on from the old "
          "source");

    // And the whole path end to end: anchor, latch, stroke, exact copy.
    AppState::CloneSourceState live;
    setCloneAnchor(live, Vec2{40.0f, 100.0f});
    check(latchCloneOffset(live, Vec2{160.0f, 100.0f}), "gesture: the end-to-end path latches");
    StrokeSession s2;
    std::string e2;
    check(s2.begin(od, 0, discTip(10.0f, 1.0f), Tool::CloneStamp, &e2, nullptr, DynamicInputs{},
                   &live) &&
              s2.route() == StrokeRoute::CloneStamp && e2.empty(),
          "gesture: with a source set the same call BEGINS, and reports the clone route");
    check(s2.cloneOffsetX() == -120 && s2.cloneOffsetY() == 0 &&
              s2.cloneSnapshotTiles() == store.occupiedTileCount(),
          "gesture: the session latched the rounded offset and took a snapshot of every tile "
          "the layer had -- both internals the tool's claims are actually about");
    // A short DRAG, not a single click: `brush/StrokePath` needs more than one
    // sample before it emits anything (it fits a curve through them), so a
    // one-point stroke deposits nothing at all -- which would make every
    // assertion below vacuously true about an untouched fixture.
    s2.addPoint(160.0f, 100.0f);
    s2.addPoint(164.0f, 100.0f);
    s2.addPoint(168.0f, 100.0f);
    s2.end();
    std::printf("  [measured] pen-down at (160,100) with the anchor at (40,100): texel "
                "(%.6f %.6f %.6f %.6f), want %.6f; %zu texels, %zu new entries, last \"%s\"\n",
                static_cast<double>(readAt(store, 160, 100)[0]),
                static_cast<double>(readAt(store, 160, 100)[1]),
                static_cast<double>(readAt(store, 160, 100)[2]),
                static_cast<double>(readAt(store, 160, 100)[3]),
                static_cast<double>(columnValue(40)), s2.texelsWritten(),
                od.history.entries().size() - entries,
                od.history.entries().empty() ? "" : od.history.entries().back().label.c_str());
    check(readAt(store, 160, 100) == columnTexel(40) && s2.texelsWritten() > 0,
          "gesture: the stroke's first dab lands the ANCHOR's own texel at the pen-down "
          "position -- offset = anchor - penDown, read all the way through the session");
    check(od.history.entries().size() == entries + 1 &&
              od.history.entries().back().label == "clone stamp",
          "gesture: and the whole stroke is exactly one history entry, labelled \"clone "
          "stamp\" rather than \"brush stroke\"");
    check(s2.cloneSnapshotTiles() == 0,
          "gesture: and the snapshot is dropped at pen-up -- it shares a tile with the layer "
          "for every tile the layer had, so holding one across an idle session would double "
          "the document");
  }

  // ======================================================================
  // 5. The active selection bounds the clone, both ways (PRD E1, P0)
  // ======================================================================
  //
  // brush/RgbDeposit §4's argument, which brush/CloneStamp inherits along with
  // the accumulator: the selection scales what one dab transfers AND caps what
  // any number of dabs can transfer. The first alone is a speed limit, and a
  // scrubbed stroke walks straight through it.
  {
    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    const std::array<float, 4> a{1.0f, 1.0f, 1.0f, 1.0f};
    const std::array<float, 4> b{0.0f, 0.0f, 0.0f, 1.0f};
    fillRect(store, 0, 0, 255, 60, a);     // the source band
    fillRect(store, 0, 80, 255, 160, b);   // the destination band
    // A fractional left edge, so one column is PARTIALLY selected -- an
    // in-or-out boundary would pass against a version that treated the
    // selection as a bitmask.
    Selection sel = selectRectangle(100.25f, 0.0f, 200.0f, 256.0f);
    const float partial = selectionCoverageAt(&sel, PixelCoord{100, 120});
    const std::array<float, 4> outsideBefore = readAt(store, 90, 120);

    CloneStampStroke s;
    s.begin(store, Vec2{0.0f, -100.0f}, 1.0f, false);
    const BrushTip t = discTip(30.0f, 1.0f);
    for (int k = 0; k < 30; ++k)
      s.cloneDab(store, t, Vec2{110.0f, 120.0f}, 256, 256, &sel, nullptr);
    s.end();

    // `src` is opaque, so a sequence of source-overs composes exactly to
    // `src * A + dst * (1 - A)` with `A` the stroke's total -- capped at the
    // coverage. The rejected rate-only model has no cap, so `A -> 1`.
    const float want = 1.0f * partial + 0.0f * (1.0f - partial);
    const float got = readAt(store, 100, 120)[0];
    std::printf("  [measured] 30 dabs through a %.4f-covered column: %.6f (bounded model "
                "wants %.6f; the rate-only model reaches %.6f)\n",
                static_cast<double>(partial), static_cast<double>(got),
                static_cast<double>(want), 1.0);

    check(partial > 0.0f && partial < 1.0f,
          "selection: the boundary column really is PARTIALLY covered -- the premise the two "
          "assertions below rest on, or they would pass against a bitmask selection");
    check(readAt(store, 90, 120) == outsideBefore && readAt(store, 60, 120) == outsideBefore,
          "selection: a texel outside the ants is BIT-IDENTICAL afterwards, at zero tolerance");
    check(std::fabs(got - want) <= 30.0f * kHalfRel * want + kHalfFloor && got < 0.9f,
          "selection: and a half-covered texel cannot be SCRUBBED past its coverage -- 30 "
          "overlapping dabs land on `coverage`, where a rate-only gate would reach 1.0");
    check(readAt(store, 130, 120) == a,
          "selection: while a fully selected texel inside the same dab is the exact source -- "
          "the bound must not cost the selected region its exactness");
  }

  // ======================================================================
  // 6. Opacity is a per-stroke CEILING, not a per-dab multiplier
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(256, 256);
    TileStore& store = *od.document.layers[0].rgbTiles;
    fillRect(store, 0, 0, 255, 60, {1.0f, 1.0f, 1.0f, 1.0f});
    fillRect(store, 0, 80, 255, 160, {0.0f, 0.0f, 0.0f, 1.0f});

    CloneStampStroke s;
    s.begin(store, Vec2{0.0f, -100.0f}, 0.5f, false);
    const BrushTip t = discTip(20.0f, 1.0f);
    size_t writingDabs = 0;
    for (int k = 0; k < 40; ++k)
      if (s.cloneDab(store, t, Vec2{120.0f, 120.0f}, 256, 256, nullptr, nullptr).texels > 0)
        ++writingDabs;
    const float got = readAt(store, 120, 120)[0];
    const float accum = s.strokeAlphaAt(PixelCoord{120, 120});
    s.end();

    std::printf("  [measured] 40 dabs at opacity 0.5: %zu of them wrote anything; texel %.6f, "
                "accumulator %.6f (a per-dab model reaches 1.0)\n",
                writingDabs, static_cast<double>(got), static_cast<double>(accum));
    check(accum == 0.5f && got == 0.5f,
          "ceiling: 40 overlapping dabs at opacity 0.5 land the texel exactly half way from "
          "the destination to the source and stop -- a per-dab opacity would converge to a "
          "full copy, which is a second flow slider with a different name");
    check(writingDabs == 1,
          "ceiling: and only the FIRST dab writes at all -- once the ceiling is reached the "
          "rest touch no texel, so a scrubbed clone stops dirtying tiles for re-upload");
  }

  // ======================================================================
  // 7. Cloning nothing COSTS nothing, end to end
  // ======================================================================
  {
    OpenDocument od = makeRgbDoc(512, 512);
    TileStore& store = *od.document.layers[0].rgbTiles;
    const size_t entries = od.history.entries().size();
    const uint64_t rev = od.revision;

    AppState::CloneSourceState clone;
    setCloneAnchor(clone, Vec2{20.0f, 400.0f});
    latchCloneOffset(clone, Vec2{60.0f, 200.0f});

    StrokeSession s;
    std::string e;
    check(s.begin(od, 0, discTip(30.0f, 1.0f), Tool::CloneStamp, &e, nullptr, DynamicInputs{},
                  &clone) &&
              s.route() == StrokeRoute::CloneStamp,
          "empty clone: begins normally on the blank RGB layer, and reports the clone route");
    for (int k = 0; k < 20; ++k) s.addPoint(60.0f + 20.0f * static_cast<float>(k), 200.0f);
    s.end();

    std::printf("  [measured] a %zu-dab clone whose source is blank canvas: %zu texels, %zu "
                "tiles reported, %zu tiles in the layer\n",
                s.dabCount(), s.texelsWritten(), s.strokeTiles().size(),
                store.occupiedTileCount());
    check(s.dabCount() > 0 && store.occupiedTileCount() == 0 && s.strokeTiles().empty() &&
              s.texelsWritten() == 0,
          "empty clone: a stroke of dozens of dabs reading unpainted canvas allocates NOT ONE "
          "tile and reports none -- 128 KiB per tile crossed, plus a dirty tile per frame of "
          "the drag, is what the empty-source skip is worth");
    check(od.history.entries().size() == entries && od.revision == rev,
          "empty clone: and it records NO history entry and moves no revision -- an undo step "
          "that undoes nothing is worse than a missing one");
  }

  // ======================================================================
  // 7b. PAPER GRAIN reaches this route (app/StrokeSession's grainReachesRoute)
  // ======================================================================
  //
  // `grainReachesRoute()` now answers true for the clone, and that predicate is
  // what the BRUSH panel greys the whole PAPER GRAIN group on. **Asserted end to
  // end rather than as a predicate**, because the identical claim has already
  // been wrong once for three other routes: `grainCoverageAt()` was added to
  // brush/RgbDeposit, brush/RgbErase and brush/PigmentErase while the UI kept
  // its private copy of the old answer, leaving a working control greyed out
  // over a sentence explaining that it could not work. A predicate assertion
  // would have passed then too.
  {
    auto dabWithGrain = [&](bool grain) {
      OpenDocument od = makeRgbDoc(256, 256);
      TileStore& store = *od.document.layers[0].rgbTiles;
      fillColumns(store, 0, 20, 255, 140);
      BrushTip t = discTip(24.0f, 1.0f);
      if (grain) {
        t.grain.enabled = true;
        t.grain.periodX = 24;
        t.grain.periodY = 24;
        t.grain.depth = 0.6f;
        t.grain.strength = 1.0f;
      }
      CloneStampStroke s;
      // A HORIZONTAL offset, not a vertical one: the fixture varies by column,
      // so a vertical shift would copy each texel onto an identical value and
      // "exact copy" would be true of an untouched fixture too.
      s.begin(store, Vec2{-4.0f, 0.0f}, 1.0f, false);
      const DepositCount c =
          s.cloneDab(store, t, Vec2{128.5f, 100.5f}, 256, 256, nullptr, nullptr);
      s.end();
      // How many of the dab's texels came out as an EXACT copy of their
      // source. Grain does not stop a texel being written -- at full tip
      // coverage `1 - G` is still positive -- it lowers the coverage, so the
      // observable is that the copy stops being exact wherever the tooth
      // stands up, which is the whole point of a paper.
      int32_t exact = 0;
      for (int32_t y = 76; y <= 124; ++y)
        for (int32_t x = 104; x <= 152; ++x) {
          const float ddx = (static_cast<float>(x) + 0.5f) - 128.5f;
          const float ddy = (static_cast<float>(y) + 0.5f) - 100.5f;
          if (!(ddx * ddx + ddy * ddy < 24.0f * 24.0f)) continue;
          if (readAt(store, x, y) == columnTexel(x - 4)) ++exact;
        }
      return std::pair<size_t, int32_t>{c.texels, exact};
    };
    const auto plain = dabWithGrain(false);
    const auto grained = dabWithGrain(true);
    std::printf("  [measured] one dab of %zu texels: %d exact copies smooth, %d through paper "
                "tooth\n",
                plain.first, plain.second, grained.second);
    check(plain.second > 1700 && grained.second < plain.second / 2,
          "grain: the paper tooth is applied on THIS route -- a grained clone stops being an "
          "exact copy wherever the tooth stands up, while the smooth one is exact everywhere. "
          "The predicate the BRUSH panel greys that group on is true because the call is "
          "there, not because a table says so");
  }

  // ======================================================================
  // 8. The routing table's CloneStamp rows (app/StrokeSession §1b)
  // ======================================================================
  //
  // Every one of these rows used to be `None` for the trivial reason that
  // `Tool::CloneStamp` sat in the not-built list -- so the tool did nothing,
  // said nothing, and the palette cell was greyed out over a tooltip saying so.
  {
    Layer rgbLayer = makeRgbLayer("r");
    Layer pigment = makePigmentLayer("p");
    Layer lockedRgb = makeRgbLayer("lr");
    lockedRgb.locked = true;
    Layer hiddenRgb = makeRgbLayer("hr");
    hiddenRgb.visible = false;
    Layer alphaLockedRgb = makeRgbLayer("alr");
    alphaLockedRgb.alphaLocked = true;
    Layer storelessRgb = makeRgbLayer("sr");
    storelessRgb.rgbTiles.reset();
    Layer adjustment = makeAdjustmentLayer("adj");
    Layer media = makeRgbLayer("m");
    media.kind = LayerKind::Media;
    media.rgbTiles.reset();

    check(strokeRouteFor(Tool::CloneStamp, &rgbLayer) == StrokeRoute::CloneStamp,
          "routing: the clone stamp on a writable RGB layer -> the clone route; this row used "
          "to say None, which is why the tool did nothing at all");
    check(strokeRouteFor(Tool::CloneStamp, &pigment) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, &pigment) == StrokeRoute::CpuDeposit,
          "routing: a Pigment layer REFUSES the clone by name while still taking the brush -- "
          "partial coverage there is a Kubelka-Munk mixture of two latents, not a lerp of four "
          "premultiplied channels, so it is a second module and not this one");
    check(strokeRouteFor(Tool::CloneStamp, nullptr) == StrokeRoute::None &&
              strokeRouteFor(Tool::Brush, nullptr) == StrokeRoute::PaintSim,
          "routing: no layer at all is None for the clone and PaintSim for the brush -- the "
          "solver canvas is not a tile store, so there is nothing to SAMPLE, and a clone sent "
          "there would lay down the foreground colour instead");
    check(strokeRouteFor(Tool::CloneStamp, &lockedRgb) == StrokeRoute::None,
          "routing: a locked RGB layer refuses the clone, exactly as it refuses every other "
          "stroke -- the lock is checked before the kind, so the message names the one thing "
          "a user can fix");
    check(strokeRouteFor(Tool::CloneStamp, &alphaLockedRgb) == StrokeRoute::CloneStamp &&
              strokeRouteFor(Tool::Eraser, &alphaLockedRgb) == StrokeRoute::None,
          "routing: an ALPHA-LOCKED layer still takes the clone while refusing the eraser -- "
          "the row where the two tools deliberately disagree, because alpha lock exists to "
          "permit colour changes and refuse alpha ones");
    check(strokeRouteFor(Tool::CloneStamp, &hiddenRgb) == StrokeRoute::CloneStamp,
          "routing: a HIDDEN RGB layer still clones -- visibility is a view decision, the same "
          "answer every other route gives");
    check(strokeRouteFor(Tool::CloneStamp, &storelessRgb) == StrokeRoute::None &&
              strokeRouteFor(Tool::CloneStamp, &adjustment) == StrokeRoute::None &&
              strokeRouteFor(Tool::CloneStamp, &media) == StrokeRoute::None,
          "routing: an RGB layer with no store, an Adjustment layer and a Media layer each "
          "refuse -- none of them holds texels to write, so each says so instead of silently "
          "doing nothing");
    check(std::string(strokeRouteName(StrokeRoute::CloneStamp)) == "clone-stamp" &&
              strokeRouteWritesLayer(StrokeRoute::CloneStamp) &&
              grainReachesRoute(StrokeRoute::CloneStamp) &&
              !wetnessReachesSolver(StrokeRoute::CloneStamp),
          "routing: the new route has a name of its own, answers the predicate four call sites "
          "ask, and is a grain route rather than a solver one -- a route that READS a layer as "
          "well as writing one still writes one");
    check(std::string(strokeEditLabel(Tool::CloneStamp)) == "clone stamp" &&
              std::string(strokeEditLabel(Tool::Brush)) == "brush stroke",
          "routing: and its history entry is labelled for what it did -- PRD O2's panel is "
          "scanned to find an edit to undo, and a clone filed under \"brush stroke\" is the "
          "row a user cannot find");
    check(toolBeginsStroke(Tool::CloneStamp) && toolHasCanvasHandler(Tool::CloneStamp) &&
              toolImplemented(Tool::CloneStamp) &&
              toolNoHandlerException(Tool::CloneStamp) == nullptr,
          "routing: the palette cell is live, and it is live through the same probe of "
          "strokeRouteFor() the canvas block is gated on -- no hand-written second table, and "
          "no recorded exception");
  }

  // ======================================================================
  // 9. The offset is snapped to whole texels (brush/CloneStamp §3)
  // ======================================================================
  //
  // A stated narrowing rather than an accident: §1's exactness claim only
  // exists at integer offsets, and a filter kernel is `ops/Resample`'s decision
  // rather than a deposit loop's. Asserted so the snapping is visible in a test
  // rather than discoverable by measuring a blur.
  {
    TileStore store;
    CloneStampStroke s;
    s.begin(store, Vec2{-1.5f, 2.4f}, 1.0f, false);
    check(s.offsetX() == -2 && s.offsetY() == 2,
          "offset: rounded to the nearest whole texel, away from zero at the half -- a "
          "truncating cast is asymmetric about zero, which would make a leftward clone and a "
          "rightward one of the same magnitude land differently");
    s.begin(store, Vec2{-0.5f, 0.5f}, 1.0f, false);
    check(s.offsetX() == -1 && s.offsetY() == 1,
          "offset: including at exactly half a texel in both directions");
    s.end();
  }

  std::printf("[selftest] clone stamp %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
