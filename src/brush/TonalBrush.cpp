#include "brush/TonalBrush.hpp"

#include <algorithm>
#include <cmath>

#include "brush/Grain.hpp"
#include "color/Space.hpp"

namespace np {

const char* tonalDirectionName(TonalDirection dir) noexcept {
  switch (dir) {
    case TonalDirection::Dodge: return "dodge";
    case TonalDirection::Burn: return "burn";
  }
  return "?";
}

float tonalCurve(float displayValue, float gamma) noexcept {
  // Header §2's domain rule, and the reason it is a rule rather than a clamp:
  // `d` outside [0,1] is a real input (color/Space.hpp is explicit that
  // working-space values exceed 1.0 and can go slightly negative, and
  // `srgbEncode()` is unclamped), and `d^g` there has the WRONG SIGN in both
  // directions -- a dodge exponent darkens a superwhite highlight and a burn
  // exponent brightens it, invisibly, because both still clip to white.
  //
  // `!(x > y)` rather than `x <= y`, so a NaN refuses instead of reaching
  // `std::pow` -- the same guard shape `depositRgbTexel()`, `eraseRgbTexel()`
  // and `layerCoverage()` all use.
  if (!(displayValue > 0.0f)) return displayValue;
  if (!(displayValue < 1.0f)) return displayValue;
  if (!(gamma > 0.0f)) return displayValue;
  return std::pow(displayValue, gamma);
}

TonalStep toneRgbTexel(const std::array<float, 4>& dst, float strokeTone, float weight,
                       float strength, TonalDirection direction) noexcept {
  TonalStep out;
  // The no-op answer, returned by every refusal below. `dst` rather than
  // something recomputed from it -- see the header's note on bit-identity: a
  // stroke that has reached its ceiling must leave the tile it is scrubbing
  // over completely alone.
  out.premultiplied = dst;
  out.strokeTone = strokeTone;
  out.dabGamma = 1.0f;
  out.changed = false;

  const float cap = std::clamp(strength, 0.0f, 1.0f);
  const float t0 = std::clamp(strokeTone, 0.0f, 1.0f);

  if (!(weight > 0.0f)) return out;  // no coverage, or the dab has no flow
  if (!(cap > 0.0f)) return out;     // a stroke asked to shift nothing
  if (!(t0 < cap)) return out;       // the ceiling is already reached (§3)

  // **No coverage means no colour to shift** -- header §1. This is where this
  // module deliberately parts company with `brush/RgbErase` §4, which erases a
  // malformed `(colour, 0)` texel rather than declaring it absent: the eraser
  // removes what is there and that texel holds something, while a tonal op
  // shifts a colour and `rgb / 0` is not one. Also the divide-by-zero guard,
  // but it is the meaning that decides the rule, not the division.
  const float a = dst[3];
  if (!(a > 0.0f)) return out;

  // §3. `T'` is the fraction of this stroke's shift applied after this dab,
  // capped at the strength; `dT` is this dab's share of it, and the dab's
  // exponent is `kTonalFullGamma^(±dT)` -- a DIFFERENCE and not a ratio, which
  // is why there is no `1 - T` divisor and no singular case at `T == 1`.
  float t1 = t0 + weight * (1.0f - t0);
  if (t1 > cap) t1 = cap;
  const float dt = t1 - t0;
  // `exp2` of a signed multiple of `log2(kTonalFullGamma)` rather than
  // `std::pow(kTonalFullGamma, ...)`: with the constant at 2 the log2 is
  // exactly 1, so this is `exp2(±dT)` and `dT == 0` gives exactly 1.0f with no
  // rounding -- which is what makes "a dab past the ceiling is the identity" a
  // fact about the arithmetic rather than a tolerance.
  const float signedDt = direction == TonalDirection::Burn ? dt : -dt;
  const float gamma = std::exp2(signedDt * std::log2(kTonalFullGamma));

  // Header §1: un-premultiply, shift, re-premultiply, and COPY the alpha.
  //
  // The alpha is `dst[3]` itself and not a recomputed value, so the stored
  // binary16 word is unchanged bit-for-bit. A tonal op adjusts colour; it does
  // not create or destroy coverage, and dodging a fully transparent texel
  // leaves it fully transparent because there is nothing in this function that
  // could do otherwise.
  const float invA = 1.0f / a;
  std::array<float, 4> next = dst;
  next[3] = dst[3];
  for (int i = 0; i < 3; ++i) {
    const float straight = dst[i] * invA;
    const float display = srgbEncode(straight);
    const float shifted = tonalCurve(display, gamma);
    // A channel the curve left alone keeps its STORED value rather than a
    // round-tripped one. `srgbDecode(srgbEncode(x))` is `x` to within a couple
    // of ulps and `(x/a)*a` is not exactly `x` either, so recomputing an
    // untouched channel would write a value that differs from the one already
    // there for no reason the user asked for -- and would defeat §5's
    // "unmoved texels cost nothing" comparison below.
    next[i] = shifted == display ? dst[i] : srgbDecode(shifted) * a;
  }

  // §5's third skip: a texel the shift does not actually move. The fixed points
  // of every power law (pure black, pure white), every channel §2's domain rule
  // left alone, and the tail of the falloff where the shift is smaller than the
  // value's own binary16 spacing all arrive here as an unchanged quadruple. A
  // comparison rather than a special case, so a future curve inherits it.
  if (next == dst) return out;

  out.premultiplied = next;
  out.strokeTone = t1;
  out.dabGamma = gamma;
  out.changed = true;
  return out;
}

void TonalStroke::begin(float strength, TonalDirection direction) noexcept {
  strength_ = std::clamp(strength, 0.0f, 1.0f);
  direction_ = direction;
  // A fresh accumulator, not a cleared one: assigning a default-constructed
  // store drops every `shared_ptr` slot and therefore every tile the previous
  // stroke held, which is `end()`'s free as well as this one's.
  toned_ = StrokeAlphaStore{};
  active_ = true;
}

void TonalStroke::end() noexcept {
  toned_ = StrokeAlphaStore{};
  active_ = false;
}

float TonalStroke::ceilingGamma() const noexcept {
  const float signedT = direction_ == TonalDirection::Burn ? strength_ : -strength_;
  return std::exp2(signedT * std::log2(kTonalFullGamma));
}

float TonalStroke::strokeToneAt(PixelCoord doc) const noexcept {
  const StrokeAlphaTile* tile = toned_.find(tileCoordAt(doc));
  return tile == nullptr ? 0.0f : tile->at(tileLocalOffset(doc));
}

DepositCount TonalStroke::toneDab(TileStore& store, const BrushTip& tip, Vec2 centre,
                                  int32_t canvasW, int32_t canvasH, const Selection* selection,
                                  std::vector<TileCoord>* touchedOut) {
  DepositCount count;
  if (!(tip.flow > 0.0f)) return count;
  if (!(strength_ > 0.0f)) return count;

  // `dabPixelBounds()` and `dabCoverage()` unchanged from the three routes that
  // already use them -- the shape of a dab is not a property of what the dab
  // does, and a second falloff here would be a second place for the tonal
  // brush and the paint brush to disagree about where a tip ends. That matters
  // here for the reason it matters for the eraser: a painter dodges an edge
  // they painted, and a rim that lightened one texel wider than it painted
  // would put a halo on the stroke it was shaping.
  const PixelBounds b = dabPixelBounds(tip, centre, canvasW, canvasH);
  if (b.empty()) return count;

  const TileCoord first = tileCoordAt(PixelCoord{b.x0, b.y0});
  const TileCoord last = tileCoordAt(PixelCoord{b.x1, b.y1});

  // Tile-major, then texel within tile: one hash lookup per tile per dab
  // instead of one per texel, across the three stores keyed by the same
  // coordinate (the layer, the accumulator, the selection). Ascending (y, x) so
  // `touchedOut` comes out in `sortUniqueTiles()`'s order for the common
  // single-dab case.
  for (int32_t ty = first.y; ty <= last.y; ++ty) {
    for (int32_t tx = first.x; tx <= last.x; ++tx) {
      const TileCoord coord{tx, ty};

      // **A tile that does not exist holds no colour to shift** -- header §5.
      // Skipped before anything is allocated, so a dodge dragged across blank
      // canvas costs nothing at all rather than 224 KiB per tile it crossed
      // plus a dirty tile per frame of the drag. This is also why the read
      // handle below is non-null on every path that follows.
      const Tile* srcTile = store.find(coord);
      if (srcTile == nullptr) continue;

      // §4. The null branch is owned here rather than borrowed from
      // `selectionCoverageAt()`, which core/SelectionMask.hpp requires of every
      // hoisted loop -- a null *Selection* is "no restriction" and a null *tile*
      // inside an engaged selection is "selects nothing".
      const SelectionTile* cover = nullptr;
      if (selection != nullptr) {
        cover = selection->tiles.find(coord);
        if (cover == nullptr) continue;
      }

      const PixelCoord org = tileOrigin(coord);
      const int32_t x0 = std::max(b.x0, org.x);
      const int32_t x1 = std::min(b.x1, org.x + kTileSize - 1);
      const int32_t y0 = std::max(b.y0, org.y);
      const int32_t y1 = std::min(b.y1, org.y + kTileSize - 1);

      // The accumulator's READ handle, which may legitimately be absent -- a
      // texel this stroke has not reached has `T == 0`, and that is exactly what
      // an absent tile says, so a dab that turns out to change nothing never
      // allocates the 64 KiB.
      const StrokeAlphaTile* toneRead = toned_.find(coord);

      // Both write handles, fetched lazily at the first texel this dab actually
      // changes -- brush/Deposit §3, fact 2. A tile the bounding box clipped but
      // the disc missed, or one every texel of which has already reached the
      // ceiling, is never unshared and never reported.
      Tile* dst = nullptr;
      StrokeAlphaTile* toneWrite = nullptr;

      for (int32_t y = y0; y <= y1; ++y) {
        const float dy = (static_cast<float>(y) + 0.5f) - centre.y;
        for (int32_t x = x0; x <= x1; ++x) {
          const float dx = (static_cast<float>(x) + 0.5f) - centre.x;
          const PixelCoord local = tileLocalOffset(PixelCoord{x, y});

          const float rawCov = dabCoverage(tip, dx, dy);
          if (!(rawCov > 0.0f)) continue;

          // Paper tooth, at this texel's ABSOLUTE canvas position -- `x`/`y`,
          // not `dx`/`dy`, which is why it cannot live inside `dabCoverage()`.
          // Identical line and identical reasoning to brush/Deposit.cpp's own
          // (§2e). Present from this route's first commit rather than added
          // later, because `grainReachesRoute()` (app/StrokeSession.hpp) says
          // every layer-writing route calls it, and that predicate is what the
          // BRUSH panel greys the PAPER GRAIN group on -- a route that skipped
          // the call would make a working control lie in the other direction.
          const float cov = grainCoverageAt(tip.grain, rawCov, x, y);
          if (!(cov > 0.0f)) continue;  // a grain peak too tall for this pressure
          const float sel = selection != nullptr ? selectionTileCoverage(cover, local) : 1.0f;
          if (!(sel > 0.0f)) continue;

          const std::array<float, 4> before = srcTile->readPixel(local);
          const float applied = toneRead != nullptr ? toneRead->at(local) : 0.0f;
          // **The selection enters TWICE, and both are load-bearing** -- §4.
          // Into the weight, so one pass through a half-selected texel shifts
          // half as far; and into the ceiling, so *no number of passes* takes
          // that texel past half. The first alone is a speed limit rather than a
          // bound, and a scrubbed stroke walks straight through it.
          const TonalStep step =
              toneRgbTexel(before, applied, tip.flow * cov * sel, strength_ * sel, direction_);
          // The ceiling, the transparent tail of the falloff, a texel the
          // selection excluded, a texel with no coverage and a texel the curve
          // does not move all arrive here as `changed == false`, and all five
          // mean the same thing: do not touch this texel, do not unshare its
          // tile, do not report it dirty.
          if (!step.changed) continue;

          if (dst == nullptr) {
            dst = &store.getOrCreate(coord);
            // `getOrCreate` unshares a copy-on-write tile, so after it the
            // store's tile at this coordinate is a *different object* and
            // `srcTile` would keep showing the pre-write value --
            // core/TileStore.hpp calls that "detached", and a stroke reading
            // through a detached pointer would shift every dab after the first
            // from the pre-stroke texel, so a scrubbed dodge would repeatedly
            // apply the same first step and never get further.
            srcTile = dst;
            ++count.tiles;
            if (touchedOut != nullptr) touchedOut->push_back(coord);
          }
          if (toneWrite == nullptr) {
            // `getOrCreate` on a store nobody else holds is an allocate-or-find
            // with no copy behind it -- the accumulator is never copied out of
            // the stroke, so the copy-on-write barrier never fires. If the tile
            // already existed, this is the same object `toneRead` names.
            toneWrite = &toned_.getOrCreate(coord);
            toneRead = toneWrite;
          }
          toneWrite->set(local, step.strokeTone);
          dst->writePixel(local, step.premultiplied);
          ++count.texels;
        }
      }
    }
  }
  return count;
}

StrokeDeposit TonalStroke::toneDabs(TileStore& store, const BrushTip& tip,
                                    const std::vector<Vec2>& dabs, int32_t canvasW,
                                    int32_t canvasH, const Selection* selection) {
  StrokeDeposit out;
  for (const Vec2& dab : dabs) {
    const DepositCount c = toneDab(store, tip, dab, canvasW, canvasH, selection, &out.tiles);
    out.texels += c.texels;
    ++out.dabs;
  }
  sortUniqueTiles(out.tiles);
  return out;
}

}  // namespace np
