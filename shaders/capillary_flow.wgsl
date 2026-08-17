// Curtis §4.3 SimulateCapillaryFlow — water wicking through the paper fibres
// below the surface. This is what makes a wet stroke keep creeping outward after
// the brush has left, and what produces blooms when wet meets wet.
//
// The paper's scatter loop is rewritten as an antisymmetric gather so the
// exchange stays conservative when every cell updates in parallel.
//#include "include/common.wgsl"

@group(0) @binding(0) var<uniform> P : SimParams;
@group(0) @binding(1) var waterSrc : texture_2d<f32>;
@group(0) @binding(2) var paperTex : texture_2d<f32>;
@group(0) @binding(3) var satSrc   : texture_2d<f32>;
@group(0) @binding(4) var waterDst : texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var satDst   : texture_storage_2d<rgba16float, write>;

fn capacityAt(p: vec2<i32>) -> f32 {
  return textureLoad(paperTex, p, 0).y * P.capacityScale;
}

@compute @workgroup_size(8, 8)
fn main(@builtin(global_invocation_id) gid : vec3<u32>) {
  let p = vec2<i32>(gid.xy);
  if (!inBounds(p, P.resolution)) { return; }
  let res = P.resolution;

  var water = textureLoad(waterSrc, p, 0);
  var s = textureLoad(satSrc, p, 0).x;
  let c = capacityAt(p);

  // --- absorb standing water into the fibres ---
  let room = max(c - s, 0.0);
  let absorbed = min(water.z * P.absorbRate * P.dt, room);
  s = s + absorbed;
  water.z = water.z - absorbed;

  // --- diffuse between neighbours, antisymmetric so mass is conserved ---
  var net = 0.0;
  let offs = array<vec2<i32>, 4>(
    vec2<i32>(-1, 0), vec2<i32>(1, 0), vec2<i32>(0, -1), vec2<i32>(0, 1)
  );
  for (var i = 0; i < 4; i = i + 1) {
    let q = clampCoord(p + offs[i], res);
    let sq = textureLoad(satSrc, q, 0).x;
    let cq = capacityAt(q);
    // Flow only downhill in saturation, and only into somewhere with room.
    let d = (s - sq) * P.diffuseRate * 0.25;
    if (d > 0.0) {
      net = net - min(d, max(cq - sq, 0.0) * 0.25);
    } else {
      net = net - max(d, -max(c - s, 0.0) * 0.25);
    }
  }
  s = max(s + net, 0.0);

  // --- saturated fibres re-wet the surface: this is the bloom ---
  if (s > P.wetThreshold) {
    water.w = max(water.w, 1.0);
  }

  // --- evaporation ---
  let evap = P.evaporation * P.dt;
  water.z = max(water.z - evap, 0.0);
  s = max(s - evap * 0.25, 0.0);
  if (water.z <= 0.0 && s <= P.wetThreshold) {
    water.w = max(water.w - evap * 4.0, 0.0);
  }

  textureStore(waterDst, p, water);
  textureStore(satDst, p, vec4<f32>(s, 0.0, 0.0, 0.0));
}
