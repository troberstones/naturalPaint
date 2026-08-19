#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/Document.hpp"
#include "io/NpaintFile.hpp"
#include "io/TileResidency.hpp"

// app/DocumentLifecycle (PLAN.md "Phase 4 -- Write it out", step 8:
// "Document lifecycle -- revert, duplicate document, save a copy, save
// incremental, open recent"). PLAN.md cites this as PRD I17; in the current
// PRD.md that requirement is **I18** ("Revert, duplicate document, save a
// copy, save incremental, open recent", P1) -- I17 is now "Export layer comps
// to files", which is phase 5. The wording PLAN.md quotes is I18's, so I18 is
// what this module implements; the number in PLAN.md is stale, not the step.
//
// --- The decision this step exists to make: who owns the open document -----
//
// Before this module there was no answer. `main.cpp` held no `core::Document`
// at all; `app::AppState` held the op stack and the view; io/ExportAs' dialog
// held function-local statics and said outright that "app/AppState's ownership
// is PLAN.md Phase 4 step 8's decision to make"; io/TileResidency's header
// said a document-level residency record "is exactly what PLAN.md Phase 4 step
// 8 has to invent". Three modules deferred the same question to here.
//
// The answer, in three parts:
//
//  1. **`OpenDocument` is the record.** A `core::Document` plus everything a
//     *file-backed* document needs that `core/` must not know about: the path
//     it is bound to, io/NpaintFile's `NpaintCarry` (PRD I10), the dirty
//     state, the residency mode, and a per-process identity.
//  2. **`DocumentSession` owns them**, as an ordered list with an active
//     index. Not a single `std::optional<OpenDocument>`, because "duplicate
//     document" produces a *second* open document and would have nowhere to
//     go -- an operation this step is required to deliver would have been
//     unimplementable against a single-document owner. Phase 5 step 14
//     ("Tabs + optional two-tab split") is the tab strip over this list, not a
//     replacement for it.
//  3. **`app::AppState` holds the session**, so the running application
//     genuinely holds documents rather than the operations below being
//     reachable only from `--selftest`.
//
// Why `app/` and not `core/` or `io/`: the record is neither. `core/` is the
// domain model and must not grow a bag of EXR header attributes -- that is
// io/NpaintFile.hpp's own stated reason for keeping `NpaintCarry` off
// `core::Document`, and io/TileResidency.hpp's for keeping residency out of
// `core::TileStore`. And it is not `io/`, because a file format module has no
// business holding the application's list of open windows. What is left is
// exactly what `app/` already is: `app::AppState` is where session-scoped,
// application-owned state lives.
//
// --- The gap this step does NOT close, stated exactly ---------------------
//
// Every prior UI-facing step's Findings row records the same missing bridge,
// and this row must not pretend to have closed more of it than it has.
//
// **What is now wired.** `app::AppState::documents` is a real
// `DocumentSession`. File > New Document, File > Open..., File > Open Recent,
// Save, Save As, Save a Copy, Save Incremental, Revert and Duplicate Document
// all act on it, in the running application, on documents that really are in
// memory. Opening a PNG or a `.npaint` puts real pixels in a real
// `core::Document` that the process holds until it is closed.
//
// **What is still not wired: the painting canvas is not one of those
// documents.** `sim::PaintSim` owns a single dense GPU texture with no layer
// or document awareness; painting a stroke writes that texture and touches no
// `Layer::rgbTiles` anywhere. So a document opened from a file and then
// painted on does not record the paint, and saving it writes what was opened,
// not what is on screen. Nothing here claims otherwise: `OpenDocument` has no
// "canvas" member, `recordEdit()` is called by the operations that really do
// mutate the document, and the UI labels the canvas as separate from the
// document rather than implying they are the same thing.
//
// That bridge is genuinely larger than this step: it needs a decision about
// which layer the solver deposits into, a GPU-texture-to-tile path with its
// own dirty tracking (PRD O6's "dirty tiles flush on the same timer"), and it
// only stops being provisional once Phase 5 gives `Document` multiple layers
// and a history. The record is shaped so that bridge slots in without a
// redesign: it lands as one more mutator on `OpenDocument` that writes tiles
// and calls `recordEdit()`, and every operation below is already correct
// against it, because each one is defined in terms of the document's tiles
// and its carry rather than in terms of where the pixels came from.
//
// --- Residency (io/TileResidency's deferred question) ---------------------
//
// io/TileResidency asked this step for "an owning record holding one
// `LayerResidency` per layer". It gets a `residencyMode` and the source
// information a residency is opened from, and **not** a vector of
// `LayerResidency` objects. The reason is that module's own measured warning:
// "a `Document` returned alongside a cached `LayerResidency` has an **empty**
// `rgbTiles` until tiles are promoted, and flattening or probing it today
// would see transparent black". Moving tile ownership out of
// `Layer::rgbTiles` today would break `saveNpaint()`, `flattenDocumentToLinear()`,
// `core/Probe`, `core/Histogram` and `ui/NaturalPaintUI` simultaneously, for
// no gain, since no consumer reads through a residency yet. So today every
// open document is `TileResidencyMode::Eager` and `Layer::rgbTiles` stays the
// single source of truth; the field records the choice so that when consumers
// are converted, the mode is already a property of the open document rather
// than a fifth thing to invent.
//
// What this step *does* take from io/TileResidency is a correctness
// obligation nobody could have discovered without it. OpenImageIO's
// `ImageCache` keeps serving a file's old tiles after the file is rewritten,
// and a residency opened *after* the rewrite passes its own size+mtime
// staleness check (the stamp is taken at open, and it matches the new file)
// while the cache underneath still holds the old pixels. Measured on this
// build: without an invalidation, a cached read of a just-overwritten
// `.npaint` returns the **previous** contents and reports success. So every
// operation here that writes or re-reads a path calls `tileCacheInvalidate()`
// for it -- revert, save, save as, save a copy and save incremental alike.
// `--selftest` proves the post-write read returns the new pixels.
//
// --- What every operation owes the format ---------------------------------
//
// All four save operations route through io/NpaintFile's `saveNpaint()` and
// hand it the document's `carry`. Not "should": there is no other writer
// here, and none of these functions assembles a part or an attribute of its
// own. That is what makes PRD I10 (unknown attributes and parts preserved
// verbatim) and PRD I12 (the composite part regenerated on every save, never
// stale) properties of one implementation rather than promises repeated five
// times -- and it is why `--selftest` can assert the carry survives every one
// of them.
//
// PRD I11 ("a save that would lose data names exactly what") shows up here as
// the confirmation `revertDocument()` requires: it is the one operation whose
// entire purpose is to discard work, so the discard is a parameter, and the
// refusal it produces without that parameter *names the edits* it would throw
// away rather than saying "unsaved changes".
//
// --- NP_USE_OIIO=OFF ------------------------------------------------------
//
// Native `.npaint` is OIIO-only, so every operation here that touches a file
// refuses in the OFF build -- but it refuses through io/NpaintFile's existing
// refusal, which already names `.npaint`, `NP_USE_OIIO` and the cmake line,
// rather than through a second message this module invents. The operations
// that touch no file work identically in both builds: creating a blank
// document, duplicating one, the incremental *naming* rule, and the whole
// recent-documents list. `--selftest` asserts the correct answer for each
// configuration through a single `kOiioBuild` constant and is not compiled
// out of either (PLAN.md §1.5).
namespace np {

// Declared here and defined at the bottom of this header: the save and open
// operations take an optional pointer to it, and the operations read better
// grouped together than reordered around one class definition.
class RecentDocuments;

// --- Identity -------------------------------------------------------------

// A per-process handle for one open document. Monotonically allocated, never
// reused within a run, and deliberately *not* derived from the path -- a
// document may have no path (never saved), two documents may transiently name
// the same path (a duplicate before its first save), and a path may change
// under a document (save as, save incremental).
//
// This is the "undo-relevant identity" the step's brief asks for. `core/History`
// is Phase 5 step 7 and does not exist, so nothing keys off this yet; what
// matters now is that it is allocated in one place with a rule that already
// answers the question history will ask, namely *is this the same document*.
// The answer that makes duplication correct is the one encoded here: a
// duplicate is a **different** document, so it gets a fresh id, and a future
// history keyed by id cannot leak one document's undo stack into its copy.
using DocumentId = uint64_t;

// Never returns 0; 0 is the "no document" value.
DocumentId allocateDocumentId();

// --- The record -----------------------------------------------------------

// How many edit labels are kept for the PRD I11 refusal message. Beyond this
// the count keeps rising but the labels stop being stored: a refusal listing
// four hundred strokes is not more informative than one listing thirty-two
// and a count, and an unbounded list on a record the application holds for
// the whole session is a slow leak.
inline constexpr size_t kMaxTrackedUnsavedEdits = 32;

struct OpenDocument {
  DocumentId id = 0;

  Document document;

  // PRD I10's carry bag, kept beside the document exactly as
  // io/NpaintFile.hpp requires ("loadNpaint() returns one, saveNpaint()
  // optionally takes one, and when PLAN.md step 8's document lifecycle lands
  // it is the open document's record that owns the pair"). This member is
  // that ownership. Empty for a document that was never loaded from a file,
  // which is precisely the case saveNpaint() documents as "writes a file with
  // nothing but what this build knows about".
  NpaintCarry carry;

  // The file this document is bound to, or empty for one that has never been
  // saved. "Bound to" is the load-bearing phrase: `saveDocument()` writes
  // here, `revertDocument()` re-reads here, and the entire difference between
  // save-as and save-a-copy is whether this member changes.
  std::string path;

  // The user-facing name when there is no path. Set by
  // `duplicateDocument()` and by `makeBlankOpenDocument()`; ignored once
  // `path` is non-empty, because a file's own name is a better label than any
  // remembered one. See `documentDisplayName()`.
  std::string title;

  // Today always Eager -- see this header's residency section for why the
  // record does not hold `LayerResidency` objects yet.
  TileResidencyMode residencyMode = TileResidencyMode::Eager;

  // Monotonic change counter. Bumped by `recordEdit()`; a save sets
  // `savedRevision = revision`; a revert bumps it *and* marks clean, because
  // the document changed (back) and anything caching a derived product must
  // re-read, even though there is now nothing unsaved.
  uint64_t revision = 0;
  uint64_t savedRevision = 0;

  // Labels for the edits since the last save, oldest first, capped at
  // kMaxTrackedUnsavedEdits. Exists so PRD I11's "names exactly what" can be
  // honoured by the one operation that discards work.
  std::vector<std::string> unsavedEdits;
  // Edits that happened after the cap was reached, so the count stays exact
  // even though the labels stop.
  size_t unsavedEditsDropped = 0;

  bool isDirty() const noexcept { return revision != savedRevision; }
  bool hasPath() const noexcept { return !path.empty(); }

  // Records one document-mutating edit. `label` is what the user would
  // recognise, in the imperative-free noun form the eventual History panel
  // (PRD O2) will want: "place image as layer", "duplicate", not "Edited".
  void recordEdit(std::string label);

  // One sentence naming exactly what is unsaved, for PRD I11 refusals and for
  // the UI. Empty when the document is clean.
  std::string unsavedWorkSummary() const;
};

// The label to show for a document: its file name when it has a path, its
// `title` otherwise, and a last-resort synthesised name so a document can
// never be nameless in a menu.
std::string documentDisplayName(const OpenDocument& doc);

// A blank document (PRD C7) wrapped in a lifecycle record. `title` is what
// the caller wants it called; empty gets "Untitled".
//
// The document starts **clean**, not dirty: a blank canvas nobody has touched
// has nothing to lose, and marking it dirty at birth would mean every "close
// without saving" prompt fires on documents with nothing in them.
OpenDocument makeBlankOpenDocument(int32_t width, int32_t height, WorkingSpace space,
                                   std::string title = {});

// --- The session ----------------------------------------------------------

// The application's open documents, ordered, with one active.
//
// `std::vector<std::unique_ptr<OpenDocument>>` rather than
// `std::vector<OpenDocument>` on purpose: a `OpenDocument*` handed to a dialog
// or a draw call must not dangle because another document was opened in the
// meantime, and a vector of values reallocates. The indirection costs one
// pointer chase per access on a list that is at most a few dozen entries.
class DocumentSession {
 public:
  size_t count() const noexcept { return docs_.size(); }
  bool empty() const noexcept { return docs_.empty(); }

  // The active document, or nullptr when none is open. This is the pointer
  // every UI path takes, and the nullptr case is why the File menu can be
  // honest about what is unavailable rather than crashing.
  OpenDocument* active() noexcept;
  const OpenDocument* active() const noexcept;

  size_t activeIndex() const noexcept { return activeIndex_; }
  // Ignored (rather than asserted) for an out-of-range index: a stale index
  // from a menu whose document was closed is an ordinary race, not a bug.
  void setActive(size_t index) noexcept;

  OpenDocument* at(size_t index) noexcept;
  const OpenDocument* at(size_t index) const noexcept;
  OpenDocument* find(DocumentId id) noexcept;

  // Adds `doc` and makes it active. Returns a stable pointer to it.
  OpenDocument* add(OpenDocument&& doc);

  // Closes the document at `index`.
  //
  // Refuses a **dirty** document unless `discardUnsavedChanges` is true, and
  // the refusal names what would be lost (PRD I11) -- the same rule and the
  // same message shape as `revertDocument()`, because they are the same
  // hazard. Returns false and leaves the session untouched on refusal.
  bool close(size_t index, bool discardUnsavedChanges, std::string* errorOut);

 private:
  std::vector<std::unique_ptr<OpenDocument>> docs_;
  size_t activeIndex_ = 0;
};

// --- Operation results ----------------------------------------------------

// The shape every operation below returns, matching io/NpaintFile's
// NpaintSaveResult rather than inventing a third result type: `error` is
// non-empty exactly when `!ok`, `warnings` are the non-fatal things the
// caller must still be told, and both are io/NpaintFile's own strings
// verbatim when the failure came from there.
struct DocumentOpResult {
  bool ok = false;
  std::string error;
  std::vector<std::string> warnings;
  // The file that was read or written, when the operation touched one. For
  // `saveDocumentIncremental()` this is how the caller learns the version
  // number that was chosen.
  std::string path;
};

// --- Open ------------------------------------------------------------------

// Opens a `.npaint` into a fresh record, binding it to `path` and taking its
// carry.
//
// `recent`, when non-null, gets `path` recorded on success -- opening a
// document is the canonical thing "open recent" is a list of.
DocumentOpResult openNpaintDocument(const std::string& path, OpenDocument* out,
                                    RecentDocuments* recent = nullptr);

// --- The five operations PRD I18 names -------------------------------------

struct RevertOptions {
  // Required to be true when the document is dirty. Deliberately a parameter
  // rather than a dialog's private business: the operation that throws work
  // away should be impossible to call *accidentally* from a menu handler, a
  // script, or a future action (PRD P1), not merely guarded by whichever UI
  // happens to call it.
  bool discardUnsavedChanges = false;
};

// **Revert** -- back to the last saved state.
//
// Re-reads `doc.path` through io/NpaintFile's `loadNpaint()` and replaces the
// document *and its carry* with what the file holds. Replacing the carry is
// the point rather than a detail: reverting is defined as "what the file
// says", so a foreign part that was in the file is in the document again
// afterwards, and one that had been added in memory since the load is gone --
// which is exactly what reverting means.
//
// Refuses, by name:
//  * when the document has no path (nothing to revert *to*);
//  * when the document is dirty and `discardUnsavedChanges` is false, naming
//    the edits that would be discarded (PRD I11);
//  * when the file cannot be read, forwarding loadNpaint()'s own error --
//    including the whole NP_USE_OIIO=OFF refusal.
//
// **On any failure the in-memory document is left exactly as it was.** The
// load happens into a temporary and is only moved in once it has succeeded,
// so a revert that fails because the file was deleted does not also destroy
// the copy in memory, which would turn a recoverable mistake into data loss.
DocumentOpResult revertDocument(OpenDocument& doc, const RevertOptions& options = {});

// **Duplicate document** -- an independent copy of the in-memory document.
//
// Three things it must get right, each of which is asserted in `--selftest`:
//
//  * **Tile storage is deep-copied.** `core::TileStore` is a plain
//    `unordered_map<TileCoord, Tile>` of uniquely-owned 128 KiB buffers today
//    (Phase 5 step 6 is where copy-on-write arrives), so the copy is a real
//    copy and painting on one cannot show up in the other. The cost is stated
//    rather than hidden: duplicating an N-tile document allocates N x 128 KiB.
//    When COW tiles land this becomes cheap without this function changing.
//  * **The carry bag comes with it.** A duplicate of a document holding a
//    newer build's Pigment part must still hold it, or "duplicate then save"
//    becomes the operation that quietly drops what PRD I10 exists to protect.
//  * **The path does NOT come with it.** The duplicate is unbound, so the
//    next Save cannot overwrite the original's file. This is the single most
//    dangerous thing a naive struct copy would get wrong, and it has its own
//    test.
//
// The duplicate is **dirty** from birth, with "duplicate of <name>" as its
// one unsaved edit -- it holds content that exists nowhere on disk, which is
// the definition of unsaved.
//
// Residency: forced to `Eager`. A cached residency is a claim on a *file*,
// and the duplicate has no file; when cached opens exist for real, this is
// the line that must promote the source's clean tiles into the copy instead.
OpenDocument duplicateDocument(const OpenDocument& source);

// **Save** -- write the document back to the file it is bound to.
//
// Refuses when the document has no path, naming save-as as what to use
// instead. Clears the dirty state, records the path in `recent`, and
// invalidates the tile cache for it (see this header's residency section).
DocumentOpResult saveDocument(OpenDocument& doc, const NpaintSaveOptions& options = {},
                              RecentDocuments* recent = nullptr);

// **Save As** -- write elsewhere *and rebind*. The document's path becomes
// `path`, so the next Save writes there. Present here as the deliberate
// contrast to save-a-copy below; the pair is meaningless apart.
DocumentOpResult saveDocumentAs(OpenDocument& doc, const std::string& path,
                                const NpaintSaveOptions& options = {},
                                RecentDocuments* recent = nullptr);

// **Save a copy** -- write elsewhere *without* rebinding.
//
// The distinction from Save As is the whole point of the operation, so it is
// enforced by the signature rather than by discipline: the document is a
// `const OpenDocument&`, so this function **cannot** change the path, cannot
// clear the dirty flag, and cannot record an edit. A reviewer does not have
// to read the body to know it does not rebind, and a later change cannot make
// it rebind without changing the declaration.
//
// It also takes no `RecentDocuments*`, and that too is a decision rather than
// an omission: "open recent" is a list of documents that have been *open*,
// and a copy never was. Offering it there invites reopening the copy in the
// belief it is the working file -- which is the same confusion the operation
// exists to avoid. (Photoshop's Save a Copy behaves the same way.)
DocumentOpResult saveDocumentCopy(const OpenDocument& doc, const std::string& path,
                                  const NpaintSaveOptions& options = {});

// **Save incremental** -- write the next version of this document and rebind
// to it.
//
// Naming scheme: `<stem>_v<NNN><ext>`, three digits minimum, zero-padded --
// `paint_v001.npaint`. Chosen because it is the versioning convention of the
// industry this file format already belongs to (a `.npaint` *is* an EXR, PRD
// I8), because `_v` is an unambiguous marker where a bare trailing number is
// not, and because zero padding makes a directory listing sort in version
// order.
//
// The rules, each of which has its own test:
//
//  * **A name that already ends in a version is incremented, not re-versioned.**
//    `shot_v007.npaint` gives `shot_v008.npaint`, and the existing zero
//    padding is preserved (so a `_v7` series stays `_v8`, not `_v008`).
//    Padding only grows, never truncates: `_v999` gives `_v1000`.
//  * **The next version is one above the highest existing sibling, not one
//    above this file.** So saving incrementally from `shot_v003.npaint` when
//    `shot_v009.npaint` exists gives `shot_v010.npaint`.
//  * **Gaps are not filled.** With `_v001` and `_v003` present the next is
//    `_v004`. Filling the hole would write a file whose version number is
//    lower than one that already exists, i.e. a "newer" file that sorts as
//    older -- which destroys the only thing the number is for.
//  * **An existing file is never overwritten.** After the rule above picks a
//    candidate, the candidate is still checked against the filesystem and
//    stepped past anything present (a differently padded `_v04` alongside a
//    `_v004`, or a file created between the scan and the write).
//  * **A trailing number without `_v` is not a version.** `render001.npaint`
//    gives `render001_v001.npaint`, because a trailing digit is at least as
//    likely to be a frame, a size or part of the name, and guessing would
//    renumber someone's series.
//
// Refuses when the document has no path: there is nothing to increment from,
// and inventing a name for an unsaved document is Save As's job.
DocumentOpResult saveDocumentIncremental(OpenDocument& doc,
                                         const NpaintSaveOptions& options = {},
                                         RecentDocuments* recent = nullptr);

// The naming rule above, on its own, so it can be tested without writing a
// file and so a UI can show the name before committing to it.
//
// Reads the containing directory (to find the highest existing sibling) but
// writes nothing. Returns false with `*errorOut` naming the path for an empty
// path or a directory that cannot be listed.
bool nextIncrementalPath(const std::string& currentPath, std::string* outPath,
                         std::string* errorOut);

// --- Open recent (the MRU) -------------------------------------------------

// One entry. `path` is stored lexically normalised and absolute, which is
// what makes dedup work for `./a.npaint` and `<cwd>/x/../a.npaint`.
struct RecentDocument {
  std::string path;
  // The file name alone, for a menu. Derived, never stored in the file.
  std::string displayName;
};

// The persisted most-recently-used list (PRD I18's "open recent").
//
// **Capacity is 10.** macOS's own Recent Items default, and the point past
// which a menu stops being scannable and becomes a search problem -- at which
// point the answer is a file browser, not a longer menu.
//
// **Storage: `~/Library/Application Support/naturalPaint/recent-documents.txt`**,
// `${XDG_CONFIG_HOME:-~/.config}/naturalPaint/recent-documents.txt` elsewhere,
// with `$NP_RECENT_DOCUMENTS` overriding both. That is io/ExportAs'
// `defaultExportPresetsPath()` exactly -- the same directory, the same
// override mechanism, and for the same reason it gave: this is user data the
// application writes at run time, not shipped data a developer hand-edits, so
// app/Keymap's `${NP_KEYMAP_DIR}` source-tree pattern is the wrong one and a
// read-only install must still be able to write it. The override exists so
// `--selftest` never touches the developer's real list.
//
// **Format: one path per line, not JSON**, and that is a deliberate departure
// from export-presets.json. io/ExportAs.hpp records that its hand-rolled JSON
// reader is the second in this codebase and that "a third consumer is when it
// should be hoisted". This would have been the third. Hoisting means editing
// app/Keymap and io/ExportAs -- two tested subsystems -- which is a refactor
// this step should not carry; and the data here is a list of strings with no
// nesting, which does not need an object syntax to begin with. So the file is
// a comment line and then paths, which is diffable, hand-editable, and needs
// no parser at all. The cost of that choice is recorded rather than hidden:
// a path containing a newline cannot be represented, so such a path is
// **refused by name** at `add()` rather than silently truncating the file.
class RecentDocuments {
 public:
  static constexpr size_t kCapacity = 10;

  // Parses the file format. Unparseable *lines* are skipped and described in
  // problems(); there is no structural failure mode, which is the practical
  // benefit of a line format -- a corrupt recent list can never stop the
  // application from starting.
  void loadFromString(std::string_view text, std::string_view sourceLabel);
  std::string serialize() const;

  // A file that does not exist is not an error (a user who has opened
  // nothing). Returns false only for a file that exists and cannot be read.
  bool loadFromFile(const std::string& path);
  bool saveToFile(const std::string& path, std::string* errorOut) const;

  // Records `path` as the most recent. Moves an existing equal entry to the
  // front rather than duplicating it, and drops the oldest beyond kCapacity.
  //
  // Equality is on the lexically normalised absolute form, so the same file
  // named two ways is one entry. Returns false with `*errorOut` for an empty
  // path or one containing a control character (see the format note above).
  bool add(const std::string& path, std::string* errorOut = nullptr);

  // Explicit removal -- what the UI offers for an entry whose file is gone.
  // Returns false if there was no such entry.
  bool remove(const std::string& path);

  void clear() { entries_.clear(); }

  // Most recent first.
  const std::vector<RecentDocument>& entries() const noexcept { return entries_; }
  const std::vector<std::string>& problems() const noexcept { return problems_; }
  const std::string& error() const noexcept { return error_; }

 private:
  std::vector<RecentDocument> entries_;
  std::vector<std::string> problems_;
  std::string error_;
};

std::string defaultRecentDocumentsPath();

// The normalisation `RecentDocuments` compares on, exposed because a caller
// that wants to ask "is this document already in the list" must ask the same
// question the list answers.
std::string normalizeDocumentPath(const std::string& path);

// True when a recent entry's file is no longer readable, with `*whyOut` set
// to a sentence naming the path and the reason.
//
// **A missing entry is never silently dropped.** io/Export's and
// io/NpaintFile's refusal style is the rule here: the entry stays in the
// list, the menu shows it disabled with this sentence as its reason, and
// removing it is a thing the user does deliberately. Silently pruning the
// list would mean a user who unplugs an external disk comes back to a shorter
// history and no explanation.
bool recentDocumentMissing(const std::string& path, std::string* whyOut);

// Opens entry `index` of `recent`.
//
// Refuses a missing file by name -- with `recentDocumentMissing()`'s sentence
// plus the fact that the entry has been kept and how to drop it -- rather
// than forwarding a lower-level "could not open" and rather than removing the
// entry as a side effect.
DocumentOpResult openRecentDocument(RecentDocuments& recent, size_t index, OpenDocument* out);

}  // namespace np
