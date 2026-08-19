#pragma once

#include <cstdint>

// IEEE 754 binary16 ("half") <-> binary32 (`float`) conversion, written out
// by hand rather than relying on `_Float16` (available on this arm64
// toolchain, but not a type the rest of the codebase uses, and not
// universally portable) so every build does the conversion the same way.
//
// Originally private to sim/PaintSim.cpp, which decodes GPU-computed f16
// buffers back to float for diagnostics. Factored out here once
// core/Tile's pixel storage needed the encode direction too (rgba16float
// per DESIGN-imaging.md §2) -- keeping a second hand-rolled copy of this
// precision-critical bit manipulation around would just let the two drift
// apart. PaintSim.cpp now calls the version here.
//
// Both directions round-to-nearest-even and cover the full range: zero and
// negative zero, subnormals in both formats, ordinary normals, overflow to
// infinity (including values that only overflow *after* rounding, e.g. a
// float mantissa that rounds a half's exponent past its max), and NaN/Inf
// passthrough. Before landing, both were checked bit-exact against the
// hardware `_Float16` path on this machine across all 2^32 `float` inputs
// (floatToHalf) and all 2^16 half inputs (halfToFloat) -- see
// app/SelfTest.cpp's TileStore test for the permanent, much smaller
// regression sample of that sweep (zero, negative zero, subnormals,
// ordinary values, and the highest/lowest finite half magnitudes).
namespace np {

// binary16 -> binary32. Exact for every half value (every half is exactly
// representable as a float).
float halfToFloat(uint16_t h) noexcept;

// binary32 -> binary16, round-to-nearest-even (ties to even), same
// rounding rule IEEE 754 arithmetic itself uses.
uint16_t floatToHalf(float f) noexcept;

}  // namespace np
