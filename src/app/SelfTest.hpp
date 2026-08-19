#pragma once
#include <cstddef>

#include "gfx/Context.hpp"
#include "paint/Palette.hpp"
#include "sim/PaintSim.hpp"

namespace np {

// Headless check: paints a Hansa Yellow stroke, lets it settle, then drags
// Phthalo Blue across it and samples the overlap.
//
// This is the one assertion that matters for the whole design. Pigment is
// transported in Mixbox latent space, where linear operations are Kubelka-Munk
// mixes, so wet blue over wet yellow must read GREEN. An RGB solver averaging
// #0D1B44 with #FCD300 gives a muddy grey — if this test sees grey, the latent
// pipeline is broken somewhere between the splat and the composite.
//
// Writes the canvas to `outPng` for eyeballing. Returns true if the overlap is
// green by a clear margin.
bool runSelfTest(GpuContext& gpu, PaintSim& sim, const MixboxLut& lut,
                 const char* outPng);

// Lays one wet pigmented blob, then runs `seconds` of simulation at 60 Hz,
// reporting pigment mass, wet area, and flow speed as it goes. Written to answer
// a specific question: why does the spreading water lose its colour?
// Renders the same pair of strokes in every medium, so the three models can be
// compared side by side and each is exercised at least once.
void runModeTest(GpuContext& gpu, PaintSim& sim, const MixboxLut& lut,
                 const char* outPrefix);

void runDiagnostic(GpuContext& gpu, PaintSim& sim, const MixboxLut& lut,
                   float seconds, const char* outPngPrefix);

// Headless, GPU-free check on the fixed-timestep accumulator (PRD H7,
// app/FixedStep.hpp). --diag and the rest of --selftest never exercise it —
// they call PaintSim::frame() directly in a plain frame-count loop with no
// wall clock involved, so they'd pass unchanged whether or not the
// accumulator worked at all. This is the actual test of the new code that
// runs in main.cpp's interactive loop.
bool runAccumulatorTest();

// Headless, GPU-free check on color/Space's sRGB and Rec.709 transfer
// functions (Phase 2.3). Colour-transfer-function bugs are exactly the
// kind that silently corrupt everything downstream -- every pixel that
// crosses the import/export boundary goes through these -- without ever
// producing an obviously-wrong picture, so this is worth a permanent gate
// rather than a one-off scratch check: round-trips encode/decode across a
// spread of values (negative, zero, near each curve's toe breakpoint, mid-
// range, and above 1.0 to confirm HDR-ish linear values are intentionally
// left unclamped) for both curves and checks decode(encode(x)) == x and
// encode(decode(x)) == x within float tolerance.
bool runColorSpaceTest();

// Headless, GPU-free check on color/Shaper (Phase 3 step 1, ADR-0004). This
// is the *grading* transfer function -- the shaper's log domain curves are
// authored in, per ADR-0004's "format-level commitment" callout -- as
// opposed to runColorSpaceTest() above, which covers the *display* transfer
// functions. Three things, at the rigor ADR-0004 demands for its own
// self-declared hardest-to-reverse decision:
//  - Continuity at the breakpoint: the linear-segment and log2-segment
//    formulas are evaluated directly here (re-typed from the published
//    ACEScct spec, independent of Shaper.cpp's own copy of the constants),
//    checked to agree with each other and with the real shaperEncode() --
//    proving the constants are genuinely self-consistent, not merely
//    plausible-looking.
//  - Round trip: decode(encode(x)) == x and encode(decode(y)) == y across
//    negative, zero, either side of the breakpoint, 0.18 (18% grey),
//    1.0 exactly, and 2.0/4.0/16.0 (the HDR-headroom property -- values
//    well above 1.0 still land inside [0,1] once shaped).
//  - A known-value sanity check independent of the code's own internal
//    consistency (shaperEncode(1.0) == 9.72/17.52 ~ 0.5547945), and
//    monotonicity over a sorted sampled spread -- a non-monotonic
//    log-domain shaper would silently corrupt curve editing.
bool runShaperTest();

// Pins ADR-0003 (1.3): paints the same straight-line path twice at very
// different simulated stroke speeds and checks the deposited pigment mass
// matches within tolerance. See SelfTest.cpp for why this, and not
// --diag or the accumulator test, is the assertion that actually exercises
// speed-independence.
bool runStrokeSpeedTest(GpuContext& gpu, PaintSim& sim, const MixboxLut& lut);

// 1.4 / ADR-0001 bullet 5: asserts idle RSS stays under the ~294 MB the sim
// used to cost unconditionally at startup. `idleRssBytes` is measured by the
// caller (main.cpp), immediately after SDL/window/GPU setup and *before*
// PaintSim is constructed anywhere -- by the time any --selftest code runs,
// a PaintSim already exists (--selftest needs one to exercise the solver at
// all), so this function can't take its own measurement and call it "idle";
// it has to be handed the one true idle sample instead.
bool runIdleMemoryTest(size_t idleRssBytes);

// 1.4 / ADR-0001 bullets 2 and 3. Two things, both about the same invariant:
// (a) right after PaintSim::init(), still the default Watercolour mode, the
// ink lattice and oil brush grid are genuinely absent -- constructing the
// sim never allocates media it wasn't asked to run; (b) switching modes
// frees the *outgoing* medium's fields, not just allocates the incoming
// one's -- so a session that visits all three media never holds more than
// one's worth of optional fields at a time, matching ADR-0001's per-mode
// residency table. Mutates `sim`'s mode via setMode() and leaves it back in
// Watercolour when done.
bool runFieldAllocationTest(GpuContext& gpu, PaintSim& sim);

// Headless, GPU-free check on app/Keymap (Phase 2 step 15, PRD R7/R8).
// Loads the real shipped keymaps/default.json and confirms it has no
// false-positive conflicts and resolves its real bindings to the expected
// action names; then loads a small in-memory fixture keymap (not the
// shipped file -- see SelfTest.cpp for why) built specifically to exercise
// the layer-kind-scope-aware conflict detector: two bindings on the same
// key with disjoint, specific layer-kind scopes must NOT be reported as
// conflicting, two bindings on the same key with overlapping (global or
// identical-scope) scope MUST be, and resolve() must return the
// scope-correct action for a scoped-only binding.
bool runKeymapTest();

// Headless, GPU-free check on core/Half (the shared half<->float codec,
// factored out of sim/PaintSim.cpp) and core/TileStore (Phase 2 step 2).
// See SelfTest.cpp for the full breakdown; in short: direct round-trip
// cases for both Half directions (zero, negative zero, subnormals,
// ordinary values, highest/lowest finite half magnitudes), plus
// TileStore's three PLAN.md-mandated behaviours -- allocate on write,
// query without allocating, iterate exactly the occupied tiles -- and a
// negative document coordinate to exercise Tile.hpp's floor semantics
// rather than only the positive-coordinate happy path.
bool runTileStoreTest();

// Headless, GPU-free check on io/ImageDecode (PLAN.md Phase 2 step 6's
// decode half). Generates small in-memory PNG (8-bit and 16-bit), BMP, TGA
// and JPEG fixtures with known pixel values via stb_image_write (the 16-bit
// PNG fixture is hand-built instead -- stb_image_write's PNG writer is
// 8-bit-only, see the helper in SelfTest.cpp), decodes each through
// decodeImageLinear(), and checks the decoded pixels land near their
// expected linear-light value: known sRGB-encoded corners decode to the
// srgbDecode() of their normalized byte/word value, alpha passes through
// unencoded, and sources without an alpha channel come back fully opaque.
// This is the PLAN.md verify criterion for this phase: open both an 8-bit
// and a 16-bit PNG and check known pixel values.
bool runImageDecodeTest();

// Headless, GPU-free check on core/Document + core/Layer (PLAN.md Phase 2
// step 4: "layer list holding exactly one entry, with the kind enum from
// CONTEXT.md present but only RGB implemented. Design for N, ship 1.").
// Confirms: LayerKind has all seven CONTEXT.md-named values and
// layerKindName()/layerKindFromName() (relocated here from app/Keymap in
// this same step) round-trip every one; a default-constructed Layer's kind
// is Pigment (CONTEXT.md's domain default) with no RGB tile storage
// populated; a Document holds the width/height/working-space it was built
// with and starts with an empty layer list; and a one-entry layer list
// holding an RGB-kind Layer round-trips a pixel through its
// std::optional<TileStore> via the already-tested core::TileStore API.
bool runDocumentTest();

// Headless, GPU-free check on PLAN.md Phase 2 step 14 / PRD C16: "the base
// layer is an ordinary layer with alpha, no locked Background." Confirms a
// createBlank()'d document's one layer writes and reads back both a fully
// transparent (alpha=0) and an arbitrary partial-alpha pixel through the
// ordinary TileStore API, unclamped -- the two behaviours a locked/special
// Background layer would do differently, neither of which core/Layer.hpp
// implements (it has no background/locked concept at all).
bool runBaseLayerAlphaTest();

// Headless, GPU-free check on Document::createBlank() (PLAN.md Phase 2 step
// 5, PRD C7: "A document can be created blank, not only opened from a
// file."). Confirms: the returned Document holds the given width/height/
// working-space; its layer list has exactly one entry; that layer is
// RGB-kind with `rgbTiles` populated (per core/Layer.hpp's "populated only
// when kind == RGB" contract); and -- the assertion that actually catches a
// wrong implementation, per PRD C2's "memory tracks content, not canvas
// dimensions" -- `rgbTiles->occupiedTileCount() == 0` immediately after
// creation, checked at both a small size and a large (4096x4096) one, so a
// version that happens to pass by accident at a tiny size but silently
// pre-allocates a tile grid at a large one is still caught.
bool runCreateBlankTest();

// Headless, GPU-free check on io/ImageIO (PLAN.md Phase 2 step 6's remaining
// half, on top of io/ImageDecode's decode-only piece: premultiply + pack a
// decoded image into a Document's tiles). Confirms: opening a small,
// known-pixel-values PNG produces a Document whose width/height match the
// image, whose one layer is RGB-kind, and whose tiles hold the expected
// *premultiplied* values -- the fixture has alpha < 1 corners specifically,
// so premultiplied and straight alpha are distinguishable and the test
// would fail if premultiplication were silently skipped; a multi-tile-
// spanning image's occupied tile count matches its footprint in 128x128
// tiles, not anything proportional to a larger nominal canvas (PRD C2, this
// phase's own verify criterion); writeDecodedImageIntoLayer() -- the
// separately-callable piece PLAN.md step 13's "place an image as a layer"
// will reuse -- round-trips a pixel against a hand-built Layer directly,
// not only reached through openImageAsDocument(); and corrupt/truncated
// file bytes return std::nullopt cleanly rather than crashing or producing
// a bogus Document.
bool runImageIOTest();

// Headless, GPU-free check on io/ImageIO's placeImageAsLayer() (PLAN.md
// Phase 2 step 13's Document-level slice, PRD I14: "place an image as a
// layer into the open document... distinct from opening a file, which
// creates a document" -- the menu/drag-drop UI itself is out of scope here;
// see PLAN.md for why). Confirms: placing into a Document::createBlank()
// document leaves the original base layer at index 0 untouched and appends
// the placed image as a second, RGB-kind layer at the end (top of the
// stack) with premultiplied pixel content matching the source image;
// placing into a Document that starts with zero layers still ends up in a
// sane one-layer state; the DecodedImage-taking overload works directly,
// not only through the file-bytes overload; and corrupt/invalid input
// (garbage file bytes, or an invalid DecodedImage) fails cleanly, forwarding
// an error string where the signature has one, and leaves doc.layers
// completely unchanged rather than partially inserting a broken layer.
bool runPlaceImageAsLayerTest();

// Headless check on ui/NaturalPaintUI (PLAN.md Phase 2 step 8, "Tiled
// viewport draw"): a read-only proof of the tile pipeline, Document -> one
// GPU texture per occupied tile -> screen, independent of the interactive
// painting canvas. Needs `gpu` (a real device/queue) but no PaintSim -- this
// module never touches the solver.
//
// Confirms: a Document with no RGB layer, and a freshly-createBlank()'d one
// (an RGB layer with zero occupied tiles), both upload zero tiles and
// TiledDocumentView::draw() no-ops safely on both, even given a null
// ImDrawList (the one call in this module that needs a live ImGui context --
// deliberately not exercised here, see below); tileScreenRect() matches a
// hand-computed expectation for a known TileCoord/CanvasView/canvasOrigin,
// independent of anything GPU-side; and -- the assertion that actually
// proves the pipeline, not just its parts -- opening a small known-pixel PNG
// fixture, uploading its one tile, then driving a small dedicated offscreen
// WebGPU render pass (not ImGui's renderer -- see SelfTest.cpp for why) that
// places the uploaded tile texture at tileScreenRect()'s own computed rect,
// reads the render target back to CPU the same way PaintSim::readbackField()
// does, and checks known source-image corner colours land at the expected
// screen pixels, with untouched tile area and the area outside the tile's
// quad both reading back transparent black.
bool runTiledViewportTest(GpuContext& gpu);

// Headless check on ui/NaturalPaintUI's mip pyramid (PLAN.md Phase 2 step 9:
// "Mip pyramid for tiles, so a 25% zoom evaluates at a matching level").
// Three things, in order:
//
//  (a) CPU-only, no GPU: buildMipChain() run on a hand-built core::Tile
//      whose R channel holds a known 4x4 ramp. Mip level 1's four corner
//      texels are checked against the hand-computed 2x2 box-filter average
//      of level 0's ramp, and mip level 2's corner texel against the
//      hand-computed average of level 1's own (already-downsampled) four
//      values -- proving the chain is genuinely recursive (each level
//      downsamples the level above it, not the original), not just correct
//      for level 0->1.
//  (b) CPU-only, no GPU: mipLevelForZoom() checked at a spread of zoom
//      values, including PLAN.md's own literal example (zoom=0.25 -> the
//      32px level, mip 2) and zoom=1.0 -> level 0, plus clamping at both
//      extremes (very high zoom stays at level 0; very low zoom clamps at
//      the smallest level rather than going out of range).
//  (c) End-to-end, needs `gpu`: a known, non-uniform (finest-period
//      checkerboard) tile is uploaded via TiledDocumentView::setDocument(),
//      then drawn at zoom=0.25 through the same offscreen blit-and-readback
//      technique runTiledViewportTest() uses (a shared helper, extended
//      rather than duplicated), binding the level-2 view mipLevelForZoom()
//      selects. The read-back pixels are checked against level 2's known
//      uniform downsampled colour, and -- rendering the identical screen
//      rect from level 0's own view for contrast -- checked to differ from
//      what level 0 alone would have produced. This is the assertion that
//      actually proves level selection is wired into the real GPU draw
//      path, not just computed and ignored.
bool runMipPyramidTest(GpuContext& gpu);

// Headless, GPU-free check on core/Probe (PLAN.md Phase 2 step 10: "Pixel
// probe reporting both linear and display values... Sample size and
// sample-all-layers are parameters of the sample, not separate tools").
// Pure CPU -- probePixel() only ever reads a Document's tiles, no PaintSim
// or gpu involvement.
//
// Confirms: a point sample (sampleSize=1) at a known-pixel fixture returns
// the exact stored linear value and its srgbEncode()-matching display
// value; an NxN box sample over a fixture with distinct, known per-pixel
// values returns the correct average (checked two ways -- a fully-painted
// interior box, isolating averaging correctness from premultiply/bounds
// concerns, and a box straddling painted and never-painted texels, which
// proves missing texels dilute the average toward transparent rather than
// getting silently clamped/repeated to the nearest painted pixel); probing
// a translucent (alpha < 1) pixel proves un-premultiplication actually
// ran, by checking the reported straight colour against the raw
// premultiplied value read directly off the tile, not just against a
// plausible-looking number; and probing a coordinate whose tile was never
// allocated, or an out-of-range activeLayerIndex, or a Document with no
// RGB layer at all, all return ProbeSample{} (fully transparent black)
// rather than crashing or reading garbage.
bool runProbeTest();

// Headless check on the unified view transform (PLAN.md Phase 2 step 11,
// "View controls" -- PRD Q1-Q4; docs/shortcuts.md section 3's own mandate
// that mirror and rotation compose into one matrix and pen input maps back
// through its actual inverse). Three things:
//
//  (a) round-trip identity -- toCanvas(toScreen(p)) == p within a tight
//      float tolerance -- across a spread of zoom/pan/mirrorX/mirrorY/
//      rotation combinations, including at least one with both mirrors and
//      a non-zero rotation together. This is the concrete proof that "pen
//      input maps back through its inverse" actually holds, not merely that
//      the code compiles.
//  (b) one fully hand-worked combination (zoom=2, mirrorX, a 90-degree
//      rotation, an off-centre pivot) checked against an exact
//      hand-computed screen point -- not just "it round-trips" but "it
//      round-trips to the *correct* place," matching the rigor of
//      NaturalPaintUI's own tileScreenRect() test.
//  (c) view-only: toggling mirrorX/mirrorY/rotation/grayscale never mutates
//      PaintSim's own canvas texture. PLAN.md's literal check -- "mirror
//      both axes, save, reopen -- the file is unmirrored" -- can't be
//      exercised yet (no document-save path exists in this codebase), so
//      this is the closest available headless proxy: a readbackCanvas()
//      before and after, with nothing painted in between, bytewise
//      identical. Needs a live `sim`/`gpu` for this part only; (a) and (b)
//      are pure CPU math and need neither.
bool runViewTransformTest(GpuContext& gpu, PaintSim& sim);

// Headless, GPU-free check on app/Snapping.hpp (PLAN.md Phase 2 step 12,
// "Rulers, guides, grid and snapping" -- PRD Q5-Q7). Rulers, live
// drag-to-create, the "Add Guide" popup and the grid overlay itself are UI
// (ui/MacPaintUI.cpp) that no tool exists to drive with real mouse/keyboard
// input against this native app's window -- see runViewTransformTest()
// above for the same limitation, worked around the same way. What's
// genuinely testable headlessly, and what this covers:
//
//  - gridLinePositions()/isMajorGridLine(): given a spacing and subdivision
//    count, the set of grid-line positions in a range is exactly what
//    you'd hand-compute (e.g. spacing=100, subdivisions=4 over [0,100] is
//    exactly {0,25,50,75,100}), including a range that doesn't start at 0
//    (the grid still anchors to document-space 0, not the range's own
//    start) and subdivisions=1 (major lines only).
//  - parseGuidePosition(): a plain number, a percentage, surrounding
//    whitespace, and unparseable text.
//  - resolveSnap(), the function that actually matters (PRD Q6): a point
//    near a guide snaps exactly onto it; a point far from every guide,
//    grid line and canvas edge is left unchanged; a point near a grid line
//    snaps to that grid line while an unrelated axis on the same point
//    stays untouched (axes snap independently); points near the canvas's
//    near and far edges snap to them; and a non-positive threshold -- the
//    global snapping toggle's "off" state -- snaps nothing at all, even a
//    point exactly on a guide.
bool runGuidesGridSnapTest();

// Headless, GPU-free check on core/Histogram (PLAN.md Phase 3 step 7:
// "Histogram over the visible region"). Pure CPU -- computeHistogram() only
// ever reads a Document's tiles, no PaintSim or gpu involvement.
//
// Confirms: an empty/all-transparent Document produces all-zero bins and
// sampleCount == 0; a small synthetic Document built directly via
// Document::createBlank() + TileStore::getOrCreate()/Tile::writePixel()
// (matching runProbeTest()/runMipPyramidTest()'s own fixture-construction
// pattern) with a handful of hand-picked opaque pixels lands in exactly the
// expected R/G/B/Luma bins with every other bin at zero, and a mixed-in
// alpha == 0 texel contributes to none of them; a region narrower than one
// allocated tile excludes pixels inside that same tile but outside the
// region; HistogramParams::wholeDocument() spans exactly {0,0} to
// {width,height}; and a translucent (partial-alpha) pixel bins at its
// un-premultiplied straight colour, not its stored premultiplied value --
// checked against a specific hand-computed case (premultiplied
// (0.25, 0, 0, 0.5) un-premultiplies to linear (0.5, 0, 0), then
// srgbEncode()d to find the expected bin).
bool runHistogramTest();

// Headless, GPU-free check on ops/PointOps (PLAN.md Phase 3 steps 2+3;
// docs/operations.md §1.1; PRD B4). Pure CPU math, no PaintSim or gpu
// involvement -- every op under test is a plain `rgb -> rgb` function by
// ADR-0004's own design (see ops/PointOps.hpp's header comment).
//
// Covers all six point ops plus the premultiply wrapper:
//  - Levels: the neutral-params case is a true no-op; a hand-computed
//    non-trivial case; an input below blackIn does not produce NaN
//    (the internal `t` clamp works).
//  - Curves: 0/1 control points is identity with no shaper round-trip; 2
//    points reduces the Hermite formula exactly to the straight line
//    between them (checked at several interior x, not just the
//    endpoints); a hand-computed 3-point interior case; flat
//    extrapolation on both sides of the authored x-range; the (0,0)-(1,1)
//    shaper-domain-identity line leaves a spread of linear inputs
//    unchanged end to end (shaperEncode -> evalCurve -> shaperDecode
//    round-trips).
//  - Exposure: +1 stop doubles, -1 stop halves, 0 stops is identity.
//  - Saturation: scale=1 is identity; scale=0 collapses every channel to
//    the same value (the Rec.709 luma); a hand-computed non-trivial case.
//  - Grayscale: pure red (1,0,0) -> (0.2126, 0.2126, 0.2126), hand-
//    computable from kRec709LumaWeights directly; a general RGB case.
//  - Channel mixer: the identity matrix is a no-op; a hand-computed
//    channel-swap case and an offset case.
//  - applyPointOpsPremultiplied(): alpha == 0 maps to {0,0,0,0} untouched
//    regardless of what ops would otherwise do; a hand-computed partially
//    transparent example (premultiplied (0.5,0,0,0.5) -> unpremultiply ->
//    (1,0,0) -> +1 stop exposure -> (2,0,0) -> re-premultiply by the
//    unchanged alpha 0.5 -> (1,0,0,0.5)); alpha is never altered by any
//    op run through the wrapper; composing two or more ops in sequence
//    matches manual hand computation, not just each op individually.
bool runPointOpsTest();

}  // namespace np
