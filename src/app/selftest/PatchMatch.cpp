#include "app/selftest/Support.hpp"

#include <chrono>
#include <cmath>
#include <cstring>

#include "app/Command.hpp"
#include "app/FilterOps.hpp"
#include "app/RepairCommandsExtra.hpp"
#include "ops/PatchMatch.hpp"
#include "ops/Roi.hpp"
#include "ui/MenuModel.hpp"

namespace np {
namespace {

std::array<float, 4> pmReadAt(const TileStore& store, int32_t x, int32_t y) {
  const Tile* t = store.find(tileCoordAt(PixelCoord{x, y}));
  if (t == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
  return t->readPixel(tileLocalOffset(PixelCoord{x, y}));
}

void pmWriteAt(TileStore& store, int32_t x, int32_t y, const std::array<float, 4>& rgba) {
  store.getOrCreate(tileCoordAt(PixelCoord{x, y})).writePixel(tileLocalOffset(PixelCoord{x, y}), rgba);
}

void pmSelectTexel(Selection& sel, int32_t x, int32_t y) {
  sel.tiles.getOrCreate(tileCoordAt(PixelCoord{x, y}))
      .writeCoverage(tileLocalOffset(PixelCoord{x, y}), 1.0f);
}

void pmSelectRect(Selection& sel, int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
  for (int32_t y = y0; y < y1; ++y)
    for (int32_t x = x0; x < x1; ++x) pmSelectTexel(sel, x, y);
}

bool pmTilesExactlyEqual(const TileStore& a, const TileStore& b) {
  if (a.occupiedTileCount() != b.occupiedTileCount()) return false;
  for (const auto& [coord, tile] : a) {
    const Tile* other = b.find(coord);
    if (other == nullptr) return false;
    if (std::memcmp(tile.data(), other->data(), Tile::kTexelCount * sizeof(uint16_t)) != 0)
      return false;
  }
  return true;
}

// splitmix64's finalizer, this suite's own copy -- app/selftest/Tileable.cpp's
// precedent, "a shared fixture header would be one more file a change to any
// of them could ripple through".
float pmNoise(uint64_t i) noexcept {
  uint64_t z = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);
}

// A `size` x `size` field with real structure to match against: a smooth
// sine of wavelength `period` texels in x, a slower one in y so the texture
// is genuinely two-dimensional rather than a repeated 1-D column, plus a
// little per-texel noise -- structured enough that a correct fill can be
// told apart from one that merely averaged its rim into grey, and smooth
// enough that a period measurement on the result is not deciding between two
// candidate lags that are equally right by the fixture's own construction
// (a hard-edged stripe's own internal edges can do that; a sine's slope
// cannot).
void fillStripeField(TileStore& tiles, int32_t size, int32_t period) {
  constexpr double kTwoPi = 6.283185307179586;
  for (int32_t y = 0; y < size; ++y) {
    for (int32_t x = 0; x < size; ++x) {
      const double xPhase = kTwoPi * static_cast<double>(x) / static_cast<double>(period);
      const double yPhase = kTwoPi * static_cast<double>(y) / (static_cast<double>(period) * 1.7);
      const uint64_t i = static_cast<uint64_t>(y) * static_cast<uint64_t>(size) +
                         static_cast<uint64_t>(x);
      const float n = 0.04f * pmNoise(i);
      const float v = std::min(1.0f, std::max(0.0f,
                       0.5f + 0.28f * static_cast<float>(std::sin(xPhase)) +
                           0.08f * static_cast<float>(std::sin(yPhase)) + n));
      pmWriteAt(tiles, x, y, {v, v, v, 1.0f});
    }
  }
}

const MenuNode* pmFindMenuAction(const std::vector<MenuNode>& nodes, MenuAction action) {
  for (const MenuNode& n : nodes) {
    if (n.action == action) return &n;
    if (const MenuNode* found = pmFindMenuAction(n.children, action)) return found;
  }
  return nullptr;
}

}  // namespace

// PRD D7's second half (ops/PatchMatch.hpp): texture-synthesis fill for a
// selected hole, and the Edit > Content-Aware Fill command that reaches it.
//
// **What every assertion below is checking for, in one sentence:** a search
// can go wrong in ways a closed-form solve cannot -- reading the hole it is
// filling, sourcing a patch that overlaps it, or simply not being
// reproducible -- and none of those produces a crash. So this section checks
// each one directly rather than re-deriving the arithmetic of the search
// itself.
bool runPatchMatchTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("  -- A. params and refusals ---------------------------------------\n");
  {
    Selection sel = selectRectangle(10.0f, 10.0f, 20.0f, 20.0f);
    PatchMatchParams good;
    good.hole = &sel;
    check(patchMatchParamsValid(good), "params: a real hole and default fields are valid");

    PatchMatchParams noHole = good;
    noHole.hole = nullptr;
    check(!patchMatchParamsValid(noHole),
          "params: a NULL hole is refused -- the identical inversion ops/Inpaint.hpp states");

    Selection engagedButEmpty;
    PatchMatchParams emptyHole = good;
    emptyHole.hole = &engagedButEmpty;
    check(!patchMatchParamsValid(emptyHole), "params: an engaged-but-empty selection is refused too");

    PatchMatchParams badPatch = good;
    badPatch.patchRadius = 0;
    const bool zeroPatchRefused = !patchMatchParamsValid(badPatch);
    badPatch.patchRadius = kPatchMatchMaxPatchRadius + 1;
    const bool hugePatchRefused = !patchMatchParamsValid(badPatch);
    PatchMatchParams badIter = good;
    badIter.iterations = 0;
    const bool zeroIterRefused = !patchMatchParamsValid(badIter);
    PatchMatchParams badLevels = good;
    badLevels.pyramidLevels = kPatchMatchMaxPyramidLevels + 1;
    const bool hugeLevelsRefused = !patchMatchParamsValid(badLevels);
    check(zeroPatchRefused && hugePatchRefused && zeroIterRefused && hugeLevelsRefused,
          "params: an out-of-range patch radius, iteration count or pyramid level count is "
          "refused by name rather than clamped");

    // Beyond the size cap: a hole whose bounding box exceeds
    // `kPatchMatchMaxHoleTexels` is refused before any work is done -- cheap
    // to check even with an empty source, which is the point of checking it
    // this way.
    Selection huge = selectRectangle(0.0f, 0.0f, 700.0f, 700.0f);
    PatchMatchParams overCap = good;
    overCap.hole = &huge;
    const TileStore emptySrc;
    TileStore capDst;
    const PixelRect capCanvas{0, 0, 700, 700};
    check(roiTexelCount(*patchMatchHoleBounds(overCap)) > kPatchMatchMaxHoleTexels &&
              !patchMatchTiles(emptySrc, capCanvas, overCap, &capDst),
          "engine: a hole past kPatchMatchMaxHoleTexels is refused by name, cheaply");
  }

  std::printf("  -- B. determinism, and never sourcing from the hole -------------\n");
  {
    constexpr int32_t kSize = 96;
    constexpr int32_t kPeriod = 8;
    TileStore field;
    fillStripeField(field, kSize, kPeriod);

    // A hole wider than one period, so "the fill continued the period" is a
    // claim with room to be wrong in.
    const int32_t x0 = 40, x1 = 40 + 3 * kPeriod;  // 3 periods wide
    const int32_t y0 = 30, y1 = 66;
    Selection hole;
    pmSelectRect(hole, x0, y0, x1, y1);
    check(x1 - x0 > kPeriod, "fixture: the hole is wider than one stripe period");

    PatchMatchParams params;
    params.hole = &hole;
    params.patchRadius = 3;
    params.iterations = 6;
    params.pyramidLevels = 3;
    // Small on purpose -- the fixture's own real texture is well within this
    // margin, and a margin as wide as ops/PatchMatch.hpp's default would
    // mostly search the canvas's own absent-tile padding on a fixture this
    // small (see ops/PatchMatch.hpp's own "absent tiles read as transparent
    // black" argument -- true, but not interesting texture to match against).
    params.sourceMargin = 24;
    params.seed = 12345;
    const PixelRect canvas{0, 0, kSize, kSize};

    std::vector<PatchMatchNnfEntry> nnf;
    TileStore filledA;
    check(patchMatchTiles(field, canvas, params, &filledA, &nnf),
          "engine: a well-formed request over the whole canvas succeeds");

    TileStore filledB;
    check(patchMatchTiles(field, canvas, params, &filledB),
          "engine: the same request run again also succeeds");
    check(pmTilesExactlyEqual(filledA, filledB),
          "engine: the SAME seed produces a BIT-IDENTICAL fill on a second run -- the whole "
          "reproducibility contract ops/PatchMatch.hpp states");

    PatchMatchParams differentSeed = params;
    differentSeed.seed = 999999;
    TileStore filledC;
    patchMatchTiles(field, canvas, differentSeed, &filledC);
    check(!pmTilesExactlyEqual(filledA, filledC),
          "engine: a DIFFERENT seed can produce a different fill on a fixture with more than "
          "one equally good match");

    // Only the hole was written, and nothing outside its bounding box.
    bool onlyHoleWritten = true;
    for (const auto& [coord, tile] : filledA) {
      (void)tile;
      const TileRange holeTiles = roiTileRange(PixelRect{x0, y0, x1, y1});
      if (coord.x < holeTiles.x0 || coord.x >= holeTiles.x1 || coord.y < holeTiles.y0 ||
          coord.y >= holeTiles.y1) {
        onlyHoleWritten = false;
      }
    }
    bool outsideUntouched = true;
    for (int32_t y = y0 - 10; y < y1 + 10 && outsideUntouched; ++y) {
      for (int32_t x = x0 - 10; x < x1 + 10; ++x) {
        const bool inHole = x >= x0 && x < x1 && y >= y0 && y < y1;
        if (inHole) continue;
        if (pmReadAt(filledA, x, y) != std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f}) {
          outsideUntouched = false;
          break;
        }
      }
    }
    check(onlyHoleWritten && outsideUntouched,
          "engine: only the hole's tiles carry a write, and every texel outside the hole (even "
          "just past its rim) is absent from the destination");

    // **The NNF instrumentation, checked from OUTSIDE the engine.** For every
    // accepted match, the WHOLE (2r+1)^2 source window must have coverage <=
    // 0.5 under `hole` -- not merely the centre.
    bool noSourceOverlapsHole = !nnf.empty();
    for (const PatchMatchNnfEntry& e : nnf) {
      for (int32_t dy = -params.patchRadius; dy <= params.patchRadius; ++dy) {
        for (int32_t dx = -params.patchRadius; dx <= params.patchRadius; ++dx) {
          const PixelCoord probe{e.source.x + dx, e.source.y + dy};
          if (selectionCoverageAt(&hole, probe) > 0.5f) noSourceOverlapsHole = false;
        }
      }
    }
    check(noSourceOverlapsHole,
          "engine: every accepted match's WHOLE source patch has coverage <= 0.5 everywhere -- "
          "checked against the selection directly, not against the engine's own bookkeeping");
    check(nnf.size() == static_cast<size_t>((x1 - x0) * (y1 - y0)),
          "engine: the NNF instrumentation names exactly one entry per hole texel");

    // The period: cross-correlate the hole against a few candidate lags near
    // the fixture's period and confirm the true period wins by a wide
    // margin. Averaged over every row of the hole, not just one, so a single
    // row's own imperfect match cannot decide the result.
    auto sadAtLag = [&](int32_t lag) {
      double sum = 0.0;
      int32_t n = 0;
      for (int32_t y = y0; y < y1; ++y) {
        for (int32_t x = x0 + lag; x < x1; ++x) {
          sum += std::fabs(pmReadAt(filledA, x, y)[0] - pmReadAt(filledA, x - lag, y)[0]);
          ++n;
        }
      }
      return sum / std::max(n, 1);
    };
    double bestSad = 1e18;
    int32_t bestLag = -1;
    for (int32_t lag = kPeriod - 2; lag <= kPeriod + 2; ++lag) {
      const double s = sadAtLag(lag);
      if (s < bestSad) {
        bestSad = s;
        bestLag = lag;
      }
    }
    check(bestLag == kPeriod,
          "PRD D7: on a periodic texture, the fill's own dominant period matches the source's");
    std::printf("  [measured] period search over [%d,%d]: best lag %d (source period %d)\n",
               kPeriod - 2, kPeriod + 2, bestLag, kPeriod);
  }

  std::printf("  -- C. the command: one bridge, one selection, one seed ----------\n");
  {
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "content-aware fill no sel");
    fillStripeField(*od.document.layers[0].rgbTiles, 64, 6);
    od.recordEdit("caf fixture", EditKind::Content);
    check(applyContentAwareFill(od, ContentAwareFillRequest{}).refusal ==
              PixelOpRefusal::NoSelection,
          "command: with no selection, content-aware fill refuses NoSelection -- the same "
          "inversion Inpaint's own command asserts");

    OpenDocument doc = makeBlankOpenDocument(64, 64, WorkingSpace{}, "content-aware fill scratch");
    fillStripeField(*doc.document.layers[0].rgbTiles, 64, 6);
    Selection sel;
    pmSelectRect(sel, 20, 20, 32, 32);
    doc.recordEdit("caf fixture", EditKind::Content);
    doc.selection = sel;

    ContentAwareFillRequest req;
    req.patchRadius = 2;
    req.iterations = 3;
    req.pyramidLevels = 2;
    req.seed = 777;

    OpenDocument reference = makeBlankOpenDocument(64, 64, WorkingSpace{}, "caf reference");
    reference.document.layers[0].rgbTiles = doc.document.layers[0].rgbTiles;
    reference.selection = sel;
    const FilterOpResult refResult = applyContentAwareFill(reference, req);

    const CommandResult cmdResult = applyCommand(doc, contentAwareFillCommand(req));
    check(cmdResult.ok && refResult.refusal == PixelOpRefusal::None,
          "command: the encoder's command runs through applyCommand() and the direct applier "
          "both succeed");
    check(pmTilesExactlyEqual(*doc.document.layers[0].rgbTiles,
                              *reference.document.layers[0].rgbTiles),
          "command: applyCommand(contentAwareFillCommand(seed)) is BIT-IDENTICAL to the direct "
          "applier called with the same seed -- the encoder round-trip");

    // Invalid params, refused by name.
    JsonValue bad = JsonValue::object();
    bad.set("patch_radius", JsonValue::number(0));
    const CommandResult badResult = applyCommand(doc, Command{"content_aware_fill", bad});
    check(!badResult.ok && !badResult.status.empty(),
          "command: patch_radius 0 is refused by name, not clamped to the engine's default");
  }

  std::printf("  -- D. the menu item ----------------------------------------------\n");
  {
    MenuContext ctx;
    ctx.hasDocument = true;
    ctx.filterLayerUsable = true;
    ctx.hasEngagedSelection = true;
    const std::vector<MenuNode> withSel = buildMenuModel(ctx);
    const MenuNode* enabled = pmFindMenuAction(withSel, MenuAction::ContentAwareFill);
    ctx.hasEngagedSelection = false;
    const std::vector<MenuNode> withoutSel = buildMenuModel(ctx);
    const MenuNode* disabled = pmFindMenuAction(withoutSel, MenuAction::ContentAwareFill);
    check(enabled != nullptr && enabled->enabled,
          "menu: Edit > Content-Aware Fill is enabled on a usable layer with a selection");
    check(disabled != nullptr && !disabled->enabled && !disabled->tooltip.empty(),
          "menu: with no selection the item is disabled with a reason in its tooltip");
    check(menuActionEffect(MenuAction::ContentAwareFill) == MenuEffect::Deferred,
          "menu: ContentAwareFill is Deferred -- it opens a modal");
  }

  std::printf("  -- E. measured: a 200x200 hole in a 1000x1000 layer --------------\n");
  {
    constexpr int32_t kSize = 1000;
    TileStore field;
    fillStripeField(field, kSize, 16);
    Selection hole;
    pmSelectRect(hole, 400, 400, 600, 600);
    PatchMatchParams params;
    params.hole = &hole;
    params.patchRadius = 3;
    params.iterations = 3;
    params.pyramidLevels = 4;
    params.sourceMargin = 128;
    params.seed = 1;
    const PixelRect canvas{0, 0, kSize, kSize};
    TileStore filled;
    const auto t0 = std::chrono::steady_clock::now();
    const bool measuredOk = patchMatchTiles(field, canvas, params, &filled);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    check(measuredOk, "PRD D7: a 200x200 hole in a 1000x1000 layer succeeds");
    std::printf("  [measured] 200x200 hole, 1000x1000 layer, patch 7x7, 3 iter x 4 levels: %.1f ms\n",
               ms);
  }

  std::printf("[selftest] patchmatch %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
