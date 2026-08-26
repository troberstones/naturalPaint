#include "app/selftest/Support.hpp"

#include <cmath>

#include "app/StrokePreview.hpp"
#include "app/StrokeSession.hpp"
#include "brush/Grain.hpp"

namespace np {

// app/StrokePreview -- the BRUSH EDITOR's TEST STROKE strip.
//
// **The whole point of this section is that the strip is the engine's own
// stroke and not an impression of one.** `app/StrokePreview.hpp` §2 inherits
// that rule verbatim from `app/DabPreview` §1, and it is worth a great deal
// more here: a dab preview could only drift on the falloff, while a stroke
// preview could drift on spacing, on the dab cadence, on the stroke-local
// dynamics pass, on scatter, on the size floor, and on grain -- six separate
// ways to show a painter a mark their brush does not make.
//
// So the assertions below are mostly of one shape: **change one setting that
// a single dab cannot express, and require the strip to change.** A preview
// that ignored spacing would pass a "does it draw something" test forever.
bool runStrokePreviewTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  MixboxLut lut;
  if (!lut.load(NP_MIXBOX_LUT)) {
    std::printf("  %-58s %s\n", "stroke-preview: Mixbox LUT loads", "FAIL");
    std::printf("[selftest] stroke preview FAIL\n");
    return false;
  }

  // How many of the two images' bytes differ. Used everywhere below rather
  // than `a != b`, because "these differ" and "these differ in 40% of their
  // pixels" are different findings and only the second one rules out a
  // one-byte rounding wobble being mistaken for a feature working.
  auto differingBytes = [](const StrokePreviewImage& a, const StrokePreviewImage& b) {
    if (a.rgba.size() != b.rgba.size()) return a.rgba.size() + b.rgba.size();
    size_t n = 0;
    for (size_t i = 0; i < a.rgba.size(); ++i)
      if (a.rgba[i] != b.rgba[i]) ++n;
    return n;
  };

  // ======================================================================
  // 1. It paints, it is the stated size, and it is DETERMINISTIC.
  //    Determinism first, because every "X changes the mark" assertion
  //    below is worthless if two renders of the SAME brush already differ:
  //    the difference could be noise rather than the setting.
  // ======================================================================
  BrushState base;
  {
    const StrokePreviewImage a = rasteriseStrokePreview(base, lut);
    check(a.width == kStrokePreviewWidth && a.height == kStrokePreviewHeight,
          "strip is exactly kStrokePreviewWidth x kStrokePreviewHeight");
    check(a.rgba.size() == static_cast<size_t>(a.width) * static_cast<size_t>(a.height) * 4u,
          "the byte count matches the stated size -- RGBA, row-major, no padding");
    check(!a.refused, "the default brush's stroke is NOT refused");
    check(a.dabs > 1, "the default brush emits MORE THAN ONE dab -- this is a stroke, not a dab");
    check(a.texels > 0, "and those dabs actually covered texels");

    const StrokePreviewImage b = rasteriseStrokePreview(base, lut);
    check(differingBytes(a, b) == 0,
          "two renders of the SAME brush are BYTE-IDENTICAL -- the per-dab draws are seeded "
          "from the stroke's start (strokeSeedFromStart()), which is fixed here, so every "
          "'this setting changes the mark' check below is signal and not noise");

    // Opaque everywhere: the ground is paper, not the panel behind it.
    bool allOpaque = true;
    for (size_t i = 3; i < a.rgba.size(); i += 4)
      if (a.rgba[i] != 255u) allOpaque = false;
    check(allOpaque, "every texel is opaque -- the strip is a scrap of paper, not a cutout");
  }

  // ======================================================================
  // 2. SPACING. The single most consequential number in the panel and the
  //    one a three-dab preview cannot express at all -- 0.05 and 0.9 draw
  //    the identical dab. If this section passes and nothing else in the
  //    file does, the preview is already worth more than what it replaced.
  // ======================================================================
  {
    BrushState tight = base;
    tight.spacing = 0.05f;
    BrushState loose = base;
    loose.spacing = 0.90f;

    const StrokePreviewImage t = rasteriseStrokePreview(tight, lut);
    const StrokePreviewImage l = rasteriseStrokePreview(loose, lut);

    // The dab COUNT is what spacing changes, and it is reported rather than
    // inferred from pixels -- `brush/StrokePath` decides it from
    // `tip.spacingPx()`, and this is that decision surfacing.
    check(t.dabs > l.dabs * 4,
          "spacing 0.05 emits at least 4x the dabs of spacing 0.90 -- the ratio of the two "
          "spacings is 18x, so a factor of 4 is a wide margin and not a coin flip");
    std::printf("    [measured] spacing 0.05 -> %zu dabs, 0.90 -> %zu dabs\n", t.dabs, l.dabs);

    // And the PICTURE differs, not merely the counter: a preview that
    // reported a dab count it did not draw with would satisfy the check
    // above and still be a lie.
    const size_t diff = differingBytes(t, l);
    check(diff > t.rgba.size() / 20,
          "and the rasterised strips differ in over 5% of their bytes -- the dab COUNT reaching "
          "the picture, not just the caption");
    std::printf("    [measured] tight vs loose: %zu of %zu bytes differ\n", diff, t.rgba.size());
  }

  // ======================================================================
  // 3. The other things a stationary dab cannot show (StrokePreview.hpp §1).
  //    Each is one edit against `base`, and each must move the picture.
  // ======================================================================
  {
    const StrokePreviewImage ref = rasteriseStrokePreview(base, lut);

    // SCATTER: a per-dab positional jitter that needs more than one dab to
    // exist at all.
    BrushState scattered = base;
    addLink(scattered.links, BrushLink{DynamicSource::Random, DynamicTarget::Scatter, Curve{}, 0.0f, 1.5f});
    check(differingBytes(rasteriseStrokePreview(scattered, lut), ref) > 0,
          "a RANDOM -> Scatter link changes the strip -- scatter displaces dab CENTRES, so it "
          "is invisible in any preview that draws one dab at one place");

    // A stroke-local source: VELOCITY has no value at all without a stroke.
    BrushState velocity = base;
    addLink(velocity.links, BrushLink{DynamicSource::Velocity, DynamicTarget::Size, Curve{}, 0.2f, 1.0f});
    check(differingBytes(rasteriseStrokePreview(velocity, lut), ref) > 0,
          "a VELOCITY -> Size link changes the strip -- sourceIsStrokeLocal(), so a stationary "
          "dab resolves it to the target's identity and shows nothing");

    // DIRECTION: there is no direction of travel in a dab.
    BrushState direction = base;
    direction.roundness = 0.4f;  // an ellipse, so an angle is visible at all
    BrushState directionRef = direction;
    addLink(direction.links, BrushLink{DynamicSource::Direction, DynamicTarget::Angle, Curve{}, 0.0f, 360.0f});
    check(differingBytes(rasteriseStrokePreview(direction, lut),
                         rasteriseStrokePreview(directionRef, lut)) > 0,
          "a DIRECTION -> Angle link changes an ELLIPTICAL tip's strip -- the S-curve turns "
          "through every heading, which is the whole reason the path is a full sine period");

    // GRAIN is keyed to ABSOLUTE document position, so it needs a mark that
    // travels before it reads as paper rather than as one sampled patch.
    BrushState grainy = base;
    grainy.grain.enabled = true;
    grainy.grain.depth = 0.5f;
    check(differingBytes(rasteriseStrokePreview(grainy, lut), ref) > 0,
          "turning PAPER GRAIN on changes the strip");
  }

  // ======================================================================
  // 4. The integer scale (§4), and that it is derived from REACH rather
  //    than from the nominal radius.
  // ======================================================================
  {
    BrushState small = base;
    small.radius = 8.0f;
    check(strokePreviewScale(strokePreviewReach(small, lut)) == 1,
          "a small brush previews 1:1 -- life-size, never magnified");

    BrushState big = base;
    big.radius = kBrushRadiusMax;
    const int bigScale = strokePreviewScale(strokePreviewReach(big, lut));
    check(bigScale > 1, "a 200 px brush is minified rather than clipped");
    std::printf("    [measured] radius %.0f -> reach %.1f -> 1:%d\n",
                static_cast<double>(big.radius),
                static_cast<double>(strokePreviewReach(big, lut)), bigScale);

    // Derived rather than measured, and derived in STRIP texels -- which is
    // the distinction that matters, because the amplitude scales with the
    // document while the reach does not (strokePreviewScale()'s own comment).
    // Working it in document pixels instead gives 1:3 for a 120 px brush and
    // clips it by 30 texels; the "top and bottom rows are uniform paper"
    // assertion below is what caught that, and this is the corrected form it
    // is checked against.
    const float reach = strokePreviewReach(big, lut);
    const float budget =
        (static_cast<float>(kStrokePreviewHeight) - 8.0f) * 0.5f - kStrokePreviewAmplitude;
    const int wanted = static_cast<int>(std::ceil(reach / budget));
    check(bigScale == wanted,
          "the scale is exactly ceil(reach / ((height - air)/2 - amplitude)) -- the closed form "
          "in strip texels, not a number that happens to look big enough");

    // The mark must actually FIT: a scale that rounded down would clip, and
    // a clipped mark reads as a brush that stops mid-stroke.
    const StrokePreviewImage img = rasteriseStrokePreview(big, lut);
    bool topRowBare = true, bottomRowBare = true;
    const size_t rowBytes = static_cast<size_t>(img.width) * 4u;
    const size_t lastRow = static_cast<size_t>(img.height - 1) * rowBytes;
    // Compared CHANNEL BY CHANNEL against the row's own first pixel, not
    // against a single byte: paper is (0xf8, 0xf4, 0xf4, 0xff), so its four
    // channels differ from each other and a byte-vs-byte[0] comparison would
    // report every uniform paper row as non-uniform. (Written the wrong way
    // first, and it failed on a correctly-fitting brush -- which is a useful
    // reminder that an assertion failing is not by itself evidence the code
    // is wrong.)
    for (size_t i = 0; i < rowBytes; ++i) {
      if (img.rgba[i] != img.rgba[i % 4u]) topRowBare = false;
      if (img.rgba[lastRow + i] != img.rgba[lastRow + (i % 4u)]) bottomRowBare = false;
    }
    check(topRowBare && bottomRowBare,
          "the widest brush's top and bottom rows are still uniform paper -- the mark FITS, "
          "which is what the ceiling in strokePreviewScale() is for");

    // **Scatter widens the mark without touching the radius**, which is why
    // §4 says the scale comes from reach and not from `brush.radius`.
    BrushState wide = base;
    wide.radius = 30.0f;
    BrushState wideScattered = wide;
    addLink(wideScattered.links,
            BrushLink{DynamicSource::Random, DynamicTarget::Scatter, Curve{}, 3.0f, 3.0f});
    check(strokePreviewReach(wideScattered, lut) > strokePreviewReach(wide, lut) * 2.0f,
          "a RANDOM -> Scatter link of 3 radii more than doubles the reach at an UNCHANGED "
          "radius -- and RANDOM is STROKE-LOCAL, so brushTipFor() reports tip.scatter == 0 for "
          "it and the reach has to read the link set directly or this brush scatters its dabs "
          "straight off the top of its own document");
  }

  // ======================================================================
  // 5. The CACHE. A preview whose cache stops invalidating is indis-
  //    tinguishable, from the user's chair, from a slider that does
  //    nothing -- which is the complaint this whole feature exists to
  //    answer, so it is asserted rather than assumed.
  // ======================================================================
  {
    StrokePreviewCache cache;
    const uint64_t gen0 = cache.generation();
    cache.imageFor(base, lut);
    check(cache.rasterisations() == 1 && cache.generation() > gen0,
          "cache: the first call rasterises and bumps the generation");

    cache.imageFor(base, lut);
    check(cache.rasterisations() == 1 && cache.hits() == 1,
          "cache: an unchanged brush HITS -- no second rasterisation, and the generation the "
          "GPU keys its upload on does not move");

    // Every field below is one a single dab cannot express, which means every
    // one of them would have been missed by `dabPreviewTipsEqual()`'s own
    // subset. This is the list that makes `brushTipEqual()` worth its
    // static_assert.
    struct Case {
      const char* what;
      BrushState brush;
    };
    BrushState spacing = base;
    spacing.spacing = 0.7f;
    BrushState radius = base;
    radius.radius = 40.0f;
    BrushState hardness = base;
    hardness.hardness = 0.9f;
    BrushState roundness = base;
    roundness.roundness = 0.3f;
    BrushState grain = base;
    grain.grain.enabled = true;
    BrushState linked = base;
    addLink(linked.links, BrushLink{DynamicSource::Random, DynamicTarget::Scatter, Curve{}, 0.0f, 2.0f});

    const Case cases[] = {{"spacing", spacing},     {"radius", radius},
                          {"hardness", hardness},   {"roundness", roundness},
                          {"grain", grain},         {"a new stroke-local LINK", linked}};

    size_t missed = 0;
    for (const Case& c : cases) {
      const uint64_t before = cache.rasterisations();
      cache.imageFor(c.brush, lut);
      if (cache.rasterisations() != before + 1) {
        ++missed;
        std::printf("    [missed] cache did not invalidate on %s\n", c.what);
      }
      cache.imageFor(base, lut);  // back to the reference, so each case is independent
    }
    check(missed == 0,
          "cache: invalidates on spacing, radius, hardness, roundness, grain and a new "
          "stroke-local link -- FOUR of those six are invisible to a single dab, so this is the "
          "list dabPreviewTipsEqual()'s own narrower key would have got wrong");

    // The stroke-local link case deserves its own named assertion: it is the
    // one that is not a tip field at all, so a key built only from tips would
    // pass every other case here and still hand back a stale stroke.
    StrokePreviewCache linkCache;
    linkCache.imageFor(base, lut);
    const uint64_t beforeLink = linkCache.rasterisations();
    linkCache.imageFor(linked, lut);
    check(linkCache.rasterisations() == beforeLink + 1,
          "cache: a RANDOM -> Scatter link added with every TIP field unchanged still "
          "invalidates -- stroke-local sources never reach a BrushTip, so the key compares the "
          "link SET too");
  }

  // ======================================================================
  // 6. brushTipEqual() itself -- the complete comparison the cache leans
  //    on, and the fields a dab-shaped key omits.
  // ======================================================================
  {
    BrushTip a;
    check(brushTipEqual(a, a), "brushTipEqual: a tip equals itself");

    BrushTip spacing = a;
    spacing.spacing = a.spacing + 0.1f;
    check(!brushTipEqual(a, spacing),
          "brushTipEqual: SPACING alone makes two tips differ -- it cannot change one dab, "
          "which is exactly why the dab preview's own comparison omits it");

    BrushTip scatter = a;
    scatter.scatter = 2.0f;
    check(!brushTipEqual(a, scatter), "brushTipEqual: SCATTER alone makes two tips differ");

    BrushTip axes = a;
    axes.scatterBothAxes = !a.scatterBothAxes;
    check(!brushTipEqual(a, axes), "brushTipEqual: scatterBothAxes alone makes two tips differ");

    BrushTip floor = a;
    floor.sizeFloorPx = 5.0f;
    check(!brushTipEqual(a, floor),
          "brushTipEqual: sizeFloorPx alone makes two tips differ -- B6's floor is applied "
          "downstream of the tip, so two tips can agree on radius and still deposit differently");

    BrushTip opacity = a;
    opacity.opacity = 0.5f;
    check(!brushTipEqual(a, opacity), "brushTipEqual: opacity alone makes two tips differ");

    BrushTip grain = a;
    grain.grain.enabled = true;
    check(!brushTipEqual(a, grain), "brushTipEqual: grain alone makes two tips differ");
  }

  // ======================================================================
  // 7. A REFUSAL is reported, not rendered as an empty box. A strip that
  //    silently drew nothing would be the silent-no-op class
  //    (docs/reachability-audit.md) wearing a preview's clothes.
  // ======================================================================
  {
    BrushState nothing = base;
    // Every enabled link that could shrink Size, set to a range of exactly
    // zero: the size product collapses and the dab has no radius to cover a
    // texel with. The stroke still BEGINS -- there is a paintable layer -- so
    // this is the "emitted dabs, wrote no texels" state the image's own
    // comment distinguishes from a refusal, and it must be distinguishable.
    nothing.radius = kBrushRadiusMin;
    for (BrushLink& l : nothing.links.links)
      if (l.target == DynamicTarget::Size) {
        l.rangeLo = 0.0f;
        l.rangeHi = 0.0f;
      }
    const StrokePreviewImage img = rasteriseStrokePreview(nothing, lut);
    check(!img.refused,
          "a brush whose size product collapses is NOT reported as refused -- the stroke began "
          "fine, which is a different state from a route that would not take it");
    check(img.texels == 0,
          "...and it honestly reports zero texels covered, which is what lets the panel say "
          "something true instead of showing a blank strip that reads as a broken preview");
  }

  std::printf("[selftest] stroke preview %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
