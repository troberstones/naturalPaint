// Second half of flats/Membrane.cpp's `correct()`: accept the line-searched
// correction, u += alpha*e on free cells. `alpha` is computed on the CPU from
// the reduction in membrane_correct_reduce.wgsl and written into this
// uniform right before this dispatch.

struct Params {
  width: u32,
  height: u32,
  _pad: u32,
  alpha: f32,
}

@group(0) @binding(0) var<uniform> P : Params;
@group(0) @binding(1) var<storage, read_write> u : array<f32>;
@group(0) @binding(2) var<storage, read> e : array<f32>;
@group(0) @binding(3) var<storage, read> freeMask : array<f32>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let x = gid.x;
  let y = gid.y;
  if (x >= P.width || y >= P.height) { return; }
  let i = y * P.width + x;
  if (freeMask[i] == 0.0) { return; }
  u[i] = u[i] + P.alpha * e[i];
}
