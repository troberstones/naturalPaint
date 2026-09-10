#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>

#include "app/FilterOps.hpp"
#include "ui/MenuModel.hpp"

namespace np {

namespace {

// splitmix64's finalizer again -- the same three lines app/selftest/Blur.cpp,
// app/selftest/Filters.cpp and app/selftest/FilterMenu.cpp each keep a private
// copy of. This section follows that precedent rather than breaking it, for
// the reason FilterMenu.cpp states: a shared fixture header would be one more
// file a change to any of them could ripple through.
float tileableNoise(uint64_t i) noexcept {
  uint64_t z = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);  // [0,1)
}

constexpr int32_t kSize = 256;  // 2x2 tiles

// The illumination this section's fixture is lit by: a linear ramp, left to
// right, from 0.2 to 2.0.
//
// **Linear on purpose.** A symmetric kernel reproduces a linear function
// exactly in the interior, so `blur(R * L)` is `mean(R) * L` to the accuracy
// of the texture's own averaging -- which makes "did the divide remove the
// light" a question about the op rather than about how well a Gaussian
// happens to approximate this particular field. The edge is a different
// matter and section A's bands stay 4 sigma away from it; see there.
float fixtureIllumination(int32_t x) noexcept {
  return 0.2f + 1.8f * (static_cast<float>(x) / static_cast<float>(kSize - 1));
}

// Reflectance: white noise in [0.2, 0.6], the same at every x, so its LOCAL
// mean is spatially flat and any surviving left-to-right trend in the result
// is the lighting the op failed to remove rather than the texture's own.
float fixtureReflectance(int32_t x, int32_t y, int32_t channel) noexcept {
  const uint64_t i = (static_cast<uint64_t>(y) * static_cast<uint64_t>(kSize) +
                      static_cast<uint64_t>(x)) *
                         4ULL +
                     static_cast<uint64_t>(channel);
  return 0.2f + 0.4f * tileableNoise(i);
}

// A photographed-wall stand-in: a flat texture times a strong lighting ramp,
// opaque, premultiplied (alpha 1, so premultiplied and straight agree and the
// assertions below can be read as ordinary colour).
void fillLitField(TileStore& tiles) {
  for (int32_t ty = 0; ty < kSize / kTileSize; ++ty) {
    for (int32_t tx = 0; tx < kSize / kTileSize; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t ly = 0; ly < kTileSize; ++ly) {
        for (int32_t lx = 0; lx < kTileSize; ++lx) {
          const int32_t x = tx * kTileSize + lx;
          const int32_t y = ty * kTileSize + ly;
          const float lit = fixtureIllumination(x);
          t.writePixel(PixelCoord{lx, ly},
                       {fixtureReflectance(x, y, 0) * lit, fixtureReflectance(x, y, 1) * lit,
                        fixtureReflectance(x, y, 2) * lit, 1.0f});
        }
      }
    }
  }
}

std::array<float, 4> readAt(const TileStore& store, int32_t x, int32_t y) {
  const Tile* tile = store.find(tileCoordAt(PixelCoord{x, y}));
  if (tile == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
  return tile->readPixel(tileLocalOffset(PixelCoord{x, y}));
}

// Mean of one colour channel over a rectangle. Used both for "did the light
// come out" (a band) and for "was the mean put back" (the canvas).
double meanOver(const TileStore& store, const PixelRect& r, size_t channel) {
  double sum = 0.0;
  for (int32_t y = r.y0; y < r.y1; ++y)
    for (int32_t x = r.x0; x < r.x1; ++x) sum += static_cast<double>(readAt(store, x, y)[channel]);
  const double n = static_cast<double>(r.width()) * static_cast<double>(r.height());
  return n > 0.0 ? sum / n : 0.0;
}

// Bit-for-bit equality of the raw half words over a rectangle -- the standard
// app/selftest/FilterMenu.cpp holds undo to, applied here to the two claims
// that are about exactness rather than about arithmetic: an offset copies
// texels, and a split request produces the same texels as a whole one.
bool exactlyEqualOver(const TileStore& a, const TileStore& b, const PixelRect& r) {
  for (int32_t y = r.y0; y < r.y1; ++y)
    for (int32_t x = r.x0; x < r.x1; ++x)
      if (readAt(a, x, y) != readAt(b, x, y)) return false;
  return true;
}

bool tilesExactlyEqual(const TileStore& a, const TileStore& b) {
  if (a.occupiedTileCount() != b.occupiedTileCount()) return false;
  for (const auto& [coord, tile] : a) {
    const Tile* other = b.find(coord);
    if (other == nullptr) return false;
    if (std::memcmp(tile.data(), other->data(), Tile::kTexelCount * sizeof(uint16_t)) != 0)
      return false;
  }
  return true;
}

OpenDocument makeLitDocument(const char* title) {
  OpenDocument od = makeBlankOpenDocument(kSize, kSize, WorkingSpace{}, title);
  fillLitField(*od.document.layers[0].rgbTiles);
  // Recorded rather than merely written, for app/selftest/FilterMenu.cpp's
  // stated reason: an undo assertion against a fixture that never committed
  // its own content compares a filled store against a blank one and reports a
  // history defect that is really a fixture defect.
  od.recordEdit("tileable fixture field", EditKind::Content);
  return od;
}

const MenuNode* findMenuAction(const std::vector<MenuNode>& nodes, MenuAction action) {
  for (const MenuNode& n : nodes) {
    if ((n.kind == MenuNodeKind::Command || n.kind == MenuNodeKind::Check) && n.action == action)
      return &n;
    if (const MenuNode* found = findMenuAction(n.children, action)) return found;
  }
  return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// See the doc block in app/SelfTest.hpp -- restated only in outline here.
// ---------------------------------------------------------------------------
bool runTileableTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-74s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  const PixelRect kCanvas{0, 0, kSize, kSize};
  // Two bands, each 4 sigma (64 texels at sigma 16) clear of the canvas edge,
  // so the coverage-weighted blur's edge bias -- ops/Blur.hpp's "blurring past
  // the edge of the painted region fades to transparent" -- is not what these
  // assertions are measuring.
  const PixelRect kLeftBand{64, 0, 96, kSize};
  const PixelRect kRightBand{160, 0, 192, kSize};
  constexpr float kSigma = 16.0f;

  std::printf("  -- A. lighting-gradient removal: the light comes out --\n");
  {
    TileStore src;
    fillLitField(src);

    LightingGradientParams params;
    params.sigma = kSigma;
    params.statsRect = kCanvas;

    TileStore out;
    check(removeLightingGradientTiles(src, kCanvas, params, &out),
          "gradient: a positive sigma over the canvas is accepted by the engine");

    const double inLeft = meanOver(src, kLeftBand, 0);
    const double inRight = meanOver(src, kRightBand, 0);
    const double outLeft = meanOver(out, kLeftBand, 0);
    const double outRight = meanOver(out, kRightBand, 0);

    // The fixture really is lit unevenly -- asserted rather than assumed, so
    // that the assertion below cannot pass by measuring a flat input.
    check(inRight / inLeft > 1.5,
          "gradient: the fixture's two bands differ by more than 1.5x BEFORE the op -- the "
          "defect this op removes is actually present");
    check(std::fabs(outRight / outLeft - 1.0) < 0.02,
          "gradient: after the op the same two bands agree within 2% -- the lighting ramp is "
          "gone, which subtracting a blur would not have achieved (it re-centres the level "
          "and leaves the contrast halved)");
  }

  std::printf("  -- B. the re-centred mean: the step a naive version forgets --\n");
  {
    TileStore src;
    fillLitField(src);

    LightingGradientParams params;
    params.sigma = kSigma;
    params.statsRect = kCanvas;

    TileStore out;
    check(removeLightingGradientTiles(src, kCanvas, params, &out), "recentre: the op ran");

    const std::array<float, 3> k = lightingGradientRecentre(src, params);
    const double meanIn = meanOver(src, kCanvas, 0);
    const double meanOut = meanOver(out, kCanvas, 0);

    check(std::fabs(meanOut - meanIn) < 0.002 * meanIn,
          "recentre: the canvas mean after the op is within 0.2% of the mean before it -- the "
          "constant is derived as sum(src)/sum(ratio), not estimated");

    // The sensitivity half, and the whole reason this section exists: divide
    // the result back by the re-centring constant and what is left is the raw
    // ratio field, whose mean is 1.0 by construction. An implementation that
    // forgot the re-centre would ship exactly that -- a near-white layer at
    // 2.3x the input's mean, which the assertion above would catch and this
    // one names.
    const double rawMean = meanOut / static_cast<double>(k[0]);
    check(std::fabs(rawMean - 1.0) < 0.10,
          "recentre: the un-re-centred field's own mean is 1.0 +/- 10% regardless of the "
          "input's exposure -- which is what 'washed out' means arithmetically");
    check(std::fabs(meanIn - rawMean) > 0.3,
          "recentre: and that is far from the input's mean -- so the constant is doing real "
          "work rather than being 1.0 with extra steps");
    check(std::fabs(static_cast<double>(k[0]) - 1.0) > 0.2,
          "recentre: the constant itself is far from 1.0 on this fixture -- an assertion that "
          "passed with k == 1 would be testing nothing");

    // Alpha is coverage, not colour, and this op has no business touching it.
    bool alphaUntouched = true;
    for (int32_t y = 0; y < kSize && alphaUntouched; ++y)
      for (int32_t x = 0; x < kSize; ++x)
        if (readAt(out, x, y)[3] != readAt(src, x, y)[3]) {
          alphaUntouched = false;
          break;
        }
    check(alphaUntouched,
          "recentre: every output alpha is bit-identical to its input -- the divide changes "
          "colour, and coverage is not colour");
  }

  std::printf("  -- C. the statistics rectangle, not the request rectangle --\n");
  {
    TileStore src;
    fillLitField(src);

    LightingGradientParams params;
    params.sigma = kSigma;
    params.statsRect = kCanvas;

    const PixelRect leftHalf{0, 0, kSize / 2, kSize};
    const PixelRect rightHalf{kSize / 2, 0, kSize, kSize};

    TileStore whole, left, right;
    check(removeLightingGradientTiles(src, kCanvas, params, &whole) &&
              removeLightingGradientTiles(src, leftHalf, params, &left) &&
              removeLightingGradientTiles(src, rightHalf, params, &right),
          "stats: the whole-canvas call and the two half-canvas calls all succeed");
    check(exactlyEqualOver(left, whole, leftHalf) && exactlyEqualOver(right, whole, rightHalf),
          "stats: a request split in two is BIT-IDENTICAL to the same request made whole -- "
          "ops/Filters.hpp's seam invariant, which a mean taken over outRect would break");

    // The sensitivity half. Taking the mean over the REQUEST rectangle is the
    // naive implementation this parameter exists to prevent, and it is not a
    // subtle difference: the left half of a left-lit canvas has a very
    // different mean from the whole.
    LightingGradientParams local = params;
    local.statsRect = leftHalf;
    TileStore leftLocal;
    check(removeLightingGradientTiles(src, leftHalf, local, &leftLocal),
          "stats: the same half computed against its OWN mean also succeeds");
    check(!exactlyEqualOver(leftLocal, whole, leftHalf),
          "stats: and it DIFFERS from the whole-canvas answer -- proving the assertion above "
          "is sensitive to which rectangle the mean came from, not merely satisfied");
  }

  std::printf("  -- D. sigma 0 is the erase, and is refused rather than clamped --\n");
  {
    LightingGradientParams zero;
    zero.sigma = 0.0f;
    zero.statsRect = kCanvas;
    LightingGradientParams negative = zero;
    negative.sigma = -4.0f;
    LightingGradientParams nan = zero;
    nan.sigma = std::nanf("");
    LightingGradientParams noStats;
    noStats.sigma = kSigma;
    LightingGradientParams good;
    good.sigma = kSigma;
    good.statsRect = kCanvas;

    check(!lightingGradientParamsValid(zero) && !lightingGradientParamsValid(negative) &&
              !lightingGradientParamsValid(nan) && !lightingGradientParamsValid(noStats) &&
              lightingGradientParamsValid(good),
          "sigma0: zero, negative, non-finite and a missing statsRect are all invalid, and a "
          "real request is not");

    TileStore src;
    fillLitField(src);
    TileStore out;
    check(!removeLightingGradientTiles(src, kCanvas, zero, &out) && out.occupiedTileCount() == 0,
          "sigma0: the engine refuses sigma 0 by name and writes nothing at all");

    // Why the refusal matters, measured rather than argued: at a sigma small
    // enough to be a delta, every ratio is 1 and the result is a FLAT field of
    // the re-centring constant. That is the erase this op's zero would be, and
    // it is the reason this is the one filter in the menu whose slider must
    // not reach its own left end.
    LightingGradientParams delta;
    delta.sigma = 0.02f;
    delta.statsRect = kCanvas;
    TileStore flattened;
    check(removeLightingGradientTiles(src, kCanvas, delta, &flattened),
          "sigma0: a near-delta sigma is still a legal request");
    const std::array<float, 4> corner = readAt(flattened, 3, 3);
    bool everyTexelIdentical = true;
    for (int32_t y = 0; y < kSize && everyTexelIdentical; ++y)
      for (int32_t x = 0; x < kSize; ++x) {
        const std::array<float, 4> v = readAt(flattened, x, y);
        if (v[0] != corner[0] || v[1] != corner[1] || v[2] != corner[2]) {
          everyTexelIdentical = false;
          break;
        }
      }
    check(everyTexelIdentical,
          "sigma0: at a near-delta sigma every texel comes out the SAME colour -- the neutral "
          "setting of a divide-by-a-blur is infinity, not zero, and reading the other "
          "filters' convention across to this one would erase the layer");
  }

  std::printf("  -- E. offset: an addressing change, and offset-by-half --\n");
  {
    OpenDocument od = makeLitDocument("offset");
    const TileStore before = *od.document.layers[0].rgbTiles;

    const PixelCoord half = offsetByHalf(od);
    check(half.x == kSize / 2 && half.y == kSize / 2,
          "offset: by-half on a 256x256 canvas is exactly (128, 128)");

    OpenDocument odd = makeBlankOpenDocument(101, 33, WorkingSpace{}, "odd");
    const PixelCoord oddHalf = offsetByHalf(odd);
    check(oddHalf.x == 50 && oddHalf.y == 16,
          "offset: by-half on a 101x33 canvas floors to (50, 16) -- whole texels, so the "
          "canonical gesture never asks for a half-texel shift nothing could honour without "
          "resampling");

    const OffsetRequest request{half.x, half.y, OffsetEdge::Wrap};
    const FilterOpResult r = applyOffset(od, request);
    check(r.refusal == PixelOpRefusal::None && r.texelsChanged > 0,
          "offset: by half on an unselected RGB layer is not refused and moves texels");

    // The claim docs/operations.md:117 makes -- "free, an addressing change,
    // no filtering" -- as an exact test: every output texel is a COPY of the
    // one source texel `offsetSourceTexel()` names, bit for bit. Any
    // implementation that interpolated, or that wrapped with C's truncating
    // remainder instead of a Euclidean one, fails this on the first texel of
    // the left column.
    OffsetParams engineParams;
    engineParams.dx = half.x;
    engineParams.dy = half.y;
    engineParams.edge = OffsetEdge::Wrap;
    engineParams.wrapRect = kCanvas;
    const TileStore& after = *od.document.layers[0].rgbTiles;
    bool everyTexelIsACopy = true;
    for (int32_t y = 0; y < kSize && everyTexelIsACopy; ++y)
      for (int32_t x = 0; x < kSize; ++x) {
        const PixelCoord from = offsetSourceTexel(engineParams, PixelCoord{x, y});
        if (readAt(after, x, y) != readAt(before, from.x, from.y)) {
          everyTexelIsACopy = false;
          break;
        }
      }
    check(everyTexelIsACopy,
          "offset: every output texel is bit-identical to the source texel offsetSourceTexel() "
          "names -- no filtering anywhere, and the wrap rectangle really is the canvas");

    // The involution, which is the property a make-tileable user actually
    // leans on: offset by half, work on the seam, offset by half again and be
    // exactly where you started. True only because nothing resampled.
    const FilterOpResult back = applyOffset(od, request);
    check(back.refusal == PixelOpRefusal::None &&
              tilesExactlyEqual(*od.document.layers[0].rgbTiles, before),
          "offset: offsetting by half TWICE returns the layer bit-identical to the original -- "
          "the round trip a resampling offset would soften twice over");
  }

  std::printf("  -- F. offset refuses under a selection, and says why --\n");
  {
    OpenDocument od = makeLitDocument("offset selection");
    const TileStore before = *od.document.layers[0].rgbTiles;
    od.selection = selectRectangle(0.0f, 0.0f, 64.0f, 64.0f);
    const size_t entriesBefore = od.history.entries().size();

    const OffsetRequest request{128, 128, OffsetEdge::Wrap};
    const FilterOpResult refused = applyOffset(od, request);
    check(refused.refusal == PixelOpRefusal::SelectionActive && refused.texelsChanged == 0,
          "selection: an offset under a marquee refuses with SelectionActive and moves nothing");
    check(od.history.entries().size() == entriesBefore &&
              tilesExactlyEqual(*od.document.layers[0].rgbTiles, before),
          "selection: the refusal leaves the layer bit-identical and records no history entry "
          "-- a refusal is not an edit");

    TileStore previewOut;
    const FilterOpResult previewRefused = previewOffset(od, request, &previewOut);
    check(previewRefused.refusal == PixelOpRefusal::SelectionActive &&
              previewOut.occupiedTileCount() == 0,
          "selection: the PREVIEW refuses on the identical question rather than showing a torn "
          "document the button would then decline to produce");

    const std::string message =
        pixelOpRefusalMessage(PixelOpRefusal::SelectionActive, activeLayerOf(od), "offset");
    check(contains(message, "offset") && contains(message, "Deselect") &&
              !contains(message, "Lock"),
          "selection: the sentence names the op, names the fix (Deselect), and does not send "
          "the user looking for a lock that is not set");

    // Both ways round: the same call on the same document with the marquee
    // dropped must succeed, or the assertion above would pass for a function
    // that refused everything.
    od.selection.reset();
    const FilterOpResult allowed = applyOffset(od, request);
    check(allowed.refusal == PixelOpRefusal::None && allowed.texelsChanged > 0,
          "selection: with the marquee dropped the identical request succeeds -- the gate is "
          "the selection, not a hard-coded no");

    // The layer-shaped questions still come first. A locked layer under a
    // selection must refuse for the LOCK, which is the answer the LAYERS panel
    // can act on; answering SelectionActive there would send the user to
    // deselect and leave them just as stuck.
    OpenDocument locked = makeLitDocument("offset locked");
    locked.document.layers[0].locked = true;
    locked.selection = selectRectangle(0.0f, 0.0f, 64.0f, 64.0f);
    check(applyOffset(locked, request).refusal == PixelOpRefusal::Locked,
          "selection: a locked layer under a marquee refuses Locked, not SelectionActive -- "
          "the layer-shaped question is asked first");
  }

  std::printf("  -- G. history, and the menu rows --\n");
  {
    OpenDocument od = makeLitDocument("tileable history");
    const TileStore before = *od.document.layers[0].rgbTiles;
    const size_t entriesBefore = od.history.entries().size();

    const FilterOpResult r = applyRemoveLightingGradient(od, kSigma);
    check(r.refusal == PixelOpRefusal::None && r.texelsChanged > 0,
          "history: lighting-gradient removal on the fixture changes texels");
    check(od.history.entries().size() == entriesBefore + 1 && !od.unsavedEdits.empty() &&
              od.unsavedEdits.back() == "remove lighting gradient",
          "history: exactly one entry, named for the op rather than a shared \"filter\"");
    const Document* prior = od.history.undo();
    check(prior != nullptr, "history: undo returns the pre-op document");
    if (prior != nullptr) od.document = *prior;
    check(tilesExactlyEqual(*od.document.layers[0].rgbTiles, before),
          "history: undo restores the pre-op tiles EXACTLY -- memcmp on the raw half words");

    // A sigma the engine refuses is a no-op, not a crash and not an entry:
    // app/FilterOps.hpp's own rule for every request an engine declines.
    OpenDocument odZero = makeLitDocument("tileable zero");
    const size_t zeroBefore = odZero.history.entries().size();
    const FilterOpResult zero = applyRemoveLightingGradient(odZero, 0.0f);
    check(zero.refusal == PixelOpRefusal::None && zero.texelsChanged == 0 &&
              odZero.history.entries().size() == zeroBefore,
          "history: a sigma the engine refuses records nothing and reports zero texels -- an "
          "edit that changed nothing must not create an undo step");

    MenuContext usable;
    usable.hasDocument = true;
    usable.filterLayerUsable = true;
    MenuContext unusable;
    unusable.hasDocument = true;
    unusable.filterLayerUsable = false;
    // The note the refusal itself would print, not a generic one -- the same
    // sentence `runFilterMenuTest()` feeds its own disabled-item assertion,
    // which is what makes "carries the SAME note its neighbours do" a claim
    // about the shared `filterItem()` lambda rather than about a default.
    unusable.filterRefusalNote = "not RGB: \"Backdrop\" holds Latents, not RGBA.";
    const std::vector<MenuNode> usableBar = buildMenuModel(usable);
    const std::vector<MenuNode> unusableBar = buildMenuModel(unusable);

    const MenuNode* removeOn = findMenuAction(usableBar, MenuAction::RemoveLightingGradient);
    const MenuNode* offsetOn = findMenuAction(usableBar, MenuAction::Offset);
    const MenuNode* removeOff = findMenuAction(unusableBar, MenuAction::RemoveLightingGradient);
    const MenuNode* offsetOff = findMenuAction(unusableBar, MenuAction::Offset);
    check(removeOn != nullptr && offsetOn != nullptr && removeOn->enabled && offsetOn->enabled,
          "menu: both D8 items are present in the built tree and live when the active layer "
          "can take a pixel op");
    check(removeOff != nullptr && offsetOff != nullptr && !removeOff->enabled &&
              !offsetOff->enabled && contains(removeOff->tooltip, "Backdrop") &&
              contains(offsetOff->tooltip, "Backdrop"),
          "menu: both go grey with filterLayerUsable false and carry the same refusal note "
          "their eight neighbours do -- a disabled item with no reason reads as missing");
    check(menuActionEffect(MenuAction::RemoveLightingGradient) == MenuEffect::Deferred &&
              menuActionEffect(MenuAction::Offset) == MenuEffect::Deferred,
          "menu: both are Deferred, not Inline -- each opens an ImGui modal, which an AppKit "
          "menu callback has no frame in progress to do");

    const std::string removeLabel = menuItemSpec(MenuAction::RemoveLightingGradient).label;
    const std::string offsetLabel = menuItemSpec(MenuAction::Offset).label;
    check(contains(removeLabel, "...") && contains(offsetLabel, "..."),
          "menu: both labels end in an ellipsis -- an ellipsis promises a dialog and both of "
          "these ask the user for a number before they act");
  }

  std::printf("[selftest] tileable ops %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
