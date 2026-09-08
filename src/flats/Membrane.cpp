#include "flats/Membrane.hpp"

#include <cmath>
#include <cstddef>

namespace np {

namespace {

struct Level {
  int w = 0, h = 0;
  FlatMask free;             // 1 = sheet may move here, 0 = pinned to 0
  std::vector<float> u;  // solution / correction
  std::vector<float> b;  // right-hand side (h² * load)
  std::vector<float> e;  // prolonged coarse correction, before it is accepted
};

Level makeLevel(int w, int h, FlatMask free) {
  Level L;
  L.w = w;
  L.h = h;
  L.free = std::move(free);
  const size_t n = static_cast<size_t>(w) * h;
  L.u.assign(n, 0.f);
  L.b.assign(n, 0.f);
  L.e.assign(n, 0.f);
  return L;
}

// A coarse cell is free only if ALL of its children are. Ink therefore
// thickens as we coarsen, which is the safe direction: the coarse grid can
// under-correct near a stroke (the smoother mops that up) but can never carry
// a correction across one.
FlatMask coarsenMask(const FlatMask& free, int w, int h, int& w2, int& h2) {
  w2 = (w + 1) >> 1;
  h2 = (h + 1) >> 1;
  FlatMask f2(static_cast<size_t>(w2) * h2, 0);
  for (int cy = 0; cy < h2; cy++) {
    for (int cx = 0; cx < w2; cx++) {
      const int x = cx << 1, y = cy << 1;
      uint8_t all = 1;
      for (int dy = 0; dy < 2 && all; dy++) {
        for (int dx = 0; dx < 2; dx++) {
          const int px = x + dx, py = y + dy;
          if (px >= w || py >= h) continue;
          if (!free[static_cast<size_t>(py) * w + px]) { all = 0; break; }
        }
      }
      f2[static_cast<size_t>(cy) * w2 + cx] = all;
    }
  }
  return f2;
}

// u <- (sum of neighbours + b) / 4 on free cells. Pinned and out-of-bounds
// neighbours contribute 0, so the denominator is always 4.
// Red-black ordering: each colour reads only the other, so the sweep is
// order-independent and converges roughly twice as fast as Jacobi.
void smooth(Level& L, int sweeps) {
  const int w = L.w, h = L.h, last = w - 1;
  const FlatMask& free = L.free;
  float* u = L.u.data();
  const float* b = L.b.data();
  // Edge cells need bounds checks; the interior does not. Pinned cells are
  // kept at zero by multiplying rather than branching -- `free` is
  // unpredictable at every stroke, and a mispredict costs more than the
  // multiply.
  auto edge = [&](int x, int y) {
    const size_t i = static_cast<size_t>(y) * w + x;
    if (!free[i]) return;
    float sum = b[i];
    if (x > 0) sum += u[i - 1];
    if (x < last) sum += u[i + 1];
    if (y > 0) sum += u[i - w];
    if (y < h - 1) sum += u[i + w];
    u[i] = sum * 0.25f;
  };
  for (int s = 0; s < sweeps; s++) {
    for (int color = 0; color < 2; color++) {
      for (int x = color & 1; x < w; x += 2) edge(x, 0);
      for (int y = 1; y < h - 1; y++) {
        const size_t row = static_cast<size_t>(y) * w;
        int x = (y ^ color) & 1;
        if (x == 0) { edge(0, y); x = 2; }
        for (; x < last; x += 2) {
          const size_t i = row + x;
          u[i] = (b[i] + u[i - 1] + u[i + 1] + u[i - w] + u[i + w]) * 0.25f * free[i];
        }
        if (x == last) edge(last, y);
      }
      if (h > 1)
        for (int x = ((h - 1) ^ color) & 1; x < w; x += 2) edge(x, h - 1);
    }
  }
}

// Residual r = b - Lu, restricted onto the coarse right-hand side in one pass.
// Full weighting is the sum over the four children (the 1/4 average times the
// 4x that h² -> (2h)² rescaling demands), so the two factors cancel.
void restrictResidual(const Level& L, Level& C) {
  const int w = L.w, h = L.h;
  std::fill(C.b.begin(), C.b.end(), 0.f);
  std::fill(C.u.begin(), C.u.end(), 0.f);
  for (int y = 0; y < h; y++) {
    const size_t row = static_cast<size_t>(y) * w;
    for (int x = 0; x < w; x++) {
      const size_t i = row + x;
      if (!L.free[i]) continue;
      float sum = 0;
      if (x > 0) sum += L.u[i - 1];
      if (x < w - 1) sum += L.u[i + 1];
      if (y > 0) sum += L.u[i - w];
      if (y < h - 1) sum += L.u[i + w];
      const float r = L.b[i] - (4 * L.u[i] - sum);
      const size_t ci = static_cast<size_t>(y >> 1) * C.w + (x >> 1);
      if (C.free[ci]) C.b[ci] += r;
    }
  }
}

// Bilinear (cell-centred 9/3/3/1) interpolation of the coarse correction into
// L.e. Pinned coarse cells read as 0, so the correction tapers off smoothly as
// it approaches a stroke. It must taper rather than stop: zeroing e wherever
// the coarse grid has nothing to say would put a cliff into the correction at
// every thin channel, and the line search below would then be measuring those
// cliffs instead of the correction.
void prolong(const Level& C, Level& L) {
  const int w = L.w, h = L.h, cw = C.w, ch = C.h;
  std::fill(L.e.begin(), L.e.end(), 0.f);
  auto at = [&](int cx, int cy) -> float {
    if (cx < 0 || cy < 0 || cx >= cw || cy >= ch) return 0.f;
    const size_t ci = static_cast<size_t>(cy) * cw + cx;
    return C.free[ci] ? C.u[ci] : 0.f;
  };
  for (int y = 0; y < h; y++) {
    const size_t row = static_cast<size_t>(y) * w;
    const int cy = y >> 1, sy = (y & 1) ? 1 : -1;
    for (int x = 0; x < w; x++) {
      const size_t i = row + x;
      if (!L.free[i]) continue;
      const int cx = x >> 1, sx = (x & 1) ? 1 : -1;
      L.e[i] = (9 * at(cx, cy) + 3 * at(cx + sx, cy) + 3 * at(cx, cy + sy) + at(cx + sx, cy + sy)) / 16;
    }
  }
}

// Accept the correction only as far as it actually helps: u += alpha*e with
// the alpha that minimises the error along e. The measure has to be the A-norm
// of the error (alpha = <r,e>/<e,Ae>), not the plain residual: a coarse-grid
// correction is *expected* to raise the residual, because it trades smooth
// error for the rough error that post-smoothing then clears. Line-searching
// on the residual instead fights the algorithm -- it settles on alpha ~ 0.08
// and the solver stalls.
//
// Without this the solver DIVERGES on real line art -- roughly doubling per
// cycle -- while behaving perfectly on a clean disc. The reason is that the
// coarse grids cannot represent a drawing: coarsening conservatively turns
// every thin channel into ink, so the coarse problem is a different problem,
// and a correction computed on it can point the wrong way. A textbook V-cycle
// takes that correction on faith. The line search cannot: at worst it picks
// alpha = 0 and the cycle degenerates to plain smoothing.
float correct(Level& L) {
  const int w = L.w, h = L.h;
  auto lap = [&](const std::vector<float>& a, size_t i, int x, int y) -> float {
    float sum = 0;
    if (x > 0) sum += a[i - 1];
    if (x < w - 1) sum += a[i + 1];
    if (y > 0) sum += a[i - w];
    if (y < h - 1) sum += a[i + w];
    return 4 * a[i] - sum;
  };
  double num = 0, den = 0;
  for (int y = 0; y < h; y++) {
    const size_t row = static_cast<size_t>(y) * w;
    for (int x = 0; x < w; x++) {
      const size_t i = row + x;
      if (!L.free[i]) continue;
      num += static_cast<double>(L.b[i] - lap(L.u, i, x, y)) * L.e[i];
      den += static_cast<double>(L.e[i]) * lap(L.e, i, x, y);
    }
  }
  if (!(den > 0)) return 0.f;
  const float alpha = static_cast<float>(num / den);
  const size_t n = static_cast<size_t>(w) * h;
  for (size_t i = 0; i < n; i++)
    if (L.free[i]) L.u[i] += alpha * L.e[i];
  return alpha;
}

double residual(const Level& L) {
  const int w = L.w, h = L.h;
  double s = 0;
  for (int y = 0; y < h; y++) {
    const size_t row = static_cast<size_t>(y) * w;
    for (int x = 0; x < w; x++) {
      const size_t i = row + x;
      if (!L.free[i]) continue;
      float sum = 0;
      if (x > 0) sum += L.u[i - 1];
      if (x < w - 1) sum += L.u[i + 1];
      if (y > 0) sum += L.u[i - w];
      if (y < h - 1) sum += L.u[i + w];
      const double r = L.b[i] - (4 * L.u[i] - sum);
      s += r * r;
    }
  }
  return std::sqrt(s);
}

constexpr int NU = 6;  // smoothing sweeps each side of the coarse correction

void vcycle(std::vector<Level>& ls, size_t k) {
  Level& L = ls[k];
  if (k == ls.size() - 1) { smooth(L, 40); return; }
  smooth(L, NU);
  restrictResidual(L, ls[k + 1]);
  vcycle(ls, k + 1);
  prolong(ls[k + 1], L);
  correct(L);
  smooth(L, NU);
}

}  // namespace

std::vector<float> flatMembraneSag(const FlatMask& line, int w, int h, int cycles, float tol) {
  const size_t n = static_cast<size_t>(w) * h;
  FlatMask free(n);
  for (size_t i = 0; i < n; i++) free[i] = line[i] ? 0 : 1;

  std::vector<Level> ls;
  ls.push_back(makeLevel(w, h, free));
  while (ls.back().w > 8 && ls.back().h > 8) {
    const Level& p = ls.back();
    int w2 = 0, h2 = 0;
    FlatMask f2 = coarsenMask(p.free, p.w, p.h, w2, h2);
    ls.push_back(makeLevel(w2, h2, std::move(f2)));
  }

  Level& top = ls[0];
  for (size_t i = 0; i < n; i++)
    if (free[i]) top.b[i] = 1.f;  // unit gravity, h = 1
  // `cycles` is a ceiling, not a setting: stop as soon as the residual is
  // small enough that further sag changes land inside the quantisation used
  // downstream.
  const double r0 = residual(top);
  for (int c = 0; c < cycles; c++) {
    if (residual(top) < tol * r0) break;
    vcycle(ls, 0);
  }

  std::vector<float> sag(n, 0.f);
  for (size_t i = 0; i < n; i++)
    if (free[i] && top.u[i] > 0) sag[i] = std::sqrt(8.f * top.u[i]);
  return sag;
}

}  // namespace np
