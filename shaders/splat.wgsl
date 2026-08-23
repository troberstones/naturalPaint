// Brush deposition. Wets the paper along the stroke segment and injects pigment
// into the suspended layer. Curtis §4 assumes a wet-area mask M is painted by
// the user; this is that step.
//#include "include/common.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc : texture_2d<f32>;
@group(0) @binding(2) var pigCSrc  : texture_2d<f32>;
@group(0) @binding(3) var pigRSrc  : texture_2d<f32>;
@group(0) @binding(4) var waterDst : texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var pigCDst  : texture_storage_2d<rgba32float, write>;
@group(0) @binding(6) var pigRDst  : texture_storage_2d<rgba32float, write>;
// Selection coverage, r8unorm, canvas-sized. **Always bound**: when no
// selection is active PaintSim fills it with 255, so this shader has no
// branch and no special case (core/SelectionMask.hpp: absent means 1.0).
@group(0) @binding(7) var selection : texture_2d<f32>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }

  var water = textureLoad(waterSrc, p, 0);
  var pigC  = textureLoad(pigCSrc, p, 0);
  var pigR  = textureLoad(pigRSrc, p, 0);

  if (P.brushActive != 0u) {
    let d = distToSegment(vec2<f32>(p), P.brushA, P.brushB);
    // Soft edge; hardness 1 gives a crisp disc, 0 a wide falloff.
    let edge = mix(P.brushRadius, P.brushRadius * 0.15, P.brushHardness);
    // PRD E1: every deposit respects the active selection. Multiplying the
    // falloff gates water and pigment together with one term, and the
    // `falloff > 0.0` test below then skips an unselected texel for free.
    // Coverage is WEIGHTED, not thresholded -- a half-selected texel takes
    // half a dab, which is what makes a feathered selection feather.
    let coverage = textureLoad(selection, p, 0).r;
    let falloff = (1.0 - smoothstep(P.brushRadius - edge, P.brushRadius, d)) * coverage;

    if (falloff > 0.0) {
      // Wet-area mask M and standing water.
      water.w = max(water.w, falloff);
      // Capped: surface tension limits film depth. Deposition is now per-dab
      // (ADR-0003) rather than per-frame, so there is no dwell-time term left
      // to hollow a wash into its own rim -- the cap just stops one dab from
      // overfilling a cell outright.
      water.z = min(water.z + P.brushWater * falloff, P.maxFilm);

      // Pigment is stored premultiplied by mass so linear transport mixes it
      // correctly in latent space. A dab deposits a fixed quantity -- not
      // scaled by dt (ADR-0003): deposition is a quantity per unit of brush
      // travel, not a rate.
      let m = P.brushPigment * falloff;
      pigC = vec4<f32>(pigC.xyz + P.brushLatentC.xyz * m, pigC.w + m);
      pigR = vec4<f32>(pigR.xyz + P.brushLatentR.xyz * m, 0.0);
    }
  }

  textureStore(waterDst, p, water);
  textureStore(pigCDst, p, pigC);
  textureStore(pigRDst, p, pigR);
}
