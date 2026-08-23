// Oil, stage 1 (IMPaSTo Fig. 5): brush-canvas contact.
//
// The 3D brush mesh of the paper is reduced to a swept disc here, so the
// "penetration" of the brush into the paint height field is just the footprint
// falloff scaled by how hard the stylus is pressing. Everything downstream —
// the pressure-driven squish velocity and the transfer rules — only needs this
// scalar, so the simplification costs less than it looks.
//#include "include/common.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc : texture_2d<f32>;
@group(0) @binding(2) var waterDst : texture_storage_2d<rgba16float, write>;
// Selection coverage, r8unorm, canvas-sized. Always bound; see splat.wgsl.
@group(0) @binding(3) var selection : texture_2d<f32>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }

  var water = textureLoad(waterSrc, p, 0);  // (u, v, volume, contact)

  var contact = 0.0;
  if (P.brushActive != 0u) {
    let d = distToSegment(vec2<f32>(p), P.brushA, P.brushB);
    let edge = mix(P.brushRadius, P.brushRadius * 0.15, P.brushHardness);
    // PRD E1: every deposit respects the active selection -- oil included. This
    // is the brush CONTACT term rather than a pigment quantity, so gating it
    // here keeps an unselected texel from being pressed into at all, which is
    // the oil equivalent of depositing nothing.
    let coverage = textureLoad(selection, p, 0).r;
    let falloff = (1.0 - smoothstep(P.brushRadius - edge, P.brushRadius, d)) * coverage;
    // ac in Algorithm 1 is the volume *penetrated*, not the whole cell, so a
    // light touch can still lay paint onto a thickly covered canvas.
    contact = P.penetration * falloff;
  }

  water.w = contact;
  textureStore(waterDst, p, water);
}
