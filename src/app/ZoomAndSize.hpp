#pragma once

#include "app/AppState.hpp"      // Tool, CanvasView
#include "brush/StrokePath.hpp"  // Vec2

namespace np {

// app/ZoomAndSize -- PRD Q1 (scrubby zoom, cursor-anchored) and PRD R5 (brush
// size by an on-canvas gesture), track8/zoom.
//
// Everything in this file is pure and headless -- no ImGui, no GPU, no
// OpenDocument -- specifically so `--selftest` can assert on the functions
// that actually compute the behaviour rather than on the constants they
// read. A prior track's own suite once asserted on named range constants in
// a header without ever calling the code that used them, and reintroducing
// the exact bug it was meant to catch left every one of those assertions
// green; this module exists so that mistake has nowhere to hide here.
// `ui/MacPaintUI.cpp`'s canvas block calls every function below by name
// rather than re-deriving any of this arithmetic inline.
//
// ==========================================================================
// 1. Zoom -- anchoring, and the scrubby-drag mapping
// ==========================================================================
//
// `ui/MacPaintUI.cpp`'s canvas block already had a wheel-zoom anchor formula
// (its `applyZoomFactor` lambda, PLAN.md Phase 2 step 11) with a comment
// claiming it kept "the point under the cursor" fixed. It does not, in
// general: the update
//
//   panX' = (panX + D) * k - D,   D = origin.x - paintOrigin.x
//
// never reads the mouse position at all, so the screen point it actually
// preserves drifts with whatever `panX` happened to already be -- provably
// so; see this track's own verification notes for a numeric counterexample
// (a mouse at screen x=300, after one wheel notch while panned, landed over
// document x=133.3 before and x=126.2 after -- not the same point). It only
// LOOKED right in the narrow case every manual test happens to hit first:
// zoomed to fit, unpanned, where the drifting anchor coincides with the
// canvas's own left edge closely enough that nobody wheel-zoomed while
// panned and watched for it. No `--selftest` covered it either way.
//
// `panForAnchoredZoom()` below is the fix, proved exact (not just plausible)
// by algebra and by 20000 randomized trials before this file was written:
// given the screen position of document x=0 at the OLD zoom (`originOld` --
// exactly `ui/MacPaintUI.cpp`'s own `origin.x`/`origin.y`, computed fresh
// every frame from `paintOrigin + margin + pan`), it solves for the pan that
// makes `anchorScreen` map to the identical document coordinate at the NEW
// zoom. `ui/MacPaintUI.cpp`'s `applyZoomFactor` now takes an explicit
// `anchorScreen` argument and calls this once per axis -- the wheel passes
// the live mouse position, the keyboard/menu commands pass the viewport
// centre (there is no cursor to anchor a keyboard command on), and the Zoom
// tool's click and scrubby drag pass the point the user actually clicked.
// One function, four callers -- not a second zoom path.
float panForAnchoredZoom(float anchorScreen, float originOld, float zoomOld, float zoomNew,
                          float paintOriginAxis, float availAxis, float texAxis) noexcept;

// `panForAnchoredZoom()`'s own header admits a limitation by name: it
// "ignores rotation/mirror" and inherits "anchoring under a rotated view is
// a pre-existing limitation" at every one of its call sites. This is the
// generalisation that does NOT ignore them -- built for the raw two-finger
// trackpad gesture (below, section 3), which combines pan+zoom+rotate in one
// continuous motion and would show that limitation immediately (a pinch
// anchor that visibly drifts the instant the same gesture also twists).
// Existing callers of `panForAnchoredZoom()` (wheel/pinch/scrubby/keyboard
// zoom) are UNCHANGED and untouched by this addition on purpose -- their own
// limitation was a prior track's deliberate, stated scope boundary
// ("nothing here makes that better or worse"), not a defect this one is
// trying to also close.
//
// Solves for the NEW pan (both axes at once, since rotation couples them)
// that keeps `anchorScreen` mapped to the same canvas point across a
// combined zoom+rotate+mirror change, using `ViewTransform`'s own analytic
// forward/inverse -- not a second, hand-derived rotation formula:
//
//   1. `oldXform` (built from `oldView` and the CURRENT `pivotScreenOld`)
//      finds the canvas point currently under `anchorScreen`.
//   2. A throwaway `ViewTransform` for `newView` with `pivotScreen == (0,0)`
//      isolates exactly what that new zoom/rotation/mirror does to a vector
//      from canvas-centre -- `M_new * (anchorCanvas - canvasCenter)` -- via
//      the SAME public `toScreen()` every other caller uses, rather than a
//      new accessor onto `ViewTransform`'s private matrix.
//   3. The pivotScreen that would put `anchorCanvas` back at `anchorScreen`
//      follows by subtraction; `pan` is recovered from it by inverting
//      `pivotScreen = paintOrigin + margin(zoomNew) + pan + drawSize(zoomNew)/2`
//      -- `panForAnchoredZoom()`'s own algebra, one axis at a time, with
//      `margin` recomputed at the NEW zoom for the identical reason that
//      function already recomputes it.
struct AnchoredPan {
  float panX;
  float panY;
};
AnchoredPan panForAnchoredZoomRotate(const CanvasView& oldView, const CanvasView& newView,
                                     Vec2 canvasCenter, Vec2 pivotScreenOld, Vec2 anchorScreen,
                                     Vec2 paintOrigin, Vec2 avail, Vec2 tex) noexcept;

// The existing zoom limits (`ui/MacPaintUI.cpp`'s `requestFitWindow`/
// `applyZoomFactor` both already clamp to this exact pair) -- named here so
// every clamp site reads the same two numbers instead of retyping 0.1f/8.0f,
// not a second limit invented for this track. PRD Q1 doesn't ask for a wider
// or narrower range, so this is "find and reuse", not "decide".
constexpr float kViewZoomMin = 0.1f;
constexpr float kViewZoomMax = 8.0f;

// The one place `st.view.zoom` gets clamped, so a headless test can prove
// "zoom clamps at the existing limits and introduces no new ones" against
// the actual function `applyZoomFactor` calls, not against the two constants
// above in isolation.
float clampViewZoom(float zoom) noexcept;

// The step a discrete zoom command multiplies by -- the keyboard's `⌘+`/
// `⌘-` (already 1.2f / 1/1.2f in `main.cpp` before this track) and the Zoom
// tool's plain click/Alt-click now share this one name instead of each
// spelling 1.2f out separately.
constexpr float kZoomStepFactor = 1.2f;

// Pixels of horizontal drag per octave (one factor-of-2 step) of scrubby
// zoom, applied PER FRAME against `ImGui::GetIO().MouseDelta.x` exactly the
// way `ui/MacPaintUI.cpp`'s existing rotate-view and pan gestures already
// apply their own per-frame deltas -- not accumulated from a remembered
// drag-start reference. That choice is safe here specifically because this
// mapping is exponential: doubling composes by ADDING exponents, so N
// one-pixel frames and one N-pixel frame produce the identical final factor
// (`zoomFactorForDrag(a) * zoomFactorForDrag(b) == zoomFactorForDrag(a+b)`
// exactly, mod floating-point rounding) -- which is what makes it honest to
// call this "a pure function of total drag pixels" even though the caller
// never actually holds a running total anywhere. A LINEAR pixels-to-factor
// mapping would feel right at one zoom level and be glacial or violent at
// another (the exact failure this track was warned against); exponential
// gives a constant *relative* rate regardless of the zoom already reached.
//
// The distance itself is reasoned, not measured -- this build cannot be run
// from here. 0.1x-8x is 6.32 octaves (log2(80)); 150px/octave puts a full
// sweep of that range at ~945px of drag, close to a typical canvas window's
// own width (`ui/MacPaintUI.cpp`'s golden-harness crop views put the canvas
// area at several hundred px tall/wide) -- one comfortable single drag
// across the visible working area, not a wrist-breaking multi-drag, and not
// so touchy that a few pixels of tremor visibly jumps the zoom.
constexpr float kZoomDragPixelsPerOctave = 150.0f;

// Pure: N pixels of horizontal drag -> a multiplicative zoom factor.
// factor(0) == 1 (no drag, no change); strictly increasing in `dragPixelsX`
// (monotonic); factor(x) * factor(-x) == 1 (symmetric about zero -- dragging
// right by x then left by x returns to the original zoom, not a compounded
// one).
float zoomFactorForDrag(float dragPixelsX) noexcept;

// ==========================================================================
// 2. Brush size -- PRD R5's on-canvas gesture and its `[`/`]` alternate
// ==========================================================================
//
// **This is deliberately about size only.** `docs/shortcuts.md` section 2
// binds `⌃⌥`-drag to "size and hardness by dragging" -- hardness is a
// second axis (read as vertical drag) that belongs to whichever track picks
// up the rest of R5; this one is `[`/`]` and the horizontal half of that
// chord, per this track's own brief.
//
// **One range, defined once.** `docs/reachability-audit.md` B3: the options
// bar clamps to 2..90 and the BRUSH panel to 1..200, so a size set in one
// silently truncates when the other is touched. `kBrushRadiusMin`/
// `kBrushRadiusMax` live in `app/AppState.hpp` (this header already pulls
// it in for `Tool`), next to `BrushState` itself and with the reconciliation
// note for the separate, not-yet-merged track that is unifying the two
// existing sliders onto the same two names. Nothing in this file hardcodes
// 1 or 200 a second time -- every clamp below calls `clampBrushRadius()`.

// The one place a brush radius gets clamped -- both the `[`/`]` dispatch in
// `main.cpp` and the `⌃⌥`-drag gesture in `ui/MacPaintUI.cpp` call this
// rather than each writing `std::clamp(r, kBrushRadiusMin, kBrushRadiusMax)`
// separately, which is exactly how B3's bug happened the first time (two
// call sites, one range each).
float clampBrushRadius(float radius) noexcept;

// The fraction of the CURRENT radius that one `[`/`]` press steps by, floored
// to a whole pixel with a 1px minimum. Proportional rather than a fixed
// pixel count for the same reason `zoomFactorForDrag()` above is
// exponential: PRD's own words for this track, "a constant is wrong across
// a 1..200 range" -- 1px would take 199 presses end to end, and a step big
// enough to be useful at 200 would overshoot wildly at 2. 10% is reasoned
// from that same "proportional, not additive" principle Photoshop's own
// bracket-key behaviour is built on -- this build cannot run Photoshop
// side-by-side to read its exact table, so this is a stated, justified
// choice rather than a claimed transcription of one.
constexpr float kBracketStepFraction = 0.1f;

// Pure: the `[`/`]` step size for the CURRENT radius (always positive; the
// caller adds or subtracts it and then clamps through `clampBrushRadius()`).
float bracketStepForRadius(float radius) noexcept;

// Pixels of per-frame horizontal `⌃⌥`-drag per octave of brush radius --
// the size-gesture's analogue of `kZoomDragPixelsPerOctave` above, same
// exponential-composes-by-addition reasoning, a SEPARATE constant because
// this gesture is reasoned to want a shorter, quicker sweep: brush size is
// adjusted constantly mid-stroke (R5's whole point is not leaving the
// canvas to reach a slider), where zoom is a more occasional, deliberate
// navigation. 1..200 is 7.64 octaves (log2(200)); 40px/octave puts a full
// sweep at ~306px -- a quick flick rather than a drag across the window.
constexpr float kSizeDragPixelsPerOctave = 40.0f;

// Pure: `startRadius` scaled by `dragPixelsX` pixels of drag (unclamped --
// the caller clamps through `clampBrushRadius()`, same as the bracket
// step). The same "exponential composes by addition" property as
// `zoomFactorForDrag()` makes this equally valid called once with a total,
// or per-frame with `ui/MacPaintUI.cpp`'s live `ImGui::GetIO().MouseDelta.x`
// against the CURRENT radius, which is how it is actually driven -- the
// same per-frame style as that file's existing rotate-view and pan
// gestures, so this file introduces no new persisted "drag start" state at
// all.
float radiusForDrag(float startRadius, float dragPixelsX) noexcept;

// ==========================================================================
// 3. The tool predicate
// ==========================================================================
//
// **Not a hand-maintained table entry.** `docs/reachability-audit.md` F1
// names the exact failure mode this must not repeat: `toolImplemented()` is
// a boolean nobody checks against whether a canvas handler exists, which is
// the direct cause of the Zoom tool (and the Eyedropper) looking selectable
// and doing nothing. A separate, in-flight track is giving
// `ui/AtelierChrome` a `toolHasCanvasHandler(Tool)` built as the
// disjunction of one predicate per canvas-gated block --
// `toolWritesRgbPixels()` (already in `app/StrokeSession`, gates the
// bucket/gradient block), and siblings for selection-drawing,
// canvas-sampling, view-panning and stroke-beginning. `toolZoomsView()`
// is this track's own member of that same family: it is the LITERAL
// expression `ui/MacPaintUI.cpp`'s canvas block gates the Zoom tool's
// click/drag handling on, not a redundant restatement of it, so a future
// `toolHasCanvasHandler()` can absorb Zoom by including this function
// rather than hand-listing `Tool::Zoom` a second time. See this track's own
// report for where it must be reconciled with that branch at merge.
bool toolZoomsView(Tool tool) noexcept;

// ==========================================================================
// 4. Canvas dimensions -- the active document is the one source of truth
// ==========================================================================
//
// naturalPaint canvasdim bug report: "Making a non-square document still
// shows up square. Changing the canvas size to non-square shows square."
// Root cause: `ui/MacPaintUI.cpp`'s canvas block computed its `texW`/`texH`
// -- the on-screen canvas geometry EVERYTHING else in that block reads
// (fit-to-window, `drawSize`, the `ViewTransform`, the corner quad, the
// navigator, the zoom anchor, hit-testing, the grid overlay and guides) --
// from `main.cpp`'s two compile-time constants `kCanvasW`/`kCanvasH`
// (1024x1024), never from the active document's own `Document::width`/
// `height`. A document opened or resized to anything other than 1024x1024
// therefore always drew as a 1024x1024 square: File > New with a portrait
// size, and Image Size / Canvas Size afterward, both looked like they had
// no effect, because the on-screen quad never read the number either one
// of them changed.
//
// `canvasDimensionsFor()` is the fix, and it is deliberately the ONLY place
// that decides -- `ui/MacPaintUI.cpp`'s canvas block calls this once, into
// its local `texW`/`texH`, rather than re-deriving "document size, or the
// fallback" inline a second time the way the Add Guide popup's percentage
// field used to (a second, independent read of `canvasW`/`canvasH` a few
// hundred lines above the canvas block, fixed alongside this).
//
// `doc` is nullable BY DESIGN, not an oversight: a session can be empty
// (`DocumentSession::active()` already returns `nullptr` for it, and
// `ui/AtelierChrome.hpp`'s `atelierPaneDocuments()` agrees -- see that
// function's own "one empty pane" comment), and the canvas still has to draw
// *something* -- the blank-paper quad -- while nothing is open. In that case
// `fallbackW`/`fallbackH` (the caller's `kCanvasW`/`kCanvasH`) are the
// answer, and ONLY in that case: once a document exists, its own size wins
// outright, with no blending or averaging against the fallback.
//
// The zero/negative guard is defensive rather than reachable in this build
// today -- every constructor of a `Document` (`Document::createBlank()`,
// every `io::` loader) enforces a positive width and height before a
// `Document` value can exist at all -- but this is the one function every
// division in the canvas block's fit-to-window and zoom-anchor arithmetic
// (`avail.x / texW`, etc.) ultimately depends on, so it re-checks rather
// than trusts a caller three modules away, the same defensive posture
// `ViewTransform::Mat2::inverse()`'s own zero-determinant guard takes for
// the same reason: total correctness costs one comparison, and a division
// by zero costs a NaN that silently poisons every frame after it.
//
// **What this function does NOT fix, and why**, is `sim::PaintSim`. The
// solver is one texture, `width_`/`height_` fixed at whichever call to
// `ensurePaintSim()` happens to construct it first (`sim/PaintSim.hpp`/
// `.cpp`) and untouched by any later one -- `ensurePaintSim()`'s own body
// returns the existing `sim.get()` immediately if `sim` is already non-null,
// ignoring the `width`/`height` it was just called with. `ui/MacPaintUI.cpp`
// now passes THIS function's own result to that first call (so a document
// open when the user makes their first Watercolour/Oil stroke gets a solver
// sized to match it, not a hardcoded 1024x1024), which closes the common
// case. It does not, and cannot without recreating the solver's textures and
// deciding what happens to whatever paint is already wet in them, close the
// case of a document resized (Image Size / Canvas Size) or switched
// (multiple open documents of different sizes sharing the one solver --
// already a known limit; see the unfocused split-pane's own comment in
// `ui/MacPaintUI.cpp`) AFTER the solver already exists: the paper quad's
// texture then stays whatever size it was first built at while the document
// quad drawn over it follows the document exactly, and the two visibly
// disagree. That mismatch is real, pre-existing in the sense that the solver
// was never document-sized OR resizable to begin with, and out of this
// track's scope -- recreating a live solver's GPU state on every Canvas Size
// change (and deciding whether wet paint survives the resize) is a
// substantially bigger change than a display-geometry fix, and PRD/PLAN.md
// name no requirement that forces it here.
struct CanvasDimensions {
  float w;
  float h;
};

// Pure: the active document's own size if `doc` is non-null and both of its
// dimensions are positive, else `{fallbackW, fallbackH}`.
CanvasDimensions canvasDimensionsFor(const OpenDocument* doc, uint32_t fallbackW,
                                     uint32_t fallbackH) noexcept;

// --- the solver's own size, which is NOT the display size -----------------
//
// `canvasDimensionsFor()` above answers "how big is the picture on screen",
// and that answer is cheap: it sizes a quad. `ensurePaintSim()` asks a
// different question with identical-looking arguments -- "how big a fluid
// solver should I allocate" -- and that answer is not cheap at all, so the
// two do not share one function even though the fix above made it tempting.
//
// **The arithmetic, MEASURED 2026-09-02 and asserted, not read off a
// header.** This paragraph used to say 176 and 272 B/texel, and both were
// low: it counted `allocFields()`'s seven ping-pongs and quietly omitted the
// five single textures allocated in the same function. The corrected figures
// are held against a live solver by `app/selftest/SolverFootprint.cpp` and
// published as `PaintSim::kFieldBytesPerTexel` /
// `PaintSim::kInkFieldBytesPerTexel`, so this can no longer drift silently.
//
// Seven ping-pong fields are allocated unconditionally, each of them TWO
// full-resolution textures (`makePingPong()` builds `tex[0]` and `tex[1]`),
// at `kWaterFormat = RGBA16Float` (8 B/texel) or `kPigmentFormat =
// RGBA32Float` (16 B/texel):
//
//   water_ 8   pigC_ 16   pigR_ 16   depC_ 16   depR_ 16   sat_ 8   aux_ 8
//   -> 2 x (8 + 16 + 16 + 16 + 16 + 8 + 8) = 176 bytes per texel
//
// and then five singles in the same function, which the old figure missed:
//
//   paper_ 8 (RGBA16Float)  selection_ 1 (R8Unorm)  canvas_ 4, grayscale_ 4,
//   graded_ 4 (RGBA8Unorm)  -> another 21 B/texel
//
//   => **197 bytes per texel**, unconditionally.
//
// `allocInkFields()` adds three more RGBA32Float ping-pongs (lbmA_, lbmB_,
// lbmC_) the moment the Ink medium is used: 2 x 3 x 16 = **96 B/texel**
// exactly, measured, for **293 bytes per texel** in the worst case.
//
// (`allocOilFields()` is deliberately absent from these figures: its three
// ping-pongs are `kBrushGrid` x `kBrushGrid` = 64x64, a fixed 1.5 MiB, not
// canvas-sized. `app/selftest/SolverFootprint.cpp` asserts that too, because
// the day it becomes canvas-sized these numbers stop describing the worst
// case.)
//
// At the 1024x1024 this build has always allocated, that is **207.7 MB**
// (197 MiB), or **308.4 MB** (293 MiB) once ink is in play -- 60.2% of the
// design's 512 MB memory budget (`app/Memory`, asserted by
// `app/selftest/AtelierChrome.cpp` as "the budget is the design's 512 MB").
// Doubling the texel count puts the solver alone over that entire budget.
// Verified against `MTLDevice.currentAllocatedSize`: constructing the solver
// at 1024x1024 moves the device's total allocation by 198.13 MiB, of which
// 197.00 MiB is these textures and the remaining 1.13 MiB is the uniform,
// occupancy and readback buffers, the samplers, and the compiled pipelines.
//
// **Why this function has to exist at all.** The canvasdim fix routes the
// active document's size into the `ensurePaintSim()` call, which is right
// for a document near the old fixed size and is how a portrait document
// gets a portrait paper quad. But `Document` dimensions are user-chosen and
// bounded only by `app/DocumentPresets.hpp`'s
// `kMaxDocumentPresetDimension = 32768`. Passed through unclamped, a
// 32768x32768 document's first Watercolour stroke asks the driver for
// 1.07 Gtexel x 293 B = **315 GB**, and even an ordinary 4000x3000 photo
// import asks for 3.5 GB where this build has never allocated more than
// 308 MB. That is not a slow path or a large number to keep an eye on; it
// is a hard allocation failure on the first brush-down, in a call that used
// to be a fixed, shipped, measured 1024x1024.
//
// **The budget, and why it is this number rather than a chosen one.** The
// cap is exactly the texel count the fixed canvas already allocated --
// 1024 x 1024 = 1,048,576 -- so this build's solver footprint after the
// canvasdim fix is what it was before it, to the byte. That makes the cap a
// status-quo guarantee rather than a judgement call: raising it becomes a
// deliberate, separately-measured decision about the 512 MB budget, and
// nothing here quietly makes one on the way past. A document inside the
// budget (the reported 800x1200 is 960,000 texels, comfortably inside) gets
// a solver shaped like itself; a document over it falls back to
// `fallbackW`/`fallbackH` and therefore to exactly the pre-fix behaviour --
// the paper quad stretches under a correctly-shaped document quad, which is
// the gap the section above already documents, not a new one.
inline constexpr uint64_t kPaintSimMaxTexels = 1024ull * 1024ull;

// Pure: `canvasDimensionsFor()`'s answer when the document fits within
// `kPaintSimMaxTexels`, else `{fallbackW, fallbackH}`. The caller owns its
// own fallback: if that already exceeds the budget this hands it back
// unchanged rather than silently overriding a number the caller chose.
CanvasDimensions paintSimDimensionsFor(const OpenDocument* doc, uint32_t fallbackW,
                                       uint32_t fallbackH) noexcept;

// ==========================================================================
// 5. The whole-view reset -- track11/pan-rotate-reset, "add a view reset
// with a hotkey"
// ==========================================================================
//
// `MenuAction::ResetRotation` (`ui/MenuModel.hpp`, `Shift+R`) already exists
// and already resets exactly one field, `rotation`. There was no command
// that reset the WHOLE view -- zoom left wherever a session's wheel-zoom/
// pinch/scrubby-zoom left it, pan left wherever a drag left it, both mirrors
// left toggled -- short of quitting and reopening. `MenuAction::ResetView`
// (`Shift+Cmd+0`, chosen to sit next to `Cmd+0` "Fit to Window" and `Cmd+1`
// "100%", the two existing single-purpose view commands) is that command,
// and folds `Shift+R`'s own reset in rather than leaving two overlapping
// "reset something" actions that disagree about what "reset" means.
//
// **Six fields, not eight, and both directions matter.** `CanvasView`
// (`app/AppState.hpp`) has EIGHT fields: zoom, panX, panY, mirrorX, mirrorY,
// rotation, grayscale, grade. This resets the first six -- the *navigation*
// state -- and deliberately leaves `grayscale`/`grade` untouched: those are
// preview toggles a user switched on explicitly through their own section
// of the UI, not view geometry, and "reset my scroll position" silently
// killing an in-progress grade preview would surprise rather than help.
// Getting either direction wrong is a real bug this function's own selftest
// case is written to catch -- missing `mirrorY` (leaves a mirrored canvas
// silently mirrored after "reset") is the headline example, but silently
// clearing `grade` too would be the opposite mistake, equally worth an
// assertion.
//
// Six fields are set explicitly below rather than `CanvasView{}` (which
// would also reset `grayscale`/`grade` today, and would silently start
// resetting whatever NINTH field a future PR adds to the struct without
// anyone revisiting this function) -- so leaving one of the six out, or
// letting a ninth field ride along unnoticed, is a diff someone has to
// write on purpose, not a default nobody chose.
CanvasView resetCanvasView(const CanvasView& current) noexcept;

}  // namespace np
