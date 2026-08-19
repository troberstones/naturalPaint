#include "core/Half.hpp"

#include <cstring>

namespace np {

float halfToFloat(uint16_t h) noexcept {
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

uint16_t floatToHalf(float value) noexcept {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);

  const uint32_t sign = (bits >> 16) & 0x8000u;
  const uint32_t exp8 = (bits >> 23) & 0xFFu;
  uint32_t man23 = bits & 0x7FFFFFu;

  if (exp8 == 0xFFu) {
    // Infinity (man23 == 0) or NaN. For NaN, force a set mantissa bit so
    // the result can't collapse into infinity; the exact payload isn't
    // meaningful here (source is either f32 arithmetic-produced or, for
    // this codebase, effectively never a NaN in the first place).
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

}  // namespace np
