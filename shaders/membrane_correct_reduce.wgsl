// First half of flats/Membrane.cpp's `correct()`: the line-search dot
// products num = <r,e> and den = <e,Ae> over free cells, in the A-norm. No
// float atomics on wgpu, so this is the same per-workgroup tree reduction
// shaders/tile_occupancy.wgsl uses -- one workgroup computes one partial
// (num, den) pair, and the caller (flats/MembraneGpu.cpp) finishes the sum
// on the CPU after reading back the (small) partials buffer.

struct Params {
  width: u32,
  height: u32,
  _pad0: u32,
  _pad1: u32,
}

@group(0) @binding(0) var<uniform> P : Params;
@group(0) @binding(1) var<storage, read> u : array<f32>;
@group(0) @binding(2) var<storage, read> b : array<f32>;
@group(0) @binding(3) var<storage, read> e : array<f32>;
@group(0) @binding(4) var<storage, read> freeMask : array<f32>;
@group(0) @binding(5) var<storage, read_write> partials : array<vec2<f32>>;

const LANES : u32 = 64u;
var<workgroup> numScratch : array<f32, 64>;
var<workgroup> denScratch : array<f32, 64>;

fn lapU(i : u32, x : u32, y : u32) -> f32 {
  var sum = 0.0;
  if (x > 0u)              { sum += u[i - 1u]; }
  if (x < P.width - 1u)    { sum += u[i + 1u]; }
  if (y > 0u)              { sum += u[i - P.width]; }
  if (y < P.height - 1u)   { sum += u[i + P.width]; }
  return 4.0 * u[i] - sum;
}

fn lapE(i : u32, x : u32, y : u32) -> f32 {
  var sum = 0.0;
  if (x > 0u)              { sum += e[i - 1u]; }
  if (x < P.width - 1u)    { sum += e[i + 1u]; }
  if (y > 0u)              { sum += e[i - P.width]; }
  if (y < P.height - 1u)   { sum += e[i + P.width]; }
  return 4.0 * e[i] - sum;
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>,
       @builtin(local_invocation_index) li : u32,
       @builtin(workgroup_id) wg : vec3<u32>,
       @builtin(num_workgroups) ng : vec3<u32>) {
  let x = gid.x;
  let y = gid.y;
  var num = 0.0;
  var den = 0.0;
  if (x < P.width && y < P.height) {
    let i = y * P.width + x;
    if (freeMask[i] != 0.0) {
      let r = b[i] - lapU(i, x, y);
      num = r * e[i];
      den = e[i] * lapE(i, x, y);
    }
  }
  numScratch[li] = num;
  denScratch[li] = den;
  workgroupBarrier();

  var stride = LANES / 2u;
  loop {
    if (stride == 0u) { break; }
    if (li < stride) {
      numScratch[li] += numScratch[li + stride];
      denScratch[li] += denScratch[li + stride];
    }
    workgroupBarrier();
    stride = stride / 2u;
  }

  if (li == 0u) {
    partials[wg.y * ng.x + wg.x] = vec2<f32>(numScratch[0], denScratch[0]);
  }
}
