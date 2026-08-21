// The stroke bridge's dirty-and-drying question, answered in 8 bytes per tile.
//
// The bake needs to know two things about every 128x128 document tile: does it
// hold any pigment worth baking, and is it still wet. Both are `max` over the
// tile, and both are wanted at once -- which is the whole reason this is one
// pass rather than two.
//
// The alternatives, and why this one:
//
//   - CPU dab bounds are free but WRONG: a wash advects and wicks well past
//     the dab that made it, so an AABB under-reports and silently loses paint.
//   - A full-field readback of depC.w is exact but costs a 1024^2 f32 copy
//     (about 1.9 ms and 16 MiB) to answer a question whose answer is a few
//     hundred bytes.
//
// So: one workgroup per tile, 64 invocations each folding a 16x16 sub-block,
// then a workgroup reduction. The output scales as (W/128)*(H/128) -- 512
// bytes at 1024^2, 2 KiB at 2048^2 -- so the readback stays at the transfer
// floor no matter how large the canvas gets.
//
// The pigment figure deliberately includes SUSPENDED pigment at the same
// weight shaders/composite.wgsl uses to draw it (0.75 watercolour, 0.55 ink).
// A tile whose pigment is all still in the water film is not empty, it is not
// finished -- and reporting it as empty is how a stroke would get dropped.
//#include "include/common.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc : texture_2d<f32>;
@group(0) @binding(2) var pigCSrc  : texture_2d<f32>;
@group(0) @binding(3) var depCSrc  : texture_2d<f32>;
@group(0) @binding(4) var satSrc   : texture_2d<f32>;
@group(0) @binding(5) var<storage, read_write> occupancy : array<vec2<f32>>;

const TILE : u32 = 128u;
const LANES : u32 = 64u;

var<workgroup> massScratch : array<f32, 64>;
var<workgroup> wetScratch  : array<f32, 64>;

@compute @workgroup_size(8, 8)
fn main(@builtin(workgroup_id) wg : vec3<u32>, @builtin(local_invocation_index) li : u32) {
  let res = vec2<i32>(P.resolution);
  let tilesX = (u32(res.x) + TILE - 1u) / TILE;

  // Suspended pigment reads weaker than settled pigment because it is sitting
  // in the film rather than lying on the fibres. Same weights composite.wgsl
  // draws with; core/PigmentBake.hpp holds the C++ copy and --selftest checks
  // the two still agree.
  var suspended = 0.75;
  if (P.mode == MODE_INK) { suspended = 0.55; }

  let originX = i32(wg.x * TILE);
  let originY = i32(wg.y * TILE);
  let lx = i32(li % 8u);
  let ly = i32(li / 8u);

  var maxMass = 0.0;
  var maxWet = 0.0;
  // 16x16 texels per invocation, strided by 8 so neighbouring lanes read
  // neighbouring texels and the loads coalesce.
  for (var by = 0; by < 16; by = by + 1) {
    for (var bx = 0; bx < 16; bx = bx + 1) {
      let p = vec2<i32>(originX + lx + bx * 8, originY + ly + by * 8);
      if (p.x >= res.x || p.y >= res.y) { continue; }
      let dep = textureLoad(depCSrc, p, 0).w;
      let pig = textureLoad(pigCSrc, p, 0).w;
      maxMass = max(maxMass, dep + suspended * pig);
      // Wetness as composite.wgsl judges it: the film's own wet mask, or the
      // capillary saturation, whichever says the tile is still working. A
      // tile is dry only when BOTH have gone.
      let water = textureLoad(waterSrc, p, 0);
      let sat = textureLoad(satSrc, p, 0).x;
      maxWet = max(maxWet, max(max(water.z, water.w), sat));
    }
  }

  massScratch[li] = maxMass;
  wetScratch[li] = maxWet;
  workgroupBarrier();

  // Tree reduction over the 64 lanes.
  var stride = LANES / 2u;
  loop {
    if (stride == 0u) { break; }
    if (li < stride) {
      massScratch[li] = max(massScratch[li], massScratch[li + stride]);
      wetScratch[li] = max(wetScratch[li], wetScratch[li + stride]);
    }
    workgroupBarrier();
    stride = stride / 2u;
  }

  if (li == 0u) {
    occupancy[wg.y * tilesX + wg.x] = vec2<f32>(massScratch[0], wetScratch[0]);
  }
}
