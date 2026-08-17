// Conservative donor-cell advection weights — Baxter et al. 2004 (IMPaSTo,
// §4.1.1). Shared by the watercolour pigment transport and the oil paint slab.
//
// Each *donor* cell splits its contents over at most four destinations with
// weights that sum to exactly 1, so mass is conserved by construction whatever
// the velocity field does. This is the whole reason to prefer it over a
// semi-Lagrangian gather, which gives no such guarantee and measured +132%
// pigment mass over 20 s.
//#include "include/common.wgsl"

// Fraction of donor cell with velocity `vn` that lands on the cell displaced
// from it by `d` (d components in {-1, 0, 1}). Separable:
//   (1-ax)(1-ay) + ax(1-ay) + (1-ax)ay + ax*ay = 1
fn donorWeight(vn: vec2<f32>, d: vec2<f32>) -> f32 {
  // CFL: a cell may never donate more than its whole contents in one step.
  let f = clamp(vn * P.dt, vec2<f32>(-1.0), vec2<f32>(1.0));
  // Adhesion: paint never fully leaves a cell, or the surface feels like Teflon.
  // Scaling the donated fraction keeps the weights summing to 1.
  let a = abs(f) * (1.0 - P.adhesion);
  let s = sign(f);

  var wx = 0.0;
  if (d.x == 0.0) { wx = 1.0 - a.x; } else if (d.x == s.x) { wx = a.x; }
  var wy = 0.0;
  if (d.y == 0.0) { wy = 1.0 - a.y; } else if (d.y == s.y) { wy = a.y; }
  return wx * wy;
}

// Maps a canvas texel to the brush's own paint grid. The brush carries paint the
// way a real one does, so it can run dry and pick colour up off the canvas.
// Returns false when the texel is outside the footprint.
//
// The footprint is the *swept* capsule from a to b, matching oil_splat. Testing
// against the segment end alone stamps a disc per frame and leaves a scalloped
// stroke instead of a continuous one.
fn brushGridCoord(p: vec2<f32>, a: vec2<f32>, b: vec2<f32>, radius: f32,
                  gridSize: f32, out: ptr<function, vec2<i32>>) -> bool {
  let ab = b - a;
  let t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
  let local = (p - (a + t * ab)) / max(radius, 1e-3);
  if (dot(local, local) > 1.0) { return false; }
  let uv = local * 0.5 + vec2<f32>(0.5);
  *out = vec2<i32>(clamp(uv * gridSize, vec2<f32>(0.0), vec2<f32>(gridSize - 1.0)));
  return true;
}
