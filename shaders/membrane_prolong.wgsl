// Bilinear (cell-centred 9/3/3/1) prolongation of the coarse correction, a
// natural gather already: dispatched one invocation per FINE cell, each reads
// up to 4 coarse cells. Mirrors flats/Membrane.cpp's `prolong()` exactly,
// including reading a pinned or out-of-range coarse cell as 0 so the
// correction tapers rather than cliffs at a stroke.

struct Params {
  fineWidth: u32,
  fineHeight: u32,
  coarseWidth: u32,
  coarseHeight: u32,
}

@group(0) @binding(0) var<uniform> P : Params;
@group(0) @binding(1) var<storage, read> coarseU : array<f32>;
@group(0) @binding(2) var<storage, read> coarseFree : array<f32>;
@group(0) @binding(3) var<storage, read> fineFree : array<f32>;
@group(0) @binding(4) var<storage, read_write> fineE : array<f32>;

fn at(cx : i32, cy : i32, cw : i32, ch : i32) -> f32 {
  if (cx < 0 || cy < 0 || cx >= cw || cy >= ch) { return 0.0; }
  let ci = u32(cy * cw + cx);
  if (coarseFree[ci] == 0.0) { return 0.0; }
  return coarseU[ci];
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let x = gid.x;
  let y = gid.y;
  if (x >= P.fineWidth || y >= P.fineHeight) { return; }
  let fi = y * P.fineWidth + x;
  if (fineFree[fi] == 0.0) {
    fineE[fi] = 0.0;
    return;
  }

  let cw = i32(P.coarseWidth);
  let ch = i32(P.coarseHeight);
  let cx = i32(x) / 2;
  let cy = i32(y) / 2;
  let sx = select(-1, 1, (x & 1u) != 0u);
  let sy = select(-1, 1, (y & 1u) != 0u);

  let v = 9.0 * at(cx, cy, cw, ch) + 3.0 * at(cx + sx, cy, cw, ch) +
          3.0 * at(cx, cy + sy, cw, ch) + at(cx + sx, cy + sy, cw, ch);
  fineE[fi] = v / 16.0;
}
