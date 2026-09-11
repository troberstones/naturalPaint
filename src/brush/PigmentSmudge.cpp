#include "brush/PigmentSmudge.hpp"

#include <algorithm>
#include <cmath>

#include "core/Blend.hpp"

namespace np {
namespace {

// "Holds no paint". `!(m > 0)` rather than `m <= 0`, so a NaN mass reads as
// empty instead of propagating into a mix weight -- the guard shape every
// sibling route uses.
bool holdsNoPaint(const PigmentTexel& t) noexcept { return !(t.mass > 0.0f); }

// The mass a texel contributes to a mix: its stored mass, with the NaN and
// negative cases (neither of which any writer in this build produces) counted
// as none rather than allowed to go negative inside a weight.
float paintMass(const PigmentTexel& t) noexcept { return t.mass > 0.0f ? t.mass : 0.0f; }

}  // namespace

PigmentSmudgeStep smudgePigmentTexel(const PigmentTexel& dst, const PigmentTexel& finger,
                                     float weight, float strength) noexcept {
  PigmentSmudgeStep out;
  // The no-op answer every refusal below returns: `dst` itself, so a dab that
  // changes nothing leaves the 224 KiB tile under it completely alone.
  out.texel = dst;
  out.dabAlpha = 0.0f;

  const float s = std::clamp(strength, 0.0f, 1.0f);
  if (!(weight > 0.0f)) return out;  // no coverage, no flow, or `sel == 0`
  if (!(s > 0.0f)) return out;       // header §2: strength 0 is a bit-exact no-op
  // Clamped for `smudgeTexel()`'s reason: a mix fraction above 1 would
  // extrapolate PAST the finger -- here into a negative mass on the far side
  // of a soft rim.
  float a = weight * s;
  if (a > 1.0f) a = 1.0f;
  if (!(a > 0.0f)) return out;

  // **Nothing to move** -- header §3. On MASS for the empty pair, whatever
  // stale latents the two carry (an erased texel keeps its hue on purpose,
  // brush/PigmentErase §3), and on all seven numbers otherwise.
  if (holdsNoPaint(dst) && holdsNoPaint(finger)) return out;
  if (dst == finger) return out;

  const float dm = paintMass(dst);
  const float fm = paintMass(finger);

  // Header §2. The mass is `brush/Smudge`'s alpha lerp; the latent is §1's
  // mass-weighted mean over what survives it -- `(1-a)*dm` of the texel's paint
  // and `a*fm` of the finger's. The weight is exactly 1 over a mass-0 texel
  // (`(1-a)*0 == 0`, so it is `x/x`), which lays the finger's latent down
  // outright instead of mixing in a stale hue, and exactly 0 for an empty
  // finger, which thins the paint and leaves its hue alone.
  const float keep = (1.0f - a) * dm;
  const float add = a * fm;
  const float denom = keep + add;
  const float w = denom > 0.0f ? add / denom : 0.0f;

  out.texel.latent = mixLatents(dst.latent, finger.latent, w);
  float m = std::lerp(dm, fm, a);
  // A convex combination of two masses each at most `kMaxMass` cannot exceed
  // it, so this bites only for a destination handed in already over the cap
  // (a hand-edited document). Clamped at the point of storage anyway, which is
  // what makes the invariant a property of the document -- `depositTexel()`'s
  // and `erasePigmentTexel()`'s discipline for the two ends of the same range.
  if (m > kMaxMass) m = kMaxMass;
  if (!(m > 0.0f)) m = 0.0f;
  out.texel.mass = m;
  out.dabAlpha = a;
  return out;
}

PigmentTexel smudgePigmentFinger(const PigmentTexel& finger, bool loaded, const PigmentTexel& pick,
                                 float strength) noexcept {
  // `brush/Smudge` §3's latch: the stroke's first dab loads, it does not blend.
  if (!loaded) return pick;
  const float s = std::clamp(strength, 0.0f, 1.0f);
  const float pm = paintMass(pick);
  const float fm = paintMass(finger);

  // How much of each survives: `s` of the carried paint, `1 - s` of what was
  // just picked up, each counted by its mass (header §2). At `s == 1` the
  // pick's share is `0 * pm == 0`, so the weight is `fm / fm == 1` and the
  // finger comes back exactly; at `s == 0` it is 0 and the pick comes back
  // exactly. With no paint on either side the latent is irrelevant (it is
  // multiplied by a mass of 0 at every reader) and falls back to the plain
  // strength lerp, which keeps both endpoints exact there too.
  const float keep = s * fm;
  const float add = (1.0f - s) * pm;
  const float denom = keep + add;
  const float w = denom > 0.0f ? keep / denom : s;

  PigmentTexel out;
  out.latent = mixLatents(pick.latent, finger.latent, w);
  out.mass = std::lerp(pm, fm, s);
  return out;
}

void PigmentSmudgeStroke::begin(float strength) noexcept {
  strength_ = std::clamp(strength, 0.0f, 1.0f);
  finger_ = PigmentTexel{};
  loaded_ = false;
  active_ = true;
}

void PigmentSmudgeStroke::end() noexcept {
  finger_ = PigmentTexel{};
  loaded_ = false;
  active_ = false;
}

DepositCount PigmentSmudgeStroke::smudgeDab(PigmentTileStore& store, const BrushTip& tip,
                                            Vec2 centre, int32_t canvasW, int32_t canvasH,
                                            const Selection* selection,
                                            std::vector<TileCoord>* touchedOut) {
  DepositCount count;
  // Strength alone is the early exit and `flow` is not, for
  // `SmudgeStroke::smudgeDab()`'s stated reason: a dab whose resolved flow
  // momentarily hit 0 must still pick up, or the finger would freeze and the
  // next dab would lay down paint from somewhere the tip has since left.
  if (!(strength_ > 0.0f)) return count;

  const PixelBounds b = dabPixelBounds(tip, centre, canvasW, canvasH);
  if (b.empty()) return count;

  const TileCoord first = tileCoordAt(PixelCoord{b.x0, b.y0});
  const TileCoord last = tileCoordAt(PixelCoord{b.x1, b.y1});

  // ---------------------------------------------------------------------
  // Pass 1 -- the pick-up (header §1), before any of this dab's writes, and
  // with no selection consulted: `brush/Smudge` §§2 and 4, unchanged.
  //
  // Coverage goes into `sumW` for every covered texel, painted or not -- an
  // absent tile and a mass-0 texel are both emptiness, and picking emptiness up
  // is how the smear thins (§1(i)). Only paint goes into the latent, and it goes
  // in as a RUNNING `mixLatents()` so a footprint of one pigment picks that
  // pigment up bit for bit (§1(ii)): the first contributor's weight is
  // `x / x == 1`, and every later `lerp(z, z, t)` returns `z`.
  // ---------------------------------------------------------------------
  double sumW = 0.0;
  double sumWM = 0.0;
  Latent latentMean{};
  for (int32_t ty = first.y; ty <= last.y; ++ty) {
    for (int32_t tx = first.x; tx <= last.x; ++tx) {
      const TileCoord coord{tx, ty};
      const PigmentTile* srcTile = store.find(coord);
      const PixelCoord org = tileOrigin(coord);
      const int32_t x0 = std::max(b.x0, org.x);
      const int32_t x1 = std::min(b.x1, org.x + kTileSize - 1);
      const int32_t y0 = std::max(b.y0, org.y);
      const int32_t y1 = std::min(b.y1, org.y + kTileSize - 1);
      for (int32_t y = y0; y <= y1; ++y) {
        const float dy = (static_cast<float>(y) + 0.5f) - centre.y;
        for (int32_t x = x0; x <= x1; ++x) {
          const float dx = (static_cast<float>(x) + 0.5f) - centre.x;
          const float rawCov = dabCoverage(tip, dx, dy);
          if (!(rawCov > 0.0f)) continue;
          const float cov = grainCoverageAt(tip.grain, rawCov, x, y);
          if (!(cov > 0.0f)) continue;
          sumW += static_cast<double>(cov);
          if (srcTile == nullptr) continue;  // no paint: weight, and nothing else
          const PigmentTexel px = srcTile->readTexel(tileLocalOffset(PixelCoord{x, y}));
          const float m = paintMass(px);
          if (!(m > 0.0f)) continue;  // an erased or unpainted texel holds no paint
          const double wm = static_cast<double>(cov) * static_cast<double>(m);
          sumWM += wm;
          latentMean = mixLatents(latentMean, px.latent, static_cast<float>(wm / sumWM));
        }
      }
    }
  }
  if (!(sumW > 0.0)) return count;  // the falloff and grain covered nowhere

  PigmentTexel pick;
  pick.mass = static_cast<float>(sumWM / sumW);
  // With no paint anywhere under the tip the latent is meaningless -- it rides
  // on a mass of exactly 0 -- and `latentMean` is still `Latent{}`.
  pick.latent = latentMean;

  finger_ = smudgePigmentFinger(finger_, loaded_, pick, strength_);
  loaded_ = true;

  // ---------------------------------------------------------------------
  // Pass 2 -- the write. `SmudgeStroke`'s loop, texel for texel, over the
  // other storage.
  // ---------------------------------------------------------------------
  if (!(tip.flow > 0.0f)) return count;
  const bool fingerEmpty = holdsNoPaint(finger_);

  for (int32_t ty = first.y; ty <= last.y; ++ty) {
    for (int32_t tx = first.x; tx <= last.x; ++tx) {
      const TileCoord coord{tx, ty};

      const PigmentTile* srcTile = store.find(coord);
      // Header §3: an absent tile holds no paint, and an empty finger has none
      // to lay down, so there is nothing to move -- skipped before a 224 KiB
      // tile could be allocated. A LOADED finger over an absent tile is the
      // tool growing the painted region, and falls through.
      if (srcTile == nullptr && fingerEmpty) continue;

      // The selection's own null branch, owned here as core/SelectionMask.hpp
      // requires of each hoisted loop: a null Selection is "no restriction", a
      // null tile inside an engaged one is "selects nothing".
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

      PigmentTile* dst = nullptr;

      for (int32_t y = y0; y <= y1; ++y) {
        const float dy = (static_cast<float>(y) + 0.5f) - centre.y;
        for (int32_t x = x0; x <= x1; ++x) {
          const float dx = (static_cast<float>(x) + 0.5f) - centre.x;
          const PixelCoord local = tileLocalOffset(PixelCoord{x, y});

          const float rawCov = dabCoverage(tip, dx, dy);
          if (!(rawCov > 0.0f)) continue;
          const float cov = grainCoverageAt(tip.grain, rawCov, x, y);
          if (!(cov > 0.0f)) continue;
          const float sel = selection != nullptr ? selectionTileCoverage(cover, local) : 1.0f;
          if (!(sel > 0.0f)) continue;

          const PigmentTexel before =
              srcTile != nullptr ? srcTile->readTexel(local) : PigmentTexel{};
          const PigmentSmudgeStep step =
              smudgePigmentTexel(before, finger_, tip.flow * cov * sel, strength_);
          if (!(step.dabAlpha > 0.0f)) continue;

          if (dst == nullptr) {
            dst = &store.getOrCreate(coord);
            // `getOrCreate` unshares a copy-on-write tile, so `srcTile` would
            // go on showing the pre-write texels -- and this route, like the
            // RGB smudge, reads what it has just written on the next dab.
            srcTile = dst;
            ++count.tiles;
            if (touchedOut != nullptr) touchedOut->push_back(coord);
          }
          dst->writeTexel(local, step.texel);
          ++count.texels;
        }
      }
    }
  }
  return count;
}

StrokeDeposit PigmentSmudgeStroke::smudgeDabs(PigmentTileStore& store, const BrushTip& tip,
                                              const std::vector<Vec2>& dabs, int32_t canvasW,
                                              int32_t canvasH, const Selection* selection) {
  StrokeDeposit out;
  for (const Vec2& dab : dabs) {
    const DepositCount c = smudgeDab(store, tip, dab, canvasW, canvasH, selection, &out.tiles);
    out.texels += c.texels;
    ++out.dabs;
  }
  sortUniqueTiles(out.tiles);
  return out;
}

}  // namespace np
