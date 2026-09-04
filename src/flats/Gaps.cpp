#include "flats/Gaps.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "flats/Morphology.hpp"

namespace np {

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

void boxBlur(std::vector<float>& v, int w, int h, int r) {
  std::vector<float> tmp(v.size());
  const float n = static_cast<float>(2 * r + 1);
  for (int y = 0; y < h; y++) {
    const size_t row = static_cast<size_t>(y) * w;
    float s = 0;
    for (int x = -r; x <= r; x++) s += v[row + std::min(w - 1, std::max(0, x))];
    for (int x = 0; x < w; x++) {
      tmp[row + x] = s / n;
      s += v[row + std::min(w - 1, x + r + 1)] - v[row + std::max(0, x - r)];
    }
  }
  for (int x = 0; x < w; x++) {
    float s = 0;
    for (int y = -r; y <= r; y++) s += tmp[static_cast<size_t>(std::min(h - 1, std::max(0, y))) * w + x];
    for (int y = 0; y < h; y++) {
      v[static_cast<size_t>(y) * w + x] = s / n;
      s += tmp[static_cast<size_t>(std::min(h - 1, y + r + 1)) * w + x] -
           tmp[static_cast<size_t>(std::max(0, y - r)) * w + x];
    }
  }
}

int iround(double v) { return static_cast<int>(std::lround(v)); }

}  // namespace

// ---------------------------------------------------------------- orientation

FlatOrientation flatOrientationField(const FlatInk& ink, int w, int h) {
  FlatOrientation f;
  f.w2 = std::max(2, w >> 2);
  f.h2 = std::max(2, h >> 2);
  const size_t m = static_cast<size_t>(f.w2) * f.h2;
  std::vector<float> a(m, 0.f);
  for (int y2 = 0; y2 < f.h2; y2++) {
    for (int x2 = 0; x2 < f.w2; x2++) {
      float s = 0;
      const int bx = x2 * 4, by = y2 * 4;
      for (int dy = 0; dy < 4; dy++) {
        const size_t y = static_cast<size_t>(std::min(h - 1, by + dy)) * w;
        for (int dx = 0; dx < 4; dx++) s += ink[y + std::min(w - 1, bx + dx)];
      }
      a[static_cast<size_t>(y2) * f.w2 + x2] = s / 16;
    }
  }
  std::vector<float> jxx(m, 0.f), jxy(m, 0.f), jyy(m, 0.f);
  for (int y = 1; y < f.h2 - 1; y++) {
    for (int x = 1; x < f.w2 - 1; x++) {
      const size_t i = static_cast<size_t>(y) * f.w2 + x;
      const float gx = (a[i + 1] - a[i - 1]) / 2;
      const float gy = (a[i + f.w2] - a[i - f.w2]) / 2;
      jxx[i] = gx * gx;
      jxy[i] = gx * gy;
      jyy[i] = gy * gy;
    }
  }
  for (std::vector<float>* j : {&jxx, &jxy, &jyy}) {
    boxBlur(*j, f.w2, f.h2, 2);
    boxBlur(*j, f.w2, f.h2, 2);
  }
  f.fx.assign(m, 0.f);
  f.fy.assign(m, 0.f);
  f.coh.assign(m, 0.f);
  for (size_t i = 0; i < m; i++) {
    const float d = jxx[i] - jyy[i], tr = jxx[i] + jyy[i];
    const float ang = 0.5f * std::atan2(2 * jxy[i], d) + static_cast<float>(M_PI) / 2;  // tangent ⟂ dominant gradient
    f.fx[i] = std::cos(ang);
    f.fy[i] = std::sin(ang);
    f.coh[i] = tr > 1e-4f ? std::sqrt(d * d + 4 * jxy[i] * jxy[i]) / tr : 0.f;
  }
  return f;
}

FlatOrientationSample flatSampleOrientation(const FlatOrientation& f, int x, int y) {
  const size_t i = static_cast<size_t>(std::min(f.h2 - 1, std::max(0, y >> 2))) * f.w2 +
                   static_cast<size_t>(std::min(f.w2 - 1, std::max(0, x >> 2)));
  return {f.fx[i], f.fy[i], f.coh[i]};
}

// --------------------------------------------------------------- relatability

FlatRelatability flatRelatable(float ax, float ay, float atx, float aty, float bx, float by, float btx,
                               float bty, float coneCos, float maxBendDeg) {
  const FlatRelatability fail{false, kInf, static_cast<float>(M_PI)};
  const float rx = bx - ax, ry = by - ay;
  const float d = std::hypot(rx, ry);
  if (d < 1e-3f) return fail;
  const float ux = rx / d, uy = ry / d;
  // monotonic progress: each tip must head toward the other (no backtracking)
  const float a = atx * ux + aty * uy;      // A's tangent projected onto A->B
  const float b = -(btx * ux + bty * uy);   // B's tangent projected onto B->A
  if (a < coneCos || b < coneCos) return fail;
  // no inflection: both tips must curve to the SAME side of the chord. Opposite
  // signs => the smooth link would be an S, which is not relatable.
  const float crossA = ux * aty - uy * atx;
  const float crossB = ux * bty - uy * btx;
  if (crossA * crossB < 0) return fail;
  // total bend of the connecting arc = turn at A + turn at B
  const float thA = std::acos(std::min(1.f, a));
  const float thB = std::acos(std::min(1.f, b));
  const float bend = thA + thB;
  if (bend > maxBendDeg * static_cast<float>(M_PI) / 180) return fail;
  // elastica proxy (kappa ~ theta/d): length + curvature cost
  const float energy = d + kFlatElasticaBeta * (thA * thA + thA * thB + thB * thB) / d;
  return {true, energy, bend};
}

float flatElasticaEnergy(const FlatPolyline& poly, float beta) {
  const size_t n = poly.size() >> 1;
  if (n < 2) return kInf;
  float len = 0;
  for (size_t i = 1; i < n; i++)
    len += std::hypot(poly[2 * i] - poly[2 * i - 2], poly[2 * i + 1] - poly[2 * i - 1]);
  float curv = 0;
  for (size_t i = 1; i + 1 < n; i++) {
    const float ax = poly[2 * i] - poly[2 * i - 2], ay = poly[2 * i + 1] - poly[2 * i - 1];
    const float bx = poly[2 * i + 2] - poly[2 * i], by = poly[2 * i + 3] - poly[2 * i + 1];
    const float la = std::hypot(ax, ay) > 0 ? std::hypot(ax, ay) : 1.f;
    const float lb = std::hypot(bx, by) > 0 ? std::hypot(bx, by) : 1.f;
    float c = (ax * bx + ay * by) / (la * lb);
    c = c < -1 ? -1 : c > 1 ? 1 : c;
    const float th = std::acos(c);
    curv += th * th / ((la + lb) / 2);
  }
  return len + beta * curv;
}

FlatPolyline flatElasticaCurve(float ax, float ay, float atx, float aty, float bx, float by, float btx,
                               float bty, int k) {
  const float len0 = std::hypot(bx - ax, by - ay);
  const float len = len0 > 0 ? len0 : 1.f;
  if (!k) k = std::max(4, std::min(16, static_cast<int>(std::ceil(len / 2))));
  // travel-direction tangents: leave A along its outward tangent, arrive at B
  // heading A->B (the reverse of B's outward-into-gap tangent)
  const float t0x = atx, t0y = aty, t1x = -btx, t1y = -bty;
  FlatPolyline best{ax, ay, bx, by};
  float bestE = len;
  for (const float hh : {0.4f, 0.6f, 0.8f, 1.0f, 1.2f}) {
    const float m = hh * len;
    FlatPolyline pts;
    pts.reserve(static_cast<size_t>(k + 1) * 2);
    for (int i = 0; i <= k; i++) {
      const float t = static_cast<float>(i) / k, t2 = t * t, t3 = t2 * t;
      const float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2;
      pts.push_back(h00 * ax + h10 * m * t0x + h01 * bx + h11 * m * t1x);
      pts.push_back(h00 * ay + h10 * m * t0y + h01 * by + h11 * m * t1y);
    }
    const float e = flatElasticaEnergy(pts);
    if (e < bestE) {
      bestE = e;
      best = std::move(pts);
    }
  }
  // reject a wild bow: keep the curve only if it stays near the chord's cost
  return bestE > 1.5f * len ? FlatPolyline{ax, ay, bx, by} : best;
}

// --------------------------------------------------------------------- fronts

namespace {

int32_t localWidth(const FlatDist& wd, int32_t i, int w, int h) {
  const int x = i % w, y = i / w;
  int32_t mx = 0;
  for (int dy = -3; dy <= 3; dy++) {
    const int yy = y + dy;
    if (yy < 0 || yy >= h) continue;
    for (int dx = -3; dx <= 3; dx++) {
      const int xx = x + dx;
      if (xx < 0 || xx >= w) continue;
      mx = std::max(mx, wd[static_cast<size_t>(yy) * w + xx]);
    }
  }
  return mx;
}

// Build a bridge through open point (tx,ty): nearest line pixel on one side,
// then march the opposite way to the line pixel on the other side.
bool bridgeAt(int tx, int ty, const FlatMask& line, int w, int h, const FlatOrientation& orient,
              int maxBridge, const FlatDist& wd, float out[4]) {
  int32_t l1 = -1;
  int d1 = 1000000000;
  const int R = std::min(maxBridge, 40);
  for (int dy = -R; dy <= R; dy++) {
    const int y = ty + dy;
    if (y < 0 || y >= h) continue;
    for (int dx = -R; dx <= R; dx++) {
      const int x = tx + dx;
      if (x < 0 || x >= w || !line[static_cast<size_t>(y) * w + x]) continue;
      const int d = dx * dx + dy * dy;
      if (d < d1) { d1 = d; l1 = y * w + x; }
    }
  }
  if (l1 < 0) return false;
  const int x1 = l1 % w, y1 = l1 / w;
  const float len1raw = std::hypot(static_cast<float>(tx - x1), static_cast<float>(ty - y1));
  const float len1 = len1raw > 0 ? len1raw : 1.f;
  const float vx = (tx - x1) / len1, vy = (ty - y1) / len1;
  for (int s = 1; s <= maxBridge; s++) {
    const int x = iround(tx + vx * s), y = iround(ty + vy * s);
    if (x < 0 || y < 0 || x >= w || y >= h) return false;
    if (!line[static_cast<size_t>(y) * w + x]) continue;
    // similarity: don't bridge a thick contour to thin hatching.
    // Anchors sit on stroke EDGES where wd≈1px, so sample the local max
    // (the stroke's half-width) in a 7x7 window.
    const int32_t w1 = localWidth(wd, l1, w, h), w2 = localWidth(wd, y * w + x, w, h);
    if (std::max(w1, w2) > 2.5 * std::min(w1, w2) + 3) return false;
    // flow check: the bridge should run along stroke tangents at its anchors
    const float blraw = std::hypot(static_cast<float>(x - x1), static_cast<float>(y - y1));
    const float bl = blraw > 0 ? blraw : 1.f;
    const float bx = (x - x1) / bl, by = (y - y1) / bl;
    const int anchors[2][2] = {{x1, y1}, {x, y}};
    for (const auto& a : anchors) {
      const FlatOrientationSample o = flatSampleOrientation(orient, a[0], a[1]);
      if (o.coh > 0.25f && std::fabs(bx * o.fx + by * o.fy) < 0.6f) return false;
    }
    out[0] = static_cast<float>(x1);
    out[1] = static_cast<float>(y1);
    out[2] = static_cast<float>(x);
    out[3] = static_cast<float>(y);
    return true;
  }
  return false;
}

// Rank a throat bridge by elastica energy using stroke orientation as the tip
// tangents. When both anchors have coherent orientation, also enforce
// relatability; reject if it fails. Falls back to the raw throat distance
// when the field is too incoherent to judge continuation.
float bridgeEnergy(const float seg[4], const FlatOrientation& orient, float fallback) {
  const float x1 = seg[0], y1 = seg[1], x2 = seg[2], y2 = seg[3];
  const float Lraw = std::hypot(x2 - x1, y2 - y1);
  const float L = Lraw > 0 ? Lraw : 1.f;
  const float ux = (x2 - x1) / L, uy = (y2 - y1) / L;
  const FlatOrientationSample f1 = flatSampleOrientation(orient, iround(x1), iround(y1));
  const FlatOrientationSample f2 = flatSampleOrientation(orient, iround(x2), iround(y2));
  if (f1.coh <= 0.25f || f2.coh <= 0.25f) return fallback;
  // orient each axial vector to point out of its tip, into the gap
  const float s1 = f1.fx * ux + f1.fy * uy >= 0 ? 1.f : -1.f;
  const float s2 = f2.fx * ux + f2.fy * uy <= 0 ? 1.f : -1.f;
  const FlatRelatability rel = flatRelatable(x1, y1, s1 * f1.fx, s1 * f1.fy, x2, y2, s2 * f2.fx, s2 * f2.fy);
  return rel.ok ? rel.energy : kInf;
}

}  // namespace

FlatFrontsResult flatAnalyzeFronts(const FlatLabels& labels, const FlatMask& line, int w, int h,
                                   const FlatOrientation& orient, int maxBridge,
                                   const std::vector<uint8_t>* isBg, bool doMerge) {
  const FlatDist ld = flatDistanceTransform(line, w, h);
  // stroke half-width at line pixels = distance to free space (similarity cue)
  const FlatDist wd = flatDistanceTransform(flatInvertMask(line), w, h);
  constexpr int32_t OPEN = 7;  // > ~2.3 px from any line = open space
  constexpr int64_t KEY = 1 << 20;

  struct P {
    int32_t a, b, T = 0, O = 0;
    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    int tx = 0, ty = 0;
    int32_t tmin = 1000000000;
  };
  std::unordered_map<int64_t, P> pairs;
  std::vector<int64_t> order;
  auto touch = [&](int32_t a, int32_t b, int x, int y, bool open, int32_t d) {
    if (a > b) std::swap(a, b);
    const int64_t k = static_cast<int64_t>(a) * KEY + b;
    auto it = pairs.find(k);
    if (it == pairs.end()) {
      it = pairs.emplace(k, P{a, b}).first;
      order.push_back(k);
    }
    P& p = it->second;
    p.T++;
    if (open) {
      p.O++;
      p.sx += x; p.sy += y; p.sxx += static_cast<double>(x) * x; p.syy += static_cast<double>(y) * y;
      p.sxy += static_cast<double>(x) * y;
      if (d < p.tmin) { p.tmin = d; p.tx = x; p.ty = y; }
    }
  };
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const size_t i = static_cast<size_t>(y) * w + x;
      const int32_t a = labels[i];
      if (x < w - 1) {
        const int32_t b = labels[i + 1];
        if (a != b) touch(a, b, x, y, ld[i] > OPEN && ld[i + 1] > OPEN, ld[i]);
      }
      if (y < h - 1) {
        const int32_t b = labels[i + w];
        if (a != b) touch(a, b, x, y, ld[i] > OPEN && ld[i + w] > OPEN, ld[i]);
      }
    }
  }

  FlatFrontsResult out;
  std::vector<std::pair<float, std::array<float, 4>>> cands;
  for (const int64_t k : order) {
    const P& p = pairs[k];
    if (!p.O) continue;
    // front axis (PCA of open boundary points) vs local stroke orientation
    bool aligned = false;
    if (p.O >= 6) {
      const double mx = p.sx / p.O, my = p.sy / p.O;
      const double cxx = p.sxx / p.O - mx * mx, cyy = p.syy / p.O - my * my, cxy = p.sxy / p.O - mx * my;
      const double phi = 0.5 * std::atan2(2 * cxy, cxx - cyy);
      const double ax = std::cos(phi), ay = std::sin(phi);
      const FlatOrientationSample o = flatSampleOrientation(orient, iround(mx), iround(my));
      aligned = o.coh > 0.25f && std::fabs(ax * o.fx + ay * o.fy) > 0.82;
    }
    const double openFrac = static_cast<double>(p.O) / p.T;
    const bool bgPair = isBg && ((static_cast<size_t>(p.a) < isBg->size() && (*isBg)[p.a]) ||
                                 (static_cast<size_t>(p.b) < isBg->size() && (*isBg)[p.b]));
    if (doMerge && !bgPair && openFrac > 0.6 && p.O >= 12 && !aligned) {
      out.merges.emplace_back(p.a, p.b);
      continue;
    }
    // suggest a bridge across the narrowest open point
    std::array<float, 4> seg{};
    if (bridgeAt(p.tx, p.ty, line, w, h, orient, maxBridge, wd, seg.data())) {
      const float e = bridgeEnergy(seg.data(), orient, static_cast<float>(p.tmin));
      if (e < kInf) cands.emplace_back(e, seg);
    }
  }
  std::stable_sort(cands.begin(), cands.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  const size_t take = std::min<size_t>(150, cands.size());
  for (size_t i = 0; i < take; i++)
    out.segs.insert(out.segs.end(), cands[i].second.begin(), cands[i].second.end());
  return out;
}

// ------------------------------------------------------------------ endpoints

namespace {

struct Ep {
  int32_t i;
  int x, y;
  std::unordered_set<int32_t> branch;
  float dx, dy;
};

int skNbrCount(const FlatMask& sk, int32_t i, int w) {
  return sk[i - w - 1] + sk[i - w] + sk[i - w + 1] + sk[i - 1] + sk[i + 1] + sk[i + w - 1] + sk[i + w] + sk[i + w + 1];
}

// A spur is a short noise nub: walking from the endpoint along the skeleton
// hits a junction (>=2 onward paths) within a few steps. Real stroke tips
// have long branches.
bool isSpur(const FlatMask& sk, int32_t i, int w, int K = 6) {
  int32_t prev = -1, cur = i;
  const int nbr[8] = {-w - 1, -w, -w + 1, -1, 1, w - 1, w, w + 1};
  for (int s = 0; s < K; s++) {
    int count = 0;
    int32_t first = -1;
    for (const int d : nbr) {
      const int32_t q = cur + d;
      if (!sk[q] || q == prev) continue;
      if (prev >= 0 && std::abs(q % w - prev % w) <= 1 && std::abs(q / w - prev / w) <= 1)
        continue;  // 8-adjacent to prev: same path, not a fork
      if (first < 0) first = q;
      count++;
    }
    if (count == 0) return false;  // dead end: genuine tip (or isolated dot)
    if (count >= 2) return true;   // junction close to the tip: spur
    prev = cur;
    cur = first;
  }
  return false;
}

std::unordered_set<int32_t> walkBranch(const FlatMask& sk, int32_t start, int w, int maxSteps) {
  std::unordered_set<int32_t> seen{start};
  std::vector<int32_t> frontier{start}, nf;
  const int nbr[8] = {-w - 1, -w, -w + 1, -1, 1, w - 1, w, w + 1};
  for (int s = 0; s < maxSteps && !frontier.empty(); s++) {
    nf.clear();
    for (const int32_t p : frontier) {
      for (const int d : nbr) {
        const int32_t q = p + d;
        if (sk[q] && seen.insert(q).second) nf.push_back(q);
      }
    }
    frontier.swap(nf);
  }
  return seen;
}

struct Endpoints {
  std::vector<Ep> info;
  FlatDist wd;
  FlatMask sk;
};

// Skeleton stroke tips with outward unit tangents (dx,dy point out of the tip,
// into the gap). keepSpurs: a tip whose branch forks a few steps in is
// normally dropped as skeleton noise, but at a real break the OTHER side of
// the gap is often close enough to fork the skeleton right behind the tip --
// so the filter throws away one tip of a genuine pair. flatSuggestGaps keeps
// the filter (it also pairs tip->stroke, so it survives losing one side); the
// tight-closure pass cannot, and compensates with its own stricter tests.
Endpoints extractEndpoints(const FlatMask& line, int w, int h, int maxBridge, bool keepSpurs = false) {
  Endpoints e;
  e.wd = flatDistanceTransform(flatInvertMask(line), w, h);
  e.sk = flatSkeletonize(line, w, h);
  std::vector<int32_t> eps;
  for (size_t i = 0; i < e.sk.size(); i++) {
    if (e.sk[i] && skNbrCount(e.sk, static_cast<int32_t>(i), w) <= 1 &&
        (keepSpurs || !isSpur(e.sk, static_cast<int32_t>(i), w)))
      eps.push_back(static_cast<int32_t>(i));
  }
  if (eps.size() > 4000) eps.resize(4000);
  e.info.reserve(eps.size());
  for (const int32_t i : eps) {
    Ep ep;
    ep.i = i;
    ep.branch = walkBranch(e.sk, i, w, maxBridge * 2);
    // JS: `[...branch]` is insertion order of a Set; the sixth entry of a BFS
    // insertion order. Reproduce by re-walking to the same order.
    std::vector<int32_t> arr;
    {
      std::unordered_set<int32_t> seen{i};
      std::vector<int32_t> frontier{i}, nf;
      arr.push_back(i);
      const int nbr[8] = {-w - 1, -w, -w + 1, -1, 1, w - 1, w, w + 1};
      for (int s = 0; s < maxBridge * 2 && !frontier.empty() && arr.size() < 7; s++) {
        nf.clear();
        for (const int32_t p : frontier) {
          for (const int d : nbr) {
            const int32_t q = p + d;
            if (e.sk[q] && seen.insert(q).second) { nf.push_back(q); arr.push_back(q); }
          }
        }
        frontier.swap(nf);
      }
    }
    const int32_t back = arr[std::min<size_t>(arr.size() - 1, 6)];
    ep.x = i % w;
    ep.y = i / w;
    float dx = static_cast<float>(ep.x - back % w), dy = static_cast<float>(ep.y - back / w);
    const float n0 = std::hypot(dx, dy);
    const float n = n0 > 0 ? n0 : 1.f;
    ep.dx = dx / n;
    ep.dy = dy / n;
    e.info.push_back(std::move(ep));
  }
  return e;
}

int32_t nearestForeignLine(const FlatMask& line, const Ep& A, int w, int h, int maxR) {
  for (int r = 3; r <= maxR; r++) {
    for (int dy = -r; dy <= r; dy++) {
      const int y = A.y + dy;
      if (y < 1 || y >= h - 1) continue;
      const int step = std::abs(dy) == r ? 1 : 2 * r;
      for (int dx = -r; dx <= r; dx += step) {
        const int x = A.x + dx;
        if (x < 1 || x >= w - 1) continue;
        const int32_t i = y * w + x;
        if (!line[i]) continue;
        if (A.dx * dx + A.dy * dy < 0.7f * r) continue;  // must continue the stroke direction (±45°)
        bool own = false;  // skip pixels of our own stroke (near its skeleton branch)
        for (int oy = -3; oy <= 3 && !own; oy++)
          for (int ox = -3; ox <= 3; ox++)
            if (A.branch.count(i + oy * w + ox)) { own = true; break; }
        if (!own) return i;
      }
    }
  }
  return -1;
}

// Virtual-bridge test: keep a suggestion only if actually drawing the bridge
// would locally split one fill region into two meaningfully-sized parts.
FlatSegs filterLeaky(const FlatSegs& segs, const FlatLabels& labels, const FlatMask& line, int w, int h) {
  constexpr int BOX = 96;
  const int side = 2 * BOX + 1;
  std::vector<uint8_t> visited(static_cast<size_t>(side) * side);
  std::vector<int32_t> queue(static_cast<size_t>(side) * side);
  FlatSegs out;
  for (size_t i = 0; i + 3 < segs.size(); i += 4) {
    const float x1 = segs[i], y1 = segs[i + 1], x2 = segs[i + 2], y2 = segs[i + 3];
    const int cx = iround((x1 + x2) / 2), cy = iround((y1 + y2) / 2);
    const int bx = cx - BOX, by = cy - BOX;
    std::fill(visited.begin(), visited.end(), 0);
    const float len0 = std::hypot(x2 - x1, y2 - y1);
    const float len = len0 > 0 ? len0 : 1.f;
    const int steps = static_cast<int>(std::ceil(len));
    for (int s = 0; s <= steps; s++) {
      const int px = iround(x1 + (x2 - x1) * s / steps), py = iround(y1 + (y2 - y1) * s / steps);
      for (int oy = -1; oy <= 1; oy++)
        for (int ox = -1; ox <= 1; ox++) {
          const int lx = px + ox - bx, ly = py + oy - by;
          if (lx >= 0 && ly >= 0 && lx < side && ly < side) visited[static_cast<size_t>(ly) * side + lx] = 1;
        }
    }
    const float nx = -(y2 - y1) / len, ny = (x2 - x1) / len;
    auto seed = [&](int sign) -> int32_t {
      for (const int d : {2, 3, 4, 6}) {
        const int px = iround(cx + nx * d * sign), py = iround(cy + ny * d * sign);
        if (px < 1 || py < 1 || px >= w - 1 || py >= h - 1) continue;
        const int32_t gi = py * w + px;
        const size_t li = static_cast<size_t>(py - by) * side + (px - bx);
        if (!line[gi] && labels[gi] && !visited[li]) return gi;
      }
      return -1;
    };
    const int32_t sa = seed(1), sb = seed(-1);
    if (sa < 0 || sb < 0 || labels[sa] != labels[sb]) continue;
    const int32_t region = labels[sa];
    auto flood = [&](int32_t start, uint8_t mark, int32_t other, bool& joined) -> long {
      size_t qt = 0, head = 0;
      long area = 0;
      bool big = false;
      queue[qt++] = start;
      visited[static_cast<size_t>(start / w - by) * side + (start % w - bx)] = mark;
      while (head < qt) {
        const int32_t p = queue[head++];
        area++;
        if (p == other) { joined = true; return area; }
        const int px = p % w, py = p / w;
        const int dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (const auto& dxy : dirs) {
          const int qx = px + dxy[0], qy = py + dxy[1];
          if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
          const int lx = qx - bx, ly = qy - by;
          if (lx < 0 || ly < 0 || lx >= side || ly >= side) { big = true; continue; }
          const size_t li = static_cast<size_t>(ly) * side + lx;
          if (visited[li]) continue;
          const int32_t gi = qy * w + qx;
          if (line[gi] || labels[gi] != region) continue;
          visited[li] = mark;
          queue[qt++] = gi;
        }
      }
      joined = false;
      return big ? 1000000000L : area;
    };
    bool joined = false;
    const long areaA = flood(sa, 2, sb, joined);
    if (joined) continue;
    const long areaB = flood(sb, 3, -1, joined);
    if (std::min(areaA, areaB) >= kFlatMinSplit) out.insert(out.end(), {x1, y1, x2, y2});
  }
  return out;
}

}  // namespace

FlatSegs flatSuggestGaps(const FlatMask& line, int w, int h, int maxGap, const FlatLabels* labels) {
  const int maxBridge = flatMaxBridge(maxGap);
  const Endpoints e = extractEndpoints(line, w, h, maxBridge);
  const std::vector<Ep>& info = e.info;
  FlatSegs segs;
  std::vector<uint8_t> used(info.size(), 0);
  // endpoint <-> endpoint, most-relatable (lowest elastica energy) pairs first
  std::vector<std::tuple<float, int, int>> pairs;
  for (size_t a = 0; a < info.size(); a++) {
    for (size_t b = a + 1; b < info.size(); b++) {
      const Ep& A = info[a];
      const Ep& B = info[b];
      const float d = std::hypot(static_cast<float>(A.x - B.x), static_cast<float>(A.y - B.y));
      if (d > maxBridge || d < 2) continue;
      if (A.branch.count(B.i)) continue;  // already connected along the stroke
      const int32_t wa = e.wd[A.i], wb = e.wd[B.i];
      if (std::max(wa, wb) > 2.5 * std::min(wa, wb) + 3) continue;
      const FlatRelatability rel = flatRelatable(static_cast<float>(A.x), static_cast<float>(A.y), A.dx, A.dy,
                                                 static_cast<float>(B.x), static_cast<float>(B.y), B.dx, B.dy);
      if (!rel.ok) continue;
      pairs.emplace_back(rel.energy, static_cast<int>(a), static_cast<int>(b));
    }
  }
  std::stable_sort(pairs.begin(), pairs.end(),
                   [](const auto& p, const auto& q) { return std::get<0>(p) < std::get<0>(q); });
  for (const auto& [en, a, b] : pairs) {
    (void)en;
    if (used[a] || used[b]) continue;
    used[a] = used[b] = 1;
    segs.insert(segs.end(), {static_cast<float>(info[a].x), static_cast<float>(info[a].y),
                             static_cast<float>(info[b].x), static_cast<float>(info[b].y)});
  }
  // endpoint -> nearest foreign line pixel
  for (size_t a = 0; a < info.size(); a++) {
    if (used[a]) continue;
    const int32_t hit = nearestForeignLine(line, info[a], w, h, maxBridge);
    if (hit >= 0)
      segs.insert(segs.end(), {static_cast<float>(info[a].x), static_cast<float>(info[a].y),
                               static_cast<float>(hit % w), static_cast<float>(hit / w)});
  }
  return labels ? filterLeaky(segs, *labels, line, w, h) : segs;
}

namespace {

constexpr float TIGHT_CONE = 0.7f;  // cos, ~45 degrees off the chord
constexpr float TIGHT_BEND = 70.f;  // degrees of total turn allowed across the join

// Is the chord's interior free of ink? Skip each end's OWN ink run and
// require what is left in the middle to be blank. If the two runs meet there
// was never a gap.
bool crossingIsOpen(const FlatMask& line, int w, int h, int x1, int y1, int x2, int y2) {
  const int steps = static_cast<int>(std::ceil(std::hypot(static_cast<float>(x2 - x1), static_cast<float>(y2 - y1))));
  if (steps < 2) return false;
  auto at = [&](int s) -> int {
    const int x = iround(x1 + static_cast<double>(x2 - x1) * s / steps);
    const int y = iround(y1 + static_cast<double>(y2 - y1) * s / steps);
    return x < 0 || y < 0 || x >= w || y >= h ? -1 : line[static_cast<size_t>(y) * w + x] ? 1 : 0;
  };
  int a = 0, b = steps;
  while (a <= steps && at(a) == 1) a++;
  while (b >= 0 && at(b) == 1) b--;
  if (a > b) return false;
  for (int s = a; s <= b; s++)
    if (at(s) != 0) return false;
  return true;
}

}  // namespace

FlatSegs flatTightClosures(const FlatMask& line, int w, int h, int maxGap) {
  const int maxBridge = flatMaxBridge(maxGap);
  const Endpoints e = extractEndpoints(line, w, h, maxBridge, true);
  const std::vector<Ep>& info = e.info;
  std::vector<std::tuple<float, int, int>> pairs;
  for (size_t a = 0; a < info.size(); a++) {
    for (size_t b = a + 1; b < info.size(); b++) {
      const Ep& A = info[a];
      const Ep& B = info[b];
      const float d = std::hypot(static_cast<float>(A.x - B.x), static_cast<float>(A.y - B.y));
      if (d > maxBridge || d < 2) continue;
      if (A.branch.count(B.i)) continue;
      const int32_t wa = e.wd[A.i], wb = e.wd[B.i];
      if (std::max(wa, wb) > 2.5 * std::min(wa, wb) + 3) continue;
      const FlatRelatability rel =
          flatRelatable(static_cast<float>(A.x), static_cast<float>(A.y), A.dx, A.dy, static_cast<float>(B.x),
                        static_cast<float>(B.y), B.dx, B.dy, TIGHT_CONE, TIGHT_BEND);
      if (!rel.ok) continue;
      if (!crossingIsOpen(line, w, h, A.x, A.y, B.x, B.y)) continue;
      pairs.emplace_back(rel.energy, static_cast<int>(a), static_cast<int>(b));
    }
  }
  std::stable_sort(pairs.begin(), pairs.end(),
                   [](const auto& p, const auto& q) { return std::get<0>(p) < std::get<0>(q); });
  std::vector<uint8_t> used(info.size(), 0);
  FlatSegs segs;
  for (const auto& [en, a, b] : pairs) {
    (void)en;
    if (used[a] || used[b]) continue;
    used[a] = used[b] = 1;
    segs.insert(segs.end(), {static_cast<float>(info[a].x), static_cast<float>(info[a].y),
                             static_cast<float>(info[b].x), static_cast<float>(info[b].y)});
  }
  return segs;
}

// --------------------------------------------------------------------- curves

FlatPolyline flatCoCompleteBridge(float x1, float y1, float x2, float y2, const FlatMask& line, int w,
                                  int h, bool* found) {
  *found = false;
  const float dx = x2 - x1, dy = y2 - y1;
  const float len = std::hypot(dx, dy);
  if (len < 8) return {};
  const float ux = dx / len, uy = dy / len, nx = -uy, ny = ux;
  const int K = std::max(6, std::min(20, iround(len / 2)));
  auto at = [&](float t, float o) -> int32_t {
    const int x = iround(x1 + ux * t * len + nx * o), y = iround(y1 + uy * t * len + ny * o);
    return x < 0 || y < 0 || x >= w || y >= h ? -1 : y * w + x;
  };
  const int maxOff = std::min(20, iround(2.5 * len));
  for (int ao = 3; ao <= maxOff; ao++) {
    for (const int o : {ao, -ao}) {
      int hits = 0;
      for (int k = 0; k <= K; k++) {
        for (int e = -1; e <= 1; e++) {
          const int32_t i = at(static_cast<float>(k) / K, static_cast<float>(o + e));
          if (i >= 0 && line[i]) { hits++; break; }
        }
      }
      const int32_t pre = at(-0.15f, static_cast<float>(o)), post = at(1.15f, static_cast<float>(o));
      if (static_cast<float>(hits) / (K + 1) < 0.85f || pre < 0 || !line[pre] || post < 0 || !line[post]) continue;
      // trace the partner's wobble and copy it across
      FlatPolyline pts{x1, y1};
      int prev = o;
      for (int k = 1; k < K; k++) {
        const float t = static_cast<float>(k) / K;
        int fo = prev;
        bool done = false;
        for (int e = 0; e <= 3 && !done; e++) {
          const int cands[2] = {e, -e};
          const int nc = e ? 2 : 1;
          for (int c = 0; c < nc; c++) {
            const int32_t i = at(t, static_cast<float>(prev + cands[c]));
            if (i >= 0 && line[i]) { fo = prev + cands[c]; done = true; break; }
          }
        }
        prev = fo;
        pts.push_back(x1 + ux * t * len + nx * (fo - o));
        pts.push_back(y1 + uy * t * len + ny * (fo - o));
      }
      pts.push_back(x2);
      pts.push_back(y2);
      *found = true;
      return pts;
    }
  }
  return {};
}

FlatPolyline flatCurveBridge(float x1, float y1, float x2, float y2, const FlatOrientation* orient) {
  const float dx = x2 - x1, dy = y2 - y1;
  const float len0 = std::hypot(dx, dy);
  const float len = len0 > 0 ? len0 : 1.f;
  float f1x = dx / len, f1y = dy / len, f2x = f1x, f2y = f1y;
  if (orient) {
    const FlatOrientationSample a = flatSampleOrientation(*orient, iround(x1), iround(y1));
    const FlatOrientationSample b = flatSampleOrientation(*orient, iround(x2), iround(y2));
    if (a.coh > 0.25f) { f1x = a.fx; f1y = a.fy; if (f1x * dx + f1y * dy < 0) { f1x = -f1x; f1y = -f1y; } }
    if (b.coh > 0.25f) { f2x = b.fx; f2y = b.fy; if (f2x * dx + f2y * dy < 0) { f2x = -f2x; f2y = -f2y; } }
  }
  return flatElasticaCurve(x1, y1, f1x, f1y, x2, y2, -f2x, -f2y);
}

std::vector<FlatPolyline> flatBridgePaths(const FlatSegs& segs, const FlatOrientation* orient,
                                          const FlatMask& line, int w, int h) {
  std::vector<FlatPolyline> paths;
  for (size_t i = 0; i + 3 < segs.size(); i += 4) {
    bool found = false;
    FlatPolyline co = flatCoCompleteBridge(segs[i], segs[i + 1], segs[i + 2], segs[i + 3], line, w, h, &found);
    paths.push_back(found ? std::move(co) : flatCurveBridge(segs[i], segs[i + 1], segs[i + 2], segs[i + 3], orient));
  }
  return paths;
}

// -------------------------------------------------------------------- closure

FlatBridgeSelection flatSelectBridges(const std::vector<FlatPolyline>& paths, const FlatLabels& labels,
                                      const FlatMask& line, int w, int h, int minSplit) {
  constexpr int BOX = 96;
  const int side = 2 * BOX + 1;
  std::vector<int32_t> mark(static_cast<size_t>(side) * side, 0);  // stamped with a per-test id
  std::vector<int32_t> queue(static_cast<size_t>(side) * side);
  FlatMask accepted(static_cast<size_t>(w) * h, 0);  // bridges taken so far, as barriers
  FlatBridgeSelection sel;
  int32_t stamp = 0;

  auto rasterize = [&](const FlatPolyline& p, auto&& put) {
    for (size_t i = 0; i + 3 < p.size(); i += 2) {
      const int steps = std::max(1, static_cast<int>(std::ceil(std::hypot(p[i + 2] - p[i], p[i + 3] - p[i + 1]))));
      for (int s = 0; s <= steps; s++) {
        const int px = iround(p[i] + (p[i + 2] - p[i]) * s / steps);
        const int py = iround(p[i + 1] + (p[i + 3] - p[i + 1]) * s / steps);
        for (int oy = -1; oy <= 1; oy++)
          for (int ox = -1; ox <= 1; ox++) put(px + ox, py + oy);
      }
    }
  };

  for (size_t pi = 0; pi < paths.size(); pi++) {
    const FlatPolyline& p = paths[pi];
    if (p.size() < 4) continue;
    // window centred on the path's midpoint
    float cxf = 0, cyf = 0;
    for (size_t i = 0; i < p.size(); i += 2) { cxf += p[i]; cyf += p[i + 1]; }
    const int cx = iround(cxf / (p.size() / 2)), cy = iround(cyf / (p.size() / 2));
    const int bx = cx - BOX, by = cy - BOX;

    // seeds either side of the path's midsegment
    const size_t mi = ((p.size() / 2) >> 1) * 2;
    const float ax = p[mi >= 2 ? mi - 2 : 0], ay = p[mi >= 1 ? mi - 1 : 0];
    const float bx2 = p[std::min(p.size() - 2, mi)], by2 = p[std::min(p.size() - 1, mi + 1)];
    const float len0 = std::hypot(bx2 - ax, by2 - ay);
    const float len = len0 > 0 ? len0 : 1.f;
    const float nx = -(by2 - ay) / len, ny = (bx2 - ax) / len;
    const float mx = (ax + bx2) / 2, my = (ay + by2) / 2;
    auto seed = [&](int sign) -> int32_t {
      for (const int d : {2, 3, 4, 6}) {
        const int px = iround(mx + nx * d * sign), py = iround(my + ny * d * sign);
        if (px < 1 || py < 1 || px >= w - 1 || py >= h - 1) continue;
        if (std::abs(px - cx) > BOX - 2 || std::abs(py - cy) > BOX - 2) continue;
        const int32_t gi = py * w + px;
        if (!line[gi] && !accepted[gi] && labels[gi]) return gi;
      }
      return -1;
    };
    const int32_t sa = seed(1), sb = seed(-1);
    if (sa < 0 || sb < 0 || labels[sa] != labels[sb]) continue;
    const int32_t region = labels[sa];

    auto flood = [&](int32_t start, int32_t target, const std::vector<uint8_t>* wall, bool& reached) -> long {
      const int32_t id = ++stamp;
      size_t qt = 0, head = 0;
      long area = 0;
      bool spill = false;
      queue[qt++] = start;
      mark[static_cast<size_t>(start / w - by) * side + (start % w - bx)] = id;
      while (head < qt) {
        const int32_t q = queue[head++];
        area++;
        if (q == target) { reached = true; return area; }
        const int qx = q % w, qy = q / w;
        const int dirs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (const auto& dxy : dirs) {
          const int rx = qx + dxy[0], ry = qy + dxy[1];
          if (rx < 0 || ry < 0 || rx >= w || ry >= h) continue;
          const int lx = rx - bx, ly = ry - by;
          if (lx < 0 || ly < 0 || lx >= side || ly >= side) { spill = true; continue; }
          const size_t li = static_cast<size_t>(ly) * side + lx;
          if (mark[li] == id) continue;
          const int32_t gi = ry * w + rx;
          if (line[gi] || accepted[gi] || labels[gi] != region) continue;
          if (wall && (*wall)[li]) continue;
          mark[li] = id;
          queue[qt++] = gi;
        }
      }
      reached = false;
      return spill ? 1000000000L : area;
    };

    // (1) Prägnanz: if the sides are ALREADY separated by accepted bridges,
    // this candidate closes nothing new -- drop it.
    bool connected = false;
    flood(sa, sb, nullptr, connected);
    if (!connected) continue;

    // (2) Closure: rasterize this path as a wall and check both sides are real.
    std::vector<uint8_t> wall(static_cast<size_t>(side) * side, 0);
    rasterize(p, [&](int gx, int gy) {
      const int lx = gx - bx, ly = gy - by;
      if (lx >= 0 && ly >= 0 && lx < side && ly < side) wall[static_cast<size_t>(ly) * side + lx] = 1;
    });
    bool joined = false;
    const long areaA = flood(sa, sb, &wall, joined);
    if (joined) continue;  // the wall doesn't actually separate the seeds
    bool dummy = false;
    const long areaB = flood(sb, -1, &wall, dummy);
    const long g = std::min(areaA, areaB);
    if (g < minSplit) continue;

    sel.keep.push_back(static_cast<int>(pi));
    sel.gain.push_back(static_cast<int>(g));
    // commit: this bridge now blocks later candidates (redundancy source)
    rasterize(p, [&](int gx, int gy) {
      if (gx >= 0 && gy >= 0 && gx < w && gy < h) accepted[static_cast<size_t>(gy) * w + gx] = 1;
    });
  }
  return sel;
}

void flatMarkLine(FlatMask& mask, int w, int h, float x0, float y0, float x1, float y1) {
  int x = iround(x0), y = iround(y0);
  const int ex = iround(x1), ey = iround(y1);
  const int dx = std::abs(ex - x), dy = -std::abs(ey - y);
  const int sx = x < ex ? 1 : -1, sy = y < ey ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    if (x >= 0 && y >= 0 && x < w && y < h) mask[static_cast<size_t>(y) * w + x] = 1;
    if (x == ex && y == ey) break;
    const int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x += sx; }
    if (e2 <= dx) { err += dx; y += sy; }
  }
}

}  // namespace np
