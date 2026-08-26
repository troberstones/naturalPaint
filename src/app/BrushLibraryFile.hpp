#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "brush/Library.hpp"

// **This header deliberately does not include `app/DabPreview.hpp`.**
// `app/AppState.hpp` holds a `BrushLibraryStore` and DabPreview needs
// `BrushState`, which lives in AppState -- so including it here would close a
// cycle. The two functions that turn a row into a rasterisable tip are in
// `app/BrushRowIcon.hpp` instead, which sits above both; the reasoning for
// them is §4 below, because that is where the decision was made.

// app/BrushLibraryFile -- **which `.abr` brush libraries this install has
// loaded, remembered across launches, and read only when a brush from one is
// actually picked.**
//
// ==========================================================================
// 0. The shape of the problem
// ==========================================================================
//
// `io/AbrBrushes` can read a Photoshop `.abr`. Until this module there was
// nowhere to put the result: an import lived until the process exited and the
// BRUSH LIBRARY pane had no way to offer one. So this is three features that
// only make sense together --
//
//   * a **preferences file**, recording which libraries are loaded;
//   * a **row cache**, so a remembered library draws its brushes at launch
//     with its `.abr` unread;
//   * an **unload**, which has to remove exactly one library's presets and
//     nothing else, and has to take the library out of the preferences file or
//     it walks back in next launch.
//
// The whole thing is in service of CONTEXT.md's *Lightweight*: "a feature-rich
// application is lightweight if nothing allocates until used". A 2.4 MB Kyle
// Webster pack is 2.4 MB of file read plus a whole Action Descriptor tree
// parsed into a `DescriptorTree`, for twelve brushes the user may not touch
// this session. Paying that at launch, for every pack a working painter has
// installed, is exactly the cost PRD A2 and ADR-0001 exist to refuse.
//
// PRD G6 ("presets save, load, and carry a thumbnail") and G7 (".abr import
// including dynamics") are what this closes; G9's import report is
// `io/AbrBrushes`' `notes`, surfaced by the pane.
//
// ==========================================================================
// 1. The file, and why it is lines rather than JSON
// ==========================================================================
//
// **There is no JSON writer in this codebase.** `app/Keymap.cpp` has a
// hand-rolled reader for exactly the subset the keymap schema uses and says of
// itself "not a general JSON parser -- there is no JSON library here";
// `io/ExportAs.cpp` writes `export-presets.json` through its own small
// emitter. Adding a third partial JSON implementation to store a list of file
// paths would be the least defensible line in the module.
//
// The precedent that fits is `recent-documents.txt` (app/DocumentLifecycle.hpp
// §"Storage"): a plain, line-based file in the same directory, one record per
// line, parsed leniently. This is that, with a key word at the front of each
// line because there is more than one kind of record.
//
//     naturalPaint-brush-libraries 1
//     library /Users/ada/Brushes/runny_inkers.abr
//     size 2412345
//     mtime 1755820800
//     row 20 0.35 1 0 0.5 0.9 3 Kyle's Inker 7
//     row 46 0.12 0.28 35 0.12 0.7 2 Big Wash
//     active 1 0
//
// A line is `<key> <rest>`. `library` takes the whole rest of the line as a
// path, so a path with spaces needs no quoting and no escape mechanism -- the
// same trick `recent-documents.txt` uses, and the reason neither file has an
// escaping bug to have. `row` takes seven numbers and then the whole rest of
// the line as a name, for the same reason. Keys are scoped: `size`, `mtime`
// and `row` belong to the most recent `library` line above them.
//
// **Nothing in this file is ever refused wholesale, including its own header
// line.** There is no invariant here that spans lines -- every line is
// independently meaningful -- so there is nothing a header check could
// protect, and a file that refuses to load costs the user every library they
// had. A first line that is not the expected token is treated as an unknown
// line (§3) and the rest of the file is read normally. `--selftest` feeds this
// a truncated file, a file with a garbage line and a file with an unknown key,
// and asserts what *survives* each, not merely that nothing crashed.
//
// ==========================================================================
// 2. What it stores, and what it deliberately does not
// ==========================================================================
//
// **The path, so the library can be found again.** The minimum.
//
// **The size and the modification time**, so a `.abr` that was edited, swapped
// or rebuilt under the same name is *detected* rather than silently believed.
// Without them the row cache below is a claim about a file that may no longer
// say any such thing, and the failure -- rows naming brushes the pack no
// longer contains -- would show up as "the library panel is lying", which is
// the hardest kind of bug to attribute. Two numbers is not a content hash and
// does not pretend to be: a file edited in place with the same length inside
// the same second is missed. Hashing 2.4 MB at launch to close that gap would
// reintroduce exactly the launch cost this module exists to avoid, and the
// consequence of the miss is a stale row, not lost data.
//
// **The active preset**, as `active <libraryOrdinal> <index>` -- ordinal 0
// meaning the built-ins, N meaning the Nth `library` line in this file. Not a
// flat index into `BrushLibrary::presets`, because how many presets a
// remembered library contributes is not known until it is read, so a flat
// index would point somewhere different depending on what happened to load.
//
// **Not the presets themselves.** A preset is a name, seven numbers and a
// whole `BrushLinkSet` of links each carrying an editable `Curve`; writing
// those out would be a second serialisation of the brush model, kept in step
// with `brush/Library.hpp` by hand, and -- the real objection -- it would be a
// *copy* of the `.abr` rather than a *cache* of it. The two have different
// invalidation rules and putting them in one file makes the wrong one apply:
// a copy is authoritative and must be migrated when the model changes; a cache
// is disposable and is thrown away when its source moves. §3 is a cache.
//
// **Not the pigment, and not the tool.** `brush/Library.hpp` already refuses
// to put those in a preset, and the reasoning there ("a library entry that
// restored a colour would mean picking a brush silently repaints in whatever
// colour it was saved with") applies with more force to a file that survives
// a relaunch.
//
// ==========================================================================
// 3. Forward compatibility: unknown lines are kept, not dropped
// ==========================================================================
//
// A file written by a later version must not make *this* version lose data it
// did not understand. Three ways were considered:
//
//   * **Refuse a file whose version is newer.** Rejected. It turns "you opened
//     the older build once" into "your brush libraries are gone", which is the
//     worst possible reading of a compatibility check.
//   * **Drop lines we do not recognise.** Rejected, and it is the tempting one
//     because it is what a strict parser does by default. Running the old
//     build for five minutes would silently strip whatever the new one had
//     added, and the user would find out on the next launch of the new build.
//   * **Keep them verbatim and write them back out**, in the scope they were
//     found in. Chosen. An unknown line before the first `library` is a
//     file-level line; one after it belongs to that library and travels with
//     it -- including out of the file, when that library is unloaded, which is
//     correct: it was that library's data.
//
// The rule that makes this work, and it is a rule for future versions of *this*
// file rather than a property of the parser: **positional keys are frozen.**
// `library` is a path, `row` is exactly seven numbers and a name. A later
// version that wants an eighth number on a row must add a new key beside it,
// never an eighth field, because an older build reading an eighth field would
// fold it into the brush's name. `--selftest` asserts the unknown-key round
// trip; it cannot assert this rule, so it is written here where the next
// person to extend the format will read it.
//
// ==========================================================================
// 4. Lazy: what a row needs, and the tension with the icon
// ==========================================================================
//
// The user's own framing of the hard part: *"cache an icon for the brush so
// that when the app loads we don't pay a price for brush libraries until the
// user needs to use a brush"*. And the tension: an icon rasterised at import
// and stored costs disk and goes stale; an icon rasterised on demand costs
// nothing and needs the preset's parameters -- which is what lazy loading is
// avoiding.
//
// **The tension dissolves once "the preset's parameters" stops being treated
// as one indivisible thing.** A `BrushPreset` is two very different halves:
//
//   * seven scalars and a name -- about sixty bytes;
//   * a `BrushLinkSet`, a vector of links each with a source, a target, a
//     range, an invert flag and an editable `Curve` of control points.
//
// The dynamics matrix is the expensive half, it is the half that needs the
// `.abr` parsed, and **it is the half a library row does not draw.** So the
// cache stores the scalars (`BrushRow` below) and rasterises the icon on
// demand from them. No pixels on disk, nothing to go stale beyond the numbers
// themselves, and no `.abr` read to draw a row.
//
// The rasteriser is `app/DabPreview`'s, unchanged and uncopied -- a row's icon
// goes through `rasteriseDabPreview()`, which goes through the deposit's own
// `dabCoverage()` and `depositTexel()`. `--selftest` asserts the thing that
// makes the whole design true rather than merely plausible: **for a preset
// with no links, the cached row alone produces the byte-identical image the
// fully-loaded preset does.** The cache lost nothing an icon needs.
//
// The honest limit, stated rather than discovered: for a preset that *does*
// have links, an unloaded row previews at neutral dynamics -- three identical
// cells rather than the pressure family §2 of app/DabPreview is about, because
// the family is a picture of the links and the links have not been read. The
// row says so (`BrushRow::linkCount` is cached, so the pane can say "3 links,
// not loaded yet") and the picture corrects itself the instant the brush is
// picked. The alternative -- caching enough of the link set to preview it --
// is caching the expensive half after all.
//
// **Cost, stated in numbers rather than adjectives.** Per preset on disk: the
// `row` line, roughly 40-70 bytes depending on the name. A twelve-brush pack
// is under a kilobyte; ten packs are under ten. Per preset at launch: one line
// parsed, seven `strtof` calls. Per remembered library at launch: **one
// `stat()`** -- not a read -- which is what lets the pane tell the truth about
// a moved or edited file before the user clicks rather than after. What is
// *not* paid at launch: opening the `.abr`, reading its bytes, and building a
// `DescriptorTree` from its `desc` block.
//
// ==========================================================================
// 5. What "first use" means
// ==========================================================================
//
// **First use is the click that selects the row**, and the three candidates
// are not close:
//
//   * **Hover.** Rejected outright. A mouse crossing the pane on its way
//     somewhere else would read every `.abr` on the machine, which is worse
//     than eager loading because it is eager loading at an unpredictable
//     moment.
//   * **The start of a stroke.** Rejected, and it is the one that sounds most
//     faithful to "until the user needs to use a brush". Between the click and
//     the stroke, the BRUSH EDITOR's sliders, its tip preview and its EDITED
//     badge are all reading the live brush -- so the pane would show a row
//     selected while every control beside it described the *previous* brush.
//     That is the exact state the pane must not be able to reach.
//   * **The click.** Chosen. Selection is the moment a preset's parameters
//     become observable, so it is the moment they have to exist.
//
// A library is read at most once per session: `useLibrary()` is a no-op on one
// already loaded, and `abrReads()` is public so `--selftest` can assert the
// second click reads nothing -- a counter that only ever goes up is the only
// way to catch a cache that quietly re-reads.
//
// **A load that fails is never a no-op.** The `.abr` may have moved, been
// deleted, been replaced with something else, or live on a volume that is not
// mounted. In every case:
//
//   * `lib.active` does **not** move and the live brush does **not** change,
//     so nothing silently becomes something else;
//   * the library's `status` becomes `Failed` and its `failure` string **names
//     the file** and says what happened;
//   * the pane draws that string and offers *Retry* (the common real case is a
//     volume that was not mounted yet) and *Remove* (which unloads, and takes
//     the library out of the preferences file).
//
// Clicking the row again is itself a retry, so the failure cannot wedge into a
// state where the only remaining action is to restart.
//
// **A preset remembered as active inside an unloaded library is not restored
// at launch**, and that is the feature rather than a shortfall: restoring it
// would mean reading that `.abr` during startup, which is the one thing this
// module exists to prevent. `lib.active` stays on a built-in, the pane marks
// the remembered row, and the user pays for it by clicking it. `--selftest`
// asserts `abrReads() == 0` after a preferences load that names a library
// preset as active.
//
// ==========================================================================
// 6. Unload, and the active preset
// ==========================================================================
//
// The built-ins carry `libraryId == 0` and there is no library with id 0, so
// there is no call that can remove them -- not a guard that can be forgotten,
// an id that does not exist.
//
// When the **active** preset belongs to the library being unloaded,
// `lib.active` falls back to built-in 0 and **the live brush is left exactly
// as it is.** Both halves are deliberate:
//
//   * Changing the live brush would alter the mark the user is about to make
//     because they tidied up their library list. A brush must not change under
//     a hand that did not ask.
//   * Leaving `lib.active` dangling is how the EDITED badge starts lying -- it
//     is defined as "the live fields no longer match `presets[active]`", which
//     needs an `active` that exists.
//
// The visible consequence is that the pane immediately shows **EDITED** on
// `Round Bristle 03`, which is *true*: the brush in hand is not that preset.
// It is also the recovery path, using machinery the pane already has --
// Duplicate captures the brush in hand into a preset the user owns, Revert
// throws it away. So unloading a library you are painting with tells you, in
// the badge that already means this, that you are holding something the
// library no longer contains.
//
// ==========================================================================
// 7. What is deliberately not here
// ==========================================================================
//
// **No ImGui and no GPU.** This module produces rows, presets, statuses and
// strings; `ui/MacPaintUI.cpp` owns the widgets and the texture. Same split as
// `app/DabPreview`, `app/LayerPanel` and `app/ControlsLayout`, and it is what
// lets every claim above be asserted headlessly.
//
// **No file dialog of its own.** Import goes through the one panel Open,
// Save As and Import Image use (ui/FileDialog.hpp, raised from
// `ui/MacPaintUI.cpp`'s document-lifecycle block) -- a real `NSOpenPanel`
// filtered to `.abr`. This module still takes a path and nothing else, which
// is what lets every claim above be asserted headlessly.
//
// **No writing of `.abr`.** Export is not a thing this reads toward.
namespace np {

// --- The cached row -------------------------------------------------------

// What the BRUSH LIBRARY pane draws for one brush, and everything the icon
// needs -- §4's "cheap half" of a `BrushPreset`.
//
// Not a `BrushPreset` with the links removed, deliberately: a distinct type
// cannot be handed to `applyPresetToBrush()` by accident, and a row that
// half-applied itself to the live brush would produce exactly the "selected
// but not loaded" state §5 rejects.
struct BrushRow {
  std::string name;
  float radius = 20.0f;
  float hardness = 0.35f;
  float roundness = 1.0f;
  float angle = 0.0f;
  float spacing = 0.25f;
  // `BrushState::load`, which `brushTipFor()` turns into the tip's `flow`.
  // Cached because it changes the icon -- a heavily loaded brush lays a flat
  // saturated core where a light one lays a ramp (app/DabPreview §4).
  float load = 0.9f;
  // Not drawn as a picture; said as a number, so a row can admit that its
  // dynamics have not been read yet (§4's honest limit).
  uint32_t linkCount = 0;
};

BrushRow brushRowFor(const BrushPreset& preset);

// Bit equality, not a tolerance, for `dabPreviewTipsEqual()`'s reason: these
// values come from a slider, from a `.abr` read, or from a `strtof` of a
// `%.9g` this module wrote, so two that should be equal are bit-equal.
bool brushRowsEqual(const BrushRow& a, const BrushRow& b) noexcept;

// The rasterisable form of a row is `app/BrushRowIcon.hpp` -- see the note at
// the top of this file on why it is not declared here.

// --- A remembered library -------------------------------------------------

enum class BrushLibraryStatus {
  // In the preferences file, its `.abr` not read. `rows` is the cache and is
  // what the pane draws. The normal state at launch.
  Remembered,
  // Read this session. Its presets are in `BrushLibrary::presets`, tagged with
  // this library's id, and `rows` mirrors them.
  Loaded,
  // Present on disk, but its size or mtime is not what was recorded (§2). The
  // rows are the last known ones and are drawn as such; the first use re-reads
  // and replaces them.
  Stale,
  // Not there, or not readable, at the last `stat()`. Rows still drawn -- the
  // names are how a user recognises which pack has gone missing -- and the
  // failure names the file.
  Missing,
  // A read was attempted and refused: corrupt, an unsupported version, no
  // `desc` block, unreadable. `failure` carries io/AbrBrushes' own words.
  Failed,
};

const char* brushLibraryStatusName(BrushLibraryStatus status) noexcept;

struct RememberedLibrary {
  // Minted by the store, never 0, never reused within a session. See
  // `BrushPreset::libraryId`.
  uint32_t id = 0;
  std::string path;
  // What the file was when `rows` were cached. 0/0 means "not recorded", which
  // a hand-edited file can produce and which is treated as `Stale` rather than
  // as a match -- believing an unrecorded size is how a cache stops being
  // checkable.
  uint64_t size = 0;
  int64_t mtime = 0;

  BrushLibraryStatus status = BrushLibraryStatus::Remembered;
  // Empty unless the status is `Missing` or `Failed`. Always names the path,
  // because a pane that says "could not load the library" and not *which* one
  // has told the user nothing they can act on.
  std::string failure;

  std::vector<BrushRow> rows;

  // What `io/AbrBrushes` could not bring across, kept from the read so the
  // pane can offer PRD G9's report after the fact rather than only in the
  // instant the import finished.
  std::vector<std::string> notes;

  // §3. Lines from the preferences file that belonged to this library and that
  // this version does not understand, verbatim and in order.
  std::vector<std::string> unknownLines;

  // The file name without its directory, for the pane's header.
  std::string displayName() const;
};

// --- The preferences file, parsed ----------------------------------------

// The version this build writes. Read but not enforced -- §1.
inline constexpr int kBrushLibraryFileVersion = 1;
inline constexpr const char* kBrushLibraryFileHeader = "naturalPaint-brush-libraries";

// `~/Library/Application Support/naturalPaint/brush-libraries.txt` on macOS,
// `${XDG_CONFIG_HOME:-~/.config}/naturalPaint/brush-libraries.txt` elsewhere,
// overridable with `$NP_BRUSH_LIBRARIES`.
//
// **The same shape, the same directory and the same override mechanism as
// `defaultRecentDocumentsPath()` and `defaultExportPresetsPath()`** -- one
// settings location for this application, not two. Written out a fourth time
// rather than factored out: those three are three copies of a *file* path
// function, not of a directory function, so there is no existing helper to
// call. Extracting one would touch three modules that have nothing else to do
// with brushes, which is a cleanup of its own rather than part of this.
std::string defaultBrushLibraryFilePath();

// --- The store ------------------------------------------------------------

// The result of an import or of a first use. `ok` is what the pane keys the
// selection on: it must never make a row active on a false.
struct BrushLibraryLoadResult {
  bool ok = false;
  uint32_t libraryId = 0;
  // Always populated -- a sentence for the pane, whether it succeeded or not.
  std::string status;
  size_t presetsAdded = 0;
  // io/AbrBrushes' per-brush report (PRD G9).
  std::vector<std::string> notes;
};

// One flat row of the pane: every built-in and loaded preset, plus every
// remembered library's cached rows, in draw order.
inline constexpr size_t kNoPresetIndex = static_cast<size_t>(-1);

struct BrushPaneRow {
  BrushRow row;
  uint32_t libraryId = 0;
  // Index within its library's `rows`. Meaningless for `libraryId == 0`.
  size_t rowIndex = 0;
  // Where the real preset is in `BrushLibrary::presets`, or `kNoPresetIndex`
  // when this row's library has not been read. The pane's whole lazy branch is
  // this one comparison.
  size_t presetIndex = kNoPresetIndex;
  BrushLibraryStatus status = BrushLibraryStatus::Loaded;
};

class BrushLibraryStore {
 public:
  // --- Persistence ------------------------------------------------------

  // Parse preferences text. Never fails: §1. `libraries()` afterwards holds
  // everything the text described that this version could make sense of, and
  // every line it could not is preserved for the next write.
  //
  // Does no filesystem work at all -- `refreshStatuses()` is the one that
  // `stat()`s, and it is separate so a test can parse without a disk.
  void parse(const std::string& text);

  // `stat()` every remembered library and set `Remembered`, `Stale` or
  // `Missing` accordingly. One stat per library; never opens a file.
  void refreshStatuses();

  // Apply the `active` line to `lib`. Split out from `parse()` because the
  // parse must work on text alone -- `--selftest` reads a preferences file
  // with no `BrushLibrary` anywhere near it -- and because the resolution is
  // where §5's "not restored at launch" rule lives:
  //
  //   * ordinal 0 names a built-in or a Duplicate, which exists already, so it
  //     is applied to `lib.active` directly;
  //   * a nonzero ordinal names a preset inside a library that has not been
  //     read, and reading it here would be a `.abr` parsed during startup. So
  //     it is recorded as *pending* and `lib.active` is left alone.
  //
  // An ordinal or an index that does not resolve -- a hand-edited file, or a
  // library the user removed by hand -- falls back to built-in 0 rather than
  // leaving `lib.active` pointing at nothing.
  void resolveActive(BrushLibrary& lib);

  // Read the file if it is there, `refreshStatuses()`, then `resolveActive()`.
  // A file that does not exist is not an error -- it is a fresh install.
  bool loadFromFile(const std::string& path, BrushLibrary& lib, std::string* errorOut);

  // The exact bytes `saveToFile()` writes.
  std::string serialize(const BrushLibrary& lib) const;
  bool saveToFile(const std::string& path, const BrushLibrary& lib,
                  std::string* errorOut) const;

  // The version integer read off the header line, or `kBrushLibraryFileVersion`
  // when there was no header. A value above ours means §3's preserved lines are
  // load bearing rather than theoretical.
  int fileVersion() const noexcept { return fileVersion_; }
  // Lines the parse did not understand, from before the first `library`.
  const std::vector<std::string>& unknownLines() const noexcept { return unknownLines_; }

  // --- Contents ---------------------------------------------------------

  const std::vector<RememberedLibrary>& libraries() const noexcept { return libraries_; }
  const RememberedLibrary* find(uint32_t id) const;

  // Every row the pane draws, in order: `lib.presets` first (built-ins, user
  // duplicates and any loaded library), then each unread library's cache.
  std::vector<BrushPaneRow> paneRows(const BrushLibrary& lib) const;

  // --- Operations -------------------------------------------------------

  // Import a `.abr` now. Reads the file, appends its presets to `lib` tagged
  // with a fresh id, and caches their rows. This is the one entry point that
  // reads a file the user has not asked to *use* -- because they asked to
  // import it, and an import that deferred its read could not report what it
  // dropped (PRD G9).
  BrushLibraryLoadResult importFile(const std::string& path, BrushLibrary& lib);

  // §5's first use. A no-op returning `ok` on a library already loaded; a read
  // otherwise; a retry on one that previously failed.
  BrushLibraryLoadResult useLibrary(uint32_t id, BrushLibrary& lib);

  // §6. Removes exactly this library's presets from `lib` and the library from
  // the store. Returns false, with `messageOut` set, for an id that is not
  // there -- including 0, which is the built-ins and can never be a library.
  //
  // `lib.active` is repaired when it pointed into (or past) what was removed;
  // the live brush is not touched, because this function cannot see one.
  bool unload(uint32_t id, BrushLibrary& lib, std::string* messageOut);

  // The remembered selection from the preferences file, when it named a preset
  // inside a library that has not been read (§5). `kNoPresetIndex` in
  // `rowIndex` means there is none.
  uint32_t pendingActiveLibrary() const noexcept { return pendingActiveLibrary_; }
  size_t pendingActiveRow() const noexcept { return pendingActiveRow_; }
  void clearPendingActive() noexcept {
    pendingActiveLibrary_ = 0;
    pendingActiveRow_ = kNoPresetIndex;
  }

  // --- Instrumentation --------------------------------------------------

  // `.abr` files opened and parsed this session. Public for
  // `DabPreviewCache::rasterisations()`'s reason: a lazy load that is not lazy
  // passes every test that only checks the rows came out right, so the
  // assertion has to be able to see that nothing was read.
  uint64_t abrReads() const noexcept { return abrReads_; }
  // `stat()` calls. Separate from reads because a stat is not a read and the
  // distinction is the whole of §4's launch-cost claim.
  uint64_t statCalls() const noexcept { return statCalls_; }

 private:
  RememberedLibrary* findMutable(uint32_t id);
  // The read-and-map half of `importFile()` and `useLibrary()`, so the two
  // cannot drift about what a successful load leaves behind.
  BrushLibraryLoadResult readInto(RememberedLibrary& entry, BrushLibrary& lib);

  std::vector<RememberedLibrary> libraries_;
  std::vector<std::string> unknownLines_;
  int fileVersion_ = kBrushLibraryFileVersion;
  uint32_t nextId_ = 1;
  uint32_t pendingActiveLibrary_ = 0;
  size_t pendingActiveRow_ = kNoPresetIndex;
  // The `active` line exactly as read: an ordinal into the `library` lines
  // (0 for the built-ins) and an index within whatever that names. Kept raw
  // until `resolveActive()` because `parse()` has no library to resolve
  // against.
  int activeOrdinal_ = 0;
  size_t activeIndex_ = 0;
  uint64_t abrReads_ = 0;
  uint64_t statCalls_ = 0;
};

}  // namespace np
