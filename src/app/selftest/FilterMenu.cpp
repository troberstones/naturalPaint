#include "app/selftest/Support.hpp"

#include <cstring>

#include "app/FilterOps.hpp"
#include "ui/MenuModel.hpp"

namespace np {

namespace {

// The same deterministic value source app/selftest/Blur.cpp and
// app/selftest/Filters.cpp each keep a private copy of -- splitmix64's
// finalizer, three lines, not <random> -- so this section's fixture does not
// invent a fourth mixer. Each ops/ selftest section owns its own copy rather
// than sharing one, and this section follows that precedent rather than
// breaking it: a shared fixture header would be one more file a change to any
// of the four could ripple through.
float filterMenuTestNoise(uint64_t i) noexcept {
  uint64_t z = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);  // [0,1)
}

// A 2x2-tile (256x256) opaque, varying, premultiplied field -- big enough
// that a sigma-8 Gaussian Blur has real neighbourhoods to average and that a
// partial selection's "outside" region (section B) lives in tiles the filter
// actually touched, small enough that this section stays fast. Opaque so RGB
// and A moving in lockstep has a meaningful answer, matching
// app/selftest/Blur.cpp's own fixture.
void fillFilterMenuField(TileStore& tiles) {
  uint64_t counter = 0;
  for (int32_t ty = 0; ty < 2; ++ty) {
    for (int32_t tx = 0; tx < 2; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t y = 0; y < kTileSize; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          const float v = filterMenuTestNoise(counter++);
          t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.5f * v, 1.0f});
        }
      }
    }
  }
}

// One RGB-layer document, 256x256, layer 0 filled by `fillFilterMenuField()`
// -- the fixture every section below builds on, so the shape of "make a
// document, put content on it" is written once.
OpenDocument makeFilterMenuDocument(const char* title) {
  OpenDocument od = makeBlankOpenDocument(256, 256, WorkingSpace{}, title);
  fillFilterMenuField(*od.document.layers[0].rgbTiles);
  return od;
}

// Bit-for-bit equality of every occupied tile in both stores -- the raw half
// words, `memcmp`'d, the same standard app/StrokeSession's own
// stroke-granularity undo claim is held to. Requires the occupied tile SETS
// to match too: after `core::History::undo()` replaces the whole `Document`,
// a tile the filter allocated and undo removed again must genuinely be gone,
// not merely re-zeroed, or a document that grew tiles under a filter and
// shrank back under undo would look "equal" by content while differing in
// what `TileStoreOf::occupiedTileCount()` reports -- which is exactly the
// number `core::History`'s own byte accounting depends on being honest.
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

std::array<float, 4> readAt(const TileStore& store, int32_t x, int32_t y) {
  const Tile* tile = store.find(tileCoordAt(PixelCoord{x, y}));
  if (tile == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
  return tile->readPixel(tileLocalOffset(PixelCoord{x, y}));
}

// A minimal `MenuFamilyEntry`-shaped row, matching
// app/selftest/MenuModel.cpp's own `row()` -- kept as a private copy for the
// same reason `filterMenuTestNoise()` is: this section is meant to read on
// its own, not by cross-referencing another TU's anonymous namespace.
MenuFamilyEntry menuRow(const char* label) {
  MenuFamilyEntry e;
  e.label = label;
  return e;
}

// Depth-first search for one action in a built menu tree -- the same walk
// `ui/MenuModel.cpp`'s own (private) `findNode()` performs, rewritten here
// because that one has no external linkage. Returns the first Command/Check
// node carrying `action`, or nullptr.
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
bool runFilterMenuTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  const PixelRect kCanvasRect{0, 0, 256, 256};

  std::printf("  -- A. each item reaches ITS OWN engine call with ITS OWN parameters --\n");
  {
    // Gaussian Blur: the layer's result, after `applyGaussianBlur()`, must be
    // bit-identical to calling `blurTiles()` directly with the SAME sigma on
    // a copy of the pre-filter field -- not merely "changed", which a filter
    // using the wrong radius would also satisfy.
    OpenDocument od = makeFilterMenuDocument("blur");
    const TileStore original = *od.document.layers[0].rgbTiles;
    const FilterOpResult r = applyGaussianBlur(od, 3.0f);
    check(r.refusal == PixelOpRefusal::None && r.texelsChanged > 0,
          "blur: a non-zero sigma on a filled RGB layer changes texels and is not refused");

    TileStore reference;
    BlurParams bp;
    bp.kind = BlurKind::Gaussian;
    bp.sigma = 3.0f;
    check(blurTiles(original, kCanvasRect, bp, &reference),
          "blur: the reference engine call (sigma 3.0, same as the dialog) succeeds");
    bool matchesReference = true;
    for (int32_t y = 0; y < 256 && matchesReference; ++y)
      for (int32_t x = 0; x < 256; ++x) {
        const Tile* refTile = reference.find(tileCoordAt(PixelCoord{x, y}));
        const std::array<float, 4> want =
            refTile != nullptr ? refTile->readPixel(tileLocalOffset(PixelCoord{x, y}))
                               : readAt(original, x, y);
        if (readAt(*od.document.layers[0].rgbTiles, x, y) != want) {
          matchesReference = false;
          break;
        }
      }
    check(matchesReference,
          "blur: the layer's result is bit-identical to blurTiles() called directly with the "
          "SAME sigma -- the assertion a dropped or defaulted radius would fail");

    // Sharpen and Unsharp Mask legitimately share `unsharpMaskTiles()`
    // (ops/Filters.hpp section 3: Sharpen IS that call with the radius
    // fixed), so the assertion that matters is not "which function ran" but
    // "with which radius". At Unsharp Mask's own radius set to EXACTLY
    // `kSharpenSigma`, the two must agree -- proving Sharpen truly uses the
    // fixed radius. At a DIFFERENT radius they must differ -- proving Unsharp
    // Mask's dialog-supplied radius is not silently discarded in favour of
    // Sharpen's.
    OpenDocument odSharp = makeFilterMenuDocument("sharpen");
    OpenDocument odUnsharpSame = makeFilterMenuDocument("unsharp-same-radius");
    OpenDocument odUnsharpOther = makeFilterMenuDocument("unsharp-other-radius");

    const FilterOpResult sharpR = applySharpen(odSharp, 1.0f);
    UnsharpParams sameRadius;
    sameRadius.blur.kind = BlurKind::Gaussian;
    sameRadius.blur.sigma = kSharpenSigma;
    sameRadius.amount = 1.0f;
    sameRadius.threshold = 0.0f;
    const FilterOpResult unsharpSameR = applyUnsharpMask(odUnsharpSame, sameRadius);

    UnsharpParams otherRadius = sameRadius;
    otherRadius.blur.sigma = 5.0f;  // deliberately far from kSharpenSigma (1.0)
    const FilterOpResult unsharpOtherR = applyUnsharpMask(odUnsharpOther, otherRadius);

    check(sharpR.refusal == PixelOpRefusal::None && unsharpSameR.refusal == PixelOpRefusal::None &&
              unsharpOtherR.refusal == PixelOpRefusal::None,
          "sharpen/unsharp: neither is refused on a filled RGB layer");
    check(tilesExactlyEqual(*odSharp.document.layers[0].rgbTiles,
                            *odUnsharpSame.document.layers[0].rgbTiles),
          "sharpen == unsharp mask AT kSharpenSigma: Sharpen's fixed radius really is "
          "kSharpenSigma, not a second, silently different constant");
    check(!tilesExactlyEqual(*odSharp.document.layers[0].rgbTiles,
                             *odUnsharpOther.document.layers[0].rgbTiles),
          "sharpen != unsharp mask AT A DIFFERENT RADIUS: Unsharp Mask's dialog-supplied radius "
          "reaches unsharpMaskTiles() rather than being dropped for Sharpen's constant");

    // Add Noise: two different seeds must produce different results (the
    // seed is not dropped for a hardcoded default), and the SAME seed must
    // match `addNoiseTiles()` called directly (the wiring is not adding its
    // own transformation on top of the engine's).
    OpenDocument odNoiseA = makeFilterMenuDocument("noise-seed-a");
    OpenDocument odNoiseB = makeFilterMenuDocument("noise-seed-b");
    const TileStore noiseOriginal = *odNoiseA.document.layers[0].rgbTiles;
    NoiseParams npA;
    npA.amount = 0.05f;
    npA.distribution = NoiseDistribution::Uniform;
    npA.seed = 12345;
    NoiseParams npB = npA;
    npB.seed = 67890;
    const FilterOpResult noiseRA = applyAddNoise(odNoiseA, npA);
    const FilterOpResult noiseRB = applyAddNoise(odNoiseB, npB);
    check(noiseRA.refusal == PixelOpRefusal::None && noiseRA.texelsChanged > 0 &&
              noiseRB.refusal == PixelOpRefusal::None && noiseRB.texelsChanged > 0,
          "add noise: a non-zero amount on a filled RGB layer changes texels");
    check(!tilesExactlyEqual(*odNoiseA.document.layers[0].rgbTiles,
                             *odNoiseB.document.layers[0].rgbTiles),
          "add noise: two different seeds give two different results -- the seed field reaches "
          "the engine rather than every invocation using one hardcoded default");

    TileStore noiseReference;
    check(addNoiseTiles(noiseOriginal, kCanvasRect, npA, &noiseReference),
          "add noise: the reference engine call (seed 12345, same as the dialog) succeeds");
    bool noiseMatchesReference = true;
    for (int32_t y = 0; y < 256 && noiseMatchesReference; ++y)
      for (int32_t x = 0; x < 256; ++x) {
        const Tile* refTile = noiseReference.find(tileCoordAt(PixelCoord{x, y}));
        const std::array<float, 4> want =
            refTile != nullptr ? refTile->readPixel(tileLocalOffset(PixelCoord{x, y}))
                               : readAt(noiseOriginal, x, y);
        if (readAt(*odNoiseA.document.layers[0].rgbTiles, x, y) != want) {
          noiseMatchesReference = false;
          break;
        }
      }
    check(noiseMatchesReference,
          "add noise: the layer's result is bit-identical to addNoiseTiles() called directly "
          "with the SAME params");
  }

  std::printf("  -- B. the active selection bounds a Filter-menu op --\n");
  {
    // Integer edges, so the rectangle's ramp lands exactly on texel
    // boundaries and every probed texel is fully in or fully out --
    // app/selftest/BucketRefusal.cpp section E's own discipline, restated
    // here because a filter's "outside" claim needs the identical rigour a
    // fill's does.
    //
    // **Deliberately NOT tile-aligned.** `compositeFilterResult()` gates in
    // two passes -- a per-TILE "does this tile need writing at all" scan,
    // then a per-TEXEL write -- and a selection whose boundary sits exactly
    // on a tile edge (the tempting, tidy choice) lets the tile-level scan
    // alone decide which whole tiles are touched, so a defect confined to
    // the per-texel gate could pass unnoticed: every excluded tile would
    // already have been skipped before the broken gate ever ran. Selecting
    // a quarter of ONE tile forces some texels of that same tile to be
    // covered and others not, so the per-texel gate is the only thing
    // standing between them.
    OpenDocument od = makeFilterMenuDocument("blur selection");
    const TileStore before = *od.document.layers[0].rgbTiles;
    od.selection = selectRectangle(0.0f, 0.0f, 64.0f, 64.0f);  // one quarter of tile (0,0)

    const FilterOpResult r = applyGaussianBlur(od, 8.0f);
    check(r.refusal == PixelOpRefusal::None && r.texelsChanged > 0,
          "selection: the bounded blur still runs and changes something inside the rectangle");

    const TileStore& after = *od.document.layers[0].rgbTiles;

    // The WHOLE excluded region, exhaustively -- every one of the 65 536
    // texels in the 256x256 canvas except the 4 096 inside [0,64)x[0,64) --
    // per this task's own instruction that a sample can pass a gate that
    // leaked at one edge. This includes the REST OF TILE (0,0) -- x in
    // [64,128) with y in [0,64), and all of y in [64,128) -- which is the
    // region only the per-texel gate protects, not the per-tile scan.
    bool outsideUntouched = true;
    for (int32_t y = 0; y < 256 && outsideUntouched; ++y) {
      for (int32_t x = 0; x < 256; ++x) {
        if (x < 64 && y < 64) continue;  // inside the selection; section below
        if (readAt(after, x, y) != readAt(before, x, y)) {
          outsideUntouched = false;
          break;
        }
      }
    }
    check(outsideUntouched,
          "selection: EVERY texel outside the rectangle is bit-identical to before -- the "
          "whole 256x256 canvas minus the 64x64 selected corner, including the REST OF THE "
          "SAME TILE the selection partially covers, not a sample and not tile-granular");

    bool insideChanged = false;
    for (int32_t y = 0; y < 64 && !insideChanged; ++y)
      for (int32_t x = 0; x < 64; ++x)
        if (readAt(after, x, y) != readAt(before, x, y)) {
          insideChanged = true;
          break;
        }
    check(insideChanged, "selection: at least the interior of the rectangle did change");

    // Contrast: the identical op with NO selection touches the whole layer,
    // so "the gate respects a selection" is distinguished from "the gate
    // always shrinks the region to something".
    OpenDocument odNoSel = makeFilterMenuDocument("blur no selection");
    const TileStore beforeNoSel = *odNoSel.document.layers[0].rgbTiles;
    applyGaussianBlur(odNoSel, 8.0f);
    bool farCornerChanged =
        readAt(*odNoSel.document.layers[0].rgbTiles, 255, 255) != readAt(beforeNoSel, 255, 255);
    check(farCornerChanged,
          "selection: with NO selection engaged, the far tile changes too -- proving the "
          "previous section's silence was the selection gate and not a rectangle-shaped default");
  }

  std::printf("  -- C. history: exactly one entry, undo exact, identity records nothing --\n");
  {
    OpenDocument od = makeFilterMenuDocument("blur history");
    const TileStore beforeFilter = *od.document.layers[0].rgbTiles;
    const size_t entriesBefore = od.history.entries().size();
    const uint64_t revisionBefore = od.revision;

    const FilterOpResult r = applyGaussianBlur(od, 8.0f);
    check(r.texelsChanged > 0, "history: the blur changed texels, so it has something to record");
    check(od.history.entries().size() == entriesBefore + 1,
          "history: exactly ONE entry was recorded, not one per tile or per texel");
    check(!od.unsavedEdits.empty() && od.unsavedEdits.back() == "gaussian blur",
          "history: the entry is named \"gaussian blur\", not \"brush stroke\" or a shared "
          "\"filter\"");
    check(od.revision > revisionBefore, "history: the revision moved");

    const Document* prior = od.history.undo();
    check(prior != nullptr, "history: undo returns the pre-filter document");
    if (prior != nullptr) od.document = *prior;
    check(tilesExactlyEqual(*od.document.layers[0].rgbTiles, beforeFilter),
          "history: undo restores the pre-filter tiles EXACTLY -- memcmp on the raw half "
          "words, not a tolerance");

    // The identity request: sigma 0 is `ops/Blur.hpp`'s own stated identity.
    // `applyGaussianBlur()` must therefore change nothing and record nothing
    // -- the same "an edit that changed nothing must not create an undo
    // step" rule app/selftest/BucketRefusal.cpp proves for a re-fill with the
    // colour already there.
    OpenDocument odIdentity = makeFilterMenuDocument("blur identity");
    const size_t entriesBeforeIdentity = odIdentity.history.entries().size();
    const uint64_t revisionBeforeIdentity = odIdentity.revision;
    const FilterOpResult idR = applyGaussianBlur(odIdentity, 0.0f);
    check(idR.texelsChanged == 0 &&
              odIdentity.history.entries().size() == entriesBeforeIdentity &&
              odIdentity.revision == revisionBeforeIdentity,
          "history: sigma 0 (the identity request) changes zero texels and records ZERO "
          "history entries");
  }

  std::printf("  -- D. the refusal is app/StrokeSession's PixelOpRefusal, reused --\n");
  {
    struct Case {
      const char* what;
      Layer (*make)(std::string);
      bool lock;
      PixelOpRefusal want;
    };
    const Case cases[] = {
        {"pigment", &makePigmentLayer, false, PixelOpRefusal::NoRgbStore},
        {"locked RGB", &makeRgbLayer, true, PixelOpRefusal::Locked},
    };
    for (const Case& c : cases) {
      OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "filter refusal");
      const size_t at = od.document.layers.size();
      recordLayerEdit(od, addLayer(od.document, at, c.make(std::string("Backdrop ") + c.what)));
      if (c.lock) recordLayerEdit(od, setLayerLocked(od.document, at, true));
      od.activeLayer = at;

      const uint64_t revisionBefore = od.revision;
      const size_t entriesBefore = od.history.entries().size();
      const FilterOpResult blurR = applyGaussianBlur(od, 8.0f);
      const FilterOpResult noiseR = applyAddNoise(od, NoiseParams{0.1f});

      check(blurR.refusal == c.want && noiseR.refusal == c.want,
            "refusal: both Filter-menu ops refuse for the SAME reason on the SAME layer -- one "
            "predicate, not a per-op copy that could disagree");
      check(blurR.texelsChanged == 0 && noiseR.texelsChanged == 0 && od.revision == revisionBefore &&
                od.history.entries().size() == entriesBefore,
            "refusal: not one texel moves and nothing is recorded -- exact zero, the same "
            "standard app/selftest/BucketRefusal.cpp holds the bucket to");

      // Reused, not reinvented: the bucket's own message-builder, called with
      // the identical layer and reason, names the layer identically -- only
      // the op's own noun differs.
      const Layer* target = activeLayerOf(od);
      const std::string filterMsg = pixelOpRefusalMessage(blurR.refusal, target, "gaussian blur");
      const std::string bucketMsg = pixelOpRefusalMessage(blurR.refusal, target, "paint bucket");
      check(contains(filterMsg, "Backdrop") && contains(bucketMsg, "Backdrop"),
            "refusal: the sentence names the layer, exactly as it does for the bucket");
      check(filterMsg != bucketMsg,
            "refusal: the two sentences differ only in the op's own name -- \"gaussian blur\" "
            "is not printed as \"paint bucket\"");
    }

    // No layer at all: `activeLayerOf()` on a document with no layers.
    OpenDocument empty = makeBlankOpenDocument(64, 64, WorkingSpace{});
    empty.document.layers.clear();
    const FilterOpResult noLayerR = applyGaussianBlur(empty, 8.0f);
    check(noLayerR.refusal == PixelOpRefusal::NoLayer && noLayerR.texelsChanged == 0,
          "refusal: an empty layer stack answers NoLayer, the same as the bucket's nullptr case");
  }

  std::printf("  -- E. Image Size and Canvas Size (ops/DocumentTransform) --\n");
  {
    OpenDocument od = makeFilterMenuDocument("image size");
    const size_t entriesBefore = od.history.entries().size();
    const DocumentOpOutcome r = applyImageSize(od, 128, 128, ResampleKernel::CatmullRom);
    check(r.ok, "image size: a valid downscale succeeds");
    check(od.document.width == 128 && od.document.height == 128,
          "image size: the document's own extent changed");
    check(od.history.entries().size() == entriesBefore + 1,
          "image size: exactly one Structural history entry");

    // A 1:1 request: no dimension change, so no edit -- the document-level
    // twin of section C's sigma-0 rule.
    const size_t entriesBeforeNoOp = od.history.entries().size();
    const DocumentOpOutcome noOp = applyImageSize(od, 128, 128, ResampleKernel::Lanczos3);
    check(noOp.ok && od.history.entries().size() == entriesBeforeNoOp,
          "image size: a request at the CURRENT extent records no entry, whatever kernel it "
          "names -- a no-op the user asked for is not an edit");

    // Refusal: a zero extent. ops/DocumentTransform's own vocabulary (an
    // error string), not PixelOpRefusal -- this is a document-level op and
    // app/FilterOps.hpp argues why it does not take a layer-shaped refusal.
    const size_t entriesBeforeRefused = od.history.entries().size();
    const DocumentOpOutcome refused = applyImageSize(od, 0, 100, ResampleKernel::CatmullRom);
    check(!refused.ok && !refused.error.empty() &&
              od.history.entries().size() == entriesBeforeRefused,
          "image size: a zero extent refuses by name and records nothing");

    OpenDocument odCanvas = makeFilterMenuDocument("canvas size");
    const std::array<float, 4> beforeCanvasOrigin =
        readAt(*odCanvas.document.layers[0].rgbTiles, 0, 0);
    const std::array<float, 4> beforeCanvasCorner =
        readAt(*odCanvas.document.layers[0].rgbTiles, 255, 255);
    const size_t canvasEntriesBefore = odCanvas.history.entries().size();
    const DocumentOpOutcome canvasR =
        applyCanvasSize(odCanvas, 300, 300, CanvasAnchor::Center);
    check(canvasR.ok && odCanvas.document.width == 300 && odCanvas.document.height == 300,
          "canvas size: the extent grows to what was asked");
    check(odCanvas.history.entries().size() == canvasEntriesBefore + 1,
          "canvas size: exactly one Structural history entry");
    // Existing pixels keep their values EXACTLY -- ops/Transform.hpp's
    // cropImage() is a zero-resample index copy (this header's own section 4
    // says so). The centred anchor places the original (0,0) at (22,22) for
    // a 256->300 growth split symmetrically (22 + 256 + 22 = 300).
    check(readAt(*odCanvas.document.layers[0].rgbTiles, 22, 22) == beforeCanvasOrigin &&
              readAt(*odCanvas.document.layers[0].rgbTiles, 277, 277) == beforeCanvasCorner,
          "canvas size: the original content's own corners moved by exactly the anchor's "
          "offset and no value changed -- zero resamples, an index copy");

    // A 1:1 request records nothing, same rule as Image Size above.
    const size_t canvasEntriesBeforeNoOp = odCanvas.history.entries().size();
    const DocumentOpOutcome canvasNoOp = applyCanvasSize(odCanvas, 300, 300, CanvasAnchor::TopLeft);
    check(canvasNoOp.ok && odCanvas.history.entries().size() == canvasEntriesBeforeNoOp,
          "canvas size: a request at the current extent records no entry, whatever anchor it "
          "names");
  }

  std::printf("  -- F. enable predicates, as pure functions of constructed state --\n");
  {
    // Filter menu: follows ctx.filterLayerUsable exactly, the same predicate
    // section D exercised directly against real layers -- this half checks
    // that buildMenuModel() actually reads it. No window, no document, no
    // GPU: MenuContext is built by hand, the way app/selftest/MenuModel.cpp's
    // own richContext() is.
    MenuContext usable;
    usable.hasDocument = true;
    usable.filterLayerUsable = true;
    const std::vector<MenuNode> usableMenus = buildMenuModel(usable);
    const MenuNode* blurUsable = findMenuAction(usableMenus, MenuAction::GaussianBlur);
    const MenuNode* sharpenUsable = findMenuAction(usableMenus, MenuAction::Sharpen);
    const MenuNode* unsharpUsable = findMenuAction(usableMenus, MenuAction::UnsharpMask);
    const MenuNode* noiseUsable = findMenuAction(usableMenus, MenuAction::AddNoise);
    check(blurUsable != nullptr && blurUsable->enabled && sharpenUsable != nullptr &&
              sharpenUsable->enabled && unsharpUsable != nullptr && unsharpUsable->enabled &&
              noiseUsable != nullptr && noiseUsable->enabled,
          "enable: all four Filter items are enabled when filterLayerUsable is true");

    MenuContext unusable;
    unusable.hasDocument = true;
    unusable.filterLayerUsable = false;
    unusable.filterRefusalNote = "not RGB: \"Backdrop\" holds Latents, not RGBA.";
    const std::vector<MenuNode> unusableMenus = buildMenuModel(unusable);
    const MenuNode* blurDisabled = findMenuAction(unusableMenus, MenuAction::GaussianBlur);
    check(blurDisabled != nullptr && !blurDisabled->enabled &&
              contains(blurDisabled->tooltip, "Backdrop"),
          "enable: disabled with filterLayerUsable false, and the tooltip is the SAME "
          "sentence the refusal would print -- not a second, generic one");

    // Image menu: follows hasDocument alone, no layer-shaped input at all.
    MenuContext noDoc;
    noDoc.hasDocument = false;
    noDoc.filterLayerUsable = false;  // never true with no document
    const std::vector<MenuNode> noDocMenus = buildMenuModel(noDoc);
    const MenuNode* imageSizeNoDoc = findMenuAction(noDocMenus, MenuAction::ImageSize);
    const MenuNode* canvasSizeNoDoc = findMenuAction(noDocMenus, MenuAction::CanvasSize);
    check(imageSizeNoDoc != nullptr && !imageSizeNoDoc->enabled && canvasSizeNoDoc != nullptr &&
              !canvasSizeNoDoc->enabled,
          "enable: Image Size and Canvas Size are disabled with no document open");

    MenuContext withDoc;
    withDoc.hasDocument = true;
    withDoc.filterLayerUsable = false;  // a Pigment-only document: Image menu still usable
    const std::vector<MenuNode> withDocMenus = buildMenuModel(withDoc);
    const MenuNode* imageSizeWithDoc = findMenuAction(withDocMenus, MenuAction::ImageSize);
    const MenuNode* canvasSizeWithDoc = findMenuAction(withDocMenus, MenuAction::CanvasSize);
    check(imageSizeWithDoc != nullptr && imageSizeWithDoc->enabled &&
              canvasSizeWithDoc != nullptr && canvasSizeWithDoc->enabled,
          "enable: Image Size and Canvas Size are enabled with a document open EVEN WHEN "
          "filterLayerUsable is false -- a document-level op does not take a layer-shaped "
          "refusal (app/FilterOps.hpp's own argument)");
  }

  std::printf("[selftest] filter menu %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
