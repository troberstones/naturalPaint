#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "app/DocumentLifecycle.hpp"
#include "core/Clipboard.hpp"
#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "core/SelectionMask.hpp"
#include "ops/DocumentTransform.hpp"
#include "ops/Transform.hpp"

// app/TransformSession (docs/reachability-audit.md C1; PRD D14, D16, E10).
//
// ops/Transform.hpp and ops/DocumentTransform.hpp are the resampler and its
// Document/Layer bridge. Both are built, tested, and had no caller: the
// application has no way to move, rotate or scale a layer, or the pixels
// under a selection. This file is the missing session -- the pure, in-memory
// state an interactive gizmo drags, with **no ui/ dependency of any kind**.
// The gizmo itself (drawing the handles, reading SDL's mouse/keyboard state)
// is a separate change to src/ui/MacPaintUI.cpp; nothing here touches it.
//
// ==========================================================================
// (1) A MATRIX, NOT A RUNNING RESAMPLE -- and why getting this backwards is
//     the classic bug in this feature
// ==========================================================================
//
// ops/Transform.hpp section 1 already makes the argument at length: composing
// matrices and resampling once, at commit, is not an optimisation, it is a
// correctness requirement (PRD D16), because a per-step resampler produces an
// image that is geometrically indistinguishable from the composed one and
// quietly worse -- every mouse-move frame would convolve the pixels again,
// and the damage does not undo when the user drags back.
//
// So `TransformSession::pending_` is a single `Mat3`, updated in place by
// every `updateDrag()` call, and the **only** place this file reads a source
// texel is `commit()`. A drag of a hundred frames costs a hundred 3x3 matrix
// multiplies; it does not touch a tile until the mouse comes up (or Return is
// pressed, or however the UI spells "commit"). `--selftest` proves this by
// counting reads of the source, not by inspecting the output -- see this
// header's sabotage-proof section 4 below and DrivenDrag's assertion.
//
// ==========================================================================
// (2) TWO TARGETS, NEVER BLURRED TOGETHER
// ==========================================================================
//
// `TransformTarget::Layer` -- the whole active layer: its pixel or pigment
// tiles, and its mask, moved together by `ops::transformLayer()`. Source
// bounds are `core::layerContentBounds()`.
//
// `TransformTarget::SelectionPixels` -- only the pixels the current selection
// covers, on the active layer. Source bounds are
// `ops::selectionContentRegion()`. See section 3 for what this does and does
// not do, and why.
//
// A session is one or the other for its whole lifetime -- `begin*()` sets it
// once, nothing here re-derives it from context, and there is no default: a
// caller that wants to know which one is dragging asks `target()`, never
// assumes.
//
// ==========================================================================
// (3) DECISION: A SELECTION-PIXELS TRANSFORM MOVES THE SELECTION WITH THE
//     CONTENT, AND ITS SCOPE
// ==========================================================================
//
// **The selection moves with the pixels.** core/SelectionMask.hpp documents a
// selection's primary role as a mask that weights an edit -- "every edit is
// refused everywhere" for an empty one, coverage read at the point of the
// edit. That rule says nothing about a selection being pinned in destination
// space once the edit itself *is* a move: on the contrary,
// core/LayerGeometry.hpp's own reason for moving a layer's mask alongside its
// pixels -- "a move that left the mask behind would slide content out from
// under its own coverage" -- applies unchanged to the active selection. A
// selection that stayed at the old location after its content moved would
// mean every subsequent action (a second transform, Delete, a fill) silently
// hits the wrong pixels, which is a worse trap than moving it. So `commit()`
// calls `ops::transformSelectionCoverage()` -- PRD E10's own machinery, built
// and unreached until now -- with the identical matrix and regions the pixel
// move uses, and replaces `OpenDocument::selection` with the result.
//
// **Scope: the selection's bounding box, not a per-pixel lift.** A `Selection`
// is antialiased coverage, and a Photoshop-style Free Transform of a
// non-rectangular marquee lifts *only* the covered texels, leaving the rest of
// the box's interior untouched. This file does not do that, and says so
// rather than approximating it: `core::cutThroughSelection()` gives a
// **coverage-weighted** clipboard (so a soft-edged selection's rim is
// correctly attenuated in what is cut), but everything this session moves is
// bounded by `selectionContentRegion()`'s rectangle, so a texel inside that
// box but outside the true selected shape moves too if it happens to sit in
// the same tile footprint the resample reads. Building a true per-pixel
// lift-and-inpaint-the-hole primitive is new engine machinery -- there is no
// "composite one TileStore over another, weighted by a mask" entry point
// anywhere in core/ or ops/ today, only whole-document compositing
// (core/Composite.hpp) and the coverage-weighted cut/clear pair this file
// already leans on. That is out of this task's brief ("build the model,
// don't reimplement the engine"), so it is named here as a real, bounded gap
// rather than silently producing a subtly wrong lift.
//
// **RGB layers only for this target.** See section 5 for why Pigment is
// refused here specifically, even though it is not refused for
// `TransformTarget::Layer`.
//
// **The layer's mask does not move with a selection-pixels transform.** A
// whole-layer transform moves pixels and mask together because they describe
// the same content (`ops/DocumentTransform.hpp` section 1). A
// selection-bounded lift has no equivalent "cut the mask through the same
// selection, move it, splice it back" primitive built, and adding one is the
// same class of new machinery section 3 already declined for the pixels
// themselves. Named, not silent: a masked layer's mask stays exactly where it
// was after a selection-pixels transform.
//
// ==========================================================================
// (4) DECISION: PIGMENT LAYERS
// ==========================================================================
//
// **A whole-layer transform of a Pigment layer is supported**, because
// ops/DocumentTransform.hpp section 2 already made this decision and this
// file inherits it rather than re-deciding it: `transformLayer()` resamples a
// Pigment layer mass-weighted, through the closed `LatentKernel` enum
// (Bilinear by default, no negative lobes, enforced by the type system, not a
// runtime check), because a resample is a linear combination of pixels and
// DESIGN-imaging.md says that stays valid on latents. Refusing it here would
// refuse the operation on `Layer::kind`'s own default.
//
// **A `TransformTarget::SelectionPixels` transform of a Pigment layer is
// refused, by name.** `beginSelectionPixels()` and `commit()` both check
// `layer.kind == LayerKind::Pigment` and refuse before touching anything. The
// reason is not section 3's kernel argument -- `core::cutThroughSelection()`
// already handles Pigment correctly, weighting **mass alone** and leaving the
// latent untouched (PRD F10's eraser rule, the same rule this file's `over`
// splice would have to invert). The reason is the *splice back*: once the
// moved content lands on top of whatever paint is still on the layer where it
// used to be a hole, recombining two overlapping films of pigment is not a
// straight alpha `over` -- it is this build's Kubelka-Munk mixing rule
// (`core/Pigment.hpp`'s deposit maths, `core::mixedPairTexel()` for the
// inter-layer case), which is deposit-time physics with no reusable entry
// point outside the brush path. Splicing pigment back with the RGB `over`
// formula below would be a silent, physically wrong merge -- a red glaze
// pasted over a still-wet blue one must not look like a straight blend of the
// two colours -- so this is refused instead, in `LayerEditResult`-style
// prose naming the reason, exactly as `ops/Transform.hpp` section 5 refuses
// what it does not decide.
//
// ==========================================================================
// (5) DECISION: THE DEFAULT KERNEL, AND WHY exactRemapKind() IS CALLED
//     EXPLICITLY RATHER THAN LEFT TO transformImage()
// ==========================================================================
//
// This file does not re-argue ops/Transform.hpp's own kernel choice --
// `DocumentTransformParams{}`'s defaults (Catmull-Rom for pixels/masks/
// selections, Bilinear for latents) are exactly what `commit()` uses unless a
// caller overrides them, for the reason already given there: Catmull-Rom is
// the sharpest kernel without Lanczos3's deepest negative lobes, which matter
// more on this build's scene-referred linear data than on display-referred
// content.
//
// What this file adds is calling `exactRemapKind(pending())` **itself**, via
// `pendingExactRemap()`, before any commit -- not because `transformImage()`
// needs the hint (it detects the exact path on its own), but because a UI
// wants to tell the user *while dragging* that a pure translate or a snapped
// 90-degree turn is about to be lossless, which is the whole point of PRD
// D15 existing as a user-visible guarantee and not just an internal fast
// path. A dropped image nudged one pixel to the right, or spun a quarter
// turn to fix its orientation, must not be silently blurred through
// Catmull-Rom because nothing told the session it did not need to be.
//
// ==========================================================================
// (6) HANDLES AND DRAG SEMANTICS ARE PURE FUNCTIONS OF (STATE, LIVE INPUT)
// ==========================================================================
//
// `transformHandlePositions()` and `hitTestTransformHandle()` take the
// session's own state (source bounds, pending matrix) and nothing from a
// caller but a cursor and a hit radius; `computeTransformDragMatrix()` takes
// a handle, the matrix as it stood **before this drag began**, the drag's own
// start cursor, and this frame's live cursor and modifiers. None of them read
// mutable session state directly -- `TransformSession`'s methods are thin
// wrappers that supply its own fields as arguments, so every one of these can
// be tested, and is tested, without constructing a session at all.
//
// **The SelectionDrag lesson, applied.** app/SelectionDrag.hpp shipped a
// gesture whose pure function was tested with argument combinations the call
// site cannot produce -- an offset varied while the cursor was held still,
// when the real call site derives the offset *from* that cursor -- and the
// unit test passed while the feature was broken. The hazard here is the same
// shape: `updateDrag()` is called once per real mouse-move frame with a live
// cursor and live modifier state, and frames are not independent -- Shift and
// Option are read **live, every call**, never latched at `beginDrag()`,
// matching the convention app/SelectionDrag.cpp's `computeSelectionDragBox()`
// established ("Shift/Option held during a drag change geometry"). So the
// drag state this file keeps is `{handle, matrix-at-drag-start, cursor-at-
// drag-start}`, fixed for the whole gesture, and every frame recomputes the
// **whole** pending matrix fresh from that triple plus the live cursor and
// live modifiers -- never an incremental multiply of frame N's result by
// frame N+1's delta, which is exactly the kind of accumulation PRD D16
// forbids for the resample and would additionally drift here from repeated
// floating-point composition. `--selftest`'s `runTransformSessionTest()`
// drives a realistic multi-frame loop -- begin, several moves, a modifier
// pressed mid-drag, release -- rather than only single calls with hand-picked
// arguments, for the identical reason app/selftest/SelectionDrag.cpp does.
//
// Per-handle semantics (`computeTransformDragMatrix()`):
//
//   **Move** -- translate by `curCursor - startCursor`, in destination space
//   (left-multiplied onto the base matrix, i.e. applied *after* it -- moving
//   an already-rotated box slides it along the canvas axes, not its own).
//
//   **Rotate** -- the signed angle between `startCursor` and `curCursor`
//   about the box's centre (mapped through the base matrix), left-multiplied
//   the same way as Move. No modifier reads it: the brief this file was
//   built against specifies Shift and Option for scale only, and this does
//   not invent a snap behaviour beyond that.
//
//   **The eight scale handles** -- both `startCursor` and `curCursor` are
//   mapped into SOURCE-LOCAL space through the base matrix's inverse, so the
//   ratio is computed in the box's own (possibly already rotated) frame
//   rather than in destination pixels, and the scale is then right-multiplied
//   onto the base matrix (applied *before* it, in source space -- resizing
//   the box about one of its own corners, not about the canvas origin). A
//   corner handle scales both axes independently by the ratio of the live
//   local offset from the anchor to the start local offset from the anchor;
//   an edge handle (top/bottom/left/right-center) scales one axis and leaves
//   the other at 1. **Option held** moves the anchor from the opposite
//   corner/edge to the box's own centre, read live, matching
//   SelectionDrag's convention exactly. **Shift held** locks the aspect: both
//   axes take whichever factor is furthest from 1 -- the scale-ratio analogue
//   of `computeSelectionDragBox()`'s "larger of the two deltas", which is
//   also what gives an edge handle a second, tied axis under Shift instead of
//   only ever moving one. That newly-tied axis anchors at the box's own
//   centreline on that axis (an edge handle was never given an anchor for
//   the axis it does not normally move), not at an edge -- so a Shift-locked
//   drag on TopCenter/BottomCenter grows the box symmetrically left-right
//   while still anchoring top-bottom at the opposite edge exactly as it does
//   without Shift, and MiddleLeft/MiddleRight is the same with the axes
//   swapped.
//
// ==========================================================================
// (7) COMMIT AND CANCEL
// ==========================================================================
//
// **Commit resamples exactly once** (section 1) and goes through the
// existing undo funnel, never around it: a `TransformTarget::Layer` commit
// calls `ops::transformLayer()` and records its `editLabel` through
// `OpenDocument::recordEdit()`, the identical path `app/LayerEditor.hpp`
// documents ("every mutation goes through recordEdit(), which is what makes
// it ... undoable" -- and, just as importantly, what bumps the revision
// `ui/DocumentTexture` caches the composite by, so a commit that bypassed it
// would resample pixels the screen never redraws). A
// `TransformTarget::SelectionPixels` commit orchestrates
// `core::cutThroughSelection()` (coverage-weighted lift + coverage-weighted
// erase, one call), `ops::transformRgbTiles()` (the one resample), a
// straight premultiplied-`over` splice of the moved tiles back onto what the
// cut left behind, and `ops::transformSelectionCoverage()` for the marquee --
// then records ONE `recordEdit()` for the whole thing, so undo takes the cut,
// the move and the splice back in a single step.
//
// **An identity pending transform commits as a no-op**: nothing is written,
// nothing is recorded. `TransformStack`'s own rule is "an empty stack
// composes to the identity ... a transform tool with no edits yet must be a
// no-op"; this file extends that one step further, because commit is the one
// place that decides whether a no-op transform reaches the document at all --
// letting it through would give every "opened the tool, changed nothing,
// clicked away" gesture its own undo step.
//
// **Cancel needs no restore step, because nothing was written until
// commit.** `pending_` lives only in this object; cancelling is resetting the
// session to inactive. The document, the layer, the selection -- none of them
// were touched, which is the entire point of section 1's "one matrix, one
// resample at the end" design: there is no partial state to unwind.
namespace np {

enum class TransformTarget { Layer, SelectionPixels };

// One of the eight box handles, the rotation affordance, or a drag on the
// box's own body (Move). `None` is "not over anything".
enum class TransformHandle {
  None,
  Move,
  Rotate,
  TopLeft,
  TopCenter,
  TopRight,
  MiddleLeft,
  MiddleRight,
  BottomLeft,
  BottomCenter,
  BottomRight,
};

// Every handle's current position, in DOCUMENT space -- i.e. already mapped
// through the pending matrix. Pure data; the UI draws these, it does not
// derive them.
struct TransformHandlePositions {
  Point2 topLeft, topCenter, topRight;
  Point2 middleLeft, middleRight;
  Point2 bottomLeft, bottomCenter, bottomRight;
  Point2 rotate;
  Point2 center;
};

// A reasonable rotate-handle reach at 1:1 zoom, in document pixels. A caller
// that knows its current zoom should pass its own value (screen px / zoom) so
// the handle keeps a constant ON-SCREEN distance regardless of how far the
// canvas is zoomed in -- this file has no notion of zoom at all, deliberately
// (see this header's section 6), so it cannot make that correction itself.
inline constexpr float kDefaultRotateHandleReach = 24.0f;

// The eight box handles plus the rotate affordance, purely as a function of
// `sourceBounds` (the untransformed source rectangle, document space) and
// `pending` (the accumulated matrix). The rotate handle sits `rotateReach`
// source-space units above the top edge's centre, in LOCAL space, before
// being mapped through `pending` -- so it turns and scales with the box
// exactly like every other handle, rather than staying screen-axis-aligned.
TransformHandlePositions transformHandlePositions(
    const DocumentRegion& sourceBounds, const Mat3& pending,
    float rotateReach = kDefaultRotateHandleReach) noexcept;

// Which handle (if any) `cursor` is within `handleRadius` of, checking the
// rotate handle and the eight box handles before falling back to an
// inside-the-box test for `Move`. The inside test maps `cursor` back into
// source-local space through `pending`'s inverse and compares against the
// axis-aligned `sourceBounds` there -- exact for any invertible `pending`,
// including a rotation, and simpler than a rotated-polygon test in
// destination space. Returns `None` for a non-invertible `pending` (nothing
// to hit-test against) or a cursor outside everything.
TransformHandle hitTestTransformHandle(const TransformHandlePositions& handles,
                                       const DocumentRegion& sourceBounds, const Mat3& pending,
                                       Point2 cursor, float handleRadius) noexcept;

// One frame of a drag: the new pending matrix, computed fresh from
// `baseMatrix` (the pending matrix as it stood when this drag began) plus
// this frame's live cursor and live modifiers. See this header's section 6
// for the semantics of each handle and why `shiftHeld`/`optionHeld` are read
// live rather than latched. Returns `baseMatrix` unchanged for `None`.
Mat3 computeTransformDragMatrix(TransformHandle handle, const DocumentRegion& sourceBounds,
                                const Mat3& baseMatrix, Point2 startCursor, Point2 curCursor,
                                bool shiftHeld, bool optionHeld) noexcept;

// The initial `pending()` a freshly-dropped layer's transform session should
// start from, when the dropped image overflows the canvas: a PROPORTIONAL
// (aspect-preserving) scale-to-fit, centred on the canvas, computed from
// `sourceBounds` (the layer's own content bounds -- the image at native
// pixel size, wherever `writeDecodedImageIntoLayer()` put it) and `canvas`
// (`documentCanvasRegion(doc)`).
//
// Returns `mat3Identity()`, UNCHANGED, whenever the image already fits both
// dimensions -- no forced upscale, and no seeded transform at all for
// content that already fit before this feature existed. `scale` is
// `min(canvas.width / sourceBounds.width, canvas.height / sourceBounds.height)`
// (whichever axis is more oversize governs both, so the image is never
// cropped) applied about `sourceBounds`'s own origin and then re-centred, so
// the result is a box of size `sourceBounds * scale` sitting in the middle
// of `canvas` regardless of where `sourceBounds` itself started -- see
// app/OpenAnyFile.cpp's drop path (this build's one caller) for why "centred
// on canvas" rather than "centred on the drop point" is the right read of a
// pending gizmo the user has not yet touched.
//
// A pure function, like every other builder in this file's section 6 --
// `main.cpp`'s drop handler calls it once, right before `beginLayer()`, and
// `--selftest` drives it directly with no session or document at all.
Mat3 computeDropFitTransform(const DocumentRegion& sourceBounds,
                             const DocumentRegion& canvas) noexcept;

// The absolute matrix a numeric-entry transform dialog's fields describe,
// composed the same order Photoshop's own numeric transform box uses when
// both scale and rotation are non-identity: scale first, in the ORIGINAL
// (unrotated) axes, about `pivot`; then rotate about the same `pivot`; then
// an independent translate on top, which does not itself depend on `pivot`
// at all. `scaleXFraction`/`scaleYFraction` are fractions (1.0 == 100%, i.e.
// no change), `rotateDegrees` is clockwise. All-defaults
// (0 degrees, 1.0, 1.0, 0, 0) returns exactly `mat3Identity()`.
Mat3 composeNumericTransform(float rotateDegrees, float scaleXFraction, float scaleYFraction,
                             float translateX, float translateY, Point2 pivot) noexcept;

struct TransformBeginResult {
  bool ok = false;
  std::string error;
};

// What `commit()` reports. `exact`/`reconstructionPasses` mirror
// `LayerTransformResult`'s own fields (ops/DocumentTransform.hpp) -- the PRD
// D16 witness, so a caller (or --selftest) can assert *how* a commit resampled,
// not only that the pixels came out right.
struct TransformCommitResult {
  bool ok = false;
  std::string error;
  std::string editLabel;
  ExactRemap exact = ExactRemap::None;
  int reconstructionPasses = 0;
};

// The session. One instance per in-progress interactive transform; a UI holds
// one, constructs it fresh (or calls `cancel()`) between gestures.
class TransformSession {
 public:
  bool active() const noexcept { return active_; }
  TransformTarget target() const noexcept { return target_; }
  size_t layerIndex() const noexcept { return layerIndex_; }
  const DocumentRegion& sourceBounds() const noexcept { return sourceBounds_; }
  const Mat3& pending() const noexcept { return pending_; }

  // The selection this session is transforming, or `nullptr` for
  // `TransformTarget::Layer` (section 2: a session is one target or the
  // other, never both). A caller that wants to read exactly what a
  // `SelectionPixels` commit will read -- ui/TransformPreviewTexture does,
  // to preview it -- must use THIS snapshot rather than the document's live
  // selection: `beginSelectionPixels()`'s own doc comment is why the two can
  // differ mid-drag, and reading the live one here would preview a
  // different region than commit() is actually going to touch.
  const Selection* selectionSnapshot() const noexcept {
    return target_ == TransformTarget::SelectionPixels ? &selectionSnapshot_ : nullptr;
  }

  // What PRD D15's exact path says about the transform as it stands right
  // now -- see this header's section 5 for why a UI wants this live, not only
  // at commit.
  ExactRemap pendingExactRemap() const noexcept { return exactRemapKind(pending_); }

  // Sets pending() directly to `m`. The one caller is the numeric Transform
  // dialog (ui/MacPaintUI.cpp's drawNumericTransformDialog()), which computes
  // the WHOLE matrix itself from its own fields every time one changes, rather
  // than deriving a delta from a drag the way beginDrag()/updateDrag() do --
  // so this is that session's own explicit-update path, not a bypass of the
  // "no setter" rule beginLayer()'s own comment states: it is exactly as
  // legitimate an operation as a drag, just driven by typed numbers instead of
  // a mouse. A no-op when no session is active, matching updateDrag()'s own
  // "no session, no effect" contract -- there is no pending() to set.
  void setPending(const Mat3& m) noexcept {
    if (active_) pending_ = m;
  }

  bool dragging() const noexcept { return drag_.active; }

  // Begins a transform of `doc.layers[layerIndex]`'s own pixels (and, at
  // commit, its mask). Refuses a locked layer or an out-of-range index by
  // name, and refuses a layer with no content to transform -- either no
  // pixel storage at all (Adjustment/Text/Strokes/Flats/Media/Group) or
  // storage that is empty. `sourceBounds()` becomes
  // `core::layerContentBounds()`. `pending()` starts at `initialPending`,
  // which defaults to identity -- the ordinary case, e.g. Cmd+T on a layer
  // that is already where the user wants it to start from. A caller that
  // already knows this fresh layer needs a non-identity starting point
  // (app/OpenAnyFile.cpp's drop path, via `computeDropFitTransform()` above,
  // for an oversize dropped image) passes it here rather than mutating
  // `pending()` after the fact. (Section 1's rule was once "no setter, on
  // purpose" -- `setPending()` below is now that setter, for the one caller,
  // the numeric Transform dialog, that legitimately needs to replace the
  // whole matrix outside a drag; every other caller should still prefer
  // `initialPending` at `begin*()` or a drag, not this.)
  TransformBeginResult beginLayer(const Document& doc, size_t layerIndex,
                                  const Mat3& initialPending = mat3Identity());

  // Begins a transform of the pixels `selection` covers on
  // `doc.layers[layerIndex]`. Refuses a locked layer, an out-of-range index,
  // a Pigment layer (this header's section 4), a layer with no RGB storage,
  // or a selection that covers nothing. `sourceBounds()` becomes
  // `ops::selectionContentRegion(selection)`. A copy of `selection` is kept
  // for `commit()`, so a change to `doc`'s live selection after `begin`
  // (which nothing in a headless session should cause mid-drag) does not
  // retarget an in-progress transform.
  TransformBeginResult beginSelectionPixels(const Document& doc, const Selection& selection,
                                            size_t layerIndex);

  TransformHandlePositions handlePositions(
      float rotateReach = kDefaultRotateHandleReach) const noexcept;
  TransformHandle hitTest(Point2 cursor, float handleRadius,
                          float rotateReach = kDefaultRotateHandleReach) const noexcept;

  // Latches `handle` and `startCursor` and snapshots `pending()` as this
  // drag's baseline. Call once per mouse-down on a handle.
  void beginDrag(TransformHandle handle, Point2 startCursor) noexcept;

  // Recomputes `pending()` for this frame. Call once per mouse-move for the
  // whole duration of the drag, with this frame's live cursor and live
  // modifier state -- see this header's section 6. A no-op if no drag is
  // active.
  void updateDrag(Point2 curCursor, bool shiftHeld, bool optionHeld) noexcept;

  // Ends the drag. `pending()` already holds the released state; this only
  // clears `dragging()`.
  void endDrag() noexcept;

  // Resamples once and writes the result into `od`, through the existing
  // undo funnel (this header's section 7). On success, `active()` becomes
  // false. On refusal, `od` is untouched and `active()` stays true, so the
  // caller can let the user adjust the transform and try again.
  TransformCommitResult commit(OpenDocument& od,
                               const DocumentTransformParams& params = DocumentTransformParams{});

  // Discards the session. Nothing was ever written to a document, so this is
  // exactly resetting this object to its default state.
  void cancel() noexcept;

 private:
  bool active_ = false;
  TransformTarget target_ = TransformTarget::Layer;
  size_t layerIndex_ = 0;
  DocumentRegion sourceBounds_;
  Mat3 pending_ = mat3Identity();
  Selection selectionSnapshot_;  // only meaningful for SelectionPixels

  struct DragState {
    bool active = false;
    TransformHandle handle = TransformHandle::None;
    Point2 start{};
    Mat3 base;
  };
  DragState drag_;
};

}  // namespace np
