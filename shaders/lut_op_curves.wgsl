// color/LutBake per-op kernel: Curves (PLAN.md Phase 3 step 4). WGSL port of
// ops/PointOps.cpp's evalCurve() -- same degenerate 0/1-point identity rule,
// same flat extrapolation outside the authored x-range, same Hermite spline
// with Catmull-Rom-style (non-uniform-x-adapted) tangents.
//
// *** This kernel does NOT follow the shaperDecode/op/shaperEncode shape
// every other lut_op_*.wgsl kernel here uses -- see color/LutBake.hpp's
// header comment for the full reasoning, summarised here: ADR-0004 curves
// are authored *in* the shaper's log domain, and the LUT texel already
// holds a shaper-domain value by the seed pass's own convention, so
// evalCurve runs directly on it with no encode/decode at all. Wrapping this
// kernel in an outer shaperDecode/shaperEncode (the way applyCurves() does
// on the CPU, for its own linear-in/linear-out contract) would decode to
// linear and re-encode straight back for no reason -- a redundant, lossy
// round trip this kernel deliberately does not pay. ***
//
// Curve control points are variable-length and don't fit a uniform buffer
// without padding every vec2<f32> out to 16 bytes (uniform address space's
// array-stride-multiple-of-16 rule) -- a read-only *storage* buffer has no
// such requirement (array<vec2<f32>> packs at its natural 8-byte stride),
// so that's what this kernel binds instead of the small uniform buffer
// every other op kernel here uses. This is Curves' one exception; see
// color/LutBake.hpp for why it's narrow and easy to justify rather than a
// precedent for a general storage-buffer-of-everything interpreter.
//#include "include/shaper.wgsl"

// Up to kMaxCurvePointsPerChannel (color/LutBake.hpp) points per channel,
// flattened R then G then B into one array. Must match color/LutBake.cpp's
// CurvesGpuBuffer byte for byte, including kMaxCurvePointsPerChannel == 16
// baked into the array size and the per-channel base offsets below.
struct CurvesBuffer {
  countR: u32,
  countG: u32,
  countB: u32,
  _pad0: u32,
  points: array<vec2<f32>, 48>,  // [0..15]=R, [16..31]=G, [32..47]=B
}

@group(0) @binding(0) var<storage, read> curves : CurvesBuffer;
@group(0) @binding(1) var lutSrc : texture_3d<f32>;
@group(0) @binding(2) var lutDst : texture_storage_3d<rgba16float, write>;

fn secantSlope(a: vec2<f32>, b: vec2<f32>) -> f32 {
  return (b.y - a.y) / (b.x - a.x);
}

// Port of ops/PointOps.cpp's tangentAt(), operating on the channel slice
// starting at `base` (0/16/32) with `count` points.
fn tangentAt(base: u32, count: u32, i: u32) -> f32 {
  if (i == 0u) {
    return secantSlope(curves.points[base], curves.points[base + 1u]);
  }
  if (i == count - 1u) {
    return secantSlope(curves.points[base + count - 2u], curves.points[base + count - 1u]);
  }
  let mPrev = secantSlope(curves.points[base + i - 1u], curves.points[base + i]);
  let mNext = secantSlope(curves.points[base + i], curves.points[base + i + 1u]);
  return 0.5 * (mPrev + mNext);
}

// Port of ops/PointOps.cpp's evalCurve(). `base`/`count` select one
// channel's slice of the shared storage buffer. Operates directly on `x` in
// whatever domain it's given -- for this kernel's one caller (main() below)
// that's always the shaper domain, per this file's own header comment.
fn evalCurveGpu(base: u32, count: u32, x: f32) -> f32 {
  if (count < 2u) {
    return x;  // degenerate: identity, matching evalCurve()'s own rule
  }

  let first = curves.points[base];
  let last = curves.points[base + count - 1u];
  if (x <= first.x) {
    return first.y;  // flat extrapolation below the authored range
  }
  if (x >= last.x) {
    return last.y;  // flat extrapolation above the authored range
  }

  // Linear scan to the segment [i, i+1] containing x -- mirrors evalCurve()'s
  // `while (i + 1 < n && points[i + 1].x < x) ++i;` exactly. Curve point
  // counts are small (<= kMaxCurvePointsPerChannel), so this need not be a
  // binary search, matching the CPU reference's own reasoning.
  var i = 0u;
  loop {
    if (i + 1u >= count || curves.points[base + i + 1u].x >= x) {
      break;
    }
    i = i + 1u;
  }

  let p0 = curves.points[base + i];
  let p1 = curves.points[base + i + 1u];
  let dx = p1.x - p0.x;
  let t = (x - p0.x) / dx;
  let t2 = t * t;
  let t3 = t2 * t;

  let h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
  let h10 = t3 - 2.0 * t2 + t;
  let h01 = -2.0 * t3 + 3.0 * t2;
  let h11 = t3 - t2;

  let m0 = tangentAt(base, count, i);
  let m1 = tangentAt(base, count, i + 1u);

  return h00 * p0.y + h10 * dx * m0 + h01 * p1.y + h11 * dx * m1;
}

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec3<i32>(gid);
  let shapedIn = textureLoad(lutSrc, p, 0).rgb;

  let shapedOut = vec3<f32>(
      evalCurveGpu(0u, curves.countR, shapedIn.r),
      evalCurveGpu(16u, curves.countG, shapedIn.g),
      evalCurveGpu(32u, curves.countB, shapedIn.b));

  let result = clamp(shapedOut, vec3<f32>(0.0), vec3<f32>(1.0));
  textureStore(lutDst, p, vec4<f32>(result, 1.0));
}
