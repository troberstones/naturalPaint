#include "ops/Pattern.hpp"

#include <algorithm>
#include <cstdio>

namespace np {
namespace {

// The euclidean modulus of ops/Pattern.hpp section 3. `n` is always positive
// here (a pattern with a zero dimension is refused before this is reached), so
// one conditional correction is enough; the general two-step form is not
// needed and would only hide which case is the real one.
//
// C's `%` truncates toward zero: `-1 % 8 == -1`. That is the value that
// indexes off the front of the pattern buffer, and it arrives the moment a
// document texel or a tiling origin goes negative.
inline int32_t euclideanMod(int32_t v, int32_t n) noexcept {
  const int32_t r = v % n;
  return r < 0 ? r + n : r;
}

// A short, stable id for a pattern defined from a layer. Not a UUID: nothing
// joins to it across a file the way a `.abr` texture's UUID joins a brush to
// its paper, and inventing a UUID generator for a session-lifetime handle
// would be a dependency for nothing. Uniqueness within the store is what is
// needed, and the counter gives that.
std::string nextDefinedPatternId() {
  static int counter = 0;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "defined-%d", ++counter);
  return std::string(buf);
}

}  // namespace

bool definePattern(const TileStore& src, const PixelRect& bounds, std::string name, Pattern* out,
                   std::string* errorOut) {
  const auto fail = [&](std::string why) {
    if (errorOut != nullptr) *errorOut = std::move(why);
    return false;
  };
  if (out == nullptr) return fail("refused: define pattern was given nowhere to put the result.");
  *out = Pattern{};
  if (roiIsEmpty(bounds))
    return fail("refused: a pattern needs a rectangle with width and height; this one is empty.");
  const int64_t w = bounds.width();
  const int64_t h = bounds.height();
  if (w > static_cast<int64_t>(kMaxDefinedPatternDimension) ||
      h > static_cast<int64_t>(kMaxDefinedPatternDimension))
    return fail("refused: a pattern may be at most " +
                std::to_string(kMaxDefinedPatternDimension) + " texels on an edge; this one is " +
                std::to_string(w) + "x" + std::to_string(h) + ".");

  out->id = nextDefinedPatternId();
  out->name = std::move(name);
  out->width = static_cast<uint32_t>(w);
  out->height = static_cast<uint32_t>(h);
  out->px.assign(out->sampleCount(), 0.0f);

  // A plain per-texel read, with the one-entry tile cache `offsetTiles()`
  // uses and for the same reason: the source coordinate walks in x with the
  // destination, so one remembered pointer is worth a real cache and costs two
  // words. Absent tiles read as transparent black, which is what the store's
  // implicit content for an unwritten tile already means -- so defining a
  // pattern from a region the user never painted gives a transparent pattern
  // rather than a refusal, which is correct: an empty pattern is a thing you
  // can ask for.
  TileCoord cachedCoord{};
  const Tile* cachedTile = nullptr;
  bool cacheValid = false;
  for (int32_t y = 0; y < static_cast<int32_t>(out->height); ++y) {
    float* row = out->px.data() + static_cast<size_t>(y) * out->width * 4u;
    for (int32_t x = 0; x < static_cast<int32_t>(out->width); ++x) {
      const PixelCoord doc{bounds.x0 + x, bounds.y0 + y};
      const TileCoord coord = tileCoordAt(doc);
      if (!cacheValid || !(coord == cachedCoord)) {
        cachedTile = src.find(coord);
        cachedCoord = coord;
        cacheValid = true;
      }
      if (cachedTile == nullptr) continue;  // already transparent black
      const std::array<float, 4> v = cachedTile->readPixel(tileLocalOffset(doc));
      float* d = row + static_cast<size_t>(x) * 4u;
      d[0] = v[0];
      d[1] = v[1];
      d[2] = v[2];
      d[3] = v[3];
    }
  }
  return true;
}

Pattern patternFromPsPattern(const PsPattern& source) {
  Pattern out;
  if (source.width <= 0 || source.height <= 0) return out;
  const size_t need = static_cast<size_t>(source.width) * static_cast<size_t>(source.height);
  if (source.height8.size() < need) return out;

  out.id = source.id;
  out.name = source.name;
  out.width = static_cast<uint32_t>(source.width);
  out.height = static_cast<uint32_t>(source.height);
  out.px.assign(out.sampleCount(), 0.0f);
  for (size_t i = 0; i < need; ++i) {
    // `height8` is 8-bit and its scale is 0..255 over the unit interval. It is
    // written into all three colour channels and alpha 1: the value is a
    // height field, so there is no colour in it to preserve and no coverage in
    // it either, and an opaque grey is the honest reading. The 1/255 is a
    // plain scale and NOT a transfer function -- `.abr` paper is a height
    // field, not an sRGB-encoded picture, so decoding it as one would darken
    // every texture in the library by the gamma the file never applied.
    // Divided, not multiplied by a reciprocal. `x * (1.0f/255.0f)` and
    // `x / 255.0f` disagree in the last bit for most of the 256 inputs, and
    // --selftest compares this against the obvious form -- so the obvious form
    // is the one that ships, rather than the suite being loosened to a
    // tolerance around a micro-optimisation nobody measured. A pattern is at
    // most 4096^2 and is converted once, on import.
    const float v = static_cast<float>(source.height8[i]) / 255.0f;
    float* d = out.px.data() + i * 4u;
    d[0] = v;
    d[1] = v;
    d[2] = v;
    d[3] = 1.0f;
  }
  return out;
}

PixelCoord patternSourceTexel(const Pattern& pattern, int32_t originX, int32_t originY,
                              PixelCoord doc) noexcept {
  const auto w = static_cast<int32_t>(pattern.width);
  const auto h = static_cast<int32_t>(pattern.height);
  if (w <= 0 || h <= 0) return PixelCoord{0, 0};
  return PixelCoord{euclideanMod(doc.x - originX, w), euclideanMod(doc.y - originY, h)};
}

bool patternFillParamsValid(const PatternFillParams& p) noexcept {
  return p.pattern != nullptr && p.pattern->valid();
}

bool patternFillTiles(const TileStore& src, const PixelRect& outRect, const PatternFillParams& p,
                      TileStore* dst) {
  if (dst == nullptr || dst == &src) return false;
  if (roiIsEmpty(outRect)) return false;
  if (!patternFillParamsValid(p)) return false;

  const Pattern& pattern = *p.pattern;
  for (int32_t y = outRect.y0; y < outRect.y1; ++y) {
    for (int32_t x = outRect.x0; x < outRect.x1; ++x) {
      const PixelCoord doc{x, y};
      const PixelCoord s = patternSourceTexel(pattern, p.originX, p.originY, doc);
      const float* q = pattern.px.data() +
                       (static_cast<size_t>(s.y) * pattern.width + static_cast<size_t>(s.x)) * 4u;
      // Written even when the pattern texel is transparent black. A fill
      // REPLACES; skipping the transparent texels would leave whatever was
      // underneath showing through the pattern's holes, which is a composite,
      // not a fill, and it is the difference between filling with a stencil
      // and filling with a stamp. The selection does the bounding upstream, so
      // nothing outside the marquee is reached by this loop's writes anyway.
      dst->getOrCreate(tileCoordAt(doc))
          .writePixel(tileLocalOffset(doc), {q[0], q[1], q[2], q[3]});
    }
  }
  return true;
}

void PatternStore::define(Pattern pattern) {
  for (size_t i = 0; i < patterns_.size(); ++i) {
    if (patterns_[i].name == pattern.name) {
      patterns_[i] = std::move(pattern);
      return;
    }
  }
  patterns_.push_back(std::move(pattern));
}

const Pattern* PatternStore::findByName(std::string_view name) const noexcept {
  for (const Pattern& p : patterns_)
    if (p.name == name) return &p;
  return nullptr;
}

const Pattern* PatternStore::findById(std::string_view id) const noexcept {
  for (const Pattern& p : patterns_)
    if (p.id == id) return &p;
  return nullptr;
}

PatternStore& sessionPatterns() {
  // Function-local static: constructed on first use, which is what keeps this
  // out of the static-initialisation order between translation units. The
  // store is touched from the UI thread and from `applyCommand()`, which in a
  // `--batch` run is the same thread; nothing here is thread-safe and nothing
  // needs to be, which is stated rather than assumed -- a future threaded
  // batch runner has to add the lock here and not discover the absence.
  static PatternStore store;
  return store;
}

}  // namespace np
