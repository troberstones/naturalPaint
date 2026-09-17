// Red-black Gauss-Seidel smoother for flats/MembraneGpu -- one colour per
// dispatch. Cells of one colour only ever read the OTHER colour, which this
// dispatch does not write, so every invocation is independent: no ping-pong,
// no race, same property flats/Membrane.cpp's `smooth()` already leans on.

struct Params {
  width: u32,
  height: u32,
  color: u32,   // 0 or 1: (x+y) parity this dispatch updates
  _pad: u32,
}

@group(0) @binding(0) var<uniform> P : Params;
@group(0) @binding(1) var<storage, read_write> u : array<f32>;
@group(0) @binding(2) var<storage, read> b : array<f32>;
@group(0) @binding(3) var<storage, read> freeMask : array<f32>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let x = gid.x;
  let y = gid.y;
  if (x >= P.width || y >= P.height) { return; }
  if ((x + y) % 2u != P.color) { return; }
  let i = y * P.width + x;
  if (freeMask[i] == 0.0) { return; }

  var sum = b[i];
  if (x > 0u)              { sum += u[i - 1u]; }
  if (x < P.width - 1u)    { sum += u[i + 1u]; }
  if (y > 0u)              { sum += u[i - P.width]; }
  if (y < P.height - 1u)   { sum += u[i + P.width]; }
  u[i] = sum * 0.25;
}
