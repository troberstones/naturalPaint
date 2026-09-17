#include "app/selftest/Support.hpp"

#include <array>
#include <cmath>
#include <cstdio>

#include "ops/PolarRemap.hpp"

// app/selftest/PolarRemap -- docs/operations.md §3: Polar Coordinates
// (ops/PolarRemap.hpp), long marked "future work"
// (docs/spec-vs-implementation.md §4). Headless and GPU-free.
//
// The property worth proving is not "the two directions look plausible" --
// it is the three things ops/PolarRemap.hpp's own header comment claims and
// a casual read of the code cannot confirm: the ANGULAR SEAM actually wraps
// rather than zero-padding at the source's column 0/width edge, the POLE
// actually widens to a rotational average rather than reading one arbitrary
// tap, and the two directions are genuine inverses of one another rather
// than two unrelated remaps that happen to share a dialog.
namespace np {
namespace {

constexpr float kPi = 3.14159265358979323846f;

TileStore polarFlatField(int32_t sizePx, const std::array<float, 4>& v) {
  TileStore tiles;
  const int32_t tilesPerSide = (sizePx + kTileSize - 1) / kTileSize;
  for (int32_t ty = 0; ty < tilesPerSide; ++ty)
    for (int32_t tx = 0; tx < tilesPerSide; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t y = 0; y < kTileSize; ++y)
        for (int32_t x = 0; x < kTileSize; ++x) t.writePixel(PixelCoord{x, y}, v);
    }
  return tiles;
}

// A pure function of COLUMN, smooth AND exactly periodic over `sizePx` --
// value(0) == value(sizePx) by construction, which is what makes it able to
// tell a wrapping sampler from a zero-padding one: the two agree everywhere
// except within one texel of the seam, where a zero-padding sampler reads
// the true value blended with silence instead of the true value blended with
// its periodic continuation.
TileStore polarColumnCosineField(int32_t sizePx) {
  TileStore tiles;
  const int32_t tilesPerSide = (sizePx + kTileSize - 1) / kTileSize;
  for (int32_t ty = 0; ty < tilesPerSide; ++ty)
    for (int32_t tx = 0; tx < tilesPerSide; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t y = 0; y < kTileSize; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          const float col = static_cast<float>(tx * kTileSize + x);
          const float v =
              0.5f + 0.5f * std::cos(2.0f * kPi * col / static_cast<float>(sizePx));
          t.writePixel(PixelCoord{x, y}, {v, v, v, 1.0f});
        }
      }
    }
  return tiles;
}

// Alternates between two far-apart values on a period of 3 columns --
// deliberately not a smooth field, because the pole check below wants "the
// average of several very different taps", not "several taps that happened
// to be similar already". Period 3 rather than 2: the 8 pole taps below sit
// at columns spaced `sizePx / 8` apart, and a period that divides that
// spacing evenly would alias every tap onto the SAME phase, which a period
// of 3 (coprime with 8) cannot do -- the eight taps visit all three phases.
TileStore polarColumnCheckerField(int32_t sizePx) {
  TileStore tiles;
  const int32_t tilesPerSide = (sizePx + kTileSize - 1) / kTileSize;
  for (int32_t ty = 0; ty < tilesPerSide; ++ty)
    for (int32_t tx = 0; tx < tilesPerSide; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t y = 0; y < kTileSize; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          const int32_t col = tx * kTileSize + x;
          const float v = (col % 3 == 0) ? 0.0f : 1.0f;
          t.writePixel(PixelCoord{x, y}, {v, v, v, 1.0f});
        }
      }
    }
  return tiles;
}

std::array<float, 4> polarReadAt(const TileStore& s, int32_t x, int32_t y) {
  const Tile* t = s.find(tileCoordAt(PixelCoord{x, y}));
  if (t == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
  return t->readPixel(tileLocalOffset(PixelCoord{x, y}));
}

}  // namespace

bool runPolarRemapTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-64s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] polar remap: docs/operations.md section 3, Polar Coordinates\n");

  std::printf("  -- A. Engine contract --\n");
  {
    TileStore src = polarFlatField(64, {0.4f, 0.4f, 0.4f, 1.0f});
    const PixelRect whole{0, 0, 64, 64};
    TileStore dst;
    check(!polarRemapTiles(src, whole, PolarRemapParams{}, nullptr),
          "polar: a null destination is refused");
    check(!polarRemapTiles(src, whole, PolarRemapParams{}, &src),
          "polar: a destination aliasing the source is refused");
    check(!polarRemapTiles(src, PixelRect{10, 10, 10, 10}, PolarRemapParams{}, &dst),
          "polar: an empty outRect is refused");
    check(polarRemapDirectionFromName("rect_to_polar") == PolarRemapDirection::RectToPolar &&
              polarRemapDirectionFromName("polar_to_rect") == PolarRemapDirection::PolarToRect &&
              !polarRemapDirectionFromName("sideways").has_value(),
          "polar: direction names round-trip and an unknown one is refused");
  }

  std::printf("  -- B. A constant field stays constant, both directions --\n");
  {
    const TileStore flat = polarFlatField(64, {0.2f, 0.5f, 0.7f, 1.0f});
    const PixelRect whole{0, 0, 64, 64};
    for (const PolarRemapDirection dir :
        {PolarRemapDirection::RectToPolar, PolarRemapDirection::PolarToRect}) {
      TileStore out;
      polarRemapTiles(flat, whole, PolarRemapParams{dir}, &out);
      bool allConstant = true;
      // Only the disc PolarRemap actually covers is provably constant: a
      // corner outside the inscribed circle (Rect->Polar's own dest has none;
      // Polar->Rect's does, and is transparent by design -- ops/PolarRemap
      // .hpp's own header comment).
      const float cx = 32.0f, cy = 32.0f, rMax = 32.0f;
      for (int32_t y = 0; y < 64; ++y) {
        for (int32_t x = 0; x < 64; ++x) {
          if (dir == PolarRemapDirection::PolarToRect) {
            const float dx = static_cast<float>(x) + 0.5f - cx;
            const float dy = static_cast<float>(y) + 0.5f - cy;
            if (std::sqrt(dx * dx + dy * dy) > rMax - 1.0f) continue;
          }
          // `TileStore` is half-float (core/TileStore.hpp) -- a source texel
          // round-trips through bilinear taps that are themselves read back
          // out of half precision, and rgba16float's ULP near 0.7 is
          // already ~5e-4. 2e-3 is generous against that, not against a
          // logic bug: the seam and pole checks below hold their sources to
          // the SAME storage and use tolerances no looser.
          const std::array<float, 4> v = polarReadAt(out, x, y);
          if (std::fabs(v[0] - 0.2f) > 2e-3f || std::fabs(v[1] - 0.5f) > 2e-3f ||
              std::fabs(v[2] - 0.7f) > 2e-3f)
            allConstant = false;
        }
      }
      check(allConstant, dir == PolarRemapDirection::RectToPolar
                             ? "polar: Rect->Polar of a flat field is the identical flat field"
                             : "polar: Polar->Rect of a flat field is the identical flat field "
                               "inside the disc");
    }
  }

  std::printf("  -- C. The angular seam wraps rather than zero-padding --\n");
  {
    // Smooth AND periodic: value(0) == value(sizePx) exactly, so a correct
    // wrap and a zero-padding failure only disagree within one texel of the
    // seam -- which is exactly where this probes.
    const TileStore src = polarColumnCosineField(64);
    const PixelRect whole{0, 0, 64, 64};
    TileStore out;
    polarRemapTiles(src, whole, PolarRemapParams{PolarRemapDirection::PolarToRect}, &out);
    // The destination texel straddling the negative x-axis from the centre
    // (32, 32): dx < 0, dy in {0, -1} (this build's y-down convention puts
    // "just above" at dy = -1, "just on" at dy = 0), both at radius far
    // outside the pole's supersampled disc. atan2 is discontinuous exactly
    // here -- +pi on one side, just over -pi on the other -- which is what
    // makes this the one place a broken sampler is forced to show its hand:
    // one side reads a source column near the very end of the row, the
    // other reads a column near the very start, and only a wrap ties them
    // back together into the same smooth curve.
    const std::array<float, 4> onAxis = polarReadAt(out, 12, 32);     // dx=-20, dy=0 -> angle = pi
    const std::array<float, 4> justAbove = polarReadAt(out, 12, 31);  // dx=-20, dy=-1
    // Both sample within a texel of column 0 (u wraps to ~0) or column 63 (u
    // ~1) of a field that is IDENTICAL there by construction -- so the two
    // dest texels, physically adjacent, must read nearly the same value. A
    // sampler that zero-pads instead of wrapping would instead show one of
    // them pulled toward 0 (the out-of-bounds fill), which this field's true
    // value (near 1.0, cosine's own peak at the seam) is not.
    const float diff = std::fabs(onAxis[0] - justAbove[0]);
    check(diff < 0.05f,
          "polar: two dest texels straddling the seam read nearly the same value");
    check(onAxis[0] > 0.8f,
          "polar: the seam reads close to the source's true periodic value (near cosine's peak), "
          "not pulled toward a zero-padded edge");
  }

  std::printf("  -- D. The pole widens to a rotational average, not one arbitrary tap --\n");
  {
    // The 8 pole taps at (32, 32) -- dx=dy=0.5, radius 0.707, inside the
    // supersample disc -- land at source columns 0, 8, .., 56, five of
    // which are `col % 3 != 0` (value 1.0) and three `== 0` (value 0.0):
    // an average of 5/8 = 0.625. The pixel is not exactly at the pole, so
    // the result also blends in the single ordinary tap at its own angle
    // (weight ~0.35, one texel of value 1.0 here) -- worked out in full,
    // 0.625*0.6465 + 1.0*0.3535 =~ 0.76. What a single ARBITRARY tap with no
    // averaging at all would read instead is one of the source's only two
    // values, 0.0 or 1.0 exactly -- which 0.76 is comfortably not.
    const TileStore src = polarColumnCheckerField(64);
    const PixelRect whole{0, 0, 64, 64};
    TileStore out;
    polarRemapTiles(src, whole, PolarRemapParams{PolarRemapDirection::PolarToRect}, &out);
    const std::array<float, 4> centre = polarReadAt(out, 32, 32);
    check(centre[0] > 0.3f && centre[0] < 0.9f,
          "polar: the near-centre texel reads a rotational average, not a single extreme tap");
  }

  std::printf("  -- E. Rect->Polar and Polar->Rect are genuine inverses --\n");
  {
    // A field with real angular AND radial structure -- see this file's own
    // reason (section D's comment on why a flat/ramp fixture proves nothing).
    TileStore src;
    {
      const int32_t tilesPerSide = (96 + kTileSize - 1) / kTileSize;
      for (int32_t ty = 0; ty < tilesPerSide; ++ty)
        for (int32_t tx = 0; tx < tilesPerSide; ++tx) {
          Tile& t = src.getOrCreate(TileCoord{tx, ty});
          for (int32_t y = 0; y < kTileSize; ++y) {
            for (int32_t x = 0; x < kTileSize; ++x) {
              const float gx = static_cast<float>(tx * kTileSize + x);
              const float gy = static_cast<float>(ty * kTileSize + y);
              const float dx = gx - 48.0f, dy = gy - 48.0f;
              const float r = std::sqrt(dx * dx + dy * dy) / 48.0f;
              const float a = std::atan2(dy, dx);
              const float v = 0.5f + 0.25f * std::sin(3.0f * a) + 0.25f * r;
              t.writePixel(PixelCoord{x, y}, {v, v, v, 1.0f});
            }
          }
        }
    }
    const PixelRect whole{0, 0, 96, 96};
    TileStore polar, back;
    polarRemapTiles(src, whole, PolarRemapParams{PolarRemapDirection::RectToPolar}, &polar);
    polarRemapTiles(polar, whole, PolarRemapParams{PolarRemapDirection::PolarToRect}, &back);

    // Compared only inside the disc, away from the pole (the supersample
    // widening is a deliberate, documented departure from a point sample
    // there) and away from the disc's own edge (a half-texel of resample
    // softening at a hard boundary is not what this proves). What IS proven
    // by the middle annulus agreeing is that the two formulae are actual
    // inverses of one another on the frame they share, not merely that
    // "some picture came back" -- a bug that swapped sin/cos or dropped the
    // -pi offset would still produce a plausible-looking image here and
    // still fail this comparison everywhere.
    double sumAbsDiff = 0.0;
    int64_t sampleCount = 0;
    for (int32_t y = 0; y < 96; ++y) {
      for (int32_t x = 0; x < 96; ++x) {
        const float dx = static_cast<float>(x) + 0.5f - 48.0f;
        const float dy = static_cast<float>(y) + 0.5f - 48.0f;
        const float r = std::sqrt(dx * dx + dy * dy);
        if (r < 10.0f || r > 40.0f) continue;
        const std::array<float, 4> a = polarReadAt(src, x, y);
        const std::array<float, 4> b = polarReadAt(back, x, y);
        sumAbsDiff += std::fabs(a[0] - b[0]);
        ++sampleCount;
      }
    }
    const double meanAbsDiff = sampleCount > 0 ? sumAbsDiff / static_cast<double>(sampleCount) : 1.0;
    check(sampleCount > 1000, "polar: the compared annulus is not degenerately small");
    // A generous bound against a signal whose own structured amplitude is
    // 0.5 (docs/operations.md itself scopes this engine to a single
    // bilinear reconstruction pass each way, not the full Lanczos3/Mitchell
    // prefilter kernel set it names as future work for the pole) -- this
    // proves the two directions are genuine inverses, not that the pair is
    // lossless.
    check(meanAbsDiff < 0.025,
          "polar: Rect->Polar then Polar->Rect recovers the original away from the pole and "
          "the disc edge");
  }

  return ok;
}

}  // namespace np
