// Grayscale preview (PLAN.md Phase 2 step 11, PRD Q3): a dedicated per-pixel
// desaturate pass over the canvas, not an ImGui tint multiply -- a flat tint
// can't collapse independent R/G/B into their luminance average, and this
// toggle exists so a painter can actually judge values against each other.
//
// Reads the canvas 1:1 (same resolution as its source, no sampler needed --
// see PaintSim::updateGrayscalePreview()) and writes into its own separate
// texture; the source is never written to, which is what keeps this a view-
// only preview rather than something that could ever mutate the document.
//
// Same oversized-triangle full-screen blit shape as composite.wgsl.
@group(0) @binding(0) var srcTex : texture_2d<f32>;

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
  let c = textureLoad(srcTex, px, 0);
  // Rec. 709 luma weights. The canvas is already resolved to display RGB by
  // composite.wgsl, so this is a plain display-space desaturate -- a value
  // check, not a colourimetrically exact one; PRD Q3 asks for the former.
  let l = dot(c.rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
  return vec4<f32>(l, l, l, c.a);
}
