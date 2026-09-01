#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/DocumentLifecycle.hpp"

// app/Journal (PLAN.md "Phase 4 -- Write it out", step 9: "`core/Journal` --
// the recovery journal from ADR-0008. Serialises the document *model* on a
// timer and after every structural edit, using **the same writer as native
// save** aimed at a scratch file. Dirty tiles flush on the same timer **for
// the active document**, not only on deactivate. Unclean scratch directories
// are offered on launch, named and dated"). PRD O5-O10.
//
// --- Why this is `app/Journal` and not `core/Journal` ---------------------
//
// PLAN.md names the module `core/Journal`. It cannot be in `core/` and the
// reason is the same one io/NpaintFile.hpp and app/DocumentLifecycle.hpp
// already gave for their own placement, so this is a correction of the plan's
// directory rather than of its content (recorded in PLAN.md's Deviations).
//
// The journal is defined by two dependencies. It serialises through
// io/NpaintFile's `saveNpaint()` -- PRD O7 makes that mandatory, not
// incidental -- and what it serialises is an `app::OpenDocument`, because a
// `core::Document` alone does not know the path it is bound to, does not hold
// PRD I10's carry bag, and has no dirty state to decide *whether* to write.
// A `core/` module that included `io/NpaintFile.hpp` and
// `app/DocumentLifecycle.hpp` would invert the dependency direction
// core/Layer.hpp states outright ("app/ depends on core/, never the
// reverse"), and would put a bag of EXR header attributes inside the domain
// model, which is precisely what both of those headers refused to do.
//
// `app/` is also where the thing being journalled already lives: the session
// of open documents is `app::AppState::documents`, and a journal is
// session-scoped, per-process state with a directory and a lock -- the same
// category as the recent-documents list, not the same category as `Layer`.
//
// --- What a journal entry is, exactly -------------------------------------
//
// Two files per document, in one per-session scratch directory:
//
//   doc-0001.npaint    the model, written by `saveNpaint()` -- the same
//                      writer native save uses, aimed at a scratch path
//                      (PRD O7, O9). Layers, their tiles, every `np:*`
//                      layer attribute, and the whole PRD I10 carry bag.
//   doc-0001.journal   the sidecar: everything an `OpenDocument` holds that
//                      a `.npaint` has no place for, plus the integrity
//                      record that makes a truncated model file detectable.
//
// The split is not "model here, extras there" by convenience -- it is the
// line between `core::Document` and `app::OpenDocument`. Everything in the
// sidecar is a *lifecycle* fact, never document content: the bound path, the
// title, the document id, the revision/savedRevision pair, the edit labels,
// the residency mode. A `.npaint` deliberately records none of those (a
// document file does not know which file it is, nor that it has three
// unsaved changes), so writing them beside it invents no second serialiser
// for the model and PRD O7 stays true in the sense that matters: **there is
// exactly one piece of code in this application that turns layers and tiles
// into bytes.**
//
// --- What is NOT journalled, and why that is the format's problem ---------
//
// PRD O5 lists "layers, op stacks, masks, dabs, selections, paths". This
// build journals the first of those and none of the rest, and the reason is
// worth stating precisely because it looks like under-delivery:
//
//   **Anything `saveNpaint()` cannot write, the journal cannot recover.**
//
// That is the correct coupling rather than a defect in this module. PRD O7
// forbids a second serialiser, so the journal's coverage is by construction
// identical to native save's, and io/NpaintFile.hpp's deferral list is
// therefore this module's deferral list too, unchanged and with the same
// unblocking conditions: masks (Phase 5 step 4), dabs (a Strokes layer that
// holds any), selections (`core::Document` holds none), paths (no in-memory
// representation), pigment latents (Phase 5 step 3).
//
// The op stack deserves its own sentence because it is the one item on that
// list which exists today. `core::OpStack` is real, but it lives on
// `app::AppState`, is session-global rather than per-document, and
// io/NpaintFile.hpp records two separate blockers to writing it: no
// Layer/Document owns one, and `np:ops` needs a blob attribute, which that
// header *measured* as not surviving this OpenImageIO's EXR writer at all.
// Journalling it here would mean inventing a `core::Op` encoding in a
// recovery module -- a format decision taken in the wrong place, in a second
// serialiser, for data that would then not be loadable by native save. So it
// is deferred, and the honest consequence is stated rather than buried: **the
// PRD's own verification line "kill -9 mid-session, then recover -> layer
// structure and op stacks intact" is half met today.** Layer structure yes;
// op stacks no. Unblocked by io/NpaintFile gaining `np:docOps` (which needs
// the hex/base64 string carrier its header already names as the cheap fix),
// at which point the journal gets it for free with no change here.
//
// --- The canvas gap, stated as plainly as every step before this one ------
//
// `sim::PaintSim` owns one dense GPU texture with no layer awareness, so a
// painted stroke reaches no `Layer::rgbTiles` anywhere and the painting
// canvas is not a `core::Document` (app/DocumentLifecycle.hpp's own section
// on this is unchanged by this step). **So the journal cannot today recover
// what is on screen.** It recovers open documents -- what was opened, plus
// whatever document-level operations have been applied to them -- and a
// session spent painting on the canvas journals nothing, because nothing in
// the session ever became dirty. This is the single most important
// limitation of this step and it is not hidden behind a nicer sentence:
// recovery is complete for the model, and the model does not yet include the
// pixels a user is actually painting.
//
// What makes that bridge land cleanly rather than forcing a redesign here:
// the trigger this module keys on is `OpenDocument`'s existing revision
// counter, so a canvas-to-tiles path that writes tiles and calls
// `recordEdit()` -- exactly the shape app/DocumentLifecycle.hpp already
// promised the bridge would take -- becomes journalled with no change to
// this file.
//
// --- The timer, and why the interval is the number it is -----------------
//
// See kJournalIntervalSeconds below for the measurement and the arithmetic.
//
// --- NP_USE_OIIO=OFF: the journal does not run, and says so --------------
//
// `saveNpaint()` is OpenImageIO-only and refuses by name without it, so **the
// recovery journal does not work in the NP_USE_OIIO=OFF build** -- which is
// the *default* build (`NP_USE_OIIO` defaults OFF; see PLAN.md Phase 4 step
// 3's note). PRD O5-O10 are P0 data-safety requirements, so that is a serious
// thing to be accurate about rather than to phrase around.
//
// The three options were: journal a subset through a second writer, which PRD
// O7 forbids outright and which would produce recovery files this application
// could not read back; refuse to start, which is absurd for the default build
// of a painting application; or run without a journal and say so, once, by
// name. This module takes the third. `journalAvailable()` is false,
// `journalUnavailableReason()` forwards **io/NpaintFile's own refusal**
// verbatim (naming `.npaint`, `NP_USE_OIIO` and the cmake line -- this module
// invents no second vocabulary, exactly as app/DocumentLifecycle does), no
// scratch directory is created, and `main()` prints the reason at startup.
//
// Discovery still works in both builds, deliberately. A directory left by an
// ON build must never be invisible to -- or silently deleted by -- an OFF
// build; being told "there is recoverable work here and this binary cannot
// read it" is strictly better than silence, and it keeps the guarantee that
// nothing is ever auto-deleted (PRD O8) true regardless of configuration.
//
// `--selftest` asserts the correct answer for each configuration through a
// single `kOiioBuild` constant and is not compiled out of either (PLAN.md
// §1.5). Roughly half the section is build-independent by construction: the
// scratch directory's naming and dating, liveness, the sidecar format, the
// truncation refusal and the whole timer are file-format-free.
namespace np {

// --- Availability ---------------------------------------------------------

// True exactly when this build has a writer for the journal to use.
bool journalAvailable();

// Empty when `journalAvailable()`. Otherwise io/NpaintFile's own refusal,
// obtained by probing `saveNpaint()` itself rather than by copying its
// wording -- the probe reaches the backend gate, which sits after every
// request check and before any file is opened, so it touches no filesystem.
std::string journalUnavailableReason();

// --- Where the scratch lives ---------------------------------------------

// `~/Library/Application Support/naturalPaint/recovery` on macOS,
// `${XDG_CONFIG_HOME:-~/.config}/naturalPaint/recovery` elsewhere, with
// `$NP_JOURNAL_DIR` overriding both.
//
// The same directory family, the same override mechanism and the same reason
// as io/ExportAs' `defaultExportPresetsPath()` and app/DocumentLifecycle's
// `defaultRecentDocumentsPath()`: user data this application writes at run
// time, so a read-only install must still be able to write it, and the
// override exists so `--selftest` never touches the developer's real state.
//
// **Not `~/Library/Caches`**, which is where a scratch directory would
// normally go on this platform. Caches hold data the system may purge and the
// application can regenerate. A recovery journal is the opposite: after a
// crash it is the *only* copy of the user's work, and a purge would be
// indistinguishable from the data loss it exists to prevent.
std::string defaultJournalRootPath();

// --- The timer ------------------------------------------------------------

// **Measured, not chosen.** ADR-0008 proposes 60 s as a default; this is the
// arithmetic that either supports it or does not, run against this build.
//
// One journal write is `saveNpaint()` over the whole document -- see the "no
// per-tile dirty set" note on `JournalSession::tick()` for why a partial
// write is not available today -- plus a read-back hash, an `fsync` and a
// rename. Measured by `--selftest` every run, on a 2048x2048 document with
// all 256 tiles occupied (32.0 MiB of half data, io/TileResidency's own
// "realistic document" fixture), RelWithDebInfo, warm file cache:
//
//     one journal write of 32.0 MiB of tiles = 0.080-0.085 s (3 runs)
//     a deep copy of the same document       = 0.002-0.003 s
//
// **The fixture is not the case the interval has to survive**, so extrapolate
// before dividing. 0.082 s for 32.0 MiB is ~2.6 ms per MiB of tile data, and
// the cost is linear in occupied tiles (one part per layer, every tile
// written). PRD's own performance table names a 4K RGB document, which is
// 510 tiles / 63.8 MiB / ~0.17 s; a realistic eight-layer 4K painting is
// ~1.4 s. That is the number I is chosen against:
//
//     duty cycle = write cost / I,  for the ~1.4 s eight-layer 4K case
//     I =  15 s -> 9.3 %
//     I =  30 s -> 4.7 %
//     I =  60 s -> 2.3 %
//     I = 120 s -> 1.2 %, and two minutes of work at risk
//
// **60 s.** It is the largest interval at which the bound on lost work is
// still a sentence a user would accept ("at most a minute"), and the smallest
// at which the big-document duty cycle stays near 2 % -- the level at which
// background bookkeeping stops being something a user can attribute to the
// application. 30 s buys 30 s of safety for twice the I/O on exactly the
// documents that can least afford it; 120 s makes the journal feel like a
// promise rather than a safety net.
//
// The stall matters less than it looks, and this is the one place the
// measurement changed the design rather than confirming it. 0.082 s is ~7x
// phase 1.1's measured 12.1 ms p50 frame, so a write *is* a visible hitch --
// but PRD O10's deferral means it can never land inside a stroke, only
// between strokes, which is where a hitch costs the least. And the deep copy
// number says the fix is cheap: moving the write to a background thread
// leaves ~0.003 s on the calling thread instead of ~0.082 s, a 27x
// reduction, **without** needing Phase 5 step 6's copy-on-write tiles first.
// I expected the snapshot copy to be most of the cost; it is under 4 % of it.
//
// **That fix has since been taken** -- see `JournalOptions::asynchronous` and
// `waitForIdle()` -- and the number that forced it was not the 0.082 s
// fixture above. Instrumented on a real 5000x2559 50-layer document, one
// journal write measured **0.99-1.21 s**, twelve times what this arithmetic
// reasoned about, and a structural edit is due *immediately* rather than on
// the interval -- so six ganged eye-icon toggles froze the window for ~6 s
// with the frame counter stopped, not merely dropping frames. The interval
// arithmetic below is unchanged and still correct for what it governs (the
// *content*-edit timer); what it never governed was the structural path,
// which has no interval to be a percentage of. On that path the only bound
// available is "not on this thread".
//
// `--selftest` prints the write cost every run and fails if it exceeds 3 % of
// this interval (a 22x margin over what is measured here), so a regression
// that makes journal writes an order of magnitude slower is a test failure
// rather than a slow application nobody profiles.
inline constexpr double kJournalIntervalSeconds = 60.0;

// --- The due-ness rule, on its own ----------------------------------------

// What `JournalSession` remembers about one document between ticks.
struct JournalEntryState {
  bool everWritten = false;
  uint64_t revision = 0;
  uint64_t structuralRevision = 0;
  double lastWriteSeconds = 0.0;
  // A due write that PRD O10 held back for an active stroke.
  bool overdue = false;
};

enum class JournalDue {
  No,
  // Never journalled, or a structural edit since it last was: write now, not
  // at the next interval (PRD O5, "after every structural edit").
  Structural,
  // A content edit, and the interval has elapsed since the last write.
  Interval,
  // Was due on an earlier tick and was deferred by an active stroke.
  Overdue,
};

// The whole timer rule, as a pure function of the document, what was last
// journalled, and the clock.
//
// Lifted out of `tick()` deliberately. `tick()` cannot run at all in the
// NP_USE_OIIO=OFF build -- there is no session to run it on -- and PLAN.md
// §1.5's rule is that an unexercised path is not a seam, so the part of the
// timer that has nothing to do with a file format is written where **both**
// builds can assert it, rather than being tested in one configuration and
// hoped for in the other. `tick()` calls this and holds no second copy of the
// rule.
//
// A clean document is never due: it is either bound to a file that already
// holds its content, or it is a blank nobody has touched.
JournalDue journalWriteDue(const OpenDocument& doc, const JournalEntryState& state,
                           double nowSeconds, double intervalSeconds);

// --- Options and results ---------------------------------------------------

struct JournalOptions {
  // Empty means `defaultJournalRootPath()`.
  std::string rootDirectory;
  double intervalSeconds = kJournalIntervalSeconds;

  // Write on a background thread instead of on the caller's (PRD O10).
  //
  // **Defaults to false, and the application opts in** -- the opposite of
  // what "the good path should be the default" would suggest, for one
  // reason: `--selftest` drives a whole day of journal timing in a few
  // milliseconds of wall clock, and every assertion it makes about *what is
  // on disk after this tick* would become an assertion about a race if the
  // write moved off the thread underneath it. Keeping the default
  // synchronous keeps every one of those sections meaning exactly what it
  // meant before this change, and `main()` -- the only caller whose thread is
  // a paint loop -- passes `true`.
  //
  // The cost of that choice is stated rather than hidden: the configuration
  // users run is then *not* the configuration most of the suite exercises,
  // which is precisely the "an unexercised path is not a seam" trap PLAN.md
  // 1.5 records. So the async path has its own dedicated sections
  // (coalescing, the snapshot's independence, shutdown, error propagation),
  // and `waitForIdle()` exists so they can assert on disk state without
  // sleeping.
  bool asynchronous = false;
};

struct JournalTickInput {
  // A monotonic clock in seconds. The caller's, not this module's, so
  // `--selftest` can drive a whole day of journal timing in no time at all
  // without a second code path for testing.
  double nowSeconds = 0.0;

  // PRD O10: "A journal write never blocks the paint loop; one that would
  // collide with an active stroke defers to the end of it." True here: a due
  // write with a stroke in progress is *not* performed, is counted, and
  // happens on the first tick after the stroke ends -- not at the next
  // interval, because the whole point is that the work at risk is the work
  // just painted.
  bool strokeActive = false;
};

struct JournalTickResult {
  // Documents this tick took responsibility for. **In the asynchronous
  // configuration that means handed to the writer, not yet on disk** -- the
  // count is of decisions, which is what every caller of it actually wants
  // (whether this tick did anything). What landed is `asyncWritesPerformed()`,
  // and it is deliberately a different number: coalescing exists to make it
  // smaller than this one.
  size_t documentsWritten = 0;
  // Entries removed because their document is clean and bound to a file, so
  // the journal has no remaining job (ADR-0008).
  size_t entriesDropped = 0;
  // Writes that were due and were held back by an active stroke (PRD O10).
  size_t deferredByStroke = 0;
  // True when the *active* document was one of the written ones. PRD O6 is
  // specifically about the active document, so it is reported specifically
  // rather than inferred from a count.
  bool activeDocumentWritten = false;
  // Wall seconds spent writing during this tick. Zero on the overwhelming
  // majority of ticks, which do nothing at all.
  double writeSeconds = 0.0;
  // Non-fatal. A journal that cannot write must never take the application
  // down with it, so every failure is reported here and the tick continues
  // with the next document.
  //
  // **In the asynchronous configuration these arrive late, by one or more
  // ticks.** The writer thread has no `JournalTickResult` to push into -- the
  // one it would want belongs to a frame that has already ended -- so it
  // parks its failures in a mutex-guarded queue and the *next* `tick()`
  // drains them into this vector. `main()`'s existing loop over `errors`
  // therefore keeps working unchanged and still prints every failure; the
  // only thing that moved is which frame prints it.
  std::vector<std::string> errors;
};

// --- The session ----------------------------------------------------------

// One running process's scratch directory, its lock, and the journal entries
// inside it.
//
// **`tick()` takes the session by const reference**, so this class cannot
// modify the documents it journals -- the same signature-level enforcement
// app/DocumentLifecycle's `saveDocumentCopy()` uses, and for a stronger
// reason: this runs on a timer, unattended, and a recovery mechanism that
// could alter document state would be a data-safety hazard of its own.
class JournalSession {
 public:
  JournalSession() = default;
  ~JournalSession();
  JournalSession(const JournalSession&) = delete;
  JournalSession& operator=(const JournalSession&) = delete;

  // Creates the scratch directory and takes its lock.
  //
  // The directory is `<root>/session-YYYYMMDD-HHMMSS-<pid>`, and it carries a
  // `session.txt` recording the start time in both epoch and local form. That
  // is PRD O8's "named and dated" written down at the moment the session
  // starts, rather than reconstructed later from a directory mtime -- which
  // would be wrong, because the mtime moves with every journal write and
  // would date a recovered session to the moment of the crash rather than to
  // when the work began.
  //
  // Fails, without creating anything, when `journalAvailable()` is false.
  bool begin(const JournalOptions& options, std::string* errorOut);

  bool active() const noexcept { return lockFd_ >= 0; }
  const std::string& directory() const noexcept { return directory_; }
  size_t entryCount() const noexcept { return entries_.size(); }

  // One timer step. Cheap enough to call every frame: with nothing due it is
  // a walk over the open documents comparing two integers each.
  //
  // A document is due **immediately** when its `structuralRevision` has moved
  // since it was last journalled (PRD O5's "after every structural edit" --
  // within one frame of it), and due **on the interval** when its `revision`
  // has moved (an ordinary content edit). That distinction is exactly the one
  // ADR-0008 draws when it rejects "journal every edit, as a full operation
  // log" for making every keystroke a disk write, and it is read from
  // app/DocumentLifecycle's existing dirty model rather than from a second
  // notion of dirtiness invented here.
  //
  // **PRD O6, and the defect ADR-0008 names.** Every dirty document is
  // written, the active one included, on the timer -- not on deactivate.
  // Since a journal entry is the whole document written through
  // `saveNpaint()`, the active document's dirty *tiles* reach disk on that
  // same tick by construction; there is no separate spill file for them to
  // miss. `--selftest` asserts this against the active document specifically,
  // with no deactivation anywhere in the test.
  //
  // **No per-tile dirty set exists**, so "only tiles dirtied since the last
  // flush are written" (ADR-0008's bounded-cost claim) is *not* what happens:
  // `core::TileStore` is an `unordered_map<TileCoord, Tile>` with no dirty
  // tracking of any kind, so every journal write is a whole-document write.
  // The granularity available today is the document, via
  // `OpenDocument::revision`, which is why an unchanged document is skipped
  // entirely. Unblocked by `TileStore` gaining a dirty set -- most naturally
  // alongside Phase 5 step 6's copy-on-write tiles, where the write barrier
  // that marks a tile dirty is the same barrier that copies it.
  //
  // **Where the write happens, and why PRD O10 is now literally true.**
  //
  // With `JournalOptions::asynchronous`, `tick()` does not write. It takes a
  // snapshot on the calling thread and hands it to one writer thread, so what
  // the paint loop pays is a `core::Document` copy -- measured at 0.003 s for
  // the 32.0 MiB fixture and ~0.000011 s per history entry for the same
  // document under Phase 5 step 6's copy-on-write tiles -- instead of the
  // whole `saveNpaint()`. PRD O10 says "a journal write never blocks the
  // paint loop"; before this change only its explicit stroke clause was
  // honoured, and a *structural* edit (which `recordLayerEdit()` makes every
  // visibility toggle) blocked outright for as long as the write took. On a
  // real 5000x2559 50-layer document that was ~1.0 s per toggle.
  //
  // Three properties make the hand-off sound rather than merely fast, and
  // each is asserted by `--selftest` rather than argued:
  //
  //  * **The snapshot is a copy, not a view.** core/TileStore.hpp's thread
  //    safety section supplies the exact rule -- "take a copy on the owning
  //    thread, hand the copy over" -- and the reason the copy is affordable:
  //    a `TileStore` slot is a `shared_ptr` with an atomic refcount, so the
  //    copy shares tiles and the main thread's next write to one sees
  //    `use_count() > 1` and unshares it. The writer thread only ever reads.
  //    What is deliberately **not** copied is `OpenDocument` itself, because
  //    it holds a `History` whose entries each hold a whole `Document`;
  //    copying the record would drag the entire undo stack onto the queue for
  //    a writer that reads none of it. `WriteJob` below holds exactly the
  //    fields `writeEntry()` reads and nothing else.
  //  * **One writer, serial.** Two writes of the same document would race on
  //    the same temp file and the same rename. A single thread draining one
  //    queue makes that unreachable instead of guarded.
  //  * **Coalescing.** A snapshot queued for a document that already has one
  //    queued *replaces* it. Six ganged eye-icon toggles are the case this
  //    exists for: without it they would be six full document saves of which
  //    five are already stale by the time they run.
  //
  // And the one piece of bookkeeping that must NOT move to the writer: the
  // entry's state (`everWritten`, the two revisions, the interval clock) is
  // updated **at enqueue time, on this thread**. `journalWriteDue()` reads
  // exactly that state, so leaving it to the writer would have every
  // subsequent frame see the same structural revision still unjournalled and
  // enqueue again -- an unbounded queue and a busy loop, not a delay. Slot
  // allocation stays here for the plainer reason that it mutates `entries_`.
  // The optimism is deliberate: a write that later fails is reported through
  // the next tick's `errors`, and the document is not retried until it
  // changes again, which preserves the same anti-retry-storm property the
  // synchronous path's failure branch was written for.
  //
  // A clean document bound to a file has its entry removed: its content is in
  // the user's own file and the journal has no remaining job (ADR-0008). Note
  // that ADR-0008 justifies this by PRD I13's read-back verification of a
  // save, which **does not exist yet** (step 8's Findings row records its
  // absence), so this trusts `saveNpaint()`'s own success return. The
  // alternative -- keeping journals for saved documents -- means every launch
  // offers recovery for documents that do not need it, which trains the user
  // to dismiss the one dialog that must never be dismissed reflexively.
  JournalTickResult tick(const DocumentSession& documents, const JournalTickInput& in);

  // Journals one document now, ignoring the timer and the stroke. What the
  // synchronous tick calls; public because `--selftest` times exactly one
  // write, and because a future "the user asked to be safe right now" command
  // is this.
  //
  // **Always synchronous, in both configurations.** It is the "right now" of
  // its own description; a version that returned before the bytes were on
  // disk would be a different call with the same name.
  bool writeEntry(const OpenDocument& doc, std::string* errorOut,
                  double* secondsOut = nullptr);

  // --- The asynchronous writer, from the outside ---------------------------

  // Blocks until the queue is empty and no write is in flight.
  //
  // Exists for two callers and no others: `--selftest`, which asserts on what
  // is on disk and must not do so by sleeping, and shutdown, which must not
  // let a write outlive the directory it is writing into. A no-op in the
  // synchronous configuration, where there is never anything in flight.
  void waitForIdle();

  // How many writes the background thread has actually performed since
  // `begin()`. The number coalescing is measured in: N structural edits that
  // produce fewer than N of these is the whole point of the queue.
  size_t asyncWritesPerformed() const noexcept;

  // True while the writer thread is inside one write. `--selftest` uses it to
  // establish, rather than assume, that a write really is in flight before it
  // tests what happens during one.
  bool asyncWriteInFlight() const noexcept;

  // Removes one document's entry files. Used when a document is saved or
  // closed; a no-op for a document that has no entry.
  bool dropEntry(DocumentId id, std::string* errorOut);

  // Clean shutdown: release the lock and remove the whole session directory,
  // so a session that ended properly leaves **nothing** behind and cannot be
  // offered for recovery next launch.
  //
  // The destructor deliberately does NOT do this. It releases the lock and
  // leaves the directory, because every path that reaches the destructor
  // without reaching `finishClean()` is a path where the application did not
  // shut down normally -- an exception, an early return, a `std::exit()` from
  // a fatal error handler -- and those are exactly the sessions whose work
  // should still be offered.
  //
  // **The writer thread is stopped and joined before anything is removed.**
  // `fs::remove_all()` racing a write into the same directory is not a
  // tidiness problem: it is a thread writing through a path whose parent is
  // being deleted underneath it, and on the other side of the race a
  // `JournalSession` whose members the thread is still reading. The join is
  // the whole fix and it is first, not last.
  bool finishClean(std::string* errorOut);

 private:
  struct Entry {
    uint32_t slot = 0;
    JournalEntryState state;
  };

  // One document, frozen at the moment `tick()` decided it was due, holding
  // **exactly** what `writeEntry()` reads and nothing else.
  //
  // Not an `OpenDocument`: that record holds `History history`, whose entries
  // each hold a whole `core::Document`, so copying one would put the entire
  // undo stack on a queue for a writer that never looks at it. The two
  // derived strings are precomputed here rather than on the writer for the
  // duller reason that `documentDisplayName()` and `unsavedWorkSummary()`
  // take an `OpenDocument`, which by then no longer exists.
  struct WriteJob {
    Document document;
    NpaintCarry carry;
    DocumentId id = 0;
    uint32_t slot = 0;
    std::string path;
    std::string title;
    std::string displayName;
    std::string unsavedSummary;
    uint64_t revision = 0;
    uint64_t savedRevision = 0;
    uint64_t structuralRevision = 0;
    TileResidencyMode residencyMode = TileResidencyMode::Eager;
    std::vector<std::string> unsavedEdits;
    size_t unsavedEditsDropped = 0;
  };

  std::string modelPathForSlot(uint32_t slot) const;
  std::string sidecarPathForSlot(uint32_t slot) const;

  // The snapshot, taken on the caller's thread. `slot` is passed in because
  // allocating it mutates `entries_`, which only the owning thread may touch.
  WriteJob snapshotOf(const OpenDocument& doc, uint32_t slot) const;
  // The bytes, from a snapshot. Both `writeEntry()` and the writer thread go
  // through this and there is no second copy of the write.
  bool writeJob(const WriteJob& job, std::string* errorOut, double* secondsOut);

  void startWriter();
  // Sets the stop flag, wakes the writer, joins it, and abandons whatever is
  // still queued. Idempotent, and safe to call on a session that never
  // started one.
  void stopWriter();
  void writerLoop();
  void enqueue(WriteJob&& job);

  std::string directory_;
  int lockFd_ = -1;
  double intervalSeconds_ = kJournalIntervalSeconds;
  bool asynchronous_ = false;
  uint32_t nextSlot_ = 1;
  std::map<DocumentId, Entry> entries_;

  // --- Shared with the writer thread ---------------------------------------
  //
  // Everything above this line belongs to the owning thread alone. Everything
  // below is guarded by `queueMutex_`, with the two counters atomic so
  // `--selftest` can observe them without taking the lock the writer needs.
  mutable std::mutex queueMutex_;
  std::condition_variable queueSignal_;
  std::deque<WriteJob> queue_;
  bool writerStop_ = false;
  // The id of the job the writer currently holds, or 0. `dropEntry()` needs
  // it: removing a document's files while its own write is in flight would
  // put them straight back.
  DocumentId inFlightId_ = 0;
  std::thread writer_;
  std::atomic<size_t> writesPerformed_{0};
  std::atomic<bool> writeInFlight_{false};

  // Failures the writer had nowhere to report, drained by the next `tick()`.
  // A second mutex rather than `queueMutex_` so a failing write never
  // contends with the enqueue path it is trying not to block.
  mutable std::mutex errorMutex_;
  std::vector<std::string> pendingErrors_;
};

// --- Discovery and recovery ------------------------------------------------

// One journalled document found on disk.
struct RecoveryDocument {
  std::string modelPath;
  std::string sidecarPath;
  // What to call it in the offer: the bound file's name when it had one, the
  // remembered title otherwise.
  std::string displayName;
  // The file the document was bound to when it was journalled, or empty for
  // one that had never been saved. Recovery restores this binding, so Save
  // goes where the user expects -- but never writes to it on its own (PRD
  // O9).
  std::string boundPath;
  uint64_t modelBytes = 0;
  // app/DocumentLifecycle's own PRD I11 sentence, as it stood when the
  // document was journalled, so the offer can say *what* is at stake.
  std::string unsavedSummary;
  std::string writtenAtLocal;
  // False when the sidecar is unreadable, truncated, or disagrees with the
  // model file's size or hash. The entry is still listed -- a damaged journal
  // the user can see is better than one silently omitted -- but
  // `recoverDocument()` refuses it and `problem` says why.
  bool intact = false;
  std::string problem;
};

// One unclean scratch directory.
struct RecoverySession {
  std::string directory;
  // PRD O8's "named and dated". Local time, as recorded when the session
  // started.
  std::string startedAtLocal;
  int64_t startedAtEpoch = 0;
  long pid = 0;
  std::vector<RecoveryDocument> documents;
  // Anything about the directory itself that could not be read.
  std::vector<std::string> problems;
};

// Every unclean scratch directory under `root` that no live process holds,
// newest first. `root` empty means `defaultJournalRootPath()`.
//
// **Reads only.** Nothing is opened, moved, rewritten or deleted -- PRD O8's
// "never opened silently, never auto-deleted" is a property of this function
// having no write path at all, not of a policy applied on top of one.
//
// --- Liveness, and the stale lock that must not lock anyone out ----------
//
// A session directory exists exactly while a session is unclean, because
// `finishClean()` removes it. So "is this directory a crashed session or a
// running one?" is the only question left, and getting it wrong in the
// cautious direction is unacceptable: a machine that lost power leaves a lock
// file behind, and if that lock made the directory invisible the work would
// be unrecoverable forever.
//
// The authority is an **`flock()` probe**, not the recorded pid. A `flock`
// lives in the kernel and is released when the holding process dies for any
// reason, power loss included -- so a lock that survives a crash cannot
// exist, and the failure mode this bullet is about is structurally
// impossible. A pid check would have exactly the wrong properties: pid reuse
// after a reboot would make a crashed session look alive and hide it forever.
// The pid is recorded and reported for a human reading the directory, and is
// never consulted here.
//
// The probe can also fail for reasons that are not "held" -- `flock` is
// unsupported on some network filesystems, and a home directory can be one.
// **Every such failure lists the session anyway.** The two errors are not
// symmetrical: offering a live session's directory costs the user a confusing
// duplicate in a list, and since recovery never deletes and never writes to
// the user's own file, nothing worse; hiding a crashed one costs them the
// work.
std::vector<RecoverySession> discoverRecoverySessions(const std::string& root = {});

// Reads one journalled document back into `*out`.
//
// The integrity record in the sidecar (byte count and hash of the model file
// as written) is checked **before** `loadNpaint()` is called, so a truncated
// or corrupted journal is refused by name with the two sizes in the message
// rather than half-loaded into a document that looks plausible. That check is
// this module's own and does not depend on how OpenImageIO happens to react
// to a short file -- which is why it is asserted in both build
// configurations.
//
// **Read-only, on success and on failure alike.** Nothing in the scratch
// directory is moved or removed, because a recovery path that can lose the
// journal it was recovering from is worse than no recovery path. Discarding
// is a separate, explicitly named call below.
//
// The recovered document comes back **dirty**, bound to its original path if
// it had one, with its edit labels restored -- so the user sees what is
// unsaved, and nothing is written anywhere until they decide to write it.
DocumentOpResult recoverDocument(const RecoveryDocument& entry, OpenDocument* out);

// Deletes one scratch directory, with everything in it.
//
// The only code path in this module that removes a session someone else
// wrote, and it exists only so the user can say "throw this away" (PRD O8:
// never auto-deleted -- so it must be deliberately deletable, or the
// directory accumulates forever). Never called by discovery, never called by
// recovery, and never called on a successful recovery either: a user who
// recovers a session and then loses power before saving should still find it
// next launch.
bool discardRecoverySession(const RecoverySession& session, std::string* errorOut);

}  // namespace np
