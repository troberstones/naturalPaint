#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "color/LutBake.hpp"
#include "core/OpStack.hpp"
#include "core/SelectionMask.hpp"
#include "gfx/Context.hpp"
#include "gfx/Wgpu.hpp"
#include "paint/Palette.hpp"

namespace np {

// Which physical model the solver runs. Each has its own pass list and its own
// reading of the shared field textures; switching clears the canvas, because a
// wash of watercolour is not a valid initial state for a slab of oil paint.
enum class PaintMode {
  Watercolor,  // Curtis et al. 1997 — shallow water + pigment + capillary layer
  Oil,         // Baxter et al. 2004 (IMPaSTo) — height field + brush transfer
  Ink,         // Chu & Tai 2005 (MoXi) — D2Q9 lattice Boltzmann percolation
  Count
};

const char* paintModeName(PaintMode m);

// Must match SimParams in shaders/include/common.wgsl byte for byte.
// Every member is naturally aligned, so the WGSL uniform layout falls out.
struct SimParams {
  uint32_t resolutionX = 0;
  uint32_t resolutionY = 0;
  float dt = 1.0f;
  // Without this the wet front outruns the pigment front and a spreading wash
  // runs clear at its leading edge.
  float pigmentDiffuse = 0.25f;

  // Tuned against --selftest. The original defaults damped velocity to nothing
  // within about two steps and evaporated the water before it could flow, which
  // made every stroke read as flat gouache.
  float viscosity = 0.05f;
  float drag = 0.12f;
  float edgeDarkening = 0.30f;
  float paperSlope = 0.9f;

  float density = 0.12f;
  float staining = 0.60f;
  float granulation = 0.45f;
  float wetThreshold = 0.15f;

  float absorbRate = 0.25f;
  float capacityScale = 1.0f;
  float diffuseRate = 0.85f;
  float evaporation = 0.004f;

  float brushAx = 0.0f, brushAy = 0.0f;
  float brushBx = 0.0f, brushBy = 0.0f;
  float brushRadius = 18.0f;
  float brushWater = 1.4f;
  float brushPigment = 0.9f;
  float brushHardness = 0.35f;

  float brushLatentC[4] = {0, 0, 0, 0};
  float brushLatentR[4] = {0, 0, 0, 0};

  uint32_t brushActive = 0;
  uint32_t frame = 0;
  // Paint never fully advects out of a cell, or the canvas feels like Teflon.
  float adhesion = 0.06f;
  uint32_t mode = 0;  // PaintMode

  // --- oil (IMPaSTo) ---
  float brushLoad = 1.0f;      // paint volume carried by the brush
  float penetration = 0.55f;   // how hard the brush presses into the slab
  float xferFraction = 0.10f;  // IMPaSTo Algorithm 1
  float maxXfer = 0.02f;

  float impastoLight = 0.65f;  // strength of the height-field shading
  float oilPressure = 1.4f;    // vp = -c * grad(penetration)

  // --- ink (MoXi) ---
  float omega = 0.70f;         // LBE relaxation parameter; viscosity = (1/w - 1/2)/3
  float blocking = 0.10f;      // base blocking factor k5 in MoXi eq.6
  float grainBlock = 0.22f;    // weight on the paper grain texture
  float glue = 0.04f;          // limits spread; modulates blocking
  float receptivity = 1.20f;   // lambda in the footprint mask
  uint32_t brushReload = 0;    // refill the brush grid this frame

  // Deepest water film the paper will hold before it runs off. Roughly a small
  // multiple of the fibre capacity (~0.9); uncapped, dwell time alone builds a
  // pressure head that empties a wash into its rim.
  float maxFilm = 2.2f;
  float settleScale = 0.008f;  // per-step deposition rate scale (ink)
  // Board tilt as a gravity vector in canvas texel space (+y runs down-screen).
  float tiltX = 0.0f;
  float tiltY = 0.0f;
};
static_assert(sizeof(SimParams) == 208, "SimParams must match the WGSL layout");

// Working time: how long a wash keeps moving before it sets, in seconds.
//
// Exposed as one control because the wet lifetime is otherwise an emergent
// product of three sliders. It is dominated by the capillary layer draining:
// capillary_flow decays saturation by evaporation*0.25 per frame, and at 60 fps
// against a fibre capacity of ~0.9 that gives
//
//   T ~ 0.9 / (evaporation * 0.25 * 60) = 0.06 / evaporation
//
// Verified against --diag: the default 0.004 dries a blob at ~14 s, and
// 0.06/0.004 = 15. Surface water drains four times faster than the fibres, so
// the capillary layer is what sets the tail.
// Both drain paths scale, not just evaporation. Scaling evaporation alone left
// absorption running at a fixed rate, so the mapping compressed at the long end
// (25 s asked, 19.5 s measured). Slowing the whole drying process keeps the
// control linear in seconds, and matches the physical reading: paper that stays
// workable longer is absorbing more slowly too.
inline void setWorkingTime(SimParams& p, float seconds) {
  const float t = seconds > 0.25f ? seconds : 0.25f;
  p.evaporation = 0.06f / t;
  p.absorbRate = 3.75f / t;
}
inline float workingTimeOf(const SimParams& p) {
  return p.evaporation > 1e-6f ? 0.06f / p.evaporation : 240.0f;
}

// A field that is read from one texture and written to the other, then flipped.
struct PingPong {
  WGPUTexture tex[2] = {nullptr, nullptr};
  WGPUTextureView view[2] = {nullptr, nullptr};
  WGPUTextureFormat format = WGPUTextureFormat_RGBA16Float;
  int cur = 0;

  WGPUTextureView src() const { return view[cur]; }
  WGPUTextureView dst() const { return view[1 - cur]; }
  WGPUTexture srcTex() const { return tex[cur]; }
  void flip() { cur ^= 1; }
  void release();
};

class PaintSim {
 public:
  bool init(GpuContext& gpu, uint32_t width, uint32_t height, const MixboxLut& lut);

  // Reallocates any mode-specific fields and clears. Cheap enough to call from
  // a menu item; the ink lattice is only allocated while Ink mode is active.
  void setMode(GpuContext& gpu, PaintMode m);
  PaintMode mode() const { return mode_; }
  void resize(GpuContext& gpu, uint32_t width, uint32_t height);
  void clearCanvas(GpuContext& gpu);
  void shutdown();

  // 1.4 / ADR-0001 bullet 2: true once Ink/Oil mode has actually allocated
  // its optional field set (the D2Q9 lattice / the brush paint grid). These
  // already gate allocInkFields()/allocOilFields() internally; exposed read-
  // only so --selftest can assert, rather than merely assume, that a sim
  // freshly constructed in the default Watercolour mode holds neither --
  // i.e. that "no Media content" really does mean zero field textures for
  // the media that were never engaged.
  bool inkFieldsAllocated() const { return inkAllocated_; }
  bool oilFieldsAllocated() const { return oilAllocated_; }

  // Advance the solver and resolve to the canvas texture.
  //
  // `selectionMask`: phase-2 seam reservation (PLAN.md Phase 2 step 7, PRD
  // E1) -- unused today. For watercolour and ink, frame() only advances
  // physics substeps (deposition happens in depositDab()), but for Oil the
  // brush's contact -> velocity -> transfer pipeline (kOilSplat/kOilTransfer/
  // kOilBrush, all gated on brushActive) runs inside this function -- see
  // depositDab()'s comment -- so this is genuinely Oil's deposit path and
  // must carry the same slot depositDab() does, or Oil would need its own
  // retrofit later. Always nullptr until selection tools ship.
  void frame(GpuContext& gpu, const SimParams& params,
             const SelectionMask* selectionMask = nullptr);

  // Deposits exactly one dab at (x, y), canvas texel space -- a single,
  // self-contained dispatch of the medium's splat pass (kSplat / kInkSplat),
  // entirely decoupled from frame()'s physics substep loop (ADR-0003, 1.3).
  // `params` is reused wholesale for brushRadius/brushWater/brushPigment/
  // brushHardness/brushLatentC/R exactly as the caller set them; brushAx/Ay
  // and brushBx/By are both overwritten with (x, y) here (a dab is a point,
  // not a segment) and brushActive is forced on. `params.dt` is written but
  // unread: the WGSL deposition terms no longer scale by it (that scaling
  // was the bug ADR-0003 fixes) -- a dab now deposits a fixed quantity
  // regardless of dt.
  //
  // Oil does not deposit through this path. Its contact -> velocity ->
  // transfer pipeline is a genuine continuous exchange (kOilTransfer has no
  // per-dab "amount" to speak of), so it stays inside frame()'s existing
  // substep loop, fed by the current brushA/B/brushActive the same way it
  // always was -- see main.cpp and MacPaintUI.cpp for how those are now
  // sourced from the dab emitter instead of the raw per-frame cursor
  // segment. Calling this while mode() == PaintMode::Oil is a harmless
  // no-op.
  //
  // `selectionMask`: phase-2 seam reservation, not a feature (PLAN.md Phase 2
  // step 7; PRD E1: "every deposit and every op respects the active
  // selection"; DESIGN-imaging.md "Selections"). Nothing populates a
  // SelectionMask until the "Select and paste" phase, and this parameter is
  // not read yet -- it exists purely so this signature does not need to
  // change again, at every call site, once it is.
  void depositDab(GpuContext& gpu, const SimParams& params, float x, float y,
                  const SelectionMask* selectionMask = nullptr);

  // Blocking copy of the canvas to RGBA8 host memory. Used by --selftest; slow
  // by design (it stalls the queue), so keep it out of the frame loop.
  bool readbackCanvas(GpuContext& gpu, std::vector<uint8_t>& out);

  // Conservation and transport diagnostics. All blocking readbacks — for
  // --diag only, never the frame loop.
  struct Stats {
    double suspended = 0;   // total pigment mass still in the water
    double deposited = 0;   // total pigment mass settled on the paper
    double wetCells = 0;    // cells with M > 0.01
    double pigmentCells = 0;// cells holding any pigment at all
    double meanSpeed = 0;   // over wet cells
    double maxSpeed = 0;
    double totalWater = 0;  // sum of the pressure/height field
  };
  Stats computeStats(GpuContext& gpu);

  // --diag only: lets the harness measure where pigment actually ended up.
  // --- The stroke bridge's dirty-and-drying question -----------------------
  //
  // One entry per 128x128 document tile, in row-major tile order. `mass` is
  // the largest `deposited + weight*suspended` pigment mass anywhere in the
  // tile, using the same suspended weight shaders/composite.wgsl draws with;
  // `wetness` is the largest of the water film's depth, its wet mask and the
  // capillary saturation. A tile is ready to bake when `mass` is above the
  // noise floor and `wetness` has gone to zero.
  //
  // Deliberately NOT a bounding box. The measured cost of taking exactly the
  // tiles you need rather than one bounding-box copy is about 3.6%, so
  // bounding-box logic would buy nothing and would over-report every stroke
  // that curves.
  struct TileOccupancy {
    float mass = 0.0f;
    float wetness = 0.0f;
  };

  // --- The tile payload, always deferred ------------------------------------
  //
  // One tile of the deposited pigment pair is 128*128*16 = 256 KiB per field,
  // and a bake may want several. Blocking on that drains the queue: with a
  // realistic watercolour frame in flight, even a ONE-tile readback measured
  // 3.288 ms -- 16% of PRD F3's 20 ms budget, spent waiting for the solver to
  // finish rather than on the payload, on a frame where the user is still
  // painting. So this is a fence-and-poll state machine and the poll has never
  // had to wait (measured 0.013 ms, ready 200/200 times one frame later).
  //
  // Idle -> Submitted -> Ready -> (endPigmentReadback) -> Idle.
  //
  // Per-tile copies rather than one bounding box, deliberately: the measured
  // difference is about 3.6% (bytes are the cost, not command count), so
  // bounding-box logic would buy nothing and would over-report every stroke
  // that curves. It costs nothing in padding either -- a 128-texel RGBA32F row
  // is 2048 bytes, exactly 8 x WebGPU's 256-byte copy alignment, so a tile
  // copies with no stride padding at all.
  enum class PigmentReadback { Idle, Submitted, Ready, Failed };

  struct BridgeTile {
    uint32_t x = 0, y = 0;
  };

  // Issues the copies and submits. Non-blocking. Refuses if a readback is
  // already in flight -- one at a time is all the bake needs, and a queue of
  // them would hide which one a later failure belonged to.
  bool beginPigmentReadback(GpuContext& gpu, const std::vector<BridgeTile>& tiles);

  // Non-blocking. Pumps the instance once and reports where the machine is.
  PigmentReadback pollPigmentReadback(GpuContext& gpu);

  // Valid only while `Ready`, and only until `endPigmentReadback()`. Points
  // straight into the mapped range: the bake decodes from here into
  // PigmentTile storage without an intermediate copy, which the measurements
  // said was the largest single term at 64 tiles (2.5 ms of 3.9).
  //
  // Each is 128*128*4 floats, row-major within the tile.
  const float* pigmentReadbackDepC(size_t tileIndex) const;
  const float* pigmentReadbackDepR(size_t tileIndex) const;
  size_t pigmentReadbackTileCount() const { return readbackTiles_.size(); }
  // Which tile the i'th readback slot came from, so a caller can put it back
  // where it belongs without keeping its own parallel copy of the list it
  // passed in -- two lists that must stay in step is a bug waiting to happen.
  BridgeTile bridgeTileAt(size_t i) const {
    return i < readbackTiles_.size() ? readbackTiles_[i] : BridgeTile{};
  }

  // Unmaps and returns to Idle. Safe to call in any state. NOT called
  // automatically on failure, so a caller can ask what went wrong first.
  void endPigmentReadback();

  // The bake's other half. `app/StrokeBake::bakePigmentTiles()` reads
  // depC_/depR_ (deposited) and writes their contents into a layer's tiles;
  // this zeros exactly what a bake moved -- depC_/depR_, and pigC_/pigR_
  // (suspended) alongside them, in BOTH ping-pong halves so the clear
  // survives the next flip(). Suspended is included even though bake never
  // reads it: StrokeBake.hpp documents that a forced bake-while-wet "silently
  // drops whatever is still suspended," and if this left pigC_/pigR_ alone,
  // that dropped paint would keep rendering here while the document had no
  // record of it -- a ghost the bake already promised not to keep.
  //
  // water_/sat_ are deliberately untouched: TileOccupancy's own contract is
  // that a tile is bake-ready once wetness has gone to zero, so on a normal
  // bake they are already at rest, and on a forced bake the paper being
  // genuinely still damp there is a true fact this call has no business
  // erasing.
  //
  // Returns false and clears nothing if any tile is out of range, matching
  // beginPigmentReadback()'s validation -- a caller that named a tile
  // outside the field has a bug worth surfacing, not silently ignoring one
  // tile out of several. An empty list is a harmless no-op (true, nothing
  // to do) rather than a refusal: unlike a readback, "clear whatever the
  // last bake touched" is a legitimate call to make when that bake touched
  // nothing.
  bool clearBakedTiles(GpuContext& gpu, const std::vector<BridgeTile>& tiles);

  // Runs the reduction and reads the result back, BLOCKING. That is the right
  // call for this payload and only for this payload: it is a few hundred bytes
  // and sits at the transfer floor (measured 0.129 ms median), where a tile
  // payload is megabytes and must always be deferred. Returns false and leaves
  // `out` untouched if the sim has no fields yet.
  //
  // `out` is resized to `tileCountX() * tileCountY()`.
  bool readTileOccupancy(GpuContext& gpu, std::vector<TileOccupancy>& out);

  uint32_t tileCountX() const { return (width_ + kBridgeTile - 1) / kBridgeTile; }
  uint32_t tileCountY() const { return (height_ + kBridgeTile - 1) / kBridgeTile; }

  // The tile edge the bridge reduces over. It is core/Tile's 128, and it has
  // to stay that: the whole point is that one entry here answers for exactly
  // one document tile, so a mismatch would make the mapping many-to-many and
  // the bake would have to intersect rectangles instead of naming tiles.
  static constexpr uint32_t kBridgeTile = 128;

  bool readbackField(GpuContext& gpu, WGPUTexture tex, WGPUTextureFormat format,
                     std::vector<float>& out);
  WGPUTexture depCTexForDiag() const { return depC_.srcTex(); }
  WGPUTexture depRTexForDiag() const { return depR_.srcTex(); }
  WGPUTexture pigCTexForDiag() const { return pigC_.srcTex(); }
  WGPUTexture pigRTexForDiag() const { return pigR_.srcTex(); }

  // For ImGui::Image.
  WGPUTextureView canvasView() const { return canvasView_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

  // PLAN.md Phase 2 step 11 / PRD Q3: grayscale preview. A dedicated
  // per-pixel luminance blit (shaders/grayscale_blit.wgsl) over canvasView_,
  // written into its own separate texture rather than mutating canvas_ in
  // place -- canvas_ is the document's actual rendering (--selftest's
  // readbackCanvas() reads it directly), and this toggle is view state, not
  // document state, so it must never be able to touch canvas_ even
  // accidentally. Callers (main.cpp, after this frame's PaintSim::frame()
  // has already run -- see main.cpp's own comment on ordering) call this
  // once per frame only while AppState::CanvasView::grayscale is on; it's a
  // same-resolution blit, cheap next to the solver's own compute passes.
  // A no-op if pipelines/fields aren't built yet.
  void updateGrayscalePreview(GpuContext& gpu);
  WGPUTextureView grayscaleView() const { return grayscaleView_; }

  // PLAN.md Phase 3 step 6 ("Apply pass -- shaper -> 3-D LUT fetch ->
  // un-shape"). Grades canvasView_ through a color/LutBake-baked
  // color::Lut3D and shaders/grade_blit.wgsl's trilinear-filtered 3-D
  // texture sample, into its own separate texture (gradedView_) --
  // exactly the same "never touch canvas_" discipline
  // updateGrayscalePreview() above already established, for the same
  // reason: canvas_ is the document's actual rendering, and grading here
  // is view state layered on top of it, not document state.
  //
  // *** Deliberate, narrow target: the live simulation canvas, not
  // core::Document *** -- this fork (grade sim::PaintSim's canvas_ vs.
  // grade a core::Document/TiledDocumentView) was presented explicitly
  // and decided in favour of the live canvas; do not redirect this to
  // core::Document without a fresh product decision. There is no live-
  // canvas-to-Document bridge in this codebase yet (see
  // AppState.hpp's CanvasView::grade / AppState::opStack comments, and
  // every prior Phase-2 step's Findings entries that hit the identical
  // gap) -- this method grades exactly what the painter sees, nothing
  // more.
  //
  // *** Domain contract *** -- see shaders/grade_blit.wgsl's own header
  // comment for the full reasoning, restated briefly here: composite.wgsl
  // hands back an already-clamped-to-[0,1], stylized, display-ish RGB,
  // not scene-linear HDR light the way color::Shaper/color::LutBake's
  // design otherwise assumes. This method still follows step 6's literal
  // "shaper -> 3-D LUT fetch -> un-shape" wording as a numerical
  // contract: the clamped canvas RGB is treated as shaperEncode's
  // "linear" input. Consequence, stated plainly: only the lower portion
  // of the LUT's domain (up to shaperEncode(1.0) ~= 0.5547945) is ever
  // actually sampled by this pass -- an accepted, documented result of
  // grading the SDR live canvas rather than a linear scene-referred
  // Document, not a bug.
  //
  // *** Narrow scope: only the OpStack's FIRST run is baked/blitted ***
  // `opStack.detectRuns()` can in principle return more than one run
  // (split at a non-PointA entry), but nothing in this codebase
  // constructs a real non-PointA op with meaningful behaviour yet --
  // core/OpStack.cpp's own header comment notes SpatialB/StrokeC/BakedD
  // are selftest-only placeholders -- so in practice there is at most one
  // run today. This method takes `runs.empty() ? <empty ops, identity> :
  // runs.front()` and does not attempt to composite multiple runs/
  // multiple LUT samples in one pass; a future step revisits this if a
  // second op class ever gains real behaviour.
  //
  // Rebakes the LUT only when `opStack.version()` has changed since the
  // last call (PLAN.md's "rebake on parameter change") -- the draw itself
  // (an O(1) LUT sample per pixel) is what stays cheap regardless of run
  // length; baking is what's allowed to cost more, and only pays that
  // cost when parameters actually changed. A no-op if pipelines/textures
  // aren't built yet, matching updateGrayscalePreview()'s own guard
  // shape. Callers (main.cpp, after this frame's PaintSim::frame() has
  // already run, exactly like updateGrayscalePreview()'s own ordering
  // requirement) call this once per frame only while
  // AppState::CanvasView::grade is on.
  void updateGradePreview(GpuContext& gpu, const OpStack& opStack);
  WGPUTextureView gradedView() const { return gradedView_; }

  // Blocking copy of gradedView_'s backing texture to RGBA8 host memory --
  // mirrors readbackCanvas() exactly (same format, same blocking-map
  // technique), reading graded_ instead of canvas_. Used by --selftest;
  // slow by design, keep it out of the frame loop.
  bool readbackGraded(GpuContext& gpu, std::vector<uint8_t>& out);

  int jacobiIterations = 40;
  int substeps = 2;
  // Ink gets its own count. The LBE spreads diffusively at nu = (1/omega-1/2)/3,
  // so reach grows only as sqrt(steps) — 2 substeps moves a mark ~5 px in a
  // couple of seconds, far short of the millimetre-scale bleed sumi needs. Its
  // passes are cheap (4 dispatches, no Poisson solve) so it can afford them.
  int inkSubsteps = 8;

  // Recompiles every pipeline from disk; returns false and keeps the working
  // pipelines if anything fails to compile.
  bool reloadShaders(GpuContext& gpu);

 private:
  bool buildPipelines(GpuContext& gpu);
  void frameWatercolor(GpuContext& gpu, WGPUComputePassEncoder cpass,
                       const WGPUBindGroupEntry& ub, uint32_t gx, uint32_t gy);
  void frameOil(GpuContext& gpu, WGPUComputePassEncoder cpass,
                const WGPUBindGroupEntry& ub, uint32_t gx, uint32_t gy);
  void frameInk(GpuContext& gpu, WGPUComputePassEncoder cpass,
                const WGPUBindGroupEntry& ub, uint32_t gx, uint32_t gy);
  void allocInkFields(GpuContext& gpu);
  void allocOilFields(GpuContext& gpu);
  void allocFields(GpuContext& gpu, uint32_t w, uint32_t h);
  void releaseFields();
  void generatePaper(GpuContext& gpu);


  // Bind groups depend on the current ping-pong parities, so they are built on
  // demand and memoised; only a handful of distinct combinations ever occur.
  WGPUBindGroup bindGroup(GpuContext& gpu, int passId, WGPUBindGroupLayout layout,
                          const std::vector<WGPUBindGroupEntry>& entries);

  uint32_t width_ = 0, height_ = 0;

  PaintMode mode_ = PaintMode::Watercolor;

  // Shared. `water_` is (u, v, p, M) for watercolour and (u, v, volume, contact)
  // for oil — same texture, different reading, since both are a velocity field
  // plus a scalar height and a coverage mask.
  PingPong water_, pigC_, pigR_, depC_, depR_, sat_, aux_;

  // Ink only: the D2Q9 lattice. lbmA = f0..f3, lbmB = f4..f7,
  // lbmC = (f8, surface water s, unused, unused).
  PingPong lbmA_, lbmB_, lbmC_;
  bool inkAllocated_ = false;

  // Oil only: the brush's own paint grid, mapped onto the footprint. IMPaSTo
  // keeps paint on the brush as well as the canvas; without it the brush never
  // runs dry and never picks colour up.
  PingPong brushVol_, brushC_, brushR_;
  bool oilAllocated_ = false;
  static constexpr uint32_t kBrushGrid = 64;
  WGPUTexture paper_ = nullptr;
  WGPUTextureView paperView_ = nullptr;
  WGPUTexture canvas_ = nullptr;
  WGPUTextureView canvasView_ = nullptr;
  // Grayscale preview target -- same size/format as canvas_, populated only
  // by updateGrayscalePreview(), never by frame()/composite. See that
  // method's declaration above for why it is a separate texture rather than
  // an in-place mutation of canvas_.
  WGPUTexture grayscale_ = nullptr;
  WGPUTextureView grayscaleView_ = nullptr;
  // Grade preview target (PLAN.md Phase 3 step 6) -- same size/format as
  // canvas_/grayscale_, populated only by updateGradePreview(), never by
  // frame()/composite. Same "separate texture, never mutate canvas_"
  // discipline as grayscale_ immediately above, for the identical reason.
  WGPUTexture graded_ = nullptr;
  WGPUTextureView gradedView_ = nullptr;

  WGPUSampler linear_ = nullptr;
  WGPUBuffer uniform_ = nullptr;

  // The Apply pass's baked LUT (PLAN.md Phase 3 step 6) -- rebaked only
  // when core::OpStack::version() changes (see updateGradePreview()'s own
  // doc comment above). Default-constructed (.texture == nullptr) until
  // the first call actually bakes one; hasBakedLut_ is the authoritative
  // flag for "is lut_ currently holding a real GPU resource," since a
  // default Lut3D and a released one are bit-identical (both null).
  Lut3D lut_;
  bool hasBakedLut_ = false;
  uint64_t lastBakedOpStackVersion_ = 0;

  // Indices into pipelines_, in the order buildPipelines() creates them.
  enum Pass { kPaper, kSplat, kUpdateVel, kDivergence, kJacobi, kProject,
              kFlowOutward, kAdvectWater, kAdvectPig, kTransferPig, kCapillary,
              kOilSplat, kOilVelocity, kOilAdvect, kOilTransfer, kOilBrush,
              kInkSplat, kInkStream, kInkCollide, kInkPigment,
              kTileOccupancy,
              kPassCount };
  WGPUComputePipeline pipelines_[kPassCount] = {};
  // The bridge's occupancy buffer and its host-visible staging copy. Sized
  // by allocFields() from the tile counts, so a resize reallocates both.
  WGPUBuffer occupancyBuf_ = nullptr;
  WGPUBuffer occupancyRead_ = nullptr;
  size_t occupancyBytes_ = 0;

  // The deferred tile payload. `readbackBuf_` stays alive until
  // endPigmentReadback(): the map callback holds a raw WGPUBuffer, and
  // releasing it before the callback fires surfaces the abort at an unrelated
  // later submit, naming the wrong code. app/Screenshot.hpp states the same
  // hazard from the write side.
  WGPUBuffer readbackBuf_ = nullptr;
  // Cached at init(), and the only GPU handle this class keeps -- every other
  // entry point takes a GpuContext. endPigmentReadback() needs it and cannot
  // be given one: it is called from shutdown(), which has no GpuContext of its
  // own, and from cancel paths where threading a context through would mean
  // changing every caller to serve one of them. See its implementation for
  // what it has to pump and why.
  WGPUInstance instance_ = nullptr;
  WGPUQueue queue_ = nullptr;
  size_t readbackBytes_ = 0;
  std::vector<BridgeTile> readbackTiles_;
  const float* readbackMapped_ = nullptr;
  PigmentReadback readbackState_ = PigmentReadback::Idle;
  // Written by the map callback, read by the poll. Not atomic: wgpu callbacks
  // fire from wgpuInstanceProcessEvents() on this thread, not from another.
  int readbackDone_ = 0;
  bool readbackOk_ = false;
  WGPURenderPipeline composite_ = nullptr;
  WGPURenderPipeline grayscalePipeline_ = nullptr;
  WGPURenderPipeline gradePipeline_ = nullptr;

  std::unordered_map<uint64_t, WGPUBindGroup> bindCache_;
};

// Construct-on-demand entry point (1.4 / ADR-0001: "idle, no document: 0
// MB" is a real state, so PaintSim -- ~193 MB minimum, before any Ink/Oil
// switch -- must not exist until something actually needs it). Returns the
// existing instance if `sim` is already non-null; otherwise constructs and
// initialises a fresh one. Returns nullptr, leaving `sim` null, if
// initialisation fails (missing Mixbox LUT, shader compile failure).
//
// Used identically by main.cpp's non-interactive flags (--selftest/--diag/
// --modes still want the sim built immediately, since that's the entire
// point of those flags) and by the interactive canvas (which calls this only
// once a paint tool actually starts depositing) -- one function, so there is
// exactly one place that knows how to build a PaintSim.
PaintSim* ensurePaintSim(std::unique_ptr<PaintSim>& sim, GpuContext& gpu,
                         uint32_t width, uint32_t height, const MixboxLut& lut);

}  // namespace np
