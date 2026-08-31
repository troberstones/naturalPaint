#include "app/selftest/Support.hpp"

namespace np {

// ---------------------------------------------------------------------------
// PLAN.md Phase 5 step 2 -- core/Blend: the linear-safe set (over, plus,
// multiply, screen, min, max) plus `Mix`, the KM latent lerp, with the
// display-referred modes labelled as such. See app/SelfTest.hpp for the
// section's own contents list.
// ---------------------------------------------------------------------------
bool runBlendTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // --- Tolerances, derived rather than guessed ---------------------------
  //
  // **Almost everything below asserts at exactly zero tolerance, and that is
  // the design of the fixtures rather than a shortcut.** Every value fed to
  // `blendPixel()` here is a dyadic rational with a short binary expansion
  // (halves, quarters, eighths), and every formula in core/Blend is built from
  // `+`, `-`, `*`, `min` and `max` only -- there is not a single division in
  // any of them, which is precisely why they were derived into premultiplied
  // form instead of un-premultiplying and re-premultiplying around a straight-
  // colour blend. So each product and each sum lands back on the float grid
  // exactly, and a hand-computed reference can be compared with `==`.
  //
  // Two places need a tolerance and each derives its own:
  //
  //  * `kUnpremultiplyTol`, for anything read back through
  //    `flattenDocumentToLinear()`, whose final un-premultiply is one float
  //    division by the composited alpha. IEEE-754 requires it correctly
  //    rounded, so at most half an ulp: for a result in [0.25, 1) that is
  //    2^-25 = 2.98e-8 absolute. Landed 1.0e-7 -- 3.4x the derived bound,
  //    the identical figure and identical derivation runLayerStackTest() uses.
  //  * `kMixboxTol`, for the Mixbox comparison, derived at its own fixture
  //    below where the quantities it bounds are in view.
  constexpr float kUnpremultiplyTol = 1.0e-7f;

  auto eq4 = [](const std::array<float, 4>& a, float r, float g, float b, float alpha) {
    return a[0] == r && a[1] == g && a[2] == b && a[3] == alpha;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // The six modes this build composites, in table order, for the loops that
  // must hold for *every* mode rather than for one.
  const BlendMode kImplemented[] = {BlendMode::Normal, BlendMode::Plus, BlendMode::Multiply,
                                    BlendMode::Screen, BlendMode::Min,  BlendMode::Max};

  // --- The vocabulary: one table, one parse, one spelling ----------------
  {
    check(allBlendModes().size() == 27,
          "table: twenty-seven modes -- the linear-safe six, `Mix`, Stage 1's/Stage 2's seven "
          "new display-referred modes each, and Stage 3's six non-separable modes "
          "(docs/blend-mode-gaps.md)");

    bool indexedByEnumerator = true, namesUnique = true, labelsUnique = true,
         roundTrips = true;
    for (const BlendModeInfo& a : allBlendModes()) {
      if (&blendModeInfo(a.mode) != &a) indexedByEnumerator = false;
      const std::optional<BlendMode> back = blendModeFromName(a.name);
      if (!back.has_value() || *back != a.mode) roundTrips = false;
      for (const BlendModeInfo& b : allBlendModes()) {
        if (&a == &b) continue;
        if (std::string(a.name) == b.name) namesUnique = false;
        if (std::string(a.label) == b.label) labelsUnique = false;
      }
    }
    check(indexedByEnumerator,
          "table: blendModeInfo(m) returns m's own row -- the table is indexed by enumerator, "
          "so a row in the wrong place would hand out another mode's B7 classification");
    check(namesUnique && labelsUnique,
          "table: every np:blend name and every UI label is distinct");
    check(roundTrips,
          "table: name -> mode -> name round-trips for all fourteen, so nothing on disk means "
          "two things");

    check(blendModeFromName(kDefaultBlendName) == BlendMode::Normal,
          "table: core/Layer.hpp's kDefaultBlendName (\"normal\") IS `over` -- the default a "
          "Layer is constructed with must resolve, or every untouched document warns");
    // "linear-burn" used to be this file's stock example of a name outside the
    // set; Stage 1 (docs/blend-mode-gaps.md) made it a real mode, so "dissolve"
    // (still unimplemented -- one of Photoshop's non-separable modes, out of
    // scope for every stage of this effort) takes over as the unrecognised
    // fixture here and at every other use below.
    check(!blendModeFromName("dissolve").has_value() && !blendModeFromName("").has_value(),
          "table: an unrecognised name and an empty one both fail to parse rather than "
          "resolving to a default");
    check(!blendModeFromName("Multiply").has_value() && !blendModeFromName("MULTIPLY").has_value(),
          "table: the match is case-SENSITIVE -- two spellings on disk would both be `the` "
          "name and neither would round-trip the other");

    // **Widened by PLAN.md Phase 5 step 3**, which split "implemented" into
    // two fields because they answer different questions and both are asked:
    // `compositesPixels` is about `blendPixel()` (two RGBA texels) and
    // `compositesLatents` is about core/Composite's document walk (two Pigment
    // layers). `Mix` is permanently false for the first -- an RGBA texel has
    // no latent, ever -- and true for the second. The single-source-of-truth
    // property this assertion exists for is unchanged; it now covers a table
    // with two columns instead of one.
    bool implementedAgrees = true;
    for (const BlendModeInfo& info : allBlendModes())
      if (blendIsImplemented(info.name) != (info.compositesPixels || info.compositesLatents))
        implementedAgrees = false;
    check(implementedAgrees,
          "table: blendIsImplemented() answers exactly compositesPixels || compositesLatents, "
          "so there is one source of truth for what this build can composite");
    bool exactlyOneLatentMode = true;
    for (const BlendModeInfo& info : allBlendModes())
      if (info.compositesLatents != (info.mode == BlendMode::Mix)) exactlyOneLatentMode = false;
    check(exactlyOneLatentMode && !blendModeInfo(BlendMode::Mix).compositesPixels,
          "table: `Mix` is the one latent-level mode and is NOT a texel-level one -- the two "
          "flags separate the set rather than both meaning `implemented`");
    check(blendIsImplemented("multiply") && blendIsImplemented("screen") &&
              blendIsImplemented("min") && blendIsImplemented("max") &&
              blendIsImplemented("plus"),
          "table: the five modes step 1 could not composite are implemented now");
    check(blendIsImplemented("mix") && !blendIsImplemented("dissolve") &&
              !blendIsImplemented(""),
          "table: `mix` is implemented too now, at the layer level -- and an unrecognised "
          "name and an empty one still are not");
  }

  // --- PRD B7: display-referred modes are labelled as such ---------------
  //
  // The label is a field on the mode's metadata, so this section reads the
  // classification out of the data and then proves the classification is
  // *true* rather than merely present.
  {
    size_t displayReferred = 0;
    bool markerMatchesData = true;
    for (const BlendModeInfo& info : allBlendModes()) {
      const std::string entry = blendMenuEntryText(info.mode);
      const bool marked = entry.find("(display-referred)") != std::string::npos;
      if (marked != (info.space == BlendSpace::DisplayReferred)) markerMatchesData = false;
      if (info.space == BlendSpace::DisplayReferred) ++displayReferred;
    }
    check(markerMatchesData,
          "B7: every mode's menu entry carries the display-referred marker exactly when its "
          "BlendModeInfo::space says so -- the label is derived from the data on every call, "
          "so there is no path from a mode to menu text that skips it");
    check(displayReferred == 21 &&
              blendModeInfo(BlendMode::Screen).space == BlendSpace::DisplayReferred,
          "B7: twenty-one modes in the set are display-referred -- `screen` plus Stage 1's, "
          "Stage 2's and Stage 3's seven/seven/six new modes (docs/blend-mode-gaps.md), each "
          "of which treats 1.0 as a reference white or black (or, for Stage 3, bakes in "
          "[0,1] semantics via Lum()/Sat()/ClipColor())");

    // ...and here is why, numerically, rather than by assertion. The criterion
    // core/Blend.hpp states is monotonicity over the whole non-negative range
    // a linear working space can hold. At full coverage on both sides the
    // formulas reduce to their straight-colour forms, so this is the plain
    // test: raise the backdrop and see whether the result rises.
    const std::array<float, 4> opaqueSrc{2.0f, 2.0f, 2.0f, 1.0f};
    const std::array<float, 4> dimBackdrop{2.0f, 2.0f, 2.0f, 1.0f};
    const std::array<float, 4> brightBackdrop{3.0f, 3.0f, 3.0f, 1.0f};
    const float screenDim = blendPixel(BlendMode::Screen, opaqueSrc, dimBackdrop)[0];
    const float screenBright = blendPixel(BlendMode::Screen, opaqueSrc, brightBackdrop)[0];
    std::printf("  [measured] screen(2,2) = %.3f, screen(2,3) = %.3f (both should be >= 2 for a "
                "mode with no reference white)\n",
                static_cast<double>(screenDim), static_cast<double>(screenBright));
    check(screenDim == 0.0f && screenBright == -1.0f,
          "B7: screen(2,2) is exactly 0 and screen(2,3) exactly -1 -- above 1.0 it is not even "
          "monotone, so adding light makes it darker. That is what `1.0 is white` costs, and "
          "it is why screen carries the label");

    bool othersMonotone = true;
    for (BlendMode m : kImplemented) {
      if (m == BlendMode::Screen) continue;
      const float dim = blendPixel(m, opaqueSrc, dimBackdrop)[0];
      const float bright = blendPixel(m, opaqueSrc, brightBackdrop)[0];
      if (!(bright >= dim)) othersMonotone = false;
    }
    check(othersMonotone,
          "B7: and every other implemented mode IS monotone at the same HDR values -- so the "
          "classification separates the set rather than labelling everything");

    // The row shows the label too, not only the dropdown.
    Layer row;
    row.kind = LayerKind::RGB;
    row.blend = "screen";
    check(contains(layerRowSubLine(row), "SCREEN (display-referred)"),
          "B7: the layer row's sub-line carries the label as well -- a label that only "
          "appeared while the dropdown was open would not be `labelled as such`");
    row.blend = "multiply";
    check(!contains(layerRowSubLine(row), "display-referred") &&
              !contains(layerRowSubLine(row), "(!)"),
          "B7: a linear-light mode's row carries neither the label nor the unimplemented "
          "marker -- the working space IS linear, so labelling the majority case would bury "
          "the minority one");
    // The inverse of what step 2 asserted here, and the change is the feature:
    // `Mix` said "(not composited yet)" while it did not composite. It does
    // now, in exactly the situation this menu offers it in (the menu filters
    // through the same PRD L5 predicate the mix pairing uses), so the marker
    // would be a warning about nothing. No mode in the set carries it today;
    // the mechanism survives for `layerRowSubLine()`'s `(!)`, which marks a
    // carried blend name from a newer build -- the case that really does still
    // exist and that no dropdown ever offers.
    bool noStaleMarker = true;
    for (const BlendModeInfo& info : allBlendModes())
      if (contains(blendMenuEntryText(info.mode), "(not composited yet)")) noStaleMarker = false;
    check(noStaleMarker && !contains(blendMenuEntryText(BlendMode::Mix), "not composited"),
          "B7: no mode the dropdown can offer is marked `not composited yet` any more -- "
          "`Mix` shed that marker when step 3 wired it up");
  }

  // --- Every mode against a hand-computed reference: OPAQUE --------------
  //
  // Both fully opaque, so each formula collapses to its straight-colour form
  // and the references are the textbook ones:
  //
  //   src straight = premultiplied = (0.5,  0.25, 0.75), a = 1
  //   dst straight = premultiplied = (0.5,  1.0,  0.25), a = 1
  //
  //   plus      cs + cb            = (1.0,   1.25,  1.0)
  //   multiply  cs * cb            = (0.25,  0.25,  0.1875)
  //   screen    cs + cb - cs*cb    = (0.75,  1.0,   0.8125)
  //   min                          = (0.5,   0.25,  0.25)
  //   max                          = (0.5,   1.0,   0.75)
  //   over      cs                 = (0.5,   0.25,  0.75)
  //
  // Note plus and screen deliberately exceed 1.0 in a channel: nothing here
  // clamps, because clamping is a display/export policy (color/Space.hpp) and
  // io/Export makes that decision at its own quantization step.
  {
    const std::array<float, 4> s{0.5f, 0.25f, 0.75f, 1.0f};
    const std::array<float, 4> d{0.5f, 1.0f, 0.25f, 1.0f};
    check(eq4(blendPixel(BlendMode::Normal, s, d), 0.5f, 0.25f, 0.75f, 1.0f),
          "opaque: over gives the source exactly -- an opaque layer hides what is under it");
    check(eq4(blendPixel(BlendMode::Plus, s, d), 1.0f, 1.25f, 1.0f, 1.0f),
          "opaque: plus gives (1.0, 1.25, 1.0) exactly, unclamped past 1.0");
    check(eq4(blendPixel(BlendMode::Multiply, s, d), 0.25f, 0.25f, 0.1875f, 1.0f),
          "opaque: multiply gives (0.25, 0.25, 0.1875) exactly");
    check(eq4(blendPixel(BlendMode::Screen, s, d), 0.75f, 1.0f, 0.8125f, 1.0f),
          "opaque: screen gives (0.75, 1.0, 0.8125) exactly");
    check(eq4(blendPixel(BlendMode::Min, s, d), 0.5f, 0.25f, 0.25f, 1.0f),
          "opaque: min takes the darker channel of each pair");
    check(eq4(blendPixel(BlendMode::Max, s, d), 0.5f, 1.0f, 0.75f, 1.0f),
          "opaque: max takes the lighter channel of each pair");
  }

  // --- Stage 2, opaque: Hard Light, Overlay, Linear Light, hand-computed -
  //
  // Both fully opaque again, so as = ab = 1, sOnly = bOnly = 0, and
  // out = B(Cb,Cs) directly -- the un-premultiply in blendPixel() divides by
  // exactly 1.0, so this is as exact as the OPAQUE section above.
  //
  //   src straight = premultiplied = Cs = (0.25, 0.5,  0.75), a = 1
  //   dst straight = premultiplied = Cb = (0.75, 0.25, 0.5 ), a = 1
  //
  // Deliberately Cb != Cs in every channel, so Overlay's argument swap
  // (Overlay(Cb,Cs) = HardLight(Cs,Cb), condition on Cb not Cs) cannot pass
  // by accidental symmetry -- a reverted swap would test Cs instead and
  // give Hard Light's own answer, which channel r below catches.
  //
  //   HardLight(Cb,Cs) = Cs<=0.5 ? Cb*2Cs : Cb+(2Cs-1)-Cb*(2Cs-1)
  //     r: Cb=0.75, Cs=0.25 <=0.5 -> 0.75*0.5              = 0.375
  //     g: Cb=0.25, Cs=0.5  <=0.5 -> 0.25*1.0              = 0.25
  //     b: Cb=0.5,  Cs=0.75 >0.5  -> 0.5+0.5-0.5*0.5       = 0.75
  //   HardLight(s,d) = (0.375, 0.25, 0.75)
  //
  //   Overlay(Cb,Cs) = HardLight(Cs,Cb), i.e. Cb<=0.5 ? Cs*2Cb : Cs+(2Cb-1)-Cs*(2Cb-1)
  //     r: Cb=0.75 >0.5  -> Cs+(2Cb-1)-Cs*(2Cb-1) = 0.25+0.5-0.25*0.5 = 0.625
  //     g: Cb=0.25 <=0.5 -> Cs*2Cb = 0.5*0.5                          = 0.25
  //     b: Cb=0.5  <=0.5 -> Cs*2Cb = 0.75*1.0                         = 0.75
  //   Overlay(s,d) = (0.625, 0.25, 0.75) -- channel r differs from Hard
  //   Light's 0.375 precisely because the swap matters there and not in g/b.
  //
  //   LinearLight(Cb,Cs) = Cb + 2Cs - 1
  //     r: 0.75 + 0.5 - 1 = 0.25
  //     g: 0.25 + 1.0 - 1 = 0.25
  //     b: 0.5  + 1.5 - 1 = 1.0
  //   LinearLight(s,d) = (0.25, 0.25, 1.0)
  {
    const std::array<float, 4> s{0.25f, 0.5f, 0.75f, 1.0f};
    const std::array<float, 4> d{0.75f, 0.25f, 0.5f, 1.0f};
    check(eq4(blendPixel(BlendMode::HardLight, s, d), 0.375f, 0.25f, 0.75f, 1.0f),
          "stage2 opaque: hard light gives (0.375, 0.25, 0.75) exactly");
    check(eq4(blendPixel(BlendMode::Overlay, s, d), 0.625f, 0.25f, 0.75f, 1.0f),
          "stage2 opaque: overlay gives (0.625, 0.25, 0.75) exactly -- channel r differs from "
          "hard light's 0.375, which is exactly what breaks if the Cb<->Cs swap gets reverted");
    check(eq4(blendPixel(BlendMode::LinearLight, s, d), 0.25f, 0.25f, 1.0f, 1.0f),
          "stage2 opaque: linear light gives (0.25, 0.25, 1.0) exactly, via the single closed "
          "form Cb + 2Cs - 1");
  }

  // --- Every mode against a hand-computed reference: PARTIAL ALPHA -------
  //
  // **This is the fixture that catches the classic bug in this area.**
  // Multiply and screen are almost always written for straight, opaque colour;
  // applied to premultiplied values with partial alpha, the naive transcription
  // (`cs * cb`, `cs + cb - cs*cb` with nothing else) is wrong for multiply and
  // right for screen, and there is no way to tell which without doing the
  // arithmetic. So it is done here, twice, by two different routes.
  //
  // Fixture:
  //   src straight (1, 0, 0.5) at as = 0.5  ->  cs = (0.5, 0,   0.25)
  //   dst straight (0, 1, 0.25) at ab = 0.5 ->  cb = (0,   0.5, 0.125)
  //
  // Route 1 -- core/Blend's premultiplied forms:
  //   sOnly = 1-ab = 0.5, bOnly = 1-as = 0.5, ao = 0.5 + 0.5*0.5 = 0.75
  //   over      cs + cb*0.5                      = (0.5,    0.25,  0.3125)
  //   plus      cs + cb                          = (0.5,    0.5,   0.375)
  //   multiply  cs*cb + cs*0.5 + cb*0.5          = (0.25,   0.25,  0.21875)
  //   screen    cs + cb - cs*cb                  = (0.5,    0.5,   0.34375)
  //   min       min(ab*cs, as*cb) + cs*.5 + cb*.5= (0.25,   0.25,  0.25)
  //   max       max(ab*cs, as*cb) + cs*.5 + cb*.5= (0.5,    0.5,   0.3125)
  //
  // Route 2 -- Porter-Duff regions, computed independently. At as = ab = 0.5
  // the pixel splits into three regions of area 0.25 each (source only,
  // backdrop only, both) plus 0.25 of nothing. Taking the blue channel of
  // multiply: source-only 0.25*0.5 = 0.125, backdrop-only 0.25*0.25 = 0.0625,
  // both 0.25*(0.5*0.25) = 0.03125. Sum 0.21875. It agrees, and it agrees for
  // a reason that has nothing to do with the algebra above -- which is the
  // point of doing it twice.
  {
    const std::array<float, 4> s{0.5f, 0.0f, 0.25f, 0.5f};
    const std::array<float, 4> d{0.0f, 0.5f, 0.125f, 0.5f};
    check(eq4(blendPixel(BlendMode::Normal, s, d), 0.5f, 0.25f, 0.3125f, 0.75f),
          "partial: over at 50%/50% gives (0.5, 0.25, 0.3125) at alpha 0.75");
    check(eq4(blendPixel(BlendMode::Plus, s, d), 0.5f, 0.5f, 0.375f, 0.75f),
          "partial: plus is the premultiplied sum, and its alpha is still the UNION alpha "
          "0.75 rather than Porter-Duff PLUS's 1.0 -- coverage does not add because light "
          "does");
    check(eq4(blendPixel(BlendMode::Multiply, s, d), 0.25f, 0.25f, 0.21875f, 0.75f),
          "partial: multiply gives (0.25, 0.25, 0.21875) -- the three-term premultiplied "
          "form, NOT the naive cs*cb, which would give (0, 0, 0.03125) and lose both "
          "layers wherever the other does not cover");
    check(eq4(blendPixel(BlendMode::Screen, s, d), 0.5f, 0.5f, 0.34375f, 0.75f),
          "partial: screen gives (0.5, 0.5, 0.34375) -- here the premultiplied form really "
          "IS the straight one, cs + cb - cs*cb, which is exactly the coincidence that gets "
          "assumed for multiply and is false there");
    check(eq4(blendPixel(BlendMode::Min, s, d), 0.25f, 0.25f, 0.25f, 0.75f),
          "partial: min gives (0.25, 0.25, 0.25), via min(ab*cs, as*cb) -- no division by "
          "alpha anywhere");
    check(eq4(blendPixel(BlendMode::Max, s, d), 0.5f, 0.5f, 0.3125f, 0.75f),
          "partial: max gives (0.5, 0.5, 0.3125)");

    // The naive premultiplied transcriptions, written here so the assertions
    // above are demonstrably not tautologies against a copy of the code.
    check(blendPixel(BlendMode::Multiply, s, d)[2] != s[2] * d[2],
          "partial: and the naive `cs*cb` genuinely differs from what multiply returns, so "
          "the assertion above could not pass against the wrong implementation");
  }

  // --- Alpha is `over` for every mode ------------------------------------
  //
  // The separable-blend formula's `ao` does not mention the blend function at
  // all: a blend mode changes colour, not coverage. Checked across the whole
  // alpha grid rather than at one pair, because this is the property that
  // makes "switch a layer's blend and the composite's alpha channel does not
  // move" true.
  {
    bool alphaIsOver = true;
    const float alphas[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (float as : alphas) {
      for (float ab : alphas) {
        const std::array<float, 4> s{0.25f * as, 0.5f * as, 0.75f * as, as};
        const std::array<float, 4> d{0.75f * ab, 0.25f * ab, 0.5f * ab, ab};
        const float expect = compositeOver(s, d)[3];
        for (BlendMode m : kImplemented)
          if (blendPixel(m, s, d)[3] != expect) alphaIsOver = false;
      }
    }
    check(alphaIsOver,
          "alpha: every mode produces exactly `over`'s alpha across a 5x5 alpha grid -- a "
          "blend mode changes colour, never coverage");
  }

  // --- A fully transparent source is an exact identity, for every mode ---
  //
  // If this fails for any mode, that mode is wrong: an empty layer must
  // contribute nothing. It holds exactly (not nearly) because as == 0 makes
  // cs == 0 for well-formed premultiplied data, which kills every term
  // carrying cs and leaves `cb * 1.0f` -- a multiplication by literal 1.0f.
  {
    const std::array<float, 4> clear{0.0f, 0.0f, 0.0f, 0.0f};
    const std::array<float, 4> backdrops[] = {
        {0.75f, 0.25f, 0.5f, 1.0f},      // opaque
        {0.375f, 0.125f, 0.25f, 0.5f},   // half-covered
        {0.0f, 0.0f, 0.0f, 0.0f},        // also empty
        {1.5f, 0.0f, 2.25f, 1.0f},       // above 1.0, where screen misbehaves
    };
    bool identity = true, passthrough = true;
    for (const std::array<float, 4>& b : backdrops) {
      for (BlendMode m : kImplemented) {
        if (blendPixel(m, clear, b) != b) identity = false;
        // ...and the mirror: an empty backdrop passes the source through
        // untouched, which is what makes a single-layer document composite
        // identically under any mode.
        if (blendPixel(m, b, clear) != b) passthrough = false;
      }
      // `Mix` too: it falls back to `over`, and `over` has the identity.
      if (blendPixel(BlendMode::Mix, clear, b) != b) identity = false;
    }
    check(identity,
          "transparent: a fully transparent source is a BIT-EXACT identity on the backdrop "
          "for every mode, over four backdrops including an HDR one");
    check(passthrough,
          "transparent: and a fully transparent backdrop passes the source through bit-"
          "exactly for every mode -- which is why a single-layer document composites the "
          "same whatever its blend");
  }

  // --- Stage 1: the same two invariants, for the 7 new separable modes ---
  //
  // docs/blend-mode-gaps.md, Stage 1 of 3: Difference, Exclusion, Subtract,
  // Linear Burn, Color Dodge, Color Burn, Divide. Unlike the modes above,
  // these are not division-free in premultiplied form -- each un-premultiplies
  // to straight colour, applies Photoshop's own [0,1] formula, and
  // re-premultiplies, guarded so `src/as` can never divide by zero. `kImplemented`
  // above is deliberately NOT widened to include them: that array's loop
  // asserts monotonicity (B7's LinearLight criterion), and none of these seven
  // are monotone over the whole non-negative range -- they are
  // BlendSpace::DisplayReferred precisely because they are not. So the same
  // two checks are re-run here against a separate, local array instead.
  {
    const BlendMode kStage1[] = {BlendMode::Difference, BlendMode::Exclusion, BlendMode::Subtract,
                                  BlendMode::LinearBurn, BlendMode::ColorDodge, BlendMode::ColorBurn,
                                  BlendMode::Divide};
    bool alphaIsOver = true;
    const float alphas[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (float as : alphas) {
      for (float ab : alphas) {
        const std::array<float, 4> s{0.25f * as, 0.5f * as, 0.75f * as, as};
        const std::array<float, 4> d{0.75f * ab, 0.25f * ab, 0.5f * ab, ab};
        const float expect = compositeOver(s, d)[3];
        for (BlendMode m : kStage1)
          if (blendPixel(m, s, d)[3] != expect) alphaIsOver = false;
      }
    }
    check(alphaIsOver, "stage1: alpha is over for all 7 new modes too");

    const std::array<float, 4> clear{0.0f, 0.0f, 0.0f, 0.0f};
    const std::array<float, 4> backdrops[] = {
        {0.75f, 0.25f, 0.5f, 1.0f}, {0.375f, 0.125f, 0.25f, 0.5f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.5f, 0.0f, 2.25f, 1.0f}};
    bool identity = true, passthrough = true;
    for (const auto& b : backdrops) {
      for (BlendMode m : kStage1) {
        if (blendPixel(m, clear, b) != b) identity = false;
        if (blendPixel(m, b, clear) != b) passthrough = false;
      }
    }
    check(identity, "stage1: transparent source is a bit-exact identity for all 7 new modes");
    check(passthrough, "stage1: transparent backdrop passes the source through for all 7 new modes");
  }

  // --- Stage 1: hand-computed references, opaque alpha -------------------
  //
  // Both fully opaque, so premultiplied == straight and each formula's B(Cb,Cs)
  // is the whole answer directly (sOnly = bOnly = 0, as*ab = 1).
  //
  //   src straight (Cs) = (0.5, 0.75, 0.25), a = 1
  //   dst straight (Cb) = (0.25, 0.125, 0.0), a = 1
  //
  //   difference  |Cb - Cs|                       = (0.25, 0.625, 0.25)
  //   subtract    Cb - Cs                         = (-0.25, -0.625, -0.25)
  //   color dodge R: Cb=0.25 != 0, Cs=0.5 < 1: min(1, 0.25/(1-0.5))  = min(1, 0.5)      = 0.5
  //               G: Cb=0.125 != 0, Cs=0.75 < 1: min(1, 0.125/(1-0.75)) = min(1, 0.5)   = 0.5
  //               B: Cb=0.0 -> the Cb==0 branch fires regardless of Cs             = 0.0
  //               so color dodge                                    = (0.5, 0.5, 0.0)
  {
    const std::array<float, 4> s{0.5f, 0.75f, 0.25f, 1.0f};
    const std::array<float, 4> d{0.25f, 0.125f, 0.0f, 1.0f};
    check(eq4(blendPixel(BlendMode::Difference, s, d), 0.25f, 0.625f, 0.25f, 1.0f),
          "stage1 opaque: difference gives (0.25, 0.625, 0.25) exactly, |Cb - Cs| per channel");
    check(eq4(blendPixel(BlendMode::Subtract, s, d), -0.25f, -0.625f, -0.25f, 1.0f),
          "stage1 opaque: subtract gives (-0.25, -0.625, -0.25) exactly, Cb - Cs -- unclamped, "
          "matching this file's own stated policy that nothing here clamps the output");
    check(eq4(blendPixel(BlendMode::ColorDodge, s, d), 0.5f, 0.5f, 0.0f, 1.0f),
          "stage1 opaque: color dodge gives (0.5, 0.5, 0.0) exactly -- the blue channel "
          "exercises the Cb==0 special case, which must fire before any division is attempted");
  }

  // --- Stage 2: the same two invariants, for the 7 new "light family" modes --
  {
    const BlendMode kStage2[] = {BlendMode::HardLight, BlendMode::Overlay, BlendMode::VividLight,
                                  BlendMode::LinearLight, BlendMode::PinLight, BlendMode::SoftLight,
                                  BlendMode::HardMix};
    bool alphaIsOver = true;
    const float alphas[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (float as : alphas) {
      for (float ab : alphas) {
        const std::array<float, 4> s{0.25f * as, 0.5f * as, 0.75f * as, as};
        const std::array<float, 4> d{0.75f * ab, 0.25f * ab, 0.5f * ab, ab};
        const float expect = compositeOver(s, d)[3];
        for (BlendMode m : kStage2)
          if (blendPixel(m, s, d)[3] != expect) alphaIsOver = false;
      }
    }
    check(alphaIsOver, "stage2: alpha is over for all 7 new modes too");

    const std::array<float, 4> clear{0.0f, 0.0f, 0.0f, 0.0f};
    const std::array<float, 4> backdrops[] = {
        {0.75f, 0.25f, 0.5f, 1.0f}, {0.375f, 0.125f, 0.25f, 0.5f}, {0.0f, 0.0f, 0.0f, 0.0f}, {1.5f, 0.0f, 2.25f, 1.0f}};
    bool identity = true, passthrough = true;
    for (const auto& b : backdrops) {
      for (BlendMode m : kStage2) {
        if (blendPixel(m, clear, b) != b) identity = false;
        if (blendPixel(m, b, clear) != b) passthrough = false;
      }
    }
    check(identity, "stage2: transparent source is a bit-exact identity for all 7 new modes");
    check(passthrough, "stage2: transparent backdrop passes the source through for all 7 new modes");
  }

  // --- Stage 3: the same two invariants, for the 6 new non-separable modes -
  //
  // Hue/Saturation/Color/Luminosity/DarkerColor/LighterColor un-premultiply
  // (divide by alpha) before doing anything else, which is exactly the
  // operation that turns a well-formed as==0 texel into NaN if it is not
  // guarded: 0.0f/0.0f is NaN, and 0.0f * NaN is NaN, not 0.0f, so the
  // guard is load-bearing rather than defensive decoration. This section is
  // the six modes' own version of the two checks above.
  {
    const BlendMode kStage3[] = {BlendMode::Hue,        BlendMode::Saturation,
                                 BlendMode::Color,      BlendMode::Luminosity,
                                 BlendMode::DarkerColor, BlendMode::LighterColor};

    bool alphaIsOver = true;
    const float alphas[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    for (float as : alphas) {
      for (float ab : alphas) {
        const std::array<float, 4> s{0.25f * as, 0.5f * as, 0.75f * as, as};
        const std::array<float, 4> d{0.75f * ab, 0.25f * ab, 0.5f * ab, ab};
        const float expect = compositeOver(s, d)[3];
        for (BlendMode m : kStage3)
          if (blendPixel(m, s, d)[3] != expect) alphaIsOver = false;
      }
    }
    check(alphaIsOver, "stage3: alpha is over for all 6 new non-separable modes too");

    const std::array<float, 4> clear{0.0f, 0.0f, 0.0f, 0.0f};
    const std::array<float, 4> backdrops[] = {
        {0.75f, 0.25f, 0.5f, 1.0f},
        {0.375f, 0.125f, 0.25f, 0.5f},
        {0.0f, 0.0f, 0.0f, 0.0f},
        {1.5f, 0.0f, 2.25f, 1.0f},
    };
    bool identity = true, passthrough = true;
    for (const auto& b : backdrops) {
      for (BlendMode m : kStage3) {
        if (blendPixel(m, clear, b) != b) identity = false;
        if (blendPixel(m, b, clear) != b) passthrough = false;
      }
    }
    check(identity, "stage3: transparent source is a bit-exact identity for all 6 new modes -- "
                    "this is THE assertion that catches a missing or wrong alpha==0 early-return "
                    "guard around the un-premultiply division");
    check(passthrough, "stage3: transparent backdrop passes the source through for all 6 new modes");

    // --- Hand-computed references: Color and Luminosity, opaque ------------
    //
    // as = ab = 1, so Cs = src and Cb = dst directly -- no division, the
    // simplest case to hand-verify. Photoshop's own luma weights (0.3, 0.59,
    // 0.11) are NOT dyadic, unlike the rest of this file's fixtures, so the
    // per-channel results below use `near()` rather than `==`: hand-computed
    // decimal arithmetic and the actual float32 sequence of multiplies/adds
    // are not guaranteed to land on the identical bit pattern even though
    // both compute "the same" value. `kHslTol` is generous relative to the
    // few float ops involved (two lumHSL() evaluations -- 3 multiplies + 2
    // adds each -- one subtraction, three additions: at most ~15 rounding
    // steps on operands of magnitude <= 1, i.e. <= 15 * 2^-24 =~ 9.0e-7);
    // alpha needs no tolerance since it is exactly 1.0f by construction
    // (as = ab = 1 makes ao = as + ab*(1-as) = 1 + 0 exactly).
    //
    //   Cs (source straight) = (0.6, 0.2, 0.2)
    //   Cb (backdrop straight) = (0.2, 0.2, 0.8)
    //   lumHSL(Cb) = 0.3*0.2 + 0.59*0.2 + 0.11*0.8 = 0.06 + 0.118 + 0.088 = 0.266
    //   lumHSL(Cs) = 0.3*0.6 + 0.59*0.2 + 0.11*0.2 = 0.18 + 0.118 + 0.022 = 0.32
    //
    //   Color(Cb,Cs) = setLumHSL(Cs, lumHSL(Cb)):
    //     d = 0.266 - 0.32 = -0.054
    //     result = (0.6-0.054, 0.2-0.054, 0.2-0.054) = (0.546, 0.146, 0.146)
    //     (n = 0.146 >= 0 and x = 0.546 <= 1, so ClipColor's branches do not
    //     trigger -- chosen deliberately so this stays simple arithmetic)
    //
    //   Luminosity(Cb,Cs) = setLumHSL(Cb, lumHSL(Cs)):
    //     d = 0.32 - 0.266 = 0.054
    //     result = (0.2+0.054, 0.2+0.054, 0.8+0.054) = (0.254, 0.254, 0.854)
    //     (n = 0.254 >= 0 and x = 0.854 <= 1, no clip here either)
    constexpr float kHslTol = 2.0e-6f;
    const std::array<float, 4> hslSrc{0.6f, 0.2f, 0.2f, 1.0f};
    const std::array<float, 4> hslDst{0.2f, 0.2f, 0.8f, 1.0f};
    const std::array<float, 4> colorResult = blendPixel(BlendMode::Color, hslSrc, hslDst);
    check(near(colorResult[0], 0.546f, kHslTol) && near(colorResult[1], 0.146f, kHslTol) &&
              near(colorResult[2], 0.146f, kHslTol) && colorResult[3] == 1.0f,
          "stage3: color gives (0.546, 0.146, 0.146) at alpha 1 -- source hue/saturation, "
          "backdrop luminance, hand-verified above");
    const std::array<float, 4> lumResult = blendPixel(BlendMode::Luminosity, hslSrc, hslDst);
    check(near(lumResult[0], 0.254f, kHslTol) && near(lumResult[1], 0.254f, kHslTol) &&
              near(lumResult[2], 0.854f, kHslTol) && lumResult[3] == 1.0f,
          "stage3: luminosity gives (0.254, 0.254, 0.854) at alpha 1 -- the mirror of color, "
          "hand-verified above");

    // --- Darker/Lighter Color: a whole-triple select, no arithmetic --------
    //
    // Unlike Color/Luminosity, these two never add or rescale -- they return
    // one of the two input triples completely unchanged. At alpha 1 the
    // premultiplied and straight forms coincide, so the result is bit-exact
    // equal to one of the two source arrays and `eq4` is exact, not `near`.
    //
    //   lumHSL(bright) = 0.9 * (0.3+0.59+0.11) = 0.9 * 1.0 = 0.9
    //   lumHSL(dark)   = 0.1 * 1.0 = 0.1
    //   DarkerColor:  lum(dark) <= lum(bright)  -> true  -> returns dark
    //   LighterColor: lum(dark) >= lum(bright)  -> false -> returns bright
    const std::array<float, 4> bright{0.9f, 0.9f, 0.9f, 1.0f};
    const std::array<float, 4> dark{0.1f, 0.1f, 0.1f, 1.0f};
    check(eq4(blendPixel(BlendMode::DarkerColor, bright, dark), 0.1f, 0.1f, 0.1f, 1.0f),
          "stage3: darker-color of a bright source over a dark backdrop returns the dark "
          "triple verbatim -- a whole-triple select, not a per-channel min");
    check(eq4(blendPixel(BlendMode::LighterColor, bright, dark), 0.9f, 0.9f, 0.9f, 1.0f),
          "stage3: lighter-color of the same pair returns the bright triple verbatim");
  }

  // --- `over` did not move by one ulp ------------------------------------
  //
  // Step 1's regression boundary rests on `compositeOver()` being exactly what
  // it was, so this checks it against a second, independent transcription of
  // step 1's formula written here rather than called -- the same discipline
  // the journal section's second FNV-1a and step 1's second plain sum follow.
  // The values are deliberately NOT dyadic: 0.1f and 0.3f are not exact in
  // binary, so any reassociation of `src + dst*(1-src.a)` would show.
  {
    auto step1Over = [](const std::array<float, 4>& src, const std::array<float, 4>& dst) {
      const float inv = 1.0f - src[3];
      return std::array<float, 4>{src[0] + dst[0] * inv, src[1] + dst[1] * inv,
                                  src[2] + dst[2] * inv, src[3] + dst[3] * inv};
    };
    bool identicalToStep1 = true, dispatchIsOver = true;
    for (int i = 0; i <= 20; ++i) {
      for (int j = 0; j <= 20; ++j) {
        const float as = static_cast<float>(i) * 0.05f;
        const float ab = static_cast<float>(j) * 0.05f;
        const std::array<float, 4> s{0.1f * as, 0.3f * as, 0.7f * as, as};
        const std::array<float, 4> d{0.9f * ab, 0.3f * ab, 0.1f * ab, ab};
        if (compositeOver(s, d) != step1Over(s, d)) identicalToStep1 = false;
        if (blendPixel(BlendMode::Normal, s, d) != step1Over(s, d)) dispatchIsOver = false;
      }
    }
    check(identicalToStep1,
          "over: compositeOver() is bit-identical to step 1's formula across 441 non-dyadic "
          "alpha pairs -- moving it into core/Blend perturbed nothing");
    check(dispatchIsOver,
          "over: and blendPixel(Normal, ...) is the same function, not the general three-term "
          "form -- which is algebraically equal and NOT bit-equal, so the dispatch has to "
          "special-case it");
  }

  // --- The regression boundary, now for every mode -----------------------
  //
  // Step 1 asserts that a single-layer document and a non-overlapping
  // multi-layer one composite byte-identically to the plain sum they replaced.
  // This step could break that in a new way -- by making the answer depend on
  // the blend mode -- so the same claim is re-made here with a DIFFERENT blend
  // on every layer, against a second implementation of the plain sum written
  // in this test.
  {
    auto plainSum = [](const Document& doc) {
      const size_t w = static_cast<size_t>(doc.width);
      std::vector<float> out(w * static_cast<size_t>(doc.height) * 4, 0.0f);
      for (const Layer& layer : doc.layers) {
        if (layer.kind != LayerKind::RGB || !layer.rgbTiles.has_value()) continue;
        for (const auto& [coord, tile] : *layer.rgbTiles) {
          const PixelCoord origin = tileOrigin(coord);
          for (int32_t ty = 0; ty < kTileSize; ++ty) {
            const int32_t dy = origin.y + ty;
            if (dy < 0 || dy >= doc.height) continue;
            for (int32_t tx = 0; tx < kTileSize; ++tx) {
              const int32_t dx = origin.x + tx;
              if (dx < 0 || dx >= doc.width) continue;
              const std::array<float, 4> p = tile.readPixel(PixelCoord{tx, ty});
              float* o = &out[(static_cast<size_t>(dy) * w + static_cast<size_t>(dx)) * 4];
              for (int c = 0; c < 4; ++c) o[c] += p[c];
            }
          }
        }
      }
      return out;
    };
    auto addRgb = [](Document& doc, const char* blend) {
      Layer l;
      l.kind = LayerKind::RGB;
      l.rgbTiles.emplace();
      l.blend = blend;
      doc.layers.push_back(std::move(l));
    };
    auto writeStraight = [](Document& doc, size_t li, int32_t x, int32_t y, float r, float g,
                            float b, float a) {
      const PixelCoord p{x, y};
      doc.layers[li].rgbTiles->getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p),
                                                                     {r * a, g * a, b * a, a});
    };

    Document doc = Document::createBlank(300, 300, WorkingSpace{});
    doc.layers[0].blend = "multiply";
    addRgb(doc, "screen");
    addRgb(doc, "plus");
    addRgb(doc, "min");
    addRgb(doc, "max");
    // One texel per layer, all in different tiles, none overlapping, and with
    // values that are NOT exactly representable in half so a one-ulp
    // difference anywhere would show. One sits off-canvas to exercise the
    // clip, and one is translucent.
    writeStraight(doc, 0, 3, 3, 0.1f, 0.2f, 0.3f, 1.0f);
    writeStraight(doc, 1, 140, 3, 0.7f, 0.6f, 0.55f, 0.35f);
    writeStraight(doc, 2, 3, 140, 0.9f, 0.05f, 0.45f, 1.0f);
    writeStraight(doc, 3, 260, 260, 0.33f, 0.66f, 0.99f, 0.8f);
    writeStraight(doc, 4, 290, 290, 0.15f, 0.25f, 0.35f, 1.0f);

    const std::vector<float> composited = compositeDocumentPremultiplied(doc);
    const std::vector<float> summed = plainSum(doc);
    check(composited.size() == summed.size() &&
              std::memcmp(composited.data(), summed.data(),
                          composited.size() * sizeof(float)) == 0,
          "regression: five non-overlapping layers, each with a DIFFERENT blend mode, "
          "composite BYTE-identically to the plain sum step 1 replaced -- a blend mode "
          "cannot perturb content nothing overlaps");

    // And the negative control: one overlapping texel is enough to break it,
    // so the identity above is a property of non-overlap and not of the code.
    writeStraight(doc, 1, 3, 3, 0.4f, 0.4f, 0.4f, 0.5f);
    const std::vector<float> overlapped = compositeDocumentPremultiplied(doc);
    const std::vector<float> overlapSum = plainSum(doc);
    check(std::memcmp(overlapped.data(), overlapSum.data(),
                      overlapped.size() * sizeof(float)) != 0,
          "regression: and ONE overlapping texel breaks it, so the identity is about "
          "non-overlap rather than about the implementation");
  }

  // --- A blend mode really reaches the document walk ---------------------
  //
  // Everything above tests the primitive. This tests that core/Composite
  // resolves the layer's `blend` string and dispatches on it, at the document
  // level, hand-computed end to end.
  //
  //   bottom  straight (0.5, 1.0, 0.25) opaque, blend "normal"
  //   top     straight (0.5, 0.25, 0.75) opaque, blend "multiply"
  //   multiply of two opaque layers = the straight product
  //           = (0.25, 0.25, 0.1875), alpha 1
  //   alpha is exactly 1 so the flattener's un-premultiply is a division by
  //   1.0 and the whole chain is exact -- this one is a zero-tolerance check.
  {
    Document doc = Document::createBlank(1, 1, WorkingSpace{});
    Layer top;
    top.kind = LayerKind::RGB;
    top.rgbTiles.emplace();
    top.blend = "multiply";
    doc.layers.push_back(std::move(top));
    doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{0, 0},
                                                                   {0.5f, 1.0f, 0.25f, 1.0f});
    doc.layers[1].rgbTiles->getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{0, 0},
                                                                   {0.5f, 0.25f, 0.75f, 1.0f});

    std::vector<std::string> warnings;
    const DecodedImage flat = flattenDocumentToLinear(doc, &warnings);
    check(flat.valid() && flat.pixels[0] == 0.25f && flat.pixels[1] == 0.25f &&
              flat.pixels[2] == 0.1875f && flat.pixels[3] == 1.0f,
          "document: a `multiply` layer really multiplies through the flattener -- exactly "
          "(0.25, 0.25, 0.1875) at alpha 1, no tolerance");
    check(warnings.empty(),
          "document: and produces NO warning, because this build composites it faithfully "
          "now -- the step-1 approximation notice is gone for the five modes it landed");

    // The probe must agree, through the same dispatch. An eyedropper that read
    // `over` while the export wrote `multiply` would be a bug nobody could
    // explain.
    ProbeParams all;
    all.source = ProbeSource::AllLayers;
    const ProbeSample sample = probePixel(doc, PixelCoord{0, 0}, all);
    check(near(sample.linear[0], 0.25f, kUnpremultiplyTol) &&
              near(sample.linear[2], 0.1875f, kUnpremultiplyTol),
          "document: core/Probe dispatches on the same blend mode as the flattener, so the "
          "eyedropper and the export cannot disagree");

    // Switching only the blend mode must not move alpha -- the document-level
    // form of the per-pixel claim above.
    const float alphaMultiply = flat.pixels[3];
    doc.layers[1].blend = "screen";
    const DecodedImage screened = flattenDocumentToLinear(doc);
    check(screened.pixels[3] == alphaMultiply && screened.pixels[0] == 0.75f,
          "document: switching the blend mode changes colour (0.25 -> 0.75 in red) and "
          "leaves alpha exactly where it was");
  }

  // --- The unimplemented-blend contract, still holding in substance ------
  //
  // Step 1's ruling was: composited as `over`, warned by name, never silently,
  // never refused, and the value itself preserved verbatim (PRD I10). This
  // step changes *which* names fall into it, and must not weaken any of it.
  // The two remaining cases are checked separately because their sentences
  // differ -- an unknown name means "this build is behind the document",
  // `mix` means "this build is behind PLAN.md".
  {
    auto oneLayerDoc = [](const char* blend) {
      Document doc = Document::createBlank(1, 1, WorkingSpace{});
      Layer top;
      top.kind = LayerKind::RGB;
      top.rgbTiles.emplace();
      top.name = "Line pass";
      top.blend = blend;
      doc.layers.push_back(std::move(top));
      doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{0, 0},
                                                                     {0.5f, 0.0f, 0.0f, 0.5f});
      doc.layers[1].rgbTiles->getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{0, 0},
                                                                     {0.0f, 0.0f, 0.5f, 0.5f});
      return doc;
    };

    // "dissolve" here, not "linear-burn" -- see the comment where this file's
    // vocabulary section first makes the swap: Stage 1 made "linear-burn" a
    // real, implemented name, so it can no longer stand in for one this build
    // has never heard of.
    Document unknown = oneLayerDoc("dissolve");
    std::vector<std::string> unknownWarnings;
    const DecodedImage unknownFlat = flattenDocumentToLinear(unknown, &unknownWarnings);
    check(unknownWarnings.size() == 1 && contains(unknownWarnings[0], "dissolve") &&
              contains(unknownWarnings[0], "Line pass") &&
              contains(unknownWarnings[0], "newer build"),
          "unknown: an unrecognised blend still produces exactly one warning, naming the "
          "layer, the value, and where it most likely came from");
    check(contains(unknownWarnings[0], "multiply") && contains(unknownWarnings[0], "screen"),
          "unknown: and the sentence lists the modes that ARE implemented, generated from "
          "core/Blend's table rather than typed into the message");
    check(unknown.layers[1].blend == "dissolve",
          "unknown: the value itself is untouched, so PRD I10 still carries it to disk "
          "verbatim -- which is the whole reason Layer::blend stayed a std::string");

    Document asNormal = oneLayerDoc(kDefaultBlendName);
    const DecodedImage normalFlat = flattenDocumentToLinear(asNormal);
    check(unknownFlat.pixels.size() == normalFlat.pixels.size() &&
              std::memcmp(unknownFlat.pixels.data(), normalFlat.pixels.data(),
                          unknownFlat.pixels.size() * sizeof(float)) == 0,
          "unknown: and the composite is byte-identical to `over`, so the warning describes "
          "what actually happened");

    Document mixed = oneLayerDoc("mix");
    std::vector<std::string> mixWarnings;
    const DecodedImage mixFlat = flattenDocumentToLinear(mixed, &mixWarnings);
    // **Reworded at PLAN.md Phase 5 step 3.** The old sentence said `mix` had a
    // "named unblocking condition" (latent tiles). It has them now, so a `mix`
    // here is no longer "this build is behind PLAN.md" -- it is a document
    // asking for a mix where PRD L5 does not define one, on an RGB layer. The
    // fact this assertion pins is unchanged: `mix` warns with its OWN
    // sentence, distinct from an unknown name's, and the sentence names why.
    check(mixWarnings.size() == 1 && contains(mixWarnings[0], "\"mix\"") &&
              contains(mixWarnings[0], "L5") && contains(mixWarnings[0], "RGB layer") &&
              !contains(mixWarnings[0], "newer build"),
          "mix: a `mix` blend on an RGB layer warns with its OWN sentence, naming PRD L5 and "
          "this layer's kind -- a different fact from an unknown name");
    check(std::memcmp(mixFlat.pixels.data(), normalFlat.pixels.data(),
                      mixFlat.pixels.size() * sizeof(float)) == 0,
          "mix: and it too is composited as `over`, never refused -- refusing would make a "
          "preserved np:blend the thing that stops the document being saved");

    // The export boundary carries both out, on success and on refusal alike.
    const ExportResult png = exportDocument(mixed, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Srgb, ExportBitDepth::UInt8);
    check(png.ok && png.warnings.size() == 1,
          "mix: exportDocument() carries the warning out with the bytes");
  }

  // --- PRD L5: `Mix` only between two Pigment layers ---------------------
  {
    auto layerOfKind = [](LayerKind kind) {
      Layer l;
      l.kind = kind;
      if (kind == LayerKind::RGB) l.rgbTiles.emplace();
      return l;
    };
    auto menuHas = [](const std::vector<BlendMode>& menu, BlendMode m) {
      return std::find(menu.begin(), menu.end(), m) != menu.end();
    };

    Document rgbOnly = Document::createBlank(4, 4, WorkingSpace{});
    rgbOnly.layers.push_back(layerOfKind(LayerKind::RGB));
    check(blendMenuForLayer(rgbOnly, 1).size() == 26 &&
              !menuHas(blendMenuForLayer(rgbOnly, 1), BlendMode::Mix),
          "L5: an RGB layer over an RGB layer is offered every non-`Mix` mode (twenty-six: the "
          "six linear ones plus Stage 1's, Stage 2's and Stage 3's seven/seven/six new modes) "
          "and NOT `Mix`");

    Document pigments = Document::createBlank(4, 4, WorkingSpace{});
    pigments.layers.clear();
    pigments.layers.push_back(layerOfKind(LayerKind::Pigment));
    pigments.layers.push_back(layerOfKind(LayerKind::Pigment));
    check(menuHas(blendMenuForLayer(pigments, 1), BlendMode::Mix) &&
              blendMenuForLayer(pigments, 1).size() == 27,
          "L5: a Pigment layer sitting on another Pigment layer IS offered `Mix` -- twenty-seven "
          "entries total, every mode");
    check(!menuHas(blendMenuForLayer(pigments, 0), BlendMode::Mix),
          "L5: but the BOTTOM Pigment layer is not -- there is nothing beneath it to mix "
          "with, and `layers` is bottom-to-top so `beneath` is index - 1");

    Document mixedKinds = Document::createBlank(4, 4, WorkingSpace{});  // layers[0] is RGB
    mixedKinds.layers.push_back(layerOfKind(LayerKind::Pigment));
    check(!menuHas(blendMenuForLayer(mixedKinds, 1), BlendMode::Mix),
          "L5: a Pigment layer over an RGB layer is not offered `Mix` -- BOTH layers must be "
          "Pigment (docs/ui.md 3.4), because there is no latent under it to mix into");
    check(blendMenuForLayer(mixedKinds, 9).empty(),
          "L5: an out-of-range row offers nothing at all, rather than the whole set");

    // The model refuses what the menu does not offer, through the same
    // predicate -- so L5 is not merely a thing the UI declines to draw.
    const LayerOpResult refusedMix = setLayerBlend(mixedKinds, 1, BlendMode::Mix);
    check(!refusedMix.ok && contains(refusedMix.error, "L5") &&
              contains(refusedMix.error, "Pigment") &&
              mixedKinds.layers[1].blend == kDefaultBlendName,
          "L5: setLayerBlend() refuses `Mix` there by name, citing the requirement, and the "
          "layer really is unchanged -- the dropdown is not the only thing enforcing it");
    const LayerOpResult allowedMix = setLayerBlend(pigments, 1, BlendMode::Mix);
    check(allowedMix.ok && pigments.layers[1].blend == "mix" &&
              contains(allowedMix.editLabel, "mix"),
          "L5: and allows it where the pair holds, writing the canonical name and reporting "
          "an edit label naming the mode");

    // A reorder can take a layer out of L5's reach while it still carries the
    // value. The menu stops offering it and the selection reports "not in this
    // menu" rather than silently reading as the first entry.
    check(moveLayer(pigments, 1, 0).ok, "L5: the `mix` layer is moved to the bottom");
    const std::vector<BlendMode> afterMove = blendMenuForLayer(pigments, 0);
    check(!menuHas(afterMove, BlendMode::Mix) && pigments.layers[0].blend == "mix",
          "L5: after the move the menu no longer offers `Mix` while the layer still CARRIES "
          "it -- the value is preserved, not coerced");
    check(blendMenuSelection(pigments, 0, afterMove) == afterMove.size(),
          "L5: and the dropdown reports `not in this menu` rather than defaulting to entry "
          "0, which would be a silent lie about what the layer says");
  }

  // --- core/LayerOps' setter ---------------------------------------------
  {
    Document doc = Document::createBlank(4, 4, WorkingSpace{});
    const LayerOpResult set = setLayerBlend(doc, 0, BlendMode::Screen);
    check(set.ok && doc.layers[0].blend == "screen" && set.index == 0 &&
              contains(set.editLabel, "screen"),
          "setter: setLayerBlend() writes the canonical np:blend name and reports the edit "
          "label a caller should record");
    check(blendMenuSelection(doc, 0, blendMenuForLayer(doc, 0)) == 3,
          "setter: and the dropdown then selects `screen`, which is entry 3 of the menu");

    doc.layers[0].locked = true;
    const LayerOpResult refused = setLayerBlend(doc, 0, BlendMode::Multiply);
    check(!refused.ok && contains(refused.error, "locked") && doc.layers[0].blend == "screen",
          "setter: a LOCKED layer refuses it by name and really is unchanged -- which blend "
          "a layer uses is part of how it looks, so a lock that froze content but not "
          "blending would be a lock in name only");
    const LayerOpResult oob = setLayerBlend(doc, 7, BlendMode::Multiply);
    check(!oob.ok && contains(oob.error, "index"),
          "setter: an out-of-range index is refused with the same sentence every other "
          "core/LayerOps operation uses");
  }

  // --- `Mix` itself: the KM latent lerp, against real Mixbox data --------
  //
  // PRD C3 is P0 and this is the half of it that can land honestly today.
  // `core::Layer` has no latent storage -- `rgbTiles` is 4-channel rgba16float
  // and Pigment/Media need a 7-channel tile that PLAN.md Phase 5 step 3 owns
  // -- so there are no layer latents to lerp. Synthesising them from RGB was
  // rejected: `rgbToLatent()`'s decomposition is "plausible rather than true"
  // (docs/ui.md 3.3) and a `Mix` built on it would be confident, wrong colour
  // that looked like the feature working.
  //
  // What is real today is the latent representation itself, so `mixLatents()`
  // is tested against it: the actual Mixbox LUT, the actual palette pigments,
  // and PLAN.md's own Phase 5 verify sentence -- "blue ... over yellow gives
  // green under `Mix`".
  {
    MixboxLut lut;
    const bool lutLoaded = pigmentSourceReady(lut, mixboxLutPath().c_str());
    check(lutLoaded,
          "mix: this build's pigment source is live and answering with real data -- this "
          "section asserts against measured pigment "
          "data, not against a stand-in");

    // Endpoints and the implied fourth weight, which need no LUT and no
    // tolerance at all.
    const Latent a{{0.25f, 0.5f, 0.125f}, {0.0625f, -0.125f, 0.375f}};
    const Latent b{{0.75f, 0.125f, 0.0f}, {-0.5f, 0.25f, 0.125f}};
    const Latent at0 = mixLatents(a, b, 0.0f);
    const Latent at1 = mixLatents(a, b, 1.0f);
    check(at0.c == a.c && at0.res == a.res && at1.c == b.c && at1.res == b.res,
          "mix: t = 0 returns the first latent and t = 1 the second, EXACTLY -- std::lerp's "
          "endpoint guarantee, so a Pigment layer at 0% or 100% cannot drift");

    const Latent half = mixLatents(a, b, 0.5f);
    const float impliedA = 1.0f - (a.c[0] + a.c[1] + a.c[2]);
    const float impliedB = 1.0f - (b.c[0] + b.c[1] + b.c[2]);
    const float impliedHalf = 1.0f - (half.c[0] + half.c[1] + half.c[2]);
    check(impliedHalf == 0.5f * (impliedA + impliedB),
          "mix: the IMPLIED fourth pigment weight lerps by the same t -- so lerping this "
          "codebase's six floats is exactly Mixbox's seven, not an approximation of them");

    if (lutLoaded) {
      // Cadmium Yellow and Cobalt Blue, straight from the shipped palette:
      // the two pigments runSelfTest()'s own "blue crossing yellow gives
      // green" case uses, so this section and that one cannot disagree about
      // what the model says.
      const Pigment& yellow = defaultPalette()[0];
      const Pigment& blue = defaultPalette()[7];
      const Latent zy = lut.rgbToLatent(yellow.rgb[0], yellow.rgb[1], yellow.rgb[2]);
      const Latent zb = lut.rgbToLatent(blue.rgb[0], blue.rgb[1], blue.rgb[2]);
      const std::array<float, 3> km = latentToRgb(mixLatents(zy, zb, 0.5f));
      const std::array<float, 3> naive = {0.5f * (yellow.rgb[0] + blue.rgb[0]),
                                          0.5f * (yellow.rgb[1] + blue.rgb[1]),
                                          0.5f * (yellow.rgb[2] + blue.rgb[2])};
      std::printf("  [measured] mix(yellow, blue, 0.5): KM (%.3f, %.3f, %.3f) vs. naive RGB "
                  "lerp (%.3f, %.3f, %.3f)\n",
                  static_cast<double>(km[0]), static_cast<double>(km[1]),
                  static_cast<double>(km[2]), static_cast<double>(naive[0]),
                  static_cast<double>(naive[1]), static_cast<double>(naive[2]));
      check(km[1] > km[0] && km[1] > km[2],
            "mix: yellow mixed with blue gives GREEN -- green is the largest channel, which "
            "is PLAN.md's own Phase 5 verify sentence and the reason the Mixbox licence is "
            "being accepted");
      // The separation threshold, from the measurement printed above rather
      // than picked: the naive lerp's red is 0.498 (it cannot be otherwise --
      // it is the arithmetic mean of 0.996 and 0.0), and the KM mix's is
      // 0.189. Half the naive value, 0.249, sits between them with 32%
      // headroom on the measured figure, and is the natural statement of the
      // qualitative claim ("KM keeps the mix saturated where the RGB lerp
      // washes it out") rather than a number tuned to the result. This is a
      // separation, not a tolerance: it must not tighten onto 0.189.
      check(km[0] < naive[0] * 0.5f,
            "mix: and it is not the RGB lerp: the KM mix's red is under half the naive "
            "lerp's, which is the desaturated-grey failure the whole model exists to avoid");

      // The endpoints of that mix, projected back. This is what makes the
      // *middle* of it meaningful: a mix between two colours the model cannot
      // reproduce would be a mix of something else.
      //
      // It is exact for a structural reason worth stating, because the obvious
      // derivation (8-bit LUT quantisation, amplified by evalPolynomial()'s
      // largest coefficient) would bound the wrong thing entirely and land
      // three orders of magnitude too loose. `rgbToLatent()` *defines* the
      // residual as `rgb - evalPolynomial(c)`, and `latentToRgb()` returns
      // `evalPolynomial(c) + res` from the same `c` -- so every bit of LUT
      // error, quantisation included, is absorbed into the residual by
      // construction and the round trip is `p + (r - p)`.
      //
      // Tolerance, derived from that expression rather than from the LUT: two
      // correctly-rounded float operations on values of magnitude <= 1, so at
      // most 2 ulps of 1.0 = 2^-23 = 1.19e-7. Bounded at 5.0e-7, 4.2x the
      // derived bound. The measurement is printed, and it is 0.
      //
      // **Under NP_USE_MIXBOX=OFF this derivation does not apply**, and
      // pretending it still does would be exactly the "assertion asserts the
      // wrong thing in one configuration" this task's brief warns about. The
      // KM2 fallback (paint/Palette.cpp) is not a fit against a target with a
      // residual that absorbs the gap -- it is a closed-form K/S derivation
      // that *clamps* a channel at or near 0 or 1 to `kKm2ReflectanceFloor`
      // before taking the ratio, and Cadmium Yellow's blue channel and Cobalt
      // Blue's red channel (the two pigments this section uses) are both
      // exact 0.0 in `defaultPalette()`. The round trip through that clamp is
      // still exact **for the clamped value** (Kubelka's K/S ratio is
      // independent of the scattering S assumed for it, so nothing here is
      // approximate) but the clamped value is not the original one, by
      // exactly the floor's distance. That is a real, bounded, derived
      // number, not a loosened guess.
#if defined(NP_USE_MIXBOX)
      constexpr float kTol = 5.0e-7f;
#else
      constexpr float kTol = MixboxLut::kKm2ReflectanceFloor + 1.0e-6f;
#endif
      const std::array<float, 3> backY = latentToRgb(zy);
      const std::array<float, 3> backB = latentToRgb(zb);
      float worst = 0.0f;
      for (int i = 0; i < 3; ++i) {
        worst = std::fmax(worst, std::fabs(backY[i] - yellow.rgb[i]));
        worst = std::fmax(worst, std::fabs(backB[i] - blue.rgb[i]));
      }
      std::printf("  [measured] rgb -> latent -> rgb on the two palette pigments: max residual "
                  "= %.3e (bound %.3e)\n",
                  static_cast<double>(worst), static_cast<double>(kTol));
      check(worst <= kTol,
            "mix: latent -> rgb reproduces the pigments it came from -- the residual channel "
            "absorbs the LUT's error by construction, so `Mix` at t = 0 and t = 1 gives back "
            "exactly the pigments being mixed");
    }
  }

  std::printf("[selftest] blend modes %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
