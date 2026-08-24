#include "app/selftest/Support.hpp"

#include <cmath>

#include "ops/Gradient.hpp"

namespace np {

// ops/Gradient (PLAN.md "Phase 6 -- Filter and transform it"; PRD D24, and the
// gradient half of PRD D26). Pure CPU, no PaintSim and no GPU -- the same
// headless-first-class status runPointOpsTest(), runSelectionTest() and
// runResampleTest() have.
//
// Five things carry this section, and each is a decision that is invisible
// until it is wrong:
//
//   1. **The three geometries put their stops where the geometry says.** A
//      linear ramp is constant perpendicular to its drag, a radial one is
//      isotropic, and an angular one sweeps CLOCKWISE on screen because
//      document space is y-down. That last is asserted by quadrant rather than
//      described, because "which way does the angle go" is settled by a sign
//      nobody reviews.
//
//   2. **Coverage weights, it does not gate.** A gradient rendered through a
//      partially-covered texel must come out proportionally weaker, not
//      in-or-out. The failure mode is a stair-stepped edge on an antialiased
//      marquee, which looks like a bad selection rather than a bad gradient.
//
//   3. **A null Selection means EVERYWHERE.** core/SelectionMask.hpp asks for
//      exactly this: a loop that hoists the selection tile "owns the null-
//      Selection branch itself", and "--selftest must assert the null case
//      through that loop too". renderGradient() is such a loop, so the null
//      case is asserted through renderGradient() and not only through
//      selectionCoverageAt(). The engaged-but-empty selection is asserted
//      beside it, because those two are the pair that get confused.
//
//   4. **Colour stops and opacity stops are independently positioned.** The
//      assertion that catches a merged RGBA-stop model is that an opacity stop
//      at 0.25 does NOT create a colour stop at 0.25 -- the colour ramp must
//      still be the plain two-stop lerp there.
//
//   5. **Interpolation is on STRAIGHT colour in LINEAR light.** Both halves
//      are checked against the specific wrong answer they replace: linear
//      0.5 at the ramp's midpoint rather than sRGB's 0.214041, and a colour
//      ramp that survives a fade to transparent rather than collapsing onto
//      the opaque end.
bool runGradientTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // --- Tolerances, both derived rather than chosen -----------------------
  //
  // A value in [0, 1] stored in a core::Tile is rounded to binary16. The
  // coarsest binade such a value can land in is [0.5, 1), whose ulp is 2^-11,
  // so round-to-nearest bounds the absolute error at half that: 2^-12.
  //
  // Confirmed by measurement rather than assumed: a black-to-white ramp
  // rendered 1000 texels wide (deliberately not a power of two -- at width
  // 256 every (x+0.5)/256 is exactly representable in f16 and the measured
  // error is a misleading 0.0) has a worst stored-vs-analytic error of
  // 2.42188e-4, which sits just inside this bound.
  constexpr float kF16Tol = 2.44140625e-4f;  // 2^-12

  // Un-premultiplying amplifies both stored errors by 1/alpha:
  //   |rgb'/a' - c| ~= (|e_rgb| + c*|e_a|) / a  <=  2*kF16Tol / a   for c <= 1.
  // The assertions below only un-premultiply where alpha >= 0.25, so the bound
  // is 8*kF16Tol = 1.953e-3. The measured worst case over the actual fade is
  // printed beside it so the margin is visible rather than trusted.
  constexpr float kUnpremulTol = kF16Tol * 8.0f;

  // A black-to-white two-stop ramp, reused throughout. Straight linear RGB.
  GradientStops bw;
  bw.colorStops.push_back(ColorStop{0.0f, {0.0f, 0.0f, 0.0f}, 0.5f});
  bw.colorStops.push_back(ColorStop{1.0f, {1.0f, 1.0f, 1.0f}, 0.5f});

  // Reads one document texel out of a store; {0,0,0,0} where no tile exists,
  // which is the correct implicit content of an untouched tile.
  auto readDoc = [](const TileStore& tiles, int32_t x, int32_t y) -> std::array<float, 4> {
    const PixelCoord doc{x, y};
    const Tile* tile = tiles.find(tileCoordAt(doc));
    if (tile == nullptr) return {0.0f, 0.0f, 0.0f, 0.0f};
    return tile->readPixel(tileLocalOffset(doc));
  };

  // --- 1. Linear: the stops land where the drag says ----------------------
  {
    // Handles on texel BOUNDARIES, 0 -> 100. The op samples texel centres, so
    // texel x reads (x + 0.5)/100 and the ramp is symmetric about x = 49.5.
    GradientGeometry g;
    g.kind = GradientKind::Linear;
    g.x0 = 0.0f;
    g.y0 = 0.0f;
    g.x1 = 100.0f;
    g.y1 = 0.0f;

    TileStore tiles;
    const size_t written = renderGradient(tiles, GradientRegion{0, 0, 100, 8}, g, bw, nullptr);
    check(written == 100u * 8u,
          "gradient linear: an opaque fill writes every texel of the region -- 100x8, so a "
          "region that silently clipped itself to a tile would be caught here");

    const std::array<float, 4> first = readDoc(tiles, 0, 0);
    const std::array<float, 4> mid = readDoc(tiles, 49, 0);
    const std::array<float, 4> last = readDoc(tiles, 99, 0);
    check(near(first[0], 0.005f, kF16Tol) && near(mid[0], 0.495f, kF16Tol) &&
              near(last[0], 0.995f, kF16Tol),
          "gradient linear: texel x reads (x+0.5)/100 -- the op samples texel CENTRES, so a "
          "corner-sampled ramp would be half a texel off at both ends");
    check(first[0] != 0.0f && last[0] != 1.0f,
          "gradient linear: and therefore neither end texel is exactly the end stop -- a "
          "gradient across N texels never reaches t=0 or t=1, which is correct, not a bug");

    // The defining property of a linear gradient, and the one a projection
    // written with the wrong normaliser breaks.
    check(readDoc(tiles, 37, 0) == readDoc(tiles, 37, 7),
          "gradient linear: the ramp is bit-identical along every line PERPENDICULAR to the "
          "drag -- a linear gradient that varies across its own isolines is not linear");

    // Monotone, strictly, everywhere the f16 grid can express it. A ramp that
    // reverses anywhere means the projection lost a sign.
    bool monotone = true;
    for (int32_t x = 1; x < 100; ++x) {
      if (readDoc(tiles, x, 0)[0] < readDoc(tiles, x - 1, 0)[0]) monotone = false;
    }
    check(monotone,
          "gradient linear: the ramp is non-decreasing across its whole span -- a sign lost "
          "in the projection reverses part of the gradient, not all of it");

    // A diagonal drag, checked against the projection by hand: the point
    // (10.5, 10.5) projects onto the drag (0,0)->(20,0)... deliberately NOT
    // axis-aligned, so the dot product is exercised in both components.
    GradientGeometry diag;
    diag.kind = GradientKind::Linear;
    diag.x0 = 0.0f;
    diag.y0 = 0.0f;
    diag.x1 = 10.0f;
    diag.y1 = 10.0f;
    // v.d / |d|^2 with v = (10, 0), d = (10, 10): 100 / 200 = 0.5.
    check(near(gradientParameterAt(diag, 10.0f, 0.0f), 0.5f, 1e-6f) &&
              near(gradientParameterAt(diag, 0.0f, 10.0f), 0.5f, 1e-6f),
          "gradient linear: a diagonal drag projects both components -- two points either "
          "side of the drag axis share a t, which an x-only projection gets wrong");
  }

  // --- 2. Radial: isotropic about its centre ------------------------------
  {
    // Centre on a texel CENTRE (64.5, 64.5) so the probes below land on exact
    // radii and the assertion is about the geometry, not about rounding.
    GradientGeometry g;
    g.kind = GradientKind::Radial;
    g.x0 = 64.5f;
    g.y0 = 64.5f;
    g.x1 = 96.5f;  // radius 32
    g.y1 = 64.5f;

    TileStore tiles;
    renderGradient(tiles, GradientRegion{32, 32, 98, 98}, g, bw, nullptr);

    check(near(readDoc(tiles, 64, 64)[0], 0.0f, kF16Tol) &&
              near(readDoc(tiles, 80, 64)[0], 0.5f, kF16Tol) &&
              near(readDoc(tiles, 96, 64)[0], 1.0f, kF16Tol),
          "gradient radial: t is distance/radius -- the centre is the first stop, the rim "
          "texel is the last, and half way out is exactly half way along the ramp");

    // Isotropy: four texels at the same distance in four directions must be
    // bit-identical. This is what fails when a radial is implemented as a
    // per-axis maximum or as a squared distance that forgot its square root.
    const std::array<float, 4> right = readDoc(tiles, 80, 64);
    check(right == readDoc(tiles, 48, 64) && right == readDoc(tiles, 64, 80) &&
              right == readDoc(tiles, 64, 48),
          "gradient radial: four texels at equal distance in four directions are BIT-"
          "identical -- a radial that is not circular is a diamond or a square");

    // Past the rim, Pad clamps: the corner of the region is well outside r=32
    // and must be exactly the last stop, not an extrapolation.
    check(readDoc(tiles, 33, 33)[0] == 1.0f,
          "gradient radial: outside the rim, Pad holds the LAST stop exactly -- an "
          "unclamped t would run the ramp off its own end");
  }

  // --- 3. Angular: one sweep, clockwise on a y-down screen ----------------
  {
    GradientGeometry g;
    g.kind = GradientKind::Angular;
    g.x0 = 0.0f;
    g.y0 = 0.0f;
    g.x1 = 1.0f;  // zero-angle direction is +x
    g.y1 = 0.0f;

    // The four quadrant directions, by hand. Document y points DOWN, so +y is
    // a quarter turn clockwise on screen and must be t = 0.25.
    check(near(gradientParameterAt(g, 10.0f, 0.0f), 0.0f, 1e-6f) &&
              near(gradientParameterAt(g, 0.0f, 10.0f), 0.25f, 1e-6f) &&
              near(gradientParameterAt(g, -10.0f, 0.0f), 0.5f, 1e-6f) &&
              near(gradientParameterAt(g, 0.0f, -10.0f), 0.75f, 1e-6f),
          "gradient angular: +x=0, +y=0.25, -x=0.5, -y=0.75 -- the sweep is CLOCKWISE on "
          "screen because document y points down, and the sign that decides it is unread");

    // Rotating the handle rotates the ramp by the same amount, which is the
    // property that fails if the reference direction is ignored.
    GradientGeometry rotated = g;
    rotated.x1 = 0.0f;
    rotated.y1 = 1.0f;  // zero-angle direction is now +y
    check(near(gradientParameterAt(rotated, 0.0f, 10.0f), 0.0f, 1e-6f) &&
              near(gradientParameterAt(rotated, -10.0f, 0.0f), 0.25f, 1e-6f),
          "gradient angular: the handle direction IS the zero angle -- a sweep that always "
          "starts at +x ignores the drag the user made");

    // Radius must not matter at all: the angular gradient is scale-free.
    check(gradientParameterAt(g, 3.0f, 3.0f) == gradientParameterAt(g, 3000.0f, 3000.0f),
          "gradient angular: t depends on angle ALONE -- distance from the centre is not "
          "part of an angular gradient and must not leak in");

    // Wrapped into [0,1) with no value ever landing outside it.
    bool inRange = true;
    for (int i = 0; i < 360; ++i) {
      const float a = static_cast<float>(i) * 0.017453292f;
      const float t = gradientParameterAt(g, std::cos(a) * 50.0f, std::sin(a) * 50.0f);
      if (!(t >= 0.0f && t < 1.0f)) inRange = false;
    }
    check(inRange,
          "gradient angular: every direction wraps into [0,1) -- atan2's branch cut at -pi "
          "is where an unwrapped sweep produces a negative t and a black seam");
  }

  // --- 4. Selections: weighted, not gated; and null means EVERYWHERE ------
  {
    // A flat opaque white gradient, so every stored value IS the coverage and
    // nothing about the ramp can be blamed for a difference.
    GradientStops white;
    white.colorStops.push_back(ColorStop{0.0f, {1.0f, 1.0f, 1.0f}, 0.5f});
    GradientGeometry g;
    g.kind = GradientKind::Linear;
    g.x0 = 0.0f;
    g.y0 = 0.0f;
    g.x1 = 100.0f;
    g.y1 = 0.0f;
    const GradientRegion region{10, 20, 12, 21};

    // The reference: no selection at all.
    TileStore unrestricted;
    const size_t nAll = renderGradient(unrestricted, region, g, white, nullptr);
    check(nAll == 2u && readDoc(unrestricted, 10, 20)[3] == 1.0f &&
              readDoc(unrestricted, 11, 20)[3] == 1.0f,
          "gradient selection: a NULL selection means NO RESTRICTION -- the gradient fills "
          "the whole region at full strength, which is the opposite of 'nothing selected'");

    // The same call through a rectangle whose left edge cuts texel 10 at 0.75.
    // core/SelectionMask quantises that to 191/255 = 0.749020.
    const Selection partial = selectRectangle(10.25f, 20.0f, 12.0f, 21.0f);
    TileStore weighted;
    const size_t nSel = renderGradient(weighted, region, g, white, &partial);
    const std::array<float, 4> edge = readDoc(weighted, 10, 20);
    const std::array<float, 4> full = readDoc(weighted, 11, 20);
    std::printf("  [selftest] gradient: 0.75-covered texel stores alpha %.6f (uint8 grid "
                "gives 191/255 = %.6f)\n",
                static_cast<double>(edge[3]), 191.0 / 255.0);
    check(nSel == 2u && near(edge[3], 191.0f / 255.0f, kF16Tol),
          "gradient selection: a PARTIALLY covered texel is weighted by its coverage -- not "
          "rounded to in-or-out, which is what turns an antialiased marquee into stair steps");
    check(edge[3] > 0.0f && edge[3] < 1.0f && edge[3] < full[3],
          "gradient selection: and it lands strictly between 'absent' and 'full', beside a "
          "fully covered neighbour that is untouched -- the two answers a gate cannot give");
    check(edge[0] == edge[3] && edge[1] == edge[3] && edge[2] == edge[3],
          "gradient selection: coverage scales all FOUR premultiplied channels, not alpha "
          "alone -- rgb > alpha is a malformed premultiplied texel and the fringe in the "
          "other direction");
    check(full[3] == 1.0f && full[0] == 1.0f,
          "gradient selection: a fully covered texel is weighted by exactly 1.0 -- 255/255 "
          "is an identity, so a selected texel is not quietly darkened by its own selection");

    // Engaged but empty: NOT the same as null. This is the pair core/
    // SelectionMask.hpp says gets confused, so both are asserted in one place.
    Selection nothing;
    TileStore blocked;
    const size_t nNone = renderGradient(blocked, region, g, white, &nothing);
    check(nNone == 0u && blocked.occupiedTileCount() == 0u,
          "gradient selection: an ENGAGED selection with no tiles renders NOTHING and "
          "allocates nothing -- the exact inverse of the null case above, from the same "
          "argument slot");

    // Sparsity: a small marquee on a document-sized region must cost the
    // marquee's tiles, not the region's.
    const Selection small = selectRectangle(4.0f, 4.0f, 8.0f, 8.0f);
    TileStore sparse;
    renderGradient(sparse, GradientRegion{0, 0, 1024, 1024}, g, white, &small);
    check(sparse.occupiedTileCount() == 1u,
          "gradient selection: a 4x4 marquee over a 1024x1024 region allocates ONE tile -- "
          "an unselected tile is skipped BEFORE a 128 KiB destination is created for it");
  }

  // --- 5. Two stop lists, independently positioned ------------------------
  {
    GradientStops s;
    s.colorStops.push_back(ColorStop{0.0f, {1.0f, 0.0f, 0.0f}, 0.5f});
    s.colorStops.push_back(ColorStop{1.0f, {0.0f, 0.0f, 1.0f}, 0.5f});
    // Opacity stops at positions NO colour stop occupies.
    s.opacityStops.push_back(OpacityStop{0.25f, 0.0f, 0.5f});
    s.opacityStops.push_back(OpacityStop{0.75f, 1.0f, 0.5f});

    check(gradientOpacityAt(s, 0.25f) == 0.0f && gradientOpacityAt(s, 0.75f) == 1.0f &&
              near(gradientOpacityAt(s, 0.5f), 0.5f, 1e-6f),
          "gradient stops: the opacity ramp runs between ITS OWN positions -- 0.25 and 0.75 "
          "here, which no colour stop occupies");
    check(gradientOpacityAt(s, 0.0f) == 0.0f && gradientOpacityAt(s, 1.0f) == 1.0f,
          "gradient stops: and extrapolates FLAT outside them, so the ramp is defined "
          "everywhere without inventing stops at 0 and 1");

    // The assertion that catches a merged RGBA-stop model. If opacity stops
    // implied colour stops, the colour ramp would have knots at 0.25/0.75 and
    // these two values would not be the plain lerp.
    check(near(gradientColorAt(s, 0.25f)[0], 0.75f, 1e-6f) &&
              near(gradientColorAt(s, 0.25f)[2], 0.25f, 1e-6f) &&
              near(gradientColorAt(s, 0.75f)[0], 0.25f, 1e-6f) &&
              near(gradientColorAt(s, 0.75f)[2], 0.75f, 1e-6f),
          "gradient stops: an opacity stop does NOT create a colour stop -- the colour ramp "
          "is still the plain two-stop lerp at 0.25 and 0.75, which a merged RGBA-stop "
          "model cannot be");

    // Moving one list must not move the other.
    GradientStops moved = s;
    moved.opacityStops[0].position = 0.4f;
    check(gradientColorAt(moved, 0.25f) == gradientColorAt(s, 0.25f),
          "gradient stops: dragging an opacity stop leaves the colour ramp BIT-identical -- "
          "the independence is the whole reason there are two lists");

    // The empty-list asymmetry, which is a default-direction trap of the same
    // family as core/SelectionMask.hpp's.
    GradientStops noOpacity;
    noOpacity.colorStops.push_back(ColorStop{0.0f, {0.3f, 0.4f, 0.5f}, 0.5f});
    check(gradientOpacityAt(noOpacity, 0.0f) == 1.0f &&
              gradientOpacityAt(noOpacity, 0.73f) == 1.0f,
          "gradient stops: NO opacity stops means fully OPAQUE, not fully transparent -- "
          "every two-colour gradient anyone ever dragged has an empty opacity list");

    GradientGeometry g;
    g.kind = GradientKind::Linear;
    g.x0 = 0.0f;
    g.y0 = 0.0f;
    g.x1 = 8.0f;
    g.y1 = 0.0f;
    TileStore t;
    check(renderGradient(t, GradientRegion{0, 0, 8, 1}, g, noOpacity, nullptr) == 8u &&
              readDoc(t, 3, 0)[3] == 1.0f,
          "gradient stops: and that opacity reaches the tile -- an unauthored opacity ramp "
          "renders a visible gradient, not an invisible one");

    GradientStops noColor;
    noColor.opacityStops.push_back(OpacityStop{0.0f, 1.0f, 0.5f});
    TileStore t2;
    check(renderGradient(t2, GradientRegion{0, 0, 8, 1}, g, noColor, nullptr) == 0u &&
              t2.occupiedTileCount() == 0u,
          "gradient stops: while NO colour stops renders nothing at all -- the two empty "
          "lists mean opposite things and that is deliberate");
  }

  // --- 6. Straight colour, premultiplied once, at the write ---------------
  {
    GradientGeometry g;
    g.kind = GradientKind::Linear;
    g.x0 = 0.0f;
    g.y0 = 0.0f;
    g.x1 = 256.0f;
    g.y1 = 0.0f;

    // A single non-grey colour stop under a full fade to transparent. The hue
    // must be untouched by the fade all the way down -- this is the dark
    // fringe, and the separated stop model is what makes it unauthorable.
    GradientStops fade;
    fade.colorStops.push_back(ColorStop{0.0f, {0.8f, 0.2f, 0.05f}, 0.5f});
    fade.opacityStops.push_back(OpacityStop{0.0f, 1.0f, 0.5f});
    fade.opacityStops.push_back(OpacityStop{1.0f, 0.0f, 0.5f});

    TileStore tiles;
    renderGradient(tiles, GradientRegion{0, 0, 256, 1}, g, fade, nullptr);

    float worst = 0.0f;
    bool hueHeld = true;
    for (int32_t x = 0; x < 256; ++x) {
      const std::array<float, 4> px = readDoc(tiles, x, 0);
      if (px[3] < 0.25f) continue;  // below the band kUnpremulTol was derived for
      const float r = px[0] / px[3];
      const float gg = px[1] / px[3];
      const float b = px[2] / px[3];
      worst = std::max({worst, std::fabs(r - 0.8f), std::fabs(gg - 0.2f), std::fabs(b - 0.05f)});
      if (!near(r, 0.8f, kUnpremulTol) || !near(gg, 0.2f, kUnpremulTol) ||
          !near(b, 0.05f, kUnpremulTol)) {
        hueHeld = false;
      }
    }
    std::printf("  [selftest] gradient: worst un-premultiplied hue drift over a full fade "
                "is %.7f (derived bound 8 * 2^-12 = %.7f)\n",
                static_cast<double>(worst), static_cast<double>(kUnpremulTol));
    check(hueHeld,
          "gradient premultiply: a fade to transparent leaves the un-premultiplied colour "
          "unchanged -- a colour that darkens as it fades is the dark fringe, and this "
          "stop model has no transparent-black stop to author it with");

    // Alpha must be monotone down the fade, and the premultiplied channels
    // must never exceed it -- the invariant that says the premultiply happened
    // once, after the ramps, and not twice or in the wrong order.
    bool wellFormed = true;
    for (int32_t x = 0; x < 256; ++x) {
      const std::array<float, 4> px = readDoc(tiles, x, 0);
      if (px[0] > px[3] + kF16Tol || px[1] > px[3] + kF16Tol || px[2] > px[3] + kF16Tol) {
        wellFormed = false;
      }
    }
    check(wellFormed,
          "gradient premultiply: no stored channel exceeds its own alpha -- rgb > a is a "
          "texel core/Blend's `over` reads as negative backdrop contribution");

    // The specific answer premultiplied interpolation would give, asserted
    // against. Opaque red fading to transparent blue: the blue stop must still
    // contribute its hue at the midpoint. Premultiplied interpolation returns
    // pure red there, a 100% error on the blue channel.
    GradientStops rb;
    rb.colorStops.push_back(ColorStop{0.0f, {1.0f, 0.0f, 0.0f}, 0.5f});
    rb.colorStops.push_back(ColorStop{1.0f, {0.0f, 0.0f, 1.0f}, 0.5f});
    rb.opacityStops.push_back(OpacityStop{0.0f, 1.0f, 0.5f});
    rb.opacityStops.push_back(OpacityStop{1.0f, 0.0f, 0.5f});
    const std::array<float, 4> half = gradientSampleStraight(rb, 0.5f);
    check(near(half[0], 0.5f, 1e-6f) && near(half[2], 0.5f, 1e-6f) && near(half[3], 0.5f, 1e-6f),
          "gradient premultiply: red->transparent-blue is half-strength MAGENTA at its "
          "midpoint -- interpolating premultiplied values returns pure red there, because a "
          "transparent stop's premultiplied form carries no hue at all");
  }

  // --- 7. Linear working space, and the number it costs -------------------
  {
    // The geometric midpoint of a black-to-white ramp is linear 0.5 exactly,
    // not sRGB's 0.214041. Asserted both ways round, because "which space did
    // this interpolate in" has exactly two candidate answers.
    const float mid = gradientColorAt(bw, 0.5f)[0];
    std::printf("  [selftest] gradient: black->white midpoint is linear %.6f, which "
                "DISPLAYS as sRGB %.6f (sRGB-domain interpolation would give %.6f)\n",
                static_cast<double>(mid), static_cast<double>(srgbEncode(mid)),
                static_cast<double>(srgbDecode(0.5f)));
    check(mid == 0.5f,
          "gradient space: interpolation is in LINEAR light -- the midpoint of black-to-"
          "white is linear 0.5 exactly, and every other op in this build works in the same "
          "space");
    check(!near(mid, srgbDecode(0.5f), 1e-3f),
          "gradient space: and is NOT the sRGB-domain answer 0.214041 -- the two are "
          "visibly different and only one of them survives a later blur or resample");

    // A stop's own colour survives at its own position bit-exactly. This is
    // why the lerp is written a + (b-a)*u rather than a*(1-u) + b*u.
    GradientStops three;
    three.colorStops.push_back(ColorStop{0.0f, {0.1f, 0.2f, 0.3f}, 0.5f});
    three.colorStops.push_back(ColorStop{0.375f, {0.7f, 0.6f, 0.5f}, 0.5f});
    three.colorStops.push_back(ColorStop{1.0f, {0.9f, 0.8f, 0.7f}, 0.5f});
    check(gradientColorAt(three, 0.375f) == three.colorStops[1].color,
          "gradient space: a stop's authored colour comes back BIT-exact at its own "
          "position -- a*(1-u)+b*u does not guarantee that and a+(b-a)*u does");

    // The midpoint skew, and the escape hatch it provides for the choice above.
    check(gradientColorAt(bw, 0.25f)[0] == 0.25f,
          "gradient midpoint: the default 0.5 is a straight lerp with no pow() in it at "
          "all -- the common case must not pay for the feature");

    GradientStops skewed;
    skewed.colorStops.push_back(ColorStop{0.0f, {0.0f, 0.0f, 0.0f}, 0.7322f});
    skewed.colorStops.push_back(ColorStop{1.0f, {1.0f, 1.0f, 1.0f}, 0.5f});
    const float perceptual = gradientColorAt(skewed, 0.5f)[0];
    std::printf("  [selftest] gradient: midpoint 0.7322 puts linear %.6f at t=0.5 "
                "(srgbDecode(0.5) = %.6f)\n",
                static_cast<double>(perceptual), static_cast<double>(srgbDecode(0.5f)));
    check(near(perceptual, srgbDecode(0.5f), 1e-4f),
          "gradient midpoint: 0.7322 recovers the perceptually-even ramp linear "
          "interpolation gives up -- the escape hatch is real, not a promise in a comment");
    check(near(gradientColorAt(skewed, 0.7322f)[0], 0.5f, 1e-5f),
          "gradient midpoint: and it is a POSITION, not a value -- the 50% blend lands AT "
          "the midpoint, which is the opposite of what the name invites");
  }

  // --- 8. Spread, and the degenerate drag ---------------------------------
  {
    GradientGeometry g;
    g.kind = GradientKind::Linear;
    g.x0 = 0.0f;
    g.y0 = 0.0f;
    g.x1 = 4.0f;
    g.y1 = 0.0f;

    g.spread = GradientSpread::Pad;
    check(gradientParameterAt(g, -100.0f, 0.0f) == 0.0f &&
              gradientParameterAt(g, 100.0f, 0.0f) == 1.0f,
          "gradient spread: Pad clamps to the end stops in both directions");

    g.spread = GradientSpread::Repeat;
    check(near(gradientParameterAt(g, 5.0f, 0.0f), 0.25f, 1e-6f) &&
              near(gradientParameterAt(g, -1.0f, 0.0f), 0.75f, 1e-6f),
          "gradient spread: Repeat wraps with a FLOOR, so a negative t comes back positive "
          "rather than mirroring the branch a truncating fmod would take");

    g.spread = GradientSpread::Reflect;
    check(near(gradientParameterAt(g, 5.0f, 0.0f), 0.75f, 1e-6f) &&
              near(gradientParameterAt(g, 9.0f, 0.0f), 0.25f, 1e-6f) &&
              near(gradientParameterAt(g, -1.0f, 0.0f), 0.25f, 1e-6f),
          "gradient spread: Reflect ping-pongs with period 2, so the ramp is continuous at "
          "every seam regardless of what the end stops are");

    // A zero-length drag is a single click, which every tool receives by
    // accident. It must be a flat fill, not a NaN.
    GradientGeometry click;
    click.kind = GradientKind::Linear;
    click.x0 = click.x1 = 12.0f;
    click.y0 = click.y1 = 34.0f;
    const float t = gradientParameterAt(click, 99.0f, 99.0f);
    check(t == 0.0f && !std::isnan(t),
          "gradient degenerate: a zero-length drag fills flat with the FIRST stop -- a "
          "single click must not poison a tile with NaN or raise an error dialog");
    TileStore tiles;
    check(renderGradient(tiles, GradientRegion{0, 0, 4, 4}, click, bw, nullptr) == 16u &&
              readDoc(tiles, 2, 2)[0] == 0.0f && readDoc(tiles, 2, 2)[3] == 1.0f,
          "gradient degenerate: and that flat fill actually lands -- opaque black here, not "
          "an empty store");
  }

  // --- 9. Source-over, not replace ----------------------------------------
  {
    GradientGeometry g;
    g.kind = GradientKind::Linear;
    g.x0 = 0.0f;
    g.y0 = 0.0f;
    g.x1 = 4.0f;
    g.y1 = 0.0f;

    // A half-opaque white gradient over an opaque red backdrop:
    //   over: 0.5 + 1.0*(1-0.5) = 1.0 alpha; red channel 0.5 + 1.0*0.5 = 1.0;
    //         green/blue 0.5 + 0*0.5 = 0.5.
    GradientStops halfWhite;
    halfWhite.colorStops.push_back(ColorStop{0.0f, {1.0f, 1.0f, 1.0f}, 0.5f});
    halfWhite.opacityStops.push_back(OpacityStop{0.0f, 0.5f, 0.5f});

    TileStore tiles;
    Tile& backdrop = tiles.getOrCreate(TileCoord{0, 0});
    for (int32_t x = 0; x < 4; ++x) backdrop.writePixel(PixelCoord{x, 0}, {1.0f, 0.0f, 0.0f, 1.0f});

    renderGradient(tiles, GradientRegion{0, 0, 4, 1}, g, halfWhite, nullptr);
    const std::array<float, 4> px = readDoc(tiles, 1, 0);
    check(near(px[0], 1.0f, kF16Tol) && near(px[1], 0.5f, kF16Tol) &&
              near(px[2], 0.5f, kF16Tol) && near(px[3], 1.0f, kF16Tol),
          "gradient composite: a translucent gradient goes OVER what is already there -- a "
          "replace would erase the backdrop, and opacity stops would then mean nothing");

    // And at full opacity `over` degenerates to an exact replace, which is
    // what PRD D26's "fill a layer" needs.
    GradientStops solid;
    solid.colorStops.push_back(ColorStop{0.0f, {0.25f, 0.5f, 0.75f}, 0.5f});
    renderGradient(tiles, GradientRegion{0, 0, 4, 1}, g, solid, nullptr);
    const std::array<float, 4> solidPx = readDoc(tiles, 1, 0);
    check(solidPx[0] == 0.25f && solidPx[1] == 0.5f && solidPx[2] == 0.75f &&
              solidPx[3] == 1.0f,
          "gradient composite: at full opacity `over` IS a replace, exactly -- so PRD D26's "
          "solid fill pays nothing for the general case");
  }

  // --- 10. Stop sorting is the editor's job, and it is stable -------------
  {
    GradientStops s;
    s.colorStops.push_back(ColorStop{0.8f, {1.0f, 0.0f, 0.0f}, 0.5f});
    s.colorStops.push_back(ColorStop{0.2f, {0.0f, 1.0f, 0.0f}, 0.5f});
    s.colorStops.push_back(ColorStop{0.2f, {0.0f, 0.0f, 1.0f}, 0.5f});
    s.opacityStops.push_back(OpacityStop{0.9f, 0.25f, 0.5f});
    s.opacityStops.push_back(OpacityStop{0.1f, 0.75f, 0.5f});
    sortGradientStops(s);
    check(s.colorStops[0].position == 0.2f && s.colorStops[2].position == 0.8f &&
              s.opacityStops[0].position == 0.1f,
          "gradient sort: both lists sort ascending -- the render path assumes it and does "
          "not re-check, exactly as ops/PointOps' Curve does");
    check(s.colorStops[0].color[1] == 1.0f && s.colorStops[1].color[2] == 1.0f,
          "gradient sort: and it is STABLE, so two stops dragged onto the same position "
          "keep their creation order and the hard edge between them does not flip");
  }

  return ok;
}

}  // namespace np
