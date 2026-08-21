#include "app/selftest/Support.hpp"

namespace np {

// color/Shaper (Phase 3 step 1, ADR-0004). See SelfTest.hpp for the full
// breakdown; ADR-0004 flags this as the single hardest-to-reverse decision
// in the whole colour pipeline ("saved curve control points are coordinates
// in that domain, so changing the shaper later silently shifts every grade
// in every saved document"), so this gets the same rigor as
// runColorSpaceTest() above, plus an explicit breakpoint-continuity check
// that re-derives both formula branches independently of Shaper.cpp's own
// copy of the constants.
bool runShaperTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  constexpr float kTol = 1e-4f;
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // Re-typed directly from the published ACEScct spec (S-2016-005) -- the
  // same values color/Shaper.cpp uses, kept independent here (not included
  // from that file's anonymous namespace) so this test cannot pass by
  // construction just because it shares Shaper.cpp's own copy of them.
  constexpr float kBreakLin = 0.0078125f;  // 2^-7
  constexpr float kSlopeA = 10.5402377416545f;
  constexpr float kOffsetB = 0.0729055341958355f;
  constexpr float kLogA = 9.72f;
  constexpr float kLogB = 17.52f;

  // --- Continuity at the breakpoint: evaluate BOTH branch formulas
  // directly at lin = kBreakLin, not just calling shaperEncode() once. This
  // is the property that proves the published constants are genuinely
  // self-consistent (a smooth curve, not two segments that merely happen
  // to touch) -- see Shaper.cpp's header comment for the hand-worked
  // derivation this pins. ---
  const float linBranch = kSlopeA * kBreakLin + kOffsetB;
  const float logBranch = (std::log2(kBreakLin) + kLogA) / kLogB;
  std::printf("[selftest] shaper breakpoint: linear branch=%.10f  log branch=%.10f  "
              "shaperEncode(breakLin)=%.10f\n",
              linBranch, logBranch, shaperEncode(kBreakLin));
  check(near(linBranch, logBranch, 1e-6f),
        "shaper breakpoint: hand-evaluated linear and log branches agree in value");
  check(near(shaperEncode(kBreakLin), linBranch, 1e-6f),
        "shaperEncode(breakLin) matches the hand-evaluated linear branch");
  check(near(shaperEncode(kBreakLin), logBranch, 1e-6f),
        "shaperEncode(breakLin) matches the hand-evaluated log branch");

  // --- Round trip, both directions. Negative, zero, either side of the
  // breakpoint, 0.18 (18% grey), 1.0 exactly, and above 1.0 (2.0, 4.0,
  // 16.0) to prove the HDR-headroom property ADR-0004 asks for. ---
  const float linearValues[] = {-0.5f,  0.0f,        0.001f,           kBreakLin * 0.5f,
                                kBreakLin, kBreakLin * 2.0f, 0.18f,           1.0f,
                                2.0f,    4.0f,        16.0f};
  for (float x : linearValues) {
    char label[96];
    const float rt = shaperDecode(shaperEncode(x));
    std::snprintf(label, sizeof label, "shaper decode(encode(%.6f)) round-trips", x);
    check(near(rt, x, kTol), label);
  }
  // Encoded-domain spread for the inverse direction, including the
  // breakpoint itself (0.1552511415525113) and shaperEncode(1.0)
  // (0.5547945205479452) as landmark points.
  const float shapedValues[] = {-0.2f,  0.0f,       0.05f, 0.1552511415525113f,
                                0.3f,   0.5547945205479452f, 0.7f, 0.9f, 1.5f};
  for (float y : shapedValues) {
    char label[96];
    const float rt = shaperEncode(shaperDecode(y));
    std::snprintf(label, sizeof label, "shaper encode(decode(%.6f)) round-trips", y);
    check(near(rt, y, kTol), label);
  }

  // --- Known-value sanity check against a hand-computable reference point,
  // independent of the code's own internal consistency: at lin = 1.0 (above
  // the breakpoint), shaperEncode(1.0) = (log2(1)+9.72)/17.52 = 9.72/17.52. ---
  const float enc1 = shaperEncode(1.0f);
  std::printf("[selftest] shaperEncode(1.0) = %.10f (expected 9.72/17.52 = %.10f)\n", enc1,
              9.72f / 17.52f);
  check(near(enc1, 9.72f / 17.52f, kTol),
        "shaperEncode(1.0) lands near the hand-computed 9.72/17.52 (~0.5547945)");

  // --- Monotonicity: a sorted spread of linear inputs must produce a
  // sorted spread of shaped outputs, across and away from the breakpoint. A
  // non-monotonic log-domain shaper would silently break curve editing. ---
  const float sortedSpread[] = {-1.0f, -0.1f, 0.0f,      0.001f, kBreakLin, 0.05f,
                                0.18f, 0.5f,  1.0f,      2.0f,   8.0f,      16.0f,
                                64.0f};
  bool monotonic = true;
  for (size_t i = 1; i < sizeof(sortedSpread) / sizeof(sortedSpread[0]); ++i) {
    if (!(shaperEncode(sortedSpread[i]) > shaperEncode(sortedSpread[i - 1]))) monotonic = false;
  }
  check(monotonic, "shaperEncode is strictly increasing over a sorted sampled spread");

  std::printf("[selftest] shaper %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
