// Shared uniform block and helpers for the watercolour solver.
// Field naming follows Curtis et al. 1997, "Computer-Generated Watercolor", §4.

struct SimParams {
  resolution     : vec2<u32>,
  dt             : f32,
  pigmentDiffuse : f32,   // pigment spreads through the wet film, not only with the flow

  // --- shallow-water layer (§4.1) ---
  viscosity      : f32,   // mu    — velocity smoothing
  drag           : f32,   // kappa — velocity damping
  edgeDarkening  : f32,   // eta   — flowOutward strength; the signature watercolour rim
  paperSlope     : f32,   // how strongly paper height biases flow

  // --- pigment transfer (§4.2) ---
  density        : f32,   // rho   — heavy pigments settle faster
  staining       : f32,   // omega — resists being lifted back into suspension
  granulation    : f32,   // gamma — affinity for paper valleys
  wetThreshold   : f32,

  // --- capillary layer (§4.3) ---
  absorbRate     : f32,
  capacityScale  : f32,
  diffuseRate    : f32,
  evaporation    : f32,

  // --- brush ---
  brushA         : vec2<f32>,  // segment start, texel space
  brushB         : vec2<f32>,  // segment end
  brushRadius    : f32,
  brushWater     : f32,
  brushPigment   : f32,
  brushHardness  : f32,
  brushLatentC   : vec4<f32>,  // mixbox c0,c1,c2 (w unused)
  brushLatentR   : vec4<f32>,  // mixbox residual rgb (w unused)
  brushActive    : u32,
  frame          : u32,
  adhesion       : f32,   // paint never fully leaves a cell (IMPaSTo 4.1.1)
  mode           : u32,   // 0 watercolour, 1 oil, 2 ink

  // --- oil (Baxter et al. 2004, IMPaSTo) ---
  brushLoad      : f32,
  penetration    : f32,
  xferFraction   : f32,   // XFER_FRACTION in Algorithm 1
  maxXfer        : f32,   // MAX_XFER_QUANTITY in Algorithm 1

  impastoLight   : f32,
  oilPressure    : f32,   // vp = -c * grad(penetration)

  // --- ink (Chu & Tai 2005, MoXi) ---
  omega          : f32,   // LBE relaxation; viscosity = (1/omega - 1/2)/3
  blocking       : f32,   // k5, the base term of MoXi eq. 6
  grainBlock     : f32,   // k2, weight on the paper grain
  glue           : f32,   // k4, limits spread
  receptivity    : f32,   // lambda in the footprint mask
  brushReload    : u32,   // refill the brush grid this frame (stroke start)

  // Surface tension caps how deep a water film sits on paper before it runs.
  // Without a cap, splat accumulates depth linearly with dwell time and a two
  // second touch leaves a pressure head ~20x the paper's capacity, which drives
  // outward flow that never stops and empties the wash into its own rim.
  maxFilm        : f32,
  // Reconciles per-step deposition rates with this solver's step count, same
  // role as RATE_SCALE in transfer_pigment.
  settleScale    : f32,
  // Board tilt as a gravity vector in canvas texel space (+y runs down-screen).
  tilt           : vec2<f32>,
};

const MODE_WATERCOLOR : u32 = 0u;
const MODE_OIL        : u32 = 1u;
const MODE_INK        : u32 = 2u;

fn inBounds(p: vec2<i32>, res: vec2<u32>) -> bool {
  return p.x >= 0 && p.y >= 0 && p.x < i32(res.x) && p.y < i32(res.y);
}

fn clampCoord(p: vec2<i32>, res: vec2<u32>) -> vec2<i32> {
  return clamp(p, vec2<i32>(0, 0), vec2<i32>(i32(res.x) - 1, i32(res.y) - 1));
}

// Texel centre -> normalised uv, for filtered sampling during advection.
fn toUV(p: vec2<f32>, res: vec2<u32>) -> vec2<f32> {
  return (p + vec2<f32>(0.5, 0.5)) / vec2<f32>(f32(res.x), f32(res.y));
}

// Distance from point p to segment ab — brush strokes are swept discs, so that
// fast cursor motion doesn't leave a dotted trail.
fn distToSegment(p: vec2<f32>, a: vec2<f32>, b: vec2<f32>) -> f32 {
  let ab = b - a;
  let denom = max(dot(ab, ab), 1e-6);
  let t = clamp(dot(p - a, ab) / denom, 0.0, 1.0);
  return distance(p, a + t * ab);
}
