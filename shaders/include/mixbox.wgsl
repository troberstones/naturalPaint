// WGSL port of Mixbox 2.0 — Sochorova & Jamriska, "Practical Pigment Mixing for
// Digital Painting", SIGGRAPH Asia 2021 (TOG 40:6).
//
// Ported from third_party/mixbox/shaders/mixbox.glsl.
//   (c) 2022 Secret Weapons — Creative Commons Attribution-NonCommercial 4.0.
//   Commercial licensing: mixbox@scrtwpns.com
//
// The latent is 6 floats: c0,c1,c2 (c3 is implied as 1-c0-c1-c2) plus an RGB
// residual. Linear combinations of latents are Kubelka-Munk mixes, which is
// exactly what lets the fluid solver advect and blend pigment correctly.
//
// Only the latent -> RGB direction lives here, and it needs no LUT: it is a pure
// polynomial. The reverse direction is the one that needs Mixbox's 512x512 LUT,
// and it is only ever needed when the user picks a colour — so it lives on the
// CPU in paint/Palette.cpp and the GPU never sees the table at all.

fn mixboxEvalPolynomial(c: vec3<f32>) -> vec3<f32> {
  let c0 = c[0];
  let c1 = c[1];
  let c2 = c[2];
  let c3 = 1.0 - (c0 + c1 + c2);

  let c00 = c0 * c0;
  let c11 = c1 * c1;
  let c22 = c2 * c2;
  let c01 = c0 * c1;
  let c02 = c0 * c2;
  let c12 = c1 * c2;
  let c33 = c3 * c3;

  return (c0 * c00) * vec3<f32>( 0.07717053,  0.02826978,  0.24832992) +
         (c1 * c11) * vec3<f32>( 0.95912302,  0.80256528,  0.03561839) +
         (c2 * c22) * vec3<f32>( 0.74683774,  0.04868586,  0.00000000) +
         (c3 * c33) * vec3<f32>( 0.99518138,  0.99978149,  0.99704802) +
         (c00 * c1) * vec3<f32>( 0.04819146,  0.83363781,  0.32515377) +
         (c01 * c1) * vec3<f32>(-0.68146950,  1.46107803,  1.06980936) +
         (c00 * c2) * vec3<f32>( 0.27058419, -0.15324870,  1.98735057) +
         (c02 * c2) * vec3<f32>( 0.80478189,  0.67093710,  0.18424500) +
         (c00 * c3) * vec3<f32>(-0.35031003,  1.37855826,  3.68865000) +
         (c0 * c33) * vec3<f32>( 1.05128046,  1.97815239,  2.82989073) +
         (c11 * c2) * vec3<f32>( 3.21607125,  0.81270228,  1.03384539) +
         (c1 * c22) * vec3<f32>( 2.78893374,  0.41565549, -0.04487295) +
         (c11 * c3) * vec3<f32>( 3.02162577,  2.55374103,  0.32766114) +
         (c1 * c33) * vec3<f32>( 2.95124691,  2.81201112,  1.17578442) +
         (c22 * c3) * vec3<f32>( 2.82677043,  0.79933038,  1.81715262) +
         (c2 * c33) * vec3<f32>( 2.99691099,  1.22593053,  1.80653661) +
         (c01 * c2) * vec3<f32>( 1.87394106,  2.05027182, -0.29835996) +
         (c01 * c3) * vec3<f32>( 2.56609566,  7.03428198,  0.62575374) +
         (c02 * c3) * vec3<f32>( 4.08329484, -1.40408358,  2.14995522) +
         (c12 * c3) * vec3<f32>( 6.00078678,  2.55552042,  1.90739502);
}

struct MixboxLatent {
  c   : vec3<f32>,   // pigment weights
  res : vec3<f32>,   // additive residual
};

fn mixboxLatentToRgb(latent: MixboxLatent) -> vec3<f32> {
  return clamp(mixboxEvalPolynomial(latent.c) + latent.res, vec3<f32>(0.0), vec3<f32>(1.0));
}

// Pigment is stored as latent premultiplied by mass, so that advection and
// deposition — both linear — mix correctly. Undo that here.
fn latentFromMass(cm: vec4<f32>, rm: vec4<f32>) -> MixboxLatent {
  let mass = max(cm.w, 1e-5);
  var out : MixboxLatent;
  out.c = cm.xyz / mass;
  out.res = rm.xyz / mass;
  return out;
}
