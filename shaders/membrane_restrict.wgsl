// Full-weighting restriction of the fine residual onto the coarse RHS, as a
// GATHER over the coarse grid (no float atomics on wgpu, so no scatter).
// Dispatched one invocation per COARSE cell; each reads its own up-to-4 fine
// children directly out of the fine level's own u/b/free buffers, exactly
// mirroring flats/Membrane.cpp's `restrictResidual()` but restructured so the
// coarse cell -- not the fine cell -- owns the write.

struct Params {
  fineWidth: u32,
  fineHeight: u32,
  coarseWidth: u32,
  coarseHeight: u32,
}

@group(0) @binding(0) var<uniform> P : Params;
@group(0) @binding(1) var<storage, read> fineU : array<f32>;
@group(0) @binding(2) var<storage, read> fineB : array<f32>;
@group(0) @binding(3) var<storage, read> fineFree : array<f32>;
@group(0) @binding(4) var<storage, read_write> coarseU : array<f32>;
@group(0) @binding(5) var<storage, read_write> coarseB : array<f32>;
@group(0) @binding(6) var<storage, read> coarseFree : array<f32>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let cx = gid.x;
  let cy = gid.y;
  if (cx >= P.coarseWidth || cy >= P.coarseHeight) { return; }
  let ci = cy * P.coarseWidth + cx;

  coarseU[ci] = 0.0;
  if (coarseFree[ci] == 0.0) {
    coarseB[ci] = 0.0;
    return;
  }

  let fw = i32(P.fineWidth);
  let fh = i32(P.fineHeight);
  var total = 0.0;
  for (var dy = 0; dy < 2; dy = dy + 1) {
    for (var dx = 0; dx < 2; dx = dx + 1) {
      let fx = i32(cx) * 2 + dx;
      let fy = i32(cy) * 2 + dy;
      if (fx >= fw || fy >= fh) { continue; }
      let fi = u32(fy * fw + fx);
      if (fineFree[fi] == 0.0) { continue; }

      var sum = 0.0;
      if (fx > 0)      { sum += fineU[fi - 1u]; }
      if (fx < fw - 1) { sum += fineU[fi + 1u]; }
      if (fy > 0)      { sum += fineU[fi - u32(fw)]; }
      if (fy < fh - 1) { sum += fineU[fi + u32(fw)]; }
      let r = fineB[fi] - (4.0 * fineU[fi] - sum);
      total += r;
    }
  }
  coarseB[ci] = total;
}
