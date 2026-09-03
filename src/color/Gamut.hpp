#pragma once

#include <array>
#include <optional>

#include "color/Space.hpp"

// color/Gamut -- turning a `Primaries` into arithmetic, and one gamut's linear
// RGB into another's.
//
// ==========================================================================
// What this closes
// ==========================================================================
//
// `color/Space.hpp`'s `Primaries` comment said a matrix would be "a job for
// whichever later step actually needs to adapt primaries", and named export to
// a different gamut as that step. A different step got there first, and it is
// a correctness bug rather than a feature:
//
//   * `WorkingSpace::primaries` is fully load-bearing -- serialised as EXR
//     chromaticities, compared by `core/DirtyTiles`, carried by every history
//     entry, threaded through export.
//   * `kRec709Primaries` was the **only** `Primaries` value in the tree, and
//     no importer read primaries from a file. Every image that arrived was
//     assumed Rec.709.
//   * naturalPaint ships on macOS, where **Display P3 is the default** output
//     of the camera, the screenshot key and the display. So the single most
//     common file a user drags onto the canvas was the one being decoded with
//     the wrong primaries -- silently, with no warning path, and irreversibly
//     once painted over.
//
// `io/OiioBackend.hpp` documented the *transfer function* half of this
// honestly ("linearity is decided by the file's sample type"). The primaries
// half was documented nowhere: `ICCProfile` and `oiio:ColorSpace` appear
// nowhere in `src/`, and ICC appears nowhere in `PRD.md`, `PLAN.md` or
// `docs/` -- not as a goal and not as a non-goal.
//
// ==========================================================================
// The scope decision: convert on import, do not carry a foreign gamut
// ==========================================================================
//
// There are two ways to be correct here, and this file deliberately takes the
// smaller one.
//
// The large one is to make `WorkingSpace` genuinely variable: let a document
// *be* Display P3, carry that through the compositor, the history and the
// file format. That is a real feature and it is not this. It would also
// immediately break export, which **refuses** a source/target primaries
// mismatch by name today (`io/Export.hpp`: "this build converts transfer
// functions, never primaries, and says so rather than ignoring it") -- so
// carrying a P3 document would trade a silent colour error for a loud refusal
// to save it, which is not obviously an improvement for the user.
//
// The small one, taken here: **the document is still always Rec.709, and the
// file's own primaries are converted into it at the decode boundary.** The
// compositor, the history, the format and the export path are untouched and
// cannot regress. A P3 photograph simply arrives with the right colour. What
// is deliberately NOT bought is round-tripping wide-gamut data -- a P3 image
// converted to Rec.709 has values outside [0,1] where it was more saturated
// than Rec.709 can express, and those survive (the tiles are `HALF` and this
// pipeline is unclamped by policy, see `color/Space.hpp`) but they are now
// Rec.709 numbers, not P3 ones. A future variable working space is the thing
// that fixes that, and this file is what it would be built on rather than
// something it would have to undo.
//
// ==========================================================================
// Why everything is expressed relative to D50
// ==========================================================================
//
// Two sources of gamut information have to end up in one vocabulary:
//
//  1. **Named primaries** (a PNG `cHRM` chunk, or one of the constants below),
//     which are xy chromaticities plus a white point that is usually D65.
//  2. **An ICC profile's colorants** (`rXYZ`/`gXYZ`/`bXYZ`), which are
//     already XYZ and already adapted to D50 -- the ICC profile connection
//     space is defined at D50 and a matrix-shaper profile's colorant tags are
//     stored in it. That is not a convention this file is choosing; it is what
//     the tags mean.
//
// So (2) hands over a finished RGB->XYZ(D50) matrix and (1) needs one built
// and then Bradford-adapted to D50. Meeting at D50 means the ICC path -- the
// one that matters most, since it is how a phone photo and a macOS screenshot
// both carry P3 -- does no adaptation at all and cannot get it wrong. Rec.709
// pays one adaptation instead, once, into a constant.
namespace np {

// A 3x3 matrix over linear-light colour, **row-major**: `m[row * 3 + col]`.
//
// Deliberately not `ops/Transform.hpp`'s `Mat3`, which is a 2D affine
// transform of *positions* with its own homogeneous conventions. Sharing a
// type between "where a pixel is" and "what colour it is" would make a
// nonsensical multiplication compile.
struct ColorMat3 {
  std::array<float, 9> m{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
};

ColorMat3 colorMat3Multiply(const ColorMat3& a, const ColorMat3& b);

// `std::nullopt` when `a` is singular. A caller that gets one has been handed
// a degenerate gamut -- three collinear primaries, or an ICC profile whose
// colorants are zero -- and must fall back rather than divide by zero.
std::optional<ColorMat3> colorMat3Inverse(const ColorMat3& a);

std::array<float, 3> colorMat3Apply(const ColorMat3& m, const std::array<float, 3>& rgb);

// Whether every element is within `tol` of the identity, i.e. whether applying
// this matrix would be a no-op worth skipping. The decode path asks this so an
// ordinary sRGB file pays nothing at all for this feature existing.
bool colorMat3NearIdentity(const ColorMat3& m, float tol);

// --- Gamuts this build can name -------------------------------------------
//
// Values from each gamut's defining document, not from a colour library:
// Display P3 from SMPTE RP 431-2 primaries with a D65 white (Apple's
// "Display P3" is exactly that pairing), Adobe RGB (1998) from Adobe's own
// specification, Rec.2020 from ITU-R BT.2020, ProPhoto from ISO 22028-2.
//
// **ProPhoto's white is D50, not D65** -- that is not a typo and it is the
// reason the adaptation below cannot be skipped as "everything is D65 anyway".

// Display P3: DCI-P3 primaries, D65 white, sRGB transfer. The default output
// of every recent Apple camera, screenshot and display, and therefore the
// single most important non-Rec.709 gamut for this application.
inline constexpr Primaries kDisplayP3Primaries{
    0.680f, 0.320f,    // R
    0.265f, 0.690f,    // G
    0.150f, 0.060f,    // B
    0.3127f, 0.3290f,  // white (D65)
};

inline constexpr Primaries kAdobeRgb1998Primaries{
    0.640f, 0.330f,    // R
    0.210f, 0.710f,    // G
    0.150f, 0.060f,    // B
    0.3127f, 0.3290f,  // white (D65)
};

inline constexpr Primaries kRec2020Primaries{
    0.708f, 0.292f,    // R
    0.170f, 0.797f,    // G
    0.131f, 0.046f,    // B
    0.3127f, 0.3290f,  // white (D65)
};

inline constexpr Primaries kProPhotoRgbPrimaries{
    0.7347f, 0.2653f,  // R
    0.1596f, 0.8404f,  // G
    0.0366f, 0.0001f,  // B
    0.3457f, 0.3585f,  // white (D50 -- see above)
};

// The ICC profile connection space's illuminant, as xy. The XYZ form the ICC
// specification tabulates (0.9642, 1.0000, 0.8249) is what these coordinates
// reproduce, and `--selftest` checks that rather than leaving two spellings of
// one constant to drift.
inline constexpr float kD50WhiteX = 0.3457f;
inline constexpr float kD50WhiteY = 0.3585f;

// --- The arithmetic --------------------------------------------------------

// Linear RGB -> XYZ **in the primaries' own white point**, by the standard
// derivation: build the 3x3 of each primary's XYZ at unit luminance, solve for
// the per-channel scale factors that make (1,1,1) map exactly to the white
// point, and scale the columns by them.
//
// `std::nullopt` for a degenerate set (any primary with y == 0, or three
// primaries that do not span the plane).
std::optional<ColorMat3> rgbToXyz(const Primaries& p);

// The Bradford chromatic adaptation matrix taking XYZ referred to `srcWhite`
// to XYZ referred to `dstWhite`. Bradford rather than von Kries or XYZ
// scaling because it is what ICC itself specifies for the `chad` tag, so the
// named-primaries path and the ICC path agree about what "adapted to D50"
// means instead of being close but not equal.
ColorMat3 bradfordAdaptation(float srcWhiteX, float srcWhiteY, float dstWhiteX, float dstWhiteY);

// `rgbToXyz()` followed by Bradford adaptation into the D50 connection space.
// The one vocabulary this file's header describes -- every gamut, however it
// was discovered, becomes one of these.
std::optional<ColorMat3> rgbToXyzD50(const Primaries& p);

// The conversion taking linear RGB in the source gamut to linear RGB in the
// destination, both given as RGB->XYZ(D50). `std::nullopt` when `dst` cannot
// be inverted.
std::optional<ColorMat3> gamutConversion(const ColorMat3& srcRgbToXyzD50,
                                         const ColorMat3& dstRgbToXyzD50);

// `rgbToXyzD50(kRec709Primaries)`, computed once. The destination every
// import converts into, given this build's fixed working space.
const ColorMat3& rec709RgbToXyzD50();

}  // namespace np
