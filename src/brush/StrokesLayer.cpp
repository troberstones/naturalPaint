#include "brush/StrokesLayer.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "brush/Deposit.hpp"
#include "brush/Heal.hpp"
#include "core/Composite.hpp"
#include "core/Tile.hpp"
#include "flats/FlatsLayer.hpp"
#include "ops/Poisson.hpp"

namespace np {
namespace {

// Section 2. 256 rather than a smaller number because a checkpoint costs a
// `TileStore` slot map plus whatever the replay after it copy-on-writes, and
// a stroke is on the order of a few hundred dabs -- so this is roughly "one
// checkpoint per stroke", which is the granularity an undo step works at
// anyway. Exposed through `strokesCheckpointInterval()` so `--selftest` sizes
// its fixture against the implementation instead of against a copy of it.
constexpr size_t kCheckpointInterval = 256;

struct Checkpoint {
  size_t dabs = 0;
  uint64_t prefixHash = 0;
  TileStore tiles;
};

struct Entry {
  uint64_t contentHash = 0;
  // Zero when the layer holds no `DabColorSource::Below` dab -- section 1's "pays
  // nothing at all". A layer that GAINS one gets a non-zero signature and
  // therefore a miss, which is correct.
  uint64_t belowSig = 0;
  int width = 0;
  int height = 0;
  std::shared_ptr<const TileStore> tiles;
  std::vector<Checkpoint> checkpoints;
  size_t lastReplayed = 0;
};

// One entry per layer id, never a history of them -- flats/FlatsLayer's rule,
// for its reason: a layer only ever needs its current evaluation.
std::unordered_map<uint64_t, Entry>& cache() {
  static std::unordered_map<uint64_t, Entry> c;
  return c;
}

// **Both below-sampling policies, and the `!= Ink` spelling is deliberate.**
// This predicate is what decides whether `strokesSourceComposite()` runs at
// all, so a policy missing from it does not draw a wrong colour -- it draws
// NOTHING, because `applyDab()` refuses a below-sampling dab with no
// composite to sample (a hole, which is what section 1 says a wrong answer
// here looks like). Written as "anything that is not its own ink" so that a
// fourth policy is included by default and has to be excluded on purpose,
// which is the safe direction for a predicate whose omission is silent.
bool contentSamplesBelow(const StrokesContent& content) noexcept {
  for (const DabRecord& d : content.dabs)
    if (d.source != DabColorSource::Ink) return true;
  return false;
}

// The stored shape fields, as the `BrushTip` `dabCoverage()` takes. The whole
// bridge between the two dab types, and deliberately four lines long: a
// stored dab carries no bitmap and no dual tip (core/StrokesContent §1), so
// those members keep their null defaults and `dabCoverage()` takes its
// procedural path.
BrushTip tipOf(const DabRecord& d) noexcept {
  BrushTip tip;
  tip.radius = d.radius;
  tip.hardness = d.hardness;
  tip.roundness = d.roundness;
  tip.angle = d.angle;
  return tip;
}

// Source-over of a premultiplied `src` onto a premultiplied `dst`. The
// compositor's own arithmetic, spelled here rather than reached for because
// core/Composite's is a whole-layer walk over tiles and this is one texel.
inline void over(std::array<float, 4>& dst, const std::array<float, 4>& src) noexcept {
  const float inv = 1.0f - src[3];
  dst[0] = src[0] + dst[0] * inv;
  dst[1] = src[1] + dst[1] * inv;
  dst[2] = src[2] + dst[2] * inv;
  dst[3] = src[3] + dst[3] * inv;
}

// One dab, composited into `out`. The single per-dab step BOTH the pure
// `strokesRasterize()` and the cached, checkpointed path go through -- the
// header's promise that the two cannot disagree about what a dab looks like
// is this function having one definition.
void applyDab(TileStore& out, const DabRecord& d, int width, int height,
              const std::vector<float>& below) {
  const DabBounds b = dabRecordBounds(d);
  if (b.empty()) return;
  const float flow = std::clamp(d.flow, 0.0f, 1.0f);
  if (flow <= 0.0f) return;
  const bool fromBelow = d.source != DabColorSource::Ink;
  const bool healed = d.source == DabColorSource::BelowHealed;
  // A `Below` dab with no composite beneath it contributes nothing rather
  // than black -- the header says why, and the alternative would make a
  // Strokes layer at the bottom of the stack paint opaque holes.
  const size_t need = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
  if (fromBelow && below.size() < need) return;
  const int x0 = std::max(b.x0, 0), y0 = std::max(b.y0, 0);
  const int x1 = std::min(b.x1, width), y1 = std::min(b.y1, height);
  if (x1 <= x0 || y1 <= y0) return;
  const BrushTip tip = tipOf(d);

  // Section 1b: a recorded HEAL solves its patch here, once, before its first
  // texel is written -- the same ordering brush/Heal §2 requires of the live
  // tool, and for the same reason (one dab's answer must not depend on the
  // order its texels are visited in). The patch is the dab's own box grown by
  // one texel of Dirichlet skin and clipped to the canvas, exactly as
  // `HealStroke::healDab()` grows it, so every texel the dab can write is an
  // interior texel with a real correction.
  std::vector<std::array<float, 4>> patch;
  int px0 = 0, py0 = 0, pw = 0;
  if (healed) {
    px0 = std::max(b.x0 - 1, 0);
    py0 = std::max(b.y0 - 1, 0);
    const int px1 = std::min(b.x1 + 1, width);
    const int py1 = std::min(b.y1 + 1, height);
    pw = px1 - px0;
    const int ph = py1 - py0;
    if (pw <= 0 || ph <= 0) return;
    const size_t n = static_cast<size_t>(pw) * static_cast<size_t>(ph);
    std::vector<std::array<float, 4>> src(n, std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    std::vector<std::array<float, 4>> dst(n, std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
    for (int y = py0; y < py1; ++y) {
      for (int x = px0; x < px1; ++x) {
        const size_t i = static_cast<size_t>(y - py0) * static_cast<size_t>(pw) +
                         static_cast<size_t>(x - px0);
        const size_t di = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                           static_cast<size_t>(x)) * 4;
        // **The DESTINATION is what lies beneath this layer at the dab's own
        // place**, and section 1b argues the call. The live tool reads its
        // boundary out of the layer it is writing; this evaluation has no such
        // layer to read -- the marks it is producing ARE that layer -- and
        // reading its own partial output back would be section 1's feedback
        // loop with the rim doing the reading.
        dst[i] = {below[di], below[di + 1], below[di + 2], below[di + 3]};
        // The SOURCE, at the record's own offset. Nearest, out of canvas as
        // four zeros rather than clamped to the edge -- brush/Heal.cpp's rule
        // for the identical read, and for its reason: a clamp smears the
        // border row across everything sampled past it and looks like a
        // working heal.
        const int sx = static_cast<int>(std::lround(static_cast<float>(x) + d.sourceDx));
        const int sy = static_cast<int>(std::lround(static_cast<float>(y) + d.sourceDy));
        if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
        const size_t si = (static_cast<size_t>(sy) * static_cast<size_t>(width) +
                           static_cast<size_t>(sx)) * 4;
        src[i] = {below[si], below[si + 1], below[si + 2], below[si + 3]};
      }
    }
    patch = healPatch(src, dst, pw, ph);
  }
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      // Texel CENTRES, the convention brush/Deposit's own scan uses; sampling
      // at the corner would shift every stored dab half a texel up and left
      // relative to the same dab painted live.
      const float cov = dabCoverage(tip, static_cast<float>(x) + 0.5f - d.x,
                                    static_cast<float>(y) + 0.5f - d.y);
      if (cov <= 0.0f) continue;
      const float a = cov * flow;
      std::array<float, 4> src{};
      if (healed) {
        // The solved patch, clamped to what a premultiplied texel may legally
        // hold -- `healClampTexel()` by call and not by copy, because that
        // clamp is a property of core/Tile's storage and is stated once
        // (brush/Heal's own header section on it).
        const size_t pi = static_cast<size_t>(y - py0) * static_cast<size_t>(pw) +
                          static_cast<size_t>(x - px0);
        const std::array<float, 4> ink = healClampTexel(patch[pi]);
        src = {ink[0] * a, ink[1] * a, ink[2] * a, ink[3] * a};
      } else if (fromBelow) {
        // Nearest texel of the composite beneath, at the dab's own offset --
        // section 1. Nearest rather than bilinear because the offset a
        // recorded clone carries is a whole-texel drag in every gesture that
        // produces one, so interpolation would blur a copy for no gain.
        const int sx = static_cast<int>(std::lround(static_cast<float>(x) + d.sourceDx));
        const int sy = static_cast<int>(std::lround(static_cast<float>(y) + d.sourceDy));
        if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
        const size_t i = (static_cast<size_t>(sy) * width + sx) * 4;
        // `below` is already premultiplied, so scaling all four channels by
        // `a` is the whole of "this dab lays down that much of it".
        src = {below[i] * a, below[i + 1] * a, below[i + 2] * a, below[i + 3] * a};
      } else {
        // `rgba` is STRAIGHT (core/StrokesContent §3); premultiply here,
        // which is where every other producer in this build premultiplies.
        const float sa = std::clamp(d.rgba[3], 0.0f, 1.0f) * a;
        src = {d.rgba[0] * sa, d.rgba[1] * sa, d.rgba[2] * sa, sa};
      }
      if (src[3] <= 0.0f) continue;
      Tile& tile = out.getOrCreate(tileCoordAt(PixelCoord{x, y}));
      const PixelCoord local = tileLocalOffset(PixelCoord{x, y});
      std::array<float, 4> dst = tile.readPixel(local);
      over(dst, src);
      tile.writePixel(local, dst);
    }
  }
}

}  // namespace

bool strokesLayerEvaluable(const Document& doc, size_t index) noexcept {
  return index < doc.layers.size() && doc.layers[index].kind == LayerKind::Strokes &&
         doc.width > 0 && doc.height > 0;
}

std::vector<size_t> strokesSourceLayers(const Document& doc, size_t index) {
  const size_t below = std::min(index, doc.layers.size());
  std::vector<size_t> out(below);
  for (size_t i = 0; i < below; ++i) out[i] = i;
  return out;
}

std::vector<float> strokesSourceComposite(const Document& doc, size_t index) {
  const size_t below = std::min(index, doc.layers.size());
  if (below == 0 || doc.width <= 0 || doc.height <= 0) return {};
  // A shallow copy: `TileStore` shares tiles, so this costs the slot maps and
  // nothing else -- flats/FlatsLayer's own note on the same move.
  //
  // A contiguous PREFIX, which is what makes the subsetting this simple:
  // flats/FlatsLayer has to repair clips and group parents because its source
  // set can be an arbitrary subset, and a clip whose base was dropped would
  // clip to nothing. A prefix drops nothing that a layer inside it points
  // DOWN at, and `clipped`/`parent` only ever point down.
  Document sub = doc;
  sub.layers.resize(below);
  // Comps name layers by part name and nothing here reads them; carrying a
  // comp list that references dropped layers into a composite would be
  // harmless and confusing.
  sub.comps.clear();
  return compositeDocumentPremultiplied(sub);
}

TileStore strokesRasterize(const StrokesContent& content, int width, int height,
                           const std::vector<float>& below) {
  TileStore out;
  if (width <= 0 || height <= 0) return out;
  for (const DabRecord& d : content.dabs) applyDab(out, d, width, height, below);
  return out;
}

std::shared_ptr<const TileStore> strokesLayerTiles(const Document& doc, size_t index) {
  if (!strokesLayerEvaluable(doc, index)) return nullptr;
  const Layer& layer = doc.layers[index];
  const StrokesContent& content = layer.strokes;
  const uint64_t id = layer.id;
  const uint64_t hash = strokesContentHash(content);
  const bool samplesBelow = contentSamplesBelow(content);
  // Section 1: a layer of ordinary recorded paint never pays for the
  // signature, which walks every tile slot of every layer beneath it.
  const uint64_t belowSig =
      samplesBelow ? flatsSourceSignature(doc, strokesSourceLayers(doc, index)) : 0;

  // Read the entry OUT of the map before anything that could recurse.
  // `strokesSourceComposite()` composites the layers beneath, and one of
  // those may itself be a Strokes layer that re-enters this function -- which
  // may rehash the map and invalidate any reference held across the call.
  // The recursion terminates because the sub-document is a strict prefix.
  Entry entry;
  {
    auto& c = cache();
    const auto it = c.find(id);
    if (it != c.end()) entry = it->second;
  }
  if (entry.tiles && entry.contentHash == hash && entry.belowSig == belowSig &&
      entry.width == doc.width && entry.height == doc.height)
    return entry.tiles;

  // A canvas resize, or a below-composite that moved under a layer that reads
  // it, invalidates every checkpoint: those tiles were built over pixels that
  // are no longer the pixels underneath.
  if (entry.width != doc.width || entry.height != doc.height || entry.belowSig != belowSig)
    entry.checkpoints.clear();

  // Section 2: the LAST checkpoint whose prefix is still that prefix. Walked
  // from the newest so the common case -- an append, where every checkpoint
  // is still valid -- stops on the first test.
  size_t start = 0;
  TileStore tiles;
  while (!entry.checkpoints.empty()) {
    const Checkpoint& cp = entry.checkpoints.back();
    if (cp.dabs <= content.dabs.size() &&
        cp.prefixHash == strokesPrefixHash(content, cp.dabs)) {
      start = cp.dabs;
      tiles = cp.tiles;  // shares tiles; copies the slot map only
      break;
    }
    entry.checkpoints.pop_back();
  }

  std::vector<float> below;
  if (samplesBelow) below = strokesSourceComposite(doc, index);

  for (size_t i = start; i < content.dabs.size(); ++i) {
    applyDab(tiles, content.dabs[i], doc.width, doc.height, below);
    const size_t done = i + 1;
    if (done % kCheckpointInterval == 0 &&
        (entry.checkpoints.empty() || entry.checkpoints.back().dabs < done)) {
      Checkpoint cp;
      cp.dabs = done;
      cp.prefixHash = strokesPrefixHash(content, done);
      cp.tiles = tiles;
      entry.checkpoints.push_back(std::move(cp));
    }
  }

  entry.contentHash = hash;
  entry.belowSig = belowSig;
  entry.width = doc.width;
  entry.height = doc.height;
  entry.lastReplayed = content.dabs.size() - start;
  entry.tiles = std::make_shared<const TileStore>(std::move(tiles));
  std::shared_ptr<const TileStore> result = entry.tiles;
  cache()[id] = std::move(entry);
  return result;
}

void strokesForgetLayersNotIn(const Document& doc) {
  auto& c = cache();
  for (auto it = c.begin(); it != c.end();) {
    bool present = false;
    for (const Layer& l : doc.layers)
      if (l.id == it->first) {
        present = true;
        break;
      }
    it = present ? std::next(it) : c.erase(it);
  }
}

void strokesForgetAll() { cache().clear(); }

size_t strokesCacheEntryCount() noexcept { return cache().size(); }

size_t strokesCheckpointCount(uint64_t layerId) noexcept {
  const auto it = cache().find(layerId);
  return it == cache().end() ? 0 : it->second.checkpoints.size();
}

size_t strokesLastReplayedDabs(uint64_t layerId) noexcept {
  const auto it = cache().find(layerId);
  return it == cache().end() ? 0 : it->second.lastReplayed;
}

size_t strokesCheckpointInterval() noexcept { return kCheckpointInterval; }

}  // namespace np
