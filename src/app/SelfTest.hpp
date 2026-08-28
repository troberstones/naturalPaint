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

// Headless, GPU-free check on ui/Fonts -- the glyph coverage the layers panel
// depends on. **This section exists because nine other sections could not
// have caught the bug it guards**: nine of them assert `layerKindGlyph()`
// returns the right glyph and all nine pass whether or not that glyph can be
// *drawn*, because they check a return value and not a pixel. The gap was
// found by photographing the panel. Three parts: the UTF-8 decoder the check
// is built on (including the malformed inputs a display path must not hang
// on); that every above-U+00FF glyph the panel asks for is in
// `requiredUiCodepoints()`, with a tripwire that fails if a LayerKind is
// added; and -- the load-bearing one -- that a real merged font atlas can
// actually draw every one of them. Creates and destroys its own ImGui
// context; font baking is CPU-side, so no GPU and no renderer backend.
bool runFontsTest();

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

// Headless check on ui/NaturalPaintUI's mip pyramid (PLAN.md Phase 2 step 9:
// "Mip pyramid for tiles, so a 25% zoom evaluates at a matching level").
// Four things, in order:
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
//  (c) CPU-only, no GPU: tileScreenRect() matches a hand-computed
//      expectation for a known TileCoord/CanvasView/canvasOrigin. This
//      assertion originally lived in runTiledViewportTest(), the headless
//      check on ui/NaturalPaintUI's TiledDocumentView (PLAN.md Phase 2 step
//      8, "Tiled viewport draw") -- a read-only proof of Document -> one GPU
//      texture per occupied tile -> screen, deliberately never wired into
//      the interactive painting canvas. TiledDocumentView was confirmed
//      unreachable from the live application (ui/DocumentTexture became the
//      production Document -> GPU-texture path) and removed along with that
//      test; tileScreenRect() itself owed nothing to the class, so its own
//      correctness check moved here rather than disappearing with it.
//  (d) End-to-end, needs `gpu`: a known, non-uniform (finest-period
//      checkerboard) core::Tile is uploaded via uploadTileMips() (ui/
//      NaturalPaintUI.hpp), then rendered at zoom=0.25 through a small
//      dedicated offscreen WebGPU render pass (app/selftest/Support.cpp's
//      blitPipelineRenderAndReadback(), not ImGui's renderer -- this module
//      needs no live ImGui context), placed at tileScreenRect()'s own
//      computed rect and read back to CPU the same way
//      PaintSim::readbackField() does, binding the level-2 view
//      mipLevelForZoom() selects. The read-back pixels are checked against
//      level 2's known uniform downsampled colour, and -- rendering the
//      identical screen rect from level 0's own view for contrast --
//      checked to differ from what level 0 alone would have produced. This
//      is the assertion that actually proves level selection is wired into
//      a real GPU draw, not just computed and ignored.
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

// Headless, GPU-free check on the **eyedropper tool** (PLAN.md Phase 2 step
// 10's other half -- "the eyedropper, which is the same sampling code writing
// to the foreground colour instead of a readout"; PRD Q10 and PRD L4, both
// P0), on the foreground colour it needed before it could exist, and on the
// tripwire that would have caught its two-phase absence.
//
// **Why a second Probe section rather than more of runProbeTest().** That one
// is about `probePixel()` as a Document query. This one is about everything
// between a click and a colour: three sample *sources* over a stack built so
// they must disagree, the box clipping that only shows up in the alpha, the
// foreground structure a pick writes into, and whether that colour reaches a
// stroke. The two share a fixture idiom and nothing else.
//
// Confirms: `ProbeSource`'s three modes return three different, hand-computed
// colours over a three-layer stack with the active layer in the middle, one
// layer hidden and one at 50% opacity -- with the documented asymmetry intact,
// so a hidden layer is still fully probeable in `CurrentLayer` and contributes
// exactly nothing to either compositing mode, and `ActiveAndBelow` pointed at
// the top layer is bit-identical to `AllLayers`; that point/3x3/11x11 samples
// over a fixture with a linear ramp (which must not move with box size), a
// spike pattern (which must) and a constant match means computed independently
// in the test rather than by a second call to the function under test; that a
// large box at the document's corner and edge averages only the texels
// actually inside it -- alpha 1.0 on an opaque document, not 36/121 -- while
// unpainted texels *inside* the document still dilute coverage to exactly 4/9;
// that every sample size carries Photoshop's own label with both edges spelled
// out, so "9" cannot be read as either 9 px or 3x3; that a pick lands in
// `BrushState::rgb` **sRGB-encoded**, is what `foregroundSrgb()` and
// `foregroundLinearRgba()` return, and is bit-identically what `brushTipFor()`
// hands the deposit routes -- and genuinely differs from the pigment that was
// selected before it, so it cannot pass on a tip that ignored the foreground;
// that picking in PIGMENT mode switches the panel to RGB mode, says so, and
// leaves the pigment selection (and therefore the three physical constants)
// alone; that a pick on a transparent texel or with no document open is
// refused out loud and leaves the foreground untouched; and that
// `toolImplemented()` equals `toolHasCanvasHandler()` for every `Tool`, with
// exactly one recorded exception -- `Tool::Zoom`, which has the identical live
// defect and is asserted every run to still have no handler, so the row must be
// deleted the day it does.
bool runEyedropperTest();

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
// core/Half (docs/architecture-review.md **P0-1**): the CPU's own half
// convert, checked against the software routine it replaced. `core/Half.hpp`
// used to record that this equivalence had been verified once, by hand,
// across the whole input domain -- a real piece of work that was true of a
// moment rather than of the tree. P0-1 makes that equivalence the
// justification for a substitution on the innermost loop of every filter,
// composite, export and save in the application, so the sweep runs on every
// build instead: all 65,536 half values through both `halfToFloat` paths
// exhaustively, and a quarter-million structured floats -- each half's exact
// float, both float neighbours of each, every tie midpoint, and the named
// edges -- through both `floatToHalf` paths, asserting bit-equality. The
// software implementation is kept as both the portable fallback and the
// oracle; deleting it would have deleted the only thing the claim could be
// re-checked against. Two guards stop the sweeps passing vacuously: the NaN
// count is pinned (or a `halfToFloat` returning NaN for everything would
// sweep clean, every pair landing in the skipped branch) and the float sweep
// asserts its own size. The ONE documented difference is the NaN payload --
// the software path forces a mantissa bit so a NaN cannot collapse to
// infinity, the hardware path emits the platform's own -- and that is
// asserted as "both are NaN" out loud rather than quietly excluded. Also
// prints, and on aarch64/x86-64 asserts, that this build really took the
// hardware path: a silent fall back to software is two orders of magnitude
// slower and otherwise invisible. Headless and GPU-free.
bool runHalfTest();

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

// ops/Gradient (PLAN.md "Phase 6 -- Filter and transform it"; PRD D24, and the
// gradient half of D26). The op only -- no editor, no presets. Headless and
// GPU-free, pure CPU tile arithmetic.
//
// Five claims carry it. The three geometries put their stops where the
// geometry says (including that an ANGULAR sweep runs clockwise on screen,
// because document space is y-down); coverage WEIGHTS rather than gates, so a
// partially selected texel comes out proportionally weaker instead of stair-
// stepped; a null Selection means EVERYWHERE, asserted through
// renderGradient()'s own hoisted loop as core/SelectionMask.hpp demands and
// not only through selectionCoverageAt(); colour stops and opacity stops are
// independently positioned, so an opacity stop does not silently knot the
// colour ramp; and interpolation is on STRAIGHT colour in LINEAR light,
// checked against the specific wrong answers -- sRGB's 0.214041 midpoint, and
// the pure red that interpolating premultiplied values returns half way
// through a red-to-transparent-blue fade.
bool runGradientTest();
// ops/Roi, ops/Blur, ops/Feather (PLAN.md "Phase 6 -- Filter and transform
// it"; DESIGN-imaging.md "Class B"; PRD E4). Headless and GPU-free -- pure CPU
// tile arithmetic.
//
// Phase 6's spine, and two hazards carry the section. **The tile seam**: a
// blur that reads only its own tile is exactly right in the middle of every
// tile and wrong by roughly half of full scale on a grid of lines every 128
// texels, so the section asserts that a blur split across a tile boundary is
// bit-identical to the same blur computed in one call, and then computes the
// broken tile-local version on purpose to prove that assertion is sensitive
// rather than merely satisfied. **The ROI direction**: `roiBackward` and
// `roiForward` return the same rectangle for every symmetric kernel, so a
// stack of blurs cannot tell them apart -- they are checked on an asymmetric
// op instead, before the op that needs the distinction exists.
//
// Also here: the accumulator decision measured live (an f16 accumulator's
// error on a 1601-tap kernel, against this build's f32 one and against the f16
// store's own rounding), DC preservation, separability against a direct 2-D
// convolution, the linear-light and premultiplied-alpha domain checks, box
// blur's flat kernel and its full-width divisor -- and PRD E4's feather,
// including the property that put feather in the filter track: a selection's
// absent tile means 0.0, so a feather must write coverage into tiles the input
// never had.
bool runBlurTest();

// ops/Filters (PLAN.md "Phase 6 -- Filter and transform it": highpass as
// `src - blur(src)`, unsharp, offset with wrap, sharpen, add noise, local
// contrast; DESIGN-imaging.md "Class B"). Headless and GPU-free -- pure CPU
// tile arithmetic, like the blur spine it hangs off.
//
// Four hazards carry the section. **The seam, six times**: it asserts for
// every filter in the file that a request split across a tile boundary is
// bit-identical to the same request made once, on a sparse store, at a split
// that is not tile-aligned. Five of them inherit the property from ops/Blur's
// apron; add noise had to be *built* for it, because a stateful generator
// makes a texel's value depend on how many texels were drawn before it -- the
// section computes what a stream PRNG would have drawn at the same texel to
// show what is being prevented. **Premultiplied alpha at a soft edge**:
// sharpening RGB while copying alpha through is the obvious reading of
// "sharpen the picture" and it puts a bright rim inside every antialiased
// cut-out; the shipped path is asserted to leave an un-premultiplied constant
// colour alone to within four store roundings, and the rejected form is then
// computed on purpose and measured drifting by tens of percent, so the
// assertion is proved sensitive rather than merely satisfied. **The two
// domains, in opposite directions**: add noise and unsharp's threshold are
// shaper-domain quantities sitting on a linear-light blur, and the section
// measures the constancy that buys -- the same amount perturbs every level by
// the same 0.83 fraction over six stops, which is what the ACEScct log segment
// predicts -- against what the linear alternative does instead, which is drive
// light NEGATIVE at an ordinary setting. **The exactness budget**: every
// tolerance is one of the two storage formats' own rounding, including the
// claim that a shaper round trip is free, which is checked over all 31 744
// finite positive halves rather than sampled.
//
// Also here: each op's ROI declaration, including offset's -- the first
// production op in the build for which `roiBackward` and `roiForward` return
// different rectangles, which ops/Roi.hpp could previously only pin on a
// synthetic op -- and the honest limit that a wrapped read is not expressible
// as a `RoiOp` at all.
bool runFiltersTest();

// core/SelectionMask (PLAN.md "Phase 7 -- Select and paste"; PRD E1, E2, M1).
// The antialiased coverage store, its constructors, and PRD M1's
// coverage-weighted clear. Headless and GPU-free -- pure CPU tile arithmetic.
//
// The section exists mainly for one hazard: the defaults are the INVERSE of
// core/Mask.hpp's. An absent layer-mask tile reveals (1.0); an absent
// selection tile excludes (0.0); and a null Selection means no restriction
// (1.0) rather than nothing selected. Confusing any two of those yields an
// editor where either nothing can be painted or a marquee does nothing, with
// no wrong pixel anywhere to point at. Both conventions are asserted side by
// side so neither can drift onto the other.
bool runSelectionTest();

// core/Channels + io/NpaintFile's `S####` part (PLAN.md "Phase 7 -- Select and
// paste"; PRD E11, E12, E13). Alpha channels stored in the document, the
// selection<->channel round trip, saved selections, and quick mask. Mostly
// headless CPU tile arithmetic; three sections go through a real `.npaint` on
// disk, because a format claim asserted against a struct is a claim about the
// struct.
//
// Two hazards give the section its shape:
//
//  - **A channel inherits the SELECTION default, not the layer mask's**: an
//    absent tile is 0.0. Backwards, a channel painted on one tile of a
//    four-tile document comes back selecting the other three, and nothing
//    crashes -- so the two conventions are asserted side by side, exactly as
//    runSelectionTest() asserts them, and then the four-tile case is checked
//    directly.
//  - **PRD E11 is not a licence to move the active selection into `Document`.**
//    A *saved* selection is a named channel and is document data; the live
//    marquee stays session state on `app::OpenDocument`. The section snapshots
//    a document through `core::History`, undoes, and asserts that the channel
//    came back while the active selection did not move -- the assertion that
//    fails the day someone "simplifies" the two into one member and makes
//    drawing a marquee undoable.
//
// Also covered: the round trip is exact texel-for-texel *and* tile-for-tile in
// both directions; quick mask's two boundary decisions (entering with no
// selection gives an empty overlay, and leaving with an empty one gives back no
// selection rather than a selection that selects nothing); a document with
// channels saves and reloads them at **zero tolerance** -- HALF carries all 256
// points of the uint8 coverage grid exactly, with 8.06x of margin, measured and
// printed rather than assumed; a document written **without** channels still
// loads, with no channel warning, and writes bytes identical to a second save
// once capDate is masked; a two-channel `S0001` from a newer build is carried
// verbatim and does not steal the part name a real channel needs (PRD I10); and
// the two save refusals -- an unnamed channel, and two channels sharing a name
// -- each asserted on the specific string it names (PRD I11).
bool runChannelsTest();

// ops/FloodFill (PLAN.md "Phase 6" paint bucket + "Phase 7" magic wand; PRD
// D25, D26, E2, E3). Headless and GPU-free.
//
// The wand and the bucket are one algorithm, so they are one module and one
// section. Four decisions carry it. **The tolerance is measured on
// display-encoded values, not linear ones** -- the section measures the 18x
// asymmetry that makes a linear tolerance unusable (the same tolerance spans
// 0.0144 of linear light above black and 0.2621 below white), because the
// failure it prevents is a tool that feels unpredictable rather than one that
// produces a wrong pixel. **The coverage is antialiased** (PRD E2, P0) and the
// ramp's width is derived from the coarsest step of the rgba16float source
// against the 256 levels of the uint8 coverage store -- the section re-runs
// that measurement rather than trusting the constant. **The ramp weights the
// boundary and never moves it**: the same texels are reached with antialiasing
// on and off. And **the bucket has no second opinion**: filling through the
// wand's own selection is asserted to move exactly the texels the wand
// selected, which is what fails if a second tolerance implementation ever grows
// inside the fill.
bool runFloodFillTest();
// core/SelectionShapes (PRD E3's ellipse, lasso and polygon lasso). Headless
// and GPU-free.
//
// The claim it defends is EXACTNESS. Both shapes compute true covered area --
// the ellipse by closed-form integration, the polygon by clipping the texels
// its boundary crosses -- rather than counting samples, so the only permitted
// error is the store's own 1/255 quantisation. Every tolerance in that section
// is stated as a multiple of that step and was measured before it was written.
bool runSelectionShapesTest();

// core/SelectionRefine (PRD E8's grow/shrink, PRD E9's colour and luminance
// range). Headless and GPU-free.
//
// Two claims, and both are about MECHANISM rather than arithmetic. PRD E8
// demands a distance transform "so the radius is a real number and antialiasing
// survives", and iterated dilation would pass every plausible smoke test while
// failing both clauses -- so the section asserts a fractional radius produces
// fractional coverage, an integer one leaves the edge hard, and an antialiased
// boundary texel survives grow-by-zero bit-exactly while a thresholded copy of
// the same edge is destroyed. Euclidean-ness is checked against
// core/SelectionShapes' analytic disc, reached by closed-form area integration
// with no distance field anywhere.
//
// PRD E9's hazard is the one ops/FloodFill.hpp names: two implementations of
// "similar colour". The section asserts colour range fed the colour under a
// texel returns the IDENTICAL selection to a Global flood fill seeded there, so
// a second tolerance metric is a test failure and not a user complaint.
bool runSelectionRefineTest();

// core/SelectionBoundary -- the TRUE outline of a selection, which is what PRD
// E6's marching ants draw. Headless and GPU-free.
//
// **The section exists because a green suite once agreed with a bug.** The ants
// were drawn from `selectionBounds()` -- exact for a rectangle, a bounding box
// for everything else -- and nothing here noticed when PRD E3's lasso, polygon
// lasso and wand landed and every selection started drawing as a rectangle.
// Every existing assertion was about the selection MODEL, which was right in
// each case; the picture was the only thing wrong, and four separate bug
// reports were that one picture.
//
// So every assertion is one a bounding box passes and a real trace does not, or
// the reverse: a rectangle is still exactly 2*(w+h) unit edges; an L visits its
// concave corner and NOT the box corner outside the shape; a ring produces a
// second, inner contour, because a hole drawn as filled says the opposite of
// what the selection does; and a Shift-add of two disjoint rectangles produces
// two contours of 24 edges rather than one box of 52.
//
// Three more decisions are pinned here rather than left to prose. **Select All
// draws the canvas edge and not nothing**, which follows from
// core/SelectionMask.hpp's inverted default (outside a selection is coverage 0,
// so the document edge is a real edge). **The coverage threshold is 0.5**, the
// contour Photoshop draws, and the section measures how much larger the
// rejected "any coverage at all" outline is on an antialiased ellipse. And the
// **cache is asserted to invalidate**, in a form that fails both for a cache
// which never refreshes and for one which refreshes into a stale copy -- a
// frozen outline is invisible to any test that draws once.
//
// Cost is measured, not assumed: a full-canvas selection on 2048x2048 against
// PRD F3's 20 ms frame, beside the cached re-ask it has to be cheaper than.
bool runSelectionBoundaryTest();

// The intent rules behind PRD E3's five selection tools -- ui/MacPaintUI's
// commitDrawnSelection(). Headless and GPU-free.
//
// Not arithmetic: what an empty gesture MEANS, and what refining an absent
// selection means. Each rule picks between two plausible behaviours and
// neither produces a wrong pixel, so nothing else in this suite -- and no
// golden image -- can catch one written backwards.
bool runSelectionToolsTest();

// docs/testing-issues.md T10 ("The three selection-drag gestures are
// missing"): app/SelectionDrag.hpp's pure geometry behind Shift-constrain,
// Option-from-centre and Space-move on the rectangle/ellipse marquee drag.
// Headless and GPU-free -- arithmetic on a handful of floats, no ImGui
// involvement, the same split app/CurveEdit.hpp's own doc comment describes.
//
// What a golden image cannot catch here: a constrained square that jumps to
// the wrong quadrant on a drag toward the upper-left (the sign-preservation
// mistake `std::max` without `std::copysign` makes), a from-centre toggle
// that only works in one direction because the anchor got rewritten instead
// of reinterpreted, and the classic Space-move bug of translating only one
// of the anchor/current pair -- which still visibly "moves" the shape, just
// while quietly resizing it too.
bool runSelectionDragTest();

// docs/testing-issues.md T13 ("The ellipse marquee draws a rectangle while
// you drag it"): app/SelectionDrag.hpp's ellipseMarqueePreviewPoints(), the
// point run behind the ellipse marquee's live rubber band. Headless and
// GPU-free.
//
// The live preview only ever draws between a real mouse-down and mouse-up
// on the canvas, which nothing --selftest runs reaches (see this section's
// own file for why --marquee-demo does not help either), so the claim
// tested is not visual: it is that the preview's own returned points agree
// with core/SelectionBoundary's trace of what the commit path actually
// builds, for the identical box -- and, under Option-from-centre, that
// feeding the generator the drag's raw corners instead of that box (T13's
// original bug, one level down) visibly disagrees with it.
bool runEllipseMarqueePreviewTest();

// The LAYERS panel as design "naturalPaint Panels" turn 2, option 2a specifies
// it -- app/LayerPanel's half of it, which is nearly all of it. Headless and
// GPU-free.
//
// The panel is mostly string assembly and table lookup, so this is where the
// design's own example rows are asserted character for character: the kind rail
// (seven distinct colours, none of them the colour of the row it marks), the
// `NEW` popup (all seven kinds, exactly the three `core/LayerOps` can make
// wired to their own commands), the `LINKED+n` badge, the `KIND: ALL` chip and
// the header count.
//
// **The other half is the omissions**, which is what makes it worth a section
// of its own. Three pieces of 2a are deliberately not drawn because nothing in
// the model can supply them -- a Media layer's `WET 4.2s` countdown, a Flats
// layer's `153 FILLS`, and the popup's `SHIFT-CMD-N` shortcut column. Each is
// pinned by an assertion that fails the moment the number or the binding
// appears, because an omission with only a comment behind it is one a later
// revision reverses by reaching for a plausible-looking value.
bool runLayerPanel2aTest();

// ops/Transform (PLAN.md "Phase 6 -- Filter and transform it"; PRD D14, D15,
// D16, D17). Headless and GPU-free -- pure CPU resampling.
//
// Three of the section's claims are ones a golden image cannot make:
//
//  - **D15 is bit equality.** A flip is a relabelling of the pixel grid, so
//    "close enough" is exactly the filtered implementation the requirement
//    forbids. Flips and quarter turns are compared with memcmp at zero
//    tolerance, including four turns of a NON-square image through the
//    transposed extent, and the composition flip . rot90 is checked to stay on
//    the exact path so two lossless edits cannot stack into a lossy one.
//  - **D16 is a claim about a process.** Three stacked rotations composed into
//    one matrix land in the same place as three applied in turn -- no
//    geometric test separates them, and the section measures that agreement
//    (3.1e-5 px) before relying on it. It then rotates 60 degrees and back
//    along both routes and compares each against the ORIGINAL: the composed
//    route (2 resamples) measures ~1.46x less RMS error than the stacked one
//    (4 resamples), and an 8-deep stack widens that to ~1.87x while the
//    composed cost stays flat.
//  - **D17's downscale clause is about what is NOT in the output.** 1-pixel
//    stripes reduced 256 -> 35: prefiltered, the result is flat to sd 0.035;
//    unfiltered it swings at sd 0.35, loses the true mean on a period-3
//    pattern, and rings into negative linear light. The reduction is
//    deliberately not a power of two -- at an exact 8x the broken path
//    accidentally passes, and the section measures that too so the choice of
//    ratio is visible rather than arbitrary.
//
// Also here: the five kernels at their defining radii; a constant opaque field
// surviving all five across the whole image *including the border*, which is
// the assertion that caught an edge policy leaving opaque images translucent;
// premultiplied-versus-straight averaging measured as a whole channel of
// difference; crop and canvas size proven to be bit-exact index copies; the
// four-corner homography solve and its refusal of a degenerate quad; and the
// tile-store bridge's allocation behaviour in both directions.
bool runTransformTest();

// ops/DocumentTransform (PLAN.md "Phase 6 -- Filter and transform it"; PRD D14,
// D16, D17, E10). Headless and GPU-free -- runTransformTest()'s entry point,
// one level up, on real `Document`s and `Layer`s.
//
// runTransformTest() proves the resampler. This one proves the four claims that
// only exist once a Document is involved, because each is about a store the
// resampler has never heard of:
//
//  - **A crop moves the MASK.** `core::Layer` has no offset field -- tiles are
//    keyed in absolute document coordinates -- so a crop is an integer
//    translate of every store, and translating `rgbTiles` and not `mask` gives
//    a composite that is internally consistent and a mask that has slid off the
//    content it was painted for. The section asserts pixels, mask and selection
//    all landed at the same offset, and asserts the pixels landed bit-identical
//    (0 of 30 000 texels differ) rather than close.
//  - **A layer mask must NOT transform in coverage space.**
//    `transformImage()`'s outside-the-source policy is transparent black, which
//    for a selection means "unselected" (right) and for a mask would mean
//    "hidden" (a document that vanishes). Measured: the naive packing hides
//    1 649 of 3 249 destination texels of one 30-degree rotation. The shipped
//    hide-space packing (`1 - coverage`) leaves them revealed, and is bit-exact
//    for all 65 536 half words.
//  - **A pigment latent passes only through a positive-weight kernel, and only
//    MASS-WEIGHTED.** DESIGN-imaging.md section 3 puts `resample` in its
//    valid-on-latents column, so refusing pigment layers would refuse the
//    default layer kind; but a negative lobe drives the implied fourth pigment
//    weight outside the model (measured on a hard latent edge: Catmull-Rom
//    -0.0438 on a pigment weight, Lanczos3 -0.0753, both lobe-free kernels
//    exactly +0.0000). The section checks the restriction is a TYPE rather than
//    a runtime test, that `latentKernelFor()` refuses a ringing kernel by name
//    instead of rounding it down, that every output latent stays inside the
//    convex hull of the inputs that had mass (excursion 0.000e+00), and that a
//    straight (unweighted) average of the same field differs by 0.6061 of
//    latent -- which is not a fringe, it is a different pigment.
//  - **D16 is a claim about a process at document level too.** A layer rotated
//    60 degrees out and back by two routes that land identically: 2 resamples
//    against 6, measuring 1.778x less RMS error, widening to 2.647x at 16 steps
//    while the composed cost stays flat at 2.
//
// Also here: all nine canvas anchors with the odd pixel floored; a 1:1 image
// size proven bit-identical rather than run as an identity resample; document
// flips and quarter turns at zero resamples with memcmp equality; the
// area-average prefilter reaching the pigment path (mass stripes 256 -> 35, sd
// 0.0324 against 0.2939); PRD E10's selection transform on the exact path with
// every coverage byte intact; and the locked-layer split -- a per-layer
// transform refuses one by name, a document-level grid change moves it and
// reports the count.
bool runDocumentTransformTest();

// app/TransformSession (docs/reachability-audit.md C1; PRD D14, D16, E10): the
// pure, headless session behind an interactive layer/selection transform --
// handle geometry and hit-testing, the drag semantics for Move/Rotate/scale
// (Shift aspect-lock, Option scale-about-centre, both read live), and commit
// through the existing ops/DocumentTransform + recordEdit() undo funnel.
// Proves the matrix-not-running-resample discipline (a multi-frame drag reads
// the source zero times; commit reads it once), that composition order is
// respected (rotate-then-scale is not scale-then-rotate), and that
// exactRemapKind() keeps a pure translate or a snapped quarter turn lossless.
// Headless and GPU-free -- nothing here touches ui/.
bool runTransformSessionTest();

// ui/TransformPreviewTexture (docs/testing-issues.md T14): the CPU half of a
// Free Transform's live pixel preview -- crop `sourceBounds` out of a layer,
// through a selection's coverage weighting or the whole layer for a null
// one, and pack it straight-alpha for ui/CanvasQuad, matching
// ui/DocumentTexture's own convention. Proves a null selection copies the
// whole layer; a real selection crops to it AND leaves a covered-but-outside-
// selection texel transparent (the same "absent tile" rule
// imageFromTileStore() already documents, so this preview's edges do not lie
// about the selection's boundary); un-premultiply actually runs (a
// premultiplied (0.25,0,0,0.5) becomes straight (0.5,0,0,0.5), not passed
// through); a Pigment layer is refused rather than shown wrong (this file's
// own named scope reduction -- no latentToRgb() projection here); and an
// empty region is refused up front. Also measures the cost this crop pays
// ONCE per session (at `begin*()`, never per drag frame) at a realistic
// 2048x2048 fully-opaque layer against PRD F3's 20 ms -- printed, not
// gated by `check()` (a wall-clock figure is this suite's own documented
// flake class) -- and the number is OVER budget; see
// ui/TransformPreviewTexture.hpp for the honest account of why and what a
// follow-on fix looks like. Also a sabotage proof, in the identical
// bit-for-bit `memcmp` idiom app/selftest/TransformSession.cpp's own
// section 5 uses for `commit()`: the source layer's tiles, read
// independently before and after a call, are unchanged -- the safety net
// for THIS file's copy of app/TransformSession.hpp's "a reader must never
// become a writer" invariant, since TransformSession's own proof never
// calls through here (this file is ui-only and unreachable from that
// headless test). Headless and GPU-free -- only the GPU upload wrapper is
// untested here, matching this suite's own precedent for the same shape of
// class (app/selftest/DabPreview.cpp never touches DabPreviewTexture's
// wgpuQueueWriteTexture() call either).
bool runTransformPreviewTextureTest();

// core/Clipboard (PLAN.md "Phase 7 -- Select and paste"; PRD M1, M3, M4, M5,
// M8). Headless and GPU-free.
//
// Two claims carry the section. **PRD M5** -- the clipboard is a copy-on-write
// tile reference, not a flattened buffer; the PRD calls that Lightweight
// rather than a convenience and names the number (a 4K copy is 68 MB, which
// PRD A5 forbids holding invisibly). So the section measures the split: tiles
// a selection covers whole are shared, and only the tiles its edge crosses
// cost bytes. **The two weighting rules** -- an RGB texel is premultiplied so
// coverage scales all four channels, while a Pigment texel is a straight
// latent plus a mass that is the alpha analogue, so coverage scales MASS
// ALONE (PRD F10). The pigment assertions check the latent is bit-unchanged
// at partial coverage, because scaling it by reflex from the RGB path is the
// natural mistake and turns a half-copied red into something that is no
// longer red.
bool runClipboardTest();

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

// The brush dynamics link model (design "naturalPaint Panels" turn 4a):
// what one link resolves to, and how several links onto one target combine.
// The DYNAMICS matrix draws an identical square whether the link behind it is
// inverted, clamped or wrong by a factor of two, so a golden image cannot
// check any of this. Pure CPU -- brush/Dynamics.hpp has no ImGui, GPU or
// PaintSim involvement at all.
bool runBrushDynamicsTest();

// A6 (docs/reachability-audit.md): the DYNAMICS matrix's dead half. Four
// sources that used to be hard `0.0` forever -- VELOCITY, FADE, NOISE,
// RANDOM -- and six targets nothing ever read, of which five now do
// (SCATTER, CONCENTRATION, HUE, SATURATION, VALUE) and one is an honest
// refusal (WETNESS, brush/Dynamics.hpp's `targetUnbuildableReason()`). The
// centrepiece is determinism: the same stroke replayed through two
// independent `StrokeSession`s deposits bit-identical tiles, including
// through the two stochastic sources, because `core/History` replays a
// stroke's own dab stream and the golden harness compares pixels across
// runs of the same binary. `BrushDynamics.cpp` proves the link model this
// builds on; not restated here.
bool runDynamicsSourcesTest();

// The BRUSH EDITOR's tip preview (app/DabPreview): a real dab, rasterised
// through the deposit's own `dabCoverage()` and `depositTexel()`, at three pen
// pressures. The assertion the whole thing turns on is that the preview and a
// real `depositDab()` into a real `PigmentTileStore` agree texel for texel --
// a preview with a falloff of its own would drift the first time brush/Deposit
// §2 changed, and a preview that has drifted is worse than the abstract bar it
// replaced. Also covers brush/Deposit §2b's elliptical tip, which building
// this preview is what found: `roundness` and `angle` had five surfaces
// telling a user they shape the brush and reached no dab at all. Pure CPU --
// the rasteriser has no ImGui, GPU or PaintSim involvement.
bool runDabPreviewTest();

// io/AbrBrushes: reading Photoshop `.abr` brush libraries into brush/Library
// presets. The container framing (which parses a format from the internet and
// must refuse rather than guess) and the parameter mapping (where a wrong
// value is still in range, still plausible, and still paints). Every byte is
// written by app/selftest/DescFixture.hpp -- a real brush pack is somebody
// else's copyrighted work and megabytes besides.
bool runAbrBrushesTest();

// docs/reachability-audit.md B6 and B7: a Multiply target's floor, applied
// exactly once rather than once per contributing link. `BrushLinkSet::
// multiplyFloor` (brush/Dynamics.hpp), `BrushTip::sizeFloorPx`
// (brush/Deposit.hpp) and the three sites that consume it
// (`app/StrokeSession.cpp`'s `depositPending()` and `app/DabPreview.cpp`'s
// `dabPreviewTipsFor()`, both through `brushTipFor()`; `ui/MacPaintUI.cpp`'s
// solver route, which never builds a `BrushTip` at all). The centrepiece is
// the worked counter-example proving the floor may not be applied in both
// halves of the product `app/StrokeSession`'s hardware/stroke-local split
// creates: with `rangeHi` legally above 1.0, flooring twice overshoots the
// correct answer. Also covers the io/AbrBrushes importer's honest-range
// decomposition (that assertion lives in `AbrBrushes.cpp`'s own section 5,
// not restated here) and the `app/UserBrushLibrary.cpp` persistence of the
// new per-target floor.
bool runMultiplyFloorTest();

// app/selftest/ShelvedLinks.cpp: the dedicated test for the shelved 10x12
// link matrix (brush/Dynamics.hpp) -- a `user-presets.txt` fixture's
// link/floor/point lines round-trip byte-for-byte with no live
// `BrushLinkSet` ever built from them, and a hand-built, non-empty
// `BrushLinkSet` changes nothing `app/StrokeSession::brushTipFor()` reads.
bool runShelvedLinksTest();

// app/StrokeSession's applyPerDabScatter(): docs/reachability-audit.md B5's
// axis defect. Asserted geometrically -- each dab's displacement is
// projected onto the stroke's own tangent and perpendicular -- that the
// default (Photoshop's own default, "Both Axes" unticked) confines a dab to
// the PERPENDICULAR, that setting `BrushTip::scatterBothAxes` restores the
// old full-circle isotropic spread by contrast on the identical stroke, and
// that a stroke's first dab (no tangent yet) scatters along a deterministic
// due-+y axis rather than an undefined one. Pure CPU, no document, no GPU.
bool runScatterTest();

// Phase C Part 1: Scatter Count (`PsScatter::count`/`countJitter`, `Cnt ` on
// disk) resolved per dab in `app/StrokeSession.cpp`'s `depositPending()` and
// dispatched as N sub-dabs per nominal stroke position, each with its own
// `applyPerDabScatter()` offset. Asserts the no-op at `resolvedCount == 1`
// (dabCount()/texelsWritten()/strokeTiles() bit-identical to no model at
// all) and the measured claim that count == 3 writes exactly 3x the texels
// of count == 1. Pure CPU, no document window, no GPU.
bool runScatterCountTest();

// io/AbrBrushes' `samp` block and brush/Deposit.hpp §2c: a sampled bitmap tip
// decoded (raw and PackBits, both subversions' header skip), matched to a
// preset by UUID, resolving a `#Prc` `Dmtr` against the sample's own pixel
// dimensions, and sampled by `dabCoverage()`/`dabPixelBounds()` in place of
// the procedural profile -- plus the two places a live bitmap tip could leak
// into the wrong picture (`DabPreviewCache`, an unloaded `BrushRow`'s icon)
// found while wiring this in. Every byte is synthetic, built the same way
// app/selftest/AbrBrushes.cpp's own fixtures are; a real `.abr`'s bytes
// informed the framing (see io/AbrBrushes.hpp's header) but never touch
// --selftest. Headless and GPU-free.
bool runAbrSampledTipsTest();

// io/PsPatterns: the `patt` block -- Adobe's published Pattern structure plus
// the Virtual Memory Array List, which is 98-99% of the bytes of every brush
// pack that has one and was read by nothing at all until now. Hand-built
// fixtures only; no real pack's bytes are in this repository. The three
// sections that matter are the ones the specification does not warn you
// about: `numberOfChannels` reading 24 and not being a count, the four-byte
// record alignment, and the four-byte short tail. Headless and GPU-free.
bool runPsPatternsTest();

// io/GimpBrush: `.gbr` and `.gih`, read for the dab library. Hand-built
// fixtures ONLY -- no file GIMP itself wrote was available where this was
// written, so these prove the reader agrees with the published standard and
// cannot prove the standard agrees with GIMP. io/GimpBrush.hpp names the one
// inference that leaves open (mask polarity) and what would settle it.
// Headless and GPU-free.
bool runGimpBrushTest();

// brush/Variance: Photoshop's `brVr` object -- the same four fields at twelve
// sites -- resolved by one formula. The sections that matter are the two audit
// findings this type makes UNREPRESENTABLE rather than merely fixed: B6's
// squared floor (the minimum is outside the product) and B7's
// absent-device-resolves-to-the-floor (an unavailable axis contributes
// identity). Headless and GPU-free.
bool runVarianceTest();

// brush/CoverageBlend: the ten ways two coverage values combine into one --
// the Dual Brush's `BlnM` and the Texture panel's `textureBlendMode`, which
// are the same question asked in two panels and now share one table.
//
// Four of the arms MOVED here out of `combineDualCoverage()`, so this checks
// them against expressions written out by hand rather than against the
// function under test. And it pins the invariant BOTH callers depend on and
// neither could recover from: **no blend may create coverage where the
// primary has none** -- a second tip must not paint outside the first tip's
// footprint, and `depositDab()` only visits texels `dabCoverage()` already
// accepted. Exactly one mode can violate it (Hard Mix), which is why the
// guard lives in the shared function.
//
// Pure arithmetic: no document, no tile, no GPU, and every asserted value
// exact in binary floating point, so the comparisons are `==`.
bool runCoverageBlendTest();

// **A real scanned paper under the brush** -- `GrainParams::field`, and the
// three deposit routes that never asked for grain at all.
//
// brush/Grain generates its height field procedurally (its own §0: the patent
// names a stored table, this codebase ships no binary fixtures). A `.abr`
// carries the real thing in its `patt` block -- 98-99% of the file's bytes,
// named by 84 of the 101 presets measured -- and io/PsPatterns now decodes it.
// Sections A-C are the sampling: nearest, wrapped at negative coordinates,
// scaled, and shaped by brightness/contrast/invert in the panel's own order,
// with a null field falling through to the procedural path bit for bit.
//
// **Sections D-F close a gap rather than guard a feature.** `grainCoverageAt()`
// was called from the CPU Pigment deposit and the preview and NOWHERE else, so
// paper texture worked on a Pigment layer and silently did nothing on an RGB
// layer -- the layer an ordinary File > New selects. Those assertions fail on
// the code as it stood.
//
// The field is a checkerboard at depth 1.0, not a scan and not a hash, so the
// outcome is binary and exact: a peak texel takes no paint at all, a valley
// takes it in full, and "the grain call is missing" reads as "the disc is
// solid". Headless and GPU-free.
bool runPaperTextureTest();

// app/DabLibrary: the watched folder, and the two claims it stands on.
//
//   * **An unchanged rescan decodes nothing** -- asserted against
//     `decodeCount()`, not described. It is what makes a scan cheap enough to
//     run on a window-focus event, and a rescan that quietly re-decoded a
//     500-file folder would pass every correctness assertion here and still
//     make the feature unusable.
//   * **A rename does not orphan a preset** -- a `file:` id names a path, so
//     the index carries a fingerprint of the decoded coverage and a rescan
//     recognises the file under its new name. Section E walks up to one of
//     the edges that mitigation admits to.
//
// Also: the one rule that turns a picture into coverage (a real alpha channel
// if there is one, otherwise `1 - display-encoded luminance`), the index's
// escaping and version discipline, and that a library with no folders scans
// to nothing without creating a folder or a stub index.
//
// Runs entirely against a scratch directory under the system temp path --
// the roots are injected, so this never touches
// `~/Library/Application Support/naturalPaint`. Headless and GPU-free.
bool runDabLibraryTest();

// app/DabLibrary's PATTERN extraction -- `extractAbrPatterns()` and
// `patternsImportedRootPath()`, the `patt`-block sibling of the tip
// extraction `runDabLibraryTest()`'s own section G proves.
//
// A decoded `PaperField` written to `patterns-imported/<uuid>.png` reads back
// byte for byte (single-channel, not alpha-over-black -- a height field
// answers to none of the coverage-polarity rules a tip's mask does), a second
// import of the same uuid leaves the file on disk alone, and a malformed id
// is refused rather than trusted as a path component. A synthetic
// `PaperField` fixture throughout, not a real `.abr`. Runs entirely against a
// scratch directory under the system temp path. Headless and GPU-free.
bool runPatternExtractTest();

// ui/DabPicker's arithmetic -- the half of a picker that can be wrong in a way
// a screenshot would not show. `--selftest` cannot reach an ImGui dispatch
// site (reachability-audit F4), so everything here is a pure function of
// numbers: column fit, cell placement, the hit test, thumbnail letterboxing,
// atlas paging.
//
// The load-bearing section is the hit test, checked as the INVERSE of the cell
// placement over every cell of twelve layouts rather than against a
// hand-written table -- a hit test derived independently from the layout is
// exactly how a picker ends up selecting the cell next to the one clicked, a
// bug that looks like a rendering glitch and is not one. Headless and GPU-free.
bool runDabPickerTest();

// ui/BrushSettingsWindow's tab table -- which groups of brush settings exist,
// what they are called, and that each row of the table carries its own id.
//
// Same reason as the picker above: `--selftest` cannot reach an ImGui dispatch
// site (reachability-audit F4), so a tab strip written as a run of
// `BeginTabItem()` calls has no assertions on it and a group dropped in a
// later edit is invisible until a painter goes looking for a control that is
// no longer anywhere. The load-bearing assertion is that the table is indexed
// by its own enum -- ui/MenuModel's spec table has the identical hazard, and a
// row out of order draws one group's controls under another group's name,
// which is completely invisible on inspection. Headless and GPU-free.
bool runBrushSettingsWindowTest();

// brush/BrushModelIo: the text format for a `BrushModel` (Photoshop's Brush
// Settings panel, ~117 addressable leaves once every `Variance` is counted
// out) and the one templated visitor (brush/BrushModelIo.hpp's
// `visitBrushModelFields()`) both `brushModelToLines()` and
// `brushModelApplyLine()` walk instead of ~117 hand-written branches. Round-
// trips a model with every field set to a distinct value at zero tolerance,
// asserts a default model writes zero lines, that every path is unique, that
// the path count is pinned to a literal (so a field added to `BrushModel`
// without a matching visitor call fails here), and that an unknown path, a
// malformed value, and an out-of-range enum ordinal are each refused without
// mutating the model. Headless and GPU-free.
bool runBrushModelIoTest();

// brush/BrushModelDiff: `brushModelDiff()`/`brushModelEqual()`, the two
// functions that will let `presetMatches()` compare BrushModel's full ~151
// leaves instead of the 14 scalars it checks today, and let a round-trip
// test name which field did not survive instead of just that one didn't.
// Both are built on ONE templated visitor (brush/BrushModelDiff.hpp's
// `detail::visitBrushModel`) walked in two modes: read-only over two const
// models here, and read-write over one mutable model in this file's own
// "every field is reachable" section, which sets exactly the k-th leaf (in
// the visitor's own walk order) and nothing else, then asserts the diff
// names exactly that path. Also pins `brushModelDiffPaths().size()` to a
// literal, so a field added to BrushModel without its one visitor line
// fails here instead of silently under-counting. Pure CPU, no GPU, no file
// I/O -- BrushModel is a plain struct.
bool runBrushModelDiffTest();

// track10/angle: is the angle input interpreted correctly? An independent
// (never-read-back-from-the-code-under-test) geometric pin on two claims --
// `BrushTip::angle`'s positive sense is clockwise on screen (brush/
// Deposit.hpp §2b), and a DIRECTION -> ANGLE link actually turns the tip's
// elongated footprint to face the stroke's own travel vector, not 90 degrees
// off and not mirrored. Headless and GPU-free -- brush/Deposit.hpp and
// brush/Dynamics.hpp alone, no importer involved.
bool runAngleConventionTest();
// io/AbrBrushes' Dual Brush support and brush/Deposit.hpp §2d: a SECOND tip,
// read through the same `Brsh`-shaped reader the primary tip uses, combined
// per-texel with the primary tip's own coverage by `BlnM` (Multiply and
// Overlay, the two this build composites), threaded through
// `BrushPreset`/`BrushState`/`BrushTip` the same route `tipBitmap` already
// takes, and refused to recurse -- a nested tip's own `.dualTip` is never
// read, at import time or at composite time. Also covers what this step does
// NOT honour: a Dual Brush blend mode other than Multiply/Overlay (counted
// and named, never silently dropped), and the second tip's own spacing,
// scatter and count (also counted and named, never silently approximated
// without saying so). Every byte is synthetic, the same discipline
// app/selftest/AbrBrushes.cpp and app/selftest/AbrSampledTips.cpp already
// hold themselves to. Headless and GPU-free.
bool runAbrDualBrushTest();

// app/BrushLibraryFile: the preferences file that remembers which `.abr`
// libraries are loaded, the row cache that lets a remembered library draw its
// brushes with its file unread, and the unload that has to remove exactly one
// library's presets. Four things that fail silently and expensively -- a
// hand-edited or truncated file that loses libraries it could have kept; a
// lazy load that is not lazy (asserted with a read counter, because an eager
// load produces every correct answer); a failed load that is a silent no-op;
// and an unload that shifts every later preset index and quietly selects a
// different brush. Also asserts the design's central claim, that a cached row
// of seven numbers rasterises the byte-identical dab the loaded preset does.
// Writes `.abr` fixtures byte by byte through app/selftest/DescFixture.hpp and
// removes every one of them; `$NP_BRUSH_LIBRARIES` keeps it out of
// ~/Library/Application Support. Headless and GPU-free.
bool runBrushLibraryFileTest();

// app/UserBrushLibrary: the presets a user made, saved so they are still
// there tomorrow (PRD G6; reachability-audit.md's A7 -- `BRUSH EDITOR > Save`
// used to overwrite `lib.presets[lib.active]` in memory and nowhere else).
// Asserts the round trip a preset actually needs, not the shape of the
// structs that carry it: a preset with a `BrushLinkSet` comes back with every
// `Curve` control point at zero tolerance, and a linked preset previews
// differently from an unlinked one so that round trip cannot be vacuous.
// Also covers what Save does to a built-in (forks a new preset rather than
// overwriting one nothing can restore), a durability failure simulated by
// abandoning a `.tmp` file mid-write, unknown lines from a newer build
// surviving a save by this one, and that the `.abr` registry (app/
// BrushLibraryFile) is undisturbed by any of it. `$NP_USER_PRESETS` keeps it
// out of ~/Library/Application Support. Headless and GPU-free.
bool runUserBrushLibraryTest();

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

// PLAN.md Phase 5 step 15 ("**Native save/load carrying layers and latents**,
// with the pigment basis stamped and a baked RGB composite embedded"); PRD C8
// (P1) and I4 (P0).
//
// Most of that sentence was already true at step 3 -- io/NpaintFile round-trips
// layers and latents with zero loss and regenerates part 0 on every save, and
// runNpaintFormatTest() and runPigmentLayerTest() own those claims. What was
// missing is the word "stamped": the basis was a constant in io/NpaintFile, so
// every file this build wrote said "mixbox-v1" whatever it had opened. This
// section covers the field that replaced it:
//
//  - **The field**: on `core::Document` beside `workingSpace`, because it says
//    what the latents *mean* exactly as the working space says what the RGB
//    numbers mean. Asserted to survive undo and redo, which is the argument for
//    that placement over `app::OpenDocument`; and its default asserted to be
//    short-string-optimised, so a history entry pays no allocation for it.
//  - **One constant, not two**: io/NpaintFile's `kNpaintPigmentBasis` is checked
//    by *pointer* to be core/Document's `kPigmentBasisMixbox`, because string
//    equality would still pass after the drift that check exists to catch.
//  - **The decision**: a file declaring a basis this build cannot interpret
//    loads, keeps the string verbatim, warns by name, and saves back out under
//    its own basis -- proven across two generations with **no carry at all**, so
//    the document is the only place the value can have come from. A document
//    whose own basis and whose file's disagree, and which holds Pigment layers,
//    is still refused by name; that rejected reading is run beside the built
//    one. An empty basis is refused rather than written as an absent attribute.
//  - **Byte-identity**: an ordinary document's file is unchanged by the step,
//    proven three ways with the comparator's non-vacuity proven too. The
//    measurement behind it is a fact about the format worth knowing: a
//    `.npaint` is **not** byte-reproducible, because OpenEXR stamps a wall-clock
//    `capDate` per part. Those three fields are masked; everything else is
//    deterministic.
//  - **The adjacent debt**, nothing to do with the basis:
//    `exportDocumentWithRequest()` called the one-argument
//    `flattenDocumentToLinear()`, so `ExportResult::warnings` was always empty
//    from it and every Export As -- and every batch item io/ExportStates drove
//    through it -- reported no warnings whatever the document held.
//    io/ExportStates.hpp had the gap written down. Asserted with a negative
//    control, on a refused request as well as a successful one, and against
//    `exportDocument()`, which was correct all along.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations --
// the field, the constant and the export half are pure, and the OFF build's
// answer for the file half is that `saveNpaint()`/`loadNpaint()` refuse by name
// (PLAN.md §1.5). Headless and GPU-free; writes and removes eight `.npaint`
// files.
// The solver-to-document mass mapping (PLAN.md roadmap section 11): what a
// texel of sim/PaintSim's deposited pigment fields becomes as a PigmentTexel.
// Asserts that a baked tile renders the SAME pixel core/Composite would --
// which is what makes a bake invisible -- that `over` on Beer-Lambert
// coverages equals Beer-Lambert accumulation exactly (so a rolling bake cannot
// drift from one bake at the end), and that the constants the mapping shares
// with shaders/composite.wgsl still agree, by READING that shader rather than
// transcribing it. Headless and GPU-free; identical in both NP_USE_OIIO
// configurations.
bool runPigmentBakeTest();

// track8/solverio, docs/reachability-audit.md's B1: does dried solver paint
// SAVE? app/selftest/StrokeBridge.cpp already proves the bake reaches a real
// Pigment layer with one history entry and an exact undo, live on the GPU;
// this file is the one thing that left untested -- a baked layer run through
// saveNpaint()/loadNpaint() and compared texel by texel over the whole
// affected region, plus the one-entry/undo/redo invariants re-checked
// headless, right next to the round trip they protect. Headless and
// GPU-free: it calls bakePigmentTileFrom() directly on hand-built solver
// output rather than driving StrokeBakeCycle through a real PaintSim.
bool runSolverPersistenceTest();

// The stroke bridge's dirty-and-drying query (PLAN.md roadmap section 11):
// PaintSim::readTileOccupancy() reduced against a full-field readback of the
// same fields, so the cheap answer is proven not to under-report the expensive
// one -- the failure mode that would silently lose paint. Then the whole path,
// occupancy -> readback -> core/PigmentBake, on a real solver stroke. Needs the
// GPU.
bool runStrokeBridgeTest(GpuContext& gpu);

bool runPigmentBasisTest();

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

// ---------------------------------------------------------------------------
// UI detour step 2 -- the document, on screen
// ---------------------------------------------------------------------------
//
// Phase 5 built nine steps of document model and none of it could be seen:
// `compositeDocumentPremultiplied()` had two callers, io/Export and this file,
// so the only way to look at a document was to export it and open the export
// in something else. ui/DocumentTexture is the missing edge -- composite,
// un-premultiply, upload as RGBA16Float, draw on the canvas quad -- and this
// section is what makes each of its three decisions a fact rather than a
// preference.
//
// Covers:
//  - **core/Premultiply, the promoted guard.** The `a <= 0 -> {0,0,0,0}` rule
//    existed in FOUR retyped copies (core/Probe, io/Export, ops/PointOps,
//    ops/Resample), two of which carried notes predicting this promotion.
//    Asserted once as arithmetic -- including that `<=` and not `==` is what
//    stops a negative alpha from negating colour -- and then asserted at
//    **all five call sites at once**: an empty document probed, flattened,
//    graded, resampled and composited-for-upload all return exactly
//    {0,0,0,0}. The template's reason to exist is measured too: the double
//    instantiation is shown to give an answer the float one cannot, and
//    ops/Resample's opaque-alpha regression (its own comment's 0.999996 JPEG
//    refusal) is re-made at zero tolerance after the rewrite.
//  - **Straight alpha, with the rejected reading run beside it.** ImGui's
//    pipeline blends `(SrcAlpha, OneMinusSrcAlpha)` for every widget in the
//    window. Both readings are computed on the same half-covered texel: the
//    straight upload is proven to equal core/Blend's own `over`, and the
//    premultiplied upload is proven to differ and by how much.
//  - **RGBA16Float, with the 8-bit path run beside it.** Two linear samples
//    that round to the same byte are proven to stay distinct through f16, and
//    the maximum round-trip error of both paths is measured on the same
//    document and printed.
//  - **The upload buffer's layout**: row-major, top to bottom, four channels,
//    no padding, `w * h * 4` halves, the texel at (x, y) where it belongs --
//    and a blank document proven to be *bit-exactly zero everywhere*, which is
//    this step's regression boundary in numbers rather than in a screenshot.
//  - **The cache key.** Keyed on (id, revision, width, height); the collision
//    the obvious key would have had is demonstrated -- two different documents
//    are both at revision 0 -- and the documented way to defeat the cache
//    (writing tiles without `recordEdit()`) is asserted rather than left to be
//    discovered.
//  - **What the cache saves, measured**: the composite timed at two canvas
//    sizes against PRD F3's 20 ms pen-to-photon budget, and against the two integer
//    comparisons a cache hit costs, with the ratio printed.
//  - **The GPU round trip**, including the asymmetry that makes it a trap:
//    `wgpuQueueWriteTexture` accepts any stride while the readback direction
//    needs a 256-byte multiple, so a 61-texel-wide document (976 bytes/row) is
//    uploaded, read back through a padded staging buffer, and proven identical
//    to the CPU halves.
//  - **The visible consequences**: hiding a layer proven to give back the
//    blank buffer bit-exactly, and opacity, blend, mask, adjustment and
//    clipping each proven to change the uploaded bytes.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations.
// Needs the GPU (it uploads and reads back real textures); writes no files.
bool runDocumentTextureTest(GpuContext& gpu);

// PLAN.md Phase 5 step 14 ("Tabs + optional two-tab split, with the
// visible-documents GPU rule from ADR-0001's amendment"): PRD **A5** (P1) and
// PRD **A6** (P0).
//
// What is asserted:
//  - **The split as arithmetic.** Two panes and the 2 px rule between them
//    tile the canvas region exactly, at an even and an odd size, in both
//    arrangements; and a canvas too small for two usable panes collapses to
//    one rather than producing slivers.
//  - **Which document each pane shows.** The focused pane is the session's
//    active document -- the rule every menu and panel in the application
//    depends on -- and the repairs a session can force that a click cannot: a
//    closed companion, a companion that has become active, a split with one
//    document left in it.
//  - **PRD A6 in bytes.** Twenty open tabs, two visible: exactly two slots,
//    exactly two documents' worth of texture, and eighteen tabs holding
//    nothing. A third visible document re-points a slot and adds no bytes.
//  - **The rejected alternative, run beside the built one.** One
//    `DocumentTexture` -- what this module held before this step -- driven by
//    the same alternating pair, missing its key every frame; both upload
//    counts and both texel totals are printed.
//  - **That eviction is correct, not merely bounded**: a slot that has carried
//    three documents holds the third one's composite bit for bit, and a
//    document that comes back is recomposited whole rather than resumed from a
//    stale CPU mirror.
//  - **PLAN.md Phase 5's Verify sentence**, in measured bytes: what twenty
//    blank tabs cost in real resident memory, and that hidden documents hold
//    no texture at all.
//
// PRD **A8** ("a visible-but-unfocused document continues stepping its
// solver") is **not reachable in this build** and the section says so rather
// than asserting a proxy: `sim::PaintSim` is one process-wide canvas with no
// per-document instance to step. The reachable half -- that an unfocused
// document keeps its composite up to date -- is asserted.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations.
// Needs the GPU (PRD A6 is a claim about texture bytes); writes no files.
bool runDocumentResidencyTest(GpuContext& gpu);

// UI detour step 3, problem 2 ("five built features have no entry point"):
// app/LayerEditor, the one surface the `Layer` menu and the LAYERS panel
// buttons both go through, plus core/LayerOps' five new op-stack operations.
//
// What is asserted:
//  - **Coverage.** Every `LayerCommand` is in the list the menu walks and has
//    its own label -- scanned by casting integers, so a command added to the
//    enum and left out of the list fails here rather than shipping
//    unreachable, which is the exact failure this step exists to fix.
//  - **What each command does**: the kind and the storage shape of each of the
//    three creations (a Pigment layer with rgb tiles would be the same bug in
//    a different disguise), the insertion point, where the selection lands,
//    the mask's lifecycle, the clip and the three flags.
//  - **A fresh Adjustment layer changes no pixel**, and a reveal-all mask
//    composites byte-identically to no mask -- both in pixels through
//    core/Composite, not in flags.
//  - **An unavailable command always refuses.** A matrix of commands over four
//    documents, each unavailable pair attempted anyway. The converse is
//    deliberately not claimed: Delete is offered on a locked layer and refused
//    with a sentence, because a sentence explains and a greyed item does not.
//  - **The op stack**: an op arrives disabled and is proven to change nothing
//    until enabled, reorder/delete/params edits, the out-of-range op index
//    refused with a sentence rather than the `std::out_of_range` core::OpStack
//    throws, the lock, and an op edit taken back by undo.
//  - **The rule the panel rests on**: every successful command moves
//    `OpenDocument::revision` by exactly one and appends exactly one history
//    entry, and a refusal moves neither. ui/DocumentTexture caches the
//    composite by that number, so the trap is demonstrated beside it -- a raw
//    tile write leaves the revision where it was and never reaches the screen.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations.
// Headless and GPU-free; writes no files.
bool runLayerEditorTest();

// UI detour step 3, problems 1 and 1b: app/ControlsLayout -- the right-hand
// column's section order, its default-open set, and the label column.
//
// What is asserted:
//  - **The order rule**: every section that describes the open document comes
//    before every section that tunes the solver, and the document sections are
//    exactly the ones that start open. Written against roles rather than a
//    fixed list, so a new simulation section cannot bury LAYERS and still
//    pass. The old and new position of LAYERS and HISTORY are printed.
//  - **The label column's invariant**: the widget never starts before the
//    label ends, checked per label over the column's real label set and at
//    every point during the frame that grows it, plus order independence, the
//    no-shrink rule, and the narrow-panel case where the label takes its own
//    line instead.
//  - **The rejected alternative run beside the built one**: Dear ImGui's own
//    label-to-the-right-of-a-default-width-widget, whose remaining label space
//    is computed at both the old and the new panel width and shown to clip
//    several of these labels while the built scheme clips none.
//
// Pinned to a measured number rather than a guess about a font: the running
// application prints its label column whenever it changes, and 119 px for the
// 17-character "Capillary diffuse" is the 7.0 px/char this section derives
// every other width from.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations.
// Headless, GPU-free and ImGui-free; writes no files.
bool runControlsLayoutTest();

// app/ControlsColumnLayout -- the headless model behind a CONFIGURABLE
// right-hand controls column: which sections appear, in what order, and
// whether that survives a relaunch. Where app/ControlsLayout above asserts
// the *default* order and open set never change, this is the model that
// lets a user rearrange and hide sections away from that default and get it
// back the same way tomorrow. The ImGui affordance for doing so is a
// concurrent, separate change (src/ui/MacPaintUI.cpp); nothing here draws
// anything.
//
// What is asserted:
//  - **The invariant that matters most**: the sequence holds every
//    `ControlsSection` enumerator exactly once, across every mutator
//    (`moveTo()`/`moveUp()`/`moveDown()`/`setVisible()`/`resetToDefault()`),
//    every `parse()`, and every disk round trip -- including an out-of-range
//    move index (clamped, not refused) and hiding every section at once
//    (legal, not force-corrected).
//  - **Stable text keys, not ordinals**: every enumerator's persistence key
//    is unique, round-trips through `controlsSectionFromKey()`, and an
//    unrecognised string is rejected rather than aliasing some section.
//  - **The four round-trip repair rules, each in isolation**: an unknown
//    section name is ignored; a section this build's file predates is
//    APPENDED in `controlsSections()`'s own relative order rather than
//    vanishing; a duplicated section keeps its first occurrence and drops
//    the rest; and a malformed line -- or a file that is not this format at
//    all -- invalidates the WHOLE file rather than half-applying it,
//    falling back to `resetToDefault()`'s layout. An empty or missing file
//    resolves to the default through the identical "every section missing"
//    path as the append rule, not a separate special case.
//  - **A real save/load round trip**, entirely under `$NP_PANEL_LAYOUT` so
//    the developer's real `~/Library/Application Support/naturalPaint/` is
//    never touched, plus the same durability shape app/
//    selftest/UserBrushLibrary.cpp proves for its own file: a stale `.tmp`
//    left beside the real path is consumed by the next real save, and a
//    `.tmp` abandoned mid-write afterward cannot corrupt the real file.
//
// Headless, GPU-free and ImGui-free; writes files only under a
// `$NP_PANEL_LAYOUT`-redirected temp directory it removes before returning.
bool runControlsColumnLayoutTest();

// ---------------------------------------------------------------------------
// The incremental composite
// ---------------------------------------------------------------------------
//
// `OpenDocument::revision` says *that* a document changed and never *what*, so
// ui/DocumentTexture had two answers: the frame is free, or the whole canvas is
// recomposited -- 22 ms at 1024x1024 and 89 ms at 2048x2048, against PRD F3's
// 20 ms pen-to-photon budget and its second clause, "the in-progress stroke
// does not wait on a full document re-composite". core/DirtyTiles localises a
// change to a tile set, core/Composite recomposites exactly that set, and
// ui/DocumentTexture uploads exactly those sub-rectangles.
//
// Covers:
//  - **Localisation by copy-on-write slot identity.** A `Document` copy shares
//    every tile, so the barrier must copy on the next write and the slot
//    address must move; that is proved as arithmetic (use counts and pointer
//    equality), and its converse is proved too -- with no snapshot held, the
//    barrier writes in place and the address does *not* move, which is exactly
//    why holding one is what makes the set complete.
//  - **The one leak, demonstrated rather than described.** A write through a
//    handle taken *before* the snapshot mutates both copies, so no address
//    moves and the tile is missed. core/TileStore.hpp already states that
//    caller rule; here it is a fixture with a stale texel in it, beside the
//    supported ordering that does not leak.
//  - **The classification of what is NOT tile-local**, one named reason at a
//    time: visibility, opacity, blend, clip, op stack, mask presence, layer
//    count, kind, canvas size and working space each force a full recomposite,
//    while name, locked and parent are asserted to force *nothing* -- zero
//    tiles and a bit-identical composite. The one reorder that is tile-local
//    is proved by patching only its two dirty tiles into the previous
//    composite and comparing against a full recomposite.
//  - **Bit-identity of the region walk**, on a document carrying a mixed
//    Pigment pair, a masked and faded layer, a clipping run, a masked
//    adjustment layer and an unhonourable blend: composited whole, then one
//    tile at a time, and compared by `memcmp`. Also into a buffer at a tile's
//    own origin rather than the canvas's, and with the warnings proved
//    identical -- a cheap frame must not stop reporting an approximation.
//  - **Ten edits end to end** through one `DocumentTexture` and a real GPU
//    texture -- single-tile paint, multi-tile stroke, property change,
//    reorder, added and removed layer, mask edit, clipped run, adjustment
//    layer and an empty edit -- each compared against a fresh full composite
//    at zero tolerance, then read back off the GPU and compared again.
//  - **The cost and the crossover, measured**: the full recomposite, one tile,
//    a four-tile dab and eight tiles, fitted to a setup-plus-per-tile line,
//    with the crossover printed beside the fraction the policy actually uses.
//  - **A photographable sequence** on a 1024x1024 document, asserted
//    bit-identical at every step. It writes PNGs only when
//    `NP_INCREMENTAL_PNG_DIR` is set in the environment; a plain `--selftest`
//    run writes no files.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations.
// Needs the GPU (it uploads sub-rectangles and reads them back).
bool runIncrementalCompositeTest(GpuContext& gpu);

// PLAN.md Phase 5 step 10 / PRD C10 (P0), C11 (P1): core/Merge -- merge down,
// merge visible, stamp visible, flatten image, and rasterise a parametric
// layer.
//
// What is asserted:
//  - **The property that makes a merge trustworthy**: the document's composite
//    before and after, compared at a *derived* bound rather than a chosen one
//    (2^-11 relative plus 2^-25 absolute -- one f16 round trip, which is the
//    only error there is), with the measured maximum printed beside it. A
//    fixture whose composite is exactly representable in f16 is merged too and
//    asserted bit-exact, which is what shows the error is storage and not
//    arithmetic.
//  - **Every refusal, with its numbers**: the bottom layer, an out-of-range
//    index, a locked layer on either side, a hidden layer, a non-normal blend
//    on either side, an Adjustment layer, an inert kind, a group boundary, a
//    clip base whose member sits above the pair, and the one clip arrangement
//    of four that has no answer.
//  - **Pigment, decided rather than degraded**: two Pigment layers under `over`
//    are refused (a glaze has no latent), a `Mix` pair merges *in latent space*
//    and the result is asserted to still be mixable through the project's own
//    `blendModeAvailableForLayer()`. The rejected RGB fallback is built beside
//    it and shown to fail that same predicate.
//  - **One walk, not two**: merge visible is run over a fixture carrying a mix
//    pair, a clipping run and an adjustment layer at once, and the collapsed
//    single layer is asserted to reproduce the composite.
//  - **Stamp visible's honest invariant**: it is *not* appearance-preserving,
//    so what is asserted is that the stamped layer alone composites to what the
//    whole document did -- and the case where the full document then differs is
//    measured and shown to be warned about.
//  - **Undo**: a merge through `applyLayerCommand()` moves the revision exactly
//    once, appends exactly one history entry, and undoes to a bit-identical
//    composite.
//  - **The cost of reusing core/Composite**, printed: the sub-document's shared
//    tiles against what a deep copy would have cost, the canvas-sized
//    intermediate buffer, and the merged layer's own tiles.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations.
// Headless and GPU-free; writes no files.
bool runMergeFamilyTest();

// PLAN.md Phase 5 step 12 ("**Layer comps** -- named sets of visibility,
// position and properties, restorable in one click and **persisted in the
// document** as an `np:comps` blob on part 0"; PRD C14).
//
// Two corrections this section prints on every run rather than burying:
// **`np:comps` cannot be a blob** (docs/document-format.md's own measured
// warning -- array attributes are silently absent through this OpenImageIO,
// and io/NpaintFile refuses such a save by name, so a blob comp list would
// have made every save of a document with comps fail), and **position cannot
// be captured**, because `core::Layer` has no offset, origin or transform
// field of any kind and tiles are keyed by absolute document coordinates.
//
// What is asserted:
//  - **The four captured properties in pixels**: two comps of one document
//    proven to be two different composites, and each restore proven to give
//    its picture back byte-identically.
//  - **The four excluded properties proven excluded** -- mask, op stack, name
//    and lock each changed and then proven untouched by a restore, because an
//    exclusion is a promise that a click will not silently overwrite something.
//  - **The layer-set mismatch, with the rejected alternative run beside the
//    built one.** A comp captured over five layers, restored after a delete
//    and an add: keyed by `Layer::id` not one layer holds another layer's
//    state; the index-keyed restore an implementation with no identity would
//    have to use is implemented inside the test and its misapplication count
//    printed beside zero.
//  - **Every refusal with its numbers**: an out-of-range comp, two layers
//    sharing an id (the whole restore refused, nothing changed), a comp whose
//    layers are all gone, a comp of a document with no layers, and an
//    unreadable carried record.
//  - **The lock honoured rather than bypassed**: visibility restored on a
//    locked layer and opacity/blend/clip not, through core/LayerOps' own
//    setters so there is no second copy of the rule.
//  - **Restoring is an edit**: one revision, one history entry, and undo
//    proven to give back the pre-restore picture byte-identically; a refusal
//    proven to record nothing.
//  - **io/CompSerial**: a round trip, a payload typed out **by hand** and
//    decoded field by field, `npcomps2:` refused by name, and an unrecognised
//    comp record proven to survive whole and in position between two readable
//    ones.
//  - **Persistence**: comps, layer ids and the id counter round-trip through
//    `.npaint`; a restore after a reload applies in full and composites
//    identically to the same restore in memory; and a document whose comps are
//    cleared writes a file **byte-identical** to one written before any comp
//    existed, with the comparator proven non-vacuous.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations --
// the model, the panel and the carrier are pure, and the OFF build's answer for
// the file half is that `saveNpaint()` refuses by name, which is asserted
// rather than skipped (PLAN.md §1.5). Headless and GPU-free; writes and removes
// five `.npaint` files.
bool runLayerCompTest();

// PLAN.md Phase 5's C7/C12 follow-on ("layer grouping": PRD C7, P0). The
// model (`LayerKind::Group`, `Layer::groupTag`), core/LayerSetOps'
// `GroupLayers`/`UngroupLayers` span-splice and its order-preservation proof,
// core/Composite's pass-through fold (visibility and opacity reaching a
// group's children, and nested groups multiplying coverage), a hand-built
// cycle proven to terminate rather than hang, undo/redo through the same
// funnel every other set command uses, and the `.npaint` round trip --
// including what a build that does not recognise a group's `np:kind` does
// with one (carried verbatim and warned about by name, simulated with a
// synthetic future kind since there is no stale binary in this tree to run).
// Headless and GPU-free; writes and removes six `.npaint` files.
bool runLayerGroupTest();

// task/group-ui, the LAYERS panel's own half of PRD C7: docs/reachability-
// audit.md's C7 stayed open because nothing issued `GroupLayers`/
// `UngroupLayers` -- which turned out to already be false (ui/MacPaintUI.cpp's
// pre-existing generic `core::allLayerSetCommands()` walk picked both up the
// moment they joined that list; `--ui-multiselect-demo select:0.1,group`
// proves it). What was actually missing, and what this covers: a group row
// distinguishable from an ordinary one, its members indenting to read as
// inside it (nesting included, ordered immediate-parent-first), and the
// collapsed-row predicate a disclosure triangle drives -- hiding exactly a
// collapsed group's descendants, never the row that collapsed it, and never
// an unrelated sibling. Headless and GPU-free; app/LayerPanel's pure half,
// the same split that file already draws for every other row property.
bool runLayerGroupPanelTest();

// PLAN.md Phase 5 step 13 ("**Export comps to files, and layers to files** --
// one shared loop: set a document state, composite, write through phase 4's
// Export As presets with a name template"); PRD I16 and I17.
//
// One correction this section prints on every run: **the name template is PRD
// I17, not I18.** I18 is "revert, duplicate document, save a copy, save
// incremental, open recent" -- phase 4 step 8, already landed. I17 is the row
// whose text contains "with a name template".
//
// What is asserted:
//  - **The name template, hazard by hazard**: the three tokens, the ordinal's
//    zero padding, an extension derived from the *format* rather than from the
//    template, an absent `{doc}` rendering as nothing, and a refusal each for
//    an unknown token, an unbalanced brace, a path separator in the template's
//    literal text, a path separator in a substituted layer name,
//    `../../etc/passwd`, `..`, a colon, a leading dot, a control character, an
//    empty resolved name, and a 300-character name -- that last one refused
//    with **both numbers**, and 251 + ".png" = 255 accepted at the exact edge.
//  - **The plan writes nothing**, so a dialog can show the exact filenames a
//    click would produce before anything is committed.
//  - **Collisions refused before the first byte**, case-insensitively because
//    APFS and NTFS are, with both spellings quoted -- and the same pair
//    planning cleanly once `{index}` is in the template, so the refusal is
//    provably about the filename and not about the names.
//  - **PLAN.md's own verify sentence, literally**: four comps exported, four
//    files confirmed by name, by size against the reported byte count, by
//    being pairwise *different* bytes (which an exporter ignoring the
//    state-set would fail), by decoding back out of the PNG to the colours
//    each comp should hold, and -- the definition of "correct" a user can
//    check -- by being **byte-identical to clicking that comp in the panel and
//    using File > Export As**.
//  - **The document ends exactly as it started, structurally**: every entry
//    point takes a `const Document&`, so what is asserted is that the
//    composite bytes, the revision, the structural revision, the history entry
//    count, the unsaved-edit labels and the dirty flag are all unmoved after
//    four exports. **Zero history entries for four exports** -- a batch export
//    is not an edit and never reaches `recordLayerEdit()`.
//  - **Layers alone on transparency (PRD I16), with the rejected reading run
//    beside it**: an isolated layer proven byte-identical to hiding every
//    other, the "composited over what is beneath" alternative proven to be a
//    different file and costed at one line, an Adjustment layer refused by
//    name because isolating one composites to nothing, and an isolated
//    *clipped* layer un-clipped-and-warned -- with the alternative measured,
//    since leaving the clip on writes a fully transparent file.
//  - **Partial failure**: file 3 of 4 fails to write, and the run stops with 2
//    written, 1 failed by name, 1 not attempted and genuinely absent from the
//    disk -- PRD P4's own acceptance row ("files 13-40 untouched").
//  - **PRD P4's other half**: an existing output path refuses the whole batch
//    with the counts and leaves that file byte-for-byte untouched; explicit
//    overwrite proceeds.
//  - **It really is phase 4's presets**: an `ExportPreset` saved and
//    round-tripped through `ExportPresetStore`'s own serialiser, then assigned
//    into the batch request as one field, and its `FitWithin` resize proven to
//    reach the file (64x64 comps out at 32x32).
//  - **The measurement**, marked `[measured]`: the scratch copy, the shared
//    state-set, the composite-and-encode it feeds, one comp end to end and
//    four comps end to end.
//
// Runs -- and asserts the correct answers -- in BOTH NP_USE_OIIO
// configurations. A PNG batch is identical in each (PRD I1), and the seam is
// asserted rather than skipped: the same four-comp EXR batch writes four files
// in an ON build and is refused **before the first byte** in an OFF build,
// with `exportRequestAvailability()`'s string verbatim rather than four
// identical per-file failures. Headless and GPU-free; writes and removes a
// selftest_exportstates/ scratch directory.
bool runExportStatesTest();
// PLAN.md Phase 5 -- **the CPU Pigment deposit**: `brush/Deposit` (what one dab
// does to one texel) and `app/StrokeSession` (the stroke lifecycle around it).
// The project's oldest open blocker: before this step no stroke had ever
// reached a `Layer`, so PRD C3's `Mix` -- a P0 feature -- was asserted and
// never witnessed, and `core/History` had never seen a stroke.
//
// **What it is not**, stated first because it is the part a reader must not
// misread: this is the cheap interim, *not* the GPU->CPU solver readback
// designed in `scratchpad/design-stroke-bridge.md`. Nothing here simulates
// water. `sim::PaintSim::readbackCanvas()` is deliberately not reused (8-bit,
// display-referred, PRD B6). See brush/Deposit.hpp's opening section.
//
// What is asserted:
//  - **The falloff**, including the clause the whole footprint argument rests
//    on: coverage is *exactly* `0.0f` at and beyond the radius. The rejected
//    linear ramp is run beside the built smoothstep and both profiles' worst
//    slope discontinuity is printed.
//  - **What one dab does to one texel**, against the equation the header
//    states: the lerp form and the quotient form agree to a derived 4-ulp
//    bound, `m + dm == 0` returns the brush's latent (the limit, not a
//    convention), and mass saturates at 1 while the mixing weight does not --
//    with the rejected "cap the delta instead" run beside it, freezing a
//    full-mass texel exactly as predicted.
//  - **Hue idempotence at ZERO tolerance**: 65 deposits of one pigment leave
//    the latent bit-identical to the brush's, and two half-mass dabs split the
//    same way as one full one. Its stated limit is asserted too -- two
//    *different* pigments do not split, because Kubelka-Munk mixing is
//    order-dependent.
//  - **Latent space against RGB space**, both printed: blue into yellow gives
//    green in latents and a desaturated tan in RGB.
//  - **Mass IS alpha** (PRD F10): the composite's alpha at a deposited texel
//    is the stored mass and its RGB is `latentToRgb(latent) * mass`; 41
//    overlapping dabs leave nothing above mass 1.
//  - **Footprint completeness**, brute-forced rather than argued: for six dab
//    positions the reported tile set is exactly the set of tiles whose raw half
//    words changed, including a dab centred on the corner where four tiles
//    meet. The set is *tight* as well -- a dab that clips a tile corner without
//    reaching it allocates one tile where a bounding box would have allocated
//    four, and the 224 KiB-per-tile difference is printed.
//  - **One stroke is ONE undo step**: hundreds of dabs, exactly one
//    `HistoryEntry` labelled for the tool, the content revision moved and the
//    structural one not, and undo proven to return the tiles to a
//    **byte-identical** pre-stroke state by `memcmp` of the raw half words.
//  - **Every refusal**: out-of-range index, an RGB layer, a locked Pigment
//    layer and the Water tool each refuse by name and record nothing; a stroke
//    that deposited nothing records no entry at all; a held-still brush still
//    emits no dabs (ADR-0003).
//  - **The whole routing table** (which tools reach the CPU deposit and which
//    still reach `sim::PaintSim`), every row.
//  - **`Mix`, witnessed**: two hand-painted strokes on two Pigment layers,
//    composited under `Normal` and under `Mix`, with both colours printed --
//    the first time in this project's life that a stroke, rather than a
//    literal, produced PLAN.md's green.
//  - **Live feedback**: an in-progress stroke fed one sample per frame with a
//    document snapshot held across every frame (as ui/DocumentTexture holds
//    one), the per-frame deposit-plus-region-composite cost measured against
//    PRD F3's 20 ms **end-to-end** budget, and the region walk proven
//    bit-identical to the full one over the same tiles.
//  - **The measurement**: per-dab cost, a ~400-dab stroke, tiles touched
//    against tiles on the canvas, and the distinct bytes the stroke's history
//    entry costs under copy-on-write.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations --
// it reads no file at all, and `oiioBackendCompiledIn()` is checked so that
// claim is made twice rather than merely compiled twice. Headless and GPU-free;
// writes no files.
bool runPigmentDepositTest();
// **Painting on a plain RGB layer** (brush/RgbDeposit), and the routing fix
// that made it reachable (app/StrokeSession section 1; PRD E1 (P0) for the
// selection gate).
//
// **This section exists because of a wrong-target bug, not a missing feature.**
// `strokeRouteFor()` used to end "everything else keeps today's behaviour
// exactly, which is the solver canvas", one line below the row that refuses a
// locked layer *precisely so that paint never lands on the solver canvas when
// the user aimed at a layer*. So selecting an RGB layer -- the layer
// `Document::createBlank()` actually makes, and therefore the one an ordinary
// File > New selects -- and dragging the brush painted the dense canvas
// texture. Colour appeared, in the right place; nothing composited it, saved
// it or undid it. Section 10 below is the assertion that would have caught it.
//
// What this section proves:
//
//  - **Premultiplied storage, established rather than assumed**: white ink at
//    half alpha stores 0.5 and not 1.0, and `core/Composite` reads the
//    deposited texel through bit-identically -- so writer and reader hold one
//    convention rather than two that agree only at alpha 1.
//  - **Linear colour**: the tip's RGB is bit-identical to
//    `foregroundLinearRgba()` for every palette entry (the decode is written
//    twice, because `app/` may not include `ui/`), and the palette is checked
//    to really differ from its own decode so the assertion cannot pass against
//    a version that skipped the conversion.
//  - **The flow/opacity model, which is the one thing here that can be
//    *plausibly* wrong.** Flow is what one dab lays down; opacity is the
//    ceiling one STROKE can reach, held by a sparse per-stroke alpha
//    accumulator. 50 overlapping dabs at opacity 0.5 reach exactly 0.5 in the
//    accumulator and 0.5 within a derived f16 bound in the layer -- **with the
//    rejected per-dab model computed on the identical numbers beside it,
//    reaching 0.9999**, and asserted to be wrong so the good assertion cannot
//    pass against it. Once the ceiling is reached the remaining dabs write
//    nothing at all, and a 3 %-coverage rim texel reaches the same ceiling as
//    the 100 % centre.
//  - **Speed independence at zero tolerance**: 61 samples and 4 samples over
//    the same 180 px emit the identical dabs and leave **bit-identical** tiles.
//  - **The ceiling is per stroke**, asserted from the other side: a second
//    stroke at opacity 0.5 builds to 0.75, because the accumulator is thrown
//    away at pen-up.
//  - **The selection bounds the deposit, both ways** (PRD E1). It scales what
//    one dab lays down *and* caps what any number of dabs can reach; the second
//    was found by measurement rather than designed, since gating only the flow
//    lets a scrubbed stroke climb to full opacity anyway. The null-Selection
//    branch and the engaged-but-absent-tile case are both asserted through this
//    module's own hoisted loop, which core/SelectionMask.hpp requires.
//  - **The whole routing table**, including the four rows whose answer this
//    step changed, and the one row that is still `PaintSim` (no target at all).
//  - **Paint lands on the ACTIVE layer and on no other**: two RGB layers
//    painted over the identical pixels through `setActiveLayer()` and
//    `od.activeLayer`, each holding its own colour with the other's channel at
//    exactly zero.
//  - **Hiding a layer takes its paint with it**, through a real composite of a
//    real painted layer rather than by reading the flag back -- every float of
//    the frame exactly 0 -- and un-hiding restores the bit-identical composite.
//  - **Every refusal**: a locked RGB layer, an Adjustment layer, an
//    out-of-range index and the Water tool each refuse by name and record
//    nothing; a stroke off the canvas and a stroke at opacity 0 each record no
//    entry at all.
//  - **The accumulator's lifetime**, measured on both sides of pen-up: one
//    64 KiB float tile per touched tile while painting, zero afterwards.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations --
// it reads no file at all, and `oiioBackendCompiledIn()` is checked so that
// claim is made twice rather than merely compiled twice. Headless and GPU-free;
// writes no files.
bool runRgbDepositTest();
// **The eraser on a plain RGB layer** (brush/RgbErase), and the routing that
// made it exist at all (app/StrokeSession section 1; PRD F9 and F10, both
// **P0**; ADR-0007; PRD E1 (P0) for the selection bound).
//
// **This section exists because the tool did nothing.** Not "did the wrong
// thing" and not "was approximate": `Tool::Eraser` sat in `strokeRouteFor()`'s
// not-built list beside nineteen unimplemented palette cells, so a drag with it
// reached no layer, wrote no texel, produced no message and recorded nothing. It
// drew a cursor ring. Two P0 requirements described it in full and one ADR
// specified it per layer kind.
//
// What this section proves:
//
//  - **Destination-out on ALL FOUR channels.** `dst' = dst * (1 - e)`, one
//    factor for rgb and alpha alike, because core::Tile is premultiplied -- the
//    same all-four-channels argument `fillThroughSelection()` makes for the
//    bucket's feathered edge, checked here rather than assumed. A strength-0.5
//    dab halves every channel at **zero tolerance** (multiplying a normal
//    binary16 by 0.5 only decrements its exponent), and the un-premultiplied
//    colour is proven unchanged -- scaling the alpha alone would leave it
//    brighter by `1/(1-e)`, which is a fringe on exactly the soft edges an
//    eraser is used for.
//  - **Erasing to nothing leaves EXACTLY nothing**, all four channels at zero
//    and through a real composite, because a texel with colour at alpha 0 is
//    malformed: `core/Composite` reads it as an additive glow with no coverage,
//    nothing flags it, and it survives a save.
//  - **Strength is a per-stroke FLOOR, which is the one piece of arithmetic here
//    that can be *plausibly* wrong.** The accumulator holds the **fraction
//    removed**, not the alpha, so the floor is `alpha_0 * (1 - strength)` and one
//    stroke takes an opaque texel and an alpha-0.3 texel to the same *proportion*
//    of themselves -- both asserted, because an absolute floor would leave faint
//    paint untouched and would pass every assertion made only on an opaque texel.
//    50 overlapping dabs remove exactly the strength in the accumulator, at zero
//    tolerance, and land on the floor in the layer within a derived f16 bound --
//    **with the rejected per-dab model computed on the identical numbers beside
//    it, grinding the texel to under 1 % of itself**, and asserted to be wrong so
//    the good assertion cannot pass against it. Once the floor is reached the
//    remaining dabs write nothing at all, and a 3 %-coverage rim texel reaches
//    the same floor as the 100 % centre.
//  - **The floor is per stroke**, asserted from the other side: a second pass at
//    strength 0.5 takes the texel to 0.25, because an accumulator that survived
//    pen-up would be a tool that appeared to stop working after one drag.
//  - **The selection bounds the erase, both ways** (PRD E1): it scales what one
//    dab removes *and* caps what any number of dabs can remove, so a
//    half-selected texel cannot be scrubbed past its coverage. Asserted through
//    the module and again end to end through `StrokeSession`, with the texels
//    outside the ants **bit-identical** afterwards -- what a runaway eraser
//    destroys is invisible until the layer under it is, and one undo step covers
//    the whole stroke. The null-Selection branch and the engaged-but-absent-tile
//    case are both driven through this module's own hoisted loop, which
//    core/SelectionMask.hpp requires.
//  - **Speed independence at zero tolerance**: 61 samples and 4 samples over the
//    same straight 180 px emit the identical dabs and leave **bit-identical**
//    tiles -- with the straightness stated as a condition of the claim, since two
//    sample rates describe two slightly different Catmull-Rom curves.
//  - **Erasing nothing costs nothing**: a stroke of dozens of dabs across blank
//    canvas allocates **not one tile**, reports none, records no entry and moves
//    no revision -- while a *malformed* texel (colour at alpha 0) is erased
//    rather than skipped, because the skip tests all four channels.
//  - **The routing table's Eraser rows**, including the two that are decisions:
//    no layer at all is `None` for the eraser and `PaintSim` for the brush (the
//    solver has no alpha, so an eraser sent there would *add* pigment); and
//    Media, Adjustment and the storeless kinds each refuse rather than silently
//    doing nothing. The Pigment row is asserted here only for agreement -- it
//    used to be a refusal by name, on the single stated ground that the pigment
//    deposit had no PRD E1 gate, and it is now `StrokeRoute::PigmentErase`.
//    `runPigmentSelectionTest()` owns that row and the gate that unblocked it.
//  - **The erase lands on the ACTIVE layer and on no other**, driven through
//    `setActiveLayer()` and `od.activeLayer`, with the other layer asserted
//    bit-identical.
//  - **One stroke is ONE undo step, labelled "erase"** and not "brush stroke",
//    moving the content revision and not the structural one; and the
//    accumulator's lifetime measured on both sides of pen-up.
//  - **Every refusal by name**, with the locked and wrong-kind sentences
//    asserted *different* -- both present to a user as "the eraser did nothing",
//    and only one has a switch in LAYERS that fixes it.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations --
// it reads no file at all, and `oiioBackendCompiledIn()` is checked so that
// claim is made twice rather than merely compiled twice. Headless and GPU-free;
// writes no files.
bool runRgbEraseTest();
// **The active selection on a Pigment layer** (brush/Deposit §4; PRD E1, **P0**)
// and **the eraser that gate unblocked** (brush/PigmentErase; PRD F9/F10, both
// **P0**; ADR-0007's Pigment row).
//
// **This section exists because of a P0 that had never been implemented on one
// layer kind, not because of an approximation.** `brush/Deposit.hpp` did not
// contain the string "Selection": `depositDab(PigmentTileStore&, ...)` took no
// selection, so `app/StrokeSession`'s pigment branch could not pass one while
// the RGB branch on the line directly above it passed `doc_->selection`
// explicitly. Painting natural media on a Pigment layer -- the kind
// `Document::createBlank()` makes -- went straight through the marching ants.
//
// What this section proves:
//
//  - **A dab through an engaged selection deposits NOTHING outside it**, on a
//    texel the dab's own falloff covers fully and the selection does not,
//    asserted at **exactly zero** on mass *and* latent -- so the texel is
//    indistinguishable from paper rather than merely faint. This is the
//    assertion that fails against the code the section replaced.
//  - **The two nulls, which are opposites**, each driven through
//    `brush/Deposit`'s own hoisted loop as core/SelectionMask.hpp requires: a
//    **null `Selection*`** deposits everywhere in the footprint ("no
//    restriction"), and an **engaged selection naming no tile** at that
//    coordinate deposits nothing and allocates **no tiles at all**. A fix that
//    confuses the two fails in one direction or the other.
//  - **Coverage is proportional, not in-or-out** (PRD E2): one dab through a
//    partially selected texel lands exactly `flow * coverage`, within the single
//    binary16 rounding of the write that produced it.
//  - **`kMaxMass` was NOT already a bound, derived rather than assumed.** The RGB
//    route's argument for capping as well as gating rests on a per-stroke
//    accumulator this route does not have -- so the question was answered from
//    what `depositTexel()` does, and mass accumulates **linearly**: gating only
//    the rate reaches `kMaxMass` *exactly*, at the shipped defaults in **six
//    dabs** on a half-selected texel, which is 1.5 radii of travel. Both models
//    run on identical inputs and both numbers are printed; the built one reaches
//    `kMaxMass * coverage` at **zero** tolerance, because the cap writes that
//    float itself.
//  - **A deposit never REMOVES mass**: a texel already thicker than the
//    selection allows keeps every bit of what it had -- while its hue still
//    moves, because coverage bounds how much paint is present and not which
//    paint it is.
//  - **The gate costs the unselected document nothing**: a null selection and a
//    Select All deposit **bit-identical** tiles, so `--pigment-stroke-demo` and
//    the `canvas` golden reference are untouched by its existence.
//  - **The selection reaches the deposit through `StrokeSession`**, not only
//    through the free function -- with all 3731 pre-painted texels outside the
//    ants asserted over the whole rectangle rather than sampled, and the stroke
//    still exactly one history entry.
//  - **ADR-0007's Pigment eraser**: the per-stroke **floor**
//    `mass_0 * (1 - strength)`, asserted at **zero** tolerance (halving a normal
//    binary16 is exact) **with the rejected per-dab model computed on the
//    identical inputs beside it, grinding the same texel to under 1 % of the
//    floor**; a half-selected texel that cannot be scrubbed past its coverage,
//    with the un-capped model shown removing over 99 % of it; **the latent
//    bit-identical throughout** (PRD §7's own acceptance row: "Mass falls, Latent
//    unchanged"); an erase across blank canvas allocating **not one 224 KiB
//    tile** and recording nothing; and one stroke as one entry labelled "erase".
//  - **A Pigment texel emptied by the eraser is NOT malformed, by the rule this
//    storage actually has** -- the deliberate inverse of brush/RgbErase's. Mass 0
//    with a stale hue composites to **four exact zeros**, because the latent is
//    straight rather than premultiplied and `projectPigmentTexel()` multiplies;
//    and the next deposit onto it takes the brush's latent outright, which is
//    what `depositTexel()`'s §1(ii) was written for.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations --
// it reads no file at all, and `oiioBackendCompiledIn()` is checked so that
// claim is made twice rather than merely compiled twice. Headless and GPU-free;
// writes no files.
bool runPigmentSelectionTest();
// PLAN.md Phase 5 step 11 ("Multi-select, align and distribute, colour labels,
// linking, panel filtering"; PRD C12 (P0), C13 (P1), C15 (P2)).
//
// **Two of PRD C12's five verbs are refused, and this section prints both
// refusals on every run rather than leaving them in a header**: there is no
// geometric **transform** of a layer anywhere in this codebase (what this step
// built is an integer-pixel translate, the one case that needs no resampling),
// and there is no `LayerKind::Group`, so **group** would mean writing an
// `np:parent` naming a group nothing composites.
//
// **C13 is delivered in full**, which is the step's surprise: core/LayerComp.hpp
// refused a third of C14 one step earlier because a layer has no position, and
// that is still true -- but aligning needs a content bounding box and a
// translate, not a position, and both are built here (core/LayerGeometry).
//
// What is asserted:
//  - **The bounding box**, to the pixel, across a tile edge, off the canvas,
//    on a Pigment layer's mass channel rather than an RGB alpha, and empty for
//    a kind that holds no pixels.
//  - **The translate is lossless at ZERO tolerance**, on raw half words rather
//    than decoded values -- 19 881 texels compared -- because it moves `uint16`
//    words and has no decode step that could round. Its two paths are measured
//    beside each other: a whole-tile shift is a re-key that copies **0 bytes**,
//    a sub-tile shift gathers. The mask moves with the layer and what moves in
//    behind it reads 1.0 (reveal), not 0.0.
//  - **The delete ordering, with the bug run beside it.** {0,2,4} of five named
//    layers, descending; the ascending walk an implementation without the rule
//    would take is implemented in the test and the layers it destroys instead
//    are printed.
//  - **All-or-nothing**: one locked member refuses the whole set, and the
//    revision, the history length and every layer are proven unchanged. The
//    trial copy this is built on is measured against the deep copy it replaces.
//  - **One gesture, one edit**: three layers hidden move the revision once,
//    append one history entry, and come back on ONE undo.
//  - **Links**: symmetric by construction, geometry-propagating and nothing
//    else, and a group with one live member is not a link -- so a deleted
//    partner leaves nothing dangling and undo restores the link with it.
//  - **Align and distribute in pixels**, including a link group moving as one
//    unit, the half-pixel residual an integer translate cannot remove, and the
//    rounding reported rather than swallowed.
//  - **Colour labels and the filter**, including a label name this build has no
//    swatch for, and the rule that a command acts only on the rows a filter
//    lets the user see while the selection itself survives.
//  - **Persistence**: `np:label` and `np:link` round-trip, and a document with
//    neither writes a file **byte-identical** to one written before either
//    attribute existed.
//
// Runs, and asserts the correct answers, in BOTH NP_USE_OIIO configurations --
// only the `.npaint` round trip differs and the OFF build's refusal is
// asserted rather than skipped (PLAN.md §1.5). Headless and GPU-free; writes
// and removes three `.npaint` files.
bool runLayerMultiSelectTest();

// The Atelier chrome -- docs/ui.md section 1's twelve design tokens and
// section 2's dimensioned layout, checked against the document rather than
// against a screenshot, plus PRD L1's working-space label, L6's surround and
// L7's resident/budget pair. Headless and GPU-free; the layout is arithmetic
// and the tokens are integers, which is why both are free of ImGui.
bool runAtelierChromeTest();

// The active layer (`OpenDocument::activeLayer`) and `brushTipFor()` -- the two
// halves of the decision app/StrokeSession.hpp section 4 said the pen was
// waiting on. Headless and GPU-free. The *wiring* is covered by `--pen-demo`,
// which drags a synthetic pointer through the real UI, because a headless test
// that built its own session would prove the deposit works rather than proving
// something calls it.
bool runActiveLayerTest();

// **The paint bucket's refusals** (app/StrokeSession section 6), and the one
// question the Layer Properties dialog's un-dimming rests on.
//
// **This section exists because of a silence, not a miscalculation.**
// ops/FloodFill was correct and runFloodFillTest() passed on both sides of this
// step. What was wrong was one line of wiring: `ui/MacPaintUI.cpp` put its "can
// this layer take a fill" test *inside* the click condition, so a bucket click
// on anything but an unlocked RGB layer disappeared -- no fill, no history
// entry, no mark on the canvas and **no message anywhere in the chrome**. The
// ordinary route to it was to add a layer (Pigment: `CONTEXT.md`'s default kind
// and the NEW popup's first entry) and click. The brush had had a refusal for
// this case since the RGB route landed; the bucket and the gradient, sitting in
// the same palette group behind the same guard, never had one.
//
// What is asserted:
//
//  - **A success first**, so a fix that silenced the bucket everywhere cannot
//    pass the refusal sections: a click on a blank RGB layer moves every texel,
//    stores the ink premultiplied, reaches the far corner, and records exactly
//    one edit named "paint bucket" -- while a second click in the same colour
//    moves nothing and records nothing.
//  - **Three refusals that change nothing**: a Pigment layer, an Adjustment
//    layer and a locked RGB layer, each with a fillable RGB layer underneath so
//    "nothing was written" is a claim about the refusal. Zero texels, exact.
//  - **The refusal NAMES the layer** -- the assertion that fails against the
//    shipped behaviour, which produced no sentence at all -- and **locked is
//    told apart from no-RGB-store**, in the half of the sentence that names the
//    fix, for two layers deliberately given the same name so the difference
//    cannot be the name.
//  - **The lying indicator.** `strokeRouteFor()` answers `None` for the bucket
//    on every layer, correctly, because it begins no stroke -- so the options
//    bar, the one place in the chrome that says what a tool will do to a layer,
//    read a grey "-> none" over a layer the bucket was about to fill. Both
//    tables are asserted at the same moment on the same layer.
//  - **PRD E1 (P0), the selection as a bound**: the flood covers a blank layer
//    entirely and the intersection cuts it to the rectangle, with the texels one
//    past each edge exactly {0,0,0,0} in an *allocated* tile -- so the
//    assertion cannot pass on a tile that merely does not exist.
//  - **The gradient refused by the same predicate**, since fixing one of the
//    two would be half a fix.
//  - **The canvas re-composites while an op stack is edited** -- the claim the
//    Layer Properties dialog's undimmed modal depends on, asserted on both
//    mechanisms it rests on: the revision `ui/DocumentTexture` caches on moves,
//    and `documentDirtyTiles()` classifies the change as `LayerOpsChanged`
//    rather than finding the empty tile set an op edit would otherwise produce.
//    The recomposited halves are compared bit for bit, and disabling the op
//    returns the bit-identical original picture.
//
// Headless and GPU-free; writes no files. The dim suppression itself is ImGui
// state inside a window this suite has none of, and is stated as untestable
// here rather than given a test that asserts something adjacent to it.
bool runBucketRefusalTest();

// The mouse pointer, and whether it describes what the next click will actually
// do (ui/ToolCursor).
//
// **Written for an absence.** Before it there was not one cursor call anywhere
// in `src/` -- no `ImGui::SetMouseCursor`, no `SDL_SetCursor`, no
// `SDL_CreateCursor` -- so the OS arrow sat over the canvas identically whether
// the next press would deposit pigment, drag a marquee, sample a colour, pan
// the view, or be refused outright and do nothing at all.
//
// What is asserted:
//
//  - **The table is total and nobody phoned in an arm.** Every one of the
//    twenty-eight `Tool` values answers a real intent. `-Wswitch` already
//    forces exhaustiveness -- `cursorForTool()` has no `default:`, the same
//    guard `strokeRouteFor()` spells its twenty-tool list out for -- so what is
//    added on top of the compiler is that **no tool answers `Arrow`**: that
//    value belongs to `Tool::Count`, which is the enum's bound and not a tool,
//    so a tool wearing it is an arm added to satisfy the compiler without a
//    decision behind it.
//  - **Non-degenerate, in both directions**: paint, select, sample, pan and
//    zoom are five different intents, *and* the members of each family agree
//    with one another -- a table handing every tool its own unique answer would
//    pass the first check and still be wrong.
//  - **The shapes, and the one collision left.** Dear ImGui's cursor set
//    contains **no crosshair** -- Arrow, TextInput, four Resize shapes, Hand,
//    Wait, Progress, NotAllowed is the whole enum, and its SDL3 backend builds
//    exactly one system cursor per value, so `SDL_SYSTEM_CURSOR_CROSSHAIR`
//    cannot be reached through `ImGui::SetMouseCursor()` at all. Routed that
//    way the brush, the four selection tools, the eyedropper and the bucket all
//    became one plain arrow. This build therefore suppresses the backend
//    (`ImGuiConfigFlags_NoMouseCursorChange`) and drives SDL system cursors
//    itself, so paint, select and sample are asserted **distinct** and each is
//    pinned to the shape ui/ToolCursor §3 documents -- a re-map has to change
//    the documented table too. Pan and MoveObject share SDL's single
//    four-pointed arrow, asserted as an *equality* with its reasoning, since it
//    is a fair collision rather than a forced one; a count of distinct shapes
//    catches a second pair collapsing unnoticed.
//  - **What suppressing the backend costs, paid back.** Nothing else will apply
//    ImGui's own cursors any more, so all eleven `ImGuiMouseCursor_` values are
//    asserted against the SDL shapes the backend used (imgui_impl_sdl3.cpp
//    624-634), plus the `None` sentinel, which is -1 and a hide request rather
//    than a shape. A wrong entry there is invisible on the canvas and shows
//    only as a panel or text box behaving oddly.
//  - **The valuable half: the cursor describes the OUTCOME, not the
//    selection.** Nine refusals -- the brush on a locked layer of either
//    paintable kind and on an Adjustment layer, the bucket and the gradient on
//    a Pigment layer, and the fills with no document at all -- each show the
//    slashed circle. These are the same gestures `app/StrokeSession` §§1 and 6
//    already refuse with a sentence in the options bar; the point is that the
//    options bar is a different band and the user is looking at the canvas.
//  - **And the negative case, so that cannot pass vacuously**: a
//    `toolCursorOnTarget()` hard-wired to "not allowed" satisfies every refusal
//    above and dies on the successes -- the brush on both paintable kinds, the
//    fills on an RGB layer, and **a stroke with no document at all**, which
//    routes to the solver's dense canvas texture and is a real destination
//    rather than a refusal. Plus the over-eager-refusal rows: a lock and an
//    unwritable kind must stop tools that *write* without stopping the
//    eyedropper, the selection tools or the Hand.
//  - **The unbuilt palette cells answer "not allowed"**, checked against a
//    perfectly writable RGB layer so the claim is about the tool. That is the
//    harsh choice of the two available and ui/ToolCursor.hpp §5 argues it,
//    including the case against; the check that it has not spilled -- every
//    *built* tool usable over that same layer -- sits on the next line.
//
// Headless and GPU-free: no window, no ImGui context, no SDL video and no
// document. Writes no files.
//
// **Two things it cannot cover, stated rather than approximated.** The wiring
// in `ui/MacPaintUI.cpp`'s canvas block -- that the request is scoped to the
// canvas hit rect, cleared every frame, and that a guide drag, a pan and a view
// rotation beat the tool -- is ImGui state inside a window this suite has none
// of. And `SystemCursorTable` itself calls into the platform through SDL's
// video subsystem: its hide/show, null-entry fallback and once-per-frame apply
// are argued in ui/ToolCursor.hpp §6 line by line against the backend function
// they replace, but nothing here executes them. Both are given no test rather
// than a test that asserts something adjacent to them.
bool runToolCursorTest();

// The presentation transfer function: what the value in a layer becomes by the
// time it is a byte in a screenshot. Establishes the surface format the adapter
// actually preferred and the gamma Dear ImGui's backend selects from it, then
// authors known linear values into a layer and runs them through the real
// upload, the real surface format, the real hardware sRGB encode and the real
// app/Screenshot writer, decodes the PNG's raw bytes and compares against
// color/Space's srgbEncode(). The same fixture is rendered at gamma 1.0 beside
// it, which is what isolates the measured error to that one uniform. Needs the
// GPU. Runs, and asserts the correct answers, in BOTH NP_USE_OIIO
// configurations; writes and removes two `selftest_present_*.png` files.
bool runPresentTransferTest(GpuContext& gpu);

// Headless, GPU-free check on io/Descriptor -- the Photoshop Action Descriptor
// reader (PLAN.md "12 -- Import brushes", first bullet; PRD G7, G9). No file
// I/O: every fixture is bytes built in the section itself.
//
// **This section's centre of gravity is the adversarial half, not the happy
// one.** io/Descriptor parses files this project did not write and cannot
// trust -- an `.abr` from a marketplace or a forum -- so the property that
// matters is that no input causes a read outside the caller's buffer. A test
// handing the parser a `std::vector` cannot check that: malloc's slack absorbs
// an overread and the test passes anyway. So **every parse in this section,
// good fixture and corrupt alike, runs out of a mapping whose last byte abuts
// an `mprotect(PROT_NONE)` page**, and a single byte of overread is a SIGSEGV
// at the instruction that did it, in the ordinary RelWithDebInfo build, with no
// sanitizer. The guarded-parse count is printed, so a change that quietly
// stopped exercising the guard shows up as a number rather than as a silent
// `pass`. Identical in both NP_USE_OIIO configurations -- nothing here touches
// OpenImageIO (PLAN.md 1.5).
//
// Covered, in order:
//
//  - Every type this build parses -- `Objc`, `GlbO`, `VlLs`, `UntF`, `doub`,
//    `TEXT`, `enum`, `long`, `comp`, `bool`, `type`, `alis` and `tdta` -- each
//    read back to its exact value, doubles included, since they are bit
//    patterns rather than conversions. Ten of them sit in one flat descriptor
//    whose whole tree is compared against a twelve-line expected dump, so a
//    framing regression shows as a diff instead of as one wrong accessor.
//  - **The Key quirk**, which is where third-party readers break: a length of
//    zero means exactly four characters, any other length means itself. All
//    four cases -- the zero form, an explicit length of *4* (legal, and what a
//    reader that special-cases the number 4 gets wrong), a 14-character key and
//    a 1-character one -- in one descriptor, with the item after each checked
//    to still be aligned.
//  - A nested `Objc` inside a `VlLs` inside the root, read through two levels,
//    with the list element after the nested descriptor proven still aligned and
//    `path()` reporting `lst /1/useTipDynamics`.
//  - The typed reads coerce nothing: `asDouble()` on a `long` is absent, not
//    3.0. An invalid cursor is usable, propagates through `field()` and reads
//    as absent, so a walk into a file that lacks the key is an expression
//    rather than a null-check ladder.
//  - Unpaired UTF-16 surrogates repaired to U+FFFD with **one warning per
//    string, not per code unit**, each naming the item and a byte offset --
//    the shape PRD G9's import report is assembled from. The rest of the
//    string survives, because a brush preset with one bad code unit in its name
//    is still a brush preset.
//  - **Every proper prefix of all three good fixtures**, each in its own
//    guarded mapping. Zero may be accepted, and every one must be refused with
//    this module's own named sentence.
//  - Hostile field values, each the good fixture with one number replaced:
//    item count 2^32-1, list count 2^32-1, `tdta` length 2^32-1, a
//    UnicodeString length of 2^30 code units, a key length of 2^32-1 and an
//    oversized classId length. Each refused before anything is reserved.
//  - `obj `, `ObAr` and an unprintable four-byte type: refused **by name**,
//    naming the item they sat under, with the escaped bytes rather than raw
//    ones. An Action Descriptor puts no length in front of a value, so an
//    unrecognised type genuinely cannot be stepped over -- the refusal says so,
//    and that sentence is asserted, because it is the argument for refusing.
//  - Both sides of the depth cap: 60 nested descriptors parse, 4000 are refused
//    naming the level and the option. Plus `maxNodes` refusing in this module's
//    words rather than the allocator's, and `maxDepth = 0` reported as a caller
//    error rather than blamed on the file.
bool runDescriptorTest();

// Closing a document that holds unsaved work (app/CloseDecision) -- PRD I11's
// refusal turned into a Save / Don't Save / Cancel question, and the one
// decision point the tab strip's close box and File > Close Document both go
// through.
//
// **The defect this section was written against was invisible to the suite.**
// Both close paths called `DocumentSession::close(..., discard = false)` and
// dropped the refusal into a line of dim grey beside the menus, so a tab with
// unsaved work read as a dead control -- which is worse than either saving or
// discarding, because it teaches the user the button is broken. The refusal
// itself was correct and is still asserted by runDocumentLifecycleTest(); what
// nothing asserted was what the *user* gets afterwards. (The close box was in
// fact unreachable for a second, independent reason -- an ImGui hit-test one
// that no headless test can see. ui/AtelierChrome.cpp's tab loop carries it.)
//
// Covered, in order:
//
//  - A clean document closes on the click, with no question raised at all, and
//    closing the LAST one leaves the empty session the application already had
//    rather than inventing a blank document or a quit.
//  - A dirty document raises the question and is **still open** afterwards,
//    unchanged down to its unsaved-edit labels; the question names the
//    document and the work in PRD I11's own `unsavedWorkSummary()` words; and
//    a second question is refused while the first is up.
//  - Cancel leaves the count, the dirty flag, the labels and the active index
//    exactly as they were.
//  - Don't Save closes exactly one document, and it is the one asked about.
//  - Save calls the writer exactly once and only then closes; a **failed**
//    save closes nothing and leaves the question up carrying the writer's own
//    error; a never-saved document asks for a destination and writes nothing;
//    and finishing that hand-off does not write the same bytes twice. The
//    production saver is proven to be `saveDocument()` through the one refusal
//    only that function produces.
//  - **The pending close is keyed on identity, not on an index.** The stale
//    index is arranged to be in range and to name a *different* document, so
//    an index-keyed implementation does not fail -- it succeeds at discarding
//    the wrong document. The same hazard's other half: a question about a
//    document something else has closed acts on nothing and says so.
//  - Escape maps to Cancel and Enter to Save, and **no** key maps to Don't
//    Save, asserted by exhaustion over the key enum.
//
// Headless, GPU-free, and writes no files: the save is injected, so every
// assertion holds in BOTH NP_USE_OIIO configurations rather than the section
// going quiet in the OFF build.
bool runCloseDecisionTest();

// Getting an image *into* the open document (app/ImportImage), and getting out
// of the application without losing what is in it (app/QuitSequence).
//
// **Both halves are the same defect twice, at opposite ends of the
// application.** `placeImageAsLayer()` was written, tested and correct, and had
// no caller in the binary outside this suite -- a finished feature that was
// nonetheless absent. Quitting set `AppState::quit` from four places and
// consulted no document at all, so `DocumentSession::close()` was never called
// on the way out and PRD I11's protection, along with the whole Save / Don't
// Save / Cancel question, was bypassed by the one exit every user takes. A
// session with three painted, unsaved documents closed on one keystroke and
// said nothing -- and because a clean shutdown removes the recovery scratch
// directory (PRD O8), the same exit deleted the journal's copy of the work.
//
// Covered, in order:
//
//  - An import adds **exactly one** layer, RGB-kind with its `rgbTiles`
//    engaged and holding pixels, at the top of the stack, and it becomes the
//    active layer. Exactly one history entry, labelled with the file's own
//    name. With three layers already open it still lands on top rather than
//    above the active one, and the active layer follows it.
//  - Five refusals -- a missing file, a file no decoder accepts, an empty
//    file, a folder, an empty path -- each adding **no** layer, recording
//    **no** history entry, bumping no revision and naming the file in its
//    message. Asserted together, because an implementation that returned
//    failure *after* appending the layer would pass a returns-false check.
//  - An image larger than the canvas: neither cropped nor resampled, warned
//    about by name with both sizes, and the tiles it really occupies counted
//    so the invisible-pixel cost is pinned rather than described.
//  - A quit with nothing unsaved exits **on the call**, storing nothing, and
//    the clean documents are not closed one at a time on the way out.
//  - Three documents of which two are dirty: exactly two questions, in session
//    order, and the clean one is never asked about and is still open when the
//    process exits. A quit raised while another close question is up is
//    refused by name and does not exit.
//  - Cancel on the second of three questions abandons the whole quit, the
//    third document is never asked about, the application does **not** exit,
//    and the unanswered documents are still open and still dirty. A document
//    already answered with Don't Save stays closed -- the cancel stops the
//    quit, it does not un-answer an answered question.
//  - A **failed** save abandons the quit rather than marching on to the next
//    document, closes nothing, leaves that document open and dirty, and leaves
//    the question up carrying the writer's own error. The successful
//    counterpart asserts the writer really ran, so a silenced saver cannot pass
//    the failure case.
//  - **Every question is keyed on `DocumentId`.** A document is closed
//    underneath a running sequence and the fixture is arranged so an
//    index-keyed queue would move to a *clean* document and let a dirty one
//    leave unasked -- it would not fail, it would succeed at the wrong thing.
//    Plus the other half: a question about a document that has gone acts on
//    nothing and moves on rather than discarding the bystander that inherited
//    its index.
//  - Escape during a quit cancels it and destroys nothing, and no key maps to
//    Don't Save on the quit path either.
//
// Headless and GPU-free. The save is injected, so the quit half holds in BOTH
// NP_USE_OIIO configurations; the import half writes PNG fixtures into a
// scratch directory of its own, which it removes, because "a path that does not
// exist is refused by name" cannot be asserted without a filesystem.
bool runQuitGuardTest();

// **docs/reachability-audit.md, items D1, D2, D4 and A4** -- four small gaps
// in what the menu bar and the command line actually reach, verified beside
// (and deliberately not duplicating) `app/selftest/MenuModel.cpp`'s general
// coverage of the menu model itself.
//
// What is asserted:
//
//  - **D1, undo/redo exist and agree.** `MenuAction::Undo`/`Redo` are named,
//    claim ⌘Z / ⇧⌘Z, and those chords resolve in the real
//    `keymaps/default.json` to `"undo"`/`"redo"` -- the pair `--selftest`
//    could not check at all before this step, because neither the
//    enumerator nor the keymap action existed.
//  - **D1, one implementation.** `ui/MacPaintUI.hpp`'s `moveHistoryCursor()`
//    -- the function the HISTORY panel's buttons and the title bar's already
//    call -- actually moves a constructed `History`'s cursor both
//    directions, and `performMenuAction()` for `Undo`/`Redo` sets the exact
//    `AppState::requestUndo`/`requestRedo` flags the keymap dispatch in
//    main.cpp sets for the same two chords (asserted on that observable
//    flag, not on which function ran).
//  - **D2, the nine are all present and none is miswired.** Every one of
//    Cut, Copy, Copy Merged, Paste, Delete, Select All, Deselect, Reselect
//    and Invert Selection appears in the Edit menu exactly once, and each
//    one's claimed chord resolves in `keymaps/default.json` to the specific
//    action name `main.cpp`'s dispatch already uses for that keystroke --
//    the assertion that reddens if, say, Copy Merged were wired to plain
//    Copy.
//  - **D2, the enable predicates.** Paste disabled against an empty
//    clipboard and enabled against a full one; Cut and Delete disabled on a
//    locked layer; Deselect and Invert Selection disabled with no selection;
//    Reselect disabled with nothing to restore -- each a constructed
//    `MenuContext`, no window, no document.
//  - **A4, the Goodies menu obeys the palette's own rule.**
//    `ui/MacPaintUI.hpp`'s `toolMenuFamily()` -- what `menuContextFromState()`
//    assigns to `MenuContext::tools` -- enables exactly the tools
//    `toolImplemented()` says are built, counted against that predicate
//    rather than a literal number, and a disabled entry's tooltip is the
//    identical "Not built yet." sentence the palette shows.
//  - **D4, positional routing is a pure predicate.** `app/OpenAnyFile.hpp`'s
//    `looksLikePositionalArgument()` -- the classification main.cpp's flag
//    loop now calls instead of carrying its own copy -- accepts a bare
//    filename and refuses every string this build already recognises as a
//    flag.
//
// Headless throughout: every `MenuContext` and every `OpenDocument` here is
// hand-built, `moveHistoryCursor()` is called with `sim` left null (the idle
// state ADR-0001 already assumes, which settles nothing), and nothing reads
// or writes the user's real recent-documents file --
// `menuContextFromState()` itself is deliberately NOT called for that reason;
// `toolMenuFamily()` is the piece of it this section needs.
bool runMenuBasicsTest();

// **ui/MenuModel -- what the menus ARE, separated from how they are drawn.**
//
// The menu bar used to be forty-one `ImGui::MenuItem()` call sites, each of
// which declared an item and performed its action in the body of an `if`, and
// nothing but Dear ImGui mid-frame could read any of them. A native `NSMenu` is
// built once, out of band, and calls back with no `if` to be the body of -- so
// the honest choices were "extract the model" or "write every action out a
// second time in Objective-C". This section is what makes the first choice
// checkable, and it is entirely headless: no window, no GPU, no ImGui context
// and no `NSApplication` anywhere near it.
//
// What is asserted:
//
//  - **The ids.** Every action has exactly one spec, every spec carries its own
//    id (a row that disagrees with its index wires one item to another's
//    behaviour, invisibly), and the count is pinned at 41 -- one per call site
//    the extraction replaced.
//  - **Reachability.** Every action appears in the tree, only the six family
//    actions ever carry a param, and no pickable item carries `None`.
//  - **The predicates**, as pure functions of a constructed state: Open Recent
//    disabled on an empty list and enabled with one entry, Save needing a path
//    where Save As... needs only a document, the check marks tracking their own
//    flags, and `buildMenuModel()` proven to give the same tree twice.
//  - **The quit**, at length, because this is the one part of the menu where
//    getting it wrong costs the user their work. Quit's declared effect is
//    `QuitRequest` and it is the only action with it; **performing it sets
//    `AppState::requestQuit` and leaves `AppState::quit` alone**, which is the
//    assertion a backend wired to `[NSApp terminate:]` could not pass; no item
//    may use a system selector; and suppressing File > Quit under a native
//    application menu removes exactly one item and no others.
//  - **The modals.** All nine are pinned as deferred-to-the-next-frame, because
//    "inline" for these means `ImGui::OpenPopup()` from an AppKit callback with
//    no frame in progress -- and the plain state flips are pinned as *not*
//    deferred, so the marking has not simply been applied to everything.
//  - **The key equivalents**, cross-checked against the real
//    keymaps/default.json. A native menu item does not display a chord, it
//    **consumes** it before SDL ever sees the key, so a chord whose menu action
//    differs from its keymap action is a documented key that quietly does
//    something else. Also: no two items claim one chord, every claimed chord
//    carries Command (a bare letter would be swallowed inside text fields), and
//    the four display-only chords are pinned in both directions.
//  - **The published snapshot**, which is the seam the AppKit backend reads:
//    a stale id is not pickable, and the shape generation moves for a document
//    opening but not for a check mark flipping.
bool runMenuModelTest();

// The Select menu (docs/reachability-audit.md C5; PRD E4/E8/E9). Headless --
// no window, no GPU, no ImGui context.
//
// Five engines -- grow, shrink (PRD E8's distance transform), feather (a
// blur of coverage), colour range and luminance range (PRD E9's whole-layer
// predicate passes) -- were correct and tested (app/selftest/
// SelectionRefine.cpp, app/selftest/Blur.cpp) with no caller outside those
// two sections, because there was no Select menu to put them behind. This
// section does not restate that those engines are correct; it proves the
// menu built on top of them actually reaches them.
//
// **What is asserted, and why it is not the same claim as the engine
// section's:** that `MenuAction::SelectGrow` reaches `growSelection()` and
// not `shrinkSelection()`; that a dialog's own radius (or swatch, tolerance,
// band) is what the engine receives rather than a struct's default that
// happened to compile; that growing by N then shrinking by N is a
// morphological CLOSING, not an identity -- extensive, so a gap narrower
// than the radius stays bridged once bridged, which core/SelectionRefine.hpp
// says in words and this section demonstrates on a texel that starts at
// coverage 0.0 and stays above it; that a null TILE inside an engaged
// Selection reads as coverage 0.0 at a grow's boundary, never as the
// OPPOSITE convention a null `Selection*` uses (1.0, "no restriction"); that
// the three enable predicates are pure functions of a hand-built
// `OpenDocument`; and that each of the five operations -- which changes only
// a Selection, never a pixel -- pushes exactly one entry onto a dedicated
// undo stack (`OpenDocument::refineUndoStack`, deliberately NOT
// `core::History`, whose entries hold nothing but a `core::Document`) and
// that undoing restores the previous mask exactly, `std::nullopt` included.
bool runSelectMenuTest();

// **File > Open accepts any file this build can read, and decides which reader
// gets it from the file's bytes** (app/OpenAnyFile, io/FileKind), plus the
// drag-and-drop gesture that had no handler at all.
//
// Three absences, one shape. `File > Open...` called `openNpaintDocument()` and
// nothing else, so it opened `.npaint` and refused every picture on the user's
// disk -- while io/ImageIO's `openImageAsDocument()`, whose own header had said
// for two phases that it was "the function a future File > Open would call",
// had no caller in the binary outside this suite. And there was no
// `SDL_EVENT_DROP_FILE` handler anywhere in `src/`, despite io/Export.hpp
// describing `placeImageAsLayer()` as written "for step 13's drag-and-drop".
//
// **What identifies a `.npaint` on disk.** There is no naturalPaint magic
// number: the container is OpenEXR's, because a `.npaint` *is* a multi-part
// tiled EXR (PRD I8 -- "`.exr` is the same container under a different name").
// What makes one ours is an attribute named `np:version` in part 0's header,
// which `saveNpaint()` stamps on every file it writes. io/FileKind walks the
// EXR header structure to find it, and section B ties that spec-derived walk to
// reality by saving a real document and asserting the walk recognises it.
//
// Covered, in order:
//
//  - Every leading signature io/FileKind knows, from literal bytes, plus TGA
//    from its trailing footer -- and six malformed or truncated EXR headers,
//    including one with a 0xFFFFFFFF attribute size, all answering "not one of
//    ours" rather than reading past the buffer. Plus the false positive the
//    attribute walk exists to avoid: the string `np:version` sitting inside an
//    attribute's *value*, which a byte search of the header would fall for.
//  - A real `.npaint` written by `saveNpaint()` sniffs as a document, and a
//    plain EXR written by io/Export sniffs as a picture.
//  - **Dispatch is by content.** A PNG named `.npaint` opens as a picture, and
//    a file carrying `np:version` named `.png` is routed to the document
//    reader -- the two assertions an extension-based fix fails and every other
//    one here passes.
//  - The three refusals told apart by their sentences: "we do not read this",
//    "this build has no reader for that format", and "your file is damaged",
//    the last being the only one a user can act on. Plus the four that come
//    first -- empty path, missing file, folder, zero bytes -- each naming the
//    file, and none of them leaving a half-built document behind.
//  - **What an opened picture is bound to: nothing.** Its `path` is empty and
//    its `title` is the picture's own name, so `saveDocument()` refuses by name
//    and points at Save As. Asserted by *calling* Save and then comparing the
//    picture on disk byte for byte with what it was -- `saveNpaint()` writes
//    EXR bytes whatever the path is called, so a document bound to `photo.png`
//    would have its next Cmd-S destroy the user's photograph. It is dirty from
//    birth, so a close asks about it; its one layer is RGB-kind with tiles
//    engaged and occupies exactly the tiles the image spans.
//  - **Import already accepts every decodable format**, and this proves it
//    rather than changing it: the section walks `allFormatCapabilities()`, the
//    live runtime query, encodes an in-memory fixture for every format this
//    build can both read and write, and asserts each one imports and adds
//    exactly one layer. Nothing is read from the repository. PSD and camera raw
//    are read-only and cannot be reached this way -- named rather than skipped
//    silently.
//  - The drop routing rule as a **pure function** of (file kind, is a document
//    open?), so it is asserted without a window: a `.npaint` always opens,
//    never imports, because importing one would flatten its whole stack through
//    its composite; a picture opens when nothing is open and becomes a layer
//    when something is.
//  - **Twelve files dropped at once.** Onto an empty session: one document and
//    eleven layers in it, not twelve tabs and not one file used and eleven
//    dropped. Onto an open document: twelve layers. A mixed drop resolves in
//    order and names the file it refused. Twenty unreadable files count all
//    twenty, name eight, and say how many were not named.
//
// Headless and GPU-free. All of it holds in BOTH NP_USE_OIIO configurations
// except the two halves that need a `.npaint` writer, which are gated on
// `oiioBackendCompiledIn()` and print a skip line rather than going quiet. It
// writes files into a scratch directory of its own, which it removes, because
// "a refused Save left the picture untouched" is a claim about a filesystem.
bool runOpenAnyFileTest();

// **The Filter and Image menus, and app/FilterOps -- the bridge that makes
// docs/reachability-audit.md's C1 stop being true for six of the ~93 entries
// it names.** `runBlurTest()` and `runFiltersTest()` already prove the
// engines; this section proves the WIRING: that a menu click reaches its own
// engine call with its own dialog's parameters, that the active selection
// bounds a Filter-menu pixel op the identical way it already bounds the
// paint bucket (`runBucketRefusalTest()`), that each op is exactly one
// `core::History` entry whose undo is exact, and that a layer kind
// `PixelOpRefusal` cannot touch refuses by name and changes nothing. Nothing
// here restates ops/Blur.hpp's or ops/Filters.hpp's own arithmetic claims.
//
// Covered, in order:
//
//  - **Reach, and that it is not shared.** Gaussian Blur, Sharpen and Unsharp
//    Mask all end up inside `unsharpMaskTiles()` (Sharpen legitimately, by
//    ops/Filters.hpp's own section 3 -- it *is* that call with the radius
//    fixed), so the assertion that matters is not "which function ran" but
//    "with which parameters": Sharpen's result is pinned to
//    `kSharpenSigma` regardless of what Unsharp Mask's own radius field
//    holds, and Unsharp Mask's radius is pinned to the dialog's field
//    rather than to Sharpen's fixed one -- the pair a sabotage that dropped
//    either parameter would flip. Add Noise is checked against its own
//    seeded reference, `filterRandomUniform()`, so a wrong or default seed
//    reddens rather than merely differing.
//  - **The selection**, over the WHOLE excluded region rather than a sample
//    point: a Gaussian Blur run with a partial marquee engaged leaves every
//    probed texel outside it bit-for-bit what it was before, checked across
//    an allocated tile so the claim cannot pass by the tile merely not
//    existing (`runBucketRefusalTest()` section E's own discipline). A
//    second run with NO selection touches the whole layer, so "the gate
//    respects a selection" is distinguished from "the gate always shrinks
//    the region".
//  - **History.** Exactly one entry per confirmed dialog, named for the op
//    (not "brush stroke" or a shared "filter"), and undo restores the
//    pre-filter tiles exactly -- `memcmp` on the raw half words, the same
//    standard app/StrokeSession's own stroke-granularity section holds
//    itself to. An identity request (Gaussian Blur at sigma 0, Add Noise at
//    amount 0) records NOTHING, the same "an edit that changed nothing must
//    not create an undo step" rule `runBucketRefusalTest()` proves for the
//    bucket.
//  - **The refusal.** A Filter-menu op invoked on a Pigment layer and on a
//    locked RGB layer both refuse by the SAME `PixelOpRefusal` the bucket
//    uses, name the layer, and leave every texel -- and the revision --
//    untouched. Reused rather than reinvented, which the assertion checks
//    directly: the refusal reason and message for a Filter op and for the
//    bucket, on the identical layer, agree up to the op's own name.
//  - **Image Size and Canvas Size.** Each resamples/re-extents through
//    `ops/DocumentTransform`, records one Structural history entry, and a
//    1:1 request (same width and height) records none -- the document-level
//    twin of the pixel-level no-op rule above, checked against
//    `DocumentTransformResult::previousWidth/Height` rather than against
//    `ok`, since `ok` is true for a no-op too.
//  - **Enable predicates, as pure functions of constructed state, no
//    window**: the Filter menu's four items follow `filterLayerUsable`
//    exactly as `pixelOpWritesLayer()` says it should for an RGB layer, a
//    Pigment layer and a locked RGB layer, matching `runMenuModelTest()`'s
//    own style of asking `buildMenuModel()` rather than a window; the Image
//    menu's two items follow `hasDocument` alone, with no layer-shaped
//    refusal to check, per app/FilterOps.hpp's own argument for why a
//    document-level op does not take one.
//
// Headless and GPU-free, like every ops/ section it sits beside.
bool runFilterMenuTest();
// Reachability audit A5/B2/B3: three BRUSH-panel bugs sharing one cause -- a
// control drawn twice (once in ui/AtelierChrome.cpp's options bar, once in
// ui/MacPaintUI.cpp's BRUSH panel) with nothing checking the two copies
// agree, or a clobber running unconditionally after a slider that looked
// live. Headless and GPU-free; deliberately does not drive Dear ImGui (see
// the function's own closing comment for exactly what that leaves
// unverified at this level). Four parts:
//
//  - **B3, generalised.** A table of every `BrushState` field both surfaces
//    edit today (radius, hardness, load, wetness), asserting each has
//    exactly one range -- app/AppState.hpp's kBrush*Min/Max constants, which
//    both widgets now read instead of each carrying its own literal -- and
//    that range matches an independently re-derived expectation (radius's
//    200 px ceiling traced to brush/Deposit.hpp's own comment naming it "the
//    widest radius the UI offers").
//  - **The round trip B3 itself asks for**: 150 (only the wide range admits
//    it) survives `std::clamp` against the shared bound twice, modelling
//    both widgets' clamps in the order a user could hit them; the retired
//    2..90 options-bar bound is then applied on purpose, for contrast, and
//    shown to still truncate 150 -> 90 -- proving the assertion above is
//    sensitive to the regression it names, not merely satisfied by it being
//    absent.
//  - **B2's WET predicate**, `wetnessReachesSolver()` (app/StrokeSession.hpp)
//    as a pure function of the route: no layer or the Water tool honours it,
//    a writable RGB or Pigment layer under Brush does not, a locked layer
//    still does not -- and the case the brief names specifically, switching
//    Brush to Water on the IDENTICAL selected layer flips the same call from
//    disabled to honoured, proving the predicate reacts to the live route
//    rather than being a fixed "always disabled".
//  - **A5, resolved explicitly**: the loaded pigment owns Density/Staining/
//    Granulation (PLAN.md's own record of brushTipFor(), 2026-08-21 --
//    "the colour travels, the three physical constants do not"). Asserted
//    by replicating main.cpp's per-frame resolution
//    (`st.sim.density = pig.density;` etc.) against a value deliberately
//    tampered away from the pigment's own first, so the round trip cannot
//    pass vacuously, then showing the pigment's own numbers are what is
//    left standing.
bool runChromeConsistencyTest();
// Reachability audit D5 / PRD I13 (P1) -- "Saves are read back and
// structurally verified before the original leaves memory." Unimplemented
// before this: `saveNpaint()` had no reader call, `saveDocumentAs()` went
// straight from write to bookkeeping, and app/Journal.cpp's own crash
// checkpoint called its hash check "a small down payment on PRD I13, which
// does not exist yet."
//
// **The finding this step surfaced that outranks the missing readback:**
// `saveNpaint()` writes in place. With verification off -- every caller
// before this step, and app/Journal.cpp's periodic checkpoint still -- a
// write that dies partway does not leave a merely-incomplete file; its own
// failure path removes it outright, so a failed save can destroy a
// previously-good version rather than merely fail to add a new one.
//
// io/NpaintFile's `NpaintSaveOptions::verifyReadback` (off by default, forced
// on by app/DocumentLifecycle.cpp's saveDocumentAs() for every explicit
// Save / Save As / Save a Copy / Save Incremental) closes both problems with
// one mechanism: write to a temp sibling, read it back through
// `loadNpaint()` -- the same reader File > Open uses -- compare structurally
// and pixel-for-pixel against the in-memory document under
// `verifyNpaintRoundTrip()`'s stated identity-tile rule, and only then
// rename it into place. At every failure point the original file has not
// been opened for writing at all.
//
// Covered, in order: a good save verifying and marking the document clean
// with the close guard falling silent; the assertion a hash cannot make (two
// different, individually intact documents, one file, one caught by
// structural comparison and not by any byte check); a genuinely truncated
// file refused, beside a demonstration that a byte-level read of it succeeds
// completely; a forced write failure and a forced rename failure, each
// against a path already holding a known-good file, proving that file's
// size is unchanged; and the same guarantee one layer up through
// saveDocumentAs() -- a failed save leaves the document dirty, the in-memory
// edit untouched, and app/CloseDecision.hpp's close guard still asking.
//
// Gated on `oiioBackendCompiledIn()` and prints a skip line rather than
// going quiet, matching runOpenAnyFileTest()'s and runNpaintFormatTest()'s
// own convention -- not reachable in practice, since the top-level
// CMakeLists.txt now defines NP_USE_OIIO unconditionally, but kept per
// PLAN.md 1.5's "an unexercised build option is not a seam."
bool runSaveReadbackTest();
// ---------------------------------------------------------------------------
// track8/zoom: app/ZoomAndSize's pure functions -- PRD Q1 (the Zoom tool's
// click/Alt-click/scrubby-drag, cursor-anchored) and PRD R5/D3 (brush size
// by the ⌃⌥-drag canvas gesture and its `[`/`]` alternate). Headless and
// GPU-free -- every function under test takes and returns plain floats or a
// `Tool`, no ImGui/GPU/OpenDocument involved, specifically so the suite
// exercises the actual arithmetic `ui/MacPaintUI.cpp`'s canvas block and
// `main.cpp`'s key dispatch call, not a restatement of it. Covers:
//  - `panForAnchoredZoom()`: the point under an arbitrary screen anchor
//    stays at the same document coordinate across a zoom change, checked
//    against hand-workable cases and a spread of randomized zoom/pan states
//    -- the property the OLD wheel-zoom formula (see this header's own
//    replacement comment) never actually had.
//  - `zoomFactorForDrag()`/`radiusForDrag()`: pure, monotonic, symmetric
//    about zero, and -- because both are exponential -- equal whether
//    called once with a total or accumulated frame by frame, which is how
//    `ui/MacPaintUI.cpp` actually drives the brush-size gesture.
//  - `clampViewZoom()`/`clampBrushRadius()`: the two clamps `applyZoomFactor`
//    and the `[`/`]`/⌃⌥-drag paths actually call, not the constants alone --
//    a prior track's suite once asserted only the constants and a
//    regression of the code that reads them stayed green.
//  - `toolZoomsView()`: true for `Tool::Zoom`, false for a sample of tools
//    with no zoom handler.
bool runZoomAndSizeTest();

// track10/input ("make Mac trackpad input feel right"): app/WheelInput.hpp's
// pure functions -- the notch-vs-precise classifier
// (`wheelDeltaIsPrecise()`), the panel scroll's discount and exponential
// smoothing (`wheelScrollPixels()`/`smoothedScrollStep()`), and the pinch
// path (`zoomFactorForPinch()`/`canvasPanForPreciseWheel()`), the latter
// composed with this same file's `panForAnchoredZoom()` to prove the anchor
// property specifically for a pinch gesture. Headless: `--selftest` cannot
// reach the SDL/ImGui dispatch sites this replaces (F4), which is why the
// decision logic was pulled out into functions this can call directly.
bool runWheelInputTest();
// track10/feel: PaintCopilot §3.2 (arXiv:2605.20941)'s two pressure-feel
// contributions -- brush/Dynamics.hpp's own header comment on
// `EasingPreset::LogTaper`/`PowerIn` has the curve-vs-hard-code argument in
// full; not restated here. Covers:
//  - `LogTaper`/`PowerIn`'s built `Curve`s: both endpoints exactly 0/1, the
//    x=0.5 knot exactly equal to the closed-form function it was sampled
//    from (`pressureResponseRadiusNorm()`/`pressureResponseOpacityNorm()`),
//    a measurable, DERIVED gap from the linear interpolation at that same
//    point (not guessed), and monotonic across a dense sweep of the whole
//    domain -- an actual property of the built spline, not merely of the
//    nine points that define it.
//  - `defaultBrushLinks()`'s existing two links still resolve to Linear --
//    the new presets are additive and change no existing brush by default.
//  - `dynamicPressureEma()`: a held input is a fixed point, the formula's
//    exact literals at a couple of hand-workable steps, and a spread-out
//    convergence check (repeated application closes the gap to a held
//    target geometrically, by the documented `1 - 0.7^n`).
//  - `StrokeSession::smoothPressure()`: the first call after `begin()`
//    returns the raw sample unchanged (no manufactured ramp-up), later
//    calls follow the same recursion `dynamicPressureEma()` proves in
//    isolation, and -- the one this section spends the most words on --
//    the state does NOT survive an `end()`/`begin()` pair: a second
//    stroke's first call returns exactly its own raw sample, not a value
//    blended with the first stroke's last smoothed reading.
// app/StrokePreview -- the BRUSH EDITOR's TEST STROKE strip.
//
// Mostly assertions of one shape: change a setting a single dab CANNOT
// express -- spacing, scatter, a stroke-local source, direction, grain -- and
// require the rasterised strip to change. A preview that ignored any of them
// would pass "does it draw something" forever, and those settings are most of
// what the panel's sliders control (app/StrokePreview.hpp §1).
//
// Also covers the cache, because a preview whose cache stopped invalidating is
// indistinguishable from a slider that does nothing -- which is the complaint
// the feature exists to answer -- and `brushTipEqual()`, the complete tip
// comparison that key leans on.
bool runStrokePreviewTest();

bool runPressureFeelTest();

// Phase C Part 2: Transfer Opacity/Flow (`PsTransfer::opacity`/`.flow`,
// `opVr`/`prVr`), latched once at `StrokeSession::begin()` -- Opacity
// directly into the RGB deposit/erase accumulators, Flow into
// `transferFlowMul_`, applied fresh every dab. Asserts a stroke with an
// inert Transfer Variance paints bit-identically to no model at all (dab/
// texel counts, tile set AND the stored pixel), and that a real, hand-built
// `opVr`/`prVr` with a `PenPressure` control measurably -- and, at a single
// non-overlapping hard-disc dab's own centre, EXACTLY -- changes the stored
// alpha (1.0 unscaled vs 0.5 halved, for both Opacity's ceiling and Flow's
// weight). Pure CPU, no document window, no GPU.
bool runTransferDynamicsTest();

// Phase C Part 3 (bounded investigation): `blendModeFromPsToolOptions()`
// (brush/ToolOptionsBlend.hpp), the edge mapping from Photoshop's own `Md `
// tool-options blend id onto `core::BlendMode`, and `brushTipFor()`'s one
// call site for it. Groundwork only -- `BrushTip::blend` is not wired into
// any of the four deposit routes (see that field's own comment in
// brush/Deposit.hpp for the two obstacles found), so this asserts the
// mapping's three honoured ids, its three distinctly-worded refusals, and
// the fallback to Normal, rather than any painted pixel. Pure CPU, no
// document window, no GPU.
bool runToolOptionsBlendTest();

// Paper tooth (brush/Deposit.hpp §2e, brush/Grain.hpp): US 5,347,620's tiled
// grain field and its `F = clamp(P*S*O1 - G, 0, 1)` overlay fraction, headless
// and GPU-free. Proves, in order: the field is a pure, deterministic function
// of its inputs; it tiles exactly at its own period; the formula's clamp
// endpoints; that a peak texel gets STRICTLY less coverage than a valley
// texel under the identical pressure, with the gap quantified rather than
// merely signed; that grain OFF costs not one extra floating-point operation
// against the identical stroke with the parameter left at its default; that
// `app/DabPreview` and a real `depositDab()` agree on a grained dab, texel for
// texel, the same identity `runDabPreviewTest()` already proves for an
// ungrained one; and -- the one assertion the whole feature stands or falls
// on -- that the SAME brush stroke painted at two different absolute document
// positions picks up DIFFERENT grain, because a field keyed on the dab's own
// offset instead of the page would be a texture printed on the brush, not on
// the paper.
bool runGrainTest();

// The OS file panel (ui/FileDialog.hpp), which replaced the typed-path modal
// that Open, Save As, Save a Copy, Import Image and Import Brushes shared.
//
// The panel itself is out of reach here -- it needs a window and a person --
// so this asserts the parts that fail *silently* rather than visibly. Chiefly:
// that **SDL's own `validate_filters()`** accepts every filter pattern this
// build ships, because a pattern it rejects does not degrade to an unfiltered
// panel, it stops the panel from appearing at all and File > Open looks like a
// menu item that does nothing; that no pattern carries a `*`, which the cocoa
// backend reads as "do not filter" for the whole panel rather than for the one
// row; that the offered extensions are derived from io/Capabilities' live
// table, so a build without OpenImageIO cannot offer to open an EXR it cannot
// read; that a saved document's first offered extension is `.npaint`, because
// macOS appends the first one to a bare filename; and that the mailbox
// carrying the answer back from SDL's callback thread hands each outcome over
// exactly once, drops a callback with no request behind it, and cannot be left
// stranded -- a stranded request refuses every later panel for the rest of the
// session, with no panel anywhere to say why.
bool runFileDialogTest();

// app/DocumentPresets: the sizes File > New offers (docs/testing-issues.md
// T9, piece 1 -- the dialog itself is not this build's change; this is
// everything under it). Headless and GPU-free.
//
// Covered: the built-in list is well-formed (every entry passes
// validateDocumentPresetSize(), no two share a name); the round trip of a
// hand-built file through parse()/serialize(); a missing file falling back
// to "just the built-ins" (the normal first-run case); a malformed `size`
// line dropping only the preset it belongs to, with every sibling preset in
// the same file surviving -- proven by a file with three presets, the
// middle one sabotaged; a truncated/garbage file falling back to the
// built-ins rather than crashing or half-applying; a zero, negative, or
// absurdly large size refused at LOAD (never reaching a value
// Document::createBlank() would receive), for both a hand-edited file and
// the interactive add() path; that a user preset can never carry a built-
// in's exact name -- refused outright from add()/rename(), silently
// disambiguated by parse(); that a built-in is refused by remove() and by
// rename(); and a simulated mid-write crash (an abandoned, unrenamed `.tmp`
// file) leaving the real file untouched, the same durability proof app/
// selftest/UserBrushLibrary.cpp already makes for its own atomic writer.
// `$NP_DOCUMENT_PRESETS` keeps every case out of ~/Library/Application
// Support.
bool runDocumentPresetsTest();

// io/ClipboardImage: the system-pasteboard bridge docs/testing-issues.md T9
// (piece 2) asks for. Headless and GPU-free.
//
// Split in two, per the header's own §2: decodeClipboardImageBytes() is a
// pure function with no SDL dependency, and this is where the actual claim
// -- "these bytes, at this MIME type, become an Image at the right size
// with the right pixels" -- is proven, against a real PNG this test encodes
// itself with io/Export's own encoder (not a hand-written fixture), plus
// deliberately-broken bytes proving Unreadable rather than a crash.
// probeClipboardImage() itself is asserted only for its Empty and
// NotAnImage outcomes, produced by querying (never overwriting with a
// synthetic image -- see the header) the CURRENT state of the real SDL/
// Cocoa pasteboard; text is written and then the previous clipboard
// contents restored afterwards so this test does not leave a stray string
// on the machine's real pasteboard. The Image outcome of
// probeClipboardImage() itself -- an image actually round-tripped off a
// live NSPasteboard -- is NOT exercised here; see this function's .cpp for
// how it was verified manually, and the report that introduced it for the
// dimensions actually observed.
bool runClipboardImageTest();

}  // namespace np
