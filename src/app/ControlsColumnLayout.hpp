#pragma once

#include <string>
#include <vector>

#include "app/ControlsLayout.hpp"

// app/ControlsColumnLayout -- **which sections the right-hand controls column
// shows, and in what order**, as a value the user can rearrange and that
// survives a relaunch. app/ControlsLayout.hpp is the seam this sits on: its
// own header comment says the section list "is data rather than a sequence of
// calls in the draw function precisely so the ordering rule... can be
// asserted headlessly" -- this file is what turns that data into something a
// user can edit instead of only something a build can assert.
//
// **This is the headless half only.** The ImGui affordance that lets a person
// drag a row or click a checkbox lives in `src/ui/MacPaintUI.cpp` and is a
// concurrent change; nothing here draws anything. What this file owns is the
// model `MacPaintUI.cpp` will read and write, and the file it persists to.
//
// ==========================================================================
// The invariant that matters most
// ==========================================================================
//
// **The sequence contains every `ControlsSection` enumerator exactly once,
// always.** Order and visibility vary; membership does not. Every operation
// below -- every mutator, `resetToDefault()`, and every `loadFromFile()` /
// `parse()` -- returns a layout that still holds this, even when the input
// that produced it (a hand-edited file, a file from an older or newer build,
// a truncated write) did not describe one.
//
// **Hiding every section is legal.** Nothing here forces one section to stay
// visible. The configuration affordance is meant to live outside the column
// itself (so a user who hides everything is never stranded with no way back
// in), and enforcing "at least one visible" here would be solving a UI
// problem in the model, one layer too low.
//
// ==========================================================================
// Serialization: stable string keys, never an enum ordinal
// ==========================================================================
//
// This project has already been bitten once by ordinal-keyed persisted data:
// an enum grows a new enumerator in the middle of its declared order and
// every file written before that point silently decodes to the WRONG
// section, with no parse error at all -- see app/UserBrushLibrary.hpp §2 for
// the same lesson learned about `DynamicSource`/`DynamicTarget` ordinals (that
// case is handled differently, by range-checking and preserving, because a
// `link`'s ordinal is frozen forever by convention; here there is a cheaper
// answer available, so it is used instead: don't write the ordinal at all).
//
// So every section is written and read as a short, stable, lower-case text
// key (`controlsSectionKey()` / `controlsSectionFromKey()` below) that is
// independent of `ControlsSection`'s declared order and independent of
// `controlsSections()`'s draw order. Inserting a thirteenth `ControlsSection`
// tomorrow, wherever in the enum it lands, cannot change what any existing key
// on disk decodes to -- the worst case is that an OLD file simply does not
// mention the new key, which is exactly the "missing section" case below and
// is handled by construction, not by care at every call site.
//
// ==========================================================================
// File format
// ==========================================================================
//
//     naturalPaint-panel-layout 1
//     section color 1
//     section layers 1
//     section history 1
//     section comps 1
//     section grade 0
//     section brush_library 0
//     section brush 0
//     section pigment 0
//     section medium 0
//     section board_tilt 0
//     section grid 0
//     section solver 0
//
// One `section <key> <0|1>` line per entry, in the column's own order, `1`/
// `0` for visible/hidden. The version on the header line is read but not
// enforced -- app/BrushLibraryFile.hpp §1's and app/UserBrushLibrary.hpp §1's
// same rule, restated here: a version mismatch is a diagnostic opportunity a
// later build could use, never a refusal to read an otherwise-fine file.
//
// ==========================================================================
// Round-trip repair, exactly
// ==========================================================================
//
// A file on disk may have been written by an older or a newer build, or by a
// person's editor. `parse()` always yields a layout satisfying the invariant
// above. The rules, applied together, in this order:
//
//  * **Unknown section name** (a key `controlsSectionFromKey()` does not
//    recognise -- a section that existed in a newer build and does not in
//    this one, or a typo) -- the line is ignored. Grammatically fine, so it
//    does not itself invalidate the file.
//  * **Duplicate section name** -- the first occurrence is kept, every
//    later one for the same section is ignored.
//  * **Missing section** -- any `ControlsSection` enumerator not named by a
//    (recognised, non-duplicate) line is APPENDED at the end, in
//    `controlsSections()`'s own default order relative to the other missing
//    sections. This is the case this codebase has a name for: "a section
//    added since the file was written must appear, not vanish" is the
//    silent-no-op failure mode app/UserBrushLibrary.hpp's whole ordinal
//    discussion exists to avoid, applied here to a section instead of a
//    dynamics link.
//  * **Malformed line** -- a grammar failure (a `section` line that is not
//    exactly three space-separated tokens, or whose third token is not
//    exactly `0` or `1`) is SKIPPED, like the unknown-key rule above it and
//    like app/UserBrushLibrary.cpp's per-line salvage.
//
//    The alternative -- one bad line invalidating the whole file -- was
//    written first and is worse on both cases it is meant to cover. It has
//    no upside on a *foreign* file, because a file that is not this format
//    has no salvageable lines at all, so every section is "missing" and the
//    rule above rebuilds the default anyway: discarding is what skipping
//    already does there, by a different road. And it has a real cost on a
//    file that is *mostly* this format -- one damaged byte in a twelve-line
//    file throws away an arrangement the user built, when eleven lines of
//    it were still perfectly readable.
//
//    "Do not half-apply" is the right instinct and this satisfies it: a
//    skipped line leaves its section unseen, the missing-section rule
//    appends it, and what comes out is always a complete layout with every
//    section exactly once -- never a partial one.
//
// A missing file is the ordinary first-run case, not an error: it is treated
// exactly like an empty one, which -- every section "missing" -- resolves to
// `resetToDefault()`'s layout by the same rule above, not by a special case.
//
// ==========================================================================
// Persistence mechanics
// ==========================================================================
//
// `~/Library/Application Support/naturalPaint/panel-layout.txt` on macOS,
// `${XDG_CONFIG_HOME:-~/.config}/naturalPaint/panel-layout.txt` elsewhere,
// overridable with `$NP_PANEL_LAYOUT` -- the same directory and the same
// override mechanism as `defaultUserPresetsFilePath()` and
// `defaultRecentDocumentsPath()`, for the reason both of those give: this is
// user data written at run time, and the override exists so `--selftest`
// never touches the developer's real settings.
//
// Written via temp-file-then-rename, matching `app/UserBrushLibrary.cpp`'s
// `writeFileAtomically()`/`syncPath()` exactly in shape and reimplemented
// rather than shared, for that file's own stated reason (app/
// UserBrushLibrary.hpp §4): a one-line pair of helpers is not worth a
// dependency between modules whose lifetimes are otherwise unrelated. The
// content here has no other copy anywhere -- exactly the property that
// module's §4 says atomic writes are worth paying for -- so this file gets
// the same treatment `recent-documents.txt` and `brush-libraries.txt`
// deliberately do NOT get.
namespace np {

// One row of the column: which section, and whether it is currently shown.
struct ControlsColumnEntry {
  ControlsSection section = ControlsSection::Color;
  bool visible = true;
};

inline constexpr int kControlsColumnLayoutFileVersion = 1;
inline constexpr const char* kControlsColumnLayoutFileHeader = "naturalPaint-panel-layout";

// The stable text key for one section -- see this header's serialization
// section for why it is a key and not `controlsSectionSpec(section).title`
// (a title is UI wording, free to be reworded; a persistence key is not) and
// not `static_cast<int>(section)` (an ordinal, exactly the trap this file
// exists to avoid).
//
// Total and defined for every `ControlsSection` enumerator -- `--selftest`
// asserts that, the same way `controlsSectionSpec()` asserts every
// enumerator has a spec.
const char* controlsSectionKey(ControlsSection section);

// The inverse. Returns false, leaving `*out` unchanged, for any string that
// is not exactly one of `controlsSectionKey()`'s outputs -- the "unknown
// section name" case `parse()`'s repair rule handles by ignoring the line.
bool controlsSectionFromKey(const std::string& key, ControlsSection* out);

// `~/Library/Application Support/naturalPaint/panel-layout.txt` on macOS (see
// this header's persistence section for the other platforms and the
// `$NP_PANEL_LAYOUT` override).
std::string defaultControlsColumnLayoutFilePath();

// The ordered, always-complete model of the right-hand controls column.
//
// A freshly constructed instance already satisfies the exactly-once
// invariant: the constructor is `resetToDefault()`.
class ControlsColumnLayout {
 public:
  ControlsColumnLayout();

  // Discards whatever order and visibility were in place and returns to
  // `controlsSections()`'s own order, every section visible. This is also
  // what a `parse()` that hits a malformed file falls back to, and what an
  // empty/missing file resolves to via the "every section missing" case --
  // so this is the one place the default layout is actually constructed;
  // everything else reaches it through here.
  void resetToDefault();

  // The full ordered list, hidden sections included -- what a "manage
  // sections" affordance would draw (every row, with its own checkbox),
  // as distinct from `visibleSections()` below (what the column itself
  // draws).
  const std::vector<ControlsColumnEntry>& entries() const noexcept { return entries_; }

  // The sections that are visible, in column order -- what the draw loop
  // iterates. A filtered view of `entries()`, computed fresh each call
  // rather than cached, because it is cheap (twelve entries) and a cache
  // would be one more thing every mutator below has to remember to
  // invalidate.
  std::vector<ControlsSection> visibleSections() const;

  // The index of `section` in `entries()`. Always found -- the invariant
  // guarantees it -- so this returns `entries_.size()` only if that
  // invariant has somehow been broken, which is a state `--selftest` treats
  // as a bug, not an input to handle gracefully.
  size_t indexOf(ControlsSection section) const noexcept;

  bool isVisible(ControlsSection section) const noexcept;

  // Shows or hides `section` in place -- its position in the order is
  // unchanged. Hiding every section is legal; see this header's own note
  // above on why that is not refused here.
  void setVisible(ControlsSection section, bool visible);

  // Moves `section` so it sits at `newIndex` in the resulting order (clamped
  // into `[0, entries().size() - 1]`), preserving every other section's
  // relative order around it. This is a genuine move, not a swap: the
  // sections between the old and new position shift by one to make room,
  // the same semantics `std::rotate` over one element gives.
  //
  // Membership is preserved by construction -- one entry is removed and the
  // same entry is reinserted, so the count and the set of sections present
  // cannot change no matter what index is asked for.
  void moveTo(ControlsSection section, size_t newIndex);

  // Swaps `section` with its neighbour one position earlier/later. A no-op
  // at the front/back respectively -- there is no wraparound.
  void moveUp(ControlsSection section);
  void moveDown(ControlsSection section);

  // Parses `text` per this header's round-trip repair rules, replacing this
  // instance's contents. Always leaves the invariant satisfied, including on
  // a malformed or empty input (see this header's repair section).
  void parse(const std::string& text);

  // The exact bytes `saveToFile()` writes for the layout as it stands now.
  std::string serialize() const;

  // Reads `path` if it exists and calls `parse()` on its contents. A missing
  // file is the ordinary first-run case, not a failure: it is treated
  // exactly like an empty one, which resolves to `resetToDefault()`'s layout
  // through `parse()`'s own "every section missing" rule -- so this always
  // returns true and always leaves a fully-populated layout, and `errorOut`
  // is only ever set for a file that exists but could not be read to the
  // end (matching `UserBrushLibraryStore::loadFromFile()`'s own contract).
  bool loadFromFile(const std::string& path, std::string* errorOut);

  // Writes via temp-file-then-rename (see this header's persistence
  // section). `errorOut` is populated on every failure path; a rename that
  // did not complete removes the `.tmp` file rather than leaving it beside
  // the real one.
  bool saveToFile(const std::string& path, std::string* errorOut) const;

 private:
  std::vector<ControlsColumnEntry> entries_;
};

}  // namespace np
