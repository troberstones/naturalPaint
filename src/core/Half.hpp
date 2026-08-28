#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

// IEEE 754 binary16 ("half") <-> binary32 (`float`) conversion.
//
// ==========================================================================
// Why this is header-only, and what it cost to not be
// ==========================================================================
//
// docs/architecture-review.md **P0-1**. These two functions are called four
// times per pixel per read and four per write, across 264 `readPixel`/
// `writePixel` call sites and 117 direct ones -- which is to say, they are on
// the innermost loop of every filter, every composite, every export and every
// save in the application. Until this change they were **out-of-line, in
// their own translation unit**, so every one of those calls was a real call
// that the optimiser could not inline, could not hoist, and above all could
// not VECTORISE across: a loop whose body calls into another TU stays scalar
// no matter how simple the body is.
//
// Measured on this machine (Apple M4 Max) before the change, over a
// 4096x4096 RGBA-f16 buffer -- 16.8 Mtexel, the review's own fixture --
// running `h = floatToHalf(halfToFloat(h) * 1.25f)` over every half word:
//
//     out-of-line software conversion   221.4 ms   0.08 Gtexel/s
//     `_Float16`, same loop shape         1.8 ms   9.09 Gtexel/s
//
// **120x**, and the accumulated check value over the result buffer was
// identical to the last bit in both runs. The review predicted 18x for this
// step; the gap is that its row B was still scalar, while the loop here
// autovectorises the moment the conversion stops being a call -- so most of
// the review's separately-listed "SIMD the compiler could not use" arrives
// here too, for free, as a consequence of inlining rather than as work.
//
// The hardware instruction has existed for a long time on both targets this
// project can run on: AArch64 has `fcvt` (and `vcvt_f32_f16` for four at a
// time), and x86-64 has had F16C's `vcvtph2ps` since 2013. The software
// routine below was emulating, with a branch and -- in the subnormal path --
// a `while` loop, something the CPU does in one instruction.
//
// ==========================================================================
// The software path is KEPT, and it is now a checked claim rather than a note
// ==========================================================================
//
// It stays for two reasons. It is the portable fallback for a target with
// neither `_Float16` nor F16C. And it is the **oracle**: the original header
// recorded that these routines had been "checked bit-exact against the
// hardware `_Float16` path on this machine across all 2^32 `float` inputs and
// all 2^16 half inputs" -- a real verification, done once, by a person, and
// then true only of the moment it was done. Deleting the software path would
// have deleted the only thing that claim could ever be re-checked against.
//
// `app/selftest/Half.cpp` now runs that sweep on every build: all 65,536 half
// values through both `halfToFloat` paths, and a large structured float sweep
// through both `floatToHalf` paths, asserting bit-equality. So the
// substitution this file performs is not asserted, it is proved, every time.
//
// **The one documented difference is the NaN payload.** The software
// `floatToHalf` forces mantissa bit 0x0200 on any NaN input so the result
// cannot collapse to infinity; the hardware convert produces the platform's
// own quiet-NaN payload. Both produce *a* NaN, and the selftest asserts
// exactly that for NaN inputs rather than bit-equality. Nothing in this
// codebase stores a meaningful NaN payload in a pixel -- see `core/Tile.hpp`
// on what a texel is allowed to contain -- so the distinction is recorded
// rather than resolved.
namespace np {
namespace detail {

// The portable implementation, and the oracle the hardware path is checked
// against. Both directions round-to-nearest-even and cover the full range:
// zero and negative zero, subnormals in both formats, ordinary normals,
// overflow to infinity (including values that only overflow *after* rounding,
// e.g. a float mantissa that rounds a half's exponent past its max), and
// NaN/Inf passthrough.
inline float halfToFloatSoftware(uint16_t h) noexcept {
  const uint32_t sign = (h & 0x8000u) << 16;
  uint32_t exp = (h >> 10) & 0x1Fu;
  uint32_t man = h & 0x3FFu;

  if (exp == 0) {
    if (man == 0) {
      const uint32_t bits = sign;
      float f; std::memcpy(&f, &bits, 4); return f;
    }
    // subnormal: renormalise
    exp = 1;
    while ((man & 0x400u) == 0) { man <<= 1; --exp; }
    man &= 0x3FFu;
    const uint32_t bits = sign | ((exp + 112u) << 23) | (man << 13);
    float f; std::memcpy(&f, &bits, 4); return f;
  }
  if (exp == 31) {  // inf / nan
    const uint32_t bits = sign | 0x7F800000u | (man << 13);
    float f; std::memcpy(&f, &bits, 4); return f;
  }
  const uint32_t bits = sign | ((exp + 112u) << 23) | (man << 13);
  float f; std::memcpy(&f, &bits, 4); return f;
}

inline uint16_t floatToHalfSoftware(float value) noexcept {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);

  const uint32_t sign = (bits >> 16) & 0x8000u;
  const uint32_t exp8 = (bits >> 23) & 0xFFu;
  uint32_t man23 = bits & 0x7FFFFFu;

  if (exp8 == 0xFFu) {
    // Infinity (man23 == 0) or NaN. For NaN, force a set mantissa bit so
    // the result can't collapse into infinity; the exact payload isn't
    // meaningful here (source is either f32 arithmetic-produced or, for
    // this codebase, effectively never a NaN in the first place). This is
    // the ONE place the hardware path differs -- see this header's top.
    return static_cast<uint16_t>(sign | 0x7C00u | (man23 != 0 ? 0x0200u : 0u));
  }

  if (exp8 == 0) {
    // Float zero or subnormal. Even the largest float subnormal (just
    // under 2^-126) is far below the halfway point to half's smallest
    // subnormal (2^-25) -- the mantissa bits can't affect the outcome, so
    // this always flushes to (signed) zero.
    return static_cast<uint16_t>(sign);
  }

  // Rebias the exponent from float's bias (127) to half's (15). This is a
  // *candidate* half exponent -- still to be range-checked and adjusted
  // for rounding below.
  int32_t exp = static_cast<int32_t>(exp8) - 127 + 15;

  if (exp >= 31) {
    // Already past half's max exponent before rounding is even considered.
    return static_cast<uint16_t>(sign | 0x7C00u);
  }

  if (exp <= 0) {
    // Too small for a normal half; either a half subnormal or a flush to
    // zero, decided by rounding below.
    if (exp < -10) {
      // Even the roundup threshold for the smallest subnormal (2^-25) is
      // above this value's magnitude -- and, critically, a shift this
      // large is undefined behaviour on a 32-bit value, so bail out
      // before computing one.
      return static_cast<uint16_t>(sign);
    }
    man23 |= 0x800000u;  // restore the implicit leading 1
    // Shift the 24-bit (leading-1-included) mantissa right so it lands as
    // a subnormal half mantissa: 13 bits for the normal-to-half shift,
    // plus one more bit for every step exp sits below 1.
    const uint32_t shift = static_cast<uint32_t>(14 - exp);  // in [14, 24]
    uint32_t man = man23 >> shift;
    const uint32_t roundBit = 1u << (shift - 1);
    const uint32_t rest = man23 & ((roundBit << 1) - 1u);
    if (rest > roundBit || (rest == roundBit && (man & 1u))) {
      ++man;  // round to nearest, ties to even; may carry into bit 10,
              // which is fine -- that's exp field value 1, a normal half
              // one exponent above subnormal, exactly where it belongs.
    }
    return static_cast<uint16_t>(sign | man);
  }

  // Normal half result: top 10 bits of the 23-bit float mantissa, rounded
  // to nearest even using the 13 bits being discarded.
  uint32_t man = man23 >> 13;
  const uint32_t rest = man23 & 0x1FFFu;
  constexpr uint32_t kRoundBit = 0x1000u;  // bit 12: the exact halfway point
  if (rest > kRoundBit || (rest == kRoundBit && (man & 1u))) {
    ++man;
    if (man == 0x400u) {
      // Mantissa rounded up past 10 bits -- carries into the exponent.
      man = 0;
      ++exp;
      if (exp >= 31) {
        return static_cast<uint16_t>(sign | 0x7C00u);  // rounded into infinity
      }
    }
  }
  return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) | man);
}

}  // namespace detail

// True when the two functions below compile to the CPU's own convert rather
// than to `detail::`'s software emulation. Exposed so `--selftest` can print
// which path a given build actually took -- a build that silently fell back
// to software would otherwise look identical from the outside while running
// two orders of magnitude slower.
#if defined(__ARM_FP16_FORMAT_IEEE) || defined(__F16C__) || defined(__AVX512FP16__)
inline constexpr bool kHalfConversionIsHardware = true;
#else
inline constexpr bool kHalfConversionIsHardware = false;
#endif

// binary16 -> binary32. Exact for every half value (every half is exactly
// representable as a float).
inline float halfToFloat(uint16_t h) noexcept {
  if constexpr (kHalfConversionIsHardware)
    return static_cast<float>(std::bit_cast<_Float16>(h));
  else
    return detail::halfToFloatSoftware(h);
}

// binary32 -> binary16, round-to-nearest-even (ties to even), same rounding
// rule IEEE 754 arithmetic itself uses -- and the same rule `_Float16`'s own
// conversion follows, which is why the two agree bit-for-bit on every
// non-NaN input.
inline uint16_t floatToHalf(float f) noexcept {
  if constexpr (kHalfConversionIsHardware)
    return std::bit_cast<uint16_t>(static_cast<_Float16>(f));
  else
    return detail::floatToHalfSoftware(f);
}

}  // namespace np
