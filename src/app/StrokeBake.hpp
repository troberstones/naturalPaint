#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "app/DocumentLifecycle.hpp"
#include "sim/PaintSim.hpp"

namespace np {

// The floor below which a solver texel is not worth a tile write. It is the
// same 1e-4 `shaders/composite.wgsl` uses to decide a texel is worth drawing,
// and that is the point: a texel the solver would not have painted must not
// become a texel the document holds.
//
// In the header rather than the .cpp because `selectDryTiles()` defaults to it
// as well, applied there to a tile's *maximum* mass -- a tile whose best texel
// is below this floor would bake to nothing, and taking it would cost a
// readback and a history entry to write zero texels.
inline constexpr float kBakeMassFloor = 1e-4f;

// The bake: the solver's deposited pigment, written into a layer's tiles.
//
// This is the third and last arithmetic piece of the stroke bridge, and it
// does no arithmetic of its own. `sim/PaintSim`'s occupancy reduction says
// *which* tiles; its deferred readback supplies *what is in them*;
// `core/PigmentBake` says what a solver texel *becomes*. This joins the three
// and writes the result, and it lives in `app/` for exactly that reason --
// `core/` must not know about the solver and `sim/` must not know about
// documents, so the one place that knows both is here.
//
// --- What it deliberately does not do -------------------------------------
//
// It does not decide *when*. Bake-on-dry is a cadence question and the caller
// owns it: `PaintSim::readTileOccupancy()` reports mass and wetness together
// precisely so a caller can apply its own rule and hand the chosen tiles here.
// Baking a still-wet tile is not refused -- a forced bake on undo-while-wet is
// a real case -- but it silently drops whatever is still suspended, so the
// result says how many tiles were wet at the time and the caller can decide
// whether that mattered.
//
// It does not clear the sim, and it does not record history. Both belong to
// the frame sequence around it, and folding either in here would make a bake
// that is *observed* impossible to write.

struct BakeResult {
  size_t tilesWritten = 0;
  size_t texelsWritten = 0;
  float peakCoverage = 0.0f;
  // Tiles whose solver texels were all below the mass floor. Not an error:
  // the occupancy pass names a tile when its *maximum* is worth baking, and a
  // tile can hold one loaded texel and 16383 empty ones. Reported so a caller
  // that expected paint and got none can tell that apart from a failure.
  size_t tilesEmpty = 0;
};

// Writes one tile of solver texels into `out`. `depC` and `depR` are each
// 128*128*4 floats in row order -- exactly what
// `PaintSim::pigmentReadbackDepC()` hands back. Returns the number of texels
// that carried enough mass to write.
//
// Texels below the floor are left untouched rather than written as
// transparent, which matters: a tile is allocated on write, and a bake that
// stamped zeros over an existing tile would erase paint that was already
// there instead of adding to it.
size_t bakePigmentTileFrom(const float* depC, const float* depR, float absorption,
                           PigmentTile& out);

// The whole bake, for a readback that is `Ready`. Walks the tiles the readback
// was issued for, converts each, and writes it into `layer`'s pigment tiles.
// Returns a zeroed result if the layer is not a Pigment layer or the readback
// is not ready -- both are caller errors, and neither can be repaired here.
BakeResult bakePigmentTiles(const PaintSim& sim, Layer& layer, float absorption);

// ==========================================================================
// The frame sequence
// ==========================================================================
//
// Everything above is arithmetic with no opinion about *when*. This half is
// the opinion, and it exists because the three calls it sequences
// (`beginPigmentReadback`, `bakePigmentTiles`, `clearBakedTiles`) have an
// ordering requirement that is invisible from any one of them.
//
// --- 1. Why the order matters, concretely ---------------------------------
//
// `ui/MacPaintUI` draws the solver canvas and the document texture as **two
// quads, stacked** -- the document over the paper. That is only invisible
// today because the document stays empty while painting. A bake makes both
// hold the same paint for as long as it takes the clear to catch up, so a
// sequence that lets a frame be presented in between shows every dried
// stroke **twice**, at double density.
//
// The reverse mistake is worse and easier to make. `DocumentTexture` caches
// on `OpenDocument::revision`, and `viewFor()` runs inside `drawUI()`. So a
// bake performed *after* `drawUI()` uploads nothing this frame -- the cache
// key has not moved yet -- while `clearBakedTiles()`'s queue write does land
// before present, because a texture view samples whatever the GPU last wrote
// into it. The paint would leave the solver a full frame before it arrived in
// the document: a one-frame **dropout**, which reads as a flicker on every
// stroke that dries.
//
// Both failures come from the same cause and have the same fix:
//
//   **`step()` must be called before `drawUI()`, once per frame.**
//
// Then the bake bumps the revision before `viewFor()` reads it, so the
// document uploads *this* frame, and the clear is submitted well before
// present, so the solver is empty in the same presented image. Neither
// picture is ever a frame ahead of the other. The call site in `main.cpp`
// says this too, because a future edit that moves it below `drawUI()` will
// look harmless and will not be.
//
// --- 2. Why the readback is deferred, and what that costs -----------------
//
// `beginPigmentReadback()` is a fence-and-poll machine: a tile is 256 KiB per
// field and blocking on it measured 3.288 ms, 16% of PRD F3's 20 ms, on a
// frame where the user is still painting. So a cycle spans **two** frames --
// scan and submit on one, bake and clear on the next -- and the poll has
// never had to wait (measured 0.013 ms, ready 200/200 times one frame later).
//
// The consequence to hold on to: the solver keeps rendering the paint for one
// extra frame after it was found dry. That is correct, not a compromise. The
// paint is still genuinely in the solver until the clear runs.
//
// --- 3. What is deliberately NOT here -------------------------------------
//
// **The cadence is one bake, one history entry -- not one stroke, one entry.**
// `OpenDocument::recordEdit()` appends to history, and a wash that dries in
// three batches therefore produces three entries where a painter expects one.
// Undo-while-wet, which has to force a bake before it can undo anything, is
// not handled either. Both belong to the deferred-history step and neither is
// repairable from inside a per-frame cycle -- collapsing entries needs to know
// a stroke ended, which is `app/StrokeSession`'s knowledge, not this file's.
// Stated here rather than discovered later.
//
// **Oil never bakes.** `kAbsorption*` are Beer-Lambert coefficients and oil is
// not a Beer-Lambert medium -- it is a height field with impasto lighting, its
// deposited field is always zero (README known bug 1), and `absorptionFor()`
// refuses it rather than picking a number that would look plausible.

// The Beer-Lambert coefficient for a medium, or `std::nullopt` for one that
// has none. One table, so the UI, the cycle and `--selftest` cannot disagree
// about what oil bakes at.
std::optional<float> absorptionFor(PaintMode mode);

// Which tiles a bake should take, from one `readTileOccupancy()` result.
//
// Pure, and separated from the cycle for exactly that reason: "dry enough and
// loaded enough" is the whole policy, and it is the part worth testing against
// hand-built occupancy rather than against a real solver.
struct DryTileScan {
  std::vector<PaintSim::BridgeTile> ready;
  // Tiles holding paint that are still wet. Not an error and not a backlog --
  // they are simply not finished. Reported so a caller can tell "nothing to
  // bake" apart from "nothing has dried yet", which are different states.
  size_t wetHeld = 0;
};

// `massFloor` is compared against the tile's *maximum* mass, which is what the
// occupancy reduction reports, so a tile is taken when any texel in it is
// worth writing. `wetnessFloor` is an upper bound: at or below it, the tile is
// dry. Both default to the values the rest of the bridge already uses.
DryTileScan selectDryTiles(const std::vector<PaintSim::TileOccupancy>& occupancy,
                           uint32_t tileCountX, uint32_t tileCountY,
                           float massFloor = kBakeMassFloor,
                           float wetnessFloor = 0.0f);

// One turn of the two-frame cycle, for reporting and for `--selftest`.
struct BakeCycleReport {
  enum class Action {
    Idle,        // nothing dry, or nothing to do
    Submitted,   // a readback was issued for `tiles` this frame
    Baked,       // a readback came back and was written to the document
    Refused,     // something was wrong; `why` says what
  };
  Action action = Action::Idle;
  size_t tiles = 0;
  size_t wetHeld = 0;
  BakeResult bake;
  const char* why = "";
};

// The state machine. Holds the tile list between the submit frame and the bake
// frame so a caller cannot get the two out of step -- which is the one bug
// `PaintSim::bridgeTileAt()` exists to prevent and this makes unreachable.
class StrokeBakeCycle {
 public:
  // Call once per frame, **before `drawUI()`** -- see section 1 above.
  //
  // `doc` may be null and the active layer may be any kind; both are ordinary
  // states rather than errors, and both report `Refused` with a reason instead
  // of baking somewhere wrong. Nothing is cleared from the solver unless the
  // paint reached a layer first, so a refusal loses no work: the paint stays
  // wet-side and the same tiles are offered again next frame.
  BakeCycleReport step(GpuContext& gpu, PaintSim& sim, OpenDocument* doc, PaintMode mode,
                       uint64_t frameIndex);

  // Frames between occupancy scans while idle. The scan is blocking but cheap
  // (measured 0.129 ms, a few hundred bytes at the transfer floor); at 60 Hz
  // this is about four scans a second, against a drying time measured in
  // seconds. Every frame would spend 0.6% of PRD F3's budget re-answering a
  // question whose answer changes on a human timescale.
  static constexpr uint64_t kScanIntervalFrames = 15;

  bool readbackInFlight() const { return !inFlight_.empty(); }

 private:
  std::vector<PaintSim::BridgeTile> inFlight_;
  uint64_t lastScanFrame_ = 0;
  bool scanned_ = false;
};

}  // namespace np
