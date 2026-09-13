#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "app/Command.hpp"
#include "app/FilterOps.hpp"
#include "app/RepairCommandsExtra.hpp"
#include "ops/Roi.hpp"
#include "ops/SeamHeal.hpp"
#include "ui/MenuModel.hpp"

namespace np {
namespace {

std::array<float, 4> shReadAt(const TileStore& store, int32_t x, int32_t y) {
  const Tile* t = store.find(tileCoordAt(PixelCoord{x, y}));
  if (t == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
  return t->readPixel(tileLocalOffset(PixelCoord{x, y}));
}

void shWriteAt(TileStore& store, int32_t x, int32_t y, const std::array<float, 4>& rgba) {
  store.getOrCreate(tileCoordAt(PixelCoord{x, y})).writePixel(tileLocalOffset(PixelCoord{x, y}), rgba);
}

bool shTilesExactlyEqual(const TileStore& a, const TileStore& b) {
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
// precedent.
float shNoise(uint64_t i) noexcept {
  uint64_t z = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);
}

// A smoothly-varying field (a sine of wavelength `period`, texels apart, plus
// a little per-texel noise) made deliberately non-tileable by a flat,
// out-of-range band `edgeWidth` texels wide at the LEFT edge (0.95, well
// above the sine's own [0.2, 0.8] range) -- so x=0 clashes hard with x=size-1
// by construction. **Smooth on purpose**: a natural adjacent-texel step in
// this field is small (the sine's own slope), so "the seam's own jump is
// down near a natural neighbour's" is a claim with a small, meaningful
// target rather than "within the size of one period step", which a flat
// stripe field's own internal edges could satisfy by coincidence.
void fillEdgeOffsetStripes(TileStore& tiles, int32_t size, int32_t period, int32_t edgeWidth) {
  constexpr double kTwoPi = 6.283185307179586;
  for (int32_t y = 0; y < size; ++y) {
    for (int32_t x = 0; x < size; ++x) {
      const uint64_t i = static_cast<uint64_t>(y) * static_cast<uint64_t>(size) +
                         static_cast<uint64_t>(x);
      const double phase = kTwoPi * static_cast<double>(x) / static_cast<double>(period);
      float v = x < edgeWidth
                    ? 0.95f
                    : 0.5f + 0.3f * static_cast<float>(std::sin(phase)) + 0.04f * shNoise(i);
      v = std::min(1.0f, std::max(0.0f, v));
      shWriteAt(tiles, x, y, {v, v, v, 1.0f});
    }
  }
}

// The seam discontinuity this fixture is built to have: how far the two
// wrap-adjacent edges (x=0 and x=size-1) disagree, averaged over every row.
double seamDiscontinuity(const TileStore& tiles, int32_t size) {
  double sum = 0.0;
  for (int32_t y = 0; y < size; ++y)
    sum += std::fabs(shReadAt(tiles, 0, y)[0] - shReadAt(tiles, size - 1, y)[0]);
  return sum / size;
}

const MenuNode* shFindMenuAction(const std::vector<MenuNode>& nodes, MenuAction action) {
  for (const MenuNode& n : nodes) {
    if (n.action == action) return &n;
    if (const MenuNode* found = shFindMenuAction(n.children, action)) return found;
  }
  return nullptr;
}

}  // namespace

// PRD D8's missing third piece (ops/SeamHeal.hpp): repair the discontinuity
// where a texture's opposite edges meet, and the Filter > Seam Heal command
// that reaches it.
//
// **The property that makes this safe to run blind, asserted directly:**
// both offsets are pure addressing changes, so every texel outside the two
// bands must round-trip to bit-identical. That is checked over a generous,
// deliberately conservative interior region rather than against the exact
// band edge, because the exact affected set is a wrapped transform of the
// centre and the band width that a test recomputing it would be checking its
// own arithmetic rather than the engine's.
bool runSeamHealTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("  -- A. params and refusals ---------------------------------------\n");
  {
    SeamHealParams good;
    good.wrapRect = PixelRect{0, 0, 128, 128};
    check(seamHealParamsValid(good), "params: a real canvas and default fields are valid");

    SeamHealParams emptyCanvas = good;
    emptyCanvas.wrapRect = roiEmptyRect();
    check(!seamHealParamsValid(emptyCanvas), "params: an empty wrapRect is refused");

    SeamHealParams zeroBand = good;
    zeroBand.bandWidth = 0;
    check(!seamHealParamsValid(zeroBand), "params: bandWidth 0 is refused, not run as a no-op");

    SeamHealParams badPatch = good;
    badPatch.patchRadius = kPatchMatchMaxPatchRadius + 1;
    check(!seamHealParamsValid(badPatch), "params: an out-of-range patch radius is refused");

    check(seamHealCentre(PixelRect{0, 0, 128, 96}).x == 64 &&
              seamHealCentre(PixelRect{0, 0, 128, 96}).y == 48,
          "params: seamHealCentre() is floor(w/2), floor(h/2) -- offsetByHalf()'s own answer");
  }

  std::printf("  -- B. the repair: outside the bands, and the discontinuity ------\n");
  {
    constexpr int32_t kSize = 128;
    constexpr int32_t kPeriod = 8;
    TileStore field;
    fillEdgeOffsetStripes(field, kSize, kPeriod, 6);
    const double before = seamDiscontinuity(field, kSize);
    check(before > 0.3, "fixture: the seam really is discontinuous before healing");

    SeamHealParams params;
    params.wrapRect = PixelRect{0, 0, kSize, kSize};
    params.bandWidth = 16;
    params.patchRadius = 3;
    params.iterations = 5;
    params.pyramidLevels = 3;
    params.seed = 42;

    TileStore healedA;
    check(seamHealTiles(field, params.wrapRect, params, &healedA),
          "engine: a well-formed request over the whole canvas succeeds");
    const double after = seamDiscontinuity(healedA, kSize);
    check(after < 0.5 * before,
          "PRD D8: the wrap-boundary discontinuity drops by at least half after healing");
    std::printf("  [measured] seam discontinuity: %.3f -> %.3f\n", before, after);

    TileStore healedB;
    seamHealTiles(field, params.wrapRect, params, &healedB);
    check(shTilesExactlyEqual(healedA, healedB),
          "engine: the SAME seed reproduces a BIT-IDENTICAL repair on a second run");

    SeamHealParams differentSeed = params;
    differentSeed.seed = 4242;
    TileStore healedC;
    seamHealTiles(field, params.wrapRect, differentSeed, &healedC);
    check(!shTilesExactlyEqual(healedA, healedC),
          "engine: a DIFFERENT seed can produce a different repair");

    // A generous, definitely-safe interior region -- comfortably more than
    // `bandWidth` away from every edge on both axes, so it cannot be inside
    // either band on any addressing this op could produce.
    bool interiorUntouched = true;
    for (int32_t y = 40; y < 88 && interiorUntouched; ++y) {
      for (int32_t x = 40; x < 88; ++x) {
        if (shReadAt(healedA, x, y) != shReadAt(field, x, y)) {
          interiorUntouched = false;
          break;
        }
      }
    }
    check(interiorUntouched,
          "engine: the interior, well away from both seams, is bit-identical to the source");

    // Refusals: dst aliasing src, and invalid params.
    TileStore alias = field;
    check(!seamHealTiles(alias, params.wrapRect, params, &alias),
          "engine: an aliased destination is refused and writes nothing new");
    SeamHealParams wrongRect = params;
    wrongRect.wrapRect = PixelRect{0, 0, kSize - 1, kSize};
    TileStore dst;
    check(!seamHealTiles(field, params.wrapRect, wrongRect, &dst) && dst.occupiedTileCount() == 0,
          "engine: outRect must equal wrapRect -- this op is not tileable in pieces");
  }

  std::printf("  -- C. the command: refusal under a selection, and the seed ------\n");
  {
    OpenDocument doc = makeBlankOpenDocument(64, 64, WorkingSpace{}, "seam heal fixture");
    fillEdgeOffsetStripes(*doc.document.layers[0].rgbTiles, 64, 8, 4);
    doc.recordEdit("seam heal fixture", EditKind::Content);

    doc.selection = selectRectangle(4.0f, 4.0f, 20.0f, 20.0f);
    check(applySeamHeal(doc, SeamHealRequest{}).refusal == PixelOpRefusal::SelectionActive,
          "command: with a live selection, seam heal refuses SelectionActive -- offsetRefusalFor()'s "
          "own reason");
    doc.selection.reset();

    SeamHealRequest req;
    req.bandWidth = 10;
    req.patchRadius = 2;
    req.iterations = 3;
    req.pyramidLevels = 2;
    req.seed = 99;

    OpenDocument reference = makeBlankOpenDocument(64, 64, WorkingSpace{}, "seam heal reference");
    reference.document.layers[0].rgbTiles = doc.document.layers[0].rgbTiles;
    const FilterOpResult refResult = applySeamHeal(reference, req);

    const CommandResult cmdResult = applyCommand(doc, seamHealCommand(req));
    check(cmdResult.ok && refResult.refusal == PixelOpRefusal::None,
          "command: the encoder's command and the direct applier both succeed");
    check(shTilesExactlyEqual(*doc.document.layers[0].rgbTiles,
                              *reference.document.layers[0].rgbTiles),
          "command: applyCommand(seamHealCommand(seed)) is BIT-IDENTICAL to the direct applier "
          "called with the same seed");

    JsonValue bad = JsonValue::object();
    bad.set("band_width", JsonValue::number(0));
    const CommandResult badResult = applyCommand(doc, Command{"seam_heal", bad});
    check(!badResult.ok && !badResult.status.empty(),
          "command: band_width 0 is refused by name");
  }

  std::printf("  -- D. the menu item -----------------------------------------------\n");
  {
    MenuContext ctx;
    ctx.hasDocument = true;
    ctx.filterLayerUsable = true;
    const std::vector<MenuNode> tree = buildMenuModel(ctx);
    const MenuNode* item = shFindMenuAction(tree, MenuAction::SeamHeal);
    check(item != nullptr && item->enabled,
          "menu: Filter > Seam Heal is in the built tree and enabled on a usable layer");
    check(menuActionEffect(MenuAction::SeamHeal) == MenuEffect::Deferred,
          "menu: SeamHeal is Deferred -- it opens a modal");
  }

  std::printf("[selftest] seamheal %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
