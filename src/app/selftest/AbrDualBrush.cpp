#include "app/selftest/Support.hpp"

#include <memory>

#include "app/BrushRowIcon.hpp"
#include "app/DabPreview.hpp"
#include "app/selftest/DescFixture.hpp"
#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "io/AbrBrushes.hpp"

namespace np {
namespace {

// --- Fixture builders -------------------------------------------------
//
// Local to this file, not shared with app/selftest/AbrBrushes.cpp's own
// `BrushSpec`/`appendBrush()` -- the precedent app/selftest/AbrSampledTips.cpp
// already set for a `.abr` fixture that needs a shape neither of the other
// two sections builds: each section's own descriptor is cheap enough to write
// by hand, and the shapes differ enough (this one needs a `dualBrush` object
// with its own nested `Brsh`, `BlnM`, `useScatter` and `Cnt `) that one
// shared struct would grow more optional fields than it would save lines.
// Every byte here is written by hand, the same discipline and the same
// caveat io/AbrBrushes.hpp's own header states: this proves the reader agrees
// with the format as documented, not with Photoshop.

struct DualTipSpec {
  bool present = false;
  const char* dmtrUnit = "#Pxl";
  double diameterPx = 30.0;  // a #Pxl VALUE, or a #Prc PERCENT when dmtrUnit is "#Prc"
  double angleDeg = 0.0;
  double roundnessPercent = 100.0;
  double spacingPercent = 25.0;
  const char* sampledDataId = nullptr;
};

struct DualBrushSpec {
  bool present = false;  // the `dualBrush` object itself
  bool on = false;       // `useDualBrush`
  bool haveBlend = false;
  const char* blendTypeId = "BlnM";
  const char* blendValueId = "Mltp";
  DualTipSpec tip;
  bool haveScatter = false;
  bool useScatter = false;
  bool haveCount = false;
  int32_t count = 1;
};

// The second tip's own `Brsh`-shaped object: `Dmtr`/`Angl`/`Rndn`/`Spcn`,
// always, plus `sampledData` when the spec asks for it -- the identical shape
// io/AbrBrushes.cpp's `readAbrTipShape()` reads for the primary tip AND for
// this one, per that function's own header comment on why it is shared.
void appendDualTip(DescFixture& f, const DualTipSpec& t) {
  const uint32_t items = 4u + (t.sampledDataId != nullptr ? 1u : 0u);
  f.objc("sampledBrush", "sampledBrush", items);
  f.key4("Dmtr").untf(t.dmtrUnit, t.diameterPx);
  f.key4("Angl").untf("#Ang", t.angleDeg);
  f.key4("Rndn").untf("#Prc", t.roundnessPercent);
  f.key4("Spcn").untf("#Prc", t.spacingPercent);
  if (t.sampledDataId != nullptr) f.keyN("sampledData").textv(t.sampledDataId);
}

// The `dualBrush` object: `useDualBrush` always, then whichever of `Brsh`,
// `BlnM`, `useScatter`, `Cnt ` the spec asks for -- deliberately not always
// all nine keys the real descriptor carries, so a fixture can isolate "no
// `Brsh` at all" (the old, still-supported fallback) from "a `Brsh` with no
// `BlnM`" from "a full second tip."
void appendDualBrush(DescFixture& f, const DualBrushSpec& d) {
  uint32_t items = 1;  // useDualBrush
  if (d.tip.present) ++items;
  if (d.haveBlend) ++items;
  if (d.haveScatter) ++items;
  if (d.haveCount) ++items;
  f.objc("dualBrush", "dualBrush", items);
  f.keyN("useDualBrush").boolv(d.on);
  if (d.tip.present) {
    f.key4("Brsh");
    appendDualTip(f, d.tip);
  }
  // `enumvLong()`, not `enumv()`: two of this file's own fixtures now use
  // `BlnM` value ids longer than four characters ("hardMix", "linearHeight"),
  // which `enumv()`'s `key4()`-only encoding would silently truncate --
  // `enumvLong()`'s own comment (app/selftest/DescFixture.hpp) is a strict
  // superset, so every existing 4-character value id here (`Mltp`, `Ovrl`,
  // `CBrn`, `Scrn`...) round-trips identically through it.
  if (d.haveBlend) f.key4("BlnM").enumvLong(d.blendTypeId, d.blendValueId);
  if (d.haveScatter) f.keyN("useScatter").boolv(d.useScatter);
  if (d.haveCount) f.key4("Cnt ").longv(d.count);
}

// One-brush `.abr` `desc` body: a name and, when `dual.present`, the
// `dualBrush` object -- deliberately no PRIMARY `Brsh` at all, since every
// section below is about the SECOND tip and `presetFromDescriptor()` already
// defends a missing primary `Brsh` (an invalid `DescriptorRef` reads as
// absent throughout, io/Descriptor.hpp's own header), so the primary tip
// simply keeps `BrushPreset`'s own defaults.
std::vector<uint8_t> oneDualBrushDesc(const char* name, const DualBrushSpec& dual) {
  DescFixture f;
  f.version();
  f.descriptor("null", "null", 1);
  f.key4("Brsh").vlls(1);
  uint32_t items = 1;  // Nm
  if (dual.present) ++items;
  f.objc("brushPreset", "brushPreset", items);
  f.key4("Nm  ").textv(name);
  if (dual.present) {
    f.keyN("dualBrush");
    appendDualBrush(f, dual);
  }
  return f.bytes;
}

std::vector<uint8_t> wrapAbrDesc(const std::vector<uint8_t>& descBody) {
  DescFixture f;
  f.u16v(6).u16v(2);
  f.code("8BIM").code("desc").u32v(static_cast<uint32_t>(descBody.size()));
  for (const uint8_t b : descBody) f.u8v(b);
  return f.bytes;
}

// One `samp` record's on-disk bytes -- identical framing to
// app/selftest/AbrSampledTips.cpp's own `buildSampRecord()`, kept local per
// that file's own precedent (this section's header comment above).
std::vector<uint8_t> buildSampRecord(const char* uuid36, uint16_t subversion, uint32_t top,
                                     uint32_t left, uint32_t bottom, uint32_t right,
                                     uint16_t depth, uint8_t compression,
                                     const std::vector<uint8_t>& imageBytes) {
  DescFixture body;
  body.u8v('$');
  for (const char* p = uuid36; *p != '\0'; ++p) body.u8v(static_cast<unsigned char>(*p));
  const size_t skipAmt = (subversion == 1) ? 47 : 301;
  for (size_t i = 37; i < skipAmt; ++i) body.u8v(0);
  body.u32v(top).u32v(left).u32v(bottom).u32v(right);
  body.u16v(depth);
  body.u8v(compression);
  for (const uint8_t b : imageBytes) body.u8v(b);

  DescFixture rec;
  rec.u32v(static_cast<uint32_t>(body.bytes.size()));
  for (const uint8_t b : body.bytes) rec.u8v(b);
  return rec.bytes;
}

std::vector<uint8_t> wrapAbrWithSampAndDesc(const std::vector<uint8_t>& sampBody,
                                            const std::vector<uint8_t>& descBody) {
  DescFixture f;
  f.u16v(6).u16v(2);
  f.code("8BIM").code("samp").u32v(static_cast<uint32_t>(sampBody.size()));
  for (const uint8_t b : sampBody) f.u8v(b);
  if (sampBody.size() % 2 != 0) f.u8v(0);
  f.code("8BIM").code("desc").u32v(static_cast<uint32_t>(descBody.size()));
  for (const uint8_t b : descBody) f.u8v(b);
  return f.bytes;
}

}  // namespace

// io/AbrBrushes' Dual Brush support and brush/Deposit.hpp §2d. Three
// sections: the coverage arithmetic in isolation (no `.abr` involved), the
// importer end to end (a synthetic `dualBrush` descriptor, extended a few
// ways), and the plumbing round trip a live second tip has to survive
// (`BrushPreset` -> `BrushState` -> `BrushTip`, and the two UI-layer leaks
// `app/selftest/AbrSampledTips.cpp` already found and fixed for a bitmap
// tip's own pointer).
bool runAbrDualBrushTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // ==========================================================================
  std::printf("  -- A. dabCoverage() composites the two tips (brush/Deposit.hpp §2d) --\n");
  // ==========================================================================
  {
    // Both tips ROUND and SOFT (hardness 0), radius 10, so at dx=5 (half the
    // radius) the smoothstep's own symmetry point lands exactly on 0.5f --
    // `u = d = 0.5` gives `1 - 0.25*(3-1) = 0.5`, every term a dyadic
    // fraction, so this is bit-exact in float32, not a tolerance.
    BrushTip soft10;
    soft10.radius = 10.0f;
    soft10.hardness = 0.0f;
    const float cov10 = dabCoverage(soft10, 5.0f, 0.0f);
    check(cov10 == 0.5f,
          "abr-dual/coverage: a soft round tip at half its radius reads exactly 0.5 -- the "
          "smoothstep symmetry point, bit-exact, and the base value the identities below build on");

    // A hard disc, radius 8, so dx=5 is INSIDE its flat core (`d <= h` with
    // `h = clamp(1,0,1) = 1`) and its own coverage is exactly 1.0f.
    BrushTip hard8;
    hard8.radius = 8.0f;
    hard8.hardness = 1.0f;
    const float cov8 = dabCoverage(hard8, 5.0f, 0.0f);
    check(cov8 == 1.0f, "abr-dual/coverage: a hard disc's own coverage well inside its radius is "
                        "exactly 1.0");

    // Multiply(0.5, 1.0) == 0.5 -- exact float arithmetic, no rounding since
    // both operands are dyadic fractions.
    BrushTip mulTip = soft10;
    mulTip.dualTip = std::make_shared<BrushTip>(hard8);
    mulTip.dualBlend = DualBrushBlend::Multiply;
    check(dabCoverage(mulTip, 5.0f, 0.0f) == cov10 * cov8,
          "abr-dual/coverage: Multiply combines the two tips' coverage by straight "
          "multiplication -- 0.5 * 1.0 == 0.5, exact");

    // Overlay(0.5, 1.0): `base == 0.5` takes the `base>=0.5` branch,
    // `1 - 2*(1-0.5)*(1-1.0) = 1 - 2*0.5*0 = 1.0` -- the `(1-second)` factor
    // is EXACTLY zero, so this is exact float arithmetic too, not a tolerance.
    BrushTip ovlTip = soft10;
    ovlTip.dualTip = std::make_shared<BrushTip>(hard8);
    ovlTip.dualBlend = DualBrushBlend::Overlay;
    check(dabCoverage(ovlTip, 5.0f, 0.0f) == 1.0f,
          "abr-dual/coverage: Overlay at base 1.0 from the second tip's own full coverage folds "
          "to exactly 1.0, by the same algebra Photoshop's Overlay/Hard-Light family always has "
          "at an extreme value");

    // Overlay(0.5, 0.5): both branches of the two-piece formula agree exactly
    // at base==0.5 and reduce to `second` -- the classic "50% gray is a
    // no-op" identity of Overlay/Soft-Light-family blend modes. Exact for the
    // same dyadic-fraction reason as the 0.5 above.
    BrushTip ovlSelf = soft10;
    ovlSelf.dualTip = std::make_shared<BrushTip>(soft10);
    ovlSelf.dualBlend = DualBrushBlend::Overlay;
    check(dabCoverage(ovlSelf, 5.0f, 0.0f) == 0.5f,
          "abr-dual/coverage: Overlay(0.5, 0.5) == 0.5 exactly -- base 0.5 is a no-op on the "
          "second tip's own value, the identity that pins the branch boundary down");
    BrushTip mulSelf = ovlSelf;
    mulSelf.dualBlend = DualBrushBlend::Multiply;
    check(dabCoverage(mulSelf, 5.0f, 0.0f) == 0.25f,
          "abr-dual/coverage: Multiply(0.5, 0.5) == 0.25 exactly");
  }

  // ==========================================================================
  std::printf("  -- A2. Color Burn and Hard Mix, on combineDualCoverage() directly --\n");
  // ==========================================================================
  {
    // `combineDualCoverage()` is exercised DIRECTLY here rather than through
    // `dabCoverage()`, for one specific reason: `dabCoverage()`'s own
    // `base == 0` short-circuit (brush/Deposit.cpp, right before its call
    // into this function) means production code NEVER calls
    // `combineDualCoverage()` with `base == 0` at all -- so a test that can
    // only observe `dabCoverage()`'s output cannot tell "the mode's own
    // formula preserves the identity" from "the wrapper masked it," and for
    // Hard Mix those are genuinely different things (see below). That is
    // also why this function is declared in brush/Deposit.hpp and defined
    // outside the anonymous namespace in brush/Deposit.cpp, instead of
    // staying a `namespace {}` local the way `singleTipCoverage()` does.
    //
    // 1e-6f tolerance throughout this block: every value below reaches its
    // result through at most four float32 operations (a subtract, a
    // divide-or-multiply, a min, a subtract) starting from operands with one
    // or two decimal digits, none of which are exact binary fractions except
    // 0.5. float32 epsilon is ~1.19e-7; four compounded roundings on
    // order-1 values bound the error at a handful of epsilons, so 1e-6 is
    // roughly 8x that worst case -- tight enough to catch a wrong formula,
    // loose enough not to flake on legitimate rounding.
    constexpr float kArithTol = 1e-6f;

    // --- base == 0 for all four members, asserted EXACTLY (no tolerance) ---
    //
    // Multiply and Overlay: `0 * second` and the `base < 0.5` branch's
    // `2 * 0 * second` are both exactly 0.0f for any finite `second` --
    // IEEE754 float multiplication by an exact 0 is exact.
    check(combineDualCoverage(DualBrushBlend::Multiply, 0.0f, 0.7f) == 0.0f,
          "abr-dual/coverage2: combineDualCoverage(Multiply, base=0, .) == 0 exactly, direct");
    check(combineDualCoverage(DualBrushBlend::Overlay, 0.0f, 0.7f) == 0.0f,
          "abr-dual/coverage2: combineDualCoverage(Overlay, base=0, .) == 0 exactly, direct");
    // Color Burn at base==0: `1 - min(1, (1-0)/second) == 1 - min(1, 1/second)`.
    // For every `second` in (0,1], `1/second >= 1`, so `min` clamps to `1`
    // and `1 - 1 == 0` -- EXACT for second==1.0 (1/1==1.0 exactly) and for
    // second==0.5 (1/0.5==2.0 exactly, a power-of-two divide). The STANDARD
    // formula already has this identity; nothing was added to get it.
    check(combineDualCoverage(DualBrushBlend::ColorBurn, 0.0f, 1.0f) == 0.0f,
          "abr-dual/coverage2: combineDualCoverage(ColorBurn, base=0, second=1) == 0 exactly -- "
          "the unmodified standard formula already has this identity");
    check(combineDualCoverage(DualBrushBlend::ColorBurn, 0.0f, 0.5f) == 0.0f,
          "abr-dual/coverage2: combineDualCoverage(ColorBurn, base=0, second=0.5) == 0 exactly, "
          "same reason");
    // Hard Mix at base==0, second==1: THIS is the case the bare Photoshop
    // formula gets wrong -- `base + second == 0 + 1 == 1 >= 1`, so an
    // unguarded `(base+second>=1)?1:0` returns 1.0f here, painting where the
    // primary tip has no coverage at all. The `base > 0.0f` guard added in
    // this function's own HardMix case is what makes this 0.0f instead.
    check(combineDualCoverage(DualBrushBlend::HardMix, 0.0f, 1.0f) == 0.0f,
          "abr-dual/coverage2: combineDualCoverage(HardMix, base=0, second=1) == 0 exactly -- the "
          "ONE member whose bare per-channel formula would answer 1 here (0+1>=1), caught by this "
          "function's own base>0 guard rather than left to dabCoverage()'s short-circuit alone");

    // --- Color Burn, second == 0 edge, both branches of the guard ---
    //
    // base < 1, second <= 0: the formula's own pole, defined as 0 (header
    // comment's "darkening by NOTHING darkens NOTHING" and its analytic
    // limit as second -> 0+).
    check(combineDualCoverage(DualBrushBlend::ColorBurn, 0.5f, 0.0f) == 0.0f,
          "abr-dual/coverage2: ColorBurn(base=0.5, second=0) == 0 exactly -- the second==0 pole, "
          "defined rather than left undefined");
    // base >= 1: saturates to white regardless of second, INCLUDING
    // second==0 -- a full-coverage primary tip stays fully covered even
    // where the second tip contributes nothing, which is the OTHER branch of
    // the same guard and must not be confused with the pole above.
    check(combineDualCoverage(DualBrushBlend::ColorBurn, 1.0f, 0.0f) == 1.0f,
          "abr-dual/coverage2: ColorBurn(base=1, second=0) == 1 exactly -- base saturation is "
          "checked BEFORE the second==0 pole, so these two zero-ish inputs give different answers "
          "for different reasons");

    // --- Two hand-computed interior points per new mode ---
    //
    // Color Burn point 1: base=0.6, second=0.5.
    //   (1 - 0.6) / 0.5 = 0.4 / 0.5 = 0.8
    //   1 - min(1, 0.8) = 1 - 0.8 = 0.2
    check(nearf(combineDualCoverage(DualBrushBlend::ColorBurn, 0.6f, 0.5f), 0.2f, kArithTol),
          "abr-dual/coverage2: ColorBurn(0.6, 0.5) == 0.2, by hand: (1-0.6)/0.5=0.8, 1-0.8=0.2");
    // Color Burn point 2: base=0.5, second=0.8.
    //   (1 - 0.5) / 0.8 = 0.5 / 0.8 = 0.625
    //   1 - min(1, 0.625) = 0.375
    check(nearf(combineDualCoverage(DualBrushBlend::ColorBurn, 0.5f, 0.8f), 0.375f, kArithTol),
          "abr-dual/coverage2: ColorBurn(0.5, 0.8) == 0.375, by hand: (1-0.5)/0.8=0.625, "
          "1-0.625=0.375");
    // Hard Mix point 1: base=0.6, second=0.5. Sum 1.1 >= 1 -> 1.0.
    check(combineDualCoverage(DualBrushBlend::HardMix, 0.6f, 0.5f) == 1.0f,
          "abr-dual/coverage2: HardMix(0.6, 0.5) == 1.0, by hand: 0.6+0.5=1.1 >= 1");
    // Hard Mix point 2: base=0.4, second=0.5. Sum 0.9 < 1 -> 0.0.
    check(combineDualCoverage(DualBrushBlend::HardMix, 0.4f, 0.5f) == 0.0f,
          "abr-dual/coverage2: HardMix(0.4, 0.5) == 0.0, by hand: 0.4+0.5=0.9 < 1");
    // Hard Mix at the exact boundary, base=0.5, second=0.5: sum is EXACTLY
    // 1.0f (both dyadic, no rounding), and the formula's own comparison is
    // `>=`, so this lands on the `1.0f` side, not the `0.0f` side -- pins
    // down which side of the threshold the boundary itself falls on.
    check(combineDualCoverage(DualBrushBlend::HardMix, 0.5f, 0.5f) == 1.0f,
          "abr-dual/coverage2: HardMix(0.5, 0.5) == 1.0 exactly -- the sum lands EXACTLY on the "
          "threshold, and Photoshop's `>=` puts it on the covered side");

    // --- Distinctness: all four modes, same inputs, four different answers ---
    //
    // base=0.6, second=0.5 (the Color Burn point 1 and Hard Mix point 1
    // above, reused): Multiply 0.6*0.5=0.30; Overlay (base>=0.5 branch)
    // 1-2*(1-0.6)*(1-0.5)=1-2*0.4*0.5=1-0.4=0.60; ColorBurn 0.20 (above);
    // HardMix 1.00 (above). Four genuinely different numbers -- this is the
    // check that a fixture collapsing to 0 or 1 everywhere cannot fake: if
    // any two of these formulas were accidentally swapped or aliased, at
    // least one of the six pairwise comparisons below would fail.
    const float mul = combineDualCoverage(DualBrushBlend::Multiply, 0.6f, 0.5f);
    const float ovl = combineDualCoverage(DualBrushBlend::Overlay, 0.6f, 0.5f);
    const float cbn = combineDualCoverage(DualBrushBlend::ColorBurn, 0.6f, 0.5f);
    const float hmx = combineDualCoverage(DualBrushBlend::HardMix, 0.6f, 0.5f);
    check(nearf(mul, 0.30f, kArithTol) && nearf(ovl, 0.60f, kArithTol) &&
              nearf(cbn, 0.20f, kArithTol) && hmx == 1.0f,
          "abr-dual/coverage2: sanity -- all four hand-computed values at base=0.6, second=0.5 "
          "match their derivations above");
    check(mul != ovl && mul != cbn && mul != hmx && ovl != cbn && ovl != hmx && cbn != hmx,
          "abr-dual/coverage2: ...and all four are PAIRWISE DISTINCT -- the same two input "
          "coverages produce four different combined values, one per mode, not four names for one "
          "number");
  }

  {
    // The footprint invariant brush/Deposit.hpp §2d argues: a SECOND tip
    // strictly LARGER than the primary can never make a dab paint outside
    // the primary's own disc, because both blend formulas are exactly 0 when
    // the primary's own coverage is 0.
    BrushTip smallHard;
    smallHard.radius = 10.0f;
    smallHard.hardness = 1.0f;
    auto bigDual = std::make_shared<BrushTip>();
    bigDual->radius = 50.0f;
    bigDual->hardness = 1.0f;  // covers dx=15 easily on its own
    smallHard.dualTip = bigDual;

    smallHard.dualBlend = DualBrushBlend::Multiply;
    check(dabCoverage(smallHard, 15.0f, 0.0f) == 0.0f,
          "abr-dual/coverage: Multiply -- a texel outside the PRIMARY tip's disc (radius 10) but "
          "inside the second tip's own (radius 50) is still exactly 0");
    smallHard.dualBlend = DualBrushBlend::Overlay;
    check(dabCoverage(smallHard, 15.0f, 0.0f) == 0.0f,
          "abr-dual/coverage: ...and Overlay gives the identical exact 0, by the same base==0 "
          "short-circuit");

    // `dabPixelBounds()` takes no dual-brush case at all (brush/Deposit.hpp
    // §2d) -- proven directly: the SAME primary tip, with and without a
    // (much larger) second tip attached, must report identical bounds.
    BrushTip primaryAlone = smallHard;
    primaryAlone.dualTip.reset();
    const Vec2 centre{100.0f, 100.0f};
    const PixelBounds boundsAlone = dabPixelBounds(primaryAlone, centre, 300, 300);
    const PixelBounds boundsWithDual = dabPixelBounds(smallHard, centre, 300, 300);
    check(boundsAlone.x0 == boundsWithDual.x0 && boundsAlone.x1 == boundsWithDual.x1 &&
              boundsAlone.y0 == boundsWithDual.y0 && boundsAlone.y1 == boundsWithDual.y1,
          "abr-dual/bounds: dabPixelBounds() is bit-identical with and without a (larger) second "
          "tip attached -- it never reads BrushTip::dualTip at all");
  }

  {
    // The no-recursion guarantee: a `BrushTip` used AS a second tip can
    // itself carry a `dualTip` (nothing in the type stops it), and
    // `dabCoverage()` must ignore that inner level entirely. Chosen so a
    // wrongly-recursive implementation WOULD change the answer: `bTip` and
    // `cTip` are both soft (fractional coverage, not 0 or 1) and genuinely
    // different shapes, so multiplying `cTip`'s coverage in as well (the bug
    // this proves absent) would move the result away from `covB`.
    BrushTip primaryHard;
    primaryHard.radius = 20.0f;
    primaryHard.hardness = 1.0f;  // base == 1.0 exactly at dx=5, so Multiply(1, second) == second

    BrushTip bTip;
    bTip.radius = 8.0f;
    bTip.hardness = 0.0f;
    const float covB = dabCoverage(bTip, 5.0f, 0.0f);
    check(covB > 0.0f && covB < 1.0f,
          "abr-dual/recursion: sanity -- B's own coverage at this offset is a genuine fraction, "
          "not 0 or 1, so this fixture can actually distinguish 'B alone' from 'B combined with "
          "something else'");

    BrushTip cTip;
    cTip.radius = 6.0f;
    cTip.hardness = 0.0f;
    const float covC = dabCoverage(cTip, 5.0f, 0.0f);
    check(covC != covB,
          "abr-dual/recursion: sanity -- C's own coverage genuinely differs from B's, so a "
          "wrongly-honoured recursion would move the combined result away from covB");

    BrushTip bTipWithNestedC = bTip;
    bTipWithNestedC.dualTip = std::make_shared<BrushTip>(cTip);
    bTipWithNestedC.dualBlend = DualBrushBlend::Multiply;

    BrushTip primaryNoNest = primaryHard;
    primaryNoNest.dualTip = std::make_shared<BrushTip>(bTip);
    primaryNoNest.dualBlend = DualBrushBlend::Multiply;
    BrushTip primaryNested = primaryHard;
    primaryNested.dualTip = std::make_shared<BrushTip>(bTipWithNestedC);
    primaryNested.dualBlend = DualBrushBlend::Multiply;

    const float outNoNest = dabCoverage(primaryNoNest, 5.0f, 0.0f);
    const float outNested = dabCoverage(primaryNested, 5.0f, 0.0f);
    check(outNoNest == covB,
          "abr-dual/recursion: primary(base==1) combined with B alone, Multiply, equals B's own "
          "coverage exactly");
    check(outNested == covB,
          "abr-dual/recursion: ...and is UNCHANGED when B itself carries a nested dual tip C -- "
          "the second level is never read, even though C's own coverage at this offset genuinely "
          "differs from B's (brush/Deposit.hpp §2d's no-recursion guarantee)");
    check(outNoNest == outNested,
          "abr-dual/recursion: restated directly -- nesting C inside B does not move "
          "dabCoverage() at all");
  }

  // ==========================================================================
  std::printf("  -- B. io/AbrBrushes.cpp: the dualBrush descriptor, end to end --\n");
  // ==========================================================================
  {
    DualBrushSpec spec;
    spec.present = true;
    spec.on = true;
    spec.haveBlend = true;
    spec.blendValueId = "Mltp";
    spec.tip.present = true;
    spec.tip.diameterPx = 30.0;         // radius 15
    spec.tip.angleDeg = 45.0;
    spec.tip.roundnessPercent = 60.0;   // roundness 0.6
    spec.tip.spacingPercent = 40.0;     // abrSpacingToRadii(40) == 0.8
    spec.haveScatter = true;
    spec.useScatter = false;
    spec.haveCount = true;
    spec.count = 1;

    const AbrImportResult r = importAbrBrushes(wrapAbrDesc(oneDualBrushDesc("Dual Multiply", spec)));
    check(r.ok && r.presets.size() == 1, "abr-dual: a one-brush library with a full Dual Brush "
                                         "still imports");
    if (r.ok && r.presets.size() == 1) {
      const BrushPreset& p = r.presets[0];
      check(p.dualTip != nullptr, "abr-dual: `useDualBrush` ON with a readable `BlnM` and `Brsh` "
                                  "builds a second tip");
      if (p.dualTip != nullptr) {
        check(nearf(p.dualTip->radius, 15.0f, 1e-4f),
              "abr-dual: the second tip's `Dmtr` is a DIAMETER -- radius is half of it, the same "
              "rule as the primary tip's");
        check(nearf(p.dualTip->angle, 45.0f, 1e-4f), "abr-dual: `Angl` arrives");
        check(nearf(p.dualTip->roundness, 0.6f, 1e-4f), "abr-dual: `Rndn` arrives, scaled to a "
                                                        "0..1 ratio");
        check(nearf(p.dualTip->spacing, 0.8f, 1e-4f),
              "abr-dual: `Spcn` converts percent-of-diameter to radii through the SAME "
              "abrSpacingToRadii() the primary tip uses");
        check(p.dualTip->hardness == 1.0f,
              "abr-dual: hardness defaults to a hard edge -- Photoshop's Dual Brush panel has no "
              "Hardness slider, so there is no `Hrdn` key on this shape to read");
        check(p.dualTip->dualTip == nullptr,
              "abr-dual: the second tip's OWN dualTip is null -- readAbrTipShape() never looks "
              "for a nested `dualBrush` key");
      }
      check(p.dualBlend == DualBrushBlend::Multiply, "abr-dual: `BlnM` 'Mltp' reads as Multiply");
      check(r.dualBrushes == 0 && r.dualBrushUnsupportedBlend == 0,
            "abr-dual: a second tip that DID arrive costs neither failure counter");
      check(r.dualBrushCadenceNotHonoured == 0,
            "abr-dual: Count 1 with scatter off is not an approximation -- stamped once, centred, "
            "IS Photoshop's own answer for this configuration, so no cadence note fires");
      check(r.notes.empty(),
            "abr-dual: a Dual Brush that loses nothing at all -- shape, blend AND cadence -- "
            "produces NO notes");
    }

    // The identical descriptor, `BlnM` 'Ovrl' instead.
    spec.blendValueId = "Ovrl";
    const AbrImportResult ro = importAbrBrushes(wrapAbrDesc(oneDualBrushDesc("Dual Overlay", spec)));
    check(ro.ok && ro.presets.size() == 1 && ro.presets[0].dualTip != nullptr &&
              ro.presets[0].dualBlend == DualBrushBlend::Overlay,
          "abr-dual: `BlnM` 'Ovrl' reads as Overlay, and a second tip still builds");

    // `BlnM` 'CBrn' -- Color Burn, sourced from `psd_tools.terminology`'s
    // `Enum` class (the SAME class `Mltp`/`Ovrl` came from) and independently
    // from `ag-psd`'s `BlnM` enum (io/AbrBrushes.cpp's own comment on the
    // mapping has both citations). A second tip must arrive, not just a
    // recognised enumerator, exactly as Mltp/Ovrl do above.
    spec.blendValueId = "CBrn";
    const AbrImportResult rcb = importAbrBrushes(wrapAbrDesc(oneDualBrushDesc("Dual ColorBurn", spec)));
    check(rcb.ok && rcb.presets.size() == 1 && rcb.presets[0].dualTip != nullptr &&
              rcb.presets[0].dualBlend == DualBrushBlend::ColorBurn,
          "abr-dual: `BlnM` 'CBrn' reads as Color Burn, and a second tip still builds");
    check(rcb.dualBrushUnsupportedBlend == 0,
          "abr-dual: ...and does NOT increment dualBrushUnsupportedBlend -- this preset now "
          "arrives with a working dual tip instead of falling back to the primary tip alone");

    // `BlnM` 'hMix' -- Hard Mix, at the spelling this build's own task brief
    // reports as OBSERVED on the wire in a real sample pack. io/AbrBrushes.cpp's
    // comment on the mapping records the caveat: this exact spelling was not
    // independently reproduced by this search (it lives in a DIFFERENT psd_tools
    // table, for a different wire format, not in the `BlnM`-specific one), so
    // this accepts it on the brief's own reported observation while ALSO
    // accepting the spelling every `BlnM`-specific source gives instead.
    spec.blendValueId = "hMix";
    const AbrImportResult rhm = importAbrBrushes(wrapAbrDesc(oneDualBrushDesc("Dual HardMix", spec)));
    check(rhm.ok && rhm.presets.size() == 1 && rhm.presets[0].dualTip != nullptr &&
              rhm.presets[0].dualBlend == DualBrushBlend::HardMix,
          "abr-dual: `BlnM` 'hMix' reads as Hard Mix, and a second tip still builds");
    check(rhm.dualBrushUnsupportedBlend == 0,
          "abr-dual: ...and does NOT increment dualBrushUnsupportedBlend");

    // `BlnM` 'hardMix' -- the SAME concept, the long-form spelling `ag-psd`'s
    // `BlnM` enum actually gives (the table purpose-built for this exact
    // field). Both spellings must land on the identical enumerator: a real
    // file is expected to use exactly one of the two, never both, but this
    // build should not care which.
    spec.blendValueId = "hardMix";
    const AbrImportResult rhm2 =
        importAbrBrushes(wrapAbrDesc(oneDualBrushDesc("Dual HardMix2", spec)));
    check(rhm2.ok && rhm2.presets.size() == 1 && rhm2.presets[0].dualTip != nullptr &&
              rhm2.presets[0].dualBlend == DualBrushBlend::HardMix,
          "abr-dual: `BlnM` 'hardMix' (the long-form spelling) ALSO reads as Hard Mix");
  }

  {
    // A `BlnM` this build does not composite: read as a real enumerated
    // value, not absence, and refused BY NAME rather than silently dropped
    // to the primary tip alone with no note -- item 3's whole requirement.
    DualBrushSpec spec;
    spec.present = true;
    spec.on = true;
    spec.haveBlend = true;
    spec.blendValueId = "Scrn";  // Screen: read, not implemented
    spec.tip.present = true;

    const AbrImportResult r =
        importAbrBrushes(wrapAbrDesc(oneDualBrushDesc("Dual Screen", spec)));
    check(r.ok && r.presets.size() == 1 && r.presets[0].dualTip == nullptr,
          "abr-dual: an unsupported BlnM builds NO second tip -- falls back to the primary tip "
          "alone");
    check(r.dualBrushUnsupportedBlend == 1 && r.dualBrushes == 0,
          "abr-dual: ...counted as UNSUPPORTED BLEND, not as 'nothing arrived at all' -- the two "
          "are told apart");
    bool sawScreenNote = false;
    for (const AbrImportNote& n : r.notes)
      if (n.what.find("Scrn") != std::string::npos &&
          n.what.find("not implemented") != std::string::npos)
        sawScreenNote = true;
    check(sawScreenNote, "abr-dual: ...and the note NAMES the raw blend mode, so a reader can "
                         "tell which one without re-deriving it");
  }

  {
    // `BlnM` 'linearHeight' -- the third id this step set out to resolve,
    // and the one left UNSUPPORTED on purpose. `ag-psd`'s `BlnM` enum (the
    // table built for this exact field) confirms the id is real and even
    // carries a "used in ABR" comment on it, so this is not a garbled or
    // misread wire value -- but what it names is Photoshop's Texture panel's
    // "Linear Height" compositing (a grayscale height-map blend, per the
    // Krita `abr_brush_importer` plugin's own texture-mode table), not a
    // colour/coverage blend mode, and no source gives it a per-pixel
    // formula. Exercised the identical way `Scrn` is above: read as a real
    // enumerated value, refused BY NAME, counted as understood-but-unsupported
    // rather than silently dropped.
    DualBrushSpec spec;
    spec.present = true;
    spec.on = true;
    spec.haveBlend = true;
    spec.blendValueId = "linearHeight";
    spec.tip.present = true;

    const AbrImportResult r =
        importAbrBrushes(wrapAbrDesc(oneDualBrushDesc("Dual LinearHeight", spec)));
    check(r.ok && r.presets.size() == 1 && r.presets[0].dualTip == nullptr,
          "abr-dual: `BlnM` 'linearHeight' builds NO second tip -- not sourced to a coverage "
          "formula, so it is not guessed at");
    check(r.dualBrushUnsupportedBlend == 1 && r.dualBrushes == 0,
          "abr-dual: ...counted as UNSUPPORTED BLEND, the same honest bucket 'Scrn' lands in "
          "above, not silently folded into Multiply, ColorBurn or any other implemented mode");
    bool sawHeightNote = false;
    for (const AbrImportNote& n : r.notes)
      if (n.what.find("linearHeight") != std::string::npos &&
          n.what.find("not implemented") != std::string::npos)
        sawHeightNote = true;
    check(sawHeightNote, "abr-dual: ...and the note names 'linearHeight' by its own wire id, not "
                         "as a guessed English name for a formula this build does not have");
  }

  {
    // The old shape this build already handled before this step: `dualBrush`
    // ON but with no `Brsh` and no `BlnM` at all -- app/selftest/AbrBrushes.cpp
    // §7's own fixture, reproduced here to prove the refactor of
    // `presetFromDescriptor()` preserved that exact outcome.
    DualBrushSpec spec;
    spec.present = true;
    spec.on = true;  // haveBlend/tip.present/haveScatter/haveCount all default false

    const AbrImportResult r =
        importAbrBrushes(wrapAbrDesc(oneDualBrushDesc("Legacy Dual", spec)));
    check(r.ok && r.presets.size() == 1 && r.presets[0].dualTip == nullptr,
          "abr-dual: `useDualBrush` ON with no `Brsh` and no `BlnM` at all still builds no second "
          "tip");
    check(r.dualBrushes == 1 && r.dualBrushUnsupportedBlend == 0,
          "abr-dual: ...and lands in the ORIGINAL `dualBrushes` bucket, unchanged");
    bool sawOldNote = false;
    for (const AbrImportNote& n : r.notes)
      if (n.what.find("Dual Brush is ON and not imported") != std::string::npos) sawOldNote = true;
    check(sawOldNote, "abr-dual: ...with the identical note text this build always gave for that "
                      "case");

    // And `useDualBrush` OFF, same as before this step: no counters, no notes.
    DualBrushSpec off = spec;
    off.on = false;
    const AbrImportResult roff = importAbrBrushes(wrapAbrDesc(oneDualBrushDesc("Off", off)));
    check(roff.ok && roff.dualBrushes == 0 && roff.dualBrushUnsupportedBlend == 0 &&
              roff.dualBrushCadenceNotHonoured == 0 && roff.notes.empty() &&
              roff.presets[0].dualTip == nullptr,
          "abr-dual: `dualBrush` present but OFF loses nothing, exactly as every real preset's "
          "off brushes always have");
  }

  {
    // Cadence: a second tip that DID build, with a blend mode this build
    // composites, but whose own Count or Scatter say it should be stamped
    // more than once, centred, per dab of the first.
    DualBrushSpec base;
    base.present = true;
    base.on = true;
    base.haveBlend = true;
    base.blendValueId = "Mltp";
    base.tip.present = true;
    base.haveScatter = true;
    base.haveCount = true;

    DualBrushSpec countThree = base;
    countThree.useScatter = false;
    countThree.count = 3;
    const AbrImportResult rc =
        importAbrBrushes(wrapAbrDesc(oneDualBrushDesc("Dual Count3", countThree)));
    check(rc.ok && rc.presets.size() == 1 && rc.presets[0].dualTip != nullptr,
          "abr-dual: Count 3 still builds a second tip -- the SHAPE is not in question");
    check(rc.dualBrushCadenceNotHonoured == 1,
          "abr-dual: ...but Count != 1 means stamping it once, centred, is an approximation, so "
          "the cadence counter fires");
    bool sawCadenceNote = false;
    for (const AbrImportNote& n : rc.notes)
      if (n.what.find("spacing, scatter and count are not honoured") != std::string::npos)
        sawCadenceNote = true;
    check(sawCadenceNote, "abr-dual: ...and says so by name");

    DualBrushSpec scatterOn = base;
    scatterOn.useScatter = true;
    scatterOn.count = 1;
    const AbrImportResult rs =
        importAbrBrushes(wrapAbrDesc(oneDualBrushDesc("Dual ScatterOn", scatterOn)));
    check(rs.dualBrushCadenceNotHonoured == 1,
          "abr-dual: Count 1 with scatter ON also loses something real -- the cadence counter "
          "fires for scatter alone, not only for Count");
  }

  {
    // The second tip's own SAMPLED bitmap: the identical `samp`-block
    // correlation the primary tip already has, exercised here against the
    // dual tip specifically.
    const std::vector<uint8_t> raw3x2 = {5, 15, 25, 35, 45, 55};
    const auto rec = buildSampRecord("11112222-3333-4444-5555-666677778888", 2, 0, 0, 2, 3, 8, 0,
                                     raw3x2);

    DualBrushSpec spec;
    spec.present = true;
    spec.on = true;
    spec.haveBlend = true;
    spec.blendValueId = "Mltp";
    spec.tip.present = true;
    spec.tip.sampledDataId = "11112222-3333-4444-5555-666677778888";
    spec.haveScatter = true;
    spec.useScatter = false;
    spec.haveCount = true;
    spec.count = 1;

    const AbrImportResult r =
        importAbrBrushes(wrapAbrWithSampAndDesc(rec, oneDualBrushDesc("Dual Sampled", spec)));
    check(r.ok && r.presets.size() == 1, "abr-dual/samp: still imports with a samp section "
                                         "attached");
    if (r.ok && r.presets.size() == 1) {
      const BrushPreset& p = r.presets[0];
      check(p.dualTip != nullptr && p.dualTip->bitmap != nullptr,
            "abr-dual/samp: a `sampledData` id matching a real sample attaches a bitmap to the "
            "SECOND tip");
      if (p.dualTip != nullptr && p.dualTip->bitmap != nullptr) {
        check(p.dualTip->bitmap->width == 3 && p.dualTip->bitmap->height == 2 &&
                  p.dualTip->bitmap->alpha == raw3x2,
              "abr-dual/samp: ...the exact decoded bytes, not the primary tip's (which has none "
              "here)");
      }
      check(r.sampledTips == 0 && r.notes.empty(),
            "abr-dual/samp: a dual tip whose sample DID resolve, with Count 1 and no scatter, "
            "costs no count and no note at all");
    }

    // `#Prc` Dmtr on the SECOND tip resolves against the SECOND tip's OWN
    // sample dimensions (3x2), independently of the primary tip (which has
    // none in this fixture): 50% of max(3,2)=3 is diameter 1.5, radius 0.75.
    DualBrushSpec pctSpec = spec;
    pctSpec.tip.dmtrUnit = "#Prc";
    pctSpec.tip.diameterPx = 50.0;
    const AbrImportResult rp =
        importAbrBrushes(wrapAbrWithSampAndDesc(rec, oneDualBrushDesc("Dual PctSize", pctSpec)));
    check(rp.ok && rp.presets.size() == 1 && rp.presets[0].dualTip != nullptr &&
              nearf(rp.presets[0].dualTip->radius, 0.75f, 1e-4f),
          "abr-dual/samp: a `#Prc` Dmtr on the second tip resolves against ITS OWN sample's "
          "larger dimension (3px), so 50% is radius 0.75");

    // The orphan case: `sampledData` naming an id this file's `samp` block
    // does not contain -- the existing fallback, on the SECOND tip.
    DualBrushSpec orphan = spec;
    orphan.tip.sampledDataId = "not-a-real-id-in-this-file";
    const AbrImportResult ro =
        importAbrBrushes(wrapAbrWithSampAndDesc(rec, oneDualBrushDesc("Dual Orphan", orphan)));
    check(ro.ok && ro.presets.size() == 1 && ro.presets[0].dualTip != nullptr &&
              ro.presets[0].dualTip->bitmap == nullptr,
          "abr-dual/samp: an id naming no real sample still builds the second tip -- shape and "
          "blend mode are unaffected -- but with no bitmap, falling back to the round procedural "
          "profile for THAT tip");
    check(ro.sampledTips == 1, "abr-dual/samp: ...counted, exactly as an unmatched sampledData "
                               "always was");
    bool sawDualSampleNote = false;
    for (const AbrImportNote& n : ro.notes)
      if (n.what.find("Dual Brush's") != std::string::npos &&
          n.what.find("sampled bitmap") != std::string::npos)
        sawDualSampleNote = true;
    check(sawDualSampleNote,
          "abr-dual/samp: ...and the note is labelled \"Dual Brush's\", so a reader can tell it "
          "apart from the primary tip losing ITS sample");
  }

  // ==========================================================================
  std::printf("  -- C. the plumbing round trip a live second tip has to survive --\n");
  // ==========================================================================
  {
    auto dualTip = std::make_shared<BrushTip>();
    dualTip->radius = 5.0f;

    BrushPreset preset;
    preset.radius = 12.0f;
    preset.dualTip = dualTip;
    preset.dualBlend = DualBrushBlend::Overlay;

    BrushState brush;
    applyPresetToBrush(preset, brush);
    check(brush.dualTip == dualTip && brush.dualBlend == DualBrushBlend::Overlay,
          "abr-dual/roundtrip: applyPresetToBrush() carries the SAME dualTip pointer and the "
          "blend mode, not a copy");

    MixboxLut lut;
    const BrushTip tip = brushTipFor(brush, lut, 1.0f);
    check(tip.dualTip == dualTip && tip.dualBlend == DualBrushBlend::Overlay,
          "abr-dual/roundtrip: brushTipFor() carries both into the BrushTip");

    const BrushPreset dup = presetFromBrush("Duplicate", brush);
    check(dup.dualTip == dualTip && dup.dualBlend == DualBrushBlend::Overlay,
          "abr-dual/roundtrip: presetFromBrush() (Duplicate) carries both back into a preset");

    // The documented blind spot: presetMatches() does not compare dualTip or
    // dualBlend (brush/Library.hpp's own comment on why, right beside
    // tipBitmap's identical one).
    BrushPreset other = dup;
    other.dualTip = std::make_shared<BrushTip>();  // a DIFFERENT dual tip
    other.dualBlend = DualBrushBlend::Multiply;     // AND a different blend mode
    check(presetMatches(other, brush.radius, brush.hardness, brush.spacing, brush.roundness,
                        brush.angle, brush.load, brush.wetness, brush.links, brush.grain),
          "abr-dual/roundtrip: presetMatches() DELIBERATELY cannot tell `other`'s different dual "
          "tip and blend mode apart from `brush`'s -- documented on BrushPreset::dualTip and on "
          "presetMatches() itself, because nothing today can move a dual tip independently of "
          "picking a whole preset");
  }

  {
    // brushRowIconTips(): a BrushRow never carries a dual tip, mirroring the
    // bitmap-tip leak app/selftest/AbrSampledTips.cpp already found and fixed
    // in this same function.
    BrushState live;
    live.dualTip = std::make_shared<BrushTip>();
    BrushRow row;
    row.radius = 15.0f;
    row.hardness = 0.4f;
    MixboxLut lut;
    const auto tips = brushRowIconTips(row, live, lut);
    bool anyLeaked = false;
    for (const BrushTip& t : tips)
      if (t.dualTip != nullptr) anyLeaked = true;
    check(!anyLeaked,
          "abr-dual/row: an unloaded library row's icon does NOT inherit the LIVE brush's dual "
          "tip -- it previews with no second tip until the row is actually picked");
  }

  {
    // DabPreviewCache must not hit across two tips differing only in their
    // dual tip or its blend mode -- the identical failure mode
    // app/selftest/AbrSampledTips.cpp already proved for `bitmap`.
    BrushTip a;
    a.radius = 10.0f;
    BrushTip b = a;
    b.dualTip = std::make_shared<BrushTip>();
    b.dualBlend = DualBrushBlend::Multiply;
    check(!dabPreviewTipsEqual(a, b),
          "abr-dual/preview: a tip with a dual tip is not equal to an otherwise-identical one "
          "without");
    BrushTip c = b;
    c.dualBlend = DualBrushBlend::Overlay;  // same dualTip pointer, different blend
    check(!dabPreviewTipsEqual(b, c),
          "abr-dual/preview: the SAME dual tip pointer combined by a DIFFERENT blend mode is not "
          "equal -- dabCoverage() would draw them differently");

    DabPreviewCache cache;
    const std::array<BrushTip, kDabPreviewCells> tipsA{a, a, a};
    const std::array<BrushTip, kDabPreviewCells> tipsB{b, b, b};
    (void)cache.imageFor(tipsA);
    (void)cache.imageFor(tipsB);
    check(cache.rasterisations() == 2 && cache.hits() == 0,
          "abr-dual/preview: DabPreviewCache does NOT hit across two tips differing only in their "
          "dual tip");
  }

  std::printf("[selftest] abr dual brush %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
