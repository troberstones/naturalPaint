#include "ops/Poisson.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace np {

namespace {

// One grid of the V-cycle. The finest level holds the solution with its
// Dirichlet ring already in place; every coarser one holds a CORRECTION, whose
// ring is zero by construction -- a correction to a field that already matches
// the boundary data must not move the boundary.
//
// **There is no `free` mask, and its absence is the difference between this
// solver and `flats/Membrane`'s.** The domain is a rectangle: a cell is interior
// exactly when `1 <= x <= w-2 && 1 <= y <= h-2`, at every level. That makes the
// interior loops bounds-check-free (every interior cell's four neighbours exist)
// and it is why the coarse-grid line search that header needs is absent here --
// see ops/Poisson.hpp §2.
struct Level {
  int w = 0, h = 0;
  std::vector<float> u;  // solution on level 0, correction below it
  std::vector<float> b;  // right-hand side; zero on level 0 (Laplace, not Poisson)
  std::vector<float> e;  // the prolonged coarse correction, before it is added
};

Level makeLevel(int w, int h) {
  Level L;
  L.w = w;
  L.h = h;
  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
  L.u.assign(n, 0.0f);
  L.b.assign(n, 0.0f);
  L.e.assign(n, 0.0f);
  return L;
}

// u <- (b + (left + right) + (up + down)) / 4 on the interior, red then black.
//
// Red-black rather than lexicographic Gauss-Seidel for `flats/Membrane`'s
// reason: within a colour every cell reads only cells of the other colour, so
// the sweep is order-independent and converges roughly twice as fast as Jacobi.
//
// **The two sums are PAIRWISE and that is load-bearing**, not a stylistic
// preference: `(l+r)+(u+d)` is exactly `4c` when all four are the same `c`,
// where `((l+r)+u)+d` need not be. It is what makes a constant field a
// bit-exact fixed point of this loop, and therefore what makes
// ops/Poisson.hpp §1's two exactness claims survive the sweeps that follow the
// answer instead of drifting a rounding error per sweep.
void smooth(Level& L, int sweeps) {
  const int w = L.w, h = L.h;
  float* u = L.u.data();
  const float* b = L.b.data();
  for (int s = 0; s < sweeps; ++s) {
    for (int color = 0; color < 2; ++color) {
      for (int y = 1; y < h - 1; ++y) {
        const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
        int x = 1 + (((y + 1) ^ color) & 1);
        for (; x < w - 1; x += 2) {
          const size_t i = row + static_cast<size_t>(x);
          const float horiz = u[i - 1] + u[i + 1];
          const float vert = u[i - static_cast<size_t>(w)] + u[i + static_cast<size_t>(w)];
          u[i] = (b[i] + (horiz + vert)) * 0.25f;
        }
      }
    }
  }
}

// The five-point residual `b - (4u - neighbours)` at one interior cell. Written
// once and called from both the restriction and the convergence test, so the
// number the stopping rule watches is the same quantity the coarse grid is
// asked to reduce.
inline float residualAt(const Level& L, size_t i) noexcept {
  const size_t w = static_cast<size_t>(L.w);
  const float horiz = L.u[i - 1] + L.u[i + 1];
  const float vert = L.u[i - w] + L.u[i + w];
  return L.b[i] - (4.0f * L.u[i] - (horiz + vert));
}

// Full-weighting restriction of the residual onto the coarse right-hand side:
// the sum over the four children, which is the 1/4 average times the 4x that
// rescaling `h^2 -> (2h)^2` demands, so the two factors cancel.
//
// A residual whose coarse cell lands on the coarse RING is dropped rather than
// added. That is the same conservative direction `flats/Membrane`'s mask
// coarsening takes: the coarse grid says nothing about the two rows nearest the
// boundary, and the post-smoothing sweeps are what clear the error there.
void restrictResidual(const Level& L, Level& C) {
  const int w = L.w, h = L.h;
  std::fill(C.b.begin(), C.b.end(), 0.0f);
  std::fill(C.u.begin(), C.u.end(), 0.0f);
  for (int y = 1; y < h - 1; ++y) {
    const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
    const int cy = y >> 1;
    if (cy < 1 || cy > C.h - 2) continue;
    for (int x = 1; x < w - 1; ++x) {
      const int cx = x >> 1;
      if (cx < 1 || cx > C.w - 2) continue;
      C.b[static_cast<size_t>(cy) * static_cast<size_t>(C.w) + static_cast<size_t>(cx)] +=
          residualAt(L, row + static_cast<size_t>(x));
    }
  }
}

// Bilinear (cell-centred 9/3/3/1) interpolation of the coarse correction into
// `L.e`. A coarse cell outside the grid, or on the coarse ring, reads as 0, so
// the correction tapers to nothing as it approaches the boundary -- which is
// exactly right here, because the boundary is where the fine level already
// carries the true Dirichlet data and has no error to correct.
void prolong(const Level& C, Level& L) {
  const int w = L.w, h = L.h, cw = C.w, ch = C.h;
  std::fill(L.e.begin(), L.e.end(), 0.0f);
  const auto at = [&](int cx, int cy) -> float {
    if (cx < 1 || cy < 1 || cx > cw - 2 || cy > ch - 2) return 0.0f;
    return C.u[static_cast<size_t>(cy) * static_cast<size_t>(cw) + static_cast<size_t>(cx)];
  };
  for (int y = 1; y < h - 1; ++y) {
    const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
    const int cy = y >> 1, sy = (y & 1) ? 1 : -1;
    for (int x = 1; x < w - 1; ++x) {
      const int cx = x >> 1, sx = (x & 1) ? 1 : -1;
      L.e[row + static_cast<size_t>(x)] =
          (9.0f * at(cx, cy) + 3.0f * at(cx + sx, cy) + 3.0f * at(cx, cy + sy) +
           at(cx + sx, cy + sy)) /
          16.0f;
    }
  }
}

void addCorrection(Level& L) {
  const int w = L.w, h = L.h;
  for (int y = 1; y < h - 1; ++y) {
    const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
    for (int x = 1; x < w - 1; ++x) L.u[row + static_cast<size_t>(x)] += L.e[row + static_cast<size_t>(x)];
  }
}

double residualNorm(const Level& L) {
  const int w = L.w, h = L.h;
  double s = 0.0;
  for (int y = 1; y < h - 1; ++y) {
    const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
    for (int x = 1; x < w - 1; ++x) {
      const double r = residualAt(L, row + static_cast<size_t>(x));
      s += r * r;
    }
  }
  return std::sqrt(s);
}

constexpr int kNu = 6;  // smoothing sweeps each side of the coarse correction

void vcycle(std::vector<Level>& ls, size_t k) {
  Level& L = ls[k];
  if (k == ls.size() - 1) {
    // The coarsest grid is a few hundred cells at most, so sweeping it hard is
    // cheaper than another level and closer to the exact solve a V-cycle wants
    // at its bottom.
    smooth(L, 40);
    return;
  }
  smooth(L, kNu);
  restrictResidual(L, ls[k + 1]);
  vcycle(ls, k + 1);
  prolong(ls[k + 1], L);
  addCorrection(L);
  smooth(L, kNu);
}

}  // namespace

void harmonicFill(std::vector<float>& u, int w, int h, int cycles, float tol) {
  if (w < 3 || h < 3) return;  // no interior to solve; the ring IS the patch
  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
  if (u.size() != n) return;

  // ops/Poisson.hpp §1: a constant ring has a constant exact solution, found
  // here rather than approached by iteration. Bit-for-bit equality is the right
  // test and not a tolerance -- the claim being made is that the answer is
  // exact, and "all these floats are within epsilon of each other" would not
  // support it.
  const float first = u[0];
  bool ringConstant = true;
  for (int x = 0; x < w && ringConstant; ++x) {
    if (u[static_cast<size_t>(x)] != first) ringConstant = false;
    if (u[static_cast<size_t>(h - 1) * static_cast<size_t>(w) + static_cast<size_t>(x)] != first)
      ringConstant = false;
  }
  for (int y = 1; y < h - 1 && ringConstant; ++y) {
    const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
    if (u[row] != first || u[row + static_cast<size_t>(w - 1)] != first) ringConstant = false;
  }
  if (ringConstant) {
    for (int y = 1; y < h - 1; ++y) {
      const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
      for (int x = 1; x < w - 1; ++x) u[row + static_cast<size_t>(x)] = first;
    }
    return;
  }

  std::vector<Level> ls;
  ls.push_back(makeLevel(w, h));
  ls[0].u = std::move(u);
  // The mean of the ring as the initial interior guess. The constant mode is
  // the slowest one a Laplace solve converges on from zero -- the multigrid
  // exists to fix exactly that, but starting where the answer already averages
  // costs one pass and hands the first V-cycle a much smaller error to chew on.
  double ringSum = 0.0;
  size_t ringCount = 0;
  const auto tally = [&](size_t i) {
    ringSum += ls[0].u[i];
    ++ringCount;
  };
  for (int x = 0; x < w; ++x) {
    tally(static_cast<size_t>(x));
    tally(static_cast<size_t>(h - 1) * static_cast<size_t>(w) + static_cast<size_t>(x));
  }
  for (int y = 1; y < h - 1; ++y) {
    const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
    tally(row);
    tally(row + static_cast<size_t>(w - 1));
  }
  const float guess = ringCount > 0 ? static_cast<float>(ringSum / static_cast<double>(ringCount)) : 0.0f;
  for (int y = 1; y < h - 1; ++y) {
    const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
    for (int x = 1; x < w - 1; ++x) ls[0].u[row + static_cast<size_t>(x)] = guess;
  }

  // Coarsen while the coarse grid still has an interior worth solving on. A
  // level whose interior is one cell wide has nothing a smoother can propagate.
  while (ls.back().w >= 8 && ls.back().h >= 8) {
    const int w2 = (ls.back().w + 1) >> 1;
    const int h2 = (ls.back().h + 1) >> 1;
    ls.push_back(makeLevel(w2, h2));
  }

  const double r0 = residualNorm(ls[0]);
  for (int c = 0; c < cycles; ++c) {
    if (residualNorm(ls[0]) < static_cast<double>(tol) * r0) break;
    vcycle(ls, 0);
  }

  u = std::move(ls[0].u);
}

std::vector<std::array<float, 4>> healPatch(const std::vector<std::array<float, 4>>& src,
                                            const std::vector<std::array<float, 4>>& dst, int w,
                                            int h, int cycles) {
  const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
  if (w < 3 || h < 3 || src.size() != n || dst.size() != n) return dst;

  std::vector<std::array<float, 4>> out = src;
  std::vector<float> plane(n, 0.0f);
  for (int ch = 0; ch < 4; ++ch) {
    // The ring carries `dst - src`; the interior is scratch that
    // `harmonicFill()` overwrites. Only the ring is written here, so the
    // interior arrives as the zeros `plane` was built with and the fill's own
    // initial guess replaces them.
    const auto ring = [&](size_t i) { plane[i] = dst[i][static_cast<size_t>(ch)] - src[i][static_cast<size_t>(ch)]; };
    for (int x = 0; x < w; ++x) {
      ring(static_cast<size_t>(x));
      ring(static_cast<size_t>(h - 1) * static_cast<size_t>(w) + static_cast<size_t>(x));
    }
    for (int y = 1; y < h - 1; ++y) {
      const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
      ring(row);
      ring(row + static_cast<size_t>(w - 1));
    }
    harmonicFill(plane, w, h, cycles);
    for (size_t i = 0; i < n; ++i) out[i][static_cast<size_t>(ch)] += plane[i];
  }

  // **The ring comes back as `dst`, bit for bit.** It is the boundary
  // condition, and `src + (dst - src)` is not guaranteed to reproduce `dst`
  // exactly in floating point. A caller comparing the rim against `dst` to
  // decide whether a texel needs writing -- which is exactly what
  // `brush/Heal`'s skip test does -- would otherwise dirty a tile per dab over
  // a patch the solve had nothing to say about.
  const auto pin = [&](size_t i) { out[i] = dst[i]; };
  for (int x = 0; x < w; ++x) {
    pin(static_cast<size_t>(x));
    pin(static_cast<size_t>(h - 1) * static_cast<size_t>(w) + static_cast<size_t>(x));
  }
  for (int y = 1; y < h - 1; ++y) {
    const size_t row = static_cast<size_t>(y) * static_cast<size_t>(w);
    pin(row);
    pin(row + static_cast<size_t>(w - 1));
  }
  return out;
}

}  // namespace np
