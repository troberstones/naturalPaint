// Oil, stage 4/5 brush side. Dispatched over the 64x64 brush paint grid, not the
// canvas, so that many canvas texels mapping to one brush cell cannot race.
//
// This is the half of IMPaSTo's transfer that makes a brush behave like a brush:
// it runs dry as paint leaves it, and it picks colour up off the canvas so
// dragging through wet paint dirties the load.
//#include "include/common.wgsl"
//#include "include/donor.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc    : texture_2d<f32>;
@group(0) @binding(2) var pigCSrc     : texture_2d<f32>;
@group(0) @binding(3) var pigRSrc     : texture_2d<f32>;
@group(0) @binding(4) var brushVolSrc : texture_2d<f32>;
@group(0) @binding(5) var brushCSrc   : texture_2d<f32>;
@group(0) @binding(6) var brushRSrc   : texture_2d<f32>;
@group(0) @binding(7) var brushVolDst : texture_storage_2d<rgba32float, write>;
@group(0) @binding(8) var brushCDst   : texture_storage_2d<rgba32float, write>;
@group(0) @binding(9) var brushRDst   : texture_storage_2d<rgba32float, write>;

const EQUAL_PAINT_CUTOFF : f32 = 0.0333333;
const BRUSH_GRID : f32 = 64.0;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let b = vec2<i32>(gid.xy);
  if (b.x >= i32(BRUSH_GRID) || b.y >= i32(BRUSH_GRID)) { return; }

  var vol = textureLoad(brushVolSrc, b, 0);
  var bC = textureLoad(brushCSrc, b, 0);
  var bR = textureLoad(brushRSrc, b, 0);

  // Stroke start: recharge the brush with the selected paint.
  if (P.brushReload != 0u) {
    let uv = (vec2<f32>(b) + vec2<f32>(0.5)) / BRUSH_GRID * 2.0 - vec2<f32>(1.0);
    let load = select(0.0, P.brushLoad, dot(uv, uv) <= 1.0);
    textureStore(brushVolDst, b, vec4<f32>(load, 0.0, 0.0, 0.0));
    // .w is pigment MASS, not a latent component. brushLatentC.w is zero, so
    // storing the vec4 wholesale leaves mass at zero, latentFromMass then
    // divides by ~0 and every stroke renders as a saturated primary.
    textureStore(brushCDst, b, vec4<f32>(P.brushLatentC.xyz * load, load));
    textureStore(brushRDst, b, vec4<f32>(P.brushLatentR.xyz * load, 0.0));
    return;
  }

  if (P.brushActive == 0u) {
    textureStore(brushVolDst, b, vol);
    textureStore(brushCDst, b, bC);
    textureStore(brushRDst, b, bR);
    return;
  }

  // Inverse of brushGridCoord: the canvas texel under the centre of this brush
  // cell. One sample is enough — at usable radii a brush cell covers ~1 texel.
  let uv = (vec2<f32>(b) + vec2<f32>(0.5)) / BRUSH_GRID * 2.0 - vec2<f32>(1.0);
  let canvasPos = P.brushB + uv * P.brushRadius;
  let c = vec2<i32>(canvasPos);
  if (dot(uv, uv) > 1.0 || !inBounds(c, P.resolution)) {
    textureStore(brushVolDst, b, vol);
    textureStore(brushCDst, b, bC);
    textureStore(brushRDst, b, bR);
    return;
  }

  let water = textureLoad(waterSrc, c, 0);
  let ab = vol.x;
  let ac = water.z * water.w;

  let paintDiff = ab - ac;
  let equalCutoff = clamp(abs(paintDiff) / EQUAL_PAINT_CUTOFF, 0.0, 1.0);
  let velocityCutoff = smoothstep(0.2, 0.3, length(P.brushB - P.brushA));

  let press = clamp(water.w / max(P.penetration, 1e-3), 0.0, 1.0);

  var amt = select(ac, ab, paintDiff > 0.0);
  amt = amt * P.xferFraction * equalCutoff * velocityCutoff * press;
  amt = clamp(amt, 0.0, P.maxXfer);

  if (paintDiff > 0.0) {
    // Depositing: the brush loses paint and its pigment load shrinks with it.
    let frac = clamp(amt / max(ab, 1e-6), 0.0, 1.0);
    vol.x = max(ab - amt, 0.0);
    bC = bC * (1.0 - frac);
    bR = bR * (1.0 - frac);
  } else {
    // Loading: pick colour up off the canvas.
    let cVol = max(water.z, 1e-6);
    let frac = clamp(amt / cVol, 0.0, 1.0);
    vol.x = ab + amt;
    bC = bC + textureLoad(pigCSrc, c, 0) * frac;
    bR = bR + textureLoad(pigRSrc, c, 0) * frac;
  }

  textureStore(brushVolDst, b, vol);
  textureStore(brushCDst, b, max(bC, vec4<f32>(0.0)));
  textureStore(brushRDst, b, bR);
}
