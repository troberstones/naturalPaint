#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "core/Path.hpp"          // PathPoint, PathBounds
#include "core/TextContent.hpp"   // TextContent

// app/TextTool -- the headless core of PLAN.md phase 14's Text tool (PRD
// K1-K3): the edit session for a `LayerKind::Text` layer's caret and its
// paragraph-frame drag, plus the gate predicate.
//
// ==========================================================================
// 1. WHAT IS AND IS NOT HERE
// ==========================================================================
//
// Everything below edits a `TextContent` (core/TextContent.hpp) and a small
// session struct: no ImGui, no GPU, no `AppState`, no shaping. This is the
// same relationship `app/PenTool.hpp` -- this task's own worked example --
// has to `ui/MacPaintUI.cpp`'s canvas gesture block, and it is deliberate for
// the same reason: a headless core can be sabotage-tested in `--selftest`
// with no window, no font, and no platform shaper.
//
// **This file calls none of `core/TextContent.hpp`'s own free functions**
// (`textContentToShapes()`, `textContentBounds()`, `textContentHash()`,
// `makeTextContent()`). Their `.cpp` is a sibling track's deliverable and does
// not exist yet in this tree, so a call here would fail to link; the header
// is included only for the `TextContent` struct's fields, which this file
// edits directly by assignment. `textBlockHit()` below takes bounds as a
// PARAMETER rather than calling `textContentBounds()` itself for exactly this
// reason -- and, once that function exists, it remains the better shape: the
// caller already has the bounds (it drew the block from them last frame), and
// re-shaping the text a second time just to hit-test a click would be wasted
// work on every pointer move.
//
// **Not here, on purpose:**
//
//   * The `TextEditState` MEMBER. The struct is declared at the bottom of
//     this header, because its transitions are defined here and a caller
//     needs the type; the member itself lives on `AppState` beside
//     `pathEdit`/`gradientDrag`, per `app/PenTool.hpp` section 8's own
//     precedent. This file declares no `AppState` member and touches no file
//     under `ui/`.
//   * Drawing, cursor mapping, the options bar, and turning a finished frame
//     drag into an actual new layer. `ui/MacPaintUI.cpp`'s canvas gesture
//     block is a thin caller of the transitions below, the string edits, and
//     `textBlockHit()`; it owns the undo-entry bookkeeping (`recordEdit()` /
//     `amendEdit()`) that `TextEditState::undoOpened` exists to let it drive
//     without a second copy of the same fact.
//
// ==========================================================================
// 2. THE GATE
// ==========================================================================
//
// `toolEditsText()` is true for exactly `Tool::Text`. Unlike `toolEditsPath()`
// -- true for the Pen/Curve PAIR, because both author the same anchor model --
// there is only one tool that edits a `TextContent`, so this predicate has no
// "which of a small group" question to answer; it exists at all so a caller
// checking "am I in text-edit mode" spells the same intent everywhere rather
// than each site re-writing `tool == Tool::Text`.
//
// **This is a term `toolHasCanvasHandler()` will eventually gate on, and it
// is NOT wired in yet.** `toolHasCanvasHandler()` and `Tool::Text`'s
// `kToolMeta` row (`implemented`) both live in `src/ui/AtelierChrome.cpp`,
// which this track does not own -- four tracks are building this tool's
// pieces in parallel, and the canvas gesture block that actually calls this
// file's transitions is the integrator's, after all four land. Flipping only
// one of the pair -- `implemented = true` with no handler, or a handler with
// `implemented` still false -- is exactly what `app/selftest/Eyedropper.cpp`'s
// tripwire exists to catch: it asserts `toolImplemented(t) ==
// toolHasCanvasHandler(t)` for every `Tool`, with a second assertion that
// `toolNoHandlerException()` holds zero rows, so the tempting stopgap (an
// exception-table row for `Tool::Text`) is exactly the row that second
// assertion keeps empty. `app/PenTool.hpp` section 2 hit this identical
// situation for `Tool::Pen`/`Tool::Curve` and the fix was the same shape: both
// halves flip together, in the integrator's commit, not in this one. Until
// then this predicate is tested here and unreferenced by `ui/`.
namespace np {

// This header is included BY `AppState.hpp` (which will own the
// `TextEditState` member below), so including it back would be a cycle --
// the same arrangement `app/PenTool.hpp`, `app/CropTool.hpp` and
// `app/GradientTool.hpp` already sit in. An opaque declaration is all
// `toolEditsText()` needs; `TextTool.cpp` includes the real definition.
enum class Tool;

bool toolEditsText(Tool t) noexcept;

// The session type, defined in full at section 5 below (its transitions are
// the reason this header exists, so they need the type before they need its
// members). A forward declaration is enough for every pointer/reference
// parameter in section 3 and section 4.
struct TextEditState;

// ==========================================================================
// 3. THE STRING EDITS -- UTF-8 BYTE OFFSETS, NEVER MID-SEQUENCE
// ==========================================================================
//
// `TextContent::utf8` is UTF-8 (core/TextContent.hpp's own comment: "not
// validated here"), and `TextEditState::caret` is a BYTE offset into it, not
// a character index -- the same unit `text/Shaper.hpp`'s `ShapedGlyph::
// cluster` uses, so a caret position and a shaped glyph's cluster are
// directly comparable with no conversion, which is what a future
// click-to-place-caret needs from `textBlockHit()`'s sibling functions.
//
// **The caret must never land inside a UTF-8 sequence.** A byte offset that
// splits "é" (0xC3 0xA9) leaves a lone continuation byte on one side and an
// orphaned lead byte on the other -- both invalid UTF-8 on their own -- and
// `text/Shaper.hpp::shapeText()` refuses a string that is not valid UTF-8
// OUTRIGHT (`ok == false` for the whole block), not just for the malformed
// tail. So a one-BYTE backspace on the boundary between "café" and the "!" a
// user just typed after it does not delete an accent: it corrupts the entire
// text layer's rendering. Every operation below routes its caret through one
// clamp (`TextTool.cpp`'s internal `clampToBoundary()`) before doing
// anything, so an offset that arrived out of range or mid-sequence -- most
// concretely, a `TextEditState` whose `caret` was set against one
// `TextContent` and is now being used against a DIFFERENT one after an
// external edit -- always resolves to a real character boundary rather than
// being trusted.
//
// **What an already-corrupt string does to these operations.** Nothing here
// validates `utf8` on the way in (that is `shapeText()`'s job, per
// core/TextContent.hpp), and a string that was already invalid before this
// file ever touched it -- an imported file with a truncated multi-byte tail,
// say -- has no boundary these functions can discover that is any more
// "correct" than another. The chosen behaviour is to degrade to single-byte
// steps AT THE POINT OF CORRUPTION rather than refuse to edit or guess a
// boundary that might not exist: a lead byte this file cannot classify (a
// stray continuation byte, or an invalid 0xF5-0xFF byte) is treated as its
// own one-byte character. That keeps every operation terminating and
// in-bounds, and keeps corruption from spreading past where it already was --
// but it does NOT promise the result is valid UTF-8 if the input was not.
// Silently "fixing" a corrupt string by, say, dropping the offending bytes
// was considered and rejected: that is a content change the user never asked
// for, and refusing to edit a corrupt string at all would leave a user unable
// to fix a typo next to the very corruption they are trying to remove.
// **For VALID input, every operation below is guaranteed to leave `utf8`
// valid UTF-8 and `caret` on a boundary.**

// Insert UTF-8 at the caret, advancing it past what was inserted. `utf8` is
// trusted to be valid UTF-8 itself (a caller assembling one code point from a
// keystroke, or pasting clipboard text) -- validating a fragment being
// inserted is a different problem from clamping a caret, and is IME/paste
// input handling this file has no reason to own.
void textInsertUtf8(TextContent* text, TextEditState* state, std::string_view utf8);

// Delete the CHARACTER before the caret -- one whole code point's worth of
// bytes, not one byte. Returns false (edits nothing) when the caret is
// already at the start.
bool textBackspace(TextContent* text, TextEditState* state);

// Delete the character AFTER the caret. Returns false (edits nothing) when
// the caret is already at the end.
bool textDeleteForward(TextContent* text, TextEditState* state);

// Move the caret by one character, or to either end. The `TextContent` is
// the only source of truth for where the boundaries are, which is why these
// take it by const-reference even though they mutate only `state->caret`.
void textCaretLeft(const TextContent& text, TextEditState* state) noexcept;
void textCaretRight(const TextContent& text, TextEditState* state) noexcept;
void textCaretHome(TextEditState* state) noexcept;
void textCaretEnd(const TextContent& text, TextEditState* state) noexcept;

// ==========================================================================
// 4. HIT TESTING
// ==========================================================================
//
// Is `at` (document coordinates) inside this text block? `bounds` is the
// caller's already-computed `textContentBounds()` -- see section 1 on why
// that is a parameter rather than a call this file makes itself.
//
// `padDoc` grows the test box on every side so that clicking just outside a
// thin glyph (an "l", a comma's descender) still selects the block --
// `text/Shaper.hpp`'s own bounds are the painted extent of actual ink, which
// for a single thin character can be a sliver a pointer will routinely miss
// by a pixel or two. `bounds.valid == false` (an empty text block; see
// `textContentBounds()`'s own comment) never hits, at any `at`.
bool textBlockHit(PathBounds bounds, PathPoint at, float padDoc) noexcept;

// ==========================================================================
// 5. THE EDIT SESSION -- the struct only; the member lives on `AppState`
// ==========================================================================
//
// Stored on `AppState` (beside `pathEdit`), but **mutated only through the
// transitions in this header, which live in app/TextTool.cpp.** `ui/` reads
// these fields and never assigns to them -- `app/PenTool.hpp` section 8's
// single-writer rule, made necessary by the exact bug that rule documents:
// a flag with more than one writer on `AppState` (`marqueeDragging`, three
// writers, one sibling tool's `else` arm silently clearing it every frame)
// made the Gradient tool inert for its entire history before anyone noticed.
struct TextEditState {
  // Which document and which layer this session is editing. A session begun
  // on another tab, or while a document's layer stack has since changed
  // under it, means nothing here and must be dropped by the caller rather
  // than applied to the wrong layer -- `app/PenTool.hpp`'s `documentId` rule,
  // `app/MeasureLine.hpp`'s `measureLineAppliesTo()`, and `CropSession::doc`'s.
  // This file stores the fact; checking it before calling into a stale
  // session is `ui/`'s job, the same division `PathEditState::documentId`
  // already has with its own canvas caller.
  uint64_t documentId = 0;

  // Which layer, by index into the active document's layer stack. `kNoLayer`
  // (below) while a paragraph-frame drag is live: that gesture creates a
  // NEW layer on pen-up, so there is no index to name until
  // `textEditFrameDragEnd()` returns true and the caller inserts one.
  size_t layerIndex = static_cast<size_t>(-1);

  // A BYTE offset into the `TextContent::utf8` being edited -- see section 3
  // for the boundary invariant every transition maintains.
  size_t caret = 0;

  // A paragraph-frame drag: pen-down on empty canvas with `Tool::Text`,
  // drag, pen-up -> either a paragraph frame (drag exceeded the minimum
  // size) or nothing, a click meaning point text instead (section 6 below).
  bool frameDragActive = false;
  PathPoint frameDragStart{};  // pen-down, document coordinates
  PathPoint frameDragNow{};    // current pointer, document coordinates

  // Whether this session has already opened an undo entry. Owned here for
  // the same reason `PathEditState::geometryEditOpened` is: the caller never
  // holds a second copy of the same fact, so `recordEdit()` vs `amendEdit()`
  // has exactly one place that can get it wrong.
  bool undoOpened = false;
};

inline constexpr size_t kNoLayer = static_cast<size_t>(-1);

// Abandon any live paragraph-frame drag. Does NOT reset `documentId`,
// `layerIndex` or `caret` -- those name the caret-editing session, which
// survives a cancelled drag exactly the way `pathEditCancel()` keeps
// `PathEditState::selection` alive across an abandoned gesture. Called when
// Escape is pressed mid-drag, or the tool changes away from `Tool::Text`
// while a drag (but no caret session) is live.
void textEditCancel(TextEditState* state) noexcept;

// Put the caret at `offset`, clamped to a UTF-8 boundary of `text.utf8`.
//
// The gesture this exists for is a click on an existing block: `textEditBegin()`
// deliberately answers only "clicked to start editing" (caret at the end), and
// a single click in this application means both that AND "clicked at this
// character". So `ui/` computes the byte offset -- `core/TextContent`'s
// `textOffsetAtPoint()`, which needs to shape and therefore cannot live in
// this file -- and hands it here.
//
// Clamped despite `textOffsetAtPoint()` already returning a boundary, because
// this is a public entry point taking a caller's `size_t` and section 3's
// invariant is this file's to hold, not its callers' to remember.
void textCaretSetOffset(TextEditState* state, const TextContent& text, size_t offset) noexcept;

// Record that this session has opened an undo entry, so the next edit amends
// rather than records. `ui/` owns the decision (only it knows whether it just
// called `recordEdit()` or `amendEdit()`); the FLAG lives here so there is not
// a second copy of the same fact in `drawUI()` -- `PathEditState::
// geometryEditOpened`'s reason exactly.
void textEditMarkUndoOpened(TextEditState* state) noexcept;

// Begin (or replace) a caret-editing session on `content`, the block already
// on layer `layerIndex` of document `documentId`. Resets any live frame
// drag and undo bookkeeping -- a fresh session shares nothing with whatever
// session or gesture came before it, which is the property section 6's
// isolation check is about: beginning a session on one document/layer must
// not leak the previous one's caret or in-progress drag into it.
//
// The caret starts at the END of `content.utf8`. A future click-to-position
// (mapping a pointer's document coordinates to a byte offset via
// `text/Shaper.hpp`'s per-glyph `cluster`s) is `ui/`'s to add on top of this;
// "clicked to start editing" and "clicked at a specific character" are two
// different gestures, and this function answers only the first, the same way
// double-clicking a text box in most editors places the caret at the end
// before a subsequent single click repositions it.
void textEditBegin(TextEditState* state, uint64_t documentId, size_t layerIndex,
                    const TextContent& content);

// Pen-down on empty canvas with `Tool::Text`: the start of a candidate
// paragraph-frame drag. Like `textEditBegin()`, this discards whatever
// session or drag was live before it -- `layerIndex` is set to `kNoLayer`
// (section 5) because the layer this drag might produce does not exist yet.
void textEditFrameDragBegin(TextEditState* state, PathPoint at, uint64_t documentId) noexcept;

// Pointer moved to `at` during a live frame drag. A no-op when no drag is
// active, so a caller does not have to guard every pointer-move event on
// `frameDragActive` itself.
void textEditFrameDragUpdate(TextEditState* state, PathPoint at) noexcept;

// ==========================================================================
// 6. THE FRAME DRAG -- a click is point text, a drag is a paragraph
// ==========================================================================
//
// Pen-up. Ends the drag either way (`frameDragActive` becomes false on
// return, success or failure) and writes `out->origin`/`out->frame.width`/
// `out->frame.height` from the finished rectangle ONLY on success; every
// other field of `*out` -- `utf8`, `style`, `fill`, ... -- is the caller's
// concern; this function edits none of them, so a caller that has already
// populated a default `TextContent` (`makeTextContent()`, once the sibling
// track's implementation lands) does not have those defaults clobbered.
//
// `origin` is always the drag's rectangle's TOP-LEFT corner, regardless of
// which of the four diagonal directions the pointer moved -- section 2's
// "shaping happens in text-space with its own origin at the block's
// top-left" has exactly one meaning for top-left, and a drag from the
// bottom-right corner backward must resolve to the same box a drag from the
// top-left corner forward would, or two users producing the identical
// on-screen rectangle would get two different `TextContent`s.
//
// Returns false -- and writes nothing -- when either dimension of the
// dragged rectangle is smaller than `minSizeDoc`: PLAN.md phase 14's "a
// click means point text, not a zero-width paragraph." The threshold is
// exclusive-of-neither (`< minSizeDoc` fails, `== minSizeDoc` passes), so a
// caller can choose `minSizeDoc` to mean "at least this big" without a
// separate off-by-one to reason about.
bool textEditFrameDragEnd(TextEditState* state, TextContent* out, float minSizeDoc) noexcept;

}  // namespace np
