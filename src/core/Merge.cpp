#include "core/Merge.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <unordered_set>
#include <utility>

#include "core/Blend.hpp"
#include "core/Composite.hpp"
#include "core/LayerOpRefusal.hpp"
#include "core/Mask.hpp"
#include "core/Pigment.hpp"
#include "core/TextContent.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"
#include "core/VectorRaster.hpp"

namespace np {
namespace {

// --- The refusal idiom is no longer rebuilt here --------------------------
//
// This file used to carry its own `fail`, `succeed`, `describe`, `inRange` and
// `notLocked`, with a comment saying they were copies of core/LayerOps.cpp's
// and that sharing them was "the right change" deferred to a later step. That
// step happened: they are core/LayerOpRefusal.hpp's `layerOpFail`,
// `layerOpSucceed`, `layerOpDescribe`, `layerOpInRange` and `layerOpNotLocked`,
// included above.
//
// One thing the copies had already lost, and worth knowing before reading the
// call sites below: the two `notLocked`s had **drifted**, and this file's
// second sentence ("A merge destroys the layers it merges...") is the better
// one for these five operations. It survives, as `kLockedMergeDestroys`, which
// every merge refusal below passes explicitly. See core/LayerOpRefusal.hpp §3.

// The two kinds that own pixel storage in this build. The other five own none
// -- core/Layer.hpp is explicit that Adjustment/Text/Strokes/Flats never will,
// and Media has no per-medium state yet -- so this is the predicate that
// separates "a layer a merge can read" from "a layer a merge has to answer
// for". core/Composite.cpp has the same three lines in its own anonymous
// namespace, for its own walk.
bool holdsPixels(const Layer& layer) noexcept {
  return (layer.kind == LayerKind::RGB && layer.rgbTiles.has_value()) ||
         (layer.kind == LayerKind::Pigment && layer.pigmentTiles.has_value());
}

// The layer's blend, resolved once. An unrecognised name (PRD I10's verbatim
// carry of a newer build's mode) is **not** `normal` -- it is a mode this
// build cannot honour, which core/Composite composites as `over` and warns
// about, and which merge down refuses for §4's reason.
bool blendIsNormal(const Layer& layer) {
  const std::optional<BlendMode> mode = blendModeFromName(layer.blend);
  return mode.has_value() && *mode == BlendMode::Normal;
}

bool blendIsMix(const Layer& layer) {
  const std::optional<BlendMode> mode = blendModeFromName(layer.blend);
  return mode.has_value() && *mode == BlendMode::Mix;
}

// A document with `doc`'s canvas and working space and no layers -- the shell
// every sub-document composite in this file is built in. The canvas has to
// match, because the composite buffer is indexed by document pixel and the
// merged layer's tiles are written back at the same coordinates.
Document canvasOf(const Document& doc) {
  Document sub;
  sub.width = doc.width;
  sub.height = doc.height;
  sub.workingSpace = doc.workingSpace;
  return sub;
}

bool tileOutsideCanvas(const Document& doc, TileCoord coord) noexcept {
  const PixelCoord origin = tileOrigin(coord);
  return origin.x >= doc.width || origin.y >= doc.height || origin.x + kTileSize <= 0 ||
         origin.y + kTileSize <= 0;
}

template <class Store>
size_t outsideCanvas(const Document& doc, const Store* store) noexcept {
  if (store == nullptr) return 0;
  size_t n = 0;
  for (const auto& entry : *store)
    if (tileOutsideCanvas(doc, entry.first)) ++n;
  return n;
}

void append(std::vector<std::string>* out, std::string sentence) {
  if (out != nullptr) out->push_back(std::move(sentence));
}

// §3. One sentence per operation, not per layer, so a merge of forty layers
// does not produce forty lines saying the same thing.
void warnOffCanvas(const Document& doc, const std::vector<size_t>& indices, const char* what,
                   std::vector<std::string>* out) {
  if (out == nullptr) return;
  size_t tiles = 0;
  for (const size_t i : indices) {
    const Layer& layer = doc.layers[i];
    tiles += outsideCanvas(doc, layer.rgbTiles.has_value() ? &*layer.rgbTiles : nullptr);
    tiles += outsideCanvas(doc, layer.pigmentTiles.has_value() ? &*layer.pigmentTiles : nullptr);
    tiles += outsideCanvas(doc, layer.mask.has_value() ? &*layer.mask : nullptr);
  }
  if (tiles == 0) return;
  append(out, std::string(what) + " discarded " + std::to_string(tiles) +
                  " occupied tile(s) lying entirely outside the " + std::to_string(doc.width) +
                  "x" + std::to_string(doc.height) +
                  " canvas: a merge is computed through core/Composite, which composites the "
                  "document's canvas rather than its content's bounding box (core/Merge.hpp §3).");
}

// §4. Everything the compositor folded into the merged pixels that used to be
// a separate, adjustable property of a layer. Reported because each one was
// reversible before the merge and is not afterwards.
void warnBaked(const Document& doc, const std::vector<size_t>& indices, const char* what,
               std::vector<std::string>* out) {
  if (out == nullptr) return;
  size_t masks = 0, stacks = 0, opacities = 0;
  for (const size_t i : indices) {
    const Layer& layer = doc.layers[i];
    if (layer.mask.has_value()) ++masks;
    if (layer.ops.size() > 0) ++stacks;
    if (layer.opacity != 1.0f) ++opacities;
  }
  if (masks > 0)
    append(out, std::string(what) + " baked " + std::to_string(masks) +
                    " layer mask(s) into the merged layer's alpha; the mask(s) no longer exist "
                    "separately.");
  if (stacks > 0)
    append(out, std::string(what) + " baked " + std::to_string(stacks) +
                    " per-layer op stack(s) into pixels; those grades were non-destructive "
                    "before the merge and are not now.");
  if (opacities > 0)
    append(out, std::string(what) + " baked " + std::to_string(opacities) +
                    " layer opacity value(s) into the merged layer's alpha; the merged layer is "
                    "at opacity 1.");
}

// §5's loss, reported where it cannot be refused.
//
// Merge down refuses a Pigment pair rather than degrade it to RGB, and can:
// there is always another gesture. Merge visible and flatten cannot -- they
// collapse an entire stack whose other members are RGB layers and adjustment
// layers, so the result has to be RGB, and refusing every document that
// contains a Pigment layer would make PRD C10's P0 unusable in a program whose
// default layer kind is Pigment. So this is the one place a latent really is
// spent, and the rule that applies is core/Composite.hpp §7's: the pixels are
// what the user is looking at, and every boundary that makes the loss durable
// says so.
void warnPigmentLost(const Document& doc, const std::vector<size_t>& indices, const char* what,
                     std::vector<std::string>* out) {
  if (out == nullptr) return;
  size_t pigment = 0;
  for (const size_t i : indices)
    if (doc.layers[i].kind == LayerKind::Pigment) ++pigment;
  if (pigment == 0) return;
  append(out, std::string(what) + " turned " + std::to_string(pigment) +
                  " Pigment layer(s) into RGB. The latents are gone, so the result cannot be "
                  "`Mix`ed (PRD C3) and an eraser reduces its alpha rather than its mass (PRD "
                  "F10). Merge Down refuses that trade and keeps a Mix-paired pair in latent "
                  "space; this operation collapses a whole stack and has nowhere to keep it.");
}

// core/Composite's own warnings, carried out with their origin named. Their
// layer indices are indices into the *sub-document* that was composited, which
// is not the document the caller is holding -- so the prefix says so rather
// than letting a reader take "layer 1" for a row in the panel.
void carryCompositeWarnings(const char* what, const char* scope,
                            const std::vector<std::string>& from, std::vector<std::string>* out) {
  if (out == nullptr) return;
  for (const std::string& w : from)
    append(out, std::string(what) + ": while compositing " + scope + ", " + w);
}

// The one place a merged layer is put together, so the five operations cannot
// disagree about what a merged layer *is*: opacity 1, visible, `normal`, no
// mask, no ops, unlocked. Only `clipped` and `parent` vary, and only merge
// down sets them (§6).
Layer mergedLayer(const Document& doc, const std::vector<float>& premultiplied, std::string name) {
  return layerFromPremultiplied(doc, premultiplied, std::move(name));
}

// Texels whose alpha is strictly between 0 and 1, and whether any texel is
// transparent at all -- §7's and §10's "this is preserved exactly where the
// result is opaque" measured rather than asserted.
size_t partiallyTransparentTexels(const std::vector<float>& premultiplied) noexcept {
  size_t n = 0;
  for (size_t i = 3; i < premultiplied.size(); i += 4) {
    const float a = premultiplied[i];
    if (a > 0.0f && a < 1.0f) ++n;
  }
  return n;
}

// --- Merge down, the RGB path (§4) ----------------------------------------

LayerOpResult mergeDownComposited(Document& doc, size_t index, std::vector<std::string>* warnings) {
  const size_t lower = index - 1;

  // §6. The sub-document is the pair alone, with the clip relationship the
  // pair has *between themselves* preserved and the one they have with layers
  // further down removed -- the latter is re-attached to the merged layer
  // afterwards.
  const bool lowerClipped = doc.layers[lower].clipped;
  Document sub = canvasOf(doc);
  sub.layers.push_back(doc.layers[lower]);  // shares tiles: TileStoreOf is COW
  sub.layers.push_back(doc.layers[index]);
  sub.layers[0].clipped = false;
  if (lowerClipped) sub.layers[1].clipped = false;

  std::vector<std::string> subWarnings;
  const std::vector<float> premultiplied = compositeDocumentPremultiplied(sub, &subWarnings);
  carryCompositeWarnings("merge down", "the pair on its own", subWarnings, warnings);
  warnOffCanvas(doc, {lower, index}, "merge down", warnings);
  warnBaked(doc, {lower, index}, "merge down", warnings);

  Layer merged = mergedLayer(doc, premultiplied, doc.layers[lower].name);
  merged.clipped = lowerClipped;
  merged.parent = doc.layers[lower].parent;

  const std::string label =
      "merge " + layerOpDescribe(doc, index) + " down into " + layerOpDescribe(doc, lower);
  doc.layers.erase(doc.layers.begin() + static_cast<std::ptrdiff_t>(index));
  doc.layers[lower] = std::move(merged);
  return layerOpSucceed(label, lower);
}

// --- Merge down, the latent path (§5) -------------------------------------
//
// Not a composite: a tile walk over the union of the two Pigment stores,
// producing the `(Lmix, mmix)` core/Composite's `mixedPairTexel()` computes
// internally. The two must stay in step, and the thing that keeps them in step
// is that both call `mixLatents()` with the same weight -- the upper layer's
// **mass** -- and union the masses the way `over` unions coverage.
LayerOpResult mergeDownLatent(Document& doc, size_t index, std::vector<std::string>* warnings) {
  const size_t lower = index - 1;
  const PigmentTileStore& lowStore = *doc.layers[lower].pigmentTiles;
  const PigmentTileStore& upStore = *doc.layers[index].pigmentTiles;

  std::unordered_set<TileCoord> coords;
  for (const auto& entry : lowStore) coords.insert(entry.first);
  for (const auto& entry : upStore) coords.insert(entry.first);

  Layer merged;
  merged.kind = LayerKind::Pigment;
  merged.pigmentTiles.emplace();
  merged.name = doc.layers[lower].name;
  // **`normal`, not the lower half's blend**, and that is a correctness point
  // rather than a default. core/Composite composites a mixed pair with `over`
  // unconditionally -- "the pair's own arithmetic *is* the mix, and what it
  // produces meets everything beneath it as ordinary coverage" -- so the lower
  // half's own blend name is a value the walk never reads. Carrying it forward
  // would hand it to a layer the walk *does* read it on, and change how the
  // merged layer meets the backdrop.
  merged.blend = kDefaultBlendName;
  merged.parent = doc.layers[lower].parent;
  if (doc.layers[lower].blend != std::string(kDefaultBlendName))
    append(warnings, "merge down dropped " + layerOpDescribe(doc, lower) + "'s blend \"" +
                         doc.layers[lower].blend +
                         "\": core/Composite composites a mixed pair with `over` regardless of "
                         "the lower half's blend, so the name was already having no effect and "
                         "keeping it would have given it one.");

  for (const TileCoord& coord : coords) {
    const PigmentTile* low = lowStore.find(coord);
    const PigmentTile* up = upStore.find(coord);
    PigmentTile& out = merged.pigmentTiles->getOrCreate(coord);
    for (int32_t y = 0; y < kTileSize; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const PixelCoord local{x, y};
        // An unallocated tile means an all-zero, mass-0 texel -- the same
        // reading core/Composite's mixed-pair walk gives it, quoted there as
        // "a texel where only one of them has a tile still mixes".
        const PigmentTexel a = low != nullptr ? low->readTexel(local) : PigmentTexel{};
        const PigmentTexel b = up != nullptr ? up->readTexel(local) : PigmentTexel{};
        PigmentTexel m;
        m.latent = mixLatents(a.latent, b.latent, b.mass);
        m.mass = b.mass + a.mass * (1.0f - b.mass);
        out.writeTexel(local, m);
      }
    }
  }

  // §3's asymmetry, said out loud at the one place it happens rather than left
  // for a reader to notice that the numbers differ between two merges.
  const size_t offCanvas =
      outsideCanvas(doc, &lowStore) + outsideCanvas(doc, &upStore);
  if (offCanvas > 0)
    append(warnings, "merge down (latent) kept " + std::to_string(offCanvas) +
                         " occupied tile(s) lying entirely outside the canvas: this path walks "
                         "tiles rather than the canvas-sized composite buffer, so unlike every "
                         "other merge it discards nothing (core/Merge.hpp §3).");
  append(warnings,
         "merge down merged two Pigment layers in latent space: the result is still a Pigment "
         "layer, still mixable (PRD C3) and still erasable as mass (PRD F10). The upper layer's "
         "`mix` blend is consumed by the merge and the merged layer is `" +
             std::string(kDefaultBlendName) + "`.");

  const std::string label = "merge " + layerOpDescribe(doc, index) + " down into " +
                            layerOpDescribe(doc, lower) + " (latent mix)";
  doc.layers.erase(doc.layers.begin() + static_cast<std::ptrdiff_t>(index));
  doc.layers[lower] = std::move(merged);
  return layerOpSucceed(label, lower);
}

// §5's conditions, each with its own sentence. Returns an empty string when
// the latent path is available. `doc.layers[index]` and `[index - 1]` are
// known to be Pigment layers with storage.
std::string latentPathObstacle(const Document& doc, size_t index) {
  const size_t lower = index - 1;
  const Layer& low = doc.layers[lower];
  const Layer& up = doc.layers[index];

  if (!blendIsMix(up))
    return "the upper layer's blend is \"" + up.blend +
           "\", not \"mix\". `over` between two Pigment layers is a glaze -- an upper film of "
           "paint sitting on a lower one -- and no single (latent, mass) texel is a glaze, so "
           "there is nothing to merge them into that would still be paint. Set " +
           layerOpDescribe(doc, index) +
           "'s blend to Mix to merge them as a mixture, which is exact, or rasterise the pair "
           "another way.";

  const MixPairing pairing = mixPairing(doc);
  if (index >= pairing.mixedWithBelow.size() || !pairing.mixedWithBelow[index])
    return layerOpDescribe(doc, index) + " carries \"mix\" but is not paired with " +
           layerOpDescribe(doc, lower) +
           ": core::mixPairing() pairs greedily from the bottom and PRD L5 refuses a pair whose "
           "halves are not both unclipped Pigment layers, so this layer is composited as `over` "
           "and warned about. There is no mix to merge.";

  if (index + 1 < doc.layers.size() && blendIsMix(doc.layers[index + 1]))
    return layerOpDescribe(doc, index + 1) +
           " directly above the pair also carries \"mix\". Merging the pair would offer it a new "
           "partner and change how it composites, which is the picture changing one layer above "
           "the merge. Clear that layer's blend first.";

  if (low.opacity != 1.0f || up.opacity != 1.0f)
    return "one of the two layers is not at opacity 1 (" + layerOpDescribe(doc, lower) +
           " is at " + std::to_string(low.opacity) + ", " + layerOpDescribe(doc, index) + " at " +
           std::to_string(up.opacity) +
           "). Opacity is transparency and fades the *projected* colour (core/Composite.hpp §3), "
           "so it cannot be folded into a latent without turning transparency into mass -- which "
           "would change the mixture's hue rather than let the backdrop through.";

  if (low.mask.has_value() || up.mask.has_value())
    return "one of the two layers has a mask. A mask multiplies coverage, never mass "
           "(core/Layer.hpp), so baking it into a latent merge would make it an eraser (PRD F10) "
           "instead of a mask. Remove the mask, or apply it another way, first.";

  if (low.ops.size() > 0 || up.ops.size() > 0)
    return "one of the two layers has a non-empty op stack. A layer's ops run *after* the latent "
           "-> RGB projection (core/Composite.hpp §1), so a graded pigment texel is not a pigment "
           "texel: baking a curve into c0..c2 would not be a grade of the colour, it would be a "
           "different pigment. Clear the stack first.";

  if (low.clipped || up.clipped)
    return "one of the two layers is clipped, and PRD L5 refuses a `Mix` pair whose halves are "
           "not both unclipped (core/Composite.hpp §15). Un-clip it first.";

  return {};
}

}  // namespace

// ==========================================================================
// The measured costs (§2, §3)
// ==========================================================================

size_t mergeCompositeBufferBytes(const Document& doc) noexcept {
  if (doc.width <= 0 || doc.height <= 0) return 0;
  return static_cast<size_t>(doc.width) * static_cast<size_t>(doc.height) * 4 * sizeof(float);
}

size_t offCanvasTileCount(const Document& doc, const Layer& layer) noexcept {
  size_t n = 0;
  n += outsideCanvas(doc, layer.rgbTiles.has_value() ? &*layer.rgbTiles : nullptr);
  n += outsideCanvas(doc, layer.pigmentTiles.has_value() ? &*layer.pigmentTiles : nullptr);
  n += outsideCanvas(doc, layer.mask.has_value() ? &*layer.mask : nullptr);
  return n;
}

// ==========================================================================
// The merged layer
// ==========================================================================

Layer layerFromPremultiplied(const Document& doc, const std::vector<float>& premultiplied,
                             std::string name) {
  Layer layer = makeRgbLayer(std::move(name));
  if (doc.width <= 0 || doc.height <= 0) return layer;
  const size_t need = static_cast<size_t>(doc.width) * static_cast<size_t>(doc.height) * 4;
  if (premultiplied.size() != need) return layer;

  const int32_t tilesX = (doc.width + kTileSize - 1) / kTileSize;
  const int32_t tilesY = (doc.height + kTileSize - 1) / kTileSize;
  for (int32_t tileY = 0; tileY < tilesY; ++tileY) {
    for (int32_t tileX = 0; tileX < tilesX; ++tileX) {
      const TileCoord coord{tileX, tileY};
      const PixelCoord origin = tileOrigin(coord);
      const int32_t spanX = std::min(kTileSize, doc.width - origin.x);
      const int32_t spanY = std::min(kTileSize, doc.height - origin.y);

      // PRD C2. The occupancy scan runs before `getOrCreate()`, so a tile the
      // composite left entirely transparent costs nothing at all -- not an
      // allocation later freed, not an allocation kept. A merge is the
      // operation most likely to reintroduce "memory tracks canvas size",
      // because it is handed a canvas-sized buffer and asked for a layer.
      bool occupied = false;
      for (int32_t y = 0; y < spanY && !occupied; ++y) {
        const size_t row =
            (static_cast<size_t>(origin.y + y) * static_cast<size_t>(doc.width) +
             static_cast<size_t>(origin.x)) *
            4;
        for (int32_t k = 0; k < spanX * 4; ++k) {
          if (premultiplied[row + static_cast<size_t>(k)] != 0.0f) {
            occupied = true;
            break;
          }
        }
      }
      if (!occupied) continue;

      Tile& tile = layer.rgbTiles->getOrCreate(coord);
      for (int32_t y = 0; y < spanY; ++y) {
        for (int32_t x = 0; x < spanX; ++x) {
          const size_t at = (static_cast<size_t>(origin.y + y) * static_cast<size_t>(doc.width) +
                             static_cast<size_t>(origin.x + x)) *
                            4;
          tile.writePixel(PixelCoord{x, y}, {premultiplied[at + 0], premultiplied[at + 1],
                                             premultiplied[at + 2], premultiplied[at + 3]});
        }
      }
    }
  }
  return layer;
}

// ==========================================================================
// Merge down (PRD C10)
// ==========================================================================

LayerOpResult mergeLayerDown(Document& doc, size_t index, std::vector<std::string>* warningsOut) {
  LayerOpResult refusal;
  if (!layerOpInRange(doc, index, "merge down", &refusal)) return refusal;
  if (index == 0)
    return layerOpFail(
        "merge down refused: " + layerOpDescribe(doc, 0) +
        " is the bottom layer -- there is nothing below it to merge into. Merge down "
        "folds a layer into the one beneath it, so it needs one.");
  const size_t lower = index - 1;
  if (!layerOpNotLocked(doc, index, "merge down", kLockedMergeDestroys, &refusal)) return refusal;
  if (!layerOpNotLocked(doc, lower, "merge down", kLockedMergeDestroys, &refusal)) return refusal;

  const Layer& up = doc.layers[index];
  const Layer& low = doc.layers[lower];

  // §4's two refusals that are not about arithmetic.
  if (!up.visible || !low.visible)
    return layerOpFail(
        "merge down refused: " + layerOpDescribe(doc, up.visible ? lower : index) +
        " is hidden. Merging a hidden layer discards its pixels and changes nothing on "
        "screen, which is the most silently destructive thing this operation could do. "
        "Show it first, or delete it. (A layer at opacity 0 is *not* refused -- opacity "
        "bakes exactly, visibility is a switch you expect to be able to flip back.)");

  if (up.parent != low.parent)
    return layerOpFail(
        "merge down refused: " + layerOpDescribe(doc, index) + " and " +
        layerOpDescribe(doc, lower) + " are in different groups (np:parent \"" + up.parent +
        "\" and \"" + low.parent +
        "\"). A merge across a group boundary has to decide which group the result joins, "
        "and this build creates no groups at all (core/Layer.hpp) -- so the two names "
        "came from a file and are not this build's to reinterpret.");

  if (index + 1 < doc.layers.size() && doc.layers[index + 1].clipped)
    return layerOpFail(
        "merge down refused: " + layerOpDescribe(doc, index + 1) +
        " directly above is clipped "
        "to " +
        layerOpDescribe(doc, index) + "'s alpha, and merging unions that alpha with " +
        layerOpDescribe(doc, lower) +
        "'s -- so the clipped layer would show through where it is currently cut away. "
        "Un-clip it first, or merge it down instead.");

  // §6's fourth arrangement.
  if (low.clipped && !up.clipped)
    return layerOpFail(
        "merge down refused: " + layerOpDescribe(doc, lower) + " is clipped and " +
        layerOpDescribe(doc, index) +
        " is not. The merged layer has to be one or the other, and either answer changes "
        "a picture -- clipped, and the upper layer's pixels are cut away by a base it "
        "never touched; unclipped, and the lower layer's escape one. Clip them alike, or "
        "un-clip both.");

  // Kinds. Each of the three refusals below names what the kind *is* rather
  // than that it is unsupported.
  const bool pigmentPair = up.kind == LayerKind::Pigment && low.kind == LayerKind::Pigment &&
                           up.pigmentTiles.has_value() && low.pigmentTiles.has_value();

  if (up.kind == LayerKind::Adjustment || low.kind == LayerKind::Adjustment)
    return layerOpFail(
        "merge down refused: " +
        layerOpDescribe(doc, up.kind == LayerKind::Adjustment ? index : lower) +
        " is an Adjustment layer, which holds no pixels -- it transforms what is beneath "
        "it (PRD C5). There is nothing to merge into it and nothing in it to merge. "
        "Layer > Rasterise Layer turns it into pixels first (PRD C11), and then this "
        "merge is an ordinary one.");

  if (!holdsPixels(up) || !holdsPixels(low))
    return layerOpFail(
        "merge down refused: " + layerOpDescribe(doc, holdsPixels(up) ? lower : index) +
        " is a " + layerKindName(holdsPixels(up) ? low.kind : up.kind) +
        " layer, which owns no pixel storage in this build (core/Layer.hpp: Vector holds "
        "shapes and Text holds a text block, neither of which is a tile store; Media needs "
        "the fluid solver's per-medium state; Strokes and Flats have no parameter member "
        "yet). A merge would produce an empty layer and call it a merge. Layer > Rasterise "
        "Layer turns a Vector or Text layer into pixels first (PRD C11), and then this "
        "merge is an ordinary one.");

  if (pigmentPair) {
    const std::string obstacle = latentPathObstacle(doc, index);
    if (obstacle.empty()) return mergeDownLatent(doc, index, warningsOut);
    return layerOpFail(
        "merge down refused: " + layerOpDescribe(doc, index) + " and " +
        layerOpDescribe(doc, lower) +
        " are both Pigment layers, and merging them through RGB would spend PRD C3's "
        "`Mix` (P0) and PRD F10's mass -- both of which live in the latents -- to satisfy "
        "PRD C10. The one merge that stays in latent space is a mixed pair, and " +
        obstacle);
  }

  if (up.kind == LayerKind::Pigment || low.kind == LayerKind::Pigment)
    return layerOpFail(
        "merge down refused: " +
        layerOpDescribe(doc, up.kind == LayerKind::Pigment ? index : lower) +
        " is a Pigment layer and " +
        layerOpDescribe(doc, up.kind == LayerKind::Pigment ? lower : index) +
        " is not. The merged layer would have to be RGB, which throws away the latents "
        "PRD C3's `Mix` (P0) and PRD F10's eraser both act on -- silently, and with no "
        "way back. Two Pigment layers with `Mix` merge in latent space and lose nothing; "
        "a mixed pair of kinds has no such answer.");

  // §4's equation. Both blends must be `over` for the merge to preserve the
  // picture, and this codebase refuses rather than changing it quietly.
  if (!blendIsNormal(up) || !blendIsNormal(low)) {
    const bool upperIsTheProblem = !blendIsNormal(up);
    const size_t which = upperIsTheProblem ? index : lower;
    return layerOpFail(
        "merge down refused: " + layerOpDescribe(doc, which) + " uses blend \"" +
        doc.layers[which].blend +
        "\". A blend combines its layer with everything beneath it, not with the one "
        "layer below -- so folding the pair into one layer changes the picture the "
        "moment anything at all sits under them, and no merged layer can reproduce it "
        "(core/Merge.hpp §4). Set the blend to Normal, or use Layer > Merge Visible or "
        "Flatten Image, which collapse the backdrop too and are exact for any blend.");
  }

  return mergeDownComposited(doc, index, warningsOut);
}

// ==========================================================================
// Merge visible / stamp visible / flatten (PRD C10)
// ==========================================================================

LayerOpResult mergeVisibleLayers(Document& doc, std::vector<std::string>* warningsOut) {
  std::vector<size_t> visible;
  for (size_t i = 0; i < doc.layers.size(); ++i)
    if (doc.layers[i].visible) visible.push_back(i);

  if (visible.size() < 2)
    return layerOpFail(
        "merge visible refused: this document has " + std::to_string(visible.size()) +
        " visible layer(s) of " + std::to_string(doc.layers.size()) +
        ", and a merge needs two. One visible layer collapsed on its own is a bake of its "
        "mask, opacity and op stack rather than a merge; Layer > Stamp Visible makes the "
        "same pixels as a new layer and keeps the original.");

  for (const size_t i : visible) {
    LayerOpResult refusal;
    if (!layerOpNotLocked(doc, i, "merge visible", kLockedMergeDestroys, &refusal))
      return refusal;
  }
  for (const size_t i : visible)
    if (doc.layers[i].parent != doc.layers[visible.front()].parent)
      return layerOpFail(
          "merge visible refused: the visible layers span more than one group (" +
          layerOpDescribe(doc, visible.front()) + " has np:parent \"" +
          doc.layers[visible.front()].parent + "\", " + layerOpDescribe(doc, i) + " has \"" +
          doc.layers[i].parent +
          "\"). This build creates no groups (core/Layer.hpp), so those names came from a "
          "file and are not this build's to collapse.");

  std::vector<std::string> subWarnings;
  const std::vector<float> premultiplied = compositeDocumentPremultiplied(doc, &subWarnings);
  carryCompositeWarnings("merge visible", "the whole document", subWarnings, warningsOut);
  warnOffCanvas(doc, visible, "merge visible", warningsOut);
  warnBaked(doc, visible, "merge visible", warningsOut);
  warnPigmentLost(doc, visible, "merge visible", warningsOut);

  const size_t at = visible.front();
  Layer merged = mergedLayer(doc, premultiplied, "Merged");
  merged.parent = doc.layers[at].parent;

  const size_t hidden = doc.layers.size() - visible.size();
  if (hidden > 0)
    append(warningsOut, "merge visible left " + std::to_string(hidden) +
                            " hidden layer(s) in place. They contribute nothing to a composite, so "
                            "the picture is unchanged; Flatten Image is the operation that "
                            "discards them.");

  // Erased from the top down so every remaining index stays valid.
  for (size_t k = visible.size(); k-- > 0;)
    doc.layers.erase(doc.layers.begin() + static_cast<std::ptrdiff_t>(visible[k]));
  doc.layers.insert(doc.layers.begin() + static_cast<std::ptrdiff_t>(at), std::move(merged));

  return layerOpSucceed("merge " + std::to_string(visible.size()) + " visible layers", at);
}

LayerOpResult stampVisibleLayers(Document& doc, std::vector<std::string>* warningsOut) {
  size_t visible = 0;
  for (const Layer& layer : doc.layers)
    if (layer.visible) ++visible;
  if (visible == 0)
    return layerOpFail(
        "stamp visible refused: this document has " + std::to_string(doc.layers.size()) +
        " layer(s) and none of them is visible, so the stamp would be an empty layer.");

  std::vector<std::string> subWarnings;
  const std::vector<float> premultiplied = compositeDocumentPremultiplied(doc, &subWarnings);
  carryCompositeWarnings("stamp visible", "the whole document", subWarnings, warningsOut);

  // §7. The stamp sits *over* the layers it was made from, so it is not
  // appearance-preserving where the composite has partial alpha -- and the
  // number of texels where that is true is measured rather than described.
  const size_t partial = partiallyTransparentTexels(premultiplied);
  if (partial > 0)
    append(warningsOut,
           "stamp visible added a layer over the ones it was made from, and the composite has " +
               std::to_string(partial) +
               " texel(s) with alpha strictly between 0 and 1, where the picture will now build "
               "up against itself. The stamped layer *alone* is exactly what the document showed "
               "-- hide the layers beneath it to see that (core/Merge.hpp §7).");

  Layer stamp = mergedLayer(doc, premultiplied, "Stamp");
  const size_t at = doc.layers.size();
  doc.layers.push_back(std::move(stamp));
  return layerOpSucceed("stamp " + std::to_string(visible) + " visible layers to a new layer",
                        at);
}

LayerOpResult flattenDocument(Document& doc, std::vector<std::string>* warningsOut) {
  if (doc.layers.empty())
    return layerOpFail(
        "flatten image refused: this document has no layers. Flatten collapses a stack to "
        "one layer and there is no stack.");
  for (size_t i = 0; i < doc.layers.size(); ++i) {
    LayerOpResult refusal;
    if (!layerOpNotLocked(doc, i, "flatten image", kLockedMergeDestroys, &refusal))
      return refusal;
  }

  // Only the visible layers are *baked* into the result; the hidden ones are
  // discarded outright and get their own sentence below, so counting their
  // masks and op stacks among the baked ones would be a true number about the
  // wrong thing.
  std::vector<size_t> visible;
  for (size_t i = 0; i < doc.layers.size(); ++i)
    if (doc.layers[i].visible) visible.push_back(i);

  std::vector<std::string> subWarnings;
  const std::vector<float> premultiplied = compositeDocumentPremultiplied(doc, &subWarnings);
  carryCompositeWarnings("flatten image", "the whole document", subWarnings, warningsOut);
  warnOffCanvas(doc, visible, "flatten image", warningsOut);
  warnBaked(doc, visible, "flatten image", warningsOut);
  warnPigmentLost(doc, visible, "flatten image", warningsOut);

  const size_t hidden = doc.layers.size() - visible.size();
  if (hidden > 0)
    append(warningsOut, "flatten image discarded " + std::to_string(hidden) +
                            " hidden layer(s) -- that is the whole of what makes it different "
                            "from Merge Visible, which leaves them alone.");

  // §9. Alpha survives, because PRD C16 leaves this build with no background
  // to composite in and inventing one here would reinstate exactly the
  // privileged Background C16 removed.
  const size_t partial = partiallyTransparentTexels(premultiplied);
  append(warningsOut, "flatten image preserved alpha: " + std::to_string(partial) +
                          " texel(s) are still partially transparent, and fully transparent "
                          "regions are still transparent. PRD C16 leaves this build no background "
                          "layer to composite in, and io/Export decides opacity at the encoder "
                          "instead (core/Merge.hpp §9).");

  const size_t count = doc.layers.size();
  Layer flat = mergedLayer(doc, premultiplied, "Flattened");
  doc.layers.clear();
  doc.layers.push_back(std::move(flat));
  return layerOpSucceed("flatten " + std::to_string(count) + " layers to one", 0);
}

// ==========================================================================
// Rasterise a parametric layer (PRD C11)
// ==========================================================================

LayerOpResult rasteriseLayer(Document& doc, size_t index, std::vector<std::string>* warningsOut) {
  LayerOpResult refusal;
  if (!layerOpInRange(doc, index, "rasterise layer", &refusal)) return refusal;
  if (!layerOpNotLocked(doc, index, "rasterise layer", kLockedMergeDestroys, &refusal))
    return refusal;

  const Layer& layer = doc.layers[index];

  // **Vector and Text rasterise WITHOUT looking at the composite below**, and
  // that is the whole difference from the Adjustment case that occupies the
  // rest of this function. An Adjustment layer holds no pixels because it *is*
  // a transformation of what is beneath it, so the only meaning "rasterise" can
  // have there is "evaluate against the stack below and become that" -- with
  // all of that path's consequences: the layers below stay, the result is only
  // exact where they are opaque, and a clipped adjustment must not be cut
  // twice. A Vector or Text layer holds no pixels for an entirely different
  // reason: its content is parametric and self-contained. Its rasterised form
  // depends on nothing outside itself, so this is a straight substitution of
  // the tiles core/VectorRaster would have handed the compositor anyway, with
  // no warning to issue and nothing beneath it to be inexact about.
  //
  // The mask, opacity, blend, visibility, clip flag and group tag all carry
  // over untouched, because every one of them applied to the layer before and
  // must apply to it after -- rasterising changes what the layer's content IS,
  // not how it participates in the stack.
  if (layerRastersToTiles(layer.kind)) {
    const std::vector<VectorShape> shapes = layer.kind == LayerKind::Text
                                                ? textContentToShapes(layer.text)
                                                : layer.shapes;
    const size_t geometryCount = shapes.size();
    Layer raster = layer;
    raster.kind = LayerKind::RGB;
    raster.rgbTiles = rasterizeVectorLayer(shapes, doc.width, doc.height);
    raster.shapes.clear();
    raster.nextShapeId = 1;
    raster.text = TextContent{};
    append(warningsOut,
           "rasterise layer baked " + std::to_string(geometryCount) +
               " shape(s) into pixels; the layer was resolution-independent before and is "
               "not now, and its geometry is gone. Undo restores it.");
    const std::string vectorLabel = "rasterise " + layerOpDescribe(doc, index);
    doc.layers[index] = std::move(raster);
    return layerOpSucceed(vectorLabel, index);
  }

  if (layer.kind != LayerKind::Adjustment)
    return layerOpFail(
        "rasterise layer refused: " + layerOpDescribe(doc, index) + " is a " +
        layerKindName(layer.kind) +
        " layer. PRD C11 rasterises a *parametric* layer, and of the kinds this build "
        "has, Adjustment, Vector and Text are the three that qualify -- a Strokes layer "
        "here has no dabs and a Flats layer no regions, because neither has a parameter "
        "member yet, and Media needs the fluid solver's per-medium state (core/Layer.hpp). "
        "An RGB or Pigment layer is already pixels and has nothing to rasterise.");

  if (layer.ops.size() == 0)
    return layerOpFail(
        "rasterise layer refused: " + layerOpDescribe(doc, index) +
        " has an empty op stack, which core/Composite treats as an exact no-op -- so "
        "rasterising it would produce a copy of the composite beneath it under a "
        "misleading name. Layer > Stamp Visible is that operation. Add and enable an op "
        "first.");

  if (index == 0)
    return layerOpFail(
        "rasterise layer refused: " + layerOpDescribe(doc, 0) +
        " is an Adjustment layer at the bottom of the stack, so there is nothing beneath "
        "it to evaluate against -- an adjustment layer transforms the composite below it "
        "(PRD C5), and below this one there is none. Move it up first.");

  // §10. The adjustment layer *included*, so the mask, the clip, the coverage
  // and `adjustedPremultiplied()` are all the compositor's own -- there is no
  // second evaluation of an adjustment layer anywhere in this file.
  Document sub = canvasOf(doc);
  for (size_t i = 0; i <= index; ++i) sub.layers.push_back(doc.layers[i]);

  std::vector<std::string> subWarnings;
  const std::vector<float> premultiplied = compositeDocumentPremultiplied(sub, &subWarnings);
  carryCompositeWarnings("rasterise layer", "the adjustment layer and everything beneath it",
                         subWarnings, warningsOut);

  // An adjustment layer never touches alpha (core/Composite.hpp: "out.a =
  // below.a, never touched"), so this buffer's alpha *is* the alpha of the
  // composite below -- one buffer answers both questions, and the count below
  // is exactly the set of texels where the rasterised layer sitting over the
  // layers it was computed from will not reproduce them.
  const size_t partial = partiallyTransparentTexels(premultiplied);
  if (partial > 0)
    append(warningsOut,
           "rasterise layer left the " + std::to_string(index) +
               " layer(s) beneath in place, and the composite beneath has " +
               std::to_string(partial) +
               " texel(s) with alpha strictly between 0 and 1 -- the rasterised layer now sits "
               "over them and builds up against itself there. It is exact wherever that "
               "composite is opaque (core/Merge.hpp §10).");
  append(warningsOut, "rasterise layer baked " + std::to_string(layer.ops.size()) +
                          " op(s) into pixels; the grade was non-destructive before and is not "
                          "now.");

  Layer raster = mergedLayer(doc, premultiplied, layer.name);
  // **Unclipped, even when the adjustment layer was clipped**, and that is the
  // one place this operation could quietly double-apply something. The buffer
  // above is the composite of `[0 .. index]` *with* the clip already folded in
  // by core/Composite's own group bracket; a rasterised layer that also
  // carried `clipped` would be cut a second time by the base's alpha, and it
  // holds the base's pixels as well as the adjustment's effect, so the second
  // cut would remove content that never belonged to the clipped layer.
  raster.clipped = false;
  raster.parent = layer.parent;
  const std::string label = "rasterise " + layerOpDescribe(doc, index);
  doc.layers[index] = std::move(raster);
  return layerOpSucceed(label, index);
}

}  // namespace np
