#include "brush/StrokePath.hpp"

#include <algorithm>
#include <cmath>

namespace np {
namespace {

float distanceOf(Vec2 a, Vec2 b) {
  const float dx = b.x - a.x, dy = b.y - a.y;
  return std::sqrt(dx * dx + dy * dy);
}

Vec2 lerp(Vec2 a, Vec2 b, float t) {
  return Vec2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

// The scalar form -- axis interpolation for pressure and tilt uses this
// rather than `Vec2 lerp()` above, which would need a fabricated second
// coordinate for one number.
float lerp(float a, float b, float t) { return a + (b - a) * t; }

// Shortest-arc interpolation for the two axes that are ANGLES normalised to
// [0,1] with 0 and 1 the same orientation (app/PenAxes.hpp): azimuth (a full
// turn anticlockwise from +x, in [0,1)) and barrel rotation
// (`penBarrelNormalised()`, SDL's [-180,180] degrees -> [0,1], so the seam is
// at +-180 degrees and both 0 and 1 are reachable). A plain `lerp()` across
// that seam goes the long way round: a pen turning 4 degrees through
// +-180 (barrel 0.9986 -> 0.0097) swept every in-between dab through 0.5 --
// half a turn of Angle offset -- which is the wave-1 review's finding 3.
//
// `d -= round(d)` folds the difference into [-0.5, 0.5]: the short way. For
// every pair less than half a turn apart without crossing the seam
// `round(d) == 0` and this is `a + (b - a) * t` exactly, the same float
// operations `lerp()` performs, so every such dab is bit-identical to what
// the linear form produced. The result is folded back into [0,1] only when it
// leaves it -- NOT `v -= floor(v)`, which would also turn an input of exactly
// 1.0 (a barrel held at +180 degrees, a value `penBarrelNormalised()` really
// returns) into 0.0 at every dab between two such samples: the same
// orientation, but the other end of every Control curve's domain.
float lerpAngle01(float a, float b, float t) {
  float d = b - a;
  d -= std::round(d);
  float v = a + d * t;
  if (v < 0.0f) v += 1.0f;
  else if (v > 1.0f) v -= 1.0f;
  return v;
}

// Reflects `through` across `about`: 2*about - through. Used to extrapolate
// a control point that doesn't exist yet -- before the first real sample of
// a stroke, or after the last one -- from the two real points nearest it.
// For collinear input this makes the extrapolated point collinear too, so
// the curve degenerates to a straight line exactly where there is no real
// curvature information to do anything smarter with.
//
// Position only: an extrapolated `StrokeSample`'s axes are never read (only
// P1/P2 -- always real samples -- feed `emitAlongSegment()`'s axis
// interpolation), so there is nothing to extrapolate there and no meaning to
// invent for it.
Vec2 mirror(Vec2 about, Vec2 through) {
  return Vec2{2.0f * about.x - through.x, 2.0f * about.y - through.y};
}

// Barry & Goldman's recursive evaluation of a centripetal (alpha = 0.5)
// Catmull-Rom spline segment, evaluated at u in [0,1] across the P1->P2
// span. Standard construction; see e.g. Yuksel, Schaefer & Keyser 2011.
Vec2 evalCentripetalCatmullRom(Vec2 P0, Vec2 P1, Vec2 P2, Vec2 P3, float u) {
  auto knotStep = [](Vec2 a, Vec2 b) {
    // Centripetal: knot spacing is distance^0.5. Floored so a held-still
    // brush feeding the same point repeatedly -- or any other coincident
    // control-point pair -- can never produce a zero-width knot interval and
    // divide by zero in `blend` below.
    return std::max(std::sqrt(distanceOf(a, b)), 1e-3f);
  };
  const float t0 = 0.0f;
  const float t1 = t0 + knotStep(P0, P1);
  const float t2 = t1 + knotStep(P1, P2);
  const float t3 = t2 + knotStep(P2, P3);
  const float t = t1 + u * (t2 - t1);

  auto blend = [](Vec2 A, Vec2 B, float ta, float tb, float tt) {
    const float denom = tb - ta;
    const float w = (std::fabs(denom) > 1e-6f) ? (tt - ta) / denom : 0.0f;
    return lerp(A, B, w);
  };

  const Vec2 A1 = blend(P0, P1, t0, t1, t);
  const Vec2 A2 = blend(P1, P2, t1, t2, t);
  const Vec2 A3 = blend(P2, P3, t2, t3, t);
  const Vec2 B1 = blend(A1, A2, t0, t2, t);
  const Vec2 B2 = blend(A2, A3, t1, t3, t);
  return blend(B1, B2, t1, t2, t);
}

// How far a stroke's raw samples may wander and still count as "never moved"
// for the single-click dab in flush(). This is a FLOAT-NOISE tolerance, not a
// tremor budget: a thousandth of a texel is below anything a pointer, a pen or
// the view transform can produce, so no stroke that a user would call a drag
// can ever be reclassified as a click by it.
//
// The rejected alternative was a real slop budget -- a texel or two, to catch
// the pen that wobbles slightly during a click. That would have made a
// deliberate short drag under the slop distance emit a dab it does not emit
// today, i.e. it would have changed moving strokes, which is exactly what this
// change is not allowed to do. The wobbly-pen click therefore still deposits
// nothing; that gap is real and is left open on purpose.
constexpr float kStationaryPx = 1e-3f;

}  // namespace

void StrokePath::reset() {
  numPts_ = 0;
  leftover_ = 0.0f;
  movedPx_ = 0.0f;
}

void StrokePath::emitAlongSegment(const StrokeSample& S0, const StrokeSample& S1,
                                  const StrokeSample& S2, const StrokeSample& S3,
                                  float spacingPx, std::vector<StrokeDab>& out) {
  // Piecewise-linear walk of the curve: fine enough subdivision that the
  // chord error is negligible next to any spacing a brush will realistically
  // use, cheap enough that doing it every render frame is a non-issue --
  // this is CPU-side geometry, no GPU dispatch involved.
  constexpr int kSubdiv = 24;
  const float safeSpacing = std::max(spacingPx, 0.1f);

  Vec2 prev = evalCentripetalCatmullRom(S0.pos, S1.pos, S2.pos, S3.pos, 0.0f);
  // The curve parameter `prev` was evaluated at -- tracked alongside `prev`
  // so a dab landing partway along the [prev,cur] chord can interpolate its
  // axes at the matching fraction of [uPrev,u], not just its position along
  // the chord (this header's own comment on why `u` and not an arc-length
  // fraction). Starts at 0.0, matching `prev`'s own first evaluation.
  float uPrev = 0.0f;
  for (int i = 1; i <= kSubdiv; ++i) {
    const float u = static_cast<float>(i) / static_cast<float>(kSubdiv);
    const Vec2 cur = evalCentripetalCatmullRom(S0.pos, S1.pos, S2.pos, S3.pos, u);
    const float dx = cur.x - prev.x, dy = cur.y - prev.y;
    const float edgeLen = std::sqrt(dx * dx + dy * dy);

    if (edgeLen > 1e-8f) {
      float walked = 0.0f;  // distance already consumed along this edge
      while (leftover_ + (edgeLen - walked) >= safeSpacing) {
        const float needed = safeSpacing - leftover_;
        walked += needed;
        const float t = walked / edgeLen;
        // The curve parameter this dab actually lands at: linear between
        // this chord's own endpoints, `uPrev` and `u`, by the same fraction
        // `t` that places its position between `prev` and `cur`. Axes are
        // then interpolated between S1 and S2 -- the two REAL samples this
        // whole segment spans -- at that parameter, exactly this header's
        // documented "linear in u across the P1->P2 span" rule -- linear
        // for pressure and tilt, along the SHORT arc for the two angle axes
        // (`lerpAngle01()`'s comment); S0/S3's own axes are never read here,
        // extrapolated points as they may be.
        const float uAt = uPrev + t * (u - uPrev);
        out.push_back(StrokeDab{Vec2{prev.x + dx * t, prev.y + dy * t},
                                lerp(S1.pressure, S2.pressure, uAt),
                                lerp(S1.tilt, S2.tilt, uAt),
                                lerpAngle01(S1.azimuth, S2.azimuth, uAt),
                                lerpAngle01(S1.barrel, S2.barrel, uAt)});
        leftover_ = 0.0f;
      }
      leftover_ += (edgeLen - walked);
    }
    prev = cur;
    uPrev = u;
  }
}

void StrokePath::addPoint(const StrokeSample& sample, float spacingPx,
                          std::vector<StrokeDab>& out) {
  // Measured against the point this one displaces as newest, before the
  // shift below overwrites it. A stroke's whole travel is accumulated here
  // because flush() needs the answer for the stroke as a WHOLE, and pts_ only
  // remembers the last four samples.
  if (numPts_ > 0) movedPx_ += distanceOf(pts_[numPts_ - 1].pos, sample.pos);
  if (numPts_ < 4) {
    pts_[numPts_++] = sample;
  } else {
    pts_[0] = pts_[1]; pts_[1] = pts_[2]; pts_[2] = pts_[3]; pts_[3] = sample;
  }

  // Three real samples are needed before the *first* segment (points 1->2)
  // has enough context to walk: two to define it, a third to stand in for
  // the "future" point a 4-point Catmull-Rom wants. This is why the emitter
  // lags one sample behind the newest point rather than extrapolating a
  // synthetic future point on every call (the more common shortcut, but a
  // less faithful reading of "through the last four sampled points" -- see
  // flush() for where the always-extrapolated tail actually belongs: the
  // genuine end of the stroke, where there truly is no future sample).
  if (numPts_ < 3) return;

  const int p1 = numPts_ - 3;
  const int p2 = numPts_ - 2;
  const StrokeSample& S1 = pts_[p1];
  const StrokeSample& S2 = pts_[p2];
  const StrokeSample S0 = (p1 > 0) ? pts_[p1 - 1] : StrokeSample{mirror(S1.pos, S2.pos)};
  const StrokeSample& S3 = pts_[numPts_ - 1];  // always real: the point just added
  emitAlongSegment(S0, S1, S2, S3, spacingPx, out);
}

void StrokePath::flush(float spacingPx, std::vector<StrokeDab>& out) {
  // --- The single click: a stroke whose samples never left one spot. ---
  //
  // Nothing below can emit for it. The arc-length walk divides a TRAVELLED
  // distance by `spacingPx`, and this stroke travelled none: a click that
  // lasted one frame never even reaches the walk (`numPts_ < 3` in addPoint(),
  // and the guard below used to be `numPts_ < 2`), and a click held down for
  // several frames feeds several coincident samples whose curve has zero
  // length and so emits nothing either. Both are the same user gesture and
  // both used to paint nothing at all -- the reported defect.
  //
  // It is deliberately keyed on DISTANCE and not on the sample count. Keying
  // on `numPts_ == 1` looks like the smaller change and does not fix the bug:
  // both live routes in ui/MacPaintUI.cpp call addPoint() once per render
  // frame for as long as the button is held, without a moved-since-last-frame
  // guard, so a physical click of even 50 ms arrives as several identical
  // samples and would still have fallen through to the walk below.
  //
  // Moving strokes are untouched by this branch, by construction: any stroke
  // with real travel fails the test and takes exactly the path it took before.
  if (numPts_ > 0 && movedPx_ <= kStationaryPx) {
    // Its own position AND its own sampled axes -- not the origin's, not an
    // interpolation of anything, since a stationary stroke's every real
    // sample sits at this position, and the newest one is what the pen was
    // actually reporting the instant the click resolved (a rising or
    // wobbling pressure held over several coincident samples reads its LAST
    // value, matching every other "latest wins" reading this stroke's own
    // hardware axes had before the sample queue existed).
    out.push_back(pts_[numPts_ - 1]);
    numPts_ = 0; leftover_ = 0.0f; movedPx_ = 0.0f;
    return;
  }

  // Only a stroke with ZERO samples can still be here: one sample means
  // `movedPx_ == 0` exactly, which the branch above already took.
  if (numPts_ < 2) { numPts_ = 0; leftover_ = 0.0f; movedPx_ = 0.0f; return; }

  // NOT DONE HERE, deliberately: a moving stroke still never stamps its own
  // origin texel. `leftover_` starts at 0, so the first dab of a drag lands a
  // full `spacingPx` along the path rather than at the first sample. Seeding
  // `leftover_ = spacingPx` in reset() would fix that and make every stroke
  // stamp its start -- but it also shifts every dab of every existing stroke,
  // which is a change to drawing this one is not: the brief here was "single
  // click draws dab, moving stroke will do what it currently does". Recorded
  // so whoever weighs that change later starts from a measured fact.

  // The segment addPoint() never got to: between the last two real samples,
  // with no real point beyond them to confirm its shape, so the far control
  // point is extrapolated the same way the very start of a stroke
  // extrapolates the one before it.
  const StrokeSample& S2 = pts_[numPts_ - 1];
  const StrokeSample& S1 = pts_[numPts_ - 2];
  const StrokeSample S0 = (numPts_ >= 3) ? pts_[numPts_ - 3] : StrokeSample{mirror(S1.pos, S2.pos)};
  const StrokeSample S3{mirror(S2.pos, S1.pos)};
  emitAlongSegment(S0, S1, S2, S3, spacingPx, out);

  numPts_ = 0;
  leftover_ = 0.0f;
  movedPx_ = 0.0f;
}

// --- back-compatible overloads: position only, neutral axes ---------------

void StrokePath::addPoint(float x, float y, float spacingPx, std::vector<Vec2>& out) {
  std::vector<StrokeDab> dabs;
  addPoint(StrokeSample{Vec2{x, y}}, spacingPx, dabs);
  out.reserve(out.size() + dabs.size());
  for (const StrokeDab& d : dabs) out.push_back(d.pos);
}

void StrokePath::flush(float spacingPx, std::vector<Vec2>& out) {
  std::vector<StrokeDab> dabs;
  flush(spacingPx, dabs);
  out.reserve(out.size() + dabs.size());
  for (const StrokeDab& d : dabs) out.push_back(d.pos);
}

}  // namespace np
