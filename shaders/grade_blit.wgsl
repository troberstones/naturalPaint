// Apply pass (PLAN.md Phase 3 step 6, "shaper -> 3-D LUT fetch -> un-
// shape"): grades sim::PaintSim's live canvas_ through a baked
// color::Lut3D (color/LutBake), one linear-filtering 3-D texture sample
// per pixel -- O(1) at draw time regardless of how many ops are baked
// into the LUT, per Phase 3's own Verify criterion ("twelve stacked grade
// ops cost the same as one at draw time"). See
// sim::PaintSim::updateGradePreview()'s own doc comment for the CPU-side
// rebake-on-version-change bookkeeping this shader has no part in -- this
// file only ever draws with whatever LUT it is handed.
//
// Reads the canvas 1:1 (same resolution as its source, exact texel fetch,
// no filtering -- same as shaders/grayscale_blit.wgsl, since src and dst
// are the same resolution) and writes into its own separate texture;
// canvasView_ is never written to here, matching every other preview
// blit in this codebase (grayscale_blit.wgsl's own precedent).
//
// *** Domain contract -- deliberate and narrow, read before touching ***
// color::Shaper / color::LutBake's design otherwise assumes its input is
// scene-linear HDR light (ADR-0004: "linear -> shaper (log encode) ->
// 3-D LUT -> linear"). composite.wgsl's rgb output is NOT that: every one
// of its three return paths ends
// `vec4<f32>(clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0)` -- an
// already-clamped-to-[0,1], stylized, display-ish paint-appearance value,
// not scene-linear light. This shader still follows step 6's literal
// wording ("shaper -> 3-D LUT fetch -> un-shape") as a numerical contract
// regardless: the clamped [0,1] canvas RGB is treated as shaperEncode's
// "linear" input, exactly as if it genuinely were scene-linear. A direct,
// accepted consequence, stated plainly rather than left implicit: since
// canvas RGB never exceeds 1.0, shaperEncode(1.0) ~= 0.5547945 caps the
// shaper-domain coordinate this pass will ever sample at -- only the
// lower ~55% of the LUT's [0,1]^3 domain is ever actually reached by this
// pass. This is a documented, accepted consequence of grading the SDR
// live-painting canvas rather than a linear scene-referred core::Document
// (no live-canvas-to-Document bridge exists yet -- see AppState.hpp's
// CanvasView::grade / AppState::opStack comments), not a bug and not
// something this shader works around.
//#include "include/shaper.wgsl"

@group(0) @binding(0) var srcTex : texture_2d<f32>;
@group(0) @binding(1) var lutTex : texture_3d<f32>;
// Linear-filtering (trilinear across the 3-D grid) -- unlike color/
// LutBake.cpp's own bake-time kernels, which only ever textureLoad exact
// texels, this is the first sampler-filtered 3-D texture read anywhere in
// this codebase. Trilinear interpolation across the baked 32^3 lattice is
// what turns a coarse grid into a smooth per-pixel grade.
@group(0) @binding(2) var lutSamp : sampler;

struct VSOut {
  @builtin(position) pos : vec4<f32>,
  @location(0) uv : vec2<f32>,
};

@vertex
fn vs(@builtin(vertex_index) vi : u32) -> VSOut {
  var p = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -3.0), vec2<f32>(-1.0, 1.0), vec2<f32>(3.0, 1.0)
  );
  var out : VSOut;
  out.pos = vec4<f32>(p[vi], 0.0, 1.0);
  out.uv = vec2<f32>(p[vi].x * 0.5 + 0.5, 0.5 - p[vi].y * 0.5);
  return out;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4<f32> {
  let dims = textureDimensions(srcTex);
  let px = vec2<i32>(in.uv * vec2<f32>(dims));
  let src = textureLoad(srcTex, px, 0);

  // Treat the clamped-[0,1] canvas RGB as shaperEncode's "linear" input --
  // see this file's header comment for the domain-contract reasoning.
  let shaped = vec3<f32>(shaperEncode(src.r), shaperEncode(src.g), shaperEncode(src.b));
  let sampled = textureSampleLevel(lutTex, lutSamp, shaped, 0.0).rgb;
  let unshaped = vec3<f32>(shaperDecode(sampled.r), shaperDecode(sampled.g), shaperDecode(sampled.b));
  let result = clamp(unshaped, vec3<f32>(0.0), vec3<f32>(1.0));

  // composite.wgsl's alpha is always 1.0 today; passed through rather than
  // hardcoded here in case that ever changes.
  return vec4<f32>(result, src.a);
}
