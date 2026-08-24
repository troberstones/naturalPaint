#include "ops/Transform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

#include "core/Premultiply.hpp"
#include "ops/Resample.hpp"

namespace np {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// The largest destination extent this file will allocate for. Not a policy
// about what a document may be -- it is the point past which `width * height *
// 4` floats stops being a buffer and starts being a bug in the caller's
// arithmetic. 2^30 samples is 4 GiB of float, which is already far past
// anything PRD A5 tolerates holding invisibly; the refusal names the number so
// a caller that hit it by overflow can see it did.
constexpr size_t kMaxDestinationSamples = size_t{1} << 30;

// How far, in destination pixels, the leftover transform after a prefilter is
// allowed to displace a corner before this file bothers running a
// reconstruction pass for it.
//
// **This number is measured, not chosen.** After an area-average prefilter
// lands the source on the destination extent, the leftover matrix is
// `scale(dstW/srcW) * scale(srcW/prefW)` evaluated in float, which is 1.0 only
// to within rounding: for a 4096-wide image that residual displaces the far
// corner by a fraction of a pixel that is pure float noise. The value below is
// the maximum such displacement observed over every (source, destination) size
// pair with source in [2, 4096] and destination in [1, source] -- 8 386 560
// pairs, swept by a throwaway harness (not committed) -- with headroom, taken
// as the next power of two above the measurement:
//
//     measured maximum corner displacement : 4.861354828e-04 px (src 4091 -> 4078)
//     value used                           : 9.765625e-04 px = 2^-10 (2.01x headroom)
//
// The same sweep confirmed the other half of the arrangement: over all
// 8 386 560 pairs, `lround(srcN * float(dstN)/float(srcN))` equalled `dstN`
// **every time**, so the prefilter always does land exactly on the destination
// extent and this skip is always the one that fires for a pure downscale.
//
// It is ~1/1024 of a pixel. Two things follow, and both matter: no real
// transform lands inside it (a translate that small is not a translate), and
// running a Catmull-Rom pass to "honour" a displacement 2000x smaller than the
// half-float quantum at the values involved would attenuate real detail to
// chase noise. Below the threshold the correct answer is a verbatim copy, and
// that is what makes a pure downscale through this file bit-identical to
// `resampleAreaAverage()`'s own output.
constexpr float kIdentitySkipPixels = 9.765625e-04f;

bool failTransform(std::string* errorOut, TransformImage* out, std::string message) {
  if (out) {
    out->px.clear();
    out->width = 0;
    out->height = 0;
  }
  if (errorOut) *errorOut = std::move(message);
  return false;
}

// True when `v` is exactly representable as a whole number. Exact `==`, not a
// tolerance: this gates PRD D15's no-resample path, and a tolerance here would
// let a transform that is 0.001 px off a flip copy texels verbatim and be
// wrong by a pixel. See Transform.hpp section 4.
bool isExactInteger(float v) noexcept {
  return std::isfinite(v) && v == std::floor(v);
}

// The homography's Jacobian at a source point, as the two column vectors
// (d(dst)/dx, d(dst)/dy). For an affine matrix (bottom row 0, 0, 1) this
// reduces exactly to the linear part, which is why there is one formula here
// and not two.
void jacobianAt(const Mat3& t, float x, float y, float jac[4]) noexcept {
  const float* m = t.m.data();
  const float u = m[0] * x + m[1] * y + m[2];
  const float v = m[3] * x + m[4] * y + m[5];
  const float w = m[6] * x + m[7] * y + m[8];
  const float w2 = w * w;
  if (!(std::fabs(w2) > 0.0f) || !std::isfinite(w2)) {
    jac[0] = jac[1] = jac[2] = jac[3] = std::numeric_limits<float>::quiet_NaN();
    return;
  }
  jac[0] = (m[0] * w - u * m[6]) / w2;  // dX/dx
  jac[1] = (m[1] * w - u * m[7]) / w2;  // dX/dy
  jac[2] = (m[3] * w - v * m[6]) / w2;  // dY/dx
  jac[3] = (m[4] * w - v * m[7]) / w2;  // dY/dy
}

float sincPi(float x) noexcept {
  if (x == 0.0f) return 1.0f;
  const float px = kPi * x;
  return std::sin(px) / px;
}

// Mitchell and Netravali's cubic family. Catmull-Rom is (B, C) = (0, 0.5) and
// Mitchell is (1/3, 1/3); the two kernels PLAN.md names separately are two
// points on this one curve, so they share an implementation. Writing them out
// as two hard-coded polynomials would be two chances to transpose a
// coefficient and no chance for a reader to see that they are related.
float mitchellNetravali(float t, float B, float C) noexcept {
  const float x = std::fabs(t);
  const float x2 = x * x;
  const float x3 = x2 * x;
  if (x < 1.0f) {
    return ((12.0f - 9.0f * B - 6.0f * C) * x3 + (-18.0f + 12.0f * B + 6.0f * C) * x2 +
            (6.0f - 2.0f * B)) /
           6.0f;
  }
  if (x < 2.0f) {
    return ((-B - 6.0f * C) * x3 + (6.0f * B + 30.0f * C) * x2 + (-12.0f * B - 48.0f * C) * x +
            (8.0f * B + 24.0f * C)) /
           6.0f;
  }
  return 0.0f;
}

// Un-premultiply a whole image into a straight-alpha buffer, and back.
//
// This pair exists for exactly one call -- the prefilter, which goes through
// ops/Resample's `resampleAreaAverage()`, whose interface is straight-alpha
// because its own caller (io/ExportAs) lives on the file side of the
// premultiply boundary. Everything else in this file stays premultiplied
// throughout (Transform.hpp section 2).
//
// The round trip is not free and the cost is stated rather than absorbed: two
// extra full-image passes, and one divide plus one multiply per channel whose
// rounding is not exactly invertible for alphas that are not powers of two.
// It **is** mathematically the identity around the filter, because
// `resampleAreaAverage()` premultiplies internally by the same alpha this pair
// divided out, so what the box filter integrates is the original premultiplied
// values -- which is the correct thing to integrate.
//
// The alternative was a second area-average written natively in premultiplied
// space. Rejected, and not narrowly: the exact-footprint weight construction
// in ops/Resample.cpp (fractional end weights, per-axis plans, double weights
// because normalised float ones cost an opaque image its exportability) is
// subtle enough that a second copy would be a second thing to keep correct,
// and the failure mode of the two drifting apart is a downscale that
// prefilters slightly differently depending on which entry point reached it.
void unpremultiplyImage(const TransformImage& src, std::vector<float>* out) {
  out->resize(src.px.size());
  const size_t texels = src.px.size() / 4u;
  for (size_t i = 0; i < texels; ++i) {
    const float* p = src.px.data() + i * 4u;
    const std::array<float, 4> straight =
        unpremultiply(std::array<float, 4>{p[0], p[1], p[2], p[3]});
    float* d = out->data() + i * 4u;
    for (int c = 0; c < 4; ++c) d[c] = straight[c];
  }
}

void premultiplyInPlace(std::vector<float>* buf) {
  const size_t texels = buf->size() / 4u;
  for (size_t i = 0; i < texels; ++i) {
    float* p = buf->data() + i * 4u;
    const float a = p[3];
    p[0] *= a;
    p[1] *= a;
    p[2] *= a;
  }
}

// PRD D15's path. Scatter rather than gather: a signed permutation with an
// integer translation is a bijection on the integer pixel grid, so walking the
// *source* and writing each texel to its image covers every in-range
// destination exactly once and needs no inverse matrix -- and therefore has no
// inverse-matrix rounding to be exact in spite of.
//
// The inner statement is a 16-byte copy of four floats. No multiply, no add,
// no kernel. That is the entire content of "exact": --selftest compares with
// memcmp because anything a tolerance would accept, this must not need.
void exactRemapScatter(const TransformImage& src, const Mat3& t, TransformImage* out) {
  for (uint32_t j = 0; j < src.height; ++j) {
    for (uint32_t i = 0; i < src.width; ++i) {
      const Point2 p = mat3MapPoint(
          t, Point2{static_cast<float>(i) + 0.5f, static_cast<float>(j) + 0.5f});
      // Exact by construction: a signed permutation of half-integers plus an
      // integer translation is another half-integer, and floor() of a
      // half-integer is exact in binary floating point.
      const float fx = std::floor(p.x);
      const float fy = std::floor(p.y);
      if (fx < 0.0f || fy < 0.0f) continue;
      const auto dx = static_cast<uint32_t>(fx);
      const auto dy = static_cast<uint32_t>(fy);
      if (dx >= out->width || dy >= out->height) continue;
      const float* s = src.px.data() + (static_cast<size_t>(j) * src.width + i) * 4u;
      float* d = out->px.data() + (static_cast<size_t>(dy) * out->width + dx) * 4u;
      std::memcpy(d, s, sizeof(float) * 4u);
    }
  }
}

}  // namespace

// ==========================================================================
// Matrix arithmetic
// ==========================================================================

Mat3 mat3Identity() noexcept { return Mat3{}; }

Mat3 mat3Multiply(const Mat3& a, const Mat3& b) noexcept {
  Mat3 r;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      // Accumulated in float, deliberately. The exact path (Transform.hpp
      // section 4) depends on products and sums of exact 0 and +-1 staying
      // exact, which they do in IEEE float; widening to double and narrowing
      // back would also be exact but would suggest the precision mattered
      // here, and it does not -- these are 3x3 matrices, not an accumulation.
      r.m[row * 3 + col] = a.m[row * 3 + 0] * b.m[0 * 3 + col] +
                           a.m[row * 3 + 1] * b.m[1 * 3 + col] +
                           a.m[row * 3 + 2] * b.m[2 * 3 + col];
    }
  }
  return r;
}

bool mat3Invert(const Mat3& in, Mat3* out) noexcept {
  if (out == nullptr) return false;
  const float* m = in.m.data();
  for (int i = 0; i < 9; ++i) {
    if (!std::isfinite(m[i])) return false;
  }

  const float c00 = m[4] * m[8] - m[5] * m[7];
  const float c01 = m[5] * m[6] - m[3] * m[8];
  const float c02 = m[3] * m[7] - m[4] * m[6];
  const float det = m[0] * c00 + m[1] * c01 + m[2] * c02;

  // Scale-relative singularity test. An absolute epsilon would be wrong in
  // both directions here: this file's matrices carry translations of canvas
  // size (thousands), so their determinants can be large, while a transform
  // built from a normalised quad can legitimately have a determinant near 1e-3
  // and still be perfectly invertible. Comparing against the cube of the
  // largest entry makes the test dimensionless, which is what "is this matrix
  // degenerate" actually asks.
  float norm = 0.0f;
  for (int i = 0; i < 9; ++i) norm = std::max(norm, std::fabs(m[i]));
  if (norm <= 0.0f) return false;
  if (!std::isfinite(det) || std::fabs(det) <= 1e-9f * norm * norm * norm) return false;

  const float inv = 1.0f / det;
  Mat3 r;
  r.m[0] = c00 * inv;
  r.m[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
  r.m[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
  r.m[3] = c01 * inv;
  r.m[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
  r.m[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
  r.m[6] = c02 * inv;
  r.m[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
  r.m[8] = (m[0] * m[4] - m[1] * m[3]) * inv;
  *out = r;
  return true;
}

Point2 mat3MapPoint(const Mat3& t, Point2 p) noexcept {
  const float* m = t.m.data();
  const float x = m[0] * p.x + m[1] * p.y + m[2];
  const float y = m[3] * p.x + m[4] * p.y + m[5];
  const float w = m[6] * p.x + m[7] * p.y + m[8];
  // An affine matrix has w == 1 exactly, and dividing by it would be a no-op
  // that still costs a rounding. Skipped by comparison rather than by a flag,
  // so the exact path (which needs `-x + W` to come back bit-exact) does not
  // depend on the caller having told us the matrix was affine.
  if (w == 1.0f) return Point2{x, y};
  if (w == 0.0f) {
    const float inf = std::numeric_limits<float>::infinity();
    return Point2{x >= 0.0f ? inf : -inf, y >= 0.0f ? inf : -inf};
  }
  return Point2{x / w, y / w};
}

// ==========================================================================
// Builders
// ==========================================================================

Mat3 transformTranslate(float tx, float ty) noexcept {
  Mat3 t;
  t.m = {1.0f, 0.0f, tx, 0.0f, 1.0f, ty, 0.0f, 0.0f, 1.0f};
  return t;
}

Mat3 transformScale(float sx, float sy) noexcept {
  Mat3 t;
  t.m = {sx, 0.0f, 0.0f, 0.0f, sy, 0.0f, 0.0f, 0.0f, 1.0f};
  return t;
}

Mat3 transformScaleAbout(float sx, float sy, Point2 pivot) noexcept {
  return mat3Multiply(
      mat3Multiply(transformTranslate(pivot.x, pivot.y), transformScale(sx, sy)),
      transformTranslate(-pivot.x, -pivot.y));
}

Mat3 transformRotateDegrees(float degrees) noexcept {
  // The snap that makes a rotation box reading "90" land on PRD D15's exact
  // path. `cosf(pi/2)` is -4.37e-8 rather than zero, so a quarter turn built
  // through the trigonometric path is a signed permutation matrix that is
  // *almost* exact -- and exactRemapKind() compares with `==`, correctly, so
  // it would classify as None and the flip-free quarter turn would be
  // resampled. The snap is on the *input angle*, which is a number the user
  // typed, not on the resulting entries, which would be tolerance-based
  // rounding of a computed result and a much worse idea.
  if (std::isfinite(degrees)) {
    const float quarters = degrees / 90.0f;
    if (quarters == std::floor(quarters)) {
      const int k = static_cast<int>(std::fmod(quarters, 4.0f));
      const int kk = ((k % 4) + 4) % 4;
      static constexpr float kCos[4] = {1.0f, 0.0f, -1.0f, 0.0f};
      static constexpr float kSin[4] = {0.0f, 1.0f, 0.0f, -1.0f};
      Mat3 t;
      t.m = {kCos[kk], -kSin[kk], 0.0f, kSin[kk], kCos[kk], 0.0f, 0.0f, 0.0f, 1.0f};
      return t;
    }
  }
  const float r = degrees * (kPi / 180.0f);
  const float c = std::cos(r);
  const float s = std::sin(r);
  Mat3 t;
  t.m = {c, -s, 0.0f, s, c, 0.0f, 0.0f, 0.0f, 1.0f};
  return t;
}

Mat3 transformRotateDegreesAbout(float degrees, Point2 pivot) noexcept {
  return mat3Multiply(
      mat3Multiply(transformTranslate(pivot.x, pivot.y), transformRotateDegrees(degrees)),
      transformTranslate(-pivot.x, -pivot.y));
}

Mat3 transformSkewDegrees(float xDegrees, float yDegrees) noexcept {
  // Clamped short of the tangent's pole. A 90 degree skew is not a large skew,
  // it is a projection onto a line, and letting `tanf` return 1.6e16 would
  // produce a matrix that transformImage() refuses -- reporting the failure at
  // the resampler, a long way from the slider that caused it.
  //
  // Note what is deliberately *not* clamped: skewing 45 degrees on **both**
  // axes gives the shear matrix [[1,1],[1,1]], whose determinant is exactly
  // zero. That one is genuinely degenerate rather than merely extreme -- there
  // is no "nearly right" answer to fudge towards -- so it reaches
  // transformImage()'s named refusal, which is the correct outcome.
  const float lim = 89.9f;
  const float ax = std::clamp(std::isfinite(xDegrees) ? xDegrees : 0.0f, -lim, lim);
  const float ay = std::clamp(std::isfinite(yDegrees) ? yDegrees : 0.0f, -lim, lim);
  Mat3 t;
  t.m = {1.0f,                              std::tan(ax * (kPi / 180.0f)), 0.0f,
         std::tan(ay * (kPi / 180.0f)),      1.0f,                         0.0f,
         0.0f,                               0.0f,                         1.0f};
  return t;
}

Mat3 transformFlipHorizontal(uint32_t width) noexcept {
  // x' = W - x. Pixel centre i + 0.5 maps to W - i - 0.5, the centre of texel
  // W - 1 - i. Exact 0 / +-1 entries plus an integer translation, so this is
  // on the exact path and stays there under composition.
  Mat3 t;
  t.m = {-1.0f, 0.0f, static_cast<float>(width), 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
  return t;
}

Mat3 transformFlipVertical(uint32_t height) noexcept {
  Mat3 t;
  t.m = {1.0f, 0.0f, 0.0f, 0.0f, -1.0f, static_cast<float>(height), 0.0f, 0.0f, 1.0f};
  return t;
}

Mat3 transformRotate90(int quarterTurns, uint32_t width, uint32_t height) noexcept {
  const int k = ((quarterTurns % 4) + 4) % 4;
  const Mat3 rot = transformRotateDegrees(static_cast<float>(k) * 90.0f);

  // The translation is derived by mapping the source rectangle's corners and
  // shifting the minimum to the origin, rather than by four hand-written
  // cases. Hand-written cases are where an off-by-one in the odd-turn extent
  // lives; this is exact for the same reason the rest of the path is (the
  // entries are 0 / +-1 and the corners are whole numbers).
  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  const Point2 corners[4] = {{0.0f, 0.0f}, {w, 0.0f}, {0.0f, h}, {w, h}};
  float minX = std::numeric_limits<float>::infinity();
  float minY = minX;
  for (const Point2& c : corners) {
    const Point2 p = mat3MapPoint(rot, c);
    minX = std::min(minX, p.x);
    minY = std::min(minY, p.y);
  }
  return mat3Multiply(transformTranslate(-minX, -minY), rot);
}

bool transformFromQuad(const std::array<Point2, 4>& src, const std::array<Point2, 4>& dst,
                       Mat3* out, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (out == nullptr) {
    if (errorOut) *errorOut = "four-corner transform refused: no output matrix (internal error).";
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    if (!std::isfinite(src[i].x) || !std::isfinite(src[i].y) || !std::isfinite(dst[i].x) ||
        !std::isfinite(dst[i].y)) {
      if (errorOut)
        *errorOut =
            "four-corner transform refused: corner " + std::to_string(i) +
            " is not a finite point. A handle dragged to infinity has no homography.";
      return false;
    }
  }

  // Eight unknowns (h8 is fixed at 1, which is the standard normalisation and
  // is why a homography has eight degrees of freedom rather than nine), two
  // equations per correspondence:
  //
  //     h0 x + h1 y + h2 - h6 x X - h7 y X = X
  //     h3 x + h4 y + h5 - h6 x Y - h7 y Y = Y
  //
  // Solved in **double**. Not caution: the system's condition number blows up
  // as the quad approaches degeneracy, and a float solve on a merely
  // *slanted* quad -- which is the whole point of a perspective handle --
  // already loses the far corner by a visible fraction of a pixel. The matrix
  // is 8x8; the cost of doing it in double is nothing.
  double A[8][9];
  for (int i = 0; i < 4; ++i) {
    const double x = src[i].x, y = src[i].y;
    const double X = dst[i].x, Y = dst[i].y;
    double* r0 = A[i * 2 + 0];
    r0[0] = x;  r0[1] = y;  r0[2] = 1; r0[3] = 0; r0[4] = 0; r0[5] = 0;
    r0[6] = -x * X; r0[7] = -y * X; r0[8] = X;
    double* r1 = A[i * 2 + 1];
    r1[0] = 0; r1[1] = 0; r1[2] = 0; r1[3] = x; r1[4] = y; r1[5] = 1;
    r1[6] = -x * Y; r1[7] = -y * Y; r1[8] = Y;
  }

  // Gauss-Jordan with partial pivoting.
  for (int col = 0; col < 8; ++col) {
    int pivot = col;
    for (int r = col + 1; r < 8; ++r) {
      if (std::fabs(A[r][col]) > std::fabs(A[pivot][col])) pivot = r;
    }
    if (std::fabs(A[pivot][col]) < 1e-12) {
      if (errorOut)
        *errorOut =
            "four-corner transform refused: the corners do not define a transform. Three "
            "points on one line, a quad collapsed to a line or a point, or a destination "
            "quad folded over itself all produce a singular system -- there is no matrix "
            "that maps these four points to those four points.";
      return false;
    }
    if (pivot != col) {
      for (int c = 0; c < 9; ++c) std::swap(A[col][c], A[pivot][c]);
    }
    const double inv = 1.0 / A[col][col];
    for (int c = col; c < 9; ++c) A[col][c] *= inv;
    for (int r = 0; r < 8; ++r) {
      if (r == col) continue;
      const double f = A[r][col];
      if (f == 0.0) continue;
      for (int c = col; c < 9; ++c) A[r][c] -= f * A[col][c];
    }
  }

  Mat3 t;
  for (int i = 0; i < 8; ++i) {
    if (!std::isfinite(A[i][8])) {
      if (errorOut)
        *errorOut =
            "four-corner transform refused: the solve produced a non-finite coefficient; the "
            "corner quad is numerically degenerate.";
      return false;
    }
    t.m[i] = static_cast<float>(A[i][8]);
  }
  t.m[8] = 1.0f;
  *out = t;
  return true;
}

Mat3 TransformStack::composed() const noexcept {
  // Right to left, because Mat3 maps column vectors: the first pushed
  // transform must end up rightmost so it is applied first. An empty stack
  // folds to the identity, which is the correct no-op for a transform tool
  // that has not been dragged yet, not an error.
  Mat3 result = mat3Identity();
  for (const Mat3& t : entries_) result = mat3Multiply(t, result);
  return result;
}

// ==========================================================================
// Kernels
// ==========================================================================

float resampleKernelRadius(ResampleKernel kernel) noexcept {
  switch (kernel) {
    case ResampleKernel::Nearest: return 0.5f;
    case ResampleKernel::Bilinear: return 1.0f;
    case ResampleKernel::CatmullRom: return 2.0f;
    case ResampleKernel::Mitchell: return 2.0f;
    case ResampleKernel::Lanczos3: return 3.0f;
  }
  return 2.0f;
}

float resampleKernelWeight(ResampleKernel kernel, float t) noexcept {
  switch (kernel) {
    case ResampleKernel::Nearest:
      // Half-open on purpose: `[-0.5, 0.5)` selects exactly one source texel
      // for every position including the exact ties at integer coordinates,
      // where a closed interval would select two and a strict one would
      // select none. A tie has to break *somewhere*, and breaking it
      // consistently upwards is what keeps a nearest-neighbour resize from
      // dropping or doubling a row at exactly the boundaries.
      return (t >= -0.5f && t < 0.5f) ? 1.0f : 0.0f;
    case ResampleKernel::Bilinear: {
      const float x = std::fabs(t);
      return x < 1.0f ? 1.0f - x : 0.0f;
    }
    case ResampleKernel::CatmullRom:
      return mitchellNetravali(t, 0.0f, 0.5f);
    case ResampleKernel::Mitchell:
      return mitchellNetravali(t, 1.0f / 3.0f, 1.0f / 3.0f);
    case ResampleKernel::Lanczos3: {
      const float x = std::fabs(t);
      if (x >= 3.0f) return 0.0f;
      return sincPi(t) * sincPi(t / 3.0f);
    }
  }
  return 0.0f;
}

const char* resampleKernelName(ResampleKernel kernel) noexcept {
  switch (kernel) {
    case ResampleKernel::Nearest: return "nearest";
    case ResampleKernel::Bilinear: return "bilinear";
    case ResampleKernel::CatmullRom: return "Catmull-Rom";
    case ResampleKernel::Mitchell: return "Mitchell";
    case ResampleKernel::Lanczos3: return "Lanczos3";
  }
  return "unknown";
}

// ==========================================================================
// Exact classification (PRD D15)
// ==========================================================================

const char* exactRemapName(ExactRemap kind) noexcept {
  switch (kind) {
    case ExactRemap::None: return "none";
    case ExactRemap::Identity: return "identity";
    case ExactRemap::FlipHorizontal: return "flip horizontal";
    case ExactRemap::FlipVertical: return "flip vertical";
    case ExactRemap::Rotate90: return "rotate 90";
    case ExactRemap::Rotate180: return "rotate 180";
    case ExactRemap::Rotate270: return "rotate 270";
    case ExactRemap::Transpose: return "transpose";
    case ExactRemap::AntiTranspose: return "anti-transpose";
  }
  return "none";
}

ExactRemap exactRemapKind(const Mat3& t) noexcept {
  const float* m = t.m.data();
  // No perspective. A projective term makes the map non-affine and there is no
  // signed permutation to find.
  if (m[6] != 0.0f || m[7] != 0.0f || m[8] != 1.0f) return ExactRemap::None;
  if (!isExactInteger(m[2]) || !isExactInteger(m[5])) return ExactRemap::None;

  const float a = m[0], b = m[1], c = m[3], d = m[4];
  // Exactly one of the two diagonals is populated, with entries exactly +-1
  // and the other pair exactly 0. Written as the eight explicit cases rather
  // than as a "count the non-zeros" loop, because the eight cases are what the
  // enum is and a reader can check them against it by eye.
  if (b == 0.0f && c == 0.0f) {
    if (a == 1.0f && d == 1.0f) return ExactRemap::Identity;
    if (a == -1.0f && d == 1.0f) return ExactRemap::FlipHorizontal;
    if (a == 1.0f && d == -1.0f) return ExactRemap::FlipVertical;
    if (a == -1.0f && d == -1.0f) return ExactRemap::Rotate180;
    return ExactRemap::None;
  }
  if (a == 0.0f && d == 0.0f) {
    if (b == -1.0f && c == 1.0f) return ExactRemap::Rotate90;
    if (b == 1.0f && c == -1.0f) return ExactRemap::Rotate270;
    if (b == 1.0f && c == 1.0f) return ExactRemap::Transpose;
    if (b == -1.0f && c == -1.0f) return ExactRemap::AntiTranspose;
    return ExactRemap::None;
  }
  return ExactRemap::None;
}

TransformBounds transformedBounds(const Mat3& t, uint32_t width, uint32_t height) noexcept {
  const float w = static_cast<float>(width);
  const float h = static_cast<float>(height);
  const Point2 corners[4] = {{0.0f, 0.0f}, {w, 0.0f}, {0.0f, h}, {w, h}};
  TransformBounds b;
  b.minX = b.minY = std::numeric_limits<float>::infinity();
  b.maxX = b.maxY = -std::numeric_limits<float>::infinity();
  for (const Point2& cn : corners) {
    const Point2 p = mat3MapPoint(t, cn);
    b.minX = std::min(b.minX, p.x);
    b.minY = std::min(b.minY, p.y);
    b.maxX = std::max(b.maxX, p.x);
    b.maxY = std::max(b.maxY, p.y);
  }
  return b;
}

// ==========================================================================
// The resampler
// ==========================================================================

bool transformImage(const TransformImage& src, const Mat3& dstFromSrc, uint32_t dstWidth,
                    uint32_t dstHeight, const TransformParams& params, TransformImage* out,
                    TransformReport* report, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  TransformReport localReport;
  if (report) *report = TransformReport{};
  if (out == nullptr) {
    if (errorOut)
      *errorOut = "transform refused: no destination image was supplied (internal error).";
    return false;
  }
  if (out == &src) {
    // Refused rather than handled. Every path here clears the destination
    // before it reads the source, so an in-place call would resample a buffer
    // it had just zeroed -- and would do it *silently*, returning true and a
    // blank image. Supporting it properly means a hidden temporary, which is
    // the allocation PRD A5 objects to being invisible; the caller can make
    // one they can see.
    if (errorOut)
      *errorOut =
          "transform refused: the source and destination are the same image. This resampler "
          "is not in-place -- it clears the destination before reading the source. Pass a "
          "separate destination and move it over the source afterwards.";
    return false;
  }
  if (!src.valid()) {
    char buf[224];
    std::snprintf(buf, sizeof(buf),
                  "transform refused: the source image is %ux%u with %zu samples, which is not "
                  "a complete RGBA buffer.",
                  src.width, src.height, src.px.size());
    return failTransform(errorOut, out, buf);
  }
  if (dstWidth == 0 || dstHeight == 0) {
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "transform refused: the destination extent is %ux%u; both dimensions must be "
                  "at least 1 pixel.",
                  dstWidth, dstHeight);
    return failTransform(errorOut, out, buf);
  }
  const size_t dstSamples =
      static_cast<size_t>(dstWidth) * static_cast<size_t>(dstHeight) * 4u;
  if (dstSamples > kMaxDestinationSamples) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "transform refused: a %ux%u destination is %zu float samples, past this "
                  "build's %zu-sample ceiling. That size is nearly always an overflowed "
                  "extent rather than an intended one.",
                  dstWidth, dstHeight, dstSamples, kMaxDestinationSamples);
    return failTransform(errorOut, out, buf);
  }

  out->width = dstWidth;
  out->height = dstHeight;
  // Transparent black everywhere first. This is both the allocation and the
  // edge policy (Transform.hpp): a destination texel nothing maps onto keeps
  // premultiplied zero, which composites as "nothing here" rather than as a
  // colour.
  out->px.assign(dstSamples, 0.0f);

  // --- PRD D15: the exact path ------------------------------------------
  const ExactRemap kind = exactRemapKind(dstFromSrc);
  if (params.allowExactPaths && kind != ExactRemap::None) {
    exactRemapScatter(src, dstFromSrc, out);
    localReport.exact = kind;
    localReport.reconstructionPasses = 0;
    localReport.axisScaleX = 1.0f;
    localReport.axisScaleY = 1.0f;
    if (report) *report = localReport;
    return true;
  }

  // --- The minification measurement, and the prefilter it decides -------
  //
  // Taken from the Jacobian at the source image's centre. For an affine
  // transform the Jacobian is constant and this is exact; for a perspective
  // one it varies and this is the centre's value, which is the approximation
  // Transform.hpp section 3 names and bounds.
  float jac[4];
  jacobianAt(dstFromSrc, static_cast<float>(src.width) * 0.5f,
             static_cast<float>(src.height) * 0.5f, jac);
  // Column norms: the length the transform gives to a unit step along each
  // *source* axis. A rotation gives both columns length 1 and so asks for no
  // prefilter, which is right -- a rotation does not minify.
  const float colX = std::sqrt(jac[0] * jac[0] + jac[2] * jac[2]);
  const float colY = std::sqrt(jac[1] * jac[1] + jac[3] * jac[3]);
  localReport.axisScaleX = std::isfinite(colX) ? colX : 1.0f;
  localReport.axisScaleY = std::isfinite(colY) ? colY : 1.0f;

  const TransformImage* sampleSrc = &src;
  Mat3 effective = dstFromSrc;
  TransformImage prefiltered;

  if (params.prefilterDownscale && std::isfinite(colX) && std::isfinite(colY)) {
    const float sx = std::min(1.0f, colX > 0.0f ? colX : 1.0f);
    const float sy = std::min(1.0f, colY > 0.0f ? colY : 1.0f);
    const auto pw = static_cast<uint32_t>(std::clamp<long>(
        std::lround(static_cast<double>(src.width) * static_cast<double>(sx)), 1L,
        static_cast<long>(src.width)));
    const auto ph = static_cast<uint32_t>(std::clamp<long>(
        std::lround(static_cast<double>(src.height) * static_cast<double>(sy)), 1L,
        static_cast<long>(src.height)));

    if (pw < src.width || ph < src.height) {
      std::vector<float> straight;
      unpremultiplyImage(src, &straight);
      std::vector<float> reduced;
      std::string resampleError;
      if (!resampleAreaAverage(straight.data(), src.width, src.height, pw, ph, &reduced,
                               &resampleError)) {
        return failTransform(errorOut, out,
                             "transform refused: the antialiasing prefilter could not run -- " +
                                 resampleError);
      }
      premultiplyInPlace(&reduced);
      prefiltered.width = pw;
      prefiltered.height = ph;
      prefiltered.px = std::move(reduced);
      sampleSrc = &prefiltered;

      // **This is the composition PRD D16 is about, applied to the prefilter
      // itself.** The prefiltered image's coordinate k stands for source
      // coordinate k * (srcW / pw), so the map from prefiltered coordinates to
      // destination coordinates is `dstFromSrc * scale(srcW/pw, srcH/ph)`. One
      // matrix, one reconstruction pass. Applying the prefilter as a separate
      // resize and then transforming the result would be two generations, and
      // would have made this file's own downscale the first thing to violate
      // the rule it exists to enforce.
      effective = mat3Multiply(
          dstFromSrc, transformScale(static_cast<float>(src.width) / static_cast<float>(pw),
                                     static_cast<float>(src.height) / static_cast<float>(ph)));
      localReport.prefiltered = true;
      localReport.prefilterWidth = pw;
      localReport.prefilterHeight = ph;
    }
  }

  // --- Is there anything left to reconstruct? ----------------------------
  //
  // After a prefilter that landed exactly on the destination extent, `effective`
  // is the identity to within float rounding. Measured as the largest corner
  // displacement over the destination extent, in pixels, and compared against
  // kIdentitySkipPixels -- see that constant for the measurement behind it.
  if (sampleSrc->width == dstWidth && sampleSrc->height == dstHeight) {
    // Each corner is compared against **where it started**, not against a
    // bounding box. The first version of this compared `transformedBounds()`
    // to the source extent, and --selftest caught it: a horizontal flip leaves
    // the bounding box exactly where it was, so the flip classified as
    // identity and the image came back unflipped. A bounding box is invariant
    // under precisely the transforms this test must not accept.
    const float w = static_cast<float>(sampleSrc->width);
    const float h = static_cast<float>(sampleSrc->height);
    const Point2 corners[4] = {{0.0f, 0.0f}, {w, 0.0f}, {0.0f, h}, {w, h}};
    float drift = 0.0f;
    for (const Point2& c : corners) {
      const Point2 p = mat3MapPoint(effective, c);
      drift = std::max(drift, std::max(std::fabs(p.x - c.x), std::fabs(p.y - c.y)));
    }
    if (std::isfinite(drift) && drift <= kIdentitySkipPixels) {
      out->px = sampleSrc->px;
      localReport.reconstructionPasses = 0;
      localReport.exact = ExactRemap::Identity;
      if (report) *report = localReport;
      return true;
    }
  }

  Mat3 srcFromDst;
  if (!mat3Invert(effective, &srcFromDst)) {
    return failTransform(
        errorOut, out,
        "transform refused: the composed matrix is not invertible. A zero scale on an axis, a "
        "45-degree skew on both axes at once, or a four-corner quad collapsed to a line all "
        "produce one; there is no source position for a destination pixel to read from, so "
        "there is nothing to resample rather than something to approximate.");
  }

  // --- The one reconstruction pass ---------------------------------------
  const ResampleKernel kernel = params.kernel;
  const float radius = resampleKernelRadius(kernel);
  const auto sw = static_cast<int64_t>(sampleSrc->width);
  const auto sh = static_cast<int64_t>(sampleSrc->height);
  const float* sp = sampleSrc->px.data();

  // Weight scratch, sized once. The footprint is at most `2*radius + 2` taps
  // per axis.
  const int maxTaps = static_cast<int>(std::ceil(2.0f * radius)) + 2;
  std::vector<float> wx(static_cast<size_t>(maxTaps));
  std::vector<float> wy(static_cast<size_t>(maxTaps));

  for (uint32_t dy = 0; dy < dstHeight; ++dy) {
    float* dstRow = out->px.data() + static_cast<size_t>(dy) * dstWidth * 4u;
    for (uint32_t dx = 0; dx < dstWidth; ++dx) {
      const Point2 s = mat3MapPoint(
          srcFromDst, Point2{static_cast<float>(dx) + 0.5f, static_cast<float>(dy) + 0.5f});
      if (!std::isfinite(s.x) || !std::isfinite(s.y)) continue;  // stays transparent

      // **The source rectangle is a hard boundary, and the taps inside it are
      // clamped.** Two separate decisions, and the first is the one that
      // decides whether a destination texel exists at all: if the position
      // this texel reads from is outside the source image, there is nothing
      // there and it stays transparent black -- which is what keeps a rotated
      // layer's far corners empty instead of smeared with border colour.
      //
      // Inside the boundary, a kernel footprint that overhangs the edge reads
      // the edge texel repeatedly rather than reading transparent black. The
      // alternative -- letting the missing taps contribute (0,0,0,0) at full
      // weight, so the border fades over the kernel's support -- is what this
      // file did first, and the harness caught it: upscaling a **fully
      // opaque** 8x8 field 8 -> 21 with Lanczos3 left the border texels at
      // alpha 0.944 instead of 1.0. A resize that makes an opaque image
      // translucent around the edge is a defect, not an edge policy, and it is
      // worst for exactly the sharpest kernel.
      //
      // Under premultiplied alpha the two policies differ **only** where the
      // source's own border texels are not transparent -- clamping replicates
      // (0,0,0,0) wherever the content already ended, which is the fade. So
      // clamping costs nothing on a layer whose content stops before the
      // buffer edge, and is the correct answer for one whose content runs to
      // it. What clamping does not do is antialias the boundary itself: a
      // full-bleed layer rotated 30 degrees gets a hard, aliased rectangle
      // edge, because a smooth one needs the coverage of each destination
      // texel by the transformed source *polygon*, which is real machinery and
      // is named as absent in Transform.hpp rather than approximated by a
      // filter artefact that happens to look soft.
      if (s.x < 0.0f || s.y < 0.0f || s.x >= static_cast<float>(sw) ||
          s.y >= static_cast<float>(sh))
        continue;

      // Footprint in source texel indices. Source texel i's centre is i + 0.5,
      // so the taps are those i with |s - (i + 0.5)| <= radius. Widened by one
      // on each side rather than computed tightly: the extra taps evaluate to
      // weight zero, and a tight bound computed with ceil/floor on a float is
      // where an off-by-one at exact tie positions lives.
      const auto i0 = static_cast<int64_t>(std::floor(s.x - radius - 0.5f));
      const auto i1 = static_cast<int64_t>(std::ceil(s.x + radius - 0.5f));
      const auto j0 = static_cast<int64_t>(std::floor(s.y - radius - 0.5f));
      const auto j1 = static_cast<int64_t>(std::ceil(s.y + radius - 0.5f));
      const int nx = static_cast<int>(i1 - i0 + 1);
      const int ny = static_cast<int>(j1 - j0 + 1);
      if (nx <= 0 || ny <= 0 || nx > maxTaps || ny > maxTaps) continue;

      // **Why the weights are normalised at all.** Nearest, bilinear and both
      // cubics are partitions of unity: their weights at any fractional offset
      // sum to exactly 1 and dividing by that sum is a no-op. Lanczos3 is not
      // -- its weights sum to 1 only to about 1e-3, and left alone that
      // residual is a periodic brightness ripple across the whole image rather
      // than an edge artefact. Since every tap is clamped into range, the sum
      // here is over the full footprint and every term of it contributes, so
      // the division is exactly the right one at the border too.
      float sumW = 0.0f;
      for (int k = 0; k < nx; ++k) {
        wx[static_cast<size_t>(k)] =
            resampleKernelWeight(kernel, s.x - (static_cast<float>(i0 + k) + 0.5f));
      }
      for (int k = 0; k < ny; ++k) {
        wy[static_cast<size_t>(k)] =
            resampleKernelWeight(kernel, s.y - (static_cast<float>(j0 + k) + 0.5f));
      }
      for (int ky = 0; ky < ny; ++ky) {
        for (int kx = 0; kx < nx; ++kx) sumW += wy[static_cast<size_t>(ky)] * wx[static_cast<size_t>(kx)];
      }
      if (!(std::fabs(sumW) > 1e-12f)) continue;

      // Accumulated in double for the same reason ops/Resample.cpp does it:
      // the destination of this may be a 32-bit float export, and the
      // accumulator should not be the coarsest stage in the pipeline. Both
      // buffers stay float; only the accumulator widens.
      double acc[4] = {0.0, 0.0, 0.0, 0.0};
      for (int ky = 0; ky < ny; ++ky) {
        const int64_t sy = std::clamp<int64_t>(j0 + ky, 0, sh - 1);
        const float wyv = wy[static_cast<size_t>(ky)];
        if (wyv == 0.0f) continue;
        const float* srcRow = sp + static_cast<size_t>(sy) * sampleSrc->width * 4u;
        for (int kx = 0; kx < nx; ++kx) {
          const int64_t sx = std::clamp<int64_t>(i0 + kx, 0, sw - 1);
          const double w = static_cast<double>(wyv) * static_cast<double>(wx[static_cast<size_t>(kx)]);
          if (w == 0.0) continue;
          const float* p = srcRow + static_cast<size_t>(sx) * 4u;
          acc[0] += w * static_cast<double>(p[0]);
          acc[1] += w * static_cast<double>(p[1]);
          acc[2] += w * static_cast<double>(p[2]);
          acc[3] += w * static_cast<double>(p[3]);
        }
      }

      const double inv = 1.0 / static_cast<double>(sumW);
      float* d = dstRow + static_cast<size_t>(dx) * 4u;
      const float alpha = static_cast<float>(acc[3] * inv);
      // core/Premultiply's own rule, applied where the value is created rather
      // than left for a reader to hit later (Transform.hpp section 2). A
      // negative alpha out of a negative kernel lobe is not a coverage.
      if (alpha <= 0.0f) {
        d[0] = d[1] = d[2] = d[3] = 0.0f;
      } else {
        d[0] = static_cast<float>(acc[0] * inv);
        d[1] = static_cast<float>(acc[1] * inv);
        d[2] = static_cast<float>(acc[2] * inv);
        d[3] = alpha;
      }
    }
  }

  localReport.reconstructionPasses = 1;
  if (report) *report = localReport;
  return true;
}

// ==========================================================================
// PRD D17: crop, canvas size, image size
// ==========================================================================

bool cropImage(const TransformImage& src, int32_t x, int32_t y, uint32_t width, uint32_t height,
               TransformImage* out, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (out == nullptr) {
    if (errorOut) *errorOut = "crop refused: no destination image (internal error).";
    return false;
  }
  if (out == &src) {
    // Same reason as transformImage(): the destination is cleared before the
    // source is read, so in-place would silently produce a blank image.
    if (errorOut)
      *errorOut =
          "crop refused: the source and destination are the same image. This crop is not "
          "in-place -- it clears the destination before reading the source.";
    return false;
  }
  if (!src.valid()) {
    return failTransform(errorOut, out,
                         "crop refused: the source image is not a complete RGBA buffer.");
  }
  if (width == 0 || height == 0) {
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "crop refused: the requested rectangle is %ux%u; both dimensions must be at "
                  "least 1 pixel.",
                  width, height);
    return failTransform(errorOut, out, buf);
  }
  const size_t samples = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  if (samples > kMaxDestinationSamples) {
    return failTransform(errorOut, out,
                         "crop refused: the requested rectangle is past this build's sample "
                         "ceiling; that is nearly always an overflowed extent.");
  }

  out->width = width;
  out->height = height;
  out->px.assign(samples, 0.0f);

  // The overlap, computed in int64 so a rectangle placed far outside the image
  // cannot wrap. Rows are copied with memcpy: a crop performs **no arithmetic
  // on texel values**, which is what makes it bit-exact and is why it does not
  // go through the matrix path.
  const int64_t sx0 = std::max<int64_t>(0, x);
  const int64_t sy0 = std::max<int64_t>(0, y);
  const int64_t sx1 = std::min<int64_t>(static_cast<int64_t>(src.width),
                                        static_cast<int64_t>(x) + static_cast<int64_t>(width));
  const int64_t sy1 = std::min<int64_t>(static_cast<int64_t>(src.height),
                                        static_cast<int64_t>(y) + static_cast<int64_t>(height));
  if (sx1 <= sx0 || sy1 <= sy0) return true;  // entirely outside: all transparent, and correct

  const size_t runBytes = static_cast<size_t>(sx1 - sx0) * 4u * sizeof(float);
  for (int64_t sy = sy0; sy < sy1; ++sy) {
    const float* s = src.px.data() + (static_cast<size_t>(sy) * src.width +
                                      static_cast<size_t>(sx0)) * 4u;
    const int64_t dyi = sy - y;
    const int64_t dxi = sx0 - x;
    float* d = out->px.data() +
               (static_cast<size_t>(dyi) * width + static_cast<size_t>(dxi)) * 4u;
    std::memcpy(d, s, runBytes);
  }
  return true;
}

bool resizeCanvas(const TransformImage& src, uint32_t width, uint32_t height, CanvasAnchor anchor,
                  TransformImage* out, std::string* errorOut) {
  if (!src.valid()) {
    if (out) { out->px.clear(); out->width = out->height = 0; }
    if (errorOut) *errorOut = "canvas size refused: the source image is not a complete RGBA buffer.";
    return false;
  }
  const int32_t dw = static_cast<int32_t>(width) - static_cast<int32_t>(src.width);
  const int32_t dh = static_cast<int32_t>(height) - static_cast<int32_t>(src.height);

  // Floored, not rounded -- see resizeCanvas()'s declaration for why the
  // parity has to be consistent. floorDiv() is core/Tile.hpp's, which already
  // exists precisely because C's `/` truncates towards zero and that is the
  // wrong half for a negative delta (a canvas being *shrunk* around its
  // centre).
  int32_t ox = 0;
  switch (anchor) {
    case CanvasAnchor::TopLeft:
    case CanvasAnchor::CenterLeft:
    case CanvasAnchor::BottomLeft: ox = 0; break;
    case CanvasAnchor::TopCenter:
    case CanvasAnchor::Center:
    case CanvasAnchor::BottomCenter: ox = floorDiv(dw, 2); break;
    case CanvasAnchor::TopRight:
    case CanvasAnchor::CenterRight:
    case CanvasAnchor::BottomRight: ox = dw; break;
  }
  int32_t oy = 0;
  switch (anchor) {
    case CanvasAnchor::TopLeft:
    case CanvasAnchor::TopCenter:
    case CanvasAnchor::TopRight: oy = 0; break;
    case CanvasAnchor::CenterLeft:
    case CanvasAnchor::Center:
    case CanvasAnchor::CenterRight: oy = floorDiv(dh, 2); break;
    case CanvasAnchor::BottomLeft:
    case CanvasAnchor::BottomCenter:
    case CanvasAnchor::BottomRight: oy = dh; break;
  }

  // The source lands at (ox, oy) in the new canvas, which is the same thing as
  // cropping the source at (-ox, -oy). One index-copy loop in this file, not
  // two.
  return cropImage(src, -ox, -oy, width, height, out, errorOut);
}

bool resizeImage(const TransformImage& src, uint32_t width, uint32_t height,
                 const TransformParams& params, TransformImage* out, TransformReport* report,
                 std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (report) *report = TransformReport{};
  if (out == nullptr) {
    if (errorOut) *errorOut = "image size refused: no destination image (internal error).";
    return false;
  }
  if (!src.valid()) {
    return failTransform(errorOut, out,
                         "image size refused: the source image is not a complete RGBA buffer.");
  }
  if (width == 0 || height == 0) {
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "image size refused: the requested size is %ux%u; both dimensions must be at "
                  "least 1 pixel.",
                  width, height);
    return failTransform(errorOut, out, buf);
  }
  if (width == src.width && height == src.height) {
    // Not a resize. Verbatim, so a no-op cannot cost an ulp.
    *out = src;
    if (report) {
      report->exact = ExactRemap::Identity;
      report->reconstructionPasses = 0;
    }
    return true;
  }

  const Mat3 scale = transformScale(static_cast<float>(width) / static_cast<float>(src.width),
                                    static_cast<float>(height) / static_cast<float>(src.height));
  return transformImage(src, scale, width, height, params, out, report, errorOut);
}

// ==========================================================================
// The tile-store bridge
// ==========================================================================

TransformImage imageFromTileStore(const TileStore& store, int32_t originX, int32_t originY,
                                  uint32_t width, uint32_t height) {
  TransformImage img;
  img.width = width;
  img.height = height;
  img.px.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0.0f);
  if (width == 0 || height == 0) return img;

  // Walked tile-first so each tile is looked up once rather than once per
  // texel: a per-texel `find()` on a 128x128 region is 16 384 hash lookups for
  // one tile's worth of pixels.
  const int32_t x1 = originX + static_cast<int32_t>(width);
  const int32_t y1 = originY + static_cast<int32_t>(height);
  const int32_t tx0 = floorDiv(originX, kTileSize);
  const int32_t ty0 = floorDiv(originY, kTileSize);
  const int32_t tx1 = floorDiv(x1 - 1, kTileSize);
  const int32_t ty1 = floorDiv(y1 - 1, kTileSize);

  for (int32_t ty = ty0; ty <= ty1; ++ty) {
    for (int32_t tx = tx0; tx <= tx1; ++tx) {
      // An absent tile is left as the transparent black the buffer was already
      // filled with -- the store's own implicit content for a tile nothing has
      // written (core/TileStore.hpp), so "never touched" and "written
      // transparent" read identically here, as they must.
      const Tile* tile = store.find(TileCoord{tx, ty});
      if (tile == nullptr) continue;
      const PixelCoord org = tileOrigin(TileCoord{tx, ty});
      const int32_t bx0 = std::max(originX, org.x);
      const int32_t by0 = std::max(originY, org.y);
      const int32_t bx1 = std::min(x1, org.x + kTileSize);
      const int32_t by1 = std::min(y1, org.y + kTileSize);
      for (int32_t py = by0; py < by1; ++py) {
        for (int32_t px = bx0; px < bx1; ++px) {
          const std::array<float, 4> rgba =
              tile->readPixel(PixelCoord{px - org.x, py - org.y});
          float* d = img.px.data() +
                     (static_cast<size_t>(py - originY) * width +
                      static_cast<size_t>(px - originX)) * 4u;
          for (int c = 0; c < 4; ++c) d[c] = rgba[c];
        }
      }
    }
  }
  return img;
}

void tileStoreFromImage(const TransformImage& img, int32_t originX, int32_t originY,
                        TileStore* store) {
  if (store == nullptr || !img.valid()) return;
  const int32_t x1 = originX + static_cast<int32_t>(img.width);
  const int32_t y1 = originY + static_cast<int32_t>(img.height);
  const int32_t tx0 = floorDiv(originX, kTileSize);
  const int32_t ty0 = floorDiv(originY, kTileSize);
  const int32_t tx1 = floorDiv(x1 - 1, kTileSize);
  const int32_t ty1 = floorDiv(y1 - 1, kTileSize);

  for (int32_t ty = ty0; ty <= ty1; ++ty) {
    for (int32_t tx = tx0; tx <= tx1; ++tx) {
      const PixelCoord org = tileOrigin(TileCoord{tx, ty});
      const int32_t bx0 = std::max(originX, org.x);
      const int32_t by0 = std::max(originY, org.y);
      const int32_t bx1 = std::min(x1, org.x + kTileSize);
      const int32_t by1 = std::min(y1, org.y + kTileSize);

      // Two passes over the region rather than one, and the first is the point:
      // a transform *empties* tiles as often as it fills them (rotate a layer
      // and the corners it used to occupy become transparent), and calling
      // getOrCreate() for every overlapped tile would allocate 128 KiB for
      // every one of those. So a tile that has nothing to say and does not
      // already exist is skipped entirely. A tile that *does* already exist is
      // written even when the region is all transparent, because clearing what
      // moved away is the whole job -- skipping it there would leave the
      // layer's old pixels behind as a ghost.
      bool anyContent = false;
      for (int32_t py = by0; py < by1 && !anyContent; ++py) {
        for (int32_t px = bx0; px < bx1; ++px) {
          const float* s = img.px.data() +
                           (static_cast<size_t>(py - originY) * img.width +
                            static_cast<size_t>(px - originX)) * 4u;
          if (s[0] != 0.0f || s[1] != 0.0f || s[2] != 0.0f || s[3] != 0.0f) {
            anyContent = true;
            break;
          }
        }
      }
      if (!anyContent && store->find(TileCoord{tx, ty}) == nullptr) continue;

      Tile& tile = store->getOrCreate(TileCoord{tx, ty});
      for (int32_t py = by0; py < by1; ++py) {
        for (int32_t px = bx0; px < bx1; ++px) {
          const float* s = img.px.data() +
                           (static_cast<size_t>(py - originY) * img.width +
                            static_cast<size_t>(px - originX)) * 4u;
          tile.writePixel(PixelCoord{px - org.x, py - org.y},
                          std::array<float, 4>{s[0], s[1], s[2], s[3]});
        }
      }
    }
  }
}

}  // namespace np
