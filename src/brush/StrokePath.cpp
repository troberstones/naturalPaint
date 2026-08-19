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

// Reflects `through` across `about`: 2*about - through. Used to extrapolate
// a control point that doesn't exist yet -- before the first real sample of
// a stroke, or after the last one -- from the two real points nearest it.
// For collinear input this makes the extrapolated point collinear too, so
// the curve degenerates to a straight line exactly where there is no real
// curvature information to do anything smarter with.
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

}  // namespace

void StrokePath::reset() {
  numPts_ = 0;
  leftover_ = 0.0f;
}

void StrokePath::emitAlongSegment(Vec2 P0, Vec2 P1, Vec2 P2, Vec2 P3,
                                  float spacingPx, std::vector<Vec2>& out) {
  // Piecewise-linear walk of the curve: fine enough subdivision that the
  // chord error is negligible next to any spacing a brush will realistically
  // use, cheap enough that doing it every render frame is a non-issue --
  // this is CPU-side geometry, no GPU dispatch involved.
  constexpr int kSubdiv = 24;
  const float safeSpacing = std::max(spacingPx, 0.1f);

  Vec2 prev = evalCentripetalCatmullRom(P0, P1, P2, P3, 0.0f);
  for (int i = 1; i <= kSubdiv; ++i) {
    const float u = static_cast<float>(i) / static_cast<float>(kSubdiv);
    const Vec2 cur = evalCentripetalCatmullRom(P0, P1, P2, P3, u);
    const float dx = cur.x - prev.x, dy = cur.y - prev.y;
    const float edgeLen = std::sqrt(dx * dx + dy * dy);

    if (edgeLen > 1e-8f) {
      float walked = 0.0f;  // distance already consumed along this edge
      while (leftover_ + (edgeLen - walked) >= safeSpacing) {
        const float needed = safeSpacing - leftover_;
        walked += needed;
        const float t = walked / edgeLen;
        out.push_back(Vec2{prev.x + dx * t, prev.y + dy * t});
        leftover_ = 0.0f;
      }
      leftover_ += (edgeLen - walked);
    }
    prev = cur;
  }
}

void StrokePath::addPoint(float x, float y, float spacingPx, std::vector<Vec2>& out) {
  const Vec2 p{x, y};
  if (numPts_ < 4) {
    pts_[numPts_++] = p;
  } else {
    pts_[0] = pts_[1]; pts_[1] = pts_[2]; pts_[2] = pts_[3]; pts_[3] = p;
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
  const Vec2 P1 = pts_[p1];
  const Vec2 P2 = pts_[p2];
  const Vec2 P0 = (p1 > 0) ? pts_[p1 - 1] : mirror(P1, P2);
  const Vec2 P3 = pts_[numPts_ - 1];  // always real: the point just added
  emitAlongSegment(P0, P1, P2, P3, spacingPx, out);
}

void StrokePath::flush(float spacingPx, std::vector<Vec2>& out) {
  if (numPts_ < 2) { numPts_ = 0; leftover_ = 0.0f; return; }

  // The segment addPoint() never got to: between the last two real samples,
  // with no real point beyond them to confirm its shape, so the far control
  // point is extrapolated the same way the very start of a stroke
  // extrapolates the one before it.
  const Vec2 P2 = pts_[numPts_ - 1];
  const Vec2 P1 = pts_[numPts_ - 2];
  const Vec2 P0 = (numPts_ >= 3) ? pts_[numPts_ - 3] : mirror(P1, P2);
  const Vec2 P3 = mirror(P2, P1);
  emitAlongSegment(P0, P1, P2, P3, spacingPx, out);

  numPts_ = 0;
  leftover_ = 0.0f;
}

}  // namespace np
