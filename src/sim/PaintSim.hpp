#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
  bool readbackField(GpuContext& gpu, WGPUTexture tex, WGPUTextureFormat format,
                     std::vector<float>& out);
  WGPUTexture depCTexForDiag() const { return depC_.srcTex(); }

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

  WGPUSampler linear_ = nullptr;
  WGPUBuffer uniform_ = nullptr;

  // Indices into pipelines_, in the order buildPipelines() creates them.
  enum Pass { kPaper, kSplat, kUpdateVel, kDivergence, kJacobi, kProject,
              kFlowOutward, kAdvectWater, kAdvectPig, kTransferPig, kCapillary,
              kOilSplat, kOilVelocity, kOilAdvect, kOilTransfer, kOilBrush,
              kInkSplat, kInkStream, kInkCollide, kInkPigment,
              kPassCount };
  WGPUComputePipeline pipelines_[kPassCount] = {};
  WGPURenderPipeline composite_ = nullptr;
  WGPURenderPipeline grayscalePipeline_ = nullptr;

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
