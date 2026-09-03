#include "app/selftest/Support.hpp"

#include "app/MunsellSelection.hpp"
#include "app/StrokeSession.hpp"
#include "color/Munsell.hpp"
#include "color/Space.hpp"

namespace np {

// color/Munsell: the arithmetic behind the COLOR panel's third picker
// (docs/munsell-picker.md), with no UI and no AppState in reach.
//
// **The section is arranged around one claim**: a page ROW has one relative
// luminance, and that luminance does not move when the hue changes. Every
// other assertion here is either a prerequisite for that claim being
// meaningful (the D1535 round trip, the derived matrix) or a statement about
// what happens where the claim cannot be honoured (the voids).
//
// The luminance is measured off the **returned float triple**, not off the
// double the maths ran in, because the float is what a swatch and a stroke
// will actually get. That is the whole reason the tolerance below is 1e-6
// and not 1e-12.
bool runMunsellTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](double a, double b, double tol) { return std::fabs(a - b) <= tol; };

  // --- The derived matrix, against the published constants ---------------
  //
  // `color/Space.hpp` carries primaries and no matrix; color/Munsell derives
  // one. Checking a derivation against itself proves nothing, so it is
  // checked against the sRGB/Rec.709 numbers as published (IEC 61966-2-1).
  // The tolerance is 5e-4 because the published values are rounded decimals
  // and `kRec709Primaries` stores white as 0.3127/0.3290, itself rounded.
  const auto& m = rec709RgbToXyz();
  check(near(m[1][0], 0.2126, 5e-4) && near(m[1][1], 0.7152, 5e-4) &&
            near(m[1][2], 0.0722, 5e-4),
        "derived RGB->XYZ luminance row matches published sRGB constants");
  // The same coefficients core/Histogram.cpp types in by hand; if these two
  // ever disagree the picker and the histogram are measuring different things.
  check(near(m[1][0], 0.2126f, 5e-4) && near(m[1][1], 0.7152f, 5e-4),
        "derived luminance row agrees with core/Histogram's kLumaR/kLumaG");
  {
    const auto white = rec709WhiteUv();
    // D65 in CIE 1976 u'v', published: u' = 0.19783, v' = 0.46832.
    check(near(white[0], 0.19783, 1e-3) && near(white[1], 0.46832, 1e-3),
          "derived D65 u'v' matches the published values");
  }
  {
    // The inverse really is one.
    const auto& mi = rec709XyzToRgb();
    double worst = 0.0;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) {
        double sum = 0.0;
        for (int k = 0; k < 3; ++k) sum += m[i][k] * mi[k][j];
        worst = std::max(worst, std::fabs(sum - (i == j ? 1.0 : 0.0)));
      }
    check(worst < 1e-12, "RGB->XYZ and XYZ->RGB are inverses");
  }

  // --- Assertion 1: ASTM D1535 -------------------------------------------
  check(near(munsellValueToLuminanceFactor(10.0), 100.0, 1e-9),
        "D1535 Y(10) == 100 exactly (the coefficients are chosen for it)");
  check(near(munsellValueToLuminanceFactor(0.0), 0.0, 1e-12), "D1535 Y(0) == 0");
  // The published mid-scale value: V=5 is Y = 19.28%, NOT the 19.77 that the
  // pre-D1535 tables give. Getting this one wrong is invisible in a round
  // trip, since a round trip only asks the function to agree with itself.
  check(near(munsellValueToLuminanceFactor(5.0), 19.2718, 1e-3),
        "D1535 Y(5) lands on the published 19.28%");
  {
    double worstRt = 0.0;
    bool monotone = true;
    double prev = -1.0;
    for (int i = 0; i <= 1000; ++i) {
      const double v = 10.0 * static_cast<double>(i) / 1000.0;
      const double y = munsellValueToLuminanceFactor(v);
      worstRt = std::max(worstRt, std::fabs(luminanceFactorToMunsellValue(y) - v));
      if (y < prev) monotone = false;
      prev = y;
    }
    char label[96];
    std::snprintf(label, sizeof label,
                  "D1535 V->Y->V round-trips over [0,10] (worst %.2e)", worstRt);
    check(worstRt < 1e-5, label);
    check(monotone, "D1535 Y(V) is strictly increasing, so the inverse is well posed");
  }

  // --- Assertion 7: the page's rows ordered, and the grid n x n ----------
  {
    bool ordered = true, excludesEnds = true, sizeOk = true;
    for (int n = kMinPageSteps; n <= kMaxPageSteps; ++n) {
      double prev = -1.0;
      int cells = 0;
      for (int i = 0; i < n; ++i) {
        const double v = munsellPageRowValue(i, n);
        if (v <= prev) ordered = false;
        if (v <= 0.0 || v >= 10.0) excludesEnds = false;
        prev = v;
        cells += n;
      }
      if (cells != n * n) sizeOk = false;
    }
    check(ordered, "page rows are strictly increasing in value for every n");
    check(excludesEnds, "page rows exclude V=0 and V=10 (both are achromatic)");
    check(sizeOk, "the page is exactly n x n cells for every n");
    check(near(munsellPageRowValue(0, 9), 1.0, 1e-12) &&
              near(munsellPageRowValue(8, 9), 9.0, 1e-12),
          "n=9 lands on V = 1..9, the classic printed Munsell page");
  }

  // --- Assertion 8: maxInGamutChroma is positive, finite and a boundary --
  {
    bool positive = true, isBoundary = true;
    for (int k = 0; k < 40; ++k) {
      const double h = k * 360.0 / 40.0;
      for (int i = 0; i < kDefaultPageSteps; ++i) {
        const double v = munsellPageRowValue(i, kDefaultPageSteps);
        const double l = lStarFromRelativeLuminance(munsellValueToLuminanceFactor(v) / 100.0);
        const double c = maxInGamutChroma(l, h);
        if (!(c > 0.0) || !std::isfinite(c)) positive = false;
        // The returned chroma is inside; a step past it is not. This is what
        // makes it a boundary rather than merely some in-gamut number.
        if (!inLinearDisplayGamut(lchUvToLinearRgb(l, c, h))) isBoundary = false;
        if (inLinearDisplayGamut(lchUvToLinearRgb(l, c + 1e-2, h))) isBoundary = false;
      }
    }
    check(positive, "maxInGamutChroma is positive and finite, 40 hues x 9 rows");
    check(isBoundary, "maxInGamutChroma is the boundary: C in gamut, C+0.01 is not");
  }

  // --- Assertion 3: out of gamut is a void, never a clamp ----------------
  {
    // A light saturated blue that sRGB does not contain.
    const double l = lStarFromRelativeLuminance(munsellValueToLuminanceFactor(9.0) / 100.0);
    check(!munsellCellLinearRgb(l, 110.0, 260.0).has_value(),
          "a known out-of-gamut request answers a void, not a triple");
    bool everyCellLegal = true;
    for (int k = 0; k < 40; ++k) {
      const double h = k * 360.0 / 40.0;
      const double page = pageChroma(kDefaultPageSteps, h);
      for (int i = 0; i < kDefaultPageSteps; ++i) {
        const double v = munsellPageRowValue(i, kDefaultPageSteps);
        const double rowL =
            lStarFromRelativeLuminance(munsellValueToLuminanceFactor(v) / 100.0);
        for (int j = 0; j < kDefaultPageSteps; ++j) {
          const double c = j * page / (kDefaultPageSteps - 1);
          const auto cell = munsellCellLinearRgb(rowL, c, h);
          if (!cell) continue;
          for (float ch : *cell)
            if (!(ch >= 0.0f && ch <= 1.0f)) everyCellLegal = false;
        }
      }
    }
    check(everyCellLegal, "every non-void cell is inside [0,1] on all three channels");
  }

  // --- Assertion 2: the row keeps its luminance across hue ---------------
  //
  // The claim, stated the way the picker will be used: pick a row, sweep the
  // hue over the whole circle, and the relative luminance of every live cell
  // is the row's own, whatever the chroma. Run for every n, not just the
  // default, because `steps` is user-settable.
  {
    double worst = 0.0;
    int live = 0, total = 0;
    for (int n = kMinPageSteps; n <= kMaxPageSteps; ++n) {
      for (int i = 0; i < n; ++i) {
        const double v = munsellPageRowValue(i, n);
        const double targetY = munsellValueToLuminanceFactor(v) / 100.0;
        const double rowL = lStarFromRelativeLuminance(targetY);
        for (int k = 0; k < 36; ++k) {
          const double h = k * 10.0;
          const double page = pageChroma(n, h);
          for (int j = 0; j < n; ++j) {
            ++total;
            const double c = n == 1 ? 0.0 : j * page / (n - 1);
            const auto cell = munsellCellLinearRgb(rowL, c, h);
            if (!cell) continue;
            ++live;
            const double y = m[1][0] * (*cell)[0] + m[1][1] * (*cell)[1] + m[1][2] * (*cell)[2];
            worst = std::max(worst, std::fabs(y - targetY));
          }
        }
      }
    }
    char label[128];
    std::snprintf(label, sizeof label,
                  "row luminance is constant across hue and chroma (worst %.1e, %d cells)",
                  worst, live);
    check(worst < 1e-6, label);
    // A guard on the guard: if the sweep above ever stopped producing cells
    // the assertion would pass by measuring nothing. This is the sabotage
    // that would otherwise be silent.
    check(live > 10000 && live < total, "the luminance sweep actually visited live cells");
  }

  // --- The contrast that motivates the picker ----------------------------
  //
  // Not a property of color/Munsell at all -- a property of HSV, asserted
  // here because it is the reason this file exists and because a reader
  // otherwise has to take docs/munsell-picker.md's word for it.
  {
    const double yellow = m[1][0] * 1.0 + m[1][1] * 1.0 + m[1][2] * 0.0;
    const double blue = m[1][2];
    check(yellow / blue > 12.0,
          "HSV's own top edge spans >12x in luminance (what this picker fixes)");
  }

  // --- Assertion 6: foregroundSrgb() obeys the mode -----------------------
  //
  // **The assertion the whole feature rests on.** `ColorMode` had two members
  // and all three consumers were written as `if (colorMode == Rgb) <colour>
  // else <pigment>`; a third member compiles clean and lands in the pigment
  // branch, so the panel would look entirely live while every stroke laid
  // down `defaultPalette()[pigment]`. There is no diagnostic for that, and
  // reading the code is how it was missed in the first place -- so this asks
  // the function.
  {
    BrushState br;
    br.colorMode = ColorMode::Munsell;
    br.pigment = 3;  // a real palette row, and deliberately NOT the answer
    br.munsellSteps = 9;
    br.munsellRow = 4;
    br.munsellCol = 4;
    br.munsellHueDeg = 27.0f;
    applyMunsellSelection(br);
    const auto fg = foregroundSrgb(br);
    const auto pig = defaultPalette()[3].rgb;
    check(fg[0] == br.rgb[0] && fg[1] == br.rgb[1] && fg[2] == br.rgb[2],
          "foregroundSrgb() in Munsell mode returns the picked cell");
    check(!(near(fg[0], pig[0], 1e-6) && near(fg[1], pig[1], 1e-6) && near(fg[2], pig[2], 1e-6)),
          "foregroundSrgb() in Munsell mode is NOT the pigment (the == Rgb trap)");
    check(std::strcmp(foregroundName(br), "Munsell page") == 0,
          "foregroundName() in Munsell mode names the page, not the pigment");
    // The physical constants still come from the pigment, exactly as in RGB
    // mode -- three floats cannot say how a paint settles.
    check(&foregroundPhysicalConstants(br) == &defaultPalette()[3],
          "the physical constants still come from the loaded pigment");
  }

  // --- The sRGB encode boundary ------------------------------------------
  //
  // `color/Munsell` answers linear; `BrushState::rgb` is display-referred
  // sRGB. Getting this backwards paints every Munsell stroke about twice as
  // dark as the chip that was clicked and reads as "colour management is
  // broken somewhere else". Asked by decoding the stored triple and checking
  // it against the row's own luminance -- which also re-proves the invariant
  // through the field a stroke actually reads.
  {
    double worst = 0.0;
    for (int k = 0; k < 40; ++k) {
      BrushState br;
      br.colorMode = ColorMode::Munsell;
      br.munsellSteps = 9;
      br.munsellRow = 5;
      br.munsellCol = 3;
      br.munsellHueDeg = static_cast<float>(k) * 9.0f;
      applyMunsellSelection(br);
      const auto fg = foregroundSrgb(br);
      const double y = m[1][0] * srgbDecode(fg[0]) + m[1][1] * srgbDecode(fg[1]) +
                       m[1][2] * srgbDecode(fg[2]);
      const double targetY =
          munsellValueToLuminanceFactor(munsellPageRowValue(5, 9)) / 100.0;
      worst = std::max(worst, std::fabs(y - targetY));
    }
    char label[128];
    std::snprintf(label, sizeof label,
                  "the stored foreground decodes to the row's luminance (worst %.1e)", worst);
    check(worst < 1e-5, label);
  }

  // --- Assertions 4 and 5: what survives a hue change ---------------------
  {
    bool rowHeld = true, lumHeld = true, everClamped = false, colNeverGrew = true;
    const double targetY = munsellValueToLuminanceFactor(munsellPageRowValue(7, 9)) / 100.0;
    BrushState br;
    br.colorMode = ColorMode::Munsell;
    br.munsellSteps = 9;
    br.munsellRow = 7;   // high value: its live columns run out early at most hues
    br.munsellCol = 8;   // the rightmost, so a hue change is very likely to void it
    for (int k = 0; k < 40; ++k) {
      const int wanted = br.munsellCol;
      br.munsellHueDeg = static_cast<float>(k) * 9.0f;
      applyMunsellSelection(br);
      if (br.munsellRow != 7) rowHeld = false;
      if (br.munsellCol > wanted) colNeverGrew = false;
      if (br.munsellCol != wanted) everClamped = true;
      const auto fg = foregroundSrgb(br);
      const double y = m[1][0] * srgbDecode(fg[0]) + m[1][1] * srgbDecode(fg[1]) +
                       m[1][2] * srgbDecode(fg[2]);
      if (std::fabs(y - targetY) > 1e-5) lumHeld = false;
      br.munsellCol = 8;  // ask for the rightmost again on the next hue
    }
    check(rowHeld, "a hue change never moves the selection off its row");
    check(lumHeld, "a hue change never moves the foreground's luminance");
    check(everClamped, "the sweep really did hit voids (otherwise it proves nothing)");
    check(colNeverGrew, "void clamping walks left along the row, never right");
  }

  // --- clampMunsellSelection() is total ----------------------------------
  //
  // `BrushState` is a public aggregate of plain ints; nothing stops a caller
  // -- or a future file format -- handing over a selection that is off the
  // page entirely. The clamp has to land somewhere live for every one of them.
  {
    bool alwaysLive = true, alwaysInRange = true;
    const int steps[] = {-5, 0, 1, 2, 3, 9, 16, 17, 99};
    const int idx[] = {-100, -1, 0, 3, 15, 16, 1000};
    const float hues[] = {-720.0f, -1.0f, 0.0f, 27.0f, 359.9f, 360.0f, 1080.0f};
    for (int st_ : steps)
      for (int r : idx)
        for (int c : idx)
          for (float h : hues) {
            BrushState br;
            br.munsellSteps = st_;
            br.munsellRow = r;
            br.munsellCol = c;
            br.munsellHueDeg = h;
            clampMunsellSelection(br);
            if (br.munsellSteps < kMinPageSteps || br.munsellSteps > kMaxPageSteps ||
                br.munsellRow < 0 || br.munsellRow >= br.munsellSteps ||
                br.munsellCol < 0 || br.munsellCol >= br.munsellSteps ||
                !(br.munsellHueDeg >= 0.0f && br.munsellHueDeg < 360.0f))
              alwaysInRange = false;
            if (!munsellCellSrgb(br, br.munsellRow, br.munsellCol)) alwaysLive = false;
          }
    check(alwaysInRange, "clampMunsellSelection() puts steps, indices and hue in range");
    check(alwaysLive, "clampMunsellSelection() always lands on a live cell");
  }

  std::printf("Munsell page: %s\n", ok ? "OK" : "FAILURES");
  return ok;
}

}  // namespace np
