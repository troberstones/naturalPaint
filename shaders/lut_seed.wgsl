// color/LutBake seed pass (PLAN.md Phase 3 step 4, ADR-0004). Initializes a
// kLutSize^3 grid with the shaper-domain coordinate each texel represents:
// texel (x,y,z) IS shaper-domain coordinate
// ((x+0.5)/kLutSize, (y+0.5)/kLutSize, (z+0.5)/kLutSize) by the LUT's own
// indexing convention -- no shaperDecode/shaperEncode round trip needed
// here, only in the per-op passes that follow (lut_op_*.wgsl). Every bake,
// whether its run has 1 op or 12, starts from this same seed.
//
// Dispatched as dispatchWorkgroups(8, 8, 8) against @workgroup_size(4,4,4):
// 8*4 == 32 == kLutSize exactly in every dimension, so every invocation is
// in-bounds -- no remainder, no clamp, unlike the 2D solver's ceiling-
// division groups() helper (sim/PaintSim.cpp).

const kLutSize: f32 = 32.0;

@group(0) @binding(0) var lutDst : texture_storage_3d<rgba16float, write>;

@compute @workgroup_size(4, 4, 4)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec3<i32>(gid);
  let coord = (vec3<f32>(gid) + vec3<f32>(0.5)) / kLutSize;
  textureStore(lutDst, p, vec4<f32>(coord, 1.0));
}
