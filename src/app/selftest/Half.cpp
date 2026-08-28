#include "app/selftest/Support.hpp"

#include <cmath>

#include "core/Half.hpp"

namespace np {

// ---------------------------------------------------------------------------
// core/Half: the hardware convert against the software one it replaced
// (docs/architecture-review.md P0-1).
//
// **This file exists because a verification was performed once and then
// recorded as prose.** `core/Half.hpp`'s original comment said both directions
// "were checked bit-exact against the hardware `_Float16` path on this machine
// across all 2^32 `float` inputs and all 2^16 half inputs". That was true, and
// it was a real piece of work -- but it was true of a moment, not of the tree,
// and the permanent regression sample it pointed at (in the TileStore test) is
// a handful of values: zero, negative zero, a couple of subnormals, ordinary
// values, and the extremes.
//
// P0-1 replaces the software routine with the CPU's own convert on every
// target that has one, which turns that sentence from a historical note into
// the load-bearing justification for the substitution. So the sweep runs on
// every build now. The software implementation was deliberately KEPT (as
// `detail::halfToFloatSoftware` / `detail::floatToHalfSoftware`) rather than
// deleted, precisely so there is something to sweep against -- it is both the
// portable fallback and the oracle.
//
// **What is NOT claimed: bit-equality on NaN.** The software `floatToHalf`
// forces mantissa bit 0x0200 on a NaN input so the result cannot collapse into
// infinity; the hardware convert emits the platform's own quiet-NaN payload.
// Section C asserts both produce *a* NaN and says so out loud, rather than
// quietly excluding NaN from the sweep and leaving a reader to wonder whether
// it was covered.
//
// Headless and GPU-free: two pure functions and a loop.
// ---------------------------------------------------------------------------
bool runHalfTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] core/Half: hardware convert vs the software oracle\n");
  std::printf("  this build converts in %s\n",
              kHalfConversionIsHardware ? "HARDWARE (fcvt / F16C)" : "SOFTWARE (portable fallback)");

  // A build that silently fell back to software is two orders of magnitude
  // slower and otherwise indistinguishable from outside, which is exactly the
  // silent-degradation shape ui/Fonts.cpp's missing-font path already taught
  // this project to make loud. On the platforms this project targets the
  // hardware path is always available, so its absence is a build problem.
#if defined(__aarch64__) || defined(__x86_64__)
  check(kHalfConversionIsHardware,
        "path: this build uses the CPU's own half convert -- on aarch64 or x86-64 a "
        "software fallback means a missing compiler flag, not a missing instruction");
#endif

  // --- A. halfToFloat: ALL 65,536 half values, exhaustively ----------------
  //
  // Not a sample. The domain is small enough to enumerate, so enumerating it
  // is strictly better than choosing interesting values and hoping the
  // uninteresting ones are uninteresting.
  {
    int mismatches = 0, nanPairs = 0, firstBad = -1;
    for (uint32_t i = 0; i <= 0xFFFFu; ++i) {
      const uint16_t h = static_cast<uint16_t>(i);
      const float hw = halfToFloat(h);
      const float sw = detail::halfToFloatSoftware(h);
      if (std::isnan(hw) || std::isnan(sw)) {
        // Both must agree that it IS a NaN; the payload is not compared, for
        // the reason this file's header gives.
        if (std::isnan(hw) && std::isnan(sw)) ++nanPairs;
        else if (firstBad < 0) firstBad = static_cast<int>(i);
        continue;
      }
      if (std::bit_cast<uint32_t>(hw) != std::bit_cast<uint32_t>(sw)) {
        ++mismatches;
        if (firstBad < 0) firstBad = static_cast<int>(i);
      }
    }
    std::printf("    halfToFloat: 65536 values swept, %d NaN pairs, %d mismatches\n", nanPairs,
                mismatches);
    check(mismatches == 0 && firstBad < 0,
          "halfToFloat: every one of the 65536 half values converts BIT-IDENTICALLY to what "
          "the software routine produced -- exhaustive, not sampled");
    // Without this, a `halfToFloat` that returned NaN for everything would
    // sweep clean: every pair would land in the NaN branch and be skipped.
    check(nanPairs == 2046,
          "halfToFloat: and exactly 2046 of them are NaN (both exponent-31 mantissa-nonzero "
          "runs, 1023 per sign) -- so the sweep above is not passing by classifying "
          "everything as an unchecked NaN");
  }

  // --- B. floatToHalf: a structured sweep, not a random one ----------------
  //
  // 2^32 floats is minutes, not milliseconds, so this is a sample -- but a
  // sample chosen by STRUCTURE rather than by randomness: every half value's
  // exact float, both neighbours of every representable half (the ties the
  // round-to-nearest-even rule is about), the subnormal boundary, and the
  // overflow-after-rounding case the software routine has a dedicated branch
  // for. A uniform random sweep would spend almost all its draws on ordinary
  // normals and almost none on the branches that are hard to get right.
  {
    int mismatches = 0, checked = 0, skippedNaN = 0, firstBad = -1;
    auto compare = [&](float f) {
      // NaN belongs to section C, which asserts the ONE documented difference
      // rather than bit-equality. Skipping it here is not sweeping it under the
      // rug -- and the count is printed, because the first version of this
      // sweep did NOT skip it and reported two mismatches that turned out to be
      // the test's own fault. Stepping one ulp in BIT space leaves the reals at
      // two places: below zero (0x00000000 - 1 wraps to 0xFFFFFFFF) and above
      // infinity (0x7F800000 + 1 is the first NaN). The conversion was right
      // and the sweep was wrong, which is the more common way round, and is why
      // the skip count is printed and pinned rather than silently absorbed.
      if (std::isnan(f)) {
        ++skippedNaN;
        return;
      }
      ++checked;
      const uint16_t hw = floatToHalf(f);
      const uint16_t sw = detail::floatToHalfSoftware(f);
      if (hw != sw) {
        ++mismatches;
        if (firstBad < 0) firstBad = checked;
      }
    };
    for (uint32_t i = 0; i <= 0xFFFFu; ++i) {
      const float exact = detail::halfToFloatSoftware(static_cast<uint16_t>(i));
      if (std::isnan(exact)) continue;  // NaN payload: section C, not here
      compare(exact);
      // The two float neighbours of every representable half. These are where
      // ties-to-even is decided, and they are the values a rounding bug moves.
      const uint32_t bits = std::bit_cast<uint32_t>(exact);
      compare(std::bit_cast<float>(bits + 1u));
      compare(std::bit_cast<float>(bits - 1u));
      // The exact midpoint to the next half up -- the tie itself.
      const float nextUp = detail::halfToFloatSoftware(static_cast<uint16_t>((i + 1) & 0xFFFFu));
      if (!std::isnan(nextUp) && !std::isinf(nextUp) && !std::isinf(exact))
        compare((exact + nextUp) * 0.5f);
    }
    // The named edges, spelled out rather than hoped for above.
    for (const float f : {0.0f, -0.0f, 1.0f, -1.0f, 65504.0f, -65504.0f, 65520.0f, 65536.0f,
                          6.1035156e-05f, 5.9604645e-08f, 2.9802322e-08f, 1.4901161e-08f,
                          1e-45f, -1e-45f, 3.4028235e38f, -3.4028235e38f,
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity()})
      compare(f);

    std::printf("    floatToHalf: %d structured values swept, %d mismatches, %d NaN skipped\n",
                checked, mismatches, skippedNaN);
    check(mismatches == 0 && firstBad < 0,
          "floatToHalf: every value in the structured sweep -- each half's exact float, both "
          "float neighbours of each, every tie midpoint, and the named edges -- rounds "
          "BIT-IDENTICALLY to what the software routine produced");
    check(checked > 200000,
          "floatToHalf: and the sweep really covered a quarter-million values, so the "
          "line above is not reporting zero mismatches over an empty loop");
    check(skippedNaN == 4,
          "floatToHalf: exactly four inputs were skipped as NaN -- one ulp below each zero "
          "and one ulp above each infinity, both artefacts of stepping in bit space rather "
          "than in the reals. Pinned so a future edit cannot start silently skipping real "
          "values as NaN and still report zero mismatches");
  }

  // --- C. NaN: the one documented difference, asserted rather than hidden --
  {
    const float qnan = std::numeric_limits<float>::quiet_NaN();
    const uint16_t hw = floatToHalf(qnan);
    const uint16_t sw = detail::floatToHalfSoftware(qnan);
    const auto isHalfNaN = [](uint16_t h) {
      return ((h >> 10) & 0x1Fu) == 0x1Fu && (h & 0x3FFu) != 0u;
    };
    std::printf("    floatToHalf(NaN): hardware 0x%04X, software 0x%04X\n", hw, sw);
    check(isHalfNaN(hw) && isHalfNaN(sw),
          "NaN: both paths produce a half NaN -- neither collapses a NaN into infinity, "
          "which is the property the software path's forced 0x0200 mantissa bit exists for");
  }

  std::printf("[selftest] core/Half %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
