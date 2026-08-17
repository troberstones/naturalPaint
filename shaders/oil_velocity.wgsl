// Oil, stage 2 (IMPaSTo §4.1.2): paint velocity.
//
//   v = vb + vp,   vp = -c * grad(penetration)
//
// vb is the brush's tangential motion, vp the "squish" that pushes paint out
// from under the pressing brush. The paper notes vp approximates the pressure
// term of Navier-Stokes with penetration standing in for internal pressure.
// Both components are clamped to +/-1 cell/step to satisfy CFL.
//#include "include/common.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc : texture_2d<f32>;
@group(0) @binding(2) var waterDst : texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }
  let res = P.resolution;

  var water = textureLoad(waterSrc, p, 0);
  let contact = water.w;

  if (contact < 1e-4) {
    // Paint only moves where the brush is touching it. Oil does not flow on its
    // own the way a wash does.
    water.x = 0.0;
    water.y = 0.0;
    textureStore(waterDst, p, water);
    return;
  }

  let cL = textureLoad(waterSrc, clampCoord(p + vec2<i32>(-1, 0), res), 0).w;
  let cR = textureLoad(waterSrc, clampCoord(p + vec2<i32>( 1, 0), res), 0).w;
  let cD = textureLoad(waterSrc, clampCoord(p + vec2<i32>(0, -1), res), 0).w;
  let cU = textureLoad(waterSrc, clampCoord(p + vec2<i32>(0,  1), res), 0).w;
  let gradP = vec2<f32>(cR - cL, cU - cD) * 0.5;

  let vb = P.brushB - P.brushA;          // cells per frame
  let vp = -P.oilPressure * gradP;

  let vel = clamp(vb + vp, vec2<f32>(-1.0), vec2<f32>(1.0));
  water.x = vel.x;
  water.y = vel.y;
  textureStore(waterDst, p, water);
}
