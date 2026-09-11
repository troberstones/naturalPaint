#include "brush/TipMips.hpp"

#include <algorithm>
#include <cstdint>

namespace np {
namespace {

// One level's own 2x2 box downsample of `src`, dimensions rounded UP (see
// this file's header for why), sources edge-clamped into `[0,src.width) x
// [0,src.height)`. `src` and the returned level share `BrushTipBitmap`'s
// layout, which is the whole reason `buildTipMips()` below can chain this
// function against its own previous output.
BrushTipBitmap downsampleOnce(const BrushTipBitmap& src) {
  BrushTipBitmap out;
  out.width = std::max(int32_t{1}, (src.width + 1) / 2);
  out.height = std::max(int32_t{1}, (src.height + 1) / 2);
  out.alpha.resize(static_cast<size_t>(out.width) * static_cast<size_t>(out.height));

  const auto at = [&](int32_t x, int32_t y) -> uint32_t {
    x = std::clamp(x, int32_t{0}, src.width - 1);
    y = std::clamp(y, int32_t{0}, src.height - 1);
    return src.alpha[static_cast<size_t>(y) * static_cast<size_t>(src.width) +
                     static_cast<size_t>(x)];
  };

  for (int32_t y = 0; y < out.height; ++y) {
    for (int32_t x = 0; x < out.width; ++x) {
      const int32_t sx = x * 2;
      const int32_t sy = y * 2;
      const uint32_t sum =
          at(sx, sy) + at(sx + 1, sy) + at(sx, sy + 1) + at(sx + 1, sy + 1);
      // Integer mean, rounded to nearest (`+2` before the `/4`) rather than
      // truncated -- so a uniform source (every one of the 4 sampled texels
      // equal) reproduces itself exactly instead of drifting down by up to
      // 3/4 of a level over a long chain, and so the hand-computed fixture
      // values in `app/selftest/TipEdge.cpp` are exact integers rather than
      // an implementation-defined rounding mode's guess.
      out.alpha[static_cast<size_t>(y) * static_cast<size_t>(out.width) +
               static_cast<size_t>(x)] = static_cast<uint8_t>((sum + 2) / 4);
    }
  }
  return out;
}

}  // namespace

void buildTipMips(BrushTipBitmap& bmp) {
  bmp.mips.clear();
  if (bmp.width <= 0 || bmp.height <= 0 ||
      bmp.alpha.size() !=
          static_cast<size_t>(bmp.width) * static_cast<size_t>(bmp.height))
    return;

  const BrushTipBitmap* prev = &bmp;
  while (prev->width > 1 || prev->height > 1) {
    bmp.mips.push_back(downsampleOnce(*prev));
    // Re-fetched AFTER the push: `push_back` may reallocate `bmp.mips`,
    // which would leave a pointer captured before it dangling. Nothing
    // between this line and the next iteration's read of `prev` touches the
    // vector, so this is the only pointer into it ever dereferenced.
    prev = &bmp.mips.back();
  }
}

}  // namespace np
