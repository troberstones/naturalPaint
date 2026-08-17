// Curtis §4.2 TransferPigment — exchange between pigment suspended in the water
// and pigment deposited on the paper.
//
//   density     rho   heavy pigments drop out of suspension faster
//   staining    omega resists being lifted back up (a stain won't lift)
//   granulation gamma affinity for the paper's valleys; this is what makes
//                     granulating pigments pool into the texture of the sheet
//
// Because both layers store latent-times-mass, moving a fraction of the mass
// moves the same fraction of every channel, and the pigment identity survives.
//#include "include/common.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc : texture_2d<f32>;
@group(0) @binding(2) var paperTex : texture_2d<f32>;
@group(0) @binding(3) var pigCSrc  : texture_2d<f32>;
@group(0) @binding(4) var pigRSrc  : texture_2d<f32>;
@group(0) @binding(5) var depCSrc  : texture_2d<f32>;
@group(0) @binding(6) var depRSrc  : texture_2d<f32>;
@group(0) @binding(7) var pigCDst  : texture_storage_2d<rgba32float, write>;
@group(0) @binding(8) var pigRDst  : texture_storage_2d<rgba32float, write>;
@group(0) @binding(9) var depCDst  : texture_storage_2d<rgba32float, write>;
@group(0) @binding(10) var depRDst : texture_storage_2d<rgba32float, write>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }

  var pigC = textureLoad(pigCSrc, p, 0);
  var pigR = textureLoad(pigRSrc, p, 0);
  var depC = textureLoad(depCSrc, p, 0);
  var depR = textureLoad(depRSrc, p, 0);

  let M = textureLoad(waterSrc, p, 0).w;
  let h = textureLoad(paperTex, p, 0).x;

  var fDown : f32;
  var fUp : f32;

  if (M < 0.01) {
    // Dry paper: whatever is still in suspension has nowhere to go but down.
    fDown = 1.0;
    fUp = 0.0;
  } else {
    // RATE_SCALE reconciles Curtis's per-step constants with this solver's step
    // count. At 60 fps with 2 substeps we take 120 steps/second; unscaled, the
    // lifting term reaches ~0.92 per step for a low-staining pigment, so
    // everything settled re-suspends immediately, rides the outward flow and
    // strands at the rim, leaving a hollow wash. Scaling makes deposition
    // compete with transport on a sane timescale.
    let RATE_SCALE = 0.06;
    fDown = clamp((1.0 - h * P.granulation) * P.density * P.dt * RATE_SCALE, 0.0, 1.0);
    fUp   = clamp((1.0 + (h - 1.0) * P.granulation) * P.density
                  / max(P.staining, 1e-3) * P.dt * RATE_SCALE, 0.0, 1.0);
  }

  let down = pigC * fDown;
  let up   = depC * fUp;
  let downR = pigR * fDown;
  let upR   = depR * fUp;

  pigC = pigC - down + up;
  depC = depC + down - up;
  pigR = pigR - downR + upR;
  depR = depR + downR - upR;

  textureStore(pigCDst, p, max(pigC, vec4<f32>(0.0)));
  textureStore(pigRDst, p, pigR);
  textureStore(depCDst, p, max(depC, vec4<f32>(0.0)));
  textureStore(depRDst, p, depR);
}
