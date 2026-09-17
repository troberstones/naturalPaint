// Sum-of-squares residual norm from flats/Membrane.cpp's `residual()`, as the
// same per-workgroup tree reduction the other membrane_*_reduce shaders use.
// The caller finishes the sum and the sqrt on the CPU after a small readback.

struct Params {
  width: u32,
  height: u32,
  _pad0: u32,
  _pad1: u32,
}

@group(0) @binding(0) var<uniform> P : Params;
@group(0) @binding(1) var<storage, read> u : array<f32>;
@group(0) @binding(2) var<storage, read> b : array<f32>;
@group(0) @binding(3) var<storage, read> freeMask : array<f32>;
@group(0) @binding(4) var<storage, read_write> partials : array<f32>;

const LANES : u32 = 64u;
var<workgroup> scratch : array<f32, 64>;

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>,
       @builtin(local_invocation_index) li : u32,
       @builtin(workgroup_id) wg : vec3<u32>,
       @builtin(num_workgroups) ng : vec3<u32>) {
  let x = gid.x;
  let y = gid.y;
  var s = 0.0;
  if (x < P.width && y < P.height) {
    let i = y * P.width + x;
    if (freeMask[i] != 0.0) {
      var sum = 0.0;
      if (x > 0u)              { sum += u[i - 1u]; }
      if (x < P.width - 1u)    { sum += u[i + 1u]; }
      if (y > 0u)              { sum += u[i - P.width]; }
      if (y < P.height - 1u)   { sum += u[i + P.width]; }
      let r = b[i] - (4.0 * u[i] - sum);
      s = r * r;
    }
  }
  scratch[li] = s;
  workgroupBarrier();

  var stride = LANES / 2u;
  loop {
    if (stride == 0u) { break; }
    if (li < stride) { scratch[li] += scratch[li + stride]; }
    workgroupBarrier();
    stride = stride / 2u;
  }

  if (li == 0u) {
    partials[wg.y * ng.x + wg.x] = scratch[0];
  }
}
