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

}  // namespace np
