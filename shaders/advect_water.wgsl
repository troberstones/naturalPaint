// Transport of the shallow-water layer itself.
//
// Without this the velocity field pushes pigment around inside a wet region that
// never moves, so a tilted board only redistributes a mark instead of running
// it: the centre of mass shifts downhill but the blob keeps its outline. Water
// has to actually flow for a run or a drip to exist.
//
// Uses the same conservative donor-cell scheme as the pigment (IMPaSTo §4.1.1),
// so water is neither created nor destroyed by the transport itself. Velocity is
// left alone here — update_velocities already self-advects it.
//#include "include/common.wgsl"
//#include "include/donor.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc : texture_2d<f32>;
@group(0) @binding(2) var waterDst : texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }
  let res = P.resolution;

  let here = textureLoad(waterSrc, p, 0);

  var depth = 0.0;
  var mask = 0.0;
  for (var oy = -1; oy <= 1; oy = oy + 1) {
    for (var ox = -1; ox <= 1; ox = ox + 1) {
      let n = p + vec2<i32>(ox, oy);
      if (!inBounds(n, res)) { continue; }
      let wn = textureLoad(waterSrc, n, 0);
      // Dry donors hold what they have; only wet cells flow.
      var vn = vec2<f32>(0.0);
      if (wn.w >= 0.01) { vn = wn.xy; }
      let w = donorWeight(vn, vec2<f32>(f32(-ox), f32(-oy)));
      if (w > 0.0) {
        depth = depth + w * wn.z;
        mask = mask + w * wn.w;
      }
    }
  }

  // Water arriving on dry paper wets it. Transporting the mask alone would
  // smear it thin at the leading edge and the front would fade out rather than
  // advance, so re-assert it wherever there is meaningful depth.
  // Smooth, not a step: a hard threshold crenellates the leading edge into
  // blocky teeth as cells flick between wet and dry.
  mask = max(mask, smoothstep(0.004, 0.05, depth));

  textureStore(waterDst, p, vec4<f32>(here.xy, max(depth, 0.0), clamp(mask, 0.0, 1.0)));
}
