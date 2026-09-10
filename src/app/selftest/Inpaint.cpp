#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "app/FilterOps.hpp"
#include "ops/Inpaint.hpp"
#include "ops/Roi.hpp"
#include "ui/MenuModel.hpp"

namespace np {

namespace {

std::array<float, 4> inpaintReadAt(const TileStore& store, int32_t x, int32_t y) {
  const Tile* t = store.find(tileCoordAt(PixelCoord{x, y}));
  if (t == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
  return t->readPixel(tileLocalOffset(PixelCoord{x, y}));
}

void inpaintWriteAt(TileStore& store, int32_t x, int32_t y, const std::array<float, 4>& rgba) {
  store.getOrCreate(tileCoordAt(PixelCoord{x, y}))
      .writePixel(tileLocalOffset(PixelCoord{x, y}), rgba);
}

// Fully select one texel. `selectRectangle()` covers the rectangular cases;
// this covers a scratch, which is the shape the feature is actually for and
// which is not a rectangle.
void inpaintSelectTexel(Selection& sel, int32_t x, int32_t y) {
  sel.tiles.getOrCreate(tileCoordAt(PixelCoord{x, y}))
      .writeCoverage(tileLocalOffset(PixelCoord{x, y}), 1.0f);
}

bool inpaintTilesExactlyEqual(const TileStore& a, const TileStore& b) {
  if (a.occupiedTileCount() != b.occupiedTileCount()) return false;
  for (const auto& [coord, tile] : a) {
    const Tile* other = b.find(coord);
    if (other == nullptr) return false;
    if (std::memcmp(tile.data(), other->data(), Tile::kTexelCount * sizeof(uint16_t)) != 0)
      return false;
  }
  return true;
}

// A `w` x `h` opaque field whose value is a pure horizontal ramp -- it depends
// on x and on nothing else. Chosen over noise for section D, because a fill
// that continues its surroundings has a *predictable* answer on a ramp (the
// ramp), and predictable is the difference between "it filled with something"
// and "it filled with the right thing".
void fillInpaintRamp(TileStore& tiles, int32_t w, int32_t h) {
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      const float v = static_cast<float>(x) / static_cast<float>(w - 1);
      inpaintWriteAt(tiles, x, y, {v, 0.25f + 0.5f * v, 1.0f - v, 1.0f});
    }
  }
}

const MenuNode* inpaintFindMenuAction(const std::vector<MenuNode>& nodes, MenuAction action) {
  for (const MenuNode& n : nodes) {
    if (n.action == action) return &n;
    if (const MenuNode* found = inpaintFindMenuAction(n.children, action)) return found;
  }
  return nullptr;
}

}  // namespace

// PLAN.md "Phase 8 -- Repair it" / PRD D7's first half: ops/Inpaint's
// diffusion fill (Telea), and the Filter > Inpaint command that reaches it.
//
// **What this section is about, in one sentence, because every assertion
// below is a consequence of it:** for every other pixel op in this build the
// selection BOUNDS where a result lands; for this one the selection IS the
// hole -- the texels whose data is wrong, filled from data outside them.
// Section A pins that inversion in the parameters, B at the engine, C at the
// menu command, and D shows it doing the job PRD D7 names.
//
// It deliberately does NOT re-derive Telea's arithmetic against a second
// implementation of Telea's arithmetic; this suite's own repeated lesson is
// that a test which reimplements its subject tests a copy of it. What it
// asserts instead are the properties the arithmetic was chosen for, each of
// which a wrong implementation cannot accidentally have: the hole's own
// content is never read (fed two different holes, demanded bit-identical
// output), every filled texel is inside the convex hull of what it read, a
// flat field fills flat, and a ramp fills as a ramp rather than as a puddle of
// its rim's mean.
bool runInpaintTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // The f16 store's own floor, restated from app/selftest/Filters.cpp section
  // 0: half an ulp just below 1.0 is 2^-12. Every tolerance below is built
  // from it rather than picked by feel.
  const float halfFloor = static_cast<float>(std::ldexp(1.0, -12));

  std::printf("  -- A. the parameters, and the inverted default -----------------\n");
  {
    Selection sel = selectRectangle(10.0f, 10.0f, 20.0f, 20.0f);
    InpaintParams good;
    good.hole = &sel;
    check(inpaintParamsValid(good),
          "params: a real hole and the default radius is a valid request");

    // **ops/Inpaint.hpp's whole first section, as an assertion.** A null
    // `Selection*` means "no restriction" to every other reader in this
    // codebase; read that way here it would mean "the entire document is
    // damage". This is the line that goes red if someone ever "fixes" the
    // inconsistency by making inpaint behave like its neighbours.
    InpaintParams noHole = good;
    noHole.hole = nullptr;
    check(!inpaintParamsValid(noHole),
          "params: a NULL selection is refused -- no hole, NOT the whole canvas, the exact "
          "inverse of what it means to every other op here");

    Selection engagedButEmpty;
    InpaintParams emptyHole = good;
    emptyHole.hole = &engagedButEmpty;
    check(selectionSelectsNothing(engagedButEmpty) && !inpaintParamsValid(emptyHole),
          "params: an ENGAGED selection that selects nothing is refused too -- a distinct "
          "state from no selection, and equally nothing to repair");

    InpaintParams badRadius = good;
    badRadius.radius = 0;
    const bool zeroRefused = !inpaintParamsValid(badRadius);
    badRadius.radius = kInpaintMaxRadius + 1;
    const bool hugeRefused = !inpaintParamsValid(badRadius);
    badRadius.radius = -3;
    const bool negRefused = !inpaintParamsValid(badRadius);
    check(zeroRefused && hugeRefused && negRefused,
          "params: radius 0, negative and past kInpaintMaxRadius are all refused by name "
          "rather than clamped -- ops/Blur.hpp's rule for a negative sigma");

    const std::optional<PixelRect> bounds = inpaintHoleBounds(good);
    check(bounds.has_value() && bounds->x0 == 10 && bounds->y0 == 10 && bounds->x1 == 20 &&
              bounds->y1 == 20,
          "params: inpaintHoleBounds() is the tightest rectangle over the selected texels");
    check(!inpaintHoleBounds(noHole).has_value(),
          "params: no hole gives nullopt bounds, not an empty rectangle at the origin");
  }

  std::printf("  -- B. the engine: what it reads, and what it writes ------------\n");
  {
    TileStore field;
    fillInpaintRamp(field, 256, 256);

    // A 9x9 square hole in the middle of the ramp. The SOURCE is damaged
    // inside it with an obviously wrong colour, so "the fill ignored the
    // hole's own data" is a claim with something to ignore.
    const PixelRect holeRect{100, 100, 109, 109};
    Selection sel;
    for (int32_t y = holeRect.y0; y < holeRect.y1; ++y)
      for (int32_t x = holeRect.x0; x < holeRect.x1; ++x) inpaintSelectTexel(sel, x, y);

    TileStore damaged = field;
    for (int32_t y = holeRect.y0; y < holeRect.y1; ++y)
      for (int32_t x = holeRect.x0; x < holeRect.x1; ++x)
        inpaintWriteAt(damaged, x, y, {0.0f, 0.0f, 0.0f, 1.0f});

    InpaintParams params;
    params.hole = &sel;
    params.radius = 5;
    const PixelRect canvas{0, 0, 256, 256};

    TileStore filled;
    check(inpaintTiles(damaged, canvas, params, &filled),
          "engine: a well-formed request over the whole canvas succeeds");

    // --- Refusals, by name ------------------------------------------------
    {
      TileStore dst;
      TileStore alias = damaged;
      InpaintParams noHole = params;
      noHole.hole = nullptr;
      const bool nullDst = !inpaintTiles(damaged, canvas, params, nullptr);
      const bool aliased = !inpaintTiles(alias, canvas, params, &alias);
      const bool emptyRect = !inpaintTiles(damaged, roiEmptyRect(), params, &dst);
      const bool noHoleRefused = !inpaintTiles(damaged, canvas, noHole, &dst);
      check(nullDst && aliased && emptyRect && noHoleRefused,
            "engine: a null destination, an aliased destination, an empty rectangle and a "
            "null hole are each refused and write nothing");
      check(dst.occupiedTileCount() == 0,
            "engine: a refused request allocated not one tile in the destination");

      // ops/Inpaint.hpp section 4: this op has no `RoiOp` because its
      // dependency is the whole hole component, so a request that does not
      // CONTAIN the hole is refused rather than answered with a fill that
      // depends on where the caller cut. A blur would happily serve this
      // rectangle; that is exactly the difference.
      TileStore partial;
      const PixelRect halfHole{0, 0, 104, 256};
      check(!inpaintTiles(damaged, halfHole, params, &partial) &&
                partial.occupiedTileCount() == 0,
            "engine: a rectangle that CUTS the hole is refused -- inpaint cannot be "
            "evaluated in pieces the way an aproned filter can");
    }

    // --- It writes the hole and nothing else ------------------------------
    {
      bool onlyHoleWritten = true;
      bool everyHoleTexelWritten = true;
      size_t tilesOutsideHole = 0;
      const TileRange holeTiles = roiTileRange(holeRect);
      for (const auto& [coord, tile] : filled) {
        (void)tile;
        if (coord.x < holeTiles.x0 || coord.x >= holeTiles.x1 || coord.y < holeTiles.y0 ||
            coord.y >= holeTiles.y1) {
          ++tilesOutsideHole;
        }
      }
      for (int32_t y = 90; y < 120 && onlyHoleWritten; ++y) {
        for (int32_t x = 90; x < 120; ++x) {
          const bool inHole = x >= holeRect.x0 && x < holeRect.x1 && y >= holeRect.y0 &&
                              y < holeRect.y1;
          const std::array<float, 4> v = inpaintReadAt(filled, x, y);
          const bool untouched = v == std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
          if (!inHole && !untouched) {
            onlyHoleWritten = false;
            break;
          }
          if (inHole && untouched) everyHoleTexelWritten = false;
        }
      }
      check(tilesOutsideHole == 0 && onlyHoleWritten,
            "engine: only the HOLE is written -- not one tile outside its bounding box, and "
            "not one texel of a straddling tile that the selection excludes");
      check(everyHoleTexelWritten,
            "engine: every texel of the hole was reached -- no unfilled island left behind by "
            "the march");
    }

    // --- **The one that proves the inversion.** ---------------------------
    //
    // Two sources identical everywhere OUTSIDE the hole and wildly different
    // inside it must produce BIT-IDENTICAL fills. Bit-identical and not
    // within-tolerance, because "the hole's own texels are never read" is an
    // exactness claim: if one of them contributed with any weight at all, two
    // different values would give two different sums.
    {
      TileStore otherDamage = field;
      for (int32_t y = holeRect.y0; y < holeRect.y1; ++y)
        for (int32_t x = holeRect.x0; x < holeRect.x1; ++x)
          inpaintWriteAt(otherDamage, x, y, {0.9f, 0.1f, 0.7f, 1.0f});
      TileStore otherFilled;
      inpaintTiles(otherDamage, canvas, params, &otherFilled);
      check(inpaintTilesExactlyEqual(filled, otherFilled),
            "engine: two documents differing ONLY inside the hole fill BIT-IDENTICALLY -- the "
            "selected texels' own data is never read, which is what makes this an inpaint "
            "rather than a filter");

      // Proved sensitive rather than merely satisfied: the two inputs really
      // did differ, and the fill really did replace the damage.
      check(!inpaintTilesExactlyEqual(damaged, otherDamage),
            "engine: the two damaged sources genuinely differ, so the assertion above had "
            "something to be insensitive to");
      const std::array<float, 4> centre = inpaintReadAt(filled, 104, 104);
      check(std::fabs(centre[0] - 0.0f) > 0.05f,
            "engine: the hole's centre is no longer the black it was damaged with");
    }

    // --- Convex hull, and the premultiplied invariant ---------------------
    //
    // ops/Inpaint.hpp section 2: every weight is strictly positive and the
    // sum divides out, so a filled texel is a convex combination of texels
    // that were already known -- inductively, of the ORIGINAL known texels.
    // The consequences are checkable without knowing a single weight.
    {
      float lo[4] = {1e9f, 1e9f, 1e9f, 1e9f};
      float hi[4] = {-1e9f, -1e9f, -1e9f, -1e9f};
      for (int32_t y = holeRect.y0 - 6; y < holeRect.y1 + 6; ++y) {
        for (int32_t x = holeRect.x0 - 6; x < holeRect.x1 + 6; ++x) {
          if (x >= holeRect.x0 && x < holeRect.x1 && y >= holeRect.y0 && y < holeRect.y1)
            continue;
          const std::array<float, 4> v = inpaintReadAt(damaged, x, y);
          for (int c = 0; c < 4; ++c) {
            lo[c] = std::min(lo[c], v[c]);
            hi[c] = std::max(hi[c], v[c]);
          }
        }
      }
      bool insideHull = true;
      bool premultiplied = true;
      for (int32_t y = holeRect.y0; y < holeRect.y1; ++y) {
        for (int32_t x = holeRect.x0; x < holeRect.x1; ++x) {
          const std::array<float, 4> v = inpaintReadAt(filled, x, y);
          for (int c = 0; c < 4; ++c)
            if (v[c] < lo[c] - halfFloor || v[c] > hi[c] + halfFloor) insideHull = false;
          for (int c = 0; c < 3; ++c)
            if (v[c] < -halfFloor || v[c] > v[3] + halfFloor) premultiplied = false;
        }
      }
      check(insideHull,
            "engine: EVERY filled texel is inside the per-channel range of the known texels "
            "around the hole -- a convex combination cannot overshoot, and there is no clamp "
            "hiding one that did");
      check(premultiplied,
            "engine: every filled texel satisfies 0 <= RGB <= A -- the premultiplied "
            "invariant is a set of linear inequalities, so a convex combination preserves it "
            "exactly");
    }

    // --- A flat field fills flat ------------------------------------------
    {
      TileStore flat;
      const std::array<float, 4> constant{0.375f, 0.5f, 0.625f, 1.0f};
      for (int32_t y = 0; y < 128; ++y)
        for (int32_t x = 0; x < 128; ++x) inpaintWriteAt(flat, x, y, constant);
      Selection flatHole;
      for (int32_t y = 60; y < 69; ++y)
        for (int32_t x = 60; x < 69; ++x) {
          inpaintSelectTexel(flatHole, x, y);
          inpaintWriteAt(flat, x, y, {0.0f, 1.0f, 0.0f, 1.0f});
        }
      InpaintParams flatParams;
      flatParams.hole = &flatHole;
      flatParams.radius = 5;
      TileStore flatFilled;
      inpaintTiles(flat, PixelRect{0, 0, 128, 128}, flatParams, &flatFilled);

      float worst = 0.0f;
      for (int32_t y = 60; y < 69; ++y)
        for (int32_t x = 60; x < 69; ++x) {
          const std::array<float, 4> v = inpaintReadAt(flatFilled, x, y);
          for (int c = 0; c < 4; ++c) worst = std::max(worst, std::fabs(v[c] - constant[c]));
        }
      // The bound is the store's own half-ulp, not a hand-picked epsilon: a
      // convex combination of identical values IS that value, so the only
      // error left is the f16 the answer is written into.
      check(worst <= halfFloor,
            "engine: a hole in a CONSTANT field fills with exactly that constant, to within "
            "the f16 store's own 2.44e-4 floor -- a convex combination of equal values");
      std::printf("  [measured] flat-field fill: worst channel deviation %.3e (f16 floor %.3e)\n",
                  static_cast<double>(worst), static_cast<double>(halfFloor));
    }

    // --- A ramp fills as a ramp -------------------------------------------
    //
    // The fill continues the structure it is surrounded by rather than
    // averaging the rim into one value. On a pure horizontal ramp the answer
    // is monotone left to right across the hole, and its spread across the
    // hole is close to the ramp's own spread over that width. A fill that
    // collapsed to the rim's mean would be flat, and a fill that read the
    // hole's own black would be dark; both are excluded by the same two
    // numbers.
    {
      const int32_t midY = 104;
      bool monotone = true;
      for (int32_t x = holeRect.x0 + 1; x < holeRect.x1; ++x) {
        if (inpaintReadAt(filled, x, midY)[0] <= inpaintReadAt(filled, x - 1, midY)[0])
          monotone = false;
      }
      const float leftMost = inpaintReadAt(filled, holeRect.x0, midY)[0];
      const float rightMost = inpaintReadAt(filled, holeRect.x1 - 1, midY)[0];
      const float spread = rightMost - leftMost;
      const float wanted = static_cast<float>(holeRect.x1 - 1 - holeRect.x0) / 255.0f;
      check(monotone,
            "engine: across a hole in a horizontal ramp the fill increases strictly left to "
            "right -- it continues the surrounding structure, it does not average the rim");
      check(spread > 0.5f * wanted && spread < 1.5f * wanted,
            "engine: and the fill's spread across the hole is within 50% of the ramp's own "
            "spread over that width -- so 'monotone' is not a hairline tilt on a puddle");
      std::printf("  [measured] ramp fill across a 9-texel hole: spread %.5f, ramp %.5f\n",
                  static_cast<double>(spread), static_cast<double>(wanted));
    }
  }

  std::printf("  -- C. the command: one bridge, one selection, one entry --------\n");
  {
    // No selection at all. **The refusal that this whole feature turns on**:
    // for every other filter this state means "run over everything", and here
    // it must mean "there is nothing to repair" and say so by name.
    {
      OpenDocument od = makeBlankOpenDocument(128, 128, WorkingSpace{}, "inpaint no selection");
      fillInpaintRamp(*od.document.layers[0].rgbTiles, 128, 128);
      od.recordEdit("inpaint fixture field", EditKind::Content);
      const TileStore before = *od.document.layers[0].rgbTiles;
      const size_t entriesBefore = od.history.entries().size();
      const uint64_t revisionBefore = od.revision;

      const FilterOpResult r = applyInpaint(od, 5);
      check(r.refusal == PixelOpRefusal::NoSelection && r.texelsChanged == 0,
            "command: with NO selection, inpaint refuses NoSelection -- it does not run over "
            "the whole layer the way every other filter correctly would");
      check(od.history.entries().size() == entriesBefore && od.revision == revisionBefore &&
                inpaintTilesExactlyEqual(*od.document.layers[0].rgbTiles, before),
            "command: the refusal recorded no history entry, moved no revision and left the "
            "layer bit-identical");

      // An engaged-but-empty selection is the second half of the same rule,
      // and core/SelectionMask.hpp is explicit that it is a different state.
      od.selection = Selection{};
      check(applyInpaint(od, 5).refusal == PixelOpRefusal::NoSelection,
            "command: an engaged selection that selects nothing refuses the same way -- "
            "'select all then deselect the world' is still no hole");
    }

    // The refusal ORDER, and the sentence.
    {
      OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "inpaint refusal order");
      const size_t at = od.document.layers.size();
      recordLayerEdit(od, addLayer(od.document, at, makeRgbLayer("Backdrop")));
      recordLayerEdit(od, setLayerLocked(od.document, at, true));
      od.activeLayer = at;
      check(inpaintRefusal(od) == PixelOpRefusal::Locked,
            "command: a LOCKED layer with no selection refuses for the lock, not for the "
            "selection -- name the problem with a switch next to it first");

      OpenDocument pigment = makeBlankOpenDocument(64, 64, WorkingSpace{}, "inpaint pigment");
      const size_t pat = pigment.document.layers.size();
      recordLayerEdit(pigment, addLayer(pigment.document, pat, makePigmentLayer("Latents")));
      pigment.activeLayer = pat;
      pigment.selection = selectRectangle(10.0f, 10.0f, 20.0f, 20.0f);
      check(applyInpaint(pigment, 5).refusal == PixelOpRefusal::NoRgbStore,
            "command: a Pigment layer refuses NoRgbStore -- the SAME vocabulary the paint "
            "bucket and the other ten filters use, not a second one");

      const Layer* target = activeLayerOf(od);
      const std::string noSel = pixelOpRefusalMessage(PixelOpRefusal::NoSelection, target, "inpaint");
      const std::string locked = pixelOpRefusalMessage(PixelOpRefusal::Locked, target, "inpaint");
      const std::string noStore =
          pixelOpRefusalMessage(PixelOpRefusal::NoRgbStore, target, "inpaint");
      const std::string noLayer =
          pixelOpRefusalMessage(PixelOpRefusal::NoLayer, nullptr, "inpaint");
      check(!noSel.empty() && noSel != locked && noSel != noStore && noSel != noLayer,
            "command: NoSelection has its own sentence, distinguishable from all three "
            "layer-shaped ones -- four reasons, four things to do about them");
    }

    // The whole path: a scratch, selected, repaired, recorded, undoable.
    {
      OpenDocument od = makeBlankOpenDocument(128, 128, WorkingSpace{}, "inpaint scratch");
      TileStore& layer = *od.document.layers[0].rgbTiles;
      fillInpaintRamp(layer, 128, 128);
      Selection scratch;
      for (int32_t i = 20; i < 100; ++i) {
        inpaintWriteAt(layer, i, 64, {0.0f, 0.0f, 0.0f, 1.0f});
        inpaintSelectTexel(scratch, i, 64);
      }
      od.recordEdit("inpaint fixture field", EditKind::Content);
      od.selection = scratch;

      const TileStore before = layer;
      const size_t entriesBefore = od.history.entries().size();

      // The engine, called directly with the same hole, is the reference the
      // command is held to -- so this asserts the WIRING (does the menu
      // command reach ops/Inpaint with the document's own selection?) rather
      // than re-asserting the arithmetic section B already covered.
      InpaintParams direct;
      direct.hole = &*od.selection;
      direct.radius = 5;
      TileStore reference;
      inpaintTiles(before, PixelRect{0, 0, 128, 128}, direct, &reference);

      TileStore preview;
      const FilterOpResult previewResult = previewInpaint(od, 5, &preview);
      const FilterOpResult r = applyInpaint(od, 5);
      const TileStore& after = *od.document.layers[0].rgbTiles;

      check(r.refusal == PixelOpRefusal::None && r.texelsChanged > 0,
            "command: with a scratch selected the command runs and changes texels");
      check(od.history.entries().size() == entriesBefore + 1 && !od.unsavedEdits.empty() &&
                od.unsavedEdits.back() == "inpaint",
            "command: exactly ONE history entry, named \"inpaint\" -- not one per texel and "
            "not a shared \"filter\"");

      bool matchesEngine = true;
      bool outsideUntouched = true;
      for (int32_t y = 0; y < 128; ++y) {
        for (int32_t x = 0; x < 128; ++x) {
          const bool inHole = y == 64 && x >= 20 && x < 100;
          if (inHole) {
            if (inpaintReadAt(after, x, y) != inpaintReadAt(reference, x, y))
              matchesEngine = false;
          } else if (inpaintReadAt(after, x, y) != inpaintReadAt(before, x, y)) {
            outsideUntouched = false;
          }
        }
      }
      check(matchesEngine,
            "command: every filled texel is BIT-IDENTICAL to ops/Inpaint called directly with "
            "the document's own selection -- the command reaches the engine, and hands it the "
            "same hole the composite blends through");
      check(outsideUntouched,
            "command: EVERY texel outside the selection is bit-identical to before -- the "
            "whole 128x128 canvas minus the 80 selected ones, not a sample");

      check(previewResult.refusal == PixelOpRefusal::None &&
                previewResult.texelsChanged == r.texelsChanged &&
                inpaintTilesExactlyEqual(preview, after),
            "command: previewInpaint() computes BIT-IDENTICALLY what applyInpaint() then "
            "commits -- T15's rule, and the reason both go through one bridge");

      const Document* prior = od.history.undo();
      if (prior != nullptr) od.document = *prior;
      check(prior != nullptr &&
                inpaintTilesExactlyEqual(*od.document.layers[0].rgbTiles, before),
            "command: undo restores the pre-inpaint tiles EXACTLY -- memcmp on the raw half "
            "words, not a tolerance");
    }

    // The menu item itself.
    {
      MenuContext ctx;
      ctx.hasDocument = true;
      ctx.filterLayerUsable = true;
      ctx.hasEngagedSelection = true;
      const MenuNode* withSelection =
          inpaintFindMenuAction(buildMenuModel(ctx), MenuAction::Inpaint);
      ctx.hasEngagedSelection = false;
      const MenuNode* withoutSelection =
          inpaintFindMenuAction(buildMenuModel(ctx), MenuAction::Inpaint);
      check(withSelection != nullptr && withSelection->enabled,
            "menu: Filter > Inpaint is in the built tree and enabled on a usable layer with a "
            "selection");
      check(withoutSelection != nullptr && !withoutSelection->enabled &&
                !withoutSelection->tooltip.empty(),
            "menu: with no selection the item is DISABLED with a reason in its tooltip -- the "
            "only Filter item that asks a second question, because it is the only one the "
            "selection is an input to");
      check(menuActionEffect(MenuAction::Inpaint) == MenuEffect::Deferred,
            "menu: Inpaint is Deferred -- it opens a modal, which a native menu's AppKit "
            "callback has no ImGui frame to do directly");
    }
  }

  std::printf("  -- D. PRD D7: the scratch actually goes away -------------------\n");
  {
    // The feature, stated as the user would: a dark scratch across a smooth
    // field, selected, and afterwards indistinguishable from its
    // surroundings. Measured against what the field WOULD have held there,
    // which the fixture knows because the ramp is analytic.
    TileStore clean;
    fillInpaintRamp(clean, 128, 128);
    TileStore scratched = clean;
    Selection sel;
    for (int32_t i = 16; i < 112; ++i) {
      const int32_t y = 30 + (i - 16) / 3;  // a shallow diagonal, 1 texel wide
      inpaintWriteAt(scratched, i, y, {0.02f, 0.0f, 0.02f, 1.0f});
      inpaintSelectTexel(sel, i, y);
    }

    InpaintParams params;
    params.hole = &sel;
    params.radius = 5;
    TileStore repaired;
    inpaintTiles(scratched, PixelRect{0, 0, 128, 128}, params, &repaired);

    float worstBefore = 0.0f;
    float worstAfter = 0.0f;
    for (int32_t i = 16; i < 112; ++i) {
      const int32_t y = 30 + (i - 16) / 3;
      const std::array<float, 4> want = inpaintReadAt(clean, i, y);
      const std::array<float, 4> was = inpaintReadAt(scratched, i, y);
      const std::array<float, 4> now = inpaintReadAt(repaired, i, y);
      for (int c = 0; c < 3; ++c) {
        worstBefore = std::max(worstBefore, std::fabs(was[c] - want[c]));
        worstAfter = std::max(worstAfter, std::fabs(now[c] - want[c]));
      }
    }
    check(worstBefore > 0.5f,
          "PRD D7: the fixture's scratch really is gross damage -- more than half of full "
          "scale wrong, so the improvement below has somewhere to come from");
    check(worstAfter < 0.02f,
          "PRD D7: after the fill, the worst scratch texel is within 2% of what the "
          "undamaged field held there -- the scratch is gone, not merely lightened");
    std::printf("  [measured] 96-texel diagonal scratch: worst error %.4f -> %.4f (%.0fx)\n",
                static_cast<double>(worstBefore), static_cast<double>(worstAfter),
                static_cast<double>(worstBefore / std::max(worstAfter, 1e-9f)));
  }

  std::printf("[selftest] inpaint %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
