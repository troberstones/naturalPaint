#pragma once

#include <cstdint>
#include <string>
#include <vector>

// app/DocumentPresets -- **the sizes File > New offers, and the ones a user
// adds to that list.** docs/testing-issues.md T9 (P0): "Making a new
// document should let you set the resolution -- a simple dialog with
// standard presets, a way to make new presets, and a preset that creates a
// document at the system clipboard's resolution and pastes its contents in."
// This header is everything under that dialog except the dialog itself
// (someone else's change, ui/MacPaintUI.cpp -- not touched here) and except
// the clipboard bridge (io/ClipboardImage.hpp): the preset value, the
// built-in set, and the user-owned list that persists across launches.
//
// ==========================================================================
// 0. What a preset actually needs to hold
// ==========================================================================
//
// `core/Document.hpp`'s `Document::createBlank(int32_t width, int32_t
// height, WorkingSpace space)` and `app/DocumentLifecycle.cpp`'s
// `makeBlankOpenDocument(width, height, space, title)` are the entire new-
// document path in this build today (T9's own reading of
// `MacPaintUI.cpp:7113`, confirmed here by reading both call sites in full
// before writing this struct). Three things go in, and only one of them
// varies across what a "preset" means:
//
//   * `width`, `height` -- exactly what a preset is for. Kept here.
//   * `WorkingSpace space` -- always `WorkingSpace{}` (Rec.709 primaries,
//     `color/Space.hpp`'s only constant) at every call site in this codebase
//     today; there is no second primaries constant anywhere to choose
//     between, so a per-preset colour field would be a control with one
//     legal position. **Not modelled.** The day a second working space
//     exists, this is the header that grows a field for it -- not before,
//     per this brief's own "do not invent fields the creation path cannot
//     consume."
//   * `title` -- a label, not a document property; the dialog supplies it
//     (or `makeBlankOpenDocument()`'s own "Untitled" default does). Not a
//     preset field for the same reason a preset does not carry a file path.
//
// **DPI is deliberately absent.** T9's own wording says "resolution," and a
// print-shop reading of that word means physical size at a DPI -- but
// nothing in `Document`, `Layer` or the save format carries a DPI value to
// receive one (checked: `core/Document.hpp`, `io/NpaintFile` in full). A
// preset like "US Letter @ 300 dpi" is real and useful, and is represented
// here the only way the creation path can consume it: **pre-multiplied into
// pixel dimensions at the moment the built-in was chosen** (2550 x 3300),
// exactly as a screen-size preset's pixel dimensions already are. If a
// future build adds a stored DPI to `Document`, presets gain a field then;
// today it would be a number nothing reads.
//
// **No background/colour-mode field either.** `Document::createBlank()`
// always builds one RGB-kind layer with zero allocated tiles -- transparent,
// by `core/TileStore.hpp`'s own "unwritten texel reads as zero" contract.
// There is no "white background" option to select; painting on a preset's
// canvas starts exactly where painting on any new document already does.
//
// ==========================================================================
// 1. Built-ins versus user presets, and the one rule that keeps them apart
// ==========================================================================
//
// A built-in is `builtinDocumentPresets()`'s own data -- never constructed
// by a file read, never mutated, never written back to disk.
// `DocumentPresetStore` (below) owns a *separate* list, `userPresets()`, and
// the file it persists (`document-presets.txt`) contains **only** that
// list: `serialize()` walks `userPresets()` alone, so a built-in can no more
// end up duplicated into the user's file than `app/UserBrushLibrary.cpp`'s
// four shipped brush defaults can end up in `user-presets.txt` -- same
// structural guarantee, same reason (that file's own §3).
//
// **The distinguishing rule, spelled out because the brief this header
// answers to asks for one explicitly:** a user preset may never carry the
// exact name of a built-in. `add()` and `rename()` -- the two places a user
// types a name -- refuse outright (returning false with a named reason) on
// an exact collision, built-in or user; the dialog surfaces that refusal and
// the person picks a different name. `parse()` -- reading a hand-edited or
// older file -- takes the opposite stance for the same reason `app/
// UserBrushLibrary.cpp`'s `parse()` does: a file the user did not just type
// into cannot hand back a UI error, so a collision there is silently
// resolved through `uniqueDocumentPresetName()` (" 2", " 3", ... appended)
// rather than dropping the preset or refusing the whole file. Either way,
// **built-in and user presets can never carry the same name at the same
// time** -- there is no second case to reason about where a lookup by name
// has to guess which one was meant.
//
// ==========================================================================
// 2. The file, and why it looks like app/UserBrushLibrary.cpp's
// ==========================================================================
//
// Line-based, following `app/UserBrushLibrary.hpp` exactly (see that header
// §1 for the reasoning this does not repeat: no JSON writer in this
// codebase, so a hand-rolled one is not the second-simplest option, it is
// the least defensible one). One preset is two lines:
//
//     naturalPaint-document-presets 1
//     preset Gallery Wrap 24x36in @ 300dpi
//     size 7200 10800
//     preset Instagram Square
//     size 1080 1080
//
// `preset <name>` opens a scope (rest-of-line, no quoting -- the same
// argument app/UserBrushLibrary.hpp §1 makes). `size <width> <height>` is
// the ONLY data line a preset has; a preset with no valid `size` line is
// dropped in full at `flush()`, exactly the "not promoted... a preset with
// no readable content is not a preset with defaulted numbers, it is a
// record this build could not read" rule that header's §1 states for its
// own `scalars`. A `size` line is validated by
// `validateDocumentPresetSize()` at parse time -- not merely parsed -- so a
// hand-edited `size 0 0` or `size -4 900` or `size 999999999 5` never
// reaches this store's in-memory list at all, matching this brief's
// robustness rule verbatim ("rejected at load rather than reaching document
// creation"). Rejections are recorded in `problems()`, one line per
// rejected preset, so a dialog (or --selftest) can show what was dropped and
// why rather than the file silently shrinking.
//
// **No ordinal-keyed field exists in this format**, unlike `app/
// UserBrushLibrary.hpp`'s `link`/`floor` lines (`DynamicSource`/
// `DynamicTarget` ordinals) -- there is no enum here at all, so there is
// nothing this format could accidentally decode against the wrong member of
// a grown enum. `width`/`height` are plain integers with a validated range,
// keyed by nothing but their position on the `size` line.
//
// Unrecognised lines -- a stray line before any `preset`, or an unknown key
// inside a preset's scope -- are preserved verbatim in `unknownLines()` and
// re-emitted at the top of the file on the next `serialize()`, the same
// forward-compatibility gesture `app/DocumentLifecycle.cpp`'s
// `RecentDocuments` and `app/UserBrushLibrary.cpp` both make, cheap enough
// to keep even though this format has no evolving per-preset sub-structure
// yet to actually need it.
//
// ==========================================================================
// 3. Durability: atomic write, following app/UserBrushLibrary.cpp exactly
// ==========================================================================
//
// This file is the only copy of a preset a user typed dimensions into by
// hand -- the same "no other copy anywhere" fact `app/UserBrushLibrary.hpp`
// §4 argues at length for its own file, and the same conclusion follows:
// write to `<path>.tmp`, `fsync()` it, `fs::rename()` it over the real path.
// A crash or a kill mid-write leaves `document-presets.txt` exactly as it
// was; the `.tmp` file is left orphaned (not renamed) rather than replacing
// good data with a half-written file. Reimplemented rather than shared for
// the same reason `app/UserBrushLibrary.cpp`'s own copy is: `app/
// Journal.cpp`'s `writeFileAtomically()` has internal linkage, and promoting
// it is a cleanup with nothing to do with document presets.
namespace np {

// One preset: a name and a pixel size. `builtin` is never set true by
// anything in this header except `builtinDocumentPresets()`'s own data, and
// is never persisted -- `DocumentPresetStore::serialize()` only ever walks
// `userPresets()`, which by construction holds none.
struct DocumentPreset {
  std::string name;
  int32_t width = 0;
  int32_t height = 0;
  bool builtin = false;
};

// The shipped set, in display order. A short, deliberately unopinionated
// list rather than a Photoshop-sized template gallery -- three shapes any
// painting tool needs someone to have picked at all (a screen size, a print
// size, a square), each represented once at a size common enough to defend,
// plus one larger option on each axis for anyone who wants to scale down
// rather than up:
//
//   * "Web (1280 x 720)"              -- HD-ready screen work at 720p.
//   * "HD (1920 x 1080)"              -- the default screen/video frame.
//   * "4K UHD (3840 x 2160)"          -- exact 2x of the 1080p entry, for
//                                        anyone painting at delivery res.
//   * "Square (2048 x 2048)"          -- a power-of-two square painters
//                                        reach for regardless of final
//                                        export target (icon work, texture
//                                        painting, social crops).
//   * "US Letter @ 300dpi (2550 x 3300)"  -- 8.5x11in at a real print DPI.
//   * "A4 @ 300dpi (2480 x 3508)"         -- 210x297mm at the same DPI, the
//                                        non-US equivalent so the built-in
//                                        set is not US-only.
//
// Every dimension here is reproducible from its own name (pixels-per-inch
// times inches, rounded), which is what "defensible" means for this list:
// nothing was picked because it looked plausible.
const std::vector<DocumentPreset>& builtinDocumentPresets();

// True exactly when `name` matches a `builtinDocumentPresets()` entry,
// case-sensitively and exactly (no trimming beyond what the caller already
// did) -- the single place both `DocumentPresetStore` and a future dialog
// ask "is this one of the presets nothing can edit or delete."
bool isBuiltinDocumentPresetName(const std::string& name);

// `candidate` unchanged if it collides with nothing in `builtinDocumentPresets()`
// or `existingUser`; otherwise `candidate`, " 2", " 3", ... until one is
// free. Used only by `DocumentPresetStore::parse()` -- **not** by `add()` or
// `rename()`, which refuse a collision outright instead (see this header's
// §1). Mirrors `brush/Library.hpp`'s `uniquePresetName()` in shape, not
// shared with it: that function's signature is `BrushLibrary`-shaped and
// there is no brush-preset relationship here to reuse.
std::string uniqueDocumentPresetName(const std::string& candidate,
                                     const std::vector<DocumentPreset>& existingUser);

// The largest width or height this build will accept in a document preset,
// on either axis independently. Generous enough that no real built-in or
// hand-chosen size comes close (the largest built-in above is 7200px longest
// side) and small enough to catch a corrupted or hostile `size` line (a
// pasted extra digit, a byte-swapped field) before it reaches
// `Document::createBlank()` and whatever downstream tile-grid arithmetic
// assumes a plausible canvas.
inline constexpr int32_t kMaxDocumentPresetDimension = 32768;

// The empty string when `width`/`height` are usable by
// `Document::createBlank()` (both positive, both at most
// `kMaxDocumentPresetDimension`); otherwise the specific reason, naming the
// values that were refused. Called by `DocumentPresetStore::parse()` before
// a `size` line's numbers are trusted, and by `add()`/`rename()` before a
// typed size is accepted -- **one function, both entry points**, so a
// dimension check added here protects a hand-edited file and a live dialog
// alike rather than needing to be kept in sync between two copies.
std::string validateDocumentPresetSize(int32_t width, int32_t height);

// `~/Library/Application Support/naturalPaint/document-presets.txt` on
// macOS, `${XDG_CONFIG_HOME:-~/.config}/naturalPaint/document-presets.txt`
// elsewhere, `$NP_DOCUMENT_PRESETS` overrides both -- the same directory,
// same override mechanism as `defaultUserPresetsFilePath()` and
// `defaultRecentDocumentsPath()`, a different file for a different kind of
// setting.
std::string defaultDocumentPresetsFilePath();

// The user's own saved sizes: parses and writes `document-presets.txt`, and
// is the one place that carries this session's memory of lines it did not
// understand (§2 above).
class DocumentPresetStore {
 public:
  // Replaces the current contents by parsing `text` (§2's format). Never
  // fails outright -- a missing/garbage file becomes an empty list, which is
  // exactly "fall back to the built-ins" once `allPresets()` (below) adds
  // them back in: there is no separate "corrupt file" code path a caller has
  // to handle, because a store with zero user presets and a store that has
  // never been loaded behave identically.
  void parse(const std::string& text);

  // Reads the file if present; a missing file is a fresh install, not an
  // error (same contract as `UserBrushLibraryStore::loadFromFile()`).
  bool loadFromFile(const std::string& path, std::string* errorOut);

  // The exact bytes `saveToFile()` writes, computed from `userPresets()` as
  // it stands right now, plus any preserved unknown lines. Built-ins never
  // appear here (§1).
  std::string serialize() const;

  // Writes via temp-file-then-rename (§3).
  bool saveToFile(const std::string& path, std::string* errorOut) const;

  // The user's own presets, in file/add order. Never includes a built-in.
  const std::vector<DocumentPreset>& userPresets() const noexcept { return presets_; }

  // Built-ins (in `builtinDocumentPresets()` order) followed by
  // `userPresets()` (in their own order) -- everything a dialog's preset
  // list should show, in one call.
  std::vector<DocumentPreset> allPresets() const;

  // Appends a new user preset named `name` at `width` x `height`. Refuses
  // (returns false, `errorOut` set) when: `validateDocumentPresetSize()`
  // rejects the size; `name` is empty; or `name` exactly matches a built-in
  // or an existing user preset (§1 -- this is the interactive path, so it
  // refuses rather than silently renaming). On success the preset is
  // appended to `userPresets()`; the caller still calls `saveToFile()` to
  // persist it, the same two-step `UserBrushLibraryStore` callers already
  // follow.
  bool add(const std::string& name, int32_t width, int32_t height, std::string* errorOut);

  // Renames the user preset named `oldName` to `newName`, in place (position
  // in `userPresets()` unchanged). Refuses when `oldName` names a built-in
  // (not renameable) or no user preset; when `newName` is empty; or when
  // `newName` exactly matches a built-in or a *different* existing user
  // preset (renaming a preset to the name it already has is a no-op, not a
  // refusal).
  bool rename(const std::string& oldName, const std::string& newName, std::string* errorOut);

  // Removes the user preset named `name`. Refuses when `name` names a
  // built-in (§1 -- "must not be editable or deletable") or matches no user
  // preset.
  bool remove(const std::string& name, std::string* errorOut);

  // One entry per `size` line rejected at parse time (missing, malformed, or
  // refused by `validateDocumentPresetSize()`), `"<source>:<line>: <why>"` --
  // the same shape `RecentDocuments::problems()` already reports in. Cleared
  // and repopulated by every `parse()`/`loadFromFile()` call.
  const std::vector<std::string>& problems() const noexcept { return problems_; }

  // Lines this build did not recognise (§2), preserved verbatim.
  const std::vector<std::string>& unknownLines() const noexcept { return unknownLines_; }

 private:
  std::vector<DocumentPreset> presets_;
  std::vector<std::string> problems_;
  std::vector<std::string> unknownLines_;
};

}  // namespace np
