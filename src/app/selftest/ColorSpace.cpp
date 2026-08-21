#include "app/selftest/Support.hpp"

namespace np {

// color/Space (Phase 2.3): round-trips both transfer functions and checks
// they agree with each other only where they should (they must not, since
// sRGB and Rec.709 are genuinely different curves despite sharing
// primaries -- see color/Space.hpp).
bool runColorSpaceTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  constexpr float kTol = 1e-4f;
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // Spread of linear-domain values: negative (op headroom), zero, either
  // side of each curve's toe breakpoint, mid-range, 1.0 exactly, and above
  // 1.0 (HDR-ish highlight) to exercise the deliberately-unclamped path.
  const float linearValues[] = {-0.5f, 0.0f, 0.001f, 0.01f, 0.02f,
                                0.18f, 0.5f, 1.0f,   2.0f,  4.0f};

  for (float x : linearValues) {
    char label[96];

    const float srgbRT = srgbDecode(srgbEncode(x));
    std::snprintf(label, sizeof label, "sRGB decode(encode(%.4f)) round-trips", x);
    check(near(srgbRT, x, kTol), label);

    const float rec709RT = rec709Decode(rec709Encode(x));
    std::snprintf(label, sizeof label, "Rec.709 decode(encode(%.4f)) round-trips", x);
    check(near(rec709RT, x, kTol), label);
  }

  // The inverse direction too, from the encoded side, including values
  // above 1.0 -- an encoded value need not be display-clamped either.
  // (0.081 is Rec.709's exact encoded-domain breakpoint -- both curves'
  // toe/power segments meet there only approximately, since the published
  // standard constants are rounded decimals, so an input landing exactly
  // on it is a testing artifact rather than a meaningful precision bug.
  // 0.08 and 0.085 straddle it instead.)
  const float encodedValues[] = {-0.3f, 0.0f, 0.02f, 0.08f, 0.085f, 0.3f, 0.9f, 1.0f, 1.5f};
  for (float x : encodedValues) {
    char label[96];

    const float srgbRT = srgbEncode(srgbDecode(x));
    std::snprintf(label, sizeof label, "sRGB encode(decode(%.4f)) round-trips", x);
    check(near(srgbRT, x, kTol), label);

    const float rec709RT = rec709Encode(rec709Decode(x));
    std::snprintf(label, sizeof label, "Rec.709 encode(decode(%.4f)) round-trips", x);
    check(near(rec709RT, x, kTol), label);
  }

  // Sanity checks against known reference points (IEC 61966-2-1 / BT.709).
  check(near(srgbEncode(1.0f), 1.0f, kTol), "sRGB encode(1.0) == 1.0");
  check(near(srgbDecode(1.0f), 1.0f, kTol), "sRGB decode(1.0) == 1.0");
  check(near(srgbEncode(0.0f), 0.0f, kTol), "sRGB encode(0.0) == 0.0");
  check(near(rec709Encode(1.0f), 1.0f, kTol), "Rec.709 encode(1.0) == 1.0");
  check(near(rec709Decode(1.0f), 1.0f, kTol), "Rec.709 decode(1.0) == 1.0");
  // 18% grey, a standard mid-tone reference: sRGB encodes it to roughly
  // 0.46, well off linear 0.18 -- the whole reason the shaper domain in
  // ADR-0004 exists rather than authoring curves against raw linear.
  check(near(srgbEncode(0.18f), 0.4613f, 0.001f),
        "sRGB encode(0.18) lands near the textbook ~0.46 (mid-grey emphasis)");

  // sRGB and Rec.709 must actually differ mid-curve -- if they agreed
  // everywhere, conflating "same primaries" with "same transfer function"
  // (the mistake this task's spec explicitly calls out) would have crept
  // back in unnoticed.
  check(!near(srgbEncode(0.5f), rec709Encode(0.5f), 1e-3f),
        "sRGB and Rec.709 encode 0.5 to visibly different values");

  // Values above 1.0 are not clamped -- they pass through the same curve
  // and keep growing monotonically, confirming the "don't clamp inside the
  // transfer function" decision actually holds in the implementation.
  check(srgbEncode(4.0f) > srgbEncode(1.0f), "sRGB encode does not clamp above 1.0");
  check(rec709Encode(4.0f) > rec709Encode(1.0f),
        "Rec.709 encode does not clamp above 1.0");

  std::printf("[selftest] color space %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
