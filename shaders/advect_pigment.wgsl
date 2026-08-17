// Transport of suspended pigment: conservative advection plus diffusion through
// the wet film.
//
// Advection is the donor-cell flux scheme from Baxter et al. 2004 (IMPaSTo,
// §4.1.1) rather than the semi-Lagrangian resample this used to do. Semi-
// Lagrangian advection is a gather that gives no conservation guarantee, and
// measured +132% pigment mass over 20s. Here each *donor* cell splits its
// contents over at most four destinations with weights that sum to exactly 1, so
// mass is conserved by construction regardless of the velocity field.
//
// Both textures hold latent-times-mass, so every operation is a linear
// combination and therefore a Kubelka-Munk-correct pigment mix — the reason for
// storing pigment in Mixbox latent space rather than RGB.
//#include "include/common.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc : texture_2d<f32>;
@group(0) @binding(2) var pigCSrc  : texture_2d<f32>;
@group(0) @binding(3) var pigRSrc  : texture_2d<f32>;
@group(0) @binding(4) var pigCDst  : texture_storage_2d<rgba32float, write>;
@group(0) @binding(5) var pigRDst  : texture_storage_2d<rgba32float, write>;

// Fraction of donor cell `n` (velocity vn) that lands on the cell displaced from
// it by `d`. Separable in x and y. The four non-zero cases sum to 1:
//   (1-ax)(1-ay) + ax(1-ay) + (1-ax)ay + ax*ay = 1
fn donorWeight(vn: vec2<f32>, d: vec2<f32>) -> f32 {
  // CFL: a cell may never donate more than its whole contents in one step.
  // IMPaSTo clamps velocity to +/-1 cell per step for exactly this reason.
  let f = clamp(vn * P.dt, vec2<f32>(-1.0), vec2<f32>(1.0));
  // Adhesion: paint never fully leaves a cell, or the canvas feels like Teflon
  // (IMPaSTo §4.1.1). Scaling the donated fraction keeps the weights summing to 1.
  let a = abs(f) * (1.0 - P.adhesion);
  let s = sign(f);

  var wx = 0.0;
  if (d.x == 0.0) { wx = 1.0 - a.x; } else if (d.x == s.x) { wx = a.x; }
  var wy = 0.0;
  if (d.y == 0.0) { wy = 1.0 - a.y; } else if (d.y == s.y) { wy = a.y; }
  return wx * wy;
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }
  let res = P.resolution;

  let mHere = textureLoad(waterSrc, p, 0).w;
  let srcC = textureLoad(pigCSrc, p, 0);
  let srcR = textureLoad(pigRSrc, p, 0);

  // --- conservative advection: gather every donor that can reach this cell ---
  var outC = vec4<f32>(0.0);
  var outR = vec4<f32>(0.0);
  for (var oy = -1; oy <= 1; oy = oy + 1) {
    for (var ox = -1; ox <= 1; ox = ox + 1) {
      let n = p + vec2<i32>(ox, oy);
      // Out-of-grid donors do not exist. A boundary cell can therefore still
      // donate off the edge and lose that mass, but strokes rarely reach it.
      if (!inBounds(n, res)) { continue; }

      let water = textureLoad(waterSrc, n, 0);
      // Dry donors hold onto everything; only wet cells flow.
      var vn = vec2<f32>(0.0);
      if (water.w >= 0.01) { vn = water.xy; }

      let w = donorWeight(vn, vec2<f32>(f32(-ox), f32(-oy)));
      if (w > 0.0) {
        outC = outC + w * textureLoad(pigCSrc, n, 0);
        outR = outR + w * textureLoad(pigRSrc, n, 0);
      }
    }
  }

  // --- diffusion ---
  // The pair weight min(M_here, M_neighbour) is symmetric, so the flux this cell
  // sees is exactly the negative of what the neighbour sees and the exchange
  // conserves mass. It also falls to zero at the edge of the wet region, which
  // stops pigment creeping onto dry paper.
  let k = clamp(P.pigmentDiffuse * P.dt, 0.0, 1.0) * 0.25;
  if (k > 0.0 && mHere >= 0.01) {
    var fluxC = vec4<f32>(0.0);
    var fluxR = vec4<f32>(0.0);
    let offs = array<vec2<i32>, 4>(
      vec2<i32>(-1, 0), vec2<i32>(1, 0), vec2<i32>(0, -1), vec2<i32>(0, 1)
    );
    for (var i = 0; i < 4; i = i + 1) {
      let q = clampCoord(p + offs[i], res);
      let w = min(mHere, textureLoad(waterSrc, q, 0).w);
      fluxC = fluxC + w * (textureLoad(pigCSrc, q, 0) - srcC);
      fluxR = fluxR + w * (textureLoad(pigRSrc, q, 0) - srcR);
    }
    outC = outC + k * fluxC;
    outR = outR + k * fluxR;
  }

  textureStore(pigCDst, p, max(outC, vec4<f32>(0.0)));
  textureStore(pigRDst, p, outR);
}
