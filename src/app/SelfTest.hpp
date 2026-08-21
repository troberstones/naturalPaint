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
//
// The ceiling is 80 MB for the dependency-free build, unchanged from phase
// 1's revised number. A NP_USE_OIIO=ON build does not fit under it -- the
// OpenImageIO dylib chain costs a measured 29.5 MB before main() starts --
// so that configuration gets a separate, additive, separately-*printed*
// 32 MB allowance rather than a raised single number. SelfTest.cpp carries
// the full measurement and why PLAN.md step 6's lazy init cannot recover it.
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
// PNG fixture goes through io/Export.hpp's encodePng16() instead --
// stb_image_write's PNG writer is 8-bit-only, and that hand-rolled 16-bit
// writer now lives in the production export module rather than in this
// file, so the fixture and PRD B6's real 16-bit export share one writer),
// decodes each through
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

// Headless, GPU-free check on core/OpStack (PLAN.md Phase 3 step 5:
// "ordered ops, dirty tracking, run detection for the collapse"). Pure CPU
// bookkeeping plus calls into the already-tested ops/PointOps functions --
// no PaintSim or gpu involvement anywhere in this function.
//
// Covers:
//  - An empty OpStack: detectRuns() returns no runs; version() starts at 0.
//  - Every mutator (add, remove, reorder, setEnabled, setOp) increments
//    version() by exactly one, including a setEnabled()/setOp() call that
//    doesn't change anything observable (OpStack.hpp's documented
//    over-bumping-is-fine policy); at()/size() -- read-only -- never do.
//  - An all-PointA stack (Exposure then Saturation): detectRuns() returns
//    exactly one run spanning the whole stack, and running a sample RGB
//    value through the run's composed PointOp list by hand matches calling
//    ops::applyExposure() then ops::applySaturation() directly, in that
//    order.
//  - A disabled PointA entry in the middle of an otherwise-PointA stack:
//    the run still spans all three indices undivided (a disabled op does
//    not split a run -- op class, not enabled state, is what defines a
//    boundary), and its composed op list has only the two enabled entries'
//    functions -- verified by running a value through and confirming it
//    matches skipping the disabled op entirely.
//  - A real class boundary (PointA, a non-PointA placeholder Op built only
//    as a test fixture, PointA again): detectRuns() returns exactly two
//    runs split at the placeholder, each containing only its own side's
//    PointA op.
//  - reorder(): Exposure and Levels (gamma != 1) are first checked to
//    genuinely disagree depending on which runs first (unlike two purely
//    linear ops, which always commute with a uniform Exposure multiply) --
//    so the test cannot pass vacuously -- then reordered, with
//    detectRuns()'s composed op list checked to reflect the new order by
//    hand-running a value through it.
//  - remove(): removing an entry shifts every later run's indices down by
//    one, and the composed op list of the run that follows the removed
//    entry no longer includes its effect.
bool runOpStackTest();

// Headless check on color/LutBake (PLAN.md Phase 3 step 4, ADR-0004): bakes
// a maximal run of adjacent point ops onto a 32^3 rgba16float 3-D LUT via a
// seed compute dispatch plus one dispatch per op, entirely on the GPU, and
// checks the baked result against the CPU ops/PointOps reference at a
// handful of hand-picked grid cells. Needs `gpu` for a real device/queue --
// genuine compute-shader work, no PaintSim involvement (this module never
// touches the solver).
//
// See SelfTest.cpp for the full breakdown; in short:
//  - Each of the six point-op kinds baked alone (a one-op run) and checked
//    against shaperEncode(clamp01(op(shaperDecode(gridCoord)))), computed
//    with the real ops/PointOps functions and the same params fed to the
//    GPU -- but compared against a CPU reference that itself simulates the
//    exact half-precision write/read the value undergoes at each ping-pong
//    stage (core::floatToHalf/halfToFloat, once for the seed write and once
//    per op-pass write), not the raw float math, since the real bake
//    round-trips through rgba16float storage at every stage and a
//    pure-float reference would be comparing against a value the GPU could
//    never actually produce. See SelfTest.cpp for the exact residual
//    tolerance chosen on top of that simulated reference and why.
//  - Curves specifically (the highest-risk kernel) at three distinct curve
//    shapes: a 2-point straight line, a 3-point interior case, and a point
//    count at kMaxCurvePointsPerChannel.
//  - A composed 3-op run (Exposure -> Saturation -> Channel mixer), baked
//    in one call and checked against running the same sequence through
//    ops::PointOps directly, in order, on the CPU -- the GPU analogue of
//    runOpStackTest()'s own composition proof, and the one case that
//    actually exercises the ping-pong sequencing rather than any single
//    kernel in isolation.
bool runLutBakeTest(GpuContext& gpu);

// Headless check on the Apply pass (PLAN.md Phase 3 step 6, "shaper ->
// 3-D LUT fetch -> un-shape"): sim::PaintSim::updateGradePreview() +
// shaders/grade_blit.wgsl, targeting the live simulation canvas (a
// deliberate fork decision -- see PaintSim.hpp's own doc comment on
// updateGradePreview()). Needs a real `gpu`/`sim` -- the same PaintSim
// instance every other PaintSim-backed --selftest case in this file
// already shares.
//
// See SelfTest.cpp for the full breakdown; in short:
//  - Builds a one-op OpStack (Saturation, scale 0.3 -- the same value
//    ui/MacPaintUI.cpp's "Test Grade (debug)" toggle uses), bakes and
//    blits it via updateGradePreview() over a known, deterministic
//    unpainted canvas (blank paper substrate only -- sim::PaintSim's
//    procedurally generated paper_ field never changes after init()),
//    and checks the graded readback at a handful of hand-picked canvas
//    pixels against an independent CPU reference.
//  - That CPU reference computes the *exact* trilinear interpolation of
//    the LUT's 8 surrounding corner texels (each corner independently
//    simulating the same half-precision ping-pong round trip
//    runLutBakeTest()'s own simulateBakeCpu() does), rather than only
//    comparing against an exact-grid-cell value -- see SelfTest.cpp for
//    why: sim::PaintSim's canvas_ is only reachable through the real
//    solver/composite pipeline, so unlike runLutBakeTest() (which can
//    freely pick any LUT grid cell to check), there is no way to force
//    an arbitrary, precisely grid-aligned byte value onto it. An exact
//    interpolation reference is correct for any input coordinate, not
//    an approximation that only holds near a grid line, so it still
//    "eliminates the variable you're not testing" the same way
//    runLutBakeTest()'s grid-cell picks do, just by a different route.
//  - An empirically measured residual tolerance, following the same
//    "measure, don't guess" discipline runLutBakeTest()'s own
//    kResidualTol derivation used -- expected larger than LutBake's own
//    2e-3, since this pass adds the graded_ texture's own RGBA8Unorm
//    quantization on top of the LUT's rgba16float one.
//  - Version-gating: two calls with an unchanged OpStack stay
//    byte-identical; mutating the OpStack (setOp) changes the graded
//    output, proving the version-bump-triggers-rebake path is exercised
//    for real (not merely that the skip branch was taken, which isn't
//    instrumented and isn't what this phase's Verify criterion needs at
//    the --selftest level).
bool runApplyPassTest(GpuContext& gpu, PaintSim& sim);

// Headless, GPU-free check on app/CurveEdit.hpp (PLAN.md Phase 3 step 8's
// curve widget, factored out for testability the same way app/Snapping.hpp
// was for Phase 2 step 12 -- see that header's own doc comment and
// runGuidesGridSnapTest() above for the precedent this mirrors). Pure CPU
// list mutation and screen<->curve-space geometry, no ImGui/GPU/PaintSim
// involvement -- the plot draw, click/drag/right-click handling and channel
// tabs themselves are UI (ui/MacPaintUI.cpp) with no headless driver; what's
// covered here is everything that UI calls into:
//
//  - curveToPlot()/plotToCurve(): a hand-computed round trip at a spread of
//    curve-space points (including the y-flip -- curve-space (0,0), y-up,
//    must land at plot-local (0, plotSize), the bottom-left corner in
//    screen-space y-down) and the inverse recovering the original point;
//    plotSize <= 0 returns (0,0) rather than dividing by zero.
//  - hitTestPoint(): a point exactly on a control point hits it; a point
//    just inside radiusPx hits, just outside misses; an empty curve always
//    misses; two points equidistant from the query resolve to the earlier
//    index (the documented tie-break).
//  - insertPoint(): inserting several points in a deliberately scrambled
//    order (not ascending, not descending) leaves `curve` sorted ascending
//    by `.x` every time, checked after each insertion, not just at the end;
//    out-of-[0,1] input is clamped before insertion.
//  - movePoint(): moving a point's x past a neighbour re-sorts the curve
//    (the point that was originally to that neighbour's other side is now
//    found at the opposite relative position) while a move that doesn't
//    cross anyone leaves relative order unchanged; the returned index
//    matches where the moved point actually ended up; out-of-[0,1] targets
//    are clamped.
//  - removePoint(): removes exactly the intended point, shifting later
//    indices down by one, leaving the rest of the curve's order untouched.
//  - The 0/1-point degenerate cases evalCurve() itself relies on (ops/
//    PointOps.hpp): insertPoint() into an empty curve and removePoint() back
//    down to empty both leave `curve` in the exact state evalCurve()
//    documents as identity.
bool runCurveEditTest();

// Headless, GPU-free check on io/Export (PLAN.md Phase 4 step 1: "Export
// path -- encode from working space to a chosen target space and bit depth,
// explicitly, never silently (PRD B6, I5)"). Pure CPU -- the export path
// only ever reads a Document's tiles and produces bytes; no PaintSim or gpu
// involvement, and every fixture and every produced file stays in memory
// except the one deliberate exportDocumentToFile() case below, which
// removes its own scratch file afterwards.
//
// Fixtures are built by writing *straight* linear RGBA through the same
// `rgb *= a` premultiply io/ImageIO.cpp's writeDecodedImageIntoLayer() does,
// so the documents under test hold what a real opened/painted document
// holds. Every precision claim is checked against the tile's own stored
// (post-half-rounding) value read back through Tile::readPixel, not against
// the float literal that was written in -- half-precision storage is
// io/ImageIO's boundary, already covered by runImageIOTest(), and leaving it
// in the arithmetic would swamp the quantization term this test is about.
// What that leaves is exactly one lossy stage per round trip, which is what
// makes the tolerance derivable rather than guessed; see SelfTest.cpp for
// the full derivation, and note that both the 8- and 16-bit worst-case
// residuals are *measured and printed* at run time, so the derivation is
// checkable rather than asserted.
//
// Covered, in order:
//
//  - A real 16-bit round trip: a 4x4 fully-opaque linear ramp exported to
//    16-bit sRGB PNG and decoded back through the existing
//    decodeImageLinear(), every channel of every pixel within the derived
//    16-bit tolerance -- plus a direct read of the file's own IHDR bit-depth
//    byte, so "16-bit" is a property of the bytes rather than of this
//    module's bookkeeping.
//  - PRD B6 proven, not assumed, in both directions. Precision: two pixels
//    whose sRGB-encoded values (0.5010 / 0.5030) both fall inside 8-bit code
//    128's bucket collapse to the *identical* sample at 8 bits and stay
//    distinct -- at their own individually correct levels -- at 16. Loud
//    failure: 16-bit into JPEG/TGA/BMP each fail with an error string that
//    is actually inspected for the format name, the refused depth, the real
//    limit and the format that could carry the request, with a PNG control
//    proving the refusal is about the format rather than a blanket "16-bit
//    never works".
//  - PRD I5 proven: the same Document exported to all three target spaces
//    produces three pairwise-different files, and each one's literal file
//    sample (recovered as srgbEncode() of the decoded value, since
//    decodeImageLinear() always sRGB-decodes) is checked against
//    color/Space's own srgbEncode()/rec709Encode() -- including the explicit
//    assertion that the linear/no-encode option writes 0.5 and emphatically
//    not srgbEncode(0.5) ~ 0.735.
//  - Premultiply: a translucent pixel's exported RGB checked against the
//    tile's own raw premultiplied storage divided by its own stored alpha
//    (runProbeTest()'s discipline, not "a plausible number"), plus the
//    proof that it differs from the raw premultiplied value, the a <= 0
//    guard on a never-painted texel, and an alpha=1 control.
//  - The primaries scope decision enforced rather than merely documented: a
//    working space carrying ACEScg's red primary is refused by name with the
//    offending coordinate quoted, with a matching-primaries control proving
//    it is a real comparison and not a blanket rejection.
//  - JPEG's missing alpha channel refused by name (with the first offending
//    pixel's coordinates), and a PNG/TGA/BMP control on the same document.
//  - PRD I1's write half: all four formats encode and decode back, with the
//    three lossless containers held to the derived 8-bit tolerance and JPEG
//    to a deliberately looser, separately-labelled one.
//  - flattenDocumentToLinear() on its own: canvas clipping of out-of-canvas
//    tiles, the multi-layer plain sum, a blank document exporting as fully
//    transparent rather than erroring, and a zero-sized document failing
//    with a specific message.
//  - exportDocumentToFile(): the file on disk byte-identical to
//    exportDocument()'s in-memory bytes (one encode path, not two), and a
//    refused request forwarding the encode's own error while leaving no file
//    behind.
bool runExportTest();

// Headless, GPU-free check on io/Capabilities and io/OiioBackend (PLAN.md
// Phase 4 steps 2 and 3: "io/OiioBackend behind NP_USE_OIIO -- EXR, TIFF,
// HDR, DPX, flattened PSD, camera raw" and "Capability query -- format
// support is discovered at runtime; the core builds and runs without OIIO
// (PRD I3)"). Pure CPU; every fixture and every produced file stays in
// memory, and nothing here touches the filesystem at all.
//
// **This section is deliberately NOT #ifdef'd out of the NP_USE_OIIO=OFF
// build.** PLAN.md §1.5's lesson -- "an unexercised build option is not a
// seam", written after NP_USE_MIXBOX=OFF had rotted from never being built
// -- applies directly, one level down: a capability query whose tests only
// run in the configuration that has the capabilities is not testing the
// query. So a single `kOiioBuild` constant carries which backend set was
// compiled in, and every assertion states the *correct* answer for that
// configuration. Both builds run the same assertions and both must pass.
//
// Covered, in order:
//
//  - PRD I1's four formats (PNG/JPEG/TGA/BMP) report identical capabilities
//    in both configurations, and report `FormatBackend::Stb` in both -- so
//    "no optional dependency" is a property of the dispatch, not a claim.
//    Their depth and alpha answers are checked individually, including that
//    none of them claims a float depth.
//  - EXR/TIFF/HDR/DPX report readable+writable exactly when the OIIO backend
//    is compiled in, with the per-format depth sets checked against what was
//    measured from this OpenImageIO: EXR half and 32-bit float only, TIFF
//    8-/16-bit integer and 32-bit float (not half), HDR 32-bit float only
//    and *no alpha channel*, DPX 8-/16-bit integer and 32-bit float (not
//    half). Every excluded depth is a case where OpenImageIO would have
//    accepted the request and silently written a different sample type --
//    and two of these rows caught a wrong hand-written expectation while
//    this test was being landed, which is the concrete argument for the
//    query existing at all.
//  - PSD is readable but not writable in the OIIO build (PSD export is
//    phase 15), and neither without it.
//  - **Camera raw is reported unsupported in BOTH builds**, with the OIIO
//    build's reason naming LibRaw's deliberate absence. This is the
//    assertion that actually proves PRD I3: a hardcoded "NP_USE_OIIO implies
//    step 2's format list" table would pass every other check here and fail
//    this one, because the query genuinely asks the linked library.
//  - PRD B6 for the float depths, in both builds: a 32-bit-float request
//    into each of the four integer formats fails with an error naming the
//    format, the refused depth and which formats could carry it; and in the
//    OFF build an EXR request fails naming NP_USE_OIIO rather than failing
//    bare.
//  - A real EXR round trip (OIIO build): a 4x4 ramp exported to half and to
//    32-bit float, read back through the existing decodeImageLinear() -- so
//    the OpenImageIO decode fallback is exercised through the production
//    entry point, not a test-only one -- and compared against the tile's own
//    stored value. Both are asserted **exactly zero**, not merely within a
//    tolerance, and SelfTest.cpp derives why that is the correct claim
//    rather than an optimistic one.
//  - 32-bit float proven distinguishable from half, with a value (0.1f) that
//    half cannot represent: the float file returns it bit-exact, the half
//    file does not, and the difference is measured and printed.
//  - Values above 1.0 survive EXR and clip in PNG, proving the [0,1] export
//    clamp is keyed to the depth rather than applied blindly.
//  - TIFF and DPX 16-bit-integer round trips against step 1's own derived
//    16-bit tolerance, and an HDR (Radiance RGBE) round trip against a
//    separately derived, much looser one -- RGBE's shared exponent is a
//    fundamentally coarser storage than half, and using one tolerance for
//    both would hide that.
//  - A hand-built 52-byte flattened PSD (the same "build the fixture by
//    hand so the test cannot pass by construction" discipline
//    encodePng16()'s 16-bit decode fixture already used) decoded through
//    decodeImageLinear() to its exact hand-computed linear values.
bool runFormatSupportTest();

// Headless check on io/NpaintFile -- the native `.npaint` container (PLAN.md
// Phase 4 step 4: "Native `.npaint` save and load -- multi-part tiled EXR via
// OIIO"; docs/document-format.md; PRD I4, I5b, I6, I7, I8, I10, I11, I12).
// The only --selftest section that writes real files, because a document
// format is a file format: everything is written to `selftest_npaint_*` in
// the working directory and removed again, including the paths whose
// assertions failed.
//
// **Not #ifdef'd out of the NP_USE_OIIO=OFF build**, for the reason
// runFormatSupportTest() above already documents at length: a single
// `kOiioBuild` constant carries the configuration and every assertion states
// the correct answer for it. In the OFF build that means all of the request
// validation plus the refusal wording; in the ON build, that plus the real
// round trips.
//
// Covered, in order:
//
//  - PRD I7, in both builds: `npaintCompressionIsLossy()` catches dwaa,
//    dwab, b44, b44a and pxr24, case-insensitively and through the
//    `name:level` form, and clears its reason string for the five lossless
//    ones; `saveNpaint()` itself refuses `dwab:60` by name and writes
//    nothing; and an *unrecognised* compressor name is refused too, because
//    a name this build does not know cannot be assumed lossless.
//  - PRD I11, in both builds: a zero-area canvas, a Pigment layer (refused
//    by index, name and kind, naming the `pig.*` channels that have no
//    storage yet), an out-of-range opacity, an RGB layer with no tile store,
//    a UINT8[n] blob attribute and a carried scanline part -- each refused
//    with its own message, each leaving no file behind.
//  - The NP_USE_OIIO=OFF refusal itself: `.npaint`, the build option, the
//    cmake line that enables it, and the alternative that does work in that
//    build.
//  - **Bit-exactness at zero tolerance.** A three-layer document with
//    content in separated tiles, a hole inside one layer's data window, a
//    translucent texel and an empty layer round-trips with every half word
//    of every tile identical -- counted and printed, not sampled. The sparse
//    tile set survives too: the hole is not filled in and the empty layer
//    comes back with zero tiles, because a rectangular EXR data window
//    cannot encode a hole and the reader drops all-zero tiles.
//  - Every one of the seven per-layer `np:*` attributes, checked to its
//    exact value, with `visible=false` and `opacity=0.0` chosen so that an
//    absent attribute reading back as its default would fail rather than
//    pass. Plus `np:version`, `np:basis`, the `L####` part ids, and PRD I6's
//    `chromaticities`.
//  - PRD I5b/I12: part 0 matches io/Export's flattener within a derived half
//    tolerance and *exactly* at every alpha-1 pixel; then a layer is mutated
//    and the document saved again, and the composite is proven to have
//    changed **and** to match the new flatten -- a test that only checked
//    part 0 existed would not catch a stale one.
//  - PRD I10 with three unrecognised document attributes, one unrecognised
//    layer attribute and a whole foreign `np:kind="Pigment"` part with
//    `pig.*` channels, all of them things this build's code has no knowledge
//    of: each survives byte for byte, the foreign part stays *between* the
//    two layers rather than being appended, the layers keep their part ids,
//    and a **second** generation is still identical.
//  - PRD I8: the same document saved under a `.exr` name loads identically.
bool runNpaintFormatTest();

// Headless check on io/TileResidency -- OpenImageIO's `ImageCache` as the
// residency layer for unmodified source tiles (PLAN.md Phase 4 step 5,
// "the main reason the dependency earns its cost"; ADR-0001's lazy-residency
// model; docs/document-format.md §1's claim that the cache serves our own
// documents and not only imported files).
//
// **Not #ifdef'd out of the NP_USE_OIIO=OFF build**, and for a stronger
// reason than runFormatSupportTest()/runNpaintFormatTest() have. Those two
// cover features that only exist with a backend, so the OFF build can only
// assert a refusal. Residency is not a feature, it is a *strategy*: the OFF
// build has a complete and correct one (Eager), so most of this section runs
// in both configurations and asserts the **same** answers rather than merely
// the right answer for each build. A single `kOiioBuild` constant carries the
// configuration for the parts that genuinely differ.
//
// Writes real files, like runNpaintFormatTest() and for the same reason -- a
// residency layer is a layer over a file. Everything goes to
// `selftest_residency_*` in the working directory and is removed again,
// including the paths whose assertions failed.
//
// Covered, in order:
//
//  - **Both builds**: the eager strategy through the same interface the
//    cached one satisfies -- Owned vs Absent, promotion from transparent
//    black, resident bytes, and copy-on-*first*-write promoting exactly once;
//    and `npaintLayerTileSource()`'s layer-to-subimage mapping over a
//    hand-built carry, including a foreign part between two layers shifting
//    the second layer's subimage.
//  - **OFF build**: the refusal, which unlike io/NpaintFile's names a
//    *complete* alternative, because PRD I1/I3 require opening and painting a
//    file to behave identically here; and that a refused open leaves nothing
//    half-built and no cache statistics to report.
//  - **ON build, the demonstration**: a 2048x2048 document (256 tiles, 32.00
//    MiB) saved as `.npaint`, then served two ways at once -- eagerly, and
//    from the cache -- with every one of the 256 tiles compared by `memcmp`
//    of all 128 KiB at **zero tolerance**. Resident bytes are printed for
//    both strategies, which is the measurement the step exists to produce.
//  - Cold and warm fetch cost, timed in-process over many iterations, printed
//    per tile *and* scaled to a 2560x1440 viewport against phase 1.1's
//    measured 12.1 ms p50 frame. The warm cost is asserted inside the derived
//    per-tile budget; the cold cost is reported honestly, because it does not
//    fit.
//  - The cache's own `stat:*` accounting, and **eviction proven rather than
//    assumed**: under a budget smaller than the document, re-reading an
//    already-swept tile increments `tiles_created`, which shows that exact
//    tile was dropped.
//  - Copy-on-first-write end to end: a promoted tile starts as the file's
//    pixels bit for bit, a painted texel survives both eviction and a full
//    cache invalidation, the rest of that tile is still the file's, and an
//    untouched neighbour is still Clean.
//  - Every failure path, each of which must refuse rather than serve
//    something plausible: a missing file (including a promotion that returns
//    nullptr instead of zero-filling, which would erase what a stroke was
//    painted on), a truncated file, a file changed on disk under the open
//    cache, a read outside the data window, and an untiled source refused at
//    open with the measurement behind the refusal.
bool runTileResidencyTest();

// Headless check on io/ExportAs and ops/Resample -- the Export As operation
// (PLAN.md Phase 4 step 7: "Export As -- format, space, depth *and resize*,
// with saveable presets (PRD I15). Downscale prefilters; see the phase 6
// warning"; PRD I5, I11, B6).
//
// This is the whole of that step except the widgets: the request model, what
// this build may offer, the validation and its refusals, the prefiltered
// downscale, and the preset file. ui/MacPaintUI.cpp's File > Export As...
// dialog calls into exactly these functions and adds nothing but ImGui, which
// is app/CurveEdit's split restated -- and the reason the interesting half of
// a dialog can be tested at all.
//
// **Not #ifdef'd out of the NP_USE_OIIO=OFF build**, and this section has the
// strongest version of that argument in the file: the behaviour under test
// *is* the cross-configuration one. A preset naming EXR half is a preset an
// OFF build cannot honour, and the requirement is that it survives the round
// trip unchanged and is refused by name rather than silently becoming a PNG.
// A single `kOiioBuild` constant carries the configuration and both builds
// run every assertion.
//
// Pure CPU, no GPU. Writes two scratch files in the working directory
// (`selftest_exportas_presets.json`, `selftest_exportas_out.png`) and removes
// both, including on the paths whose assertions failed.
//
// Covered, in order:
//
//  - What the dialog may offer, in both builds: PRD I1's four formats always,
//    EXR/TIFF/HDR/DPX exactly when the backend is compiled in, the read-only
//    formats never, and per-format depth lists that come from io/Capabilities'
//    probe -- so EXR offers half and float but *not* 8-bit, the case
//    OpenImageIO would have silently substituted.
//  - `resolveExportSize()` for all three modes against hand-computed answers,
//    including a percentage that rounds each axis independently (33% of
//    1024x768 = 338x253), the clamp that stops an axis rounding to zero,
//    fit-within picking the binding axis, and fit-within refusing to enlarge.
//  - `resampleAreaAverage()` against hand-computed references: 2x2 block means
//    for an integer factor, and the fractional edge weights of a 3->2
//    reduction. A constant image survives a lopsided 500x30 -> 61x7 reduction
//    **bit-identically at zero tolerance**, which is the statement that the
//    weight normalisation is exact -- and the reason it is done in double, as
//    float weights would leave an opaque image at alpha 0.999996 and make it
//    un-exportable to JPEG.
//  - **The phase 6 warning, measured rather than asserted.** A 1-px
//    checkerboard reduced 8x: prefiltered lands on the true mean everywhere,
//    while naive point sampling collapses a 50%-grey pattern to a *flat*
//    image at full amplitude. Then a period-3 pattern that does not divide
//    the scale factor, where the box kernel's own residual ripple is
//    measured and reported rather than claimed away.
//  - Linear light, proven by the number in the file: a black/white checker
//    halved exports as 8-bit code 188 = round(255 * srgbEncode(0.5)), and the
//    encoded-domain average this ordering prevents is shown landing at linear
//    0.214.
//  - Alpha filtered premultiplied: a fully transparent green texel next to an
//    opaque red one contributes **exactly** zero green, against the 0.5 a
//    straight-alpha average gives.
//  - **One set of refusal strings, not two**: the dialog's message, the
//    shared `exportRefusalReason()` and a real `encodeLinearImage()` failure
//    are asserted equal by string comparison, so a second UI vocabulary
//    cannot exist to drift.
//  - Every refusal path by its message: read-only formats, camera raw, EXR in
//    the OFF build naming NP_USE_OIIO, 8-bit EXR in the ON build, upscales,
//    a primaries mismatch, and a translucent document into JPEG -- plus that
//    the two optional checks are *skipped*, not silently passed, when the
//    dialog has no document to check against.
//  - PRD I11's warnings, each carrying a number: 75.0% of the pixels
//    discarded, 256 levels at 8 bits, the 12.9x shadow-step penalty of 8-bit
//    linear (derived from color/Space's curve at run time), the specific
//    highlight value an integer depth will clip and where it is, and JPEG's
//    lossiness -- with a control that a clean request warns about nothing.
//  - The preset round trip, save -> serialize -> load, every field including
//    the numbers the active resize mode does not read; replace-by-name and
//    delete, both case-insensitive; the three name refusals; a broken file, a
//    file that is not a preset file, an unknown *token* skipping one preset
//    while the rest load, an unknown *field* being harmless, duplicate names
//    failing the load, and a missing file being an empty store rather than an
//    error.
//  - `exportDocumentWithRequest()` end to end: a 64x64 1-px stripe document
//    exported at 25% comes back 16x16 with every texel at the true linear
//    mean, and a refused request writes no bytes and leaves no file.
bool runExportAsTest();

// Headless check on app/DocumentLifecycle -- the open-document record, the
// session that owns it, and the five operations PLAN.md Phase 4 step 8 names
// ("Document lifecycle -- revert, duplicate document, save a copy, save
// incremental, open recent"). PLAN.md cites PRD I17; the current PRD numbers
// that requirement **I18** -- see DocumentLifecycle.hpp.
//
// **Not #ifdef'd out of the NP_USE_OIIO=OFF build** (PLAN.md §1.5). Roughly
// two thirds of this section is build-independent by construction -- the
// record, the session, duplication, the incremental *naming* rule and the
// whole recent-documents list touch no file format at all -- and the
// remaining third asserts that each file-backed entry point forwards
// io/NpaintFile's own named refusal in the build that has no OpenImageIO,
// rather than inventing a second vocabulary for it. One `kOiioBuild` constant
// carries the configuration.
//
// Pure CPU, no GPU. All scratch state lives in a `selftest_lifecycle/`
// directory in the working directory, which is removed unconditionally at the
// end (including on failing paths) and asserted gone. A directory rather than
// loose `selftest_*` files because `nextIncrementalPath()` *lists its
// containing directory* to find the highest existing version, and run against
// the working directory its answers would depend on whatever else was there.
// `$NP_RECENT_DOCUMENTS` is overridden while the storage-location assertions
// run, so this never touches the developer's own recent list.
//
// Covered, in order:
//
//  - The record: monotonic non-zero ids, a blank document starting clean and
//    unbound, and `recordEdit()` producing a PRD I11 summary that names the
//    edits rather than merely counting them -- including that the label list
//    is capped while the count stays exact.
//  - The session: pointer stability across 32 further opens (the reason it
//    holds `unique_ptr`s), addressing by index and by id, and closing a dirty
//    document refusing with the same "names what would be lost" message
//    revert uses.
//  - **Duplicate**: the path is NOT inherited (its own assertion -- this is
//    the failure that would silently overwrite the original), a fresh id, a
//    deep tile copy proven by painting on one and checking the other, the
//    carry bag and the layer part ids coming across, dirty from birth, and
//    Save on the result refusing by name rather than writing anywhere.
//  - **Save incremental naming**: `_v001` for an unversioned name; a gap left
//    unfilled; an old version still landing above the highest sibling; other
//    extensions and other base names ignored; an existing version's own zero
//    padding preserved (`_v7` -> `_v8`); padding growing at `_v999` ->
//    `_v1000`; a trailing number with no `_v` marker treated as part of the
//    name; and the two refusals.
//  - **Open recent**: ordering, dedup by normalised path, capacity 10, the
//    empty and control-character refusals, the file round trip, a duplicate
//    line resolving to one entry at its most recent position, a corrupt line
//    reported with its line number without failing the load, a missing file
//    being an empty list rather than an error, `$NP_RECENT_DOCUMENTS`, and --
//    the requirement with teeth -- an entry whose file is gone being refused
//    **by name and kept in the list**, never silently dropped.
//  - The file-backed half, in the build that has a writer: **Save a copy**
//    leaving the document's path and dirty state unchanged and refusing to
//    write onto the document's own file; **Revert** refusing a dirty document
//    while naming the edits, restoring the file's pixels *and the file's
//    carry* once confirmed, and leaving the in-memory document untouched when
//    the file has gone; **Save incremental** writing `_v001` then `_v002` and
//    never overwriting the earlier one; duplicate-then-save leaving the
//    original file byte-intact; open and open-recent round trips; and the PRD
//    I10 carry -- three unknown attributes and a whole foreign
//    `np:kind="Pigment"` part -- asserted intact after **every** operation
//    that writes.
//  - That a cached `io/TileResidency` read after a lifecycle write returns the
//    **new** pixels. OpenImageIO's cache does not notice a rewrite, and a
//    residency opened afterwards passes its own size+mtime staleness check
//    because the stamp is taken at open; without the invalidation these
//    operations perform, the read returns the previous contents and reports
//    success.
bool runDocumentLifecycleTest();

// Headless check on app/Journal -- the recovery journal PLAN.md Phase 4 step 9
// names and ADR-0008 designs (PRD O5-O10). PLAN.md calls the module
// `core/Journal`; it is `app/Journal`, because it serialises an
// `app::OpenDocument` through `io/NpaintFile` and a `core/` module may do
// neither (see Journal.hpp's opening section, and PLAN.md's Deviations).
//
// **Not #ifdef'd out of the NP_USE_OIIO=OFF build** (PLAN.md §1.5), and the
// answer it asserts there is a genuinely uncomfortable one: `saveNpaint()` is
// OpenImageIO-only, so **the recovery journal does not run in the default
// build**, and this section asserts exactly that -- `journalAvailable()`
// false, the reason being io/NpaintFile's own named refusal, and *no scratch
// directory created*, since an empty one would be offered next launch and
// hold nothing. Everything that is not the writer is asserted identically in
// both builds: the scratch location and its override, the naming and dating,
// the whole timer rule (which is a pure function precisely so that the build
// with no session still checks it), the sidecar format, and every integrity
// refusal.
//
// Pure CPU, no GPU. `$NP_JOURNAL_DIR` points the module at a
// `selftest_journal/` directory in the working directory for the whole
// section, removed unconditionally at the end and asserted gone, so this
// never touches `~/Library/Application Support/naturalPaint`.
//
// Covered, in order:
//
//  - Availability: the correct answer per build, the refusal forwarded rather
//    than reworded, and the fact that *asking* for the reason opens no file --
//    the probe stops at io/NpaintFile's backend gate.
//  - The scratch location: `$NP_JOURNAL_DIR`, the per-user default under
//    `naturalPaint/recovery`, and that it is deliberately not under `Caches`,
//    which the system may purge.
//  - The timer, as `journalWriteDue()`: a clean document never due; a
//    never-journalled dirty one due at once; a **content** edit waiting for
//    the interval and firing to the second; a **structural** edit due
//    immediately (PRD O5); a stroke-deferred write staying due rather than
//    waiting another interval (PRD O10); and a saved document never due.
//  - Beginning a session: the directory named and dated
//    `session-YYYYMMDD-HHMMSS-<pid>`, its descriptor and lock, that a **live**
//    session is not offered back to the process holding it (the `flock` probe,
//    which is what makes a lock left by a machine that lost power impossible
//    rather than merely unlikely), and that `finishClean()` leaves nothing.
//  - A hand-built journal entry, in **both** builds, because the integrity
//    record is checked before the file format reader is: a sound entry
//    reaching the reader, a **truncated** model refused by name with both byte
//    counts, a right-length wrong-contents model refused on the hash, a
//    sidecar with no terminating `end` refused rather than half-read, and both
//    journal files still present afterwards. In the build with no reader the
//    ordering is provable, because the refusal is about truncation and not
//    about the missing backend.
//  - The round trip through the real writer: a structural edit journalled on
//    the tick, the user's own file untouched (PRD O9), an unchanged document
//    not rewritten, a tile changed in the **active** document reaching disk on
//    the timer with no deactivation anywhere (PRD O6), a stroke deferring the
//    write and the write happening on the first tick after it, an in-process
//    crash (the session destructor -- no signals, no second process), the
//    directory offered afterwards, and a recovery whose tiles are bit-identical
//    at zero tolerance, whose every `np:*` layer attribute matches, whose PRD
//    I10 carry is intact, and which comes back dirty and bound with its edit
//    labels in order.
//  - That a saved document's journal is dropped, and that neither discovery
//    nor recovery ever deletes anything -- only the explicit discard does.
//  - The measurements the interval is derived from: one journal write of a
//    2048x2048, 256-tile document; a deep copy of the same document (what a
//    background writer would still cost without copy-on-write tiles); the
//    resulting duty cycle, with a 3% ceiling asserted so an order-of-magnitude
//    regression fails rather than merely slows; `fsync` against `F_FULLFSYNC`,
//    which is the measurement the durability choice rests on; and what
//    launch-time discovery costs against PRD A2.
bool runRecoveryJournalTest();

// PLAN.md Phase 5 step 1 ("Multiple layers in `Document`, with reorder,
// visibility, lock, opacity"; PRD C4, C16): core/Composite's `over`,
// core/LayerOps' operations, app/LayerPanel's row logic, and the round trip
// that proves the stack's order survives a save.
//
// **Not #ifdef'd out of the NP_USE_OIIO=OFF build** (PLAN.md §1.5). Everything
// except the `.npaint` round trip is pure CPU and asserts the same answers in
// both configurations; the round trip asserts the correct answer for the build
// it is in through the same single `kOiioBuild` constant every other section
// uses -- the file is written and reloaded in the ON build, and in the OFF
// build the save is asserted to be refused by io/NpaintFile's own named
// message. Headless and GPU-free; writes and removes one
// `selftest_layerstack.npaint` and touches no user state.
//
// Covered, in order:
//
//  - `over` against a hand-computed reference: two 50%-alpha layers giving
//    straight (1/3, 0, 2/3, 0.75), with the full arithmetic written out at the
//    fixture, plus the explicit check that this is NOT the plain sum's opaque
//    purple -- so the fixture could not pass against the code it replaced.
//  - Reorder changing the result in the direction the ordering convention
//    predicts: moving index 0 to index 1 mirrors which colour dominates, and
//    leaves coverage alone.
//  - Opacity proven to be a coverage multiplier and **not** an alpha
//    replacement: 50% opacity on an opaque layer is bit-identical to alpha
//    0.5, while alpha 0.5 *at* 50% opacity composites to exactly 0.25. The
//    tile and the Layer are both proven unmutated afterwards, and
//    `layerCoverage()`'s clamps and NaN guard are checked directly.
//  - A hidden layer contributing **exactly nothing**, at zero tolerance,
//    against the same document with that layer deleted -- with a negative
//    control proving the visible version really does differ.
//  - **The regression boundary this step's semantics change makes necessary**:
//    against a second, independent implementation of the plain sum (written in
//    the test, not called), a single-layer document and a three-layer document
//    with no overlap composite **byte-identically**, and one overlapping texel
//    is enough to break that -- so the identity is a property of non-overlap
//    rather than of the implementation.
//  - Every layer operation's effect on the dirty state: `revision` and
//    `structuralRevision` both bumped by exactly one per operation (every
//    layer change is structural, PRD O5), the edit label naming the layer, a
//    refused operation recording nothing at all, and the deep copy
//    duplicateLayer really makes.
//  - `locked` refusing what it should, by name: remove, move-itself, opacity
//    and rename refused; hide, unlock, duplicate (with the copy inheriting the
//    lock) and moving a *different* layer past it allowed. Each refusal names
//    the layer.
//  - An unimplemented blend composited as `over` and reported by name at every
//    boundary that produces a file -- including for a hidden layer, and
//    including alongside a refusal -- with the composited pixels proven
//    byte-identical to `over` and the blend string proven untouched.
//  - core/Probe agreeing with the flattener through the same `compositeOver()`,
//    and the deliberate split where single-layer mode ignores visible/opacity.
//  - app/LayerPanel's single bottom-first/top-first reversal and its inverse,
//    the out-of-range guard, and the exact `RGB · NORMAL · 100%` sub-line text.
//  - The save -> load round trip of a **reordered** three-layer document:
//    order preserved bottom-first, all six metadata fields on the right
//    layers, each layer's tiles travelling with it, and the reloaded document
//    compositing byte-identically to the saved one.
bool runLayerStackTest();

// PLAN.md Phase 5 step 2 ("`core/Blend` -- the linear-safe set (over, plus,
// multiply, screen, min, max) and `Mix`, the KM latent lerp. Display-referred
// modes labelled as such"); PRD B7, C3, L5.
//
// **Not #ifdef'd out of the NP_USE_OIIO=OFF build** (PLAN.md §1.5): every
// assertion here is pure CPU and asserts the same answers in both
// configurations. Headless and GPU-free; writes no files and touches no user
// state. It does read the real Mixbox LUT from the source tree (NP_MIXBOX_LUT,
// the same path main.cpp loads) because the `Mix` section asserts against
// measured pigment data rather than a stand-in.
//
// Almost everything is at **exactly zero tolerance**, and that is a property
// of the arithmetic rather than a shortcut: no formula in core/Blend contains
// a division, and every fixture value is a short dyadic rational, so each
// product and sum lands on the float grid exactly. Two tolerances are derived
// at their own fixtures.
//
// Covered, in order:
//
//  - The vocabulary: seven modes, the table indexed by enumerator, names and
//    labels unique, name -> mode -> name round-tripping, a case-SENSITIVE
//    match, `kDefaultBlendName` resolving to `Normal`, and
//    `blendIsImplemented()` agreeing with `BlendModeInfo::compositesPixels`
//    for every row.
//  - **PRD B7**: the display-referred marker derived from the data on every
//    call (asserted for every mode, both ways), exactly one display-referred
//    mode, and *why* -- `screen(2,2) == 0` and `screen(2,3) == -1` exactly,
//    i.e. above 1.0 it is not monotone, while every other implemented mode is
//    monotone at the same values. Plus the label appearing on the layer row,
//    not only in the dropdown.
//  - Every mode against a hand-computed reference, **opaque** and at
//    **partial alpha**, with the partial-alpha references computed twice by
//    different routes (the premultiplied algebra, and the three Porter-Duff
//    regions) -- because the naive straight-colour transcription of multiply
//    is wrong on premultiplied input and nothing but the arithmetic says so.
//    Includes the explicit check that multiply is NOT `cs*cb`.
//  - Alpha being exactly `over`'s for every mode across a 5x5 alpha grid.
//  - A fully transparent source being a **bit-exact identity** on the backdrop
//    for every mode over four backdrops including an HDR one, and the mirror
//    (an empty backdrop passing the source through).
//  - `over` bit-identical to a second transcription of step 1's formula across
//    441 non-dyadic alpha pairs, and `blendPixel(Normal, ...)` being that same
//    function rather than the general three-term form.
//  - **Step 1's regression boundary, re-made for the whole set**: five
//    non-overlapping layers each with a *different* blend mode composite
//    byte-identically to a second implementation of the plain sum, with one
//    overlapping texel proving the identity is about non-overlap.
//  - The mode reaching the document walk: a `multiply` layer really
//    multiplying through `flattenDocumentToLinear()` at zero tolerance, with
//    no warning any more, `core/Probe` agreeing through the same dispatch, and
//    switching a blend mode leaving alpha exactly where it was.
//  - The unimplemented-blend contract still holding in substance, in both of
//    its remaining forms: an unknown name (warned, listing the implemented set
//    from the table, value untouched, pixels byte-identical to `over`) and
//    `mix` (its own sentence, naming latents and step 3).
//  - **PRD L5**, from four directions: RGB-over-RGB offered six modes and not
//    `Mix`; Pigment-over-Pigment offered seven; the bottom Pigment layer not;
//    Pigment-over-RGB not; and `core::setLayerBlend()` refusing the same cases
//    through the same predicate, so L5 is not merely something the UI declines
//    to draw. Plus a reorder taking a layer out of L5's reach while it still
//    carries the value.
//  - `core::setLayerBlend()`: canonical name written, edit label reported,
//    locked layer and out-of-range index refused by name.
//  - **`Mix` itself**: `mixLatents()` exact at both endpoints, the *implied*
//    fourth pigment weight proven to lerp by the same t (so six floats really
//    is Mixbox's seven), and -- against the real LUT and the shipped palette
//    -- yellow mixed with blue giving green, measurably unlike the RGB lerp.
bool runBlendTest();

// PLAN.md Phase 5 step 3 -- "Pigment layers -- latent x mass tile storage at
// f16. Per-layer op stack applies *after* the latent->RGB projection, so
// grading never bakes the latents." PRD C1, C3 (P0), C8, F10, L5.
//
// Runs, and asserts the same answers, in BOTH NP_USE_OIIO configurations; the
// `.npaint` block asserts io/NpaintFile's own named refusal in the OFF build
// instead of a round trip. Headless and GPU-free. Writes and removes one
// `selftest_pigment.npaint`. Loads the real Mixbox LUT (NP_MIXBOX_LUT) so
// every colour claim is against measured pigment data.
//
// Sections:
//  - **The tile**: 7 channels and 224 KiB against an RGBA tile's 128 KiB,
//    allocate-on-write and query-without-allocating from the same
//    `TileStoreOf` template, an untouched texel reading mass 0, a dyadic
//    latent round-tripping through f16 storage EXACTLY, and a real Mixbox
//    latent doing so within the derived 2^-11 relative bound (printed).
//  - **The projection**: `latentToRgb(rgbToLatent(p)) == p` against the real
//    LUT after the polynomial moved from paint/Palette into core/Pigment, and
//    the projection proven to need no LUT at all.
//  - **PLAN.md's own Phase 5 verify sentence, as a first-class assertion with
//    printed values**: blue at mass 0.5 on a Pigment layer over yellow gives
//    GREEN under `Mix`, and under `Normal` gives exactly the naive RGB lerp --
//    the muddy answer PRD §2 says Photoshop gives. Plus the mixed colour
//    checked against `latentToRgb(mixLatents(low, up, up.mass))`, which is
//    what would catch a wrong mixing weight.
//  - **Opacity is transparency, not mass**: the tiles' raw half words proven
//    bit-identical across a composite at any opacity; opacity 0 byte-identical
//    to the layer being deleted; hiding the *lower* layer leaving the mixing
//    one visible and unmixed; half the opacity measurably different from half
//    the mass; and the `lerp(dst, blend(src,dst), o) == blend(o*src, dst)`
//    identity that justifies the coverage form used on a mixed pair.
//  - **The op stack after the projection**: a grade leaving the stored latents
//    bit-identical (memcmp), the composited colour proven to be the grade *of
//    the projection*, a negative control showing the other order is a
//    different colour, a disabled op proven byte-identical, and the same
//    member working on an RGB layer.
//  - **Every other `Mix` combination**: over an RGB layer, on the bottom
//    layer, and chained three deep -- each composited as `over` and warned by
//    name, with the greedy bottom-up pairing asserted directly.
//  - **The probe and the flattener agreeing** on a mixed pair, and
//    single-layer probing of a Pigment layer.
//  - **The regression boundary**: a hidden Pigment layer contributing
//    byte-identically nothing to an RGB document, with a negative control.
//  - **The `.npaint` round trip** for a document holding both kinds: three
//    parts, every pigment tile bit-identical, `res.R` coming back as `res.R`
//    (OpenEXR sorts channel names, so the reader matches by name), metadata
//    round-tripping, the reloaded document compositing byte-identically, the
//    `np:basis` mismatch refusal docs/document-format.md §3.3 asks for, and
//    the named warning for an op stack the format cannot yet carry.
bool runPigmentLayerTest();

// PLAN.md Phase 5 step 4 -- "Layer masks -- single-channel tile store, the
// same machinery." PRD C4 (P0), C3 (P0), C2, I4, I11.
//
// Runs, and asserts the same answers, in BOTH NP_USE_OIIO configurations; the
// `.npaint` block asserts io/NpaintFile's own named refusal in the OFF build
// instead of a round trip. Headless and GPU-free. Writes and removes
// `selftest_mask.npaint`, `selftest_mask_bare.npaint` and
// `selftest_mask_again.npaint`. Loads the real Mixbox LUT (NP_MIXBOX_LUT) so
// the Pigment claims are against measured pigment data.
//
// Sections:
//  - **The tile**: one f16 channel and 32 KiB against an RGBA tile's 128 KiB
//    and a pigment tile's 224 KiB (printed), `kRevealWord` checked against
//    `floatToHalf(1.0f)`, a MISSING tile and a FRESHLY ALLOCATED one both
//    reading 1.0 -- the decision that stops a mask on one tile from blanking
//    a layer's other tiles -- dyadic values round-tripping exactly, and the
//    derived 2^-12 bound measured over a 1025-point ramp against uint8's
//    1/510.
//  - **Out of range and NaN**: clamped on write, clamped on read for raw half
//    words only a *file* could produce, and a document holding a NaN mask
//    proven to composite to finite values everywhere.
//  - **A mask multiplies coverage**, against hand-computed references with
//    printed values: alpha changes and colour does not; over a backdrop the
//    exact `over(m*src, dst)` at zero tolerance; opacity 0.25, mask 0.25 and
//    opacity 0.5 x mask 0.5 all BYTE-IDENTICAL (with a negative control); and
//    the `lerp(dst, over(src,dst), m*o) == over(m*o*src, dst)` identity at
//    exactly 0 residual over the whole grid.
//  - **Absent vs all-1.0 vs all-0.0**: reveal-all allocating nothing and
//    compositing byte-identically to absent, all-0.0 compositing
//    byte-identically to the layer being *deleted*, the panel row's `MASK`
//    being the only visible difference between absent and reveal-all, and
//    core/LayerOps' add/remove with their refusals.
//  - **The PRD C3 trap on a Pigment layer**: the stored `pig.m` proven
//    bit-identical by memcmp across four mask values; on a mixed pair, half
//    the MASK printed beside half the MASS and proven a different colour --
//    the mask giving exactly the 50/50 blend of the two projections while the
//    mass gives the Kubelka-Munk green; and both per-texel corners of
//    core/Composite.hpp §3 as byte-identity claims.
//  - **The op stack**: a mask applying after it, with opacity.
//  - **The probe and the flattener agreeing** on a masked RGB layer and on a
//    mixed pair masked on both halves -- the case where the probe's per-texel
//    lookup and the walk's per-tile hoist could most easily diverge.
//  - **The regression boundary**: a non-overlapping multi-layer document with
//    no masks still compositing byte-identically to the plain sum.
//  - **The `.npaint` round trip**: four parts, mask tiles bit-identical, a
//    reveal-all mask surviving as engaged-with-zero-tiles, a mask tile outside
//    the layer's content bounds surviving, a mask-free file loading back with
//    `Layer::mask` disengaged, a file carrying NaN/2.0/-1.0 mask samples
//    warning with a count and loading the clamped values -- and the property
//    the whole format change rests on: removing every mask gives back a file
//    byte-identical to the mask-free one, with OpenImageIO's `capDate`
//    timestamp masked out.
bool runLayerMaskTest();

// PLAN.md Phase 5 step 5 -- "Adjustment layers -- op stack against the
// composite below." PRD C5, C1 (P0), C3 (P0), C4 (P0), D13, D18, I10, I11.
//
// Runs, and asserts the same answers, in BOTH NP_USE_OIIO configurations; the
// `.npaint` block asserts io/NpaintFile's own named refusals in the OFF build
// instead of a round trip. Headless and GPU-free. Writes and removes
// `selftest_adjust.npaint`, `selftest_adjust_bare.npaint` and
// `selftest_adjust_again.npaint`. Loads no LUT -- nothing here is pigment.
//
// **Nearly every claim below is at exactly zero tolerance**, which is possible
// here in a way it was not for steps 3 and 4: an adjustment layer stores
// nothing, so the only rounding in the chain is the arithmetic, and the
// fixtures are chosen to make that exact -- every alpha is a power of two (so
// the un-premultiply bracket is exact), the two ops used for numeric claims
// are a multiply by 2 and a `+ 0.25` (exact on dyadic inputs), and every
// coverage is dyadic. One tolerance is used, for the probe-versus-flattener
// comparison, and it is the same 1.0e-7 the four preceding sections each
// derive for the flattener's own final un-premultiply.
//
// Sections:
//  - **The kind**: `makeAdjustmentLayer()` engaging no tile store at all, the
//    docs/ui.md §3.2 glyph and sub-line, and the row's new `· 2 OPS` marker
//    (absent for an empty stack, so no row written before this step changes).
//  - **io/OpSerial**, the carrier `np:ops` never had: all six PointOpKinds
//    plus a class-B entry round-tripping bit-identically, a **hand-built
//    60-character payload** decoded byte by byte from the spec rather than
//    from this module's own writer, an unknown op class surviving a round trip
//    verbatim and proven inert (no `PointOp`, and it splits a run), the six
//    container-level refusals each naming what they saw, and the two
//    forward-compatibility rules -- a longer-than-expected body and a used
//    reserved byte both becoming `Unknown` rather than being half-read.
//  - **The grade itself**, against exact references with printed values: an
//    opaque texel doubled, a half-covered one doubled in *straight* colour
//    with its coverage intact, and every accumulated alpha proven
//    **bit-identical** with and without the layer.
//  - **Opacity and a mask as "how much of the adjustment applies"**: opacity 0,
//    a hidden layer and an empty stack each byte-identical to the layer not
//    existing; opacity 1 exactly the graded value; three texels of one colour
//    under mask 1.0/0.5/0.0 printed side by side; and mask x opacity proven
//    byte-identical to their product with a negative control.
//  - **Scope**: both layers *below* graded and the one above untouched --
//    PRD C5's "the composite below it", which is not PRD C9's clipping mask --
//    and an adjustment layer over nothing composing to transparent black.
//  - **Stacking order**: two adjustment layers whose ops deliberately do not
//    commute, both orders printed and both exact, plus the proof that two ops
//    in one layer's stack equal the same two ops in two stacked layers.
//  - **The blend an adjustment layer cannot honour**, warned by name and
//    composited byte-identically to `normal`.
//  - **The probe and the flattener agreeing** on a masked, faded adjustment
//    layer, and a document of nothing but adjustment layers still probing as
//    transparent black.
//  - **The regression boundary**: a document with no adjustment layer still
//    compositing byte-identically to the plain sum.
//  - **The `.npaint` round trip**, which is the decision this step turned on:
//    an op stack surviving on an RGB layer *and* on an Adjustment layer, an
//    unknown op surviving through OpenEXR, the Adjustment part's single `mask`
//    channel and its `np:mask` attribute, an unparseable `np:ops` warned about
//    and carried verbatim through a second save, and -- the property the whole
//    change rests on -- emptying every stack giving back a file byte-identical
//    to the one written before stacks were persisted.
bool runAdjustmentLayerTest();

// PLAN.md Phase 5 step 6 ("COW tiles -- copy-on-write with reference-counted
// history"; PRD A9, O1, O4, C2). The tile-sharing primitive Phase 5 step 7's
// byte-bounded, cursor-based history is built on: a `TileStoreOf<T>` slot is a
// `std::shared_ptr<T>`, copying a store shares every tile, and the two
// barriers -- `getOrCreate()` and `findForWrite()` -- copy a shared tile
// before handing out anything writable. core/TileStore.hpp carries the design
// argument; core/TileShare.hpp carries the two byte numbers step 7 needs.
//
// Covers, and the first three are the properties the step lives or dies on:
//  - **Sharing proven by identity, not by equality**: after `Document copy =
//    src;` both documents' `find()` return the SAME ADDRESS, and both agree
//    the use count is 2.
//  - **A write to one copy is not visible in the other**, at exactly zero
//    tolerance, with the untouched tile still shared afterwards and the
//    second write to the same tile proven to be in place (copy-on-FIRST-write).
//  - **The refcount reaching zero frees exactly once**, proven with an
//    instrumented tile type instantiated through the same template: 2 fresh
//    tiles + 3 copy-on-write clones = 5 constructions and 5 destructions,
//    nothing leaked, nothing freed twice.
//  - **The barrier is the only door**, two thirds of it as compile-time facts:
//    `find()` is const-only on all three stores and iterating a non-const
//    store still yields `const T&`.
//  - **All three tile types** -- RGB (128 KiB), Pigment (224 KiB) and Mask
//    (32 KiB) -- with the mask's REVEAL default given its own assertions,
//    because a clone wrongly implemented as a fresh default would look
//    plausible for a mask and only for a mask.
//  - **io/TileResidency composing rather than competing**: its
//    copy-on-first-write is against a *file*, this one is against another
//    in-memory holder, and `tileForWrite()` on a shared owned tile copies.
//  - **The composite unchanged**: shared and deep copies of one document
//    composite bit-identically, which is steps 1-5's regression boundary.
//  - **The measurements this step is justified by**, all `[measured]` and all
//    against io/TileResidency's realistic 2048x2048 / 256-tile / 32.0 MiB
//    fixture: a document copy deep vs shared, the first write to a shared tile
//    vs a write to a unique one, RSS for 16 shared copies vs 16 deep ones, the
//    per-lookup delta against a plain `unordered_map<TileCoord, Tile>` (the
//    exact shape the store had before this step), and ten history entries
//    costed the way step 7 will have to cost them.
//  - **`.npaint`**: sharing is invisible to the format (a shared document and
//    its deep copy save to files differing only in OpenImageIO's `capDate`),
//    and -- stated rather than implied away -- sharing does NOT survive a
//    load, because the format stores each part's pixels in full.
bool runCowTileTest();

// PLAN.md Phase 5 step 7 ("`core/History` -- a linear list with a cursor, not
// a stack"; PRD O1 (P0), A9, O4, and O2/O3 made possible without building the
// panel). Undo moves the cursor back, redo moves it forward, a new edit at a
// non-end cursor truncates the tail, and the whole list is bounded in bytes
// against step 6's copy-on-write sharing.
//
// Covers, and the first three are what the step lives or dies on:
//  - **PLAN.md's own Phase 5 verify sentence**, adapted honestly and said so
//    in the output: "undo ten strokes, redo ten, pixel-identical to before the
//    undos" run as ten *tile writes through `getOrCreate()` funnelled through
//    `OpenDocument::recordEdit(..., EditKind::Content)`* -- the exact pair of
//    calls app/DocumentLifecycle.hpp says the canvas bridge will make -- because
//    no stroke reaches a `Layer` and there are therefore no strokes to undo.
//  - **Redo is not an inverse**: every intermediate undo state is proven
//    bit-identical to a direct `jumpTo()` of the same index, and `jumpTo()` is
//    timed at one and at forty steps to show PRD O3 ("one replay, not N") is
//    already a property of the code rather than a promise.
//  - **Truncation**, with the tail's memory proven released -- by the exact
//    distinct-tile accounting AND by process RSS, not by asserting that a
//    destructor exists.
//  - **Eviction under a deliberately small byte budget, proven against step
//    6's non-additive sharing**: the naive "evict until the sum of per-entry
//    `documentExclusiveTileBytes()` covers the overrun" policy is run side by
//    side on the same history and shown to over-evict every evictable entry
//    where the drop-one-then-re-measure policy takes strictly fewer and still
//    meets the budget.
//  - **A snapshot surviving an eviction that removed the same state from the
//    entry list**, and surviving a truncation -- which is the case that
//    decides snapshots are a second list rather than a flag on an entry.
//  - **The byte accounting checked against reality**: `History::bytes()`
//    against an independent count of distinct tile addresses, and against
//    process RSS across dropping a history of a known size.
//  - **The measurements the step's own correction rests on**: how much of the
//    process's tile bytes a history is actually attributable for, in both the
//    favourable and the unfavourable regime -- the numbers that say what
//    compression and an `mmap` spill could at most have bought.
//  - **app/DocumentLifecycle wiring**: every `recordEdit()` appends, the
//    labels are core/LayerOps' own, a duplicate does not inherit its source's
//    undo stack, and a `.npaint` written by a document with a history is
//    byte-identical to one written by a document without.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations.
// Headless and GPU-free; writes and removes two `.npaint` files.
bool runHistoryTest();

// PLAN.md Phase 5 step 8 ("History panel listing entries by originating tool
// or op; clicking one moves the cursor there in a single replay, not N"; PRD
// O2 (P1), O3 (P1), with O1's redo and O4's snapshots made visible). Exercises
// app/HistoryPanel, which is the pure half of the panel -- rows, row text, the
// serial mapping and the click -- with the ImGui chrome in ui/MacPaintUI.cpp,
// the same split app/LayerPanel already has.
//
// Covers, and the first three are the decisions this step turns on:
//  - **Row order, asserted beside app/LayerPanel's**: the history panel reads
//    oldest-at-top and reverses nothing, where the layers panel reverses;
//    `layerIndexForPanelRow(0, 3) == 2` and `historyRowForSerial(row 0) == 0`
//    are checked in the same assertion, so "fixing" either to match the other
//    fails here.
//  - **A row is keyed by `HistoryEntry::serial`, never by its index**, with
//    the trap demonstrated rather than described: a 0.50 MiB budget drops six
//    states, row index 3 is shown to hold a *different picture* afterwards
//    (compared over raw half words), and a click carrying the pre-eviction
//    serial is **refused with the numbers rather than redirected** to whatever
//    now sits at that position. The list's strictly-ascending-serial invariant
//    -- which the click's binary search rests on -- is asserted across begin,
//    record, truncate, evict and restore.
//  - **PRD O3, counted and timed**: a panel click reports exactly one cursor
//    move at distance 1 and at distance 40, and the per-step walk an
//    implementer would otherwise write is run beside it on the same history,
//    shown to take forty calls for the same bytes, and timed. The timing's
//    bound is *derived* (a click's work does not read the distance, so the
//    ratio must be 1.00) and the measurement's own noise floor is measured
//    beside it rather than assumed.
//  - **The redo tail is visibly distinct**: every row carries PAST / CURRENT /
//    REDOABLE in its *text*, the note names how many states the next edit
//    would discard, and after that edit a click still holding one of those
//    rows is refused with the truncation count.
//  - **Eviction and snapshots are legible**: the dropped-states note names the
//    count and the budget; snapshots are their own row group, survive the
//    eviction that emptied the linear list (PRD O4), and the two clicks refuse
//    each other's rows by name -- a cursor move and an edit are different
//    actions and a single handler would have conflated them.
//  - **The wiring**: rows are named by core/LayerOps' own `editLabel`, an
//    empty history draws nothing and refuses a click into it by number, and
//    revert's un-undoability -- the decision core/History.hpp defers to this
//    step -- is answered by the panel showing one 'revert to saved' row with
//    nothing above it.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations;
// there is no `#ifdef` in the section and nothing for one to guard, which it
// prints. Headless and GPU-free; writes no files.
bool runHistoryPanelTest();

// PLAN.md Phase 5 step 9 ("Clipping masks -- a layer or group clipped by the
// alpha of the layer below"; PRD C9 (P0), and C3/C4/C5/L5/I10/I11 which it has
// to keep honouring). One bool on `core::Layer`, one `clipRuns()` pass, and a
// three-function bracket in core/Composite that folds a clipping group per
// texel inside the base's own tile walk -- no offscreen buffer, which is the
// prediction this step falsified rather than fulfilled.
//
// Covers, and the first four are the decisions the step turns on:
//  - **A run clips to ONE base**, proven in pixels rather than in bookkeeping:
//    three clipped layers over one opaque base, each covering a different
//    texel, all three visible -- where the cumulative reading (each clipped
//    layer masked by the one below it) would confine two of them to the
//    first's single texel. The coverage half of the same claim is printed
//    beside it: three opaque clipped layers on a 0.5-alpha base leave the
//    coverage at exactly 0.5, against 0.125 under cumulative erosion and
//    0.875 under independent growth.
//  - **Which alpha, and where the group lands.** The base's *effective* alpha
//    -- stored, after its op stack, times its opacity and its mask -- and the
//    group composites internally then lands through the base's blend mode.
//    The rejected reading (each clipped layer composited onto the backdrop
//    independently) is **implemented in the test** and printed beside the
//    built one on a fixture where they differ: a half-transparent base gives
//    0.5/0/0.5 one way and 0.5/0.25/0.75 the other, and a `multiply` base
//    with an opaque white clipped layer gives 0.5 against 1.0. Hiding the
//    base is proven byte-identical to the layers not existing, a 0.5 mask on
//    the base byte-identical to 0.5 opacity on it, and a grade on the base
//    proven to leave the clip boundary bit-identical.
//  - **A clipped Adjustment layer sees its base and nothing else**, which is
//    step 5's `adjustedPremultiplied()` re-pointed rather than rewritten: the
//    same document is composited with the adjustment clipped and unclipped and
//    both are printed, the untouched texels are `memcmp`-identical to the
//    document without the layer, and the partly-transparent-base case shows
//    the grade landing on the base's *straight* colour (1.5) rather than on
//    the composite (2.0).
//  - **`Mix` and a clip are mutually exclusive**, decided in
//    `blendModeAvailableForLayer()` so the dropdown, `setLayerBlend()` and
//    `mixPairing()` cannot disagree: all three arrangements are asserted, the
//    unpaired `mix` is warned about with its own reason and proven
//    byte-identical to `over`, and a mixed pair is proven to work as a clip
//    *base*.
//  - **A layer's own mask and its clip are different operators**: the four
//    combinations of {mask 1, 0.5} x {base alpha 1, 0.5} printed side by side,
//    with the colours multiplying and the coverages not, plus a negative
//    control showing that "clip == mask by the base's alpha" gives a different
//    answer.
//  - **The bottom layer cannot be clipped**: `setLayerClipped()` and
//    `moveLayer()` each refused with the index and the layer count, the two
//    other baseless arrangements refused by their own reasons, and -- because
//    a file can carry what the setters refuse -- the compositor proven to
//    composite such a layer *unclipped*, warn by name, and produce a buffer
//    byte-identical to the flag being clear.
//  - **The regression boundary at zero tolerance**: a document with no clipped
//    layer composited byte-identically to a plain sum written inside the test,
//    and the lazy-open rule proven necessary by showing that the open/close
//    bracket is NOT a bit-exact round trip on a non-dyadic alpha.
//  - **The probe and the flattener agreeing** on a clipped stack, with the
//    largest residual measured and printed against the derived bound.
//  - **The cost claim measured**, not asserted: a clipped layer's tiles
//    outside its base's are never visited, timed against the same layer
//    unclipped.
//  - **The `.npaint` round trip**: `np:clipped` surviving on RGB, Pigment and
//    Adjustment layers, a clipped bottom layer carried rather than refused,
//    and the property the format change rests on -- clearing every clip flag
//    gives back a file byte-identical to one written before any was set.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations.
// Headless and GPU-free; writes and removes three `.npaint` files.
bool runClippingMaskTest();

}  // namespace np
