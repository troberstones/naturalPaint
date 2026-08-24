#include "app/selftest/Support.hpp"

#include <cmath>

#include "core/SelectionMask.hpp"
#include "core/SelectionOps.hpp"

namespace np {

// core/SelectionMask (PLAN.md "Phase 7 -- Select and paste"; PRD E1, E2, M1).
// Pure CPU, no PaintSim and no GPU -- the same headless-first-class status
// runPointOpsTest() and runShaperTest() have.
//
// The assertion this section exists for is section 1: the defaults are the
// INVERSE of core/Mask.hpp's, and getting them backwards does not show up as a
// wrong pixel. It shows up as an editor where either nothing can be painted or
// a selection does nothing at all, which is why they are checked directly and
// against the layer-mask convention by name.
bool runSelectionTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // --- 1. Absent, empty and full are three different things --------------
  {
    check(selectionCoverageAt(nullptr, PixelCoord{0, 0}) == 1.0f &&
              selectionCoverageAt(nullptr, PixelCoord{9999, -9999}) == 1.0f,
          "selection: NO selection means coverage 1.0 EVERYWHERE -- the absence of a "
          "restriction, not the absence of permission");

    Selection empty;
    check(selectionCoverageAt(&empty, PixelCoord{0, 0}) == 0.0f,
          "selection: an ENGAGED selection with no tiles covers nothing -- the opposite "
          "answer to the same question, which is the whole hazard in this file");
    check(selectionSelectsNothing(empty),
          "selection: and it says so by name, because 'why is nothing happening when I "
          "paint' needs an answer a UI can give");

    // The inverted default, checked against the layer-mask convention rather
    // than in isolation. core/Mask.hpp's maskCoverage(nullptr, ...) is 1.0;
    // this one is 0.0, and they are both right.
    check(selectionTileCoverage(nullptr, PixelCoord{0, 0}) == 0.0f &&
              maskCoverage(nullptr, PixelCoord{0, 0}) == 1.0f,
          "selection: an absent SELECTION tile is 0.0 while an absent MASK tile is 1.0 -- "
          "the two stores answer different questions and must not share a default");
  }

  // --- 2. Coverage, not a bitmask (PRD E2) --------------------------------
  {
    // A rectangle on exact texel boundaries: every covered texel is fully
    // covered, and the count is exact.
    const Selection whole = selectRectangle(10.0f, 20.0f, 14.0f, 23.0f);
    check(selectionCoverageAt(&whole, PixelCoord{10, 20}) == 1.0f &&
              selectionCoverageAt(&whole, PixelCoord{13, 22}) == 1.0f,
          "selection: an integer-aligned rectangle covers its interior at exactly 1.0 -- "
          "255/255, so a fully selected texel weights an edit by identity");
    check(selectionCoverageAt(&whole, PixelCoord{14, 20}) == 0.0f &&
              selectionCoverageAt(&whole, PixelCoord{10, 23}) == 0.0f,
          "selection: and the half-open far edge is NOT covered -- [10,14) is four texels, "
          "not five");
    check(!selectionSelectsNothing(whole), "selection: a real rectangle selects something");

    // The claim PRD E2 actually makes. A rectangle with fractional edges must
    // produce fractional coverage, and the value must be the covered AREA.
    const Selection frac = selectRectangle(10.25f, 20.0f, 12.0f, 21.0f);
    // Texel 10 spans [10,11) and the rect starts at 10.25 -> 0.75 covered.
    // uint8 quantisation: 0.75*255 + 0.5 = 191.75 -> 191 -> 191/255 = 0.74902.
    const float partial = selectionCoverageAt(&frac, PixelCoord{10, 20});
    std::printf("  [selftest] selection: fractional edge texel covers %.5f (exact 0.75, "
                "uint8 grid step %.5f)\n",
                static_cast<double>(partial), 1.0 / 255.0);
    check(near(partial, 0.75f, 1.0f / 255.0f) && partial != 0.0f && partial != 1.0f,
          "selection: a fractional edge gives FRACTIONAL coverage within one uint8 step of "
          "the true covered area -- this is the line between a coverage store and a bitmask");
    check(selectionCoverageAt(&frac, PixelCoord{11, 20}) == 1.0f,
          "selection: while the texel the edge does not cross is still fully covered");

    // Area, not a distance or an in-out test: a corner texel clipped on both
    // axes must be the PRODUCT of its two overlaps.
    const Selection corner = selectRectangle(10.5f, 20.5f, 12.0f, 22.0f);
    check(near(selectionCoverageAt(&corner, PixelCoord{10, 20}), 0.25f, 1.0f / 255.0f),
          "selection: a corner texel clipped on BOTH axes covers 0.5*0.5 = 0.25 -- the "
          "product, so it is a real area and not a per-axis fudge");

    // Quantisation is round-to-nearest, and 1.0 must survive it exactly.
    SelectionTile t;
    t.writeCoverage(PixelCoord{0, 0}, 1.0f);
    t.writeCoverage(PixelCoord{1, 0}, 0.0f);
    t.writeCoverage(PixelCoord{2, 0}, 2.0f);   // out of range, high
    t.writeCoverage(PixelCoord{3, 0}, -1.0f);  // out of range, low
    check(t.coverageAt(PixelCoord{0, 0}) == 1.0f && t.coverageAt(PixelCoord{1, 0}) == 0.0f,
          "selection: 1.0 and 0.0 round-trip through uint8 EXACTLY -- the two values that "
          "must not drift, because they are 'edit fully' and 'do not touch'");
    check(t.coverageAt(PixelCoord{2, 0}) == 1.0f && t.coverageAt(PixelCoord{3, 0}) == 0.0f,
          "selection: out-of-range coverage CLAMPS rather than wrapping -- an unsigned wrap "
          "would turn 'just over fully selected' into 'unselected', the worst rounding of "
          "that mistake");
  }

  // --- 3. A selection is sparse, and absence is free ----------------------
  {
    const Selection small = selectRectangle(0.0f, 0.0f, 4.0f, 4.0f);
    check(small.tiles.occupiedTileCount() == 1,
          "selection: a 4x4 marquee allocates ONE tile, not one per document tile -- the "
          "store is sparse and unselected space costs nothing");
    check(sizeof(SelectionTile) == 16 * 1024,
          "selection: and that tile is 16 KiB, half a layer mask tile (uint8, not f16 -- "
          "core/SelectionMask.hpp says why, and Photoshop stores selections at 8 bits too)");

    // A rectangle spanning a tile boundary must touch exactly two tiles.
    const Selection across = selectRectangle(126.0f, 0.0f, 130.0f, 2.0f);
    check(across.tiles.occupiedTileCount() == 2,
          "selection: a marquee crossing a tile boundary touches exactly the two tiles it "
          "overlaps");
    check(selectionCoverageAt(&across, PixelCoord{127, 0}) == 1.0f &&
              selectionCoverageAt(&across, PixelCoord{128, 0}) == 1.0f,
          "selection: and reads continuously across the seam -- tileCoordAt/tileLocalOffset "
          "agree with the constructor about where 128 lives");
  }

  // --- 4. Degenerate rectangles select NOTHING, not everything ------------
  {
    const Selection zeroWidth = selectRectangle(5.0f, 5.0f, 5.0f, 9.0f);
    check(zeroWidth.tiles.occupiedTileCount() == 0 && selectionSelectsNothing(zeroWidth),
          "selection: a zero-width rectangle selects NOTHING -- the dangerous alternative "
          "is that it reads as absent and therefore as select-all");
    check(selectionCoverageAt(&zeroWidth, PixelCoord{5, 5}) == 0.0f,
          "selection: and sampling it gives 0.0, so an edit through it lands nowhere");

    // Dragged up-and-left is a rectangle, not an error.
    const Selection backwards = selectRectangle(14.0f, 23.0f, 10.0f, 20.0f);
    check(backwards.tiles.occupiedTileCount() == 1 &&
              selectionCoverageAt(&backwards, PixelCoord{10, 20}) == 1.0f,
          "selection: a marquee dragged up-and-left normalises rather than refusing -- that "
          "is how half of all marquees are drawn");
  }

  // --- 5. PRD M1: clearing through coverage, with correct holes -----------
  {
    // A premultiplied opaque red field over one tile.
    auto makeField = []() {
      TileStore tiles;
      Tile& t = tiles.getOrCreate(TileCoord{0, 0});
      for (int32_t y = 0; y < kTileSize; ++y)
        for (int32_t x = 0; x < kTileSize; ++x)
          t.writePixel(PixelCoord{x, y}, {1.0f, 0.0f, 0.0f, 1.0f});
      return tiles;
    };

    // Full coverage over part of the tile: those texels go to nothing, the
    // rest are untouched.
    {
      TileStore tiles = makeField();
      const Selection sel = selectRectangle(0.0f, 0.0f, 8.0f, 8.0f);
      const size_t changed = clearThroughSelection(tiles, &sel);
      const Tile* t = tiles.find(TileCoord{0, 0});
      check(changed == 64 && t != nullptr,
            "clear: a fully-covered 8x8 marquee changes exactly its 64 texels");
      const std::array<float, 4> inside = t->readPixel(PixelCoord{3, 3});
      const std::array<float, 4> outside = t->readPixel(PixelCoord{9, 9});
      check(inside[0] == 0.0f && inside[3] == 0.0f,
            "clear: inside the selection the texel is gone -- colour AND alpha, which is "
            "what makes it a hole rather than a black square");
      check(outside[0] == 1.0f && outside[3] == 1.0f,
            "clear: and one texel outside is untouched");
    }

    // The requirement's actual words: coverage-weighted, premultiply-correct.
    {
      TileStore tiles = makeField();
      const Selection sel = selectRectangle(0.25f, 0.0f, 4.0f, 4.0f);
      clearThroughSelection(tiles, &sel);
      const Tile* t = tiles.find(TileCoord{0, 0});
      // Texel 0 was 0.75 covered, so 0.25 of it remains.
      const std::array<float, 4> edge = t != nullptr ? t->readPixel(PixelCoord{0, 0})
                                                     : std::array<float, 4>{-1, -1, -1, -1};
      std::printf("  [selftest] clear: 0.75-covered texel left rgba(%.4f, %.4f, %.4f, %.4f)\n",
                  static_cast<double>(edge[0]), static_cast<double>(edge[1]),
                  static_cast<double>(edge[2]), static_cast<double>(edge[3]));
      check(near(edge[3], 0.25f, 2.0f / 255.0f),
            "clear: a 0.75-covered texel keeps 0.25 of its alpha -- the clear is WEIGHTED by "
            "coverage, not thresholded by it");
      // The premultiply-correctness claim, and the one that would silently
      // fail: R must fall with A, so un-premultiplying still gives pure red.
      // If RGB were left alone while A dropped, this texel would read as a
      // 4x-overbright red -- the fringe around every feathered deletion.
      check(near(edge[0], edge[3], 1e-4f),
            "clear: and its RED falls with its ALPHA in lockstep, because the store is "
            "premultiplied -- un-premultiplying still gives pure red, so no fringe");
      const float unpremult = edge[3] > 0.0f ? edge[0] / edge[3] : 0.0f;
      check(near(unpremult, 1.0f, 1e-3f),
            "clear: proved by dividing it back out -- the colour under the hole is unchanged, "
            "only how much of it is there");
    }

    // A null selection clears everything: Select All is the default.
    {
      TileStore tiles = makeField();
      const size_t changed = clearThroughSelection(tiles, nullptr);
      check(changed == static_cast<size_t>(kTileSize) * kTileSize,
            "clear: with NO selection, Delete empties the whole layer -- consistent with "
            "coverage 1.0 everywhere, and with every editor ever shipped");
    }

    // An engaged-but-empty selection clears nothing. The mirror of the above,
    // and the assertion that would fail if the two absences were confused.
    {
      TileStore tiles = makeField();
      Selection nothingSelected;
      const size_t changed = clearThroughSelection(tiles, &nothingSelected);
      const Tile* t = tiles.find(TileCoord{0, 0});
      check(changed == 0 && t != nullptr && t->readPixel(PixelCoord{5, 5})[3] == 1.0f,
            "clear: with an EMPTY selection, Delete does nothing at all -- the exact "
            "opposite of the null case, from a one-word difference at the call site");
    }

    // Sparseness is not just a memory claim: a clear through a small marquee
    // must not unshare tiles it never touches. This is what makes clearing
    // through a marquee on a 4K document cheap (PRD M5's neighbourhood).
    {
      TileStore tiles = makeField();
      tiles.getOrCreate(TileCoord{5, 5}).writePixel(PixelCoord{0, 0}, {1, 1, 1, 1});
      TileStore shared = tiles;  // copy-on-write: every tile shared
      check(shared.isTileShared(TileCoord{5, 5}),
            "clear: (fixture) a copied store shares its tiles");
      const Selection sel = selectRectangle(0.0f, 0.0f, 4.0f, 4.0f);
      clearThroughSelection(shared, &sel);
      check(shared.isTileShared(TileCoord{5, 5}),
            "clear: a clear through a small marquee leaves the FAR tile still shared -- it "
            "is skipped before findForWrite(), so clearing does not deep-copy the document");
      check(!shared.isTileShared(TileCoord{0, 0}),
            "clear: while the tile it did touch was unshared, so the original is unharmed");
      const Tile* original = tiles.find(TileCoord{0, 0});
      check(original != nullptr && original->readPixel(PixelCoord{0, 0})[3] == 1.0f,
            "clear: and the ORIGINAL store still has its paint -- copy-on-write held");
    }
  }

  // --- 6. Boolean combination (PRD E7) and invert (PRD E4) ----------------
  //
  // core/SelectionOps. The rules are a *choice* between defensible bounds on
  // an unknowable overlap, so what is checked here is the three algebraic
  // properties that choice was made for -- not the arithmetic, which would
  // just be the implementation retyped.
  {
    // The modifier table, all four rows. Cheap to assert and expensive to get
    // wrong: an inverted row does not crash, it throws away the selection the
    // user was extending.
    check(selectionCombineFromModifiers(false, false) == SelectionCombine::Replace &&
              selectionCombineFromModifiers(true, false) == SelectionCombine::Add &&
              selectionCombineFromModifiers(false, true) == SelectionCombine::Subtract &&
              selectionCombineFromModifiers(true, true) == SelectionCombine::Intersect,
          "combine: the modifier table is Shift=add, Option=subtract, both=intersect, "
          "neither=replace -- the convention every editor shares, asserted because hands "
          "know it better than eyes do");

    // Two overlapping rectangles, on integer edges so every texel is fully in
    // or fully out and the set-theoretic answers are exact. The antialiased
    // case gets its own checks below.
    const Selection a = selectRectangle(0.0f, 0.0f, 10.0f, 10.0f);
    const Selection b = selectRectangle(5.0f, 5.0f, 15.0f, 15.0f);
    const PixelCoord onlyA{1, 1}, both{7, 7}, onlyB{12, 12}, neither{20, 20};

    const Selection added = combineSelections(a, b, SelectionCombine::Add);
    check(selectionCoverageAt(&added, onlyA) == 1.0f &&
              selectionCoverageAt(&added, both) == 1.0f &&
              selectionCoverageAt(&added, onlyB) == 1.0f &&
              selectionCoverageAt(&added, neither) == 0.0f,
          "combine: Add is the union -- Shift-drag keeps what was there and adds what "
          "was drawn");

    const Selection subtracted = combineSelections(a, b, SelectionCombine::Subtract);
    check(selectionCoverageAt(&subtracted, onlyA) == 1.0f &&
              selectionCoverageAt(&subtracted, both) == 0.0f &&
              selectionCoverageAt(&subtracted, onlyB) == 0.0f,
          "combine: Subtract removes the NEW shape from the old one -- Option-drag, and "
          "not the reverse");

    const Selection intersected = combineSelections(a, b, SelectionCombine::Intersect);
    check(selectionCoverageAt(&intersected, onlyA) == 0.0f &&
              selectionCoverageAt(&intersected, both) == 1.0f &&
              selectionCoverageAt(&intersected, onlyB) == 0.0f,
          "combine: Intersect keeps only the overlap -- Shift+Option");

    const Selection replaced = combineSelections(a, b, SelectionCombine::Replace);
    check(selectionCoverageAt(&replaced, onlyA) == 0.0f &&
              selectionCoverageAt(&replaced, onlyB) == 1.0f,
          "combine: Replace discards the base entirely, so an unmodified drag is not a "
          "special case at the call site");

    // Property 1: idempotence. The reason Add is max() and not a saturating
    // sum -- repeating a gesture must not harden an antialiased edge. Checked
    // on a FRACTIONAL rectangle, because that is the only place the two rules
    // differ at all.
    {
      const Selection frac = selectRectangle(0.25f, 0.25f, 4.75f, 4.75f);
      const Selection twice = combineSelections(frac, frac, SelectionCombine::Add);
      const float before = selectionCoverageAt(&frac, PixelCoord{0, 0});
      const float after = selectionCoverageAt(&twice, PixelCoord{0, 0});
      check(before > 0.0f && before < 1.0f && after == before,
            "combine: adding the SAME selection twice changes nothing -- an edge texel at "
            "partial coverage stays there, so repeating a Shift-drag does not quietly "
            "harden the antialiasing");
    }

    // Property 2: the De Morgan identity core/SelectionOps.hpp claims in
    // prose. Subtract is defined as intersect-with-complement, and this is
    // what stops it drifting into an independently-invented third formula.
    {
      const Selection viaSubtract = combineSelections(a, b, SelectionCombine::Subtract);
      const Selection notB = invertSelection(b, 32, 32);
      const Selection viaIntersect =
          combineSelections(a, notB, SelectionCombine::Intersect);
      bool identical = true;
      for (int32_t y = 0; y < 20 && identical; ++y) {
        for (int32_t x = 0; x < 20; ++x) {
          if (selectionCoverageAt(&viaSubtract, PixelCoord{x, y}) !=
              selectionCoverageAt(&viaIntersect, PixelCoord{x, y})) {
            identical = false;
            break;
          }
        }
      }
      check(identical,
            "combine: subtract(a,b) is EXACTLY intersect(a, invert(b)) -- the De Morgan "
            "closure the header claims, asserted rather than trusted");
    }

    // Property 3: associativity, so a chain of adds does not depend on the
    // order the user happened to draw them in.
    {
      const Selection c = selectRectangle(12.0f, 0.0f, 20.0f, 8.0f);
      const Selection left = combineSelections(
          combineSelections(a, b, SelectionCombine::Add), c, SelectionCombine::Add);
      const Selection right = combineSelections(
          a, combineSelections(b, c, SelectionCombine::Add), SelectionCombine::Add);
      bool identical = true;
      for (int32_t y = 0; y < 24 && identical; ++y) {
        for (int32_t x = 0; x < 24; ++x) {
          if (selectionCoverageAt(&left, PixelCoord{x, y}) !=
              selectionCoverageAt(&right, PixelCoord{x, y})) {
            identical = false;
            break;
          }
        }
      }
      check(identical, "combine: Add is associative -- three drags in any order agree");
    }

    // Antialiasing survives a boolean rather than being rounded to in-or-out,
    // which is the whole of PRD E2 applied to PRD E7.
    {
      const Selection soft = selectRectangle(0.0f, 0.0f, 4.5f, 8.0f);
      const Selection full = selectRectangle(0.0f, 0.0f, 8.0f, 8.0f);
      const Selection kept = combineSelections(full, soft, SelectionCombine::Intersect);
      check(near(selectionCoverageAt(&kept, PixelCoord{4, 2}), 0.5f, 1.0f / 255.0f),
            "combine: a half-covered edge texel is still half-covered after an intersect "
            "-- booleans preserve coverage, they do not quantise it");
    }

    // Sparsity, which is the other half of the design. Subtract and intersect
    // can drive a whole tile to zero, and core/SelectionMask's invariant is
    // that no stored tile is entirely zero.
    {
      const Selection far1 = selectRectangle(0.0f, 0.0f, 8.0f, 8.0f);
      const Selection far2 = selectRectangle(600.0f, 600.0f, 640.0f, 640.0f);
      const Selection none = combineSelections(far1, far2, SelectionCombine::Intersect);
      check(none.tiles.occupiedTileCount() == 0,
            "combine: intersecting two disjoint selections stores NO tiles -- not tiles "
            "full of zeros");
      check(selectionSelectsNothing(none),
            "combine: and the result is 'engaged, selects nothing', which is the honest "
            "answer rather than 'no restriction'");

      const Selection wiped = combineSelections(far1, far1, SelectionCombine::Subtract);
      check(wiped.tiles.occupiedTileCount() == 0,
            "combine: subtracting a selection from itself drops the tile rather than "
            "leaving 16 KiB of 'not selected' resident");

      // Add must not touch tiles neither input has.
      const Selection joined = combineSelections(far1, far2, SelectionCombine::Add);
      check(joined.tiles.occupiedTileCount() == far1.tiles.occupiedTileCount() +
                                                    far2.tiles.occupiedTileCount(),
            "combine: Add allocates exactly the union of the two tile sets, and nothing "
            "in the empty space between them");
    }

    // Invert (PRD E4).
    {
      const Selection quarter = selectRectangle(0.0f, 0.0f, 100.0f, 100.0f);
      const Selection inv = invertSelection(quarter, 200, 200);
      check(selectionCoverageAt(&inv, PixelCoord{50, 50}) == 0.0f &&
                selectionCoverageAt(&inv, PixelCoord{150, 150}) == 1.0f,
            "invert: what was selected is not, and what was not is");
      check(selectionCoverageAt(&inv, PixelCoord{250, 250}) == 0.0f,
            "invert: and OUTSIDE the document stays unselected -- the complement is taken "
            "within the given bounds, not over the infinite plane");

      const Selection back = invertSelection(inv, 200, 200);
      bool roundTrip = true;
      for (int32_t y = 0; y < 200 && roundTrip; y += 7) {
        for (int32_t x = 0; x < 200; x += 7) {
          if (selectionCoverageAt(&back, PixelCoord{x, y}) !=
              selectionCoverageAt(&quarter, PixelCoord{x, y})) {
            roundTrip = false;
            break;
          }
        }
      }
      check(roundTrip, "invert: inverting twice returns the original exactly");

      // A document whose size is not a multiple of the 128-texel tile: the
      // margin inside the edge tiles must NOT come back selected.
      const Selection ragged = invertSelection(Selection{}, 200, 130);
      check(selectionCoverageAt(&ragged, PixelCoord{199, 129}) == 1.0f &&
                selectionCoverageAt(&ragged, PixelCoord{200, 129}) == 0.0f &&
                selectionCoverageAt(&ragged, PixelCoord{199, 130}) == 0.0f,
            "invert: on a document that is not a whole number of tiles, the margin inside "
            "the edge tiles is unselected -- coverage stops at the document, not at the "
            "tile boundary");

      const Selection everything = selectAll(64, 64);
      check(selectionSelectsNothing(invertSelection(everything, 64, 64)),
            "invert: the complement of Select All selects nothing -- engaged and empty, "
            "which is why the UI refuses to invert when there is no selection at all");
    }
  }

  std::printf("[selftest] selection %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
