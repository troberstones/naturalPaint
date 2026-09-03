#include "app/TextTool.hpp"

#include <algorithm>

#include "app/AppState.hpp"  // the real `enum class Tool`, opaque in the header

namespace np {

bool toolEditsText(Tool t) noexcept { return t == Tool::Text; }

namespace {

// --- the one clamp (header section 3) --------------------------------------

// How many bytes a UTF-8 sequence starting with `lead` occupies, per RFC
// 3629's lead-byte table. Returns 1 for anything that is not a recognized
// multi-byte lead -- an ASCII byte, a stray continuation byte (0x80-0xBF), or
// a byte RFC 3629 never assigns (0xF5-0xFF, post-2003) -- so a caller that
// walks a string one `utf8LeadLen()` at a time always advances by at least
// one byte and never reads past a byte it could not classify. This is the
// header's documented degrade-to-single-byte behaviour for content that was
// already invalid before this file touched it; it changes nothing about
// valid UTF-8, where every lead byte here is classified correctly.
size_t utf8LeadLen(unsigned char lead) noexcept {
  if ((lead & 0x80) == 0x00) return 1;  // 0xxxxxxx
  if ((lead & 0xE0) == 0xC0) return 2;  // 110xxxxx
  if ((lead & 0xF0) == 0xE0) return 3;  // 1110xxxx
  if ((lead & 0xF8) == 0xF0) return 4;  // 11110xxx
  return 1;                             // continuation byte, or unassigned
}

// Snap `pos` to the start of the character it falls inside -- the ONE clamp
// every public caret-touching operation below routes its incoming offset
// through before doing anything else (header section 3: "put that clamp in
// one place and call it, do not scatter it"). Handles both hazards the same
// way: `pos` past the end clamps to `utf8.size()`, and `pos` mid-sequence
// (a `TextEditState::caret` left over from a `TextContent` that has since
// changed underneath it) snaps DOWN to the boundary before it.
//
// Walking forward from the string's own start (a known boundary) rather than
// probing backward a few bytes from `pos` is deliberate: a continuation byte
// alone gives no hint of how far back its lead byte sits (1, 2 or 3 bytes),
// so the only way to find the boundary a mid-sequence `pos` belongs to, for
// valid UTF-8, is a walk that starts somewhere known-correct. This runs in
// time proportional to the caret's own offset, not to the whole document --
// fine for the sizes a caret operates over, and not a per-frame hot loop.
size_t clampToBoundary(const std::string& utf8, size_t pos) noexcept {
  if (pos >= utf8.size()) return utf8.size();
  size_t i = 0;
  while (i < utf8.size()) {
    const size_t len = utf8LeadLen(static_cast<unsigned char>(utf8[i]));
    const size_t next = std::min(i + len, utf8.size());
    if (pos < next) return i;  // pos is at i, or falls inside this character
    i = next;
  }
  return utf8.size();
}

// The boundary immediately before `pos`, which callers guarantee is already
// a boundary (every public entry point clamps first). `pos == 0` has no
// character before it and returns 0, matching `textCaretLeft()`'s and
// `textBackspace()`'s shared "already at the start" case.
size_t prevBoundary(const std::string& utf8, size_t pos) noexcept {
  if (pos == 0) return 0;
  // **Clamped again here, even though every public entry point already
  // clamped.** Not belt-and-braces: without it this function HANGS rather
  // than answering wrongly. For `pos > utf8.size()` the loop's `i` saturates
  // at `utf8.size()` (the `std::min` below caps it) while `i < pos` stays
  // true forever. That is a spin, not a wrong offset, and a caret arriving
  // out of range is exactly the case the clamp exists for -- so the guard
  // belongs at BOTH ends rather than only at the one that is currently
  // reachable. Found by deliberately removing the public clamp: the test
  // process hung instead of printing a FAIL.
  if (pos > utf8.size()) pos = utf8.size();
  size_t i = 0, prev = 0;
  while (i < pos) {
    prev = i;
    i = std::min(i + utf8LeadLen(static_cast<unsigned char>(utf8[i])), utf8.size());
  }
  return prev;
}

// The boundary immediately after `pos` (a boundary already). `pos` at or
// past the end has no character after it and returns `utf8.size()`.
size_t nextBoundary(const std::string& utf8, size_t pos) noexcept {
  if (pos >= utf8.size()) return utf8.size();
  return std::min(pos + utf8LeadLen(static_cast<unsigned char>(utf8[pos])), utf8.size());
}

}  // namespace

// --- the string edits (header section 3) ------------------------------------

void textInsertUtf8(TextContent* text, TextEditState* state, std::string_view utf8) {
  if (text == nullptr || state == nullptr) return;
  state->caret = clampToBoundary(text->utf8, state->caret);
  text->utf8.insert(state->caret, utf8.data(), utf8.size());
  state->caret += utf8.size();
}

bool textBackspace(TextContent* text, TextEditState* state) {
  if (text == nullptr || state == nullptr) return false;
  state->caret = clampToBoundary(text->utf8, state->caret);
  if (state->caret == 0) return false;
  const size_t start = prevBoundary(text->utf8, state->caret);
  text->utf8.erase(start, state->caret - start);
  state->caret = start;
  return true;
}

bool textDeleteForward(TextContent* text, TextEditState* state) {
  if (text == nullptr || state == nullptr) return false;
  state->caret = clampToBoundary(text->utf8, state->caret);
  if (state->caret >= text->utf8.size()) return false;
  const size_t end = nextBoundary(text->utf8, state->caret);
  text->utf8.erase(state->caret, end - state->caret);
  return true;
}

void textCaretLeft(const TextContent& text, TextEditState* state) noexcept {
  if (state == nullptr) return;
  state->caret = clampToBoundary(text.utf8, state->caret);
  state->caret = prevBoundary(text.utf8, state->caret);
}

void textCaretRight(const TextContent& text, TextEditState* state) noexcept {
  if (state == nullptr) return;
  state->caret = clampToBoundary(text.utf8, state->caret);
  state->caret = nextBoundary(text.utf8, state->caret);
}

void textCaretHome(TextEditState* state) noexcept {
  if (state == nullptr) return;
  state->caret = 0;
}

void textCaretEnd(const TextContent& text, TextEditState* state) noexcept {
  if (state == nullptr) return;
  state->caret = text.utf8.size();
}

// --- hit testing (header section 4) -----------------------------------------

bool textBlockHit(PathBounds bounds, PathPoint at, float padDoc) noexcept {
  if (!bounds.valid) return false;
  return at.x >= bounds.minX - padDoc && at.x <= bounds.maxX + padDoc &&
         at.y >= bounds.minY - padDoc && at.y <= bounds.maxY + padDoc;
}

// --- the edit session (header sections 5 and 6) -----------------------------

void textEditCancel(TextEditState* state) noexcept {
  if (state == nullptr) return;
  state->frameDragActive = false;
  state->frameDragStart = PathPoint{};
  state->frameDragNow = PathPoint{};
  state->undoOpened = false;
  // `documentId`/`layerIndex`/`caret` are deliberately left alone -- see this
  // function's header comment: they name the caret-editing SESSION, which
  // outlives a cancelled gesture, the same way `pathEditCancel()` leaves
  // `PathEditState::selection` alone.
}

void textCaretSetOffset(TextEditState* state, const TextContent& text,
                        size_t offset) noexcept {
  if (state == nullptr) return;
  state->caret = clampToBoundary(text.utf8, offset);
}

void textEditMarkUndoOpened(TextEditState* state) noexcept {
  if (state == nullptr) return;
  state->undoOpened = true;
}

void textEditBegin(TextEditState* state, uint64_t documentId, size_t layerIndex,
                    const TextContent& content) {
  if (state == nullptr) return;
  state->documentId = documentId;
  state->layerIndex = layerIndex;
  state->caret = content.utf8.size();
  state->frameDragActive = false;
  state->frameDragStart = PathPoint{};
  state->frameDragNow = PathPoint{};
  state->undoOpened = false;
}

void textEditFrameDragBegin(TextEditState* state, PathPoint at, uint64_t documentId) noexcept {
  if (state == nullptr) return;
  state->documentId = documentId;
  state->layerIndex = kNoLayer;
  state->caret = 0;
  state->frameDragActive = true;
  state->frameDragStart = at;
  state->frameDragNow = at;
  state->undoOpened = false;
}

void textEditFrameDragUpdate(TextEditState* state, PathPoint at) noexcept {
  if (state == nullptr || !state->frameDragActive) return;
  state->frameDragNow = at;
}

bool textEditFrameDragEnd(TextEditState* state, TextContent* out, float minSizeDoc) noexcept {
  if (state == nullptr || out == nullptr || !state->frameDragActive) return false;

  const float x0 = std::min(state->frameDragStart.x, state->frameDragNow.x);
  const float y0 = std::min(state->frameDragStart.y, state->frameDragNow.y);
  const float x1 = std::max(state->frameDragStart.x, state->frameDragNow.x);
  const float y1 = std::max(state->frameDragStart.y, state->frameDragNow.y);
  const float width = x1 - x0;
  const float height = y1 - y0;

  // The drag ends either way; only the return value and `*out` distinguish
  // "a paragraph frame" from "too small, treat the pen-up as a click".
  state->frameDragActive = false;

  if (width < minSizeDoc || height < minSizeDoc) return false;

  out->origin = PathPoint{x0, y0};
  out->frame.width = width;
  out->frame.height = height;
  return true;
}

}  // namespace np
