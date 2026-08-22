#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Document.hpp"

// core/History (PLAN.md "Phase 5 -- Stack it", step 7: "`core/History` -- a
// **linear list with a cursor**, not a stack: undo moves it back, **redo**
// moves it forward, and a new edit at a non-end cursor truncates the tail.
// Undo bounded in *bytes*, compressed, tail spilled to `mmap` scratch (PRD A9,
// O1). **Snapshots** are explicit entries exempt from eviction (O4)"). See
// ADR-0005's amendment, "redo, and history as a list", which is the design
// authority for everything below.
//
// ==========================================================================
// A CORRECTION TO THIS STEP'S SPECIFICATION, WITH THE NUMBERS
// ==========================================================================
//
// Two of the four things PLAN.md's sentence asks for are built here (the
// cursor, and the byte bound). **Compression and the `mmap` spill are not,
// and the reason is that Phase 5 step 6 measured the ground out from under
// them.** Stated plainly and up front, the way Phase 4 step 6's correction
// block is, because it is the first thing a reviewer will look for.
//
// The step's wording was written when a history entry was assumed to be a
// *deep* copy of something -- pixels the history alone owned, which could
// therefore be shrunk or moved to disk without anyone else noticing. Step 6
// made a `Document` copy share every tile through a `std::shared_ptr` slot,
// and that changes *whose bytes they are*:
//
//   [measured, `--selftest` section `history`, a baseline plus ten entries
//    over io/TileResidency's realistic 2048x2048 / 256-tile / 32.0 MiB
//    fixture, one tile edited between each -- the same fixture step 6's `cow
//    tiles` section uses, so the two rows are directly comparable]
//
//     what the eleven entries SHOW        352.00 MiB
//     distinct tile bytes they refer to    33.25 MiB
//     attributable to the history alone     1.25 MiB   (3.8 % of the above)
//     the live document                    32.00 MiB   (96.2 %)
//
//   **Compression and spill can only ever touch the 1.25 MiB.** The other
//   96.2 % is the live document's own tiles, which history merely points at;
//   compressing or paging one out would compress or page out the picture the
//   user is looking at. So the ceiling on what a *perfect* compressor could
//   save here -- ratio infinity, zero CPU -- is 3.8 % of the process's tile
//   footprint, and the ceiling on the `mmap` spill is the same 3.8 %. No
//   compressor had to be linked to establish that; it is an upper bound and
//   it is measured, not estimated.
//
// The unfavourable regime is measured too, because quoting only the flattering
// one would be dishonest. When every edit rewrites *every* tile (the same ten
// entries, whole-document edits) the split inverts -- 320.00 MiB attributable
// of 352.00 MiB distinct, 90.9 %, against a 32.00 MiB live document -- and
// there compression really
// would buy something. It buys a **constant factor on a budget that is already
// a constant**: a 2x compressor at a 128 MiB budget holds exactly as many
// entries as no compressor at a 256 MiB budget, and the second costs nothing
// to write, nothing to decompress on the undo press ADR-0005 already identifies
// as the latency-critical path, and adds no failure mode.
//
// And the mechanism price is not small. Both compression and spill need a
// *fault-in point inside the tile slot* -- somewhere to notice that this tile
// is not resident and to materialise it before `find()` hands out a `const
// T*`. `TileStoreOf<T>::Slot` is a bare `std::shared_ptr<T>` and `find()`
// returns a pointer straight into it; adding residency there would put a
// paging backend inside `core::TileStore`, which is exactly the design
// io/TileResidency.hpp rejected for three stated reasons -- the first being
// "core/ is the domain model and knows nothing about files" -- and which
// core/TileStore.hpp lists under "still explicitly NOT here". A second,
// history-private spill mechanism next to io/TileResidency's is the "two
// copy-on-write mechanisms in one codebase is a smell" problem one step after
// step 6 argued its way out of it.
//
// **So A9 is delivered in its first clause and deferred in its second: undo
// history here is bounded in bytes, and its tail is dropped rather than
// spilled.** The unblocking condition is stated so it is not lost: a *tile*
// spilled to `mmap` scratch is io/TileResidency's job, not this module's --
// it already owns "where do these bytes come from", already owns the
// promote-on-write barrier, and already handles the failure mode a memory
// refcount cannot express. When it grows a scratch-file backend, a history
// entry gets spill for free, in one place, for the source-image tiles as
// well. `--selftest`'s `history` section prints both regimes' numbers on
// every run so this decision can be re-checked rather than trusted.
//
// ==========================================================================
// What an entry is
// ==========================================================================
//
// **Every entry holds a complete `Document`.** Step 6 measured a document copy
// at 0.000011 s and +0.0 MiB RSS for sixteen copies of a 32.0 MiB document,
// which turns "snapshot everything, every time" from the absurd option into
// the obvious one. It is also the option ADR-0005 needs; see the replay
// section below.
//
// The consequences worth naming, because they are what the rest of this
// header rests on:
//
//  * **An entry is a state, not a delta.** Nothing has to be applied to
//    anything to reconstruct it, in either direction. That is what makes
//    `jumpTo()` cost the same whether it moves one step or forty (PRD O3), and
//    what makes evicting a *middle* entry legal -- there is no chain to break.
//  * **There is no inverse operation anywhere in this file.** Undo is not
//    "apply the opposite"; it is "install the state at cursor - 1". Redo is
//    "install the state at cursor + 1". Same function, different index.
//  * **The marginal cost of an entry is the tiles the edit rewrote**, and
//    nothing else, because every untouched tile is shared with its
//    neighbours. The sharing *is* the delta; step 6 built it and this module
//    only has to count it.
//
// ==========================================================================
// Cursor semantics, precisely
// ==========================================================================
//
// `entries_` is the linear list; `cursor_` is an index into it naming the
// entry whose document **is the current state**. It is never past the end and
// is only ever meaningless when the list is empty.
//
//   begin(label, doc)   clears everything and makes `doc` entry 0. This is the
//                       baseline -- the state at open/new/duplicate -- and
//                       until a second entry exists there is nothing to undo
//                       *to*, which is the correct answer for a document that
//                       has just been opened.
//   record(label, doc)  **truncates first, then appends.** Everything strictly
//                       after `cursor_` is erased, `doc` is appended, and
//                       `cursor_` moves to the new last index.
//   undo()              cursor_ -> cursor_ - 1, returns that entry's document,
//                       or nullptr at 0. The list is not touched.
//   redo()              cursor_ -> cursor_ + 1, returns that entry's document,
//                       or nullptr at the end. The list is not touched.
//   jumpTo(i)           cursor_ -> i. This is the only traversal primitive;
//                       undo() and redo() are one-line calls to it, and PRD
//                       O2's panel click is the same call with any index.
//
// **What happens to the truncated tail's memory: it is released immediately
// and deterministically.** The erased entries' `shared_ptr` slots drop, and
// every tile version that only the tail held reaches zero and is freed at that
// instant -- no deferred sweep, no generation counter, no arena. A tile the
// surviving prefix still refers to is untouched. `--selftest` proves this by
// address (the tail's exclusive tiles are gone) and by the accounting
// (`bytes().attributable` falls by exactly the tail's contribution), not by
// asserting that a destructor was written.
//
// **A snapshot is never in the truncated tail** -- see below, and that is the
// single reason snapshots are not a flag on a list entry.
//
// ==========================================================================
// Bounded in bytes, under step 6's non-additive sharing
// ==========================================================================
//
// core/TileShare.hpp's warning is the thing this module has to be correct
// against: `documentExclusiveTileBytes()` is 128 KiB for the *oldest* entry
// and **zero for every entry after it**, and dropping all ten entries frees
// 1.25 MiB against a per-entry sum of 0.125 MiB -- ten times more. Exclusive
// bytes are a lower bound on a multi-entry eviction, never an additive
// quantity.
//
// Two consequences, and both are structural rather than careful:
//
//  1. **The budget is measured against the whole history at once, not summed
//     over entries.** `bytes().attributable` counts *distinct tile objects*
//     across every entry and every snapshot, deduplicated by address, and
//     keeps only those with no holder outside the history -- which it decides
//     per tile by comparing the number of slots inside the history that point
//     at it against `shared_ptr::use_count()`. Equal means nobody else holds
//     it, so it is the history's byte and dropping the history returns it.
//     A tile the live document also holds is *not* counted, because dropping
//     history would not return it. There is no sum of per-entry numbers
//     anywhere in this file.
//
//  2. **Eviction drops one entry and then re-measures**, in a loop, rather
//     than computing how many entries an overrun is worth and dropping that
//     many. That is not defensive coding: the arithmetic that would let you
//     predict it does not exist, because the tile two doomed entries share
//     becomes free only when the second one goes. Re-measuring is O(slots)
//     per drop and the drops are rare; `--selftest` prints the scan cost.
//
// **Where eviction takes from: the old end only, and never past the cursor.**
// The evictable range is `[0, cursor_)`. Two independent reasons converge on
// it, which is why it is a rule and not a preference:
//
//   * It is the only end that frees anything. A middle or trailing entry
//     holds nothing alone (the finding above), so evicting one is work that
//     returns zero bytes and loses a state.
//   * The other two candidates are both wrong for the user. Dropping the
//     entry *at* the cursor would leave the document on screen absent from
//     its own history, so the next undo would jump somewhere unrelated;
//     dropping entries *after* it silently destroys redo steps that PRD O2's
//     panel is at that moment drawing.
//
// **Growth is bounded, and the one case where it is not is reported rather
// than hidden.** If the cursor is at 0 there is nothing evictable, and if the
// snapshots alone exceed the budget there is nothing evictable either --
// because exempt means exempt (O4). In both cases the budget is exceeded,
// `overBudget()` says so and `budgetPressure()` names why. **What does NOT
// happen is a refusal to record**: an edit that cannot be recorded is an edit
// that cannot be undone, which is a worse failure than a soft budget
// overshoot, and it would arrive at the exact moment the user is deepest in
// an experiment.
//
// ==========================================================================
// Snapshots (PRD O4) -- and why they are a second list
// ==========================================================================
//
// ADR-0005: "Snapshots are explicit, user-created history entries that hold a
// full document state and are exempt from A9's byte-bounded eviction until
// dismissed. This is what makes 'try something risky' safe when the automatic
// tail may have been spilled or dropped."
//
// **The decision a reviewer should push on hardest in this file: a snapshot
// is the same `HistoryEntry` type, but it lives in `snapshots_`, not in
// `entries_`.** PLAN.md and ADR-0005 both say "entries", and the obvious
// reading is a `bool snapshot` on a list entry that eviction skips. That
// reading does not survive contact with truncation, and truncation is not an
// edge case -- it is the exact workflow the feature exists for:
//
//     take a snapshot -> try something risky -> undo back past it ->
//     do something else
//
// That last step truncates the tail, and the snapshot is *in* the tail. A
// flag that only eviction respects loses the snapshot at precisely the moment
// it was taken for. Making truncation skip it instead is worse: the snapshot
// then sits in the list between states it has no edit relationship with, and
// `undo()` -- which must walk the actual chain of edits -- would step into a
// state from an abandoned branch. Neither is what "until dismissed" means.
//
// So: `entries_` is the linear list with the cursor, truncatable and
// evictable; `snapshots_` is an unordered set of named states that only
// `takeSnapshot()` and `dismissSnapshot()` ever change. Their bytes count
// toward the budget (they are real bytes) and are never reclaimed by it.
// Restoring one is an ordinary edit -- `restoreSnapshot()` calls `record()`,
// so it truncates, appends and is itself undoable, which is both what
// Photoshop does and the only answer consistent with "history is a linear list
// with a cursor" (ADR-0005 explicitly does not adopt non-linear history, and a
// snapshot restore that moved the cursor sideways would be exactly that).
//
// ==========================================================================
// ADR-0005: "redo is not an inverse, it is the same keyframe replay with a
// longer dab stream" -- what this design does, and what is deferred
// ==========================================================================
//
// **Deferred, and named exactly: there is no dab stream, because no stroke
// reaches a `Layer`.** `sim::PaintSim` owns one dense GPU texture with no
// layer awareness; core/TileStore.hpp's enumeration of every writer in this
// build contains four file/loader paths and `--selftest`, and no brush. So
// what history can cover today is exactly the set of edits that really mutate
// a `core::Document`: the core/LayerOps operations (add, remove, reorder,
// duplicate, visibility, lock, opacity, blend, mask add/remove), placing an
// image as a layer, and duplicating a document. **It cannot cover a brush
// stroke, because there is no such thing yet to cover.** That is stated here
// rather than in a Findings row so a reader of this header cannot mistake its
// scope, and it is why `--selftest`'s section adapts PLAN.md's own verify
// sentence ("undo ten strokes, redo ten") to ten *layer edits* and says so in
// its output rather than quietly redefining the word.
//
// **What the design does so the amendment stays true when dabs arrive.** The
// amendment's load-bearing claim is not about dabs at all -- it is that redo
// must not be a second mechanism, and that this only works "provided history
// is a list with a cursor from the start; retrofitting it onto a stack would
// mean rebuilding the traversal". Three properties discharge that, and all
// three are true today and testable today:
//
//   1. **There is one traversal primitive.** `jumpTo(i)` is it. `undo()` and
//      `redo()` are `jumpTo(cursor - 1)` and `jumpTo(cursor + 1)`; the panel's
//      click is `jumpTo(anything)`. There is no code path that undo takes and
//      redo does not, so "redo is a second mechanism" is not a mistake that
//      can be made here later -- it would have to be added.
//   2. **Every entry is a keyframe.** ADR-0005's replay is "restore the
//      nearest solver keyframe, then replay the dab stream forward with the
//      undone stroke omitted or included". Today the nearest keyframe at or
//      before `i` is always `i` itself, so the replay range is empty and the
//      operation degenerates to the install. When Media layers land (phase 11)
//      and entries stop each carrying a solver keyframe, the *only* change
//      here is that `jumpTo()` grows a replay loop after the install; the
//      cursor arithmetic, the truncation rule, the eviction rule and the byte
//      accounting are all written in terms of "an entry" and mention nothing
//      about how a state is materialised.
//   3. **Undo and redo differ only in the sign of one increment.** Asserted,
//      not asserted-in-a-comment: `--selftest` checks that ten undos followed
//      by ten redos give a document bit-identical to before the undos, that
//      each intermediate state is bit-identical to a direct `jumpTo()` of the
//      same index, and that `jumpTo()` from either direction lands on the same
//      bytes.
//
// PRD O3 ("jumping back N entries costs one replay from the nearest keyframe,
// not N replays") is therefore already true and already measured: `jumpTo()`
// is O(1) in the distance travelled, and `--selftest` times a 1-step and a
// 40-step jump to show they are the same operation.
//
// ==========================================================================
// What is deliberately NOT here
// ==========================================================================
//
//  * **The History panel** (PLAN.md step 8, PRD O2). This module owes it
//    `entries()`, `snapshots()`, `cursor()`, per-entry `label` and `serial`,
//    `droppedEntryCount()` and `jumpTo()`, and owes it nothing else. No
//    widget, no selection state, no formatting.
//  * **Serialisation.** History is session state and is not written to a
//    `.npaint` or to app/Journal's sidecar. A `.npaint` written by a document
//    that has history is byte-identical to one written by a document that has
//    none, and `--selftest` asserts that rather than assuming it. (Journalling
//    history would also mean journalling N document states through
//    `saveNpaint()` on a timer, which is exactly the write amplification
//    ADR-0008 exists to avoid.)
//  * **A background evictor.** core/TileStore.hpp establishes that destroying
//    a snapshot store on one thread while another mutates a sharer is safe, so
//    this is *possible*; it is not *needed*, because the measured scan and
//    drop cost is well under a frame (`--selftest` prints it) and a second
//    thread would need a rule about who owns `entries_`.
//  * **Coalescing** (merging a run of small edits into one entry). A real
//    feature -- twenty pressure samples should not be twenty undo steps -- but
//    it is a property of a *stroke*, and there are no strokes. Adding a time
//    or label based coalescer now would be guessing the shape of an input
//    stream that does not exist.
namespace np {

// One state, with the label a panel row shows for it.
struct HistoryEntry {
  // What the user would recognise, in the noun form core/LayerOps'
  // `LayerOpResult::editLabel` already produces and app/DocumentLifecycle
  // already funnels: "add layer", "duplicate", "opened". PRD O2's panel shows
  // this and nothing else.
  std::string label;

  // The complete document state at this point. Shares its tiles with its
  // neighbours and with the live document; see the header.
  Document document;

  // Monotonic within one History, never reused, and NOT the index -- eviction
  // shifts every index down by one, which would silently repoint a panel row
  // or a pending "jump here" action at a different state. A stable id is what
  // a UI must key off; the index is what this class's own arithmetic uses.
  uint64_t serial = 0;
};

// What the history costs, in the three senses that are actually different.
// Confusing them is how a byte-bounded history over-evicts (core/TileShare.hpp
// says so at length; this struct is that warning made into a type).
struct HistoryBytes {
  // Summed over entries and snapshots without deduplication: what a
  // pre-copy-on-write history would have paid. Reported, never budgeted
  // against.
  size_t shown = 0;

  // Distinct tile objects the history refers to, deduplicated by address.
  // Includes tiles the live document also holds.
  size_t distinct = 0;
  size_t distinctTiles = 0;

  // Of `distinct`, the tiles no holder outside the history refers to --
  // **exactly what dropping the entire history would return to the
  // allocator**. This is the number the budget is spent against, and the only
  // one that is not an over-count.
  size_t attributable = 0;

  // How many of the above are held by snapshots and are therefore exempt from
  // eviction (PRD O4). A subset of `attributable` by construction: a tile a
  // snapshot and an entry both hold is counted here, because dropping every
  // evictable entry would still not return it.
  //
  // Not the whole non-reclaimable set -- the entry *at* the cursor is never
  // evicted either (see the header) -- but it is the part a user can act on,
  // by dismissing a snapshot, and the part PRD O4 is about.
  size_t exemptFromEviction = 0;
};

class History {
 public:
  // --- The default byte budget, and where the number comes from -----------
  //
  // 128 MiB. Derived, not chosen, from three things this build can measure
  // and one the PRD fixes:
  //
  //  * **The unit is 1024 RGB tiles.** An RGB tile is 128 KiB
  //    (core/TileStore.hpp's `static_assert`), so 128 MiB is exactly 1024
  //    superseded tile versions. Pigment tiles are 224 KiB and mask tiles
  //    32 KiB (core/Pigment.hpp, core/Mask.hpp), so the same budget is 585 or
  //    4096 of those -- which is why the budget is in bytes and not in tiles,
  //    and why PRD A9 says "bounded in *bytes*, not steps".
  //  * **What one undo step actually costs is measured, not assumed.**
  //    `--selftest`'s `history` section dirties a stroke-shaped band across a
  //    2048x2048 document -- the same fixture io/TileResidency and step 6 use
  //    -- and reports the attributable bytes one recorded edit adds, and the
  //    undo depth that gives at this budget. The measured numbers on this
  //    machine are printed on every run; measured at the time of writing:
  //    **0.12 MiB (one tile) per single-tile edit -> 1024 undo steps, and
  //    2.00 MiB (sixteen tiles) per stroke-shaped band -> 64 undo steps.**
  //    Photoshop's default history depth is 50 states, so 64 for a large
  //    stroke is the same order and 1024 for a small one is considerably
  //    better. An entry costs exactly the tiles its edit rewrote and nothing
  //    else -- there is no per-entry overhead, which is asserted rather than
  //    assumed.
  //  * **It must not make history the largest thing in the process.** PRD A4
  //    puts a watercolour Media layer at ~193 MB and A7 puts a 4K RGB document
  //    at 1:1 under 150 MB; A1 puts idle RSS under 80 MB (measured 63.1 MB).
  //    128 MiB sits below the first two -- so a full history is never the
  //    biggest allocation -- and above the third, which is the point: a
  //    history smaller than the idle baseline would be evicting while the
  //    process is still mostly empty.
  //
  // Deliberately a soft ceiling on the *history*, not on the process. It
  // cannot be otherwise: the attributable number excludes the live document
  // by design, because those bytes do not come back when history is dropped.
  static constexpr size_t kDefaultBudgetBytes = size_t{128} * 1024 * 1024;

  History() = default;
  explicit History(size_t budgetBytes) : budgetBytes_(budgetBytes) {}

  // --- Recording ----------------------------------------------------------

  // Clears everything -- entries, cursor and dropped count, but **not**
  // snapshots -- and makes `doc` entry 0.
  //
  // Called for a document that has just acquired a baseline with no history
  // behind it: new, opened, duplicated, or reverted. Reverting is the
  // interesting one and it is a deliberate choice rather than an oversight:
  // app/DocumentLifecycle's revert refusal already tells the user in so many
  // words that "there is no undo for it", and this step does not get to
  // quietly change a shipped promise. Making revert undoable is a decision for
  // the History panel (PLAN.md step 8), where it can also be shown.
  //
  // Snapshots survive, because a snapshot is exempt "until dismissed" and a
  // revert is not a dismissal.
  void begin(std::string label, const Document& doc);

  // Records one edit: truncates everything after the cursor, appends `doc` as
  // the new last entry, moves the cursor to it, and evicts from the old end
  // until the budget is met or nothing is evictable.
  //
  // On an empty history this is `begin()` -- the pre-edit state is already
  // gone and cannot be invented, so the post-edit state becomes the baseline
  // and the edit is simply not undoable. That is the honest answer for a
  // caller that never established one.
  void record(std::string label, const Document& doc);

  // Replaces the entry the cursor is sitting on, **keeping its serial**, when
  // that entry is the last one. Returns false and changes nothing otherwise.
  //
  // For one user action that reaches the document in several instalments. The
  // case it was built for is the stroke bridge: a wash dries tile by tile over
  // several seconds, and `app/StrokeBakeCycle` writes each batch into the
  // layer as it dries. Recording each batch would put three or four entries
  // named "dried paint" in the panel for one stroke, and undo would walk back
  // through a drying process the painter never performed as separate acts.
  // Amending keeps the top entry equal to the document after every batch, so
  // undo at any moment during the drying goes to before the stroke -- which is
  // the only state the user can name.
  //
  // **The serial is kept deliberately.** `app/HistoryPanel` keys its rows by
  // serial precisely so a row means the same thing across a mutation (step 8
  // proved index-keying installs the wrong state after an eviction), and an
  // amended entry is the same act with more of it done. A new serial would
  // make the row appear to be a different edit each time a tile dried.
  //
  // Refusing when the cursor is not at the end is the load-bearing guard: an
  // amend part-way through the list would silently rewrite history the user
  // has already undone past. The caller's own check -- "is the top entry still
  // the one my episode created?" -- is the other half, and this cannot make it
  // for them, because only they know which act is theirs.
  bool amend(std::string label, const Document& doc);

  // --- Traversal: one primitive, and two names for it ---------------------

  // Moves the cursor to `index` and returns that entry's document, or nullptr
  // for an out-of-range index.
  //
  // **The returned pointer is valid until the next mutation of this History**
  // (`record`, `begin`, `takeSnapshot`, `dismissSnapshot`, `setBudgetBytes`),
  // which is core/TileStore.hpp's `find()` rule restated at this level: the
  // usual call installs it immediately (`doc.document = *h.undo();`).
  const Document* jumpTo(size_t index);

  bool canUndo() const noexcept { return !entries_.empty() && cursor_ > 0; }
  bool canRedo() const noexcept { return !entries_.empty() && cursor_ + 1 < entries_.size(); }

  // nullptr when there is nothing to move to; the cursor does not move.
  const Document* undo();
  const Document* redo();

  // --- Snapshots (PRD O4) -------------------------------------------------

  // Adds a named state that eviction and truncation never touch. Returns its
  // index in `snapshots()`.
  size_t takeSnapshot(std::string label, const Document& doc);

  // "Until dismissed" (ADR-0005). Returns false for an out-of-range index.
  // Frees whatever the snapshot alone held, immediately.
  bool dismissSnapshot(size_t index);

  // Restores a snapshot **as an edit**: truncates, appends and moves the
  // cursor, so it is itself undoable and the list stays linear. nullptr for an
  // out-of-range index.
  const Document* restoreSnapshot(size_t index);

  const std::vector<HistoryEntry>& snapshots() const noexcept { return snapshots_; }

  // --- What a panel reads (PRD O2), and nothing more ----------------------

  const std::vector<HistoryEntry>& entries() const noexcept { return entries_; }
  size_t cursor() const noexcept { return cursor_; }
  bool empty() const noexcept { return entries_.empty(); }

  // How many entries eviction has taken off the old end over this History's
  // life. A panel says "N earlier states discarded" with it; nothing else
  // reads it. Truncation does NOT count here -- a truncated tail was not
  // discarded to save bytes, it was abandoned by the user.
  size_t droppedEntryCount() const noexcept { return droppedEntries_; }
  size_t truncatedEntryCount() const noexcept { return truncatedEntries_; }

  // --- The byte bound (PRD A9) --------------------------------------------

  size_t budgetBytes() const noexcept { return budgetBytes_; }

  // Re-evicts immediately, so lowering the budget takes effect at once rather
  // than at the next edit.
  void setBudgetBytes(size_t bytes);

  // Walks every slot of every entry and snapshot. O(total slots) with one
  // hash lookup per slot; `--selftest` prints the measured cost. Not cached:
  // the answer changes when the *live* document is written to (a tile the
  // history shared with it becomes history-only), and this class deliberately
  // does not hold a pointer to the live document, so there is nothing to
  // invalidate a cache on. Called once per `record()` in the common case.
  HistoryBytes bytes() const;

  bool overBudget() const { return bytes().attributable > budgetBytes_; }

  // One sentence naming the overrun and what is holding it, or empty when
  // within budget. The snapshots case is called out by name, because it is the
  // one a user can do something about.
  std::string budgetPressure() const;

 private:
  // Drops the oldest evictable entry -- index 0, when `cursor_ > 0`. Returns
  // false when there is nothing evictable, which is what terminates the loop.
  bool evictOldest();
  // Drop-one-then-re-measure, per this header's non-additive-sharing section.
  void enforceBudget();
  // `HistoryBytes::shown` alone, without the O(slots) scan. See the .cpp for
  // why `attributable <= shown` makes this a sound early-out and not a guess.
  size_t shownBytesCheap() const;

  std::vector<HistoryEntry> entries_;
  std::vector<HistoryEntry> snapshots_;
  size_t cursor_ = 0;
  uint64_t nextSerial_ = 1;
  size_t droppedEntries_ = 0;
  size_t truncatedEntries_ = 0;
  size_t budgetBytes_ = kDefaultBudgetBytes;
};

}  // namespace np
