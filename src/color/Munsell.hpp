#pragma once
#include <array>
#include <optional>

// The Munsell page: the geometry behind the COLOR panel's third picker
// (docs/munsell-picker.md).
//
// **The one property this file exists to guarantee.** Munsell *value* is
// defined by ASTM D1535 as a quintic in the luminance factor Y and nothing
// else, so "same Munsell value" and "same relative luminance" are the same
// statement. Every function below is arranged so that a page ROW has one
// luminance which does not move when the hue changes -- which is what an HSV
// square cannot do, its `V` being `max(r,g,b)` rather than a luminance.
//
// **Why CIELUV and not OKLab.** Within a row we need a hue angle and a chroma
// radius. CIELUV's `L*` is a pure function of `Y` and its `u*,v*` do not touch
// it, so "constant L*" and "constant Y" are identically the same set --
// exactly, not approximately. OKLab has better hue spacing and its lightness
// is a function of all three cone responses, so a constant-OK-L row would
// drift in luminance and the guarantee above would become a tolerance. A
// tolerance is what rots; the trade is not close.
//
// **Out of gamut is a void, never a clamp**, and that is a consequence of the
// same guarantee rather than a matter of taste: clamping a channel changes Y.
// A row of clamped cells silently stops being a constant-luminance row, and
// the drift is invisible where a hole is not. `munsellCellLinearRgb()`
// therefore answers `std::nullopt` rather than a clipped triple.
//
// Everything here is `double` internally and `float` at the boundary. The
// bisection in `maxInGamutChroma()` is the reason: it converges on a gamut
// boundary, and running it in `float` puts the answer's last bits inside the
// step size rather than inside the gamut.
//
// No dependency on ImGui and none on app/AppState -- this is the arithmetic,
// and `--selftest` points at it directly (`app/selftest/Munsell.cpp`).
namespace np {

// --- ASTM D1535 -----------------------------------------------------------
//
// `Y` here is the **luminance factor in percent**, 100 for the perfect
// diffuser, which is the unit the standard is written in. The relative
// luminance the rest of this codebase deals in (`core/Histogram.cpp`'s
// kLumaR/G/B, `core/SelectionRefine.cpp`'s `selectionLuminanceOf`) is this
// divided by 100.
//
// The quintic is exact at both ends by construction of its coefficients --
// `munsellValueToLuminanceFactor(10)` is 100 to within 1e-9, which
// `--selftest` pins rather than assumes -- and is strictly increasing on
// [0,10], which is what lets the inverse below be a plain bisection with no
// special cases.
double munsellValueToLuminanceFactor(double value) noexcept;
double luminanceFactorToMunsellValue(double yPercent) noexcept;

// CIE L* from **relative** luminance (Y in [0,1], not percent).
double lStarFromRelativeLuminance(double y) noexcept;

// --- The page's rows ------------------------------------------------------
//
// `V_i = 10 (i+1) / (steps+1)`, bottom-up, so row 0 is the darkest.
//
// **The offsets are load-bearing.** An inclusive sampling `10 i / (n-1)`
// would put V=0 and V=10 in the set; both are the achromatic black and white
// points, whose maximum chroma is zero, so two of the n rows would each hold
// a single colour repeated n times. The half-open form also makes the default
// `steps == 9` land on exactly V = 1, 2, ... 9 -- the classic printed Munsell
// hue page.
inline constexpr int kMinPageSteps = 3;
inline constexpr int kMaxPageSteps = 16;
inline constexpr int kDefaultPageSteps = 9;
double munsellPageRowValue(int row, int steps) noexcept;

// --- CIELUV ---------------------------------------------------------------
//
// `L*C*h(uv)` to linear Rec.709 RGB. **Not clamped and not gamut-tested** --
// the returned triple may hold channels outside [0,1], and the caller decides
// what that means. `munsellCellLinearRgb()` is the caller that decides
// correctly.
std::array<double, 3> lchUvToLinearRgb(double lStar, double chroma, double hueDeg) noexcept;

bool inLinearDisplayGamut(const std::array<double, 3>& linear) noexcept;

// The page cell. `std::nullopt` **is** the void: the requested chroma does not
// exist in sRGB at that lightness and hue, and no in-gamut triple stands for
// it. See the header comment on why this is not a clamp.
std::optional<std::array<float, 3>> munsellCellLinearRgb(double lStar, double chroma,
                                                         double hueDeg) noexcept;

// The largest chroma still inside sRGB at this lightness and hue, by
// bisection on `inLinearDisplayGamut`. Zero when even the neutral is out of
// range, which cannot happen for L* in [0,100] but is the honest answer if it
// ever does.
double maxInGamutChroma(double lStar, double hueDeg) noexcept;

// The chroma the whole page is normalised against: the largest
// `maxInGamutChroma` over the page's own rows. Normalising per PAGE rather
// than per ROW is what keeps a column meaning one chroma across rows, which
// is what makes the gamut's leaf shape legible instead of squared-off.
double pageChroma(int steps, double hueDeg) noexcept;

// --- The derived matrix ---------------------------------------------------
//
// `color/Space.hpp` carries `kRec709Primaries` and deliberately no matrix
// ("deriving one is a job for whichever later step actually needs it").
// This is that step, so the matrix is **derived from those primaries here**
// rather than typed in a second time -- two copies of a white point is how
// they drift apart. `--selftest` checks the derived result against the
// published sRGB constants rather than against itself.
const std::array<std::array<double, 3>, 3>& rec709RgbToXyz() noexcept;
const std::array<std::array<double, 3>, 3>& rec709XyzToRgb() noexcept;
// D65 in CIE 1976 u'v', from the same matrix.
std::array<double, 2> rec709WhiteUv() noexcept;

}  // namespace np
