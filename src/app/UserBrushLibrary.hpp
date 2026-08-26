#pragma once

#include <map>
#include <string>
#include <vector>

#include "brush/Library.hpp"

// app/UserBrushLibrary -- **the brushes a user made, saved so they are still
// there tomorrow.** PRD G6, and reachability-audit.md's A7: `BRUSH EDITOR >
// Save` used to overwrite `lib.presets[lib.active]` in memory and nowhere
// else, so a tuned brush did not survive the quit that followed it.
//
// ==========================================================================
// 0. Why this is not app/BrushLibraryFile, extended
// ==========================================================================
//
// app/BrushLibraryFile.hpp (read in full before this was written) already
// persists *imported* `.abr` libraries, and it is a genuinely good fit for
// what it persists: a `.abr` is somebody else's file, this build cannot write
// one back, and the honest answer to editing a library brush is "Duplicate it
// -- Save is refused" (`ui/MacPaintUI.cpp`'s `drawBrushSection()`, the
// `fromLibrary` branch). That module's whole design -- the row cache, the
// lazy read, the `stat()`-not-a-read counter -- exists to avoid paying for a
// multi-megabyte pack the user may not touch this session.
//
// None of that applies here, and forcing user presets through the same shape
// would be the wrong shape wearing the right module:
//
//   * **A user preset has no source file to defer to.** There is nothing to
//     be lazy ABOUT -- the preset the user authored is the only copy that
//     exists, so it is either fully in memory or it is gone. §4's row cache,
//     built specifically so a pack's dynamics need not be read until picked,
//     has no problem to solve here: the "expensive half" (`BrushLinkSet`) IS
//     the content.
//   * **Losing it is not "re-import the file"; it is losing the work.** The
//     PRD line this whole module exists to satisfy is blunt about that: "a
//     brush you save must still be there tomorrow." A `.abr` registry entry
//     that fails to load is an inconvenience with a fix (re-locate the file);
//     a user preset that fails to load is gone.
//   * **Different write frequency.** The `.abr` registry is written on
//     import, on unload, and on a library pick -- occasional. Save is a
//     button a working painter can press every few minutes while tuning a
//     brush. Coupling the two into one file means a bug in the row-cache
//     serialiser can block or corrupt a save of actual content, and means
//     every Save pays for re-writing a `.abr` registry that has not changed.
//
// So: **a separate file**, `user-presets.txt`, beside `brush-libraries.txt`
// in the same settings directory. Two files that change at different rates
// for different reasons and carry different consequences when lost are not
// "one settings location split in two for no reason" -- they are two
// different pieces of state that happen to share a directory, the same
// relationship `recent-documents.txt` already has to both of them.
//
// ==========================================================================
// 1. The file, its keys, and why there is no row cache
// ==========================================================================
//
// Line-based, for the reason app/BrushLibraryFile.hpp §1 gives at length and
// does not need repeating: there is no JSON writer in this codebase, and a
// third hand-rolled partial one would be the least defensible line here.
//
//     naturalPaint-user-presets 1
//     preset My Custom Wash
//     scalars 30.5 0.62 0.18 0.7 12 1.1 2.4
//     grain 1 24 24 0.35 1
//     floor 0 0.2
//     link 0 4 0.2 1 0 1
//     point 0 0
//     point 0.4 0.15
//     point 1 1
//     preset Detail Liner 2
//     scalars 6 0.9 0.08 1 0 1.2 0.5
//     grain 0 24 24 0.35 1
//
// `preset` opens a scope exactly as `library` does in the sibling file: the
// rest of the line is the name (no quoting, no escaping -- app/
// BrushLibraryFile.hpp §1's reasoning again), and every `scalars`/`grain`/
// `link`/`point` line belongs to the most recent `preset` line above it.
//
// **No `row` line and no cached scalars-only form.** app/BrushLibraryFile's
// `BrushRow` exists so a pane can draw a brush's icon and name without its
// `.abr` read (§4 there: "the dynamics matrix is the expensive half... and it
// is the half a library row does not draw"). A user preset is never in that
// state -- it is loaded whole, the instant this file is read, because the
// links are exactly the part the user authored and the part `--selftest`'s
// mandate ("assert on the round trip, not on the shape of your structs") is
// really about. One `BrushPreset`, fully populated, is both the cache and the
// content; there is no cheaper half to defer.
//
// **`scalars` is seven numbers, frozen positionally** -- radius, hardness,
// spacing, roundness, angle, load, wetness, in `BrushPreset`'s declared
// order. Same rule as the sibling file's `row`: an eighth number added here
// by a later build would be read by this one as `preset`'s name is NOT read
// by `scalars` -- wait, more precisely, as garbage that fails to parse as the
// eighth float and is dropped as a malformed record (see below), which is
// why new scalar data must arrive as a new key, never an eighth field.
//
// **`grain 1 24 24 0.35 1` -- enabled, periodX, periodY, depth, strength
// (`BrushPreset::grain`, brush/Grain.hpp's `GrainParams`) -- is exactly that:
// a NEW key added after `scalars` already had a file format in the wild,
// rather than an eighth `scalars` field.** Growing `scalars`' required count
// would fail `takeFloats(rest, 7, n)`'s exact-count parse against every
// `user-presets.txt` written before this feature existed and drop the whole
// preset it belongs to (§ this section's own paragraph above, restated for
// the case that actually arrived). A `preset` block with no `grain` line at
// all -- every file on disk before this feature shipped -- leaves
// `pending.grain` at its default-constructed value, which is grain OFF, the
// answer every such brush already had. A malformed `grain` line is treated
// like a malformed `link` line (below), not like a malformed `scalars` one:
// it costs only itself, not the whole preset.
//
// ==========================================================================
// 2. A link, and why its ordinals are load-bearing
// ==========================================================================
//
// `link <sourceOrdinal> <targetOrdinal> <rangeLo> <rangeHi> <invert> <enabled>`
// followed by zero or more `point <x> <y>` lines, one per `Curve` control
// point (ops/PointOps.hpp), in file order -- which is curve order, because a
// `Curve` is already required sorted ascending by `.x` by every writer of one
// in this codebase (the curve widget's own contract) and this module does not
// re-sort any more than `evalCurve()` does.
//
// The source and target are written as their enum's ordinal
// (`static_cast<int>`), not their display name (`sourceName()`/
// `targetName()`): those strings are UI labels the panel is free to reword,
// and a persistent file format must not break because a tooltip's wording
// changed. **The ordinals are frozen in `brush/Dynamics.hpp`'s declared enum
// order forever** -- a future source or target is appended at the end of its
// enum, never inserted, exactly the "positional keys are frozen" rule
// app/BrushLibraryFile.hpp §3 states for its own `row`, applied here to an
// enum instead of a struct.
//
// **A `link` whose ordinal is out of an OLDER build's range is preserved, not
// dropped.** This is the one place this module's forward-compatibility story
// differs in kind from a malformed line, and the difference matters enough to
// spell out:
//
//   * `link 1 2 abc 1 0 1` -- six fields, four of which do not parse as
//     numbers. Malformed. Dropped, along with whatever `point` lines follow
//     it, same as app/BrushLibraryFile.hpp's `row` with too few numbers:
//     "not promoted to an unknown line... re-emitting a row this build could
//     not read would keep a corrupt line alive in the file forever."
//   * `link 11 4 0.2 1 0 1` -- six fields, all six parse, but a future build
//     added an eleventh `DynamicSource` and this one only knows ten
//     (`kDynamicSourceCount` -- 10 as of `DynamicSource::InitialDirection`,
//     appended after `Direction` at ordinal 9; this example's ordinal 11 has
//     been out of range since before Direction or InitialDirection existed
//     and stays out of range after both, which is exactly why 11 rather
//     than 8 or 9 illustrates the case). This is not corrupt -- it is
//     correct data from a build newer than this one, describing a link this
//     build cannot evaluate but has no reason to destroy. Kept **verbatim**,
//     along with its `point` lines, as an unknown block scoped to the
//     preset it belongs to, and written back out unchanged on the next save
//     by this build. A user who round-trips a preset through this build
//     must not come back to find the newer build's twelfth link missing.
//
// The two cases look identical until the numbers are checked against the
// enum's current range, which is exactly why the range check -- not merely
// "did it parse" -- is what decides preserve-versus-drop here.
//
// ==========================================================================
// 2b. `floor` -- a per-TARGET line, not a per-link one
// ==========================================================================
//
// `floor <targetOrdinal> <value>`, one line per non-zero entry of
// `BrushLinkSet::multiplyFloor` (brush/Dynamics.hpp) -- Photoshop's Minimum
// Diameter, docs/reachability-audit.md B6, once this build stopped folding
// it into every link's own `rangeLo` and started keeping it once, per
// target. It has no `point` lines of its own (it names a target, not a
// curve) and is emitted, like `link`, only when there is something to say --
// a preset with no Minimum Diameter writes no `floor` line at all, so it
// round-trips through this build byte-identical to how it would have before
// this key existed.
//
// The target ordinal is range-checked exactly as a `link`'s two ordinals are
// (§2 above), and for the identical reason: a future build's thirteenth
// `DynamicTarget` writing its own floor is correct data this build cannot
// evaluate but must not destroy, so an out-of-range ordinal is preserved
// verbatim rather than dropped. Malformed numbers are dropped outright, the
// same as a malformed `link`.
//
// ==========================================================================
// 3. What decides overwrite versus a new preset: `BrushPreset::builtin`
// ==========================================================================
//
// Both a shipped default and a user's own saved brush carry `libraryId == 0`
// -- that field only ever distinguished "belongs to an imported `.abr`" from
// "does not". A second boolean was added to `brush/Library.hpp`,
// `BrushPreset::builtin`, true for exactly the four presets
// `defaultBrushLibrary()` constructs and false everywhere else (every
// `presetFromBrush()` call, every preset this module reads back). See that
// header for the full reasoning; the short form is that Save needed a way to
// ask "is the active preset mine to overwrite, or the app's to fork from",
// and `libraryId` alone cannot answer it.
//
// `ui/MacPaintUI.cpp`'s `drawBrushSection()` is where the answer is used:
//
//   * **Active preset is a user's own** (`builtin == false`, `libraryId ==
//     0`) -- Save overwrites it in place and re-serialises this file. This is
//     what makes Save mean what its label says without the library filling
//     with "Detail Liner 2", "Detail Liner 3", ... every time someone nudges
//     a slider and presses the same button twice.
//   * **Active preset is a built-in** (`builtin == true`) -- Save cannot mean
//     "overwrite", because a built-in edited and saved over is a factory
//     default the user can never get back (there is no "restore defaults" in
//     this build, and there does not need to be one if a built-in can simply
//     never be lost). So Save **forks**: it creates a new preset from the
//     live brush, named uniquely off the built-in's name
//     (`uniquePresetName()`, the same call `Duplicate` already makes), and
//     that new preset is what gets persisted. The button's label changes to
//     "Save As New" for exactly this case, because a button that forks
//     silently while calling itself "Save" is the failure mode PRD G6 exists
//     to close, one level up from where it started.
//   * **Active preset belongs to an imported library** (`libraryId != 0`) --
//     unchanged from before this module existed: Save stays disabled, for
//     the reason `drawBrushSection()`'s own comment gives (this build writes
//     no `.abr`, so a kept edit would be silently replaced next launch).
//
// ==========================================================================
// 4. Durability: why this file, unlike its neighbours, writes to a
//    temp file and renames
// ==========================================================================
//
// **Neither `app/DocumentLifecycle.cpp`'s `RecentDocuments::saveToFile()` nor
// `app/BrushLibraryFile.cpp`'s `BrushLibraryStore::saveToFile()` writes
// atomically** -- both open the real path with `std::ios::trunc` and write
// straight into it. Checked before writing this module, because the brief
// this module answers to says explicitly not to invent a third convention
// where a consistent one already exists -- and it does not. Both of those
// files are one `ofstream` truncate-and-write.
//
// That is the right choice for what they store and the wrong one for this.
// `recent-documents.txt` is a most-recently-used list: a torn write loses
// which files were open, not the files themselves. `brush-libraries.txt` is
// a cache of a cache -- a crash mid-write loses which `.abr` packs were
// imported, and re-importing them costs a few clicks and reads files that
// still exist on disk untouched. **This file is the only one of the three
// whose content has no other copy anywhere.** A process killed midway
// through `std::ofstream::write()` on this file, with the OS having flushed
// some prefix of the new (shorter, or differently-shaped) text over the old,
// can leave a truncated `preset` block, an orphaned `link` line, or -- worse
// -- a file that starts with the new content and ends with a tail of the old
// one, which is what "truncate, then write over it" produces when the write
// stops halfway.
//
// `app/Journal.cpp` already has the answer, for the same reason: the
// document journal is also content nothing else can reconstruct, and it
// already writes to `<path>.tmp`, `fsync()`s that file, and `fs::rename()`s
// it over the real path -- a rename is atomic on the same filesystem, so a
// reader (or a crash) only ever sees the whole old file or the whole new
// one, never a mixture. This module's `writeUserPresetsAtomically()` is the
// same technique, reimplemented rather than called: `Journal.cpp`'s version
// lives in that file's anonymous namespace (internal linkage, not reusable
// across a translation unit) and promoting it to a shared utility is a
// cleanup of `app/Journal.*` that has nothing else to do with brushes --
// exactly app/BrushLibraryFile.hpp §5's reason for not factoring out a
// fourth copy of `defaultBrushLibraryFilePath()`'s shape either. `fsync()`
// rather than `fcntl(F_FULLFSYNC)`, for the same measured trade-off
// `app/Journal.cpp` documents at its own `syncPath()`: `fsync()` covers a
// crashed process, a kill and a kernel panic -- the failure modes this
// application can actually produce -- at roughly a tenth the cost of also
// covering sudden power loss.
//
// **What this buys, precisely:** a crash or a kill during the write leaves
// `user-presets.txt` exactly as it was before the write started -- the
// `.tmp` file is either absent, partial, or complete-but-not-yet-renamed,
// and none of those three states touch the real path. `--selftest` cannot
// truly interrupt a write (there is no hook to kill this process mid-syscall
// from inside itself) but it simulates the failure the atomic write is
// bought to prevent: it writes a `.tmp` file, corrupts and abandons it
// without renaming, and asserts the real file is untouched -- see
// `app/selftest/UserBrushLibrary.cpp`'s durability section for exactly what
// is and is not proven that way.
//
// ==========================================================================
// 5. What is deliberately not here
// ==========================================================================
//
// **No thumbnail pixels on disk.** PRD G6 says presets "carry a thumbnail",
// and they do -- `ui/MacPaintUI.cpp`'s row code already rasterises one on
// demand from any preset with a `presetIndex` (`brushPresetIconTips()`, via
// `app/DabPreview`), and a user preset loaded by this module always has one:
// it is never in the "cached row, `.abr` unread" state app/BrushLibraryFile
// has to draw around, because §0 above is exactly the case where nothing is
// deferred. No new pane code, no new icon path -- a saved preset shows up in
// the same unheaded list Duplicate already populates and is drawn by the
// same row.
//
// **No rename.** Renaming a preset would mean this module's "which preset in
// `lib.presets` corresponds to which `preset` block on disk" question needs
// an identity that survives a name change -- currently that correspondence
// IS the name (see `serialize()`'s doc comment). Adding a rename is a real
// feature with its own UI (an editable text field, a collision check against
// `uniquePresetName()`) and the brief this module answers scopes it out:
// "do not build a whole library manager; do build enough that a saved brush
// can be got rid of." Delete is that "enough"; rename is not required to get
// a brush back to a name that is not confusing, because Save As New already
// derives a distinct one.
//
// **No reorder.** The list order is `lib.presets`' own order, which nothing
// in this module or its caller changes; a user preset's position is simply
// wherever `push_back()` (Duplicate, or Save As New) or a file read left it.
namespace np {

// The version this build writes. Read but not enforced, exactly as
// app/BrushLibraryFile.hpp's own `kBrushLibraryFileVersion` is not: see that
// header's §1 and §3 for why a version mismatch is a diagnostic, never a
// refusal.
inline constexpr int kUserPresetsFileVersion = 1;
inline constexpr const char* kUserPresetsFileHeader = "naturalPaint-user-presets";

// `~/Library/Application Support/naturalPaint/user-presets.txt` on macOS,
// `${XDG_CONFIG_HOME:-~/.config}/naturalPaint/user-presets.txt` elsewhere,
// overridable with `$NP_USER_PRESETS`. Same directory, same override
// mechanism as `defaultBrushLibraryFilePath()` -- a different FILE for a
// different durability contract (§4), not a different settings location.
std::string defaultUserPresetsFilePath();

// Parses and writes `user-presets.txt`, and is the one place that carries
// this session's memory of lines it did not understand (§3 of app/
// BrushLibraryFile.hpp's forward-compatibility rule, applied here).
//
// **Deliberately thin compared to `BrushLibraryStore`.** There is no row
// cache, no lazy read, no per-library status and no ids to mint: every user
// preset is fully resident in `BrushLibrary::presets` the instant this file
// is read, so the one thing this class has to remember between a `parse()`
// and a later `serialize()` is the text it could not interpret.
class UserBrushLibraryStore {
 public:
  // Parse `text`, appending every preset it describes to `lib` -- tagged
  // `libraryId = 0`, `builtin = false` (a fresh `BrushPreset`'s own
  // defaults; not set explicitly here so a future field added to
  // `BrushPreset` inherits its own default the same way), with its name run
  // through `uniquePresetName()` in case a hand-edited file collides with a
  // built-in or with another preset already in `lib`. Never fails outright,
  // same as app/BrushLibraryFile.hpp §1: every line is independently
  // meaningful, so there is no invariant spanning the whole file for a
  // refusal to protect.
  void parse(const std::string& text, BrushLibrary& lib);

  // Read the file if it is there; a missing file is a fresh install, not an
  // error, same as `BrushLibraryStore::loadFromFile()`.
  bool loadFromFile(const std::string& path, BrushLibrary& lib, std::string* errorOut);

  // The exact bytes `saveToFile()` writes, computed **from `lib.presets` as
  // it stands right now** -- every preset with `libraryId == 0 && !builtin`,
  // in `lib.presets`' own order, is a preset this file must describe;
  // nothing else is. This is what makes Delete need no code in this module
  // at all: a preset removed from `lib.presets` is, by construction, absent
  // from the next `serialize()`. Preserved unknown lines (§3 above) are
  // re-emitted beside the preset they were read with, by name; a preset
  // whose name is no longer in `lib.presets` -- because it was deleted --
  // correctly loses whatever unrecognised lines it carried along with it.
  std::string serialize(const BrushLibrary& lib) const;

  // Writes via temp-file-then-rename (§4). `errorOut` is populated on every
  // failure path, including a rename that did not complete, in which case
  // the `.tmp` file is removed rather than left beside the real one.
  bool saveToFile(const std::string& path, const BrushLibrary& lib, std::string* errorOut) const;

  // Lines before the first `preset`, verbatim.
  const std::vector<std::string>& unknownLines() const noexcept { return unknownLines_; }
  // Lines inside a `preset` scope this build did not recognise (an unknown
  // key, or a `link` whose ordinal names a source/target this build does not
  // have -- §2), keyed by the preset's name.
  const std::map<std::string, std::vector<std::string>>& presetUnknownLines() const noexcept {
    return presetUnknownLines_;
  }

 private:
  std::vector<std::string> unknownLines_;
  std::map<std::string, std::vector<std::string>> presetUnknownLines_;
};

}  // namespace np
