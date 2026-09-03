#include "color/Gamut.hpp"

#include <cmath>

namespace np {
namespace {

// The Bradford cone response matrix and its inverse, from the CIE's
// definition (and reproduced identically in the ICC specification's `chad`
// section). Written out rather than inverted at runtime: the inverse is
// published alongside the forward matrix, and computing it would introduce a
// rounding difference from every other implementation for no benefit.
constexpr ColorMat3 kBradford{{0.8951f, 0.2664f, -0.1614f, -0.7502f, 1.7135f, 0.0367f, 0.0389f,
                              -0.0685f, 1.0296f}};
constexpr ColorMat3 kBradfordInverse{{0.9869929f, -0.1470543f, 0.1599627f, 0.4323053f, 0.5183603f,
                                      0.0492912f, -0.0085287f, 0.0400428f, 0.9684867f}};

// A chromaticity pair as XYZ at unit luminance (Y = 1). Undefined for y == 0,
// which every caller guards.
std::array<float, 3> xyToXyz(float x, float y) {
  return {x / y, 1.0f, (1.0f - x - y) / y};
}

ColorMat3 diagonal(float a, float b, float c) {
  ColorMat3 d;
  d.m = {a, 0.0f, 0.0f, 0.0f, b, 0.0f, 0.0f, 0.0f, c};
  return d;
}

}  // namespace

ColorMat3 colorMat3Multiply(const ColorMat3& a, const ColorMat3& b) {
  ColorMat3 r;
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) {
      float sum = 0.0f;
      for (int k = 0; k < 3; ++k) sum += a.m[row * 3 + k] * b.m[k * 3 + col];
      r.m[row * 3 + col] = sum;
    }
  return r;
}

std::optional<ColorMat3> colorMat3Inverse(const ColorMat3& a) {
  const auto& m = a.m;
  const float c00 = m[4] * m[8] - m[5] * m[7];
  const float c01 = m[5] * m[6] - m[3] * m[8];
  const float c02 = m[3] * m[7] - m[4] * m[6];
  const float det = m[0] * c00 + m[1] * c01 + m[2] * c02;
  // A gamut whose primaries are collinear, or an ICC profile whose colorants
  // are all zero, lands here. Refused rather than returning garbage: the
  // caller's fallback (leave the pixels alone) is correct and a NaN matrix
  // would poison every texel it touched.
  if (!std::isfinite(det) || std::fabs(det) < 1e-12f) return std::nullopt;
  const float inv = 1.0f / det;

  ColorMat3 r;
  r.m[0] = c00 * inv;
  r.m[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
  r.m[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
  r.m[3] = c01 * inv;
  r.m[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
  r.m[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
  r.m[6] = c02 * inv;
  r.m[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
  r.m[8] = (m[0] * m[4] - m[1] * m[3]) * inv;
  return r;
}

std::array<float, 3> colorMat3Apply(const ColorMat3& m, const std::array<float, 3>& rgb) {
  return {m.m[0] * rgb[0] + m.m[1] * rgb[1] + m.m[2] * rgb[2],
          m.m[3] * rgb[0] + m.m[4] * rgb[1] + m.m[5] * rgb[2],
          m.m[6] * rgb[0] + m.m[7] * rgb[1] + m.m[8] * rgb[2]};
}

bool colorMat3NearIdentity(const ColorMat3& m, float tol) {
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col) {
      const float want = row == col ? 1.0f : 0.0f;
      if (!(std::fabs(m.m[row * 3 + col] - want) <= tol)) return false;
    }
  return true;
}

std::optional<ColorMat3> rgbToXyz(const Primaries& p) {
  // y == 0 would put a primary on the alychne, where X/y is unbounded. Every
  // real gamut's blue is close to it (ProPhoto's is y = 0.0001, which is
  // small but finite and produces a perfectly good matrix), so this rejects
  // only a genuinely malformed set rather than an extreme one.
  if (p.redY == 0.0f || p.greenY == 0.0f || p.blueY == 0.0f || p.whiteY == 0.0f)
    return std::nullopt;

  const std::array<float, 3> r = xyToXyz(p.redX, p.redY);
  const std::array<float, 3> g = xyToXyz(p.greenX, p.greenY);
  const std::array<float, 3> b = xyToXyz(p.blueX, p.blueY);
  const std::array<float, 3> w = xyToXyz(p.whiteX, p.whiteY);

  // Columns are the primaries, so this maps a scale triple to the XYZ it
  // produces; inverting it asks the reverse question -- which scales make
  // (1,1,1) land exactly on the white point.
  ColorMat3 primaryColumns;
  primaryColumns.m = {r[0], g[0], b[0], r[1], g[1], b[1], r[2], g[2], b[2]};

  const std::optional<ColorMat3> inverse = colorMat3Inverse(primaryColumns);
  if (!inverse) return std::nullopt;
  const std::array<float, 3> scale = colorMat3Apply(*inverse, w);

  return colorMat3Multiply(primaryColumns, diagonal(scale[0], scale[1], scale[2]));
}

ColorMat3 bradfordAdaptation(float srcWhiteX, float srcWhiteY, float dstWhiteX, float dstWhiteY) {
  // Guarded rather than asserted: a caller reaching this with y == 0 has a
  // malformed white point, and an identity adaptation is the answer that
  // leaves the colour alone instead of destroying it.
  if (srcWhiteY == 0.0f || dstWhiteY == 0.0f) return ColorMat3{};

  const std::array<float, 3> srcCone =
      colorMat3Apply(kBradford, xyToXyz(srcWhiteX, srcWhiteY));
  const std::array<float, 3> dstCone =
      colorMat3Apply(kBradford, xyToXyz(dstWhiteX, dstWhiteY));
  if (srcCone[0] == 0.0f || srcCone[1] == 0.0f || srcCone[2] == 0.0f) return ColorMat3{};

  const ColorMat3 ratio =
      diagonal(dstCone[0] / srcCone[0], dstCone[1] / srcCone[1], dstCone[2] / srcCone[2]);
  return colorMat3Multiply(kBradfordInverse, colorMat3Multiply(ratio, kBradford));
}

std::optional<ColorMat3> rgbToXyzD50(const Primaries& p) {
  const std::optional<ColorMat3> toXyz = rgbToXyz(p);
  if (!toXyz) return std::nullopt;
  // A gamut already defined at D50 (ProPhoto) gets an identity adaptation out
  // of this, arrived at by the general path rather than special-cased -- one
  // code path, and the identity is a measurable property rather than a
  // branch someone has to keep true.
  const ColorMat3 adapt = bradfordAdaptation(p.whiteX, p.whiteY, kD50WhiteX, kD50WhiteY);
  return colorMat3Multiply(adapt, *toXyz);
}

std::optional<ColorMat3> gamutConversion(const ColorMat3& srcRgbToXyzD50,
                                         const ColorMat3& dstRgbToXyzD50) {
  const std::optional<ColorMat3> xyzToDst = colorMat3Inverse(dstRgbToXyzD50);
  if (!xyzToDst) return std::nullopt;
  return colorMat3Multiply(*xyzToDst, srcRgbToXyzD50);
}

const ColorMat3& rec709RgbToXyzD50() {
  // Function-local static: computed once, on first use, from the same
  // `rgbToXyzD50()` every other gamut goes through -- so a change to the
  // derivation cannot make the destination and the sources disagree. The
  // fallback identity is unreachable for a well-formed constant and exists so
  // this returns a reference rather than an optional.
  static const ColorMat3 kValue = [] {
    const std::optional<ColorMat3> m = rgbToXyzD50(kRec709Primaries);
    return m ? *m : ColorMat3{};
  }();
  return kValue;
}

}  // namespace np
