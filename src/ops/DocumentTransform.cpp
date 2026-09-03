#include "ops/DocumentTransform.hpp"

#include "core/CanvasLimits.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/Half.hpp"
#include "core/Tile.hpp"

namespace np {
namespace {

// --------------------------------------------------------------------------
// Region arithmetic
// --------------------------------------------------------------------------

// Accumulates occupied texels into a half-open region. A separate `any` flag
// rather than a degenerate rectangle, for `LayerBounds::empty`'s reason: "this
// store has no content" must not be confusable with "this store occupies one
// texel at the origin".
struct RegionBuilder {
  bool any = false;
  int32_t minX = 0, minY = 0, maxX = 0, maxY = 0;

  void add(int32_t x, int32_t y) noexcept {
    if (!any) {
      any = true;
      minX = maxX = x;
      minY = maxY = y;
      return;
    }
    minX = std::min(minX, x);
    maxX = std::max(maxX, x);
    minY = std::min(minY, y);
    maxY = std::max(maxY, y);
  }

  DocumentRegion finish() const noexcept {
    if (!any) return DocumentRegion{};
    DocumentRegion r;
    r.x = minX;
    r.y = minY;
    r.width = static_cast<uint32_t>(maxX - minX + 1);
    r.height = static_cast<uint32_t>(maxY - minY + 1);
    return r;
  }
};

// One scan for all four content rules. The rule itself is the predicate, so the
// four callers below differ by exactly the thing that genuinely differs between
// them (which word, and what makes it content) and share the walk.
//
// `pred` is handed the **stored word** -- `uint16_t` for the three half stores,
// `uint8_t` for the selection -- never a decoded float, so a denormal or a NaN
// a file may carry counts as content. core/LayerGeometry.cpp's reasoning,
// applied to one more store type.
template <class T, class Pred>
DocumentRegion scanStoreRegion(const TileStoreOf<T>& store, int32_t channels, int32_t channel,
                               Pred pred) {
  RegionBuilder b;
  for (const auto& [coord, tile] : store) {
    const PixelCoord origin = tileOrigin(coord);
    const auto* words = tile.data();
    for (int32_t ly = 0; ly < kTileSize; ++ly) {
      for (int32_t lx = 0; lx < kTileSize; ++lx) {
        const size_t at =
            (static_cast<size_t>(ly) * static_cast<size_t>(kTileSize) + static_cast<size_t>(lx)) *
                static_cast<size_t>(channels) +
            static_cast<size_t>(channel);
        if (pred(words[at])) b.add(origin.x + lx, origin.y + ly);
      }
    }
  }
  return b.finish();
}

// A half word is content unless it is +0 or -0 -- the bit test, so an infinity
// or a NaN is not silently dropped by a float comparison.
constexpr bool halfWordIsNonZero(uint16_t w) noexcept { return (w & 0x7fffu) != 0u; }

// The half-open tile range covering a region. Callers have already checked that
// the region is non-empty.
struct TileRange {
  int32_t tx0, ty0, tx1, ty1;  // inclusive
};
TileRange tileRangeOf(const DocumentRegion& r) noexcept {
  const int32_t x1 = r.x + static_cast<int32_t>(r.width);
  const int32_t y1 = r.y + static_cast<int32_t>(r.height);
  return TileRange{floorDiv(r.x, kTileSize), floorDiv(r.y, kTileSize), floorDiv(x1 - 1, kTileSize),
                   floorDiv(y1 - 1, kTileSize)};
}

// --------------------------------------------------------------------------
// The generic store <-> flat-image walk
//
// Both directions are tile-first, so each tile is looked up or created once
// rather than once per texel -- `ops/Transform.cpp`'s own note: "a per-texel
// find() on a 128x128 region is 16 384 hash lookups for one tile's worth of
// pixels".
// --------------------------------------------------------------------------

// Reads every texel of `store` that falls inside `r`, calling
// `read(tile, localCoord, imageTexelIndex)`. Absent tiles are skipped entirely,
// so the caller's pre-filled buffer decides what "absent" means -- which is the
// whole of section 3's mask-versus-selection difference, expressed as a choice
// of fill rather than as a branch in here.
template <class T, class Fn>
void readRegionFromStore(const TileStoreOf<T>& store, const DocumentRegion& r, const Fn& read) {
  if (r.empty()) return;
  const TileRange tr = tileRangeOf(r);
  const int32_t x1 = r.x + static_cast<int32_t>(r.width);
  const int32_t y1 = r.y + static_cast<int32_t>(r.height);
  for (int32_t ty = tr.ty0; ty <= tr.ty1; ++ty) {
    for (int32_t tx = tr.tx0; tx <= tr.tx1; ++tx) {
      const T* tile = store.find(TileCoord{tx, ty});
      if (tile == nullptr) continue;
      const PixelCoord org = tileOrigin(TileCoord{tx, ty});
      const int32_t bx0 = std::max(r.x, org.x);
      const int32_t by0 = std::max(r.y, org.y);
      const int32_t bx1 = std::min(x1, org.x + kTileSize);
      const int32_t by1 = std::min(y1, org.y + kTileSize);
      for (int32_t py = by0; py < by1; ++py) {
        for (int32_t px = bx0; px < bx1; ++px) {
          const size_t index = static_cast<size_t>(py - r.y) * static_cast<size_t>(r.width) +
                               static_cast<size_t>(px - r.x);
          read(*tile, PixelCoord{px - org.x, py - org.y}, index);
        }
      }
    }
  }
}

// Builds a fresh store covering `r`, calling `write(tile, localCoord,
// imageTexelIndex)` for every texel inside it and leaving every texel outside
// at the tile type's own default.
//
// **A tile that came out equal to a default-constructed one is not kept.** The
// comparison is `memcmp` over `sizeof(T)`, which is exact for all four tile
// types because each of them static_asserts that it is nothing but its texel
// buffer. That is what makes the drop rule right for every store at once
// despite the four defaults being different values -- all-transparent,
// mass-zero, all-reveal and all-unselected are each indistinguishable from
// absent *in their own store*, and `memcmp` against that store's own default
// asks exactly that question. PRD C2.
template <class T, class Fn>
void buildStoreForRegion(const DocumentRegion& r, const Fn& write, TileStoreOf<T>* out) {
  *out = TileStoreOf<T>{};
  if (r.empty()) return;
  const TileRange tr = tileRangeOf(r);
  const int32_t x1 = r.x + static_cast<int32_t>(r.width);
  const int32_t y1 = r.y + static_cast<int32_t>(r.height);
  const T defaultTile{};
  T scratch;
  for (int32_t ty = tr.ty0; ty <= tr.ty1; ++ty) {
    for (int32_t tx = tr.tx0; tx <= tr.tx1; ++tx) {
      scratch = defaultTile;
      const PixelCoord org = tileOrigin(TileCoord{tx, ty});
      const int32_t bx0 = std::max(r.x, org.x);
      const int32_t by0 = std::max(r.y, org.y);
      const int32_t bx1 = std::min(x1, org.x + kTileSize);
      const int32_t by1 = std::min(y1, org.y + kTileSize);
      for (int32_t py = by0; py < by1; ++py) {
        for (int32_t px = bx0; px < bx1; ++px) {
          const size_t index = static_cast<size_t>(py - r.y) * static_cast<size_t>(r.width) +
                               static_cast<size_t>(px - r.x);
          write(scratch, PixelCoord{px - org.x, py - org.y}, index);
        }
      }
      if (std::memcmp(scratch.data(), defaultTile.data(), sizeof(T)) == 0) continue;
      out->getOrCreate(TileCoord{tx, ty}) = scratch;
    }
  }
}

// --------------------------------------------------------------------------
// The coordinate fold, and the one place a store-level transform is decided
// --------------------------------------------------------------------------

// `translate(-dstOrigin) * dstFromSrcDoc * translate(srcOrigin)`.
//
// One matrix, so PRD D16 holds through the coordinate change as well as through
// the user's stack: cropping the source to its region, resampling, and then
// translating the result into place would be two generations and a half-pixel
// convention argument at each seam. Both translations are integer, so a matrix
// that was on `exactRemapKind()`'s no-resample path before the fold is still on
// it after -- which is what keeps a document-level flip exact (PRD D15).
Mat3 foldRegionOrigins(const Mat3& dstFromSrcDoc, const DocumentRegion& srcRegion,
                       const DocumentRegion& dstRegion) noexcept {
  return mat3Multiply(
      mat3Multiply(transformTranslate(-static_cast<float>(dstRegion.x),
                                      -static_cast<float>(dstRegion.y)),
                   dstFromSrcDoc),
      transformTranslate(static_cast<float>(srcRegion.x), static_cast<float>(srcRegion.y)));
}

// --------------------------------------------------------------------------
// Selection translate, by hand
//
// **This is a deliberate near-copy of `core::translatedTileStore()`'s gather
// and it is not a generalisation of it.** That template moves raw `uint16_t`
// and reaches `T::kChannels`; `SelectionTile` stores `uint8_t` and has no
// channel count, because a selection is one byte of coverage and has never
// needed one. Widening the template would mean changing a `core/` type's
// contract and a `core/` template's element type to suit one `ops/` caller,
// and the two are used by every layer translate and every align in the build.
// Twenty-five lines here is the cheaper of the two prices, and the duplication
// is named rather than discovered.
//
// The alternative that needs no code at all -- routing the translate through
// `transformSelectionCoverage()` with an integer-translate matrix, which is
// bit-exact on the exact path -- was rejected on cost, not correctness: it
// materialises the selection's bounding box as four floats per texel, which for
// a Select All on a 4K canvas is 141 MB in flight for an operation that is
// otherwise a `memcpy`.
Selection translatedSelection(const Selection& in, int32_t dx, int32_t dy) {
  if (dx == 0 && dy == 0) return in;

  if (floorMod(dx, kTileSize) == 0 && floorMod(dy, kTileSize) == 0) {
    Selection out = in;  // shares every slot; the re-key copies nothing
    out.tiles.rekeyTiles(floorDiv(dx, kTileSize), floorDiv(dy, kTileSize));
    return out;
  }

  std::unordered_map<TileCoord, std::unique_ptr<SelectionTile>> built;
  for (const auto& [sourceCoord, sourceTile] : in.tiles) {
    const PixelCoord origin = tileOrigin(sourceCoord);
    for (int32_t ly = 0; ly < kTileSize; ++ly) {
      const int32_t docY = origin.y + ly + dy;
      const int32_t destTileY = floorDiv(docY, kTileSize);
      const int32_t destLocalY = floorMod(docY, kTileSize);
      int32_t lx = 0;
      while (lx < kTileSize) {
        const int32_t docX = origin.x + lx + dx;
        const int32_t destTileX = floorDiv(docX, kTileSize);
        const int32_t destLocalX = floorMod(docX, kTileSize);
        const int32_t run = std::min(kTileSize - lx, kTileSize - destLocalX);
        std::unique_ptr<SelectionTile>& slot = built[TileCoord{destTileX, destTileY}];
        if (!slot) slot = std::make_unique<SelectionTile>();
        std::memcpy(slot->data() + static_cast<size_t>(destLocalY) * kTileSize +
                        static_cast<size_t>(destLocalX),
                    sourceTile.data() + static_cast<size_t>(ly) * kTileSize +
                        static_cast<size_t>(lx),
                    static_cast<size_t>(run));
        lx += run;
      }
    }
  }

  Selection out;
  for (const auto& entry : built) {
    // A tile that selects nothing is indistinguishable from absent, so keeping
    // it would cost 16 KiB to say what a missing tile already says.
    if (entry.second->selectsNothing()) continue;
    out.tiles.getOrCreate(entry.first) = *entry.second;
  }
  return out;
}

// --------------------------------------------------------------------------
// Layer-level plumbing shared by every document entry point
// --------------------------------------------------------------------------

std::string layerLabelFor(const Document& doc, size_t index) {
  const Layer& l = doc.layers[index];
  return "layer " + std::to_string(index) +
         (l.name.empty() ? std::string(" (unnamed)") : " ('" + l.name + "')");
}

// Every store of one layer, moved by an integer document offset. The three
// stores go through `core::translatedTileStore()` -- raw half words, no decode,
// no rounding step that *could* lose a bit (core/LayerGeometry.hpp §2).
//
// **The mask moves with the pixels**, which is the whole of this file's §1: a
// crop that re-keyed `rgbTiles` and not `mask` slides a layer's coverage off
// the content it was painted for, and the damage shows up a long way from the
// crop that caused it.
void translateLayerStores(Layer& layer, int32_t dx, int32_t dy) {
  if (dx == 0 && dy == 0) return;
  if (layer.rgbTiles.has_value()) *layer.rgbTiles = translatedTileStore(*layer.rgbTiles, dx, dy);
  if (layer.pigmentTiles.has_value())
    *layer.pigmentTiles = translatedTileStore(*layer.pigmentTiles, dx, dy);
  if (layer.mask.has_value()) *layer.mask = translatedTileStore(*layer.mask, dx, dy);
}

DocumentTransformResult failDocument(const std::string& message, const Document& doc) {
  DocumentTransformResult r;
  r.error = message;
  r.previousWidth = doc.width;
  r.previousHeight = doc.height;
  return r;
}

}  // namespace

// --------------------------------------------------------------------------
// Regions
// --------------------------------------------------------------------------

DocumentRegion regionFromBounds(const LayerBounds& bounds) noexcept {
  if (bounds.empty) return DocumentRegion{};
  DocumentRegion r;
  r.x = bounds.minX;
  r.y = bounds.minY;
  r.width = static_cast<uint32_t>(bounds.maxX - bounds.minX + 1);
  r.height = static_cast<uint32_t>(bounds.maxY - bounds.minY + 1);
  return r;
}

DocumentRegion documentCanvasRegion(const Document& doc) noexcept {
  if (doc.width <= 0 || doc.height <= 0) return DocumentRegion{};
  DocumentRegion r;
  r.width = static_cast<uint32_t>(doc.width);
  r.height = static_cast<uint32_t>(doc.height);
  return r;
}

DocumentRegion transformedRegion(const Mat3& dstFromSrc, const DocumentRegion& src) noexcept {
  if (src.empty()) return DocumentRegion{};
  // `transformedBounds()` works from the image's *outer* corners, so it is fed
  // the region as an extent at the origin and the origin is added back after --
  // the same fold `foldRegionOrigins()` does for the matrix, done in float here
  // because a bounding box is not a matrix.
  const Mat3 shifted = mat3Multiply(
      dstFromSrc, transformTranslate(static_cast<float>(src.x), static_cast<float>(src.y)));
  const TransformBounds b = transformedBounds(shifted, src.width, src.height);
  if (!std::isfinite(b.minX) || !std::isfinite(b.minY) || !std::isfinite(b.maxX) ||
      !std::isfinite(b.maxY))
    return DocumentRegion{};  // a corner on a perspective horizon has no extent

  // Outset by one, floor/ceil. See the header: a rounding margin, deliberately
  // not a kernel-radius margin.
  const double x0 = std::floor(static_cast<double>(b.minX)) - 1.0;
  const double y0 = std::floor(static_cast<double>(b.minY)) - 1.0;
  const double x1 = std::ceil(static_cast<double>(b.maxX)) + 1.0;
  const double y1 = std::ceil(static_cast<double>(b.maxY)) + 1.0;

  // A transform can legitimately ask for an enormous region (a 1000x scale),
  // and an extent that does not fit an int32 is not a region this build can
  // key tiles for. Clamped rather than wrapped, so the failure is a refusal
  // downstream in `transformImage()` (which checks the buffer size) rather
  // than a negative width.
  constexpr double kLimit = 1.0e9;
  if (x0 < -kLimit || y0 < -kLimit || x1 > kLimit || y1 > kLimit) return DocumentRegion{};
  if (x1 <= x0 || y1 <= y0) return DocumentRegion{};

  DocumentRegion r;
  r.x = static_cast<int32_t>(x0);
  r.y = static_cast<int32_t>(y0);
  r.width = static_cast<uint32_t>(x1 - x0);
  r.height = static_cast<uint32_t>(y1 - y0);
  return r;
}

DocumentRegion rgbContentRegion(const TileStore& tiles) {
  return scanStoreRegion(tiles, Tile::kChannels, /*channel=*/3, halfWordIsNonZero);
}

DocumentRegion pigmentContentRegion(const PigmentTileStore& tiles) {
  // Channel 3 is `pig.m`. It shares an index with RGB's alpha and that is a
  // coincidence of two independent layouts, not a rule -- core/LayerGeometry.cpp
  // makes the same point about the same two channels.
  return scanStoreRegion(tiles, PigmentTile::kChannels, /*channel=*/3, halfWordIsNonZero);
}

DocumentRegion maskContentRegion(const MaskTileStore& tiles) {
  // A mask's "nothing here" is **reveal**, so the content test is "not exactly
  // 1.0" and not "not zero". `kRevealWord` is the only encoding of 1.0 in
  // binary16, which is what lets this be a word comparison -- io/NpaintFile's
  // own drop rule for a mask tile, reused rather than restated.
  return scanStoreRegion(tiles, MaskTile::kChannels, /*channel=*/0,
                         [](uint16_t w) { return w != MaskTile::kRevealWord; });
}

DocumentRegion selectionContentRegion(const Selection& selection) {
  return scanStoreRegion(selection.tiles, /*channels=*/1, /*channel=*/0,
                         [](uint8_t v) { return v != 0u; });
}

// --------------------------------------------------------------------------
// Kernels
// --------------------------------------------------------------------------

const char* latentKernelName(LatentKernel kernel) noexcept {
  switch (kernel) {
    case LatentKernel::Nearest: return "nearest";
    case LatentKernel::Bilinear: return "bilinear";
  }
  return "?";
}

ResampleKernel resampleKernelFor(LatentKernel kernel) noexcept {
  switch (kernel) {
    case LatentKernel::Nearest: return ResampleKernel::Nearest;
    case LatentKernel::Bilinear: return ResampleKernel::Bilinear;
  }
  return ResampleKernel::Bilinear;
}

bool resampleKernelHasNegativeLobes(ResampleKernel kernel) noexcept {
  switch (kernel) {
    case ResampleKernel::Nearest:
    case ResampleKernel::Bilinear: return false;
    case ResampleKernel::CatmullRom:
    case ResampleKernel::Mitchell:
    case ResampleKernel::Lanczos3: return true;
  }
  return true;  // an unknown kernel is assumed to ring, which is the safe way round
}

bool latentKernelFor(ResampleKernel kernel, LatentKernel* out, std::string* errorOut) {
  switch (kernel) {
    case ResampleKernel::Nearest:
      if (out) *out = LatentKernel::Nearest;
      return true;
    case ResampleKernel::Bilinear:
      if (out) *out = LatentKernel::Bilinear;
      return true;
    default: break;
  }
  if (errorOut)
    *errorOut =
        std::string("pigment transform refused: ") + resampleKernelName(kernel) +
        " has negative lobes, and a resample of a pigment latent must be a POSITIVE-weight "
        "combination or it is not a Kubelka-Munk mix at all (DESIGN-imaging.md section 3). An "
        "overshoot in c0..c2 drives the implied fourth pigment weight c3 = 1-(c0+c1+c2) outside "
        "the model, so the ringing is not a bright edge, it is a pigment that was never in the "
        "picture. Choose nearest or bilinear for the latent channels.";
  return false;
}

// --------------------------------------------------------------------------
// The store-level bridges
// --------------------------------------------------------------------------

bool transformRgbTiles(const TileStore& in, const DocumentRegion& srcRegion,
                       const Mat3& dstFromSrcDoc, const DocumentRegion& dstRegion,
                       const TransformParams& params, TileStore* out, TransformReport* report,
                       std::string* errorOut) {
  if (out == nullptr) {
    if (errorOut) *errorOut = "transform refused: no destination tile store was given.";
    return false;
  }
  *out = TileStore{};
  if (report) *report = TransformReport{};
  if (srcRegion.empty() || dstRegion.empty()) return true;

  const TransformImage src = imageFromTileStore(in, srcRegion.x, srcRegion.y, srcRegion.width,
                                                srcRegion.height);
  TransformImage dst;
  if (!transformImage(src, foldRegionOrigins(dstFromSrcDoc, srcRegion, dstRegion), dstRegion.width,
                      dstRegion.height, params, &dst, report, errorOut))
    return false;
  // `tileStoreFromImage()` already declines to allocate a tile that is entirely
  // transparent and does not already exist, which for a fresh store is every
  // empty corner of a rotation.
  tileStoreFromImage(dst, dstRegion.x, dstRegion.y, out);
  return true;
}

bool transformMaskTiles(const MaskTileStore& in, const DocumentRegion& srcRegion,
                        const Mat3& dstFromSrcDoc, const DocumentRegion& dstRegion,
                        const TransformParams& params, MaskTileStore* out, TransformReport* report,
                        std::string* errorOut) {
  if (out == nullptr) {
    if (errorOut) *errorOut = "transform refused: no destination mask store was given.";
    return false;
  }
  *out = MaskTileStore{};
  if (report) *report = TransformReport{};
  if (srcRegion.empty() || dstRegion.empty()) return true;

  // **Hide space** (header §3): h = 1 - coverage, in all four channels, so the
  // resampler's transparent-black-outside policy means *reveal* outside. The
  // buffer is pre-filled with 0, which is h for a fully revealed texel, so an
  // absent source tile needs no work at all -- it already reads as reveal.
  TransformImage src;
  src.width = srcRegion.width;
  src.height = srcRegion.height;
  src.px.assign(static_cast<size_t>(srcRegion.width) * srcRegion.height * 4u, 0.0f);
  readRegionFromStore(in, srcRegion, [&](const MaskTile& tile, PixelCoord local, size_t index) {
    const float h = 1.0f - tile.readCoverage(local);
    float* d = src.px.data() + index * 4u;
    d[0] = d[1] = d[2] = d[3] = h;
  });

  TransformImage dst;
  if (!transformImage(src, foldRegionOrigins(dstFromSrcDoc, srcRegion, dstRegion), dstRegion.width,
                      dstRegion.height, params, &dst, report, errorOut))
    return false;

  // Channel 3 rather than 0: all four carry `h` and stay equal through every
  // path here, but channel 3 is the one `transformImage()`'s alpha guard
  // protects, so reading it is reading the value that has been through the same
  // rule the rest of the codebase applies to a coverage.
  buildStoreForRegion<MaskTile>(
      dstRegion,
      [&](MaskTile& tile, PixelCoord local, size_t index) {
        tile.writeCoverage(local, 1.0f - dst.px[index * 4u + 3u]);
      },
      out);
  return true;
}

bool transformPigmentTiles(const PigmentTileStore& in, const DocumentRegion& srcRegion,
                           const Mat3& dstFromSrcDoc, const DocumentRegion& dstRegion,
                           LatentKernel kernel, bool prefilterDownscale, PigmentTileStore* out,
                           TransformReport* report, std::string* errorOut) {
  if (out == nullptr) {
    if (errorOut) *errorOut = "transform refused: no destination pigment store was given.";
    return false;
  }
  *out = PigmentTileStore{};
  if (report) *report = TransformReport{};
  if (srcRegion.empty() || dstRegion.empty()) return true;

  // Header §2(c): premultiply by mass. A texel with mass 0 holds an arbitrary
  // latent -- nothing wrote it -- and multiplying by 0 gives it zero colour AND
  // zero weight, which is the pigment-space twin of what premultiplied alpha
  // does for a transparent RGB texel.
  const size_t texels = static_cast<size_t>(srcRegion.width) * srcRegion.height;
  TransformImage srcA, srcB;
  srcA.width = srcB.width = srcRegion.width;
  srcA.height = srcB.height = srcRegion.height;
  srcA.px.assign(texels * 4u, 0.0f);
  srcB.px.assign(texels * 4u, 0.0f);
  readRegionFromStore(in, srcRegion, [&](const PigmentTile& tile, PixelCoord local, size_t index) {
    const PigmentTexel t = tile.readTexel(local);
    const float m = t.mass;
    float* a = srcA.px.data() + index * 4u;
    float* b = srcB.px.data() + index * 4u;
    for (int c = 0; c < 3; ++c) {
      a[c] = t.latent.c[c] * m;
      b[c] = t.latent.res[c] * m;
    }
    a[3] = b[3] = m;
  });

  TransformParams p;
  p.kernel = resampleKernelFor(kernel);  // no negative lobes: the type has none
  p.prefilterDownscale = prefilterDownscale;
  p.allowExactPaths = true;

  const Mat3 image = foldRegionOrigins(dstFromSrcDoc, srcRegion, dstRegion);
  TransformImage dstA, dstB;
  TransformReport reportA, reportB;
  if (!transformImage(srcA, image, dstRegion.width, dstRegion.height, p, &dstA, &reportA, errorOut))
    return false;
  if (!transformImage(srcB, image, dstRegion.width, dstRegion.height, p, &dstB, &reportB, errorOut))
    return false;
  if (report) *report = reportA;

  buildStoreForRegion<PigmentTile>(
      dstRegion,
      [&](PigmentTile& tile, PixelCoord local, size_t index) {
        const float* a = dstA.px.data() + index * 4u;
        const float* b = dstB.px.data() + index * 4u;
        const float m = a[3];
        PigmentTexel t;
        // No paint in the footprint: the default texel, mass 0 and a zero
        // latent, rather than a divide by ~0 that would invent a pigment out of
        // rounding noise.
        if (!(m > 0.0f)) {
          tile.writeTexel(local, t);
          return;
        }
        const float inv = 1.0f / m;
        for (int c = 0; c < 3; ++c) {
          t.latent.c[c] = a[c] * inv;
          t.latent.res[c] = b[c] * inv;
        }
        t.mass = m;
        tile.writeTexel(local, t);
      },
      out);
  return true;
}

bool transformSelectionCoverage(const Selection& in, const DocumentRegion& srcRegion,
                                const Mat3& dstFromSrcDoc, const DocumentRegion& dstRegion,
                                const TransformParams& params, Selection* out,
                                TransformReport* report, std::string* errorOut) {
  if (out == nullptr) {
    if (errorOut) *errorOut = "transform refused: no destination selection was given.";
    return false;
  }
  *out = Selection{};
  if (report) *report = TransformReport{};
  if (srcRegion.empty() || dstRegion.empty()) return true;

  // Verbatim, no change of variable: a selection's absent-means-0 already
  // agrees with the resampler's transparent-black outside (header §3).
  TransformImage src;
  src.width = srcRegion.width;
  src.height = srcRegion.height;
  src.px.assign(static_cast<size_t>(srcRegion.width) * srcRegion.height * 4u, 0.0f);
  readRegionFromStore(in.tiles, srcRegion,
                      [&](const SelectionTile& tile, PixelCoord local, size_t index) {
                        const float c = tile.coverageAt(local);
                        float* d = src.px.data() + index * 4u;
                        d[0] = d[1] = d[2] = d[3] = c;
                      });

  TransformImage dst;
  if (!transformImage(src, foldRegionOrigins(dstFromSrcDoc, srcRegion, dstRegion), dstRegion.width,
                      dstRegion.height, params, &dst, report, errorOut))
    return false;

  buildStoreForRegion<SelectionTile>(
      dstRegion,
      [&](SelectionTile& tile, PixelCoord local, size_t index) {
        // `writeCoverage()` clamps to [0,1] and rounds to nearest, so a ringing
        // kernel's overshoot lands on "fully selected" rather than wrapping to
        // "unselected" -- the worst possible rounding of that mistake, and the
        // one core/SelectionMask.hpp designed out at the setter.
        tile.writeCoverage(local, dst.px[index * 4u + 3u]);
      },
      &out->tiles);
  return true;
}

// --------------------------------------------------------------------------
// The layer entry point
// --------------------------------------------------------------------------

LayerTransformResult transformLayer(Document& doc, size_t index, const Mat3& dstFromSrc,
                                    const DocumentTransformParams& params) {
  LayerTransformResult r;
  if (index >= doc.layers.size()) {
    r.error = "transform layer refused: index " + std::to_string(index) +
              " is out of range; this document has " + std::to_string(doc.layers.size()) +
              " layer(s). Nothing was changed.";
    return r;
  }
  Layer& layer = doc.layers[index];
  if (layer.locked) {
    r.error = "transform layer refused: " + layerLabelFor(doc, index) +
              " is locked, and a lock freezes a layer's content. Unlock it first. Nothing was "
              "changed. (A document-level crop, canvas resize or image resize moves a locked "
              "layer anyway -- that is a change to the grid every layer is expressed in, not an "
              "edit of this one.)";
    return r;
  }

  // Refuse a matrix nothing can be sampled through *before* touching a store,
  // so a layer cannot be left half transformed. `transformImage()` would refuse
  // it too, but only after the first store had already been replaced.
  Mat3 unused;
  if (!mat3Invert(dstFromSrc, &unused)) {
    r.error = "transform layer refused: " + layerLabelFor(doc, index) +
              "'s matrix is not invertible -- a collapsed or zero-scale transform has no source "
              "position for a destination pixel to read from. Nothing was changed.";
    return r;
  }

  // Every store is transformed into a fresh local, and only committed once all
  // of them have succeeded. A layer whose pixels moved and whose mask did not
  // is the failure §1 is about, and a mid-operation refusal is the one way this
  // file could produce one.
  TileStore newRgb;
  PigmentTileStore newPigment;
  MaskTileStore newMask;
  TransformReport rgbReport, pigmentReport, maskReport;

  if (layer.rgbTiles.has_value()) {
    const DocumentRegion src = rgbContentRegion(*layer.rgbTiles);
    if (!src.empty()) {
      if (!transformRgbTiles(*layer.rgbTiles, src, dstFromSrc, transformedRegion(dstFromSrc, src),
                             params.pixels, &newRgb, &rgbReport, &r.error))
        return r;
      r.movedRgb = true;
    }
  }
  if (layer.pigmentTiles.has_value()) {
    const DocumentRegion src = pigmentContentRegion(*layer.pigmentTiles);
    if (!src.empty()) {
      if (!transformPigmentTiles(*layer.pigmentTiles, src, dstFromSrc,
                                 transformedRegion(dstFromSrc, src), params.latent,
                                 params.pixels.prefilterDownscale, &newPigment, &pigmentReport,
                                 &r.error))
        return r;
      r.movedPigment = true;
    }
  }
  if (layer.mask.has_value()) {
    const DocumentRegion src = maskContentRegion(*layer.mask);
    // A mask with no content at all is **reveal everywhere**, and reveal
    // everywhere is invariant under every transform there is. Leaving it alone
    // is not a shortcut, it is the only answer that does not turn a free
    // canonical "all 1.0" mask (core/Mask.hpp: an engaged store with zero
    // tiles) into a bounding box full of allocated reveal tiles.
    if (!src.empty()) {
      if (!transformMaskTiles(*layer.mask, src, dstFromSrc, transformedRegion(dstFromSrc, src),
                              params.pixels, &newMask, &maskReport, &r.error))
        return r;
      r.movedMask = true;
    }
  }

  if (r.movedRgb) *layer.rgbTiles = std::move(newRgb);
  if (r.movedPigment) *layer.pigmentTiles = std::move(newPigment);
  if (r.movedMask) *layer.mask = std::move(newMask);

  // A MAX, not a sum -- the stores hold disjoint data, so resampling three of
  // them is still one pass per stored value. See the header.
  r.reconstructionPasses = std::max({rgbReport.reconstructionPasses,
                                     pigmentReport.reconstructionPasses,
                                     maskReport.reconstructionPasses});
  r.exact = r.movedRgb ? rgbReport.exact : (r.movedPigment ? pigmentReport.exact : maskReport.exact);
  r.ok = true;
  r.editLabel = "transform layer";
  return r;
}

LayerTransformResult transformLayer(Document& doc, size_t index, const TransformStack& stack,
                                    const DocumentTransformParams& params) {
  // The whole of PRD D16, in one line: fold first, then resample once.
  return transformLayer(doc, index, stack.composed(), params);
}

// --------------------------------------------------------------------------
// The document entry points
// --------------------------------------------------------------------------

DocumentTransformResult cropDocument(Document& doc, int32_t x, int32_t y, uint32_t width,
                                     uint32_t height, Selection* selection) {
  if (width == 0u || height == 0u)
    return failDocument("crop refused: the requested extent is " + std::to_string(width) + "x" +
                            std::to_string(height) +
                            ", and a document with a zero dimension holds no pixels. Nothing was "
                            "changed.",
                        doc);

  DocumentTransformResult r;
  r.previousWidth = doc.width;
  r.previousHeight = doc.height;

  // The crop origin becomes the new (0,0), so every store moves by -origin.
  // This is the line §1 is about: there is no offset field to update, the tile
  // keys ARE the offset, and `translatedTileStore()` moves raw half words.
  for (Layer& layer : doc.layers) {
    if (layer.locked) ++r.lockedLayersMoved;
    translateLayerStores(layer, -x, -y);
    ++r.layersTouched;
  }
  if (selection != nullptr) {
    *selection = translatedSelection(*selection, -x, -y);
    r.selectionMoved = true;
  }

  doc.width = static_cast<int32_t>(width);
  doc.height = static_cast<int32_t>(height);

  r.ok = true;
  r.reconstructionPasses = 0;  // structurally, not measured: no matrix path was taken
  r.editLabel = "crop";
  return r;
}

DocumentTransformResult resizeDocumentCanvas(Document& doc, uint32_t width, uint32_t height,
                                             CanvasAnchor anchor, Selection* selection) {
  if (width == 0u || height == 0u)
    return failDocument("canvas size refused: the requested extent is " + std::to_string(width) +
                            "x" + std::to_string(height) +
                            ", and a document with a zero dimension holds no pixels. Nothing was "
                            "changed.",
                        doc);

  // The renderer's ceiling (core/CanvasLimits.hpp). Checked beside the
  // zero-extent refusal rather than in the UI, because Image Size and Canvas
  // Size are the two commands that can take a document that opened fine and
  // grow it past what ui/DocumentTexture can create -- an upscale to 20000px
  // aborted the process exactly as opening a 20000px file did, and the guard
  // at the open path would never have seen it. Refused before any pixel moves,
  // so "Nothing was changed" stays true.
  if (std::string why = canvasDimensionRefusal(static_cast<int32_t>(width),
                                               static_cast<int32_t>(height));
      !why.empty())
    return failDocument("canvas size refused: " + why + " Nothing was changed.", doc);

  // The offset the anchor implies, **floored** for a centred one. Growing by an
  // odd number of pixels has to put the extra pixel on one side, and flooring
  // puts it on the right/bottom for every parity -- rounding would put it on
  // different sides for different parities, which a user sees as a one-pixel
  // jitter while dragging a size field. `ops/Transform.hpp::resizeCanvas()`'s
  // rule, spelled out again here because the two are separate implementations
  // of the same convention and a reader deserves to see they agree.
  const int32_t dw = static_cast<int32_t>(width) - doc.width;
  const int32_t dh = static_cast<int32_t>(height) - doc.height;
  int32_t offsetX = 0;
  int32_t offsetY = 0;
  switch (anchor) {
    case CanvasAnchor::TopLeft:
    case CanvasAnchor::CenterLeft:
    case CanvasAnchor::BottomLeft: offsetX = 0; break;
    case CanvasAnchor::TopCenter:
    case CanvasAnchor::Center:
    case CanvasAnchor::BottomCenter: offsetX = floorDiv(dw, 2); break;
    case CanvasAnchor::TopRight:
    case CanvasAnchor::CenterRight:
    case CanvasAnchor::BottomRight: offsetX = dw; break;
  }
  switch (anchor) {
    case CanvasAnchor::TopLeft:
    case CanvasAnchor::TopCenter:
    case CanvasAnchor::TopRight: offsetY = 0; break;
    case CanvasAnchor::CenterLeft:
    case CanvasAnchor::Center:
    case CanvasAnchor::CenterRight: offsetY = floorDiv(dh, 2); break;
    case CanvasAnchor::BottomLeft:
    case CanvasAnchor::BottomCenter:
    case CanvasAnchor::BottomRight: offsetY = dh; break;
  }

  DocumentTransformResult r = cropDocument(doc, -offsetX, -offsetY, width, height, selection);
  if (r.ok) r.editLabel = "canvas size";
  return r;
}

DocumentTransformResult resizeDocumentImage(Document& doc, uint32_t width, uint32_t height,
                                            const DocumentTransformParams& params,
                                            Selection* selection) {
  if (width == 0u || height == 0u)
    return failDocument("image size refused: the requested extent is " + std::to_string(width) +
                            "x" + std::to_string(height) + ". Nothing was changed.",
                        doc);
  if (doc.width <= 0 || doc.height <= 0)
    return failDocument("image size refused: this document is " + std::to_string(doc.width) + "x" +
                            std::to_string(doc.height) +
                            ", so there is no scale factor from it to " + std::to_string(width) +
                            "x" + std::to_string(height) + ". Nothing was changed.",
                        doc);

  // The renderer's ceiling (core/CanvasLimits.hpp). Checked beside the
  // zero-extent refusal rather than in the UI, because Image Size and Canvas
  // Size are the two commands that can take a document that opened fine and
  // grow it past what ui/DocumentTexture can create -- an upscale to 20000px
  // aborted the process exactly as opening a 20000px file did, and the guard
  // at the open path would never have seen it. Refused before any pixel moves,
  // so "Nothing was changed" stays true.
  if (std::string why = canvasDimensionRefusal(static_cast<int32_t>(width),
                                               static_cast<int32_t>(height));
      !why.empty())
    return failDocument("image size refused: " + why + " Nothing was changed.", doc);

  if (static_cast<int32_t>(width) == doc.width && static_cast<int32_t>(height) == doc.height) {
    // A 1:1 "resize" must not perturb a value. `resizeImage()` short-circuits
    // for the same reason: a reconstruction kernel at exactly integer offsets is
    // an identity only to within weight rounding, and this build's own tests
    // would then be asserting against noise nobody asked for.
    DocumentTransformResult r;
    r.ok = true;
    r.previousWidth = doc.width;
    r.previousHeight = doc.height;
    r.layersTouched = 0;
    r.editLabel = "image size";
    return r;
  }

  const Mat3 scale = transformScale(static_cast<float>(width) / static_cast<float>(doc.width),
                                    static_cast<float>(height) / static_cast<float>(doc.height));
  DocumentTransformResult r = transformDocument(doc, scale, width, height, params, selection);
  if (r.ok) r.editLabel = "image size";
  return r;
}

DocumentTransformResult transformDocument(Document& doc, const Mat3& dstFromSrc,
                                          uint32_t newWidth, uint32_t newHeight,
                                          const DocumentTransformParams& params,
                                          Selection* selection) {
  if (newWidth == 0u || newHeight == 0u)
    return failDocument("document transform refused: the requested extent is " +
                            std::to_string(newWidth) + "x" + std::to_string(newHeight) +
                            ". Nothing was changed.",
                        doc);
  Mat3 unused;
  if (!mat3Invert(dstFromSrc, &unused))
    return failDocument(
        "document transform refused: the matrix is not invertible -- a collapsed or zero-scale "
        "transform has no source position for a destination pixel to read from. Nothing was "
        "changed.",
        doc);

  DocumentTransformResult r;
  r.previousWidth = doc.width;
  r.previousHeight = doc.height;

  // **Locked layers move too** -- header §5. The lock is honoured by
  // `transformLayer()`, which is the per-layer edit; this is a change to the
  // grid, and a locked layer left behind would be misregistered against every
  // other layer by exactly the transform, which is the lock destroying the
  // document in order to protect a layer.
  //
  // The lock is lifted for the duration and restored, rather than reaching past
  // `transformLayer()` into the stores directly, so there is exactly one place
  // in this file that knows how to transform a layer and the refusal ladder
  // (invertibility, storage, mask) cannot drift between two copies of it.
  for (size_t i = 0; i < doc.layers.size(); ++i) {
    Layer& layer = doc.layers[i];
    const bool wasLocked = layer.locked;
    if (wasLocked) {
      layer.locked = false;
      ++r.lockedLayersMoved;
    }
    const LayerTransformResult lr = transformLayer(doc, i, dstFromSrc, params);
    doc.layers[i].locked = wasLocked;
    if (!lr.ok) {
      // Partial: some layers have moved. Say so rather than claiming nothing
      // was changed, because something was, and a caller that believed
      // otherwise would not restore its snapshot.
      return failDocument("document transform failed after " + std::to_string(i) +
                              " layer(s) had already moved -- " + lr.error +
                              " The document is now inconsistent; restore the undo snapshot.",
                          doc);
    }
    r.reconstructionPasses = std::max(r.reconstructionPasses, lr.reconstructionPasses);
    ++r.layersTouched;
  }

  if (selection != nullptr) {
    const DocumentRegion src = selectionContentRegion(*selection);
    if (!src.empty()) {
      Selection moved;
      TransformReport selReport;
      std::string err;
      if (!transformSelectionCoverage(*selection, src, dstFromSrc,
                                      transformedRegion(dstFromSrc, src), params.pixels, &moved,
                                      &selReport, &err))
        return failDocument("document transform failed on the selection after every layer had "
                            "already moved -- " +
                                err + " The document is now inconsistent; restore the undo "
                                      "snapshot.",
                            doc);
      *selection = std::move(moved);
      r.reconstructionPasses = std::max(r.reconstructionPasses, selReport.reconstructionPasses);
    }
    r.selectionMoved = true;
  }

  doc.width = static_cast<int32_t>(newWidth);
  doc.height = static_cast<int32_t>(newHeight);
  r.ok = true;
  r.editLabel = "transform document";
  return r;
}

}  // namespace np
