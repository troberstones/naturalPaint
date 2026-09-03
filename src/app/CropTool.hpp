#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "app/DocumentLifecycle.hpp"
#include "app/SelectionDrag.hpp"  // SelectionMoveState -- the marquee's Space-move, reused
#include "core/Document.hpp"
#include "ops/DocumentTransform.hpp"
#include "ops/Transform.hpp"

// app/CropTool -- `Tool::Crop`, the palette's `C` cell, in both of its modes,
// plus the two Image-menu items that fall out of the same machinery.
//
// ==========================================================================
// (1) THIS FILE ADDS NO GEOMETRY ENGINE. IT ADDS TWO DECISIONS AND A REFUSAL.
// ==========================================================================
//
// Everything that moves a texel here already existed and is tested:
//
//   - `ops/DocumentTransform::cropDocument()` extracts a half-open region as
//     the new document, bit-exactly, by translating every store of every layer
//     -- and deliberately does NOT clip what falls outside, so an undo gives
//     back what the crop hid. That non-clipping contract is load-bearing below
//     and is asserted in `--selftest` rather than assumed.
//   - `ops/Transform::transformFromQuad()` solves the four-corner homography
//     (PRD D14) in double, with partial pivoting.
//   - `ops/DocumentTransform::transformDocument()` carries RGB, pigment, masks
//     and the selection through one matrix.
//
// `transformFromQuad()` had **no caller outside `ops/` and its own selftest**
// before this file: a finished eight-unknown solve with nothing in the UI able
// to reach it. This file is the reach, not a second solver. If a number below
// looks like it wants a matrix, it is because it is choosing the *destination*
// for one of those three functions -- which is the only thing a crop tool
// actually decides.
//
// What this file therefore owns is exactly three things, and each of them is a
// judgement call rather than arithmetic:
//
//   (a) the rectangle a rectangle-mode drag names   -- section 2
//   (b) the rectangle a four-corner quad warps into -- section 3, the big one
//   (c) which quads are refused, and in what words  -- section 4
//
// ==========================================================================
// (2) THE RECTANGLE RULE: OUTWARD, UNCLAMPED, HALF-OPEN
// ==========================================================================
//
// A drag gives two float document points. `cropRegionFromDrag()` turns them
// into a `DocumentRegion` by three rules, in this order:
//
//   **Sorted.** Either drag direction names the same rectangle. (The marquee's
//   `computeSelectionDragBox()` already sorts, and the crop drag is fed through
//   that same function in `ui/MacPaintUI.cpp` so Shift-constrain, Option-from-
//   centre and Space-move come free and cannot drift from the marquee's.)
//
//   **Rounded OUTWARD** -- `floor` on the near edge, `ceil` on the far one --
//   not rounded to nearest. A crop is destructive; the two roundings differ by
//   at most one texel per edge and they differ in *which direction they fail*.
//   Rounding to nearest can drop a texel the user could see inside their own
//   rectangle, and there is no way to get it back except undo. Rounding outward
//   can keep a texel the user could see just outside it, which the next drag
//   fixes. When a destructive op has to guess, it guesses in favour of keeping
//   pixels.
//
//   **NOT clamped to the canvas.** This is the one that looks like a bug and is
//   not. `cropDocument()`'s own header states that a negative origin, or an
//   extent running past the old canvas, "is fine and is how *extend the canvas*
//   is spelled" -- so a crop rectangle dragged off the edge of the picture
//   grows the document and fills the new margin with transparency, exactly as
//   Photoshop's crop tool does. Clamping here would be the UI quietly hiding a
//   capability the engine documents, and the marquee's clamp is not precedent
//   for it: a *selection* outside the canvas would have every consumer walking
//   tiles holding nothing, while a *crop* outside the canvas is a canvas that
//   now contains those tiles. `--selftest` asserts the negative-origin case
//   grows the extent and that undo restores the original.
//
// Refused below one texel in either axis, which is `cropDocument()`'s own
// zero-extent refusal moved one step earlier so the tool can say so instead of
// discarding the gesture in silence.
//
// **One sharp edge in the outward rule, and it is deliberate.** Rounding
// `[10.2, 10.4]` outward is one whole texel, which is right -- the user
// pointed at that texel. Rounding `[10.2, 10.2]` outward would be one whole
// texel too, which is wrong: that is a *click*, not a drag, and a click that
// armed a 1x1 crop would leave the next Enter one keystroke from destroying
// the document. So an axis with no extent at all reports zero and the region
// is refused -- the same "an empty gesture is not a gesture" rule
// `commitDrawnSelection()` applies to a zero-area marquee. Both halves are
// asserted, because an implementation that got either one alone would look
// correct from the other side.
//
// ==========================================================================
// (3) THE OUTPUT RECTANGLE FOR A PERSPECTIVE CROP
// ==========================================================================
//
// The user marks four corners of something that ought to be a rectangle -- a
// receding wall, a photographed page, a picture hung crooked -- and this warps
// that quad into a rectangle. The question the whole feature turns on is *what
// rectangle*: the homography is fully determined only once a destination is
// chosen, and every choice is visible as how much the picture stretches.
//
// **First, the honest part, because it changes what the rule can be for.**
// There is no purely 2D rule that recovers the scene's true aspect ratio from
// four image points. That is a metric-rectification problem and it needs the
// camera's intrinsics; the standard trick of estimating a focal length from the
// two vanishing points of the quad is degenerate exactly where this tool is
// used most -- a mild correction, where the quad is nearly affine, both
// vanishing points run off to infinity and the estimate goes to infinity or
// imaginary with it. A tool that silently produced a wild aspect ratio for the
// easy case in exchange for being right on the hard one would be worse than one
// that never claimed metric truth. So this file does not claim it, and the
// options bar says so where the user can read it (`ui/AtelierChrome.cpp`'s
// MODE combo tooltip) rather than leaving them to infer it from a squashed
// wall.
//
// Given that, the rule is not "which rectangle is true" -- none of them is --
// but "which rectangle throws away the least of what the source had".
//
// **The rule: each output dimension is the LONGER of its two opposite source
// edges, rounded to nearest, floored at one.**
//
//     width  = round(max(|c1 - c0|, |c2 - c3|))     the two horizontal edges
//     height = round(max(|c3 - c0|, |c2 - c1|))     the two vertical edges
//
// with corners in the fixed order c0 = top-left, c1 = top-right, c2 =
// bottom-right, c3 = bottom-left **of the output**. (The order is of the
// output, not of the screen: the same four points in a different order name a
// rotated or mirrored crop, which is why `CropQuad` carries them in an array
// with a stated meaning rather than as a set.)
//
// Why `max` and not the obvious `mean`:
//
//   1. **`mean` always minifies the near edge, and a crop is destructive.**
//      A quad whose near edge is twice its far edge -- an ordinary receding
//      wall -- has a mean of 0.75x the near edge, so every texel along the near
//      edge, the sharpest and closest part of the picture, is resampled down by
//      a quarter and that detail is gone. `max` resamples the near edge at 1:1
//      and magnifies the far edge instead. Magnifying data that was never there
//      shows up as softness where the picture *was* soft; minifying data that
//      was there destroys detail where the picture was sharp. Between an op
//      that only ever interpolates and one that discards, a destructive op with
//      nothing but undo behind it takes the first.
//
//   2. **It agrees with the rectangle mode exactly.** For an axis-aligned
//      rectangular quad the two horizontal edges are equal and so are the two
//      vertical ones, so `max` and `mean` both collapse to the rectangle's own
//      width and height -- a perspective crop of an unmoved rectangle is the
//      same extent as a rectangle crop of it. That is asserted, and it is the
//      property that keeps the mode toggle from being two different tools
//      sharing a name.
//
//   3. **It is monotone under the gesture.** Dragging a corner outward never
//      makes the output smaller. Under `mean`, pulling the near edge out while
//      the far edge is short can shrink the other axis' share of the result,
//      which reads as the picture flinching away from the handle being dragged.
//
// The cost, stated plainly because it is real: `max` produces a slightly larger
// image than `mean` for any non-rectangular quad (never smaller -- asserted),
// and the extra pixels along the far edge are interpolated rather than
// measured. `ops/Transform.hpp` section 3 already names the related limitation
// this inherits: the resampler picks its filter from the Jacobian at the
// destination *centre*, so a strong perspective is correctly filtered in the
// middle and progressively less so towards the far edge. That is not
// re-derived here and it is not exceeded silently -- the options bar warns on a
// quad whose edge ratio is past 3:1, which is where that approximation starts
// to show.
//
// ==========================================================================
// (4) REFUSALS ARE PART OF THE FEATURE, AND ONE OF THEM IS NOT THE ENGINE'S
// ==========================================================================
//
// `transformFromQuad()` refuses a singular system and says so. Read its
// implementation, though (`ops/Transform.cpp`), and the singularity test is
// `|pivot| < 1e-12` on the 8x8 solve -- which catches collinear points and
// collapsed quads, and **does not catch a bow-tie**. Four corners with corner 1
// dragged past corner 0 are not collinear and not collapsed; the system is
// well-conditioned and solves happily, and what comes back is a homography that
// folds the picture through itself. That is a UI-level judgement about which
// quads are meaningful, not a numerical one about which systems are solvable,
// so it belongs here and not in `ops/`. `cropQuadRefusal()` is that judgement.
//
// It is a **predicate the UI consults every frame**, not a message produced at
// commit: a refused quad is drawn in the refusal colour with the reason under
// it, Enter is disabled, and pressing it anyway repeats the reason rather than
// doing nothing. A bow-tie is one careless drag away and "nothing happened" is
// the worst possible answer to it.
//
// The ladder, in the order tested, each with its own sentence:
//
//   - a corner that is not finite
//   - two corners closer together than a texel   (collapsed)
//   - a corner whose two edges are within ~1.15 degrees of straight
//     (`kMinCornerSin`) -- near-collinear, where the solve's condition number
//     is already destroying the far corner before it becomes singular
//   - the four turns not all the same way        (bow-tie / self-intersecting)
//   - all four turns the *wrong* way             (corner order reversed; the
//     crop would come out mirrored, which is a legitimate picture but never
//     what a user who dragged four corners onto a wall asked for)
//   - a derived extent below one texel in either axis
//
// ==========================================================================
// (5) THE GESTURE, AND WHAT A CLICK OUTSIDE DOES
// ==========================================================================
//
// `CropSession` is the whole of the tool's state and lives on `AppState`
// beside the marquee's rubber band, for the marquee's own reason: it is an
// on-canvas gesture in document texel space and no `OpenDocument` may carry it.
// Like `app/MeasureLine` and unlike the marquee, **it outlives its own drag on
// purpose** -- the user drags a rectangle out, then adjusts it by its handles,
// then commits. So it carries the `DocumentId` it was drawn on, for
// `MeasureLine`'s reason: a rectangle in one document's texels is meaningless
// in another's, and a stale plausible rectangle is worse than none.
//
// Three states, and the third is the one the tool exists for:
//
//   `defining`  the initial rubber-band drag, pointer down
//   `active`    a shape is laid down, handles are live, outside is darkened
//   neither     no crop in progress
//
// **Commit and cancel, both discoverable:**
//
//   - **Enter / Return commits.** Also a double-click inside the shape, which
//     is the gesture Photoshop trained everyone to, and the tick in the options
//     bar, which is the one that is *visible* -- a keyboard-only commit on a
//     tool whose whole interaction is dragging is a commit half the users never
//     find.
//   - **Escape cancels**, as does the cross in the options bar, as does
//     switching tools (`app/ToolSwitch`), which discards rather than silently
//     keeping a rectangle that will reappear the next time Crop is chosen.
//   - **A click OUTSIDE the shape starts a new one; it does not commit and it
//     does not cancel.** Photoshop commits on a click outside, and that is the
//     one Photoshop behaviour deliberately not copied here: this is a
//     destructive op with no dialog, and "the user clicked somewhere else" is
//     not evidence they meant to destroy pixels. Starting a fresh rectangle is
//     what the same click does in every marquee tool in this build, so the
//     gesture is already learned, and the cost of getting it wrong is one more
//     drag rather than an undo.
//
// **The darkened surround is not decoration.** Outside the shape is drawn at
// 60% black. It is the only thing that makes the answer to "what am I keeping"
// readable at a glance on a picture that is already full of edges, and it is
// also the only on-canvas signal that a crop is pending at all.
//
// ==========================================================================
// (6) THE EIGHTH CANVAS GATE
// ==========================================================================
//
// `toolCropsCanvas()` is a NEW predicate, deliberately, and it lives *here*
// rather than being a name added to one of `app/StrokeSession`'s seven --
// which is `toolMeasuresCanvas()`'s own argument one tool over, and here it has
// the same concrete consequence: `toolDrawsSelection()` is the gate on the
// selection tools' canvas block, and a Crop that satisfied it would have every
// crop drag handed to `commitDrawnSelection()`. `ui/AtelierChrome`'s
// `toolHasCanvasHandler()` takes it as its eighth term, so the palette cell's
// `implemented` flag stays tied to a handler that actually exists.
//
// ==========================================================================
// (7) THE TWO MENU ITEMS
// ==========================================================================
//
// Image > Crop to Selection and Image > Trim to Content are `cropDocument()`
// with a rectangle each derives, exactly as `ops/DocumentTransform.hpp` section
// 6 predicted ("the op is three lines on top of `cropDocument()` and is a menu
// item, not machinery"). They are here rather than in `app/FilterOps` because
// the region rules are the same judgement the tool makes and belong beside it.
//
// **Both are intersected with the canvas, so neither can GROW the document.**
// That matters for Trim: `core/LayerGeometry.hpp` section 4 states that content
// outside `[0,width) x [0,height)` genuinely exists, so the union of layer
// content bounds on a document that has been cropped once is routinely *larger*
// than the canvas -- and a "Trim to Content" that made the picture bigger would
// be an astonishing answer to a command whose name promises the opposite. The
// tool's own drag is unclamped (section 2) and these two are clamped, and the
// difference is intent: a drag past the edge is a thing the user did on purpose
// and can see, a trim is a computed rectangle they never saw.

namespace np {

// `app/AppState.hpp`'s palette enum, forward-declared rather than included.
// This header is included BY `AppState.hpp` (it owns the `CropSession` member),
// so including it back would be a cycle -- the same arrangement
// `app/GradientTool.hpp` and `app/MeasureLine.hpp` already sit in. An opaque
// declaration is all `toolCropsCanvas()` needs; `CropTool.cpp` includes the
// real definition.
enum class Tool;

// Which shape the crop tool is drawing. Selectable in the options bar
// (`docs/ui.md` section 4b) -- the user's request was specifically for a mode
// there rather than a second palette cell, and section 4b's own test agrees:
// the crop reads no tip, so the four brush sliders would be live over nothing.
enum class CropMode {
  // Two points, an axis-aligned rectangle, `cropDocument()`. No resample: the
  // result is bit-exact.
  Rectangle,
  // Four free corners, `transformFromQuad()` + `transformDocument()`. One
  // reconstruction pass, and the picture is warped.
  Perspective,
};

// The band's own vocabulary for the two modes, one row per enum value, with
// the tooltip each needs. Same treatment and the same reason as
// `kGradientKinds` (`app/GradientTool.hpp` section 4): this table is the one
// list the MODE combo walks, so a mode added to the enum and not to the table
// is caught by `--selftest` rather than by a picker that silently offers one of
// two -- the `kToolMeta` failure in miniature, and that one shipped.
struct CropModeRow {
  CropMode mode;
  const char* label;
  const char* tip;
};
inline constexpr size_t kCropModeCount = 2;
extern const CropModeRow kCropModes[kCropModeCount];

const char* cropModeLabel(CropMode mode) noexcept;

// The four corners, in document texel space, in the fixed order **top-left,
// top-right, bottom-right, bottom-left OF THE OUTPUT**. An array with a stated
// meaning rather than four named fields, because every consumer walks them in
// a ring (the edge lengths, the turn tests, the handle hit-test) and named
// fields would make each of those a four-line unrolled thing that a later edit
// can get out of step.
struct CropQuad {
  std::array<Point2, 4> c{};

  friend bool operator==(const CropQuad&, const CropQuad&) = default;
};

// The axis-aligned quad naming `region`, in the corner order above. The bridge
// between the two modes: switching Rectangle -> Perspective keeps the shape the
// user already drew instead of throwing it away.
CropQuad cropQuadFromRegion(const DocumentRegion& region) noexcept;

// Section 2's rule: sorted, rounded outward, **not** clamped to the canvas.
// `x0/y0/x1/y1` are the drag's two corners in document texels, in either order.
DocumentRegion cropRegionFromDrag(float x0, float y0, float x1, float y1) noexcept;

// Section 3's rule: each dimension is the longer of its two opposite edges.
// Rounded to nearest and floored at one texel, so it is always a usable extent
// even for a quad `cropQuadRefusal()` is about to reject -- the caller that
// wants to know whether the quad is *meaningful* asks that function, not this
// one.
DocumentRegion perspectiveCropExtent(const CropQuad& quad) noexcept;

// Section 3, point 1, made assertable: the same rule computed by the *mean* of
// opposite edges. Not used by the tool. It exists so `--selftest` can state the
// relationship the design decision rests on -- `max >= mean` always, with
// equality exactly on a quad whose opposite edges match -- as a property of the
// two rules rather than as two hand-computed numbers that a later change to
// either would leave agreeing by luck.
DocumentRegion perspectiveCropExtentByMean(const CropQuad& quad) noexcept;

// A corner is refused as near-collinear when the sine of its interior turn is
// below this -- about 1.15 degrees. Not zero: `transformFromQuad()` goes
// singular at `|pivot| < 1e-12`, long after the solve has stopped being usable,
// and a quad that only *nearly* collapses comes back as a matrix that throws
// the far corner across the canvas. Named rather than inlined because
// `--selftest` asserts a quad on each side of it.
inline constexpr float kMinCornerSin = 0.02f;

// The edge-length ratio past which the resampler's centre-Jacobian filter
// choice (`ops/Transform.hpp` section 3) starts to show at the far edge. Not a
// refusal -- the crop is still correct, just progressively softer where it is
// most magnified -- so the options bar warns and commit stays live. Section 3's
// "do not silently exceed it".
inline constexpr float kSteepQuadEdgeRatio = 3.0f;

// Section 4's ladder. Returns an **empty string when the quad is usable**, and
// otherwise one sentence naming what is wrong, in this build's refusal voice
// (what was refused, then why, then what to do). Pure, cheap, and called every
// frame by the canvas block.
std::string cropQuadRefusal(const CropQuad& quad);

// `cropQuadRefusal(quad).empty()`, spelled as a predicate for the call sites
// that only branch on it. One function behind both, so a caller that draws the
// reason and a caller that greys the button can never disagree about whether
// there is one.
bool cropQuadIsUsable(const CropQuad& quad);

// True when the quad's opposite edges differ by more than `kSteepQuadEdgeRatio`
// on either axis -- the "this will be soft along the far edge" warning, not a
// refusal. False for any quad that is already refused, so the two messages
// never stack.
bool cropQuadIsSteep(const CropQuad& quad);

// The eighth canvas gate (section 6). `tool == Tool::Crop`, and nothing else --
// `Tool::Slice` shares its palette group and its cursor and is still unbuilt.
bool toolCropsCanvas(Tool tool) noexcept;

// --------------------------------------------------------------------------
// The gesture. Section 5.
// --------------------------------------------------------------------------
struct CropSession {
  // A shape is laid down and awaiting commit. Handles are live; outside is
  // darkened. Independent of `defining`: the initial drag sets both, and pen-up
  // clears `defining` while leaving this set.
  bool active = false;

  // The initial rubber-band drag, pointer down. `anchorX/anchorY` is where it
  // went down; the moving corner is the pointer.
  bool defining = false;
  float anchorX = 0.0f;
  float anchorY = 0.0f;

  // Which document this shape was drawn on, for `app/MeasureLine`'s reason: a
  // rectangle in one document's texels is meaningless in another's. Zero when
  // inactive.
  DocumentId doc = 0;

  CropMode mode = CropMode::Rectangle;

  // The shape, always as four corners even in Rectangle mode -- one preview
  // draw, one hit-test, one commit path, and a mode switch that keeps the shape
  // the user already drew. In Rectangle mode the four are held axis-aligned by
  // `cropDragHandle()` (moving one corner drags its two neighbours), so the
  // quad and `cropRegionOf()` agree by construction rather than by a second
  // rectangle field kept in step.
  CropQuad quad{};

  // T10's Space-move state for the initial rubber-band drag, reset by
  // `cropBeginDefine()`. The crop's defining drag is fed through the marquee's
  // own `computeSelectionDragBox()` -- one function, two tools -- so
  // Shift-constrain, Option-from-centre and Space-move behave identically here
  // and cannot drift from there. Space-move is the only one of the three that
  // needs stored state (`app/SelectionDrag.hpp`); the other two are pure
  // functions of this frame's modifiers and are reapplied fresh every frame.
  SelectionMoveState move;

  // Which handle the pointer is dragging: 0..3 for the four corners, 4..7 for
  // the four edge midpoints (Rectangle mode only -- a perspective quad's edges
  // are not axis-aligned and an edge handle on one would need a meaning nobody
  // has agreed), -1 for none.
  int dragHandle = -1;

  // Set by the options bar's CROP / CANCEL buttons, read and cleared by
  // `ui/MacPaintUI`'s crop block on the next frame.
  //
  // A request flag rather than the button committing where it is drawn, and for
  // the reason `g_imageSizeRequested` already has one translation unit over:
  // the canvas block owns the commit, so the button and the Enter key are ONE
  // commit path rather than two that agree until one of them is edited. It also
  // keeps `ui/AtelierChrome` -- which draws chrome -- out of the business of
  // resampling a document.
  bool commitRequested = false;
  bool cancelRequested = false;

  // Set by `--crop-demo`, the golden harness's way in. It pins `defining` open
  // across frames the way `--gradient-demo drag` pins the gradient's far
  // handle: a held drag's moving corner is the live mouse, and a screenshot
  // run's mouse is wherever the human left it.
  bool demoHeld = false;
};

inline constexpr int kCropHandleCount = 8;
inline constexpr int kCropCornerCount = 4;

// The handle centres, in document texels, in index order: corners 0..3 then
// edge midpoints 4..7 (4 = top, 5 = right, 6 = bottom, 7 = left). Edge handles
// are returned for a perspective quad too -- they are drawn only in Rectangle
// mode, and returning them unconditionally keeps this a pure function of the
// quad rather than of the mode.
std::array<Point2, kCropHandleCount> cropHandlePoints(const CropQuad& quad) noexcept;

// The handle within `radius` document texels of `(x, y)`, preferring corners
// over edges when both are in range (a corner is the more specific request and
// on a small rectangle the two overlap). `-1` for none. `mode` decides whether
// the four edge handles are offered at all.
int cropHandleAt(const CropQuad& quad, CropMode mode, float x, float y, float radius) noexcept;

// Move `handle` to `(x, y)`. In `CropMode::Rectangle` the quad is held
// axis-aligned -- a corner drags its two neighbours, an edge handle moves one
// side -- so the four corners never stop naming a rectangle. In
// `CropMode::Perspective` a corner moves alone and an edge handle is ignored.
//
// **No refusal here.** The quad is allowed to become a bow-tie mid-drag; it is
// drawn as one, with the reason, and commit is what refuses. A drag that
// snapped back at the moment it became invalid would fight the hand.
void cropDragHandle(CropSession& session, int handle, float x, float y) noexcept;

// The half-open region a Rectangle-mode session names. Meaningless -- and
// deliberately not offered -- for a perspective quad, whose output extent is
// `perspectiveCropExtent()` and whose origin is not in document space at all.
DocumentRegion cropRegionOf(const CropSession& session) noexcept;

// Switch modes, keeping the shape. Rectangle -> Perspective is the identity on
// the corners (a rectangle is a quad). Perspective -> Rectangle **snaps to the
// quad's bounding box**, which loses the corner placement and is the only
// lossy step in the tool; the options bar says so in the combo's tooltip rather
// than letting the shape silently change under the user.
void cropSetMode(CropSession& session, CropMode mode) noexcept;

// Start a rubber-band drag at `(x, y)` on `doc`, discarding whatever shape was
// there. Keeps `mode`: the mode is a tool setting, not a property of one crop.
void cropBeginDefine(CropSession& session, DocumentId doc, CropMode mode, float x,
                     float y) noexcept;

// Clear everything. `app/ToolSwitch` calls this, and so does Escape.
void cropCancel(CropSession& session) noexcept;

// --------------------------------------------------------------------------
// Commit, and the two menu items. Section 7.
//
// Each returns `ops/DocumentTransform`'s own `DocumentTransformResult`,
// **passed through rather than repackaged**. `app/FilterOps` flattens it into a
// two-field `DocumentOpOutcome` and that is right for a dialog, which has room
// for one sentence; it is wrong here, because the fields it drops are the ones
// this feature's assertions are made of. `reconstructionPasses` is 0 for a
// rectangle crop and 1 for a perspective crop, which is the difference between
// "moved the pixels" and "resampled them" stated as a number rather than
// inferred from which function was called; `previousWidth`/`previousHeight` are
// how a caller says what the crop did without holding the old document; and
// `lockedLayersMoved` is a count a user is entitled to be told. Reusing the
// engine's result type also means this file cannot invent a refusal wording the
// engine already has.
//
// Each records exactly one `EditKind::Structural` history entry on success,
// through `OpenDocument::recordEdit()` -- the undo funnel `ops/DocumentTransform`
// deliberately leaves to its caller -- using the engine's own `editLabel`, with
// exactly one override: `transformDocument()` answers "transform document",
// which is right for the engine and wrong in an Edit menu, so a perspective
// crop is relabelled "perspective crop". `applyCropRegion()` overrides nothing;
// `cropDocument()` already says "crop".
// --------------------------------------------------------------------------

// `cropDocument()` on `region`. Refuses an empty region, naming it. Records
// nothing for a region that is already the whole canvas, the same "a no-op the
// user asked for is not an edit" rule `applyImageSize()` states.
DocumentTransformResult applyCropRegion(OpenDocument& doc, const DocumentRegion& region);

// `transformFromQuad()` from `quad` onto `perspectiveCropExtent(quad)`'s
// rectangle at the origin, then `transformDocument()`. Refuses on
// `cropQuadRefusal()` first -- so the message the user gets at commit is the
// same sentence the canvas has been showing them -- and then on the engine's
// own refusal, which is kept rather than swallowed because the two are
// genuinely different failures.
DocumentTransformResult applyCropPerspective(OpenDocument& doc, const CropQuad& quad);

// The session's own commit: `applyCropRegion()` or `applyCropPerspective()`
// depending on `mode`, then `cropCancel()` on success. Refuses when the session
// is not active, or when it belongs to a different document than `doc` (the
// `DocumentId` carried on the session, section 5).
DocumentTransformResult applyCropSession(CropSession& session, OpenDocument& doc);

// The region Image > Crop to Selection would use: `core::selectionBounds()`
// intersected with the canvas. `std::nullopt` when there is no selection, or
// when it is empty, or when it covers nothing inside the canvas.
std::optional<DocumentRegion> cropToSelectionRegion(const OpenDocument& doc);

// The region Image > Trim to Content would use: the union of every layer's
// `core::layerContentBounds()`, intersected with the canvas so a trim can only
// ever shrink (section 7). `std::nullopt` for a document whose layers are all
// empty -- trimming to nothing is not a crop, it is a request to delete the
// document, and the menu item refuses it by name.
std::optional<DocumentRegion> trimToContentRegion(const Document& doc);

DocumentTransformResult applyCropToSelection(OpenDocument& doc);
DocumentTransformResult applyTrimToContent(OpenDocument& doc);

}  // namespace np
