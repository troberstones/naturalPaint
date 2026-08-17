// Oil, stage 4/5 canvas side (IMPaSTo Algorithm 1).
//
// Transfer at a cell is unidirectional: either the brush is depositing or the
// canvas is loading, never both. Direction follows whichever holds more paint.
// The base amount is cut off smoothly as the two equalise (otherwise the pair
// oscillates), and again when the brush is barely moving — without that the
// brush oozes paint while held still. Constants are the paper's.
//#include "include/common.wgsl"
//#include "include/donor.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc    : texture_2d<f32>;
@group(0) @binding(2) var pigCSrc     : texture_2d<f32>;
@group(0) @binding(3) var pigRSrc     : texture_2d<f32>;
@group(0) @binding(4) var brushVolSrc : texture_2d<f32>;
@group(0) @binding(5) var brushCSrc   : texture_2d<f32>;
@group(0) @binding(6) var brushRSrc   : texture_2d<f32>;
@group(0) @binding(7) var waterDst    : texture_storage_2d<rgba16float, write>;
@group(0) @binding(8) var pigCDst     : texture_storage_2d<rgba32float, write>;
@group(0) @binding(9) var pigRDst     : texture_storage_2d<rgba32float, write>;

const EQUAL_PAINT_CUTOFF : f32 = 0.0333333;
const BRUSH_GRID : f32 = 64.0;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }

  var water = textureLoad(waterSrc, p, 0);
  var pigC = textureLoad(pigCSrc, p, 0);
  var pigR = textureLoad(pigRSrc, p, 0);

  var bg = vec2<i32>(0, 0);
  let inFootprint = brushGridCoord(vec2<f32>(p), P.brushA, P.brushB,
                                   P.brushRadius, BRUSH_GRID, &bg);

  if (P.brushActive == 0u || water.w < 1e-4 || !inFootprint) {
    textureStore(waterDst, p, water);
    textureStore(pigCDst, p, pigC);
    textureStore(pigRDst, p, pigR);
    return;
  }

  let ab = textureLoad(brushVolSrc, bg, 0).x;   // paint on the brush cell
  let ac = water.z * water.w;                   // volume the brush penetrated

  let paintDiff = ab - ac;
  let equalCutoff = clamp(abs(paintDiff) / EQUAL_PAINT_CUTOFF, 0.0, 1.0);
  let velocityCutoff = smoothstep(0.2, 0.3, length(P.brushB - P.brushA));

  // Scale by how hard the brush is actually pressing here. Without this the
  // transfer is uniform across the footprint and stops dead at its rim, so each
  // frame stamps a hard-edged capsule and the impasto lighting turns the
  // overlaps into regular arcs down the stroke.
  let press = clamp(water.w / max(P.penetration, 1e-3), 0.0, 1.0);

  var amt = select(ac, ab, paintDiff > 0.0);
  amt = amt * P.xferFraction * equalCutoff * velocityCutoff * press;
  amt = clamp(amt, 0.0, P.maxXfer);

  if (paintDiff > 0.0) {
    // Brush -> canvas. Pigment rides along, premultiplied by volume, so the
    // arriving colour mixes with what is already there under Kubelka-Munk.
    let bVol = max(ab, 1e-6);
    let frac = amt / bVol;
    water.z = water.z + amt;
    pigC = pigC + textureLoad(brushCSrc, bg, 0) * frac;
    pigR = pigR + textureLoad(brushRSrc, bg, 0) * frac;
  } else {
    // Canvas -> brush. Handled on the brush side too; here we only remove.
    let cVol = max(water.z, 1e-6);
    let frac = clamp(amt / cVol, 0.0, 1.0);
    water.z = max(water.z - amt, 0.0);
    pigC = pigC * (1.0 - frac);
    pigR = pigR * (1.0 - frac);
  }

  textureStore(waterDst, p, water);
  textureStore(pigCDst, p, max(pigC, vec4<f32>(0.0)));
  textureStore(pigRDst, p, pigR);
}
