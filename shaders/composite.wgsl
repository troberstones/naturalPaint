// Final resolve: pigment fields -> screen, branching on the active model.
//
// Latents are only converted to RGB here, at the very end. Everything upstream
// stayed in pigment latent space (Mixbox's, or the two-constant Kubelka-Munk
// fallback's -- see include/pigment_basis.wgsl and gfx/ShaderLoader.cpp for
// which one NP_USE_MIXBOX picks), so a wet blue stroke dragged through a wet
// yellow one gives green rather than the grey an RGB solver would produce. That
// holds for all three models, since all three transport latent-times-mass.
//#include "include/common.wgsl"
//#include "include/pigment_basis.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var depCTex : texture_2d<f32>;
@group(0) @binding(2) var depRTex : texture_2d<f32>;
@group(0) @binding(3) var pigCTex : texture_2d<f32>;
@group(0) @binding(4) var pigRTex : texture_2d<f32>;
@group(0) @binding(5) var waterTex : texture_2d<f32>;
@group(0) @binding(6) var satTex : texture_2d<f32>;
@group(0) @binding(7) var paperTex : texture_2d<f32>;

struct VSOut {
  @builtin(position) pos : vec4<f32>,
  @location(0) uv : vec2<f32>,
};

@vertex
fn vs(@builtin(vertex_index) vi : u32) -> VSOut {
  // Oversized triangle covering the viewport.
  var p = array<vec2<f32>, 3>(
    vec2<f32>(-1.0, -3.0), vec2<f32>(-1.0, 1.0), vec2<f32>(3.0, 1.0)
  );
  var out : VSOut;
  out.pos = vec4<f32>(p[vi], 0.0, 1.0);
  out.uv = vec2<f32>(p[vi].x * 0.5 + 0.5, 0.5 - p[vi].y * 0.5);
  return out;
}

fn substrate(fibre: f32, mode: u32) -> vec3<f32> {
  if (mode == MODE_OIL) {
    // Primed linen rather than paper: cooler and a shade darker.
    return vec3<f32>(0.878, 0.867, 0.838) * (0.955 + 0.045 * fibre);
  }
  if (mode == MODE_INK) {
    // Eastern paper is warmer and more absorbent-looking than cotton rag.
    return vec3<f32>(0.957, 0.941, 0.898) * (0.950 + 0.050 * fibre);
  }
  return vec3<f32>(0.97, 0.965, 0.945) * (0.955 + 0.045 * fibre);
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4<f32> {
  let px = vec2<i32>(in.uv * vec2<f32>(f32(P.resolution.x), f32(P.resolution.y)));
  let p = clampCoord(px, P.resolution);
  let res = P.resolution;

  let depC = textureLoad(depCTex, p, 0);
  let depR = textureLoad(depRTex, p, 0);
  let pigC = textureLoad(pigCTex, p, 0);
  let pigR = textureLoad(pigRTex, p, 0);
  let water = textureLoad(waterTex, p, 0);
  let paper = textureLoad(paperTex, p, 0);
  let fibre = paper.x;

  let base = substrate(fibre, P.mode);
  var rgb = base;

  // ---------------------------------------------------------------- OIL
  if (P.mode == MODE_OIL) {
    // water.z is the paint volume: an actual height field, so it both colours
    // the surface and lights it.
    let vol = water.z;
    if (vol > 1e-4) {
      let latent = pigmentLatentFromMass(pigC, pigR);
      let paint = pigmentLatentToRgb(latent);

      // Oil is opaque; a thin scrape still lets the ground show through.
      let cover = 1.0 - exp(-14.0 * vol);
      rgb = mix(base, paint, cover);

      // Impasto: shade the height field. Ridges left by the bristles are the
      // whole point of the mode, and they only read if they catch the light.
      let hL = textureLoad(waterTex, clampCoord(p + vec2<i32>(-1, 0), res), 0).z;
      let hR = textureLoad(waterTex, clampCoord(p + vec2<i32>( 1, 0), res), 0).z;
      let hD = textureLoad(waterTex, clampCoord(p + vec2<i32>(0, -1), res), 0).z;
      let hU = textureLoad(waterTex, clampCoord(p + vec2<i32>(0,  1), res), 0).z;

      let scale = 9.0;
      let n = normalize(vec3<f32>(-(hR - hL) * scale, -(hU - hD) * scale, 1.0));
      let lightDir = normalize(vec3<f32>(-0.45, 0.62, 0.65));

      let diffuse = clamp(dot(n, lightDir), 0.0, 1.0);
      let spec = pow(clamp(dot(reflect(-lightDir, n), vec3<f32>(0.0, 0.0, 1.0)),
                           0.0, 1.0), 24.0);

      let lit = mix(1.0, 0.55 + 0.75 * diffuse, P.impastoLight * cover);
      rgb = rgb * lit + vec3<f32>(0.9, 0.89, 0.86) * spec * 0.22 * P.impastoLight * cover;
    }
    return vec4<f32>(clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
  }

  // ---------------------------------------------------------------- INK
  if (P.mode == MODE_INK) {
    // Ink that has fixed to the fibres reads full strength; ink still moving in
    // the water is paler and bleeds.
    let sumC = depC + pigC * 0.55;
    let sumR = depR + pigR * 0.55;
    let mass = sumC.w;
    if (mass > 1e-4) {
      let latent = pigmentLatentFromMass(sumC, sumR);
      let ink = pigmentLatentToRgb(latent);
      // Sumi tone builds steeply, so a single loaded touch can go near-black
      // while the tail of a dry stroke stays a pale grey.
      let opacity = 1.0 - exp(-4.2 * mass);
      rgb = mix(base, ink * (0.90 + 0.10 * fibre), opacity);
    }
    // Damp paper darkens slightly before it dries.
    let wet = clamp(water.z, 0.0, 1.0);
    rgb = rgb * (1.0 - 0.05 * wet);
    return vec4<f32>(clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
  }

  // ---------------------------------------------------------- WATERCOLOUR
  let sat = textureLoad(satTex, p, 0).x;

  // Suspended pigment reads slightly weaker than settled pigment — it is
  // sitting in the water film rather than lying on the fibres.
  let sumC = depC + pigC * 0.75;
  let sumR = depR + pigR * 0.75;
  let mass = sumC.w;

  if (mass > 1e-4) {
    let latent = pigmentLatentFromMass(sumC, sumR);
    let pigmentRgb = pigmentLatentToRgb(latent);

    // Beer-Lambert style build-up: repeated glazes darken but never quite
    // reach the raw pigment colour, which is how layered washes behave.
    let opacity = 1.0 - exp(-2.6 * mass);
    rgb = mix(base, pigmentRgb * (0.93 + 0.07 * fibre), opacity);
  }

  // Wet surface reads darker and slightly glossier until it dries.
  let wetness = clamp(max(water.w, sat * 1.5), 0.0, 1.0);
  rgb = rgb * (1.0 - 0.06 * wetness);
  rgb = rgb + vec3<f32>(0.02, 0.025, 0.035) * wetness * clamp(water.z * 2.0, 0.0, 1.0);

  return vec4<f32>(clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}
