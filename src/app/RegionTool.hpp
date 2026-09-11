#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "app/Command.hpp"   // CommandResult -- the commits go through applyCommand()
#include "app/CropTool.hpp"  // cropRegionFromDrag() -- this file's own §1
#include "app/DocumentLifecycle.hpp"
#include "core/Region.hpp"
#include "core/RegionOps.hpp"
#include "ops/Transform.hpp"  // Point2

// app/RegionTool -- `Tool::Frame` and `Tool::Slice`, one headless gesture
// module for both (docs/ui.md §4a: "Frame / Artboard" in Move's palette
// group, Slice in Crop's).
//
// ==========================================================================
// (1) NOTHING HERE REBUILDS RECTANGLE-DRAG ARITHMETIC THAT ALREADY EXISTS
// ==========================================================================
//
// This build already has two independently-tested answers to "what
// rectangle does a drag between two document-texel points name":
//
//   * `app/SelectionDrag::computeSelectionDragBox()` folds in Shift-square
//     and Option-from-centre, and is what the marquee and the Crop tool's
//     rectangle mode are both driven through in `ui/MacPaintUI.cpp`.
//   * `app/CropTool::cropRegionFromDrag()` takes that box's two corners and
//     produces a half-open `ops::DocumentRegion`, sorted and rounded
//     outward, **not** clamped to the canvas.
//
// A Frame or Slice drag is the identical question asked a third time, so
// this file calls both rather than re-deriving either. The one place the
// answer differs from Crop's is intent, not arithmetic: Crop's outward
// rounding is argued as "a destructive op should guess in favour of keeping
// pixels" (app/CropTool.hpp §2) -- a region is not destructive at all, but
// reusing the same rounding rule is worth more than inventing a technically-
// more-defensible one, because it means a user who has learned one
// rectangle tool's corner behaviour in this build gets the same behaviour
// from the next.
//
// **A corner-drag resize reuses the same function again.** Resizing a
// region by a corner is "the rectangle spanning the FIXED opposite corner
// and the live pointer", which is exactly `cropRegionFromDrag()`'s
// signature -- two document-texel points, in either order. So this file has
// exactly one rectangle-from-two-points call site, used for both the
// initial drag and every subsequent resize.
//
// ==========================================================================
// (2) THE GESTURE (brief's item 2)
// ==========================================================================
//
// One `RegionSession`, driven one frame at a time by the canvas block,
// mirroring `app::CropSession`'s shape: it carries the `DocumentId` it is
// active on (a rectangle in one document's texels means nothing in
// another's), and it is a plain `enum class` state machine rather than
// several independent booleans, so "moving and resizing at once" is not a
// state this type can represent by construction.
//
//   `Idle`      nothing in progress. `selectedId` may still name a selected
//               region (handles are drawn, Delete acts on it) -- selection
//               outlives a single gesture, drag does not.
//   `Defining`  the initial rubber-band drag on empty canvas.
//   `Moving`    dragging the selected region as a whole.
//   `Resizing`  dragging one of its four corners.
//
// **One history entry per completed gesture** (the brief's own words):
// every commit function below builds exactly one command
// (app/CommandsRegions.hpp) and hands it to `app::applyCommand()`, which
// appends the `core::History` entry AND is where the recorder taps -- so a
// Frame drawn by hand records as the `add_region` a hand-written action
// would contain. There is no per-frame commit anywhere in this file, only a
// per-frame *preview* rectangle a caller may choose to draw.
//
// **These functions ARE the UI's boundary into the command layer for
// regions** (docs/automation.md §2.3's "a fourth door"): headless, at file
// scope, so `--selftest` calls the same function the canvas block does and
// can arm a `Recorder` around it. The options row's name field and delete
// button go through `regionRenameSelected()` / `regionDeleteSelected()` for
// the same reason, rather than calling `core::renameRegion()` from inside a
// widget and recording nothing.
//
// **A gesture that changed nothing commits nothing.** Clicking a region to
// select it is a `Moving` gesture that ends where it began; committing that
// would put an undo step and a recorded `move_region` on every selection
// click. So a move to the same origin, a resize to the same rectangle, and a
// click on empty canvas all return `ok == false` with an EMPTY status -- "not
// an edit", which a caller must not surface as a refusal.
//
// **Shift = square; a click without a drag creates nothing** (the brief's
// own words): the square constraint is `computeSelectionDragBox()`'s
// `constrainSquare` flag, threaded through by the caller exactly as the
// marquee and Crop already do it; "creates nothing" falls out of
// `core::addRegion()`'s own empty-rectangle refusal (core/RegionOps.hpp) --
// a click's degenerate zero-size drag is refused there, not specially
// detected here.
//
// **What this file does NOT own**: which modifier keys are down (a session
// concern read every frame in `ui/MacPaintUI.cpp`, exactly as Crop's own
// Shift/Option handling is), and Space-move mid-drag (`SelectionMoveState`)
// -- brief item 2 does not ask for repositioning the anchor mid-drag, and
// adding it would be inventing a requirement rather than meeting one.
namespace np {

enum class Tool;  // app/AppState.hpp's palette enum; see app/CropTool.hpp's
                   // own forward-declare note for why this header does not
                   // include it back.

// The ninth and tenth canvas gates (after `toolCropsCanvas()`, the eighth):
// `tool == Tool::Frame || tool == Tool::Slice`.
bool toolCreatesRegions(Tool tool) noexcept;

// `RegionKind::Frame` for `Tool::Frame`, `RegionKind::Slice` for
// `Tool::Slice`. Precondition: `toolCreatesRegions(tool)`; any other tool
// returns `RegionKind::Frame` (the enum's own default) because there is no
// meaningful answer, not because Frame is being asserted for it -- callers
// gate on `toolCreatesRegions()` first.
RegionKind regionKindForTool(Tool tool) noexcept;

// The `Tool` a region's own kind implies, for a caller that wants to switch
// tools when a region of the other kind is clicked. The inverse of
// `regionKindForTool()`.
Tool toolForRegionKind(RegionKind kind) noexcept;

// --------------------------------------------------------------------------
// Lookups. Pure functions of a `Document`, so `--selftest` and the UI share
// one answer.
// --------------------------------------------------------------------------

// `nullptr` if no region has this id -- deleted, or never existed. An id and
// not an index: `RegionSession::selectedId` survives a delete of some other
// region without silently starting to name the wrong one.
const Region* findRegionById(const Document& doc, uint64_t id) noexcept;

// `doc.regions.size()` (one past the end, `core::LayerOpResult::index`'s own
// "not found" convention) when no region has this id.
size_t indexOfRegionId(const Document& doc, uint64_t id) noexcept;

// The region of `kind` under `(x, y)`, preferring the most recently added
// one where two overlap -- the same "last in the list is what a click sees
// first" rule `app/CropTool`'s handle preference and every z-ordered pick in
// this codebase already use. `nullptr` for no match.
const Region* regionAt(const Document& doc, RegionKind kind, float x, float y) noexcept;

// The four corner handle centres, in document texels, in `CropQuad`'s own
// fixed order: top-left, top-right, bottom-right, bottom-left.
std::array<Point2, 4> regionHandlePoints(const Region& region) noexcept;

// The handle within `radius` document texels of `(x, y)`, or -1. `CropTool`'s
// own preference rule does not apply here (a region has no edge handles,
// only the four corners), so this is a plain nearest-within-radius scan.
int regionHandleAt(const Region& region, float x, float y, float radius) noexcept;

// --------------------------------------------------------------------------
// The gesture.
// --------------------------------------------------------------------------

enum class RegionGesture {
  Idle,
  Defining,
  Moving,
  Resizing,
};

struct RegionSession {
  DocumentId doc = 0;
  RegionGesture gesture = RegionGesture::Idle;

  // `Defining`: the drag's anchor, unmodified by Shift/Option -- fed to
  // `computeSelectionDragBox()` exactly as the marquee's own anchor is.
  float anchorX = 0.0f;
  float anchorY = 0.0f;

  // `Moving`: the pointer's offset from the selected region's origin at
  // gesture start, so the shape does not jump to be centred under the
  // cursor the instant the drag begins.
  float grabOffsetX = 0.0f;
  float grabOffsetY = 0.0f;

  // `Resizing`: which corner is being dragged (`regionHandlePoints()`'s
  // index) and the FIXED opposite corner, in document texels -- so the live
  // rectangle is recomputed every frame as `cropRegionFromDrag(fixedX,
  // fixedY, pointerX, pointerY)`, matching `CropSession::quad`'s own
  // "corners are held live, not accumulated as a delta" design.
  int resizeCorner = -1;
  float fixedX = 0.0f;
  float fixedY = 0.0f;

  // The selected region's id, or 0 for none. Outlives a gesture: selecting a
  // region and then doing nothing else still shows its handles and answers
  // Delete.
  uint64_t selectedId = 0;

  // `--region-demo`'s pin, `CropSession::demoHeld`'s exact twin: held open
  // across frames so a screenshot run's live mouse (wherever the human left
  // it) does not end the gesture the instant the demo flag stops forcing it.
  bool demoHeld = false;
};

// Deselects, and starts a `Defining` drag of `kind` at `(x, y)` on `doc`.
void regionBeginDefine(RegionSession& session, DocumentId doc, RegionKind kind, float x,
                       float y) noexcept;

// The live rectangle a `Defining` session names this frame -- for the
// canvas's preview draw, not a commit. `curX/curY` is the live pointer;
// `square`/`fromCentre` are this frame's modifier reads, threaded straight
// into `computeSelectionDragBox()`.
DocumentRegion regionDefineRect(const RegionSession& session, float curX, float curY, bool square,
                                bool fromCentre) noexcept;

// Ends a `Defining` drag: `add_region` on `regionDefineRect()`'s rectangle,
// through `applyCommand()`. Selects the new region on success. Resets
// `session` to `Idle` either way. A click without a drag names an empty
// rectangle (`cropRegionFromDrag()` keeps a degenerate axis degenerate) and
// sends no command at all -- "creates nothing", not a refusal.
CommandResult regionCommitDefine(RegionSession& session, OpenDocument& od, RegionKind kind,
                                 float curX, float curY, bool square, bool fromCentre);

// Starts a `Moving` drag of the currently selected region at `(x, y)` --
// caller's job to have already confirmed `(x, y)` is inside it (`regionAt()`
// then a bounds check, or a caller that already knows). Refuses silently
// (returns without changing `session`) if `session.selectedId` does not name
// a region in `doc` -- a stale selection from a document that has since had
// the region deleted elsewhere.
void regionBeginMove(RegionSession& session, const Document& doc, float x, float y) noexcept;

// The live origin a `Moving` session names this frame, rounded to the
// nearest texel.
void regionMoveOrigin(const RegionSession& session, float curX, float curY, int32_t* outX,
                      int32_t* outY) noexcept;

// Ends a `Moving` drag: `move_region` to `regionMoveOrigin()`'s origin.
// Nothing is sent when the origin did not change (a selection click). Resets
// `session.gesture` to `Idle` (selection is kept).
CommandResult regionCommitMove(RegionSession& session, OpenDocument& od, float curX, float curY);

// Starts a `Resizing` drag on the selected region's corner `handle` (0..3).
void regionBeginResize(RegionSession& session, const Document& doc, int handle) noexcept;

// The live rectangle a `Resizing` session names this frame.
DocumentRegion regionResizeRect(const RegionSession& session, float curX, float curY) noexcept;

// Ends a `Resizing` drag: `resize_region` to `regionResizeRect()`'s
// rectangle. Refused, by name (from the command row), if the corner was
// dragged onto its fixed opposite on either axis -- the zero-size case.
// Nothing is sent when the rectangle did not change. Resets
// `session.gesture` to `Idle` either way.
CommandResult regionCommitResize(RegionSession& session, OpenDocument& od, float curX, float curY);

// Deletes the selected region, if any, through `delete_region`.
// `ok == false` with an empty status (not a refusal to surface) when
// nothing is selected -- a Delete keypress with no region selected is not a
// mistake to report, it is simply not this tool's business right now.
CommandResult regionDeleteSelected(RegionSession& session, OpenDocument& od);

// Renames the selected region through `rename_region` -- the options row's
// name field. The name that lands may carry a uniquifying suffix
// (`core::uniqueRegionName()`); read it back off the document. `ok == false`
// with an empty status when nothing is selected or the name is unchanged.
CommandResult regionRenameSelected(const RegionSession& session, OpenDocument& od,
                                   const std::string& newName);

// Cancels whatever gesture is in progress without committing. Does not
// clear `selectedId` -- `app/CropTool::cropCancel()`'s own asymmetry does
// not apply here, because unlike a pending crop shape a selected region is
// not a destructive act waiting to happen; there is nothing unsafe about
// leaving it selected. Called by `app/ToolSwitch` on leaving Frame/Slice,
// and by Escape.
void regionCancelGesture(RegionSession& session) noexcept;

}  // namespace np
