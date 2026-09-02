# ADR-0008 — Recovery needs a model journal, not tile spill

**Status:** accepted, but **SUSPENDED IN THE SHIPPING DEFAULT** since 2026-09-01 —
see "Status note" at the end · **Date:** 2026-08-17
**Corrects:** [DESIGN-imaging.md](../../DESIGN-imaging.md) §3, "Crash recovery falls out
of it nearly free."

## Context

The design claimed crash recovery as a free consequence of the `mmap` tile spill: dirty
tiles are the only irreplaceable pixel data, they are written to a per-document scratch
file on deactivate, so a crash leaves them on disk.

**That claim does not hold.** Three gaps, each fatal on its own:

1. **It only covers deactivated documents.** The spill happens *on deactivate*. The
   document you are actively painting in — the only one whose loss would hurt — holds its
   dirty tiles in host and GPU memory and has written nothing.
2. **It saves pixels without structure.** The scratch file holds tile blobs. The layer
   list, blend modes, opacities, masks, op stacks, recorded strokes, selections, paths and
   Flats edits are not in it. After a crash you would have a bag of tiles and no way to
   know which layer any of them belonged to.
3. **Everything parametric is lost entirely.** A document whose value is mostly an op
   stack and a Strokes layer — the non-destructive workflow this design is *built around*
   — has almost nothing in the tile spill. The more correctly a user works, the less
   recovery they get.

So the design's most reassuring sentence about data safety was its least accurate one.
That matters more here than in most applications: this app is expected to hold hours of
painting, and it runs on an experimental GPU stack where "the device was lost" is a real
event rather than a theoretical one.

## Decision

**Recovery is a periodic journal of the document *model*, plus the existing tile spill for
pixels.** Two mechanisms, doing two different jobs.

**The journal.** Every document gets a scratch journal alongside its tile spill. On a
timer (default 60 s) and after any structural edit, the document model is serialised:
layer list with all properties, op stacks, masks, recorded dabs, selections, paths, Flats
parameters and edits, plus the *set of dirty tile ids and where each lives in the spill*.
That last part is what turns the tile spill from a bag of blobs into recoverable
document — the journal is the index the spill has always been missing.

**Dirty tiles flush on the same timer**, for the active document too, not only on
deactivate. This is the change that makes the active document recoverable at all. The
cost is bounded: only tiles dirtied since the last flush are written.

**Serialisation reuses the save path.** The journal is the same writer that produces
`.npaint`, aimed at a scratch file. No second serialiser to keep in step with the format —
which is exactly the defect that made PSD-native untenable, and the reason not to repeat
it here.

**On launch, unclean scratch directories are offered for recovery**, named and dated, with
the choice to open or discard. Never opened silently, and never auto-deleted.

**A saved document's journal is dropped on successful save.** I13 already reads a save back
and verifies it structurally before the original leaves memory, so a verified save is the
point at which the journal has no remaining job.

## Consequences

- Recovery is genuinely free for the parametric parts, which is the opposite of the tile
  spill's bias: the more non-destructively a user works, the *cheaper* their recovery
  becomes, because the model is small and the pixels are derived.
- Journal writes must never block the paint loop. They are a background job on already
  quiesced state, and a journal write that would collide with an active stroke is deferred
  to the end of the stroke rather than interleaved.
- The idle-RSS assertion (A1) must account for the journal writer, which should hold no
  buffers at rest.
- The claim in DESIGN-imaging.md §3 is corrected rather than deleted — the tile spill is
  still a real part of recovery, it just was never the whole of it.

## Alternatives rejected

**Autosave over the user's own file**, as many editors do. Rejected: it destroys the
"saved state" the user was relying on, and a crash mid-write would take the original with
it. The journal is always a scratch file.

**Journal every edit, as a full operation log.** Would give infinite undo across sessions
and unbounded scratch growth, and it makes every keystroke a disk write. The timer plus
structural-edit trigger gets the important property — bounded loss — without it.

**Trust the GPU/driver and skip this.** Not credible on wgpu, where device loss is a
documented, handled path in the code already.

## Status note — 2026-09-01: journalling is OFF by default

The decision below stands. What does not stand is the **write policy** built on
it, so the application now starts with journalling off and `--journal` turns it
back on. This was the user's call, made with the numbers below in front of them.

**What went wrong.** `journalWriteDue()` writes immediately on every
`EditKind::Structural` edit, and `recordLayerEdit()` (app/DocumentLifecycle.cpp)
classifies **every** `core::LayerOps` operation as structural — including the
metadata-only ones: visibility, rename, opacity, blend, lock, colour label.
Each such write is a complete `.npaint` of the whole document; there is no
delta path. On a measured real document the model was **162,943,474 bytes**, so
one click of an eye icon cost ~163 MB of disk traffic and a few minutes of
ordinary layer work cost gigabytes. Painting was never the problem — strokes
are `EditKind::Content` and already ride the 60 s timer.

Note this is a **second** defect found in the same trigger. The first was that
the write ran synchronously on the paint thread, froze the UI for ~1 s per
structural edit, and sat outside every `--frame-trace` phase timer; that was
fixed by moving the write to a background thread (`d59c308`). Moving it off the
main thread stopped the freeze but did not reduce a byte of the traffic, which
is the part this note is about.

**What has to happen before the default flips back.** Split the trigger rather
than the timer:

1. Edits that **create or destroy pixels or layers** — add, delete, duplicate,
   merge, paste, import, transform commit, filters, adjustments — keep writing
   at once. That is the loss this ADR exists to prevent, and the "Journal every
   edit" alternative below was rejected partly because a bounded trigger was
   supposed to be cheap.
2. Edits that only change **metadata** ride `kJournalIntervalSeconds` like any
   content edit. Losing a re-typed layer name to a crash is not the failure
   this ADR is about.
3. Optionally debounce (1) as well, so a burst of real edits collapses into one
   write instead of one per edit.

A delta or append-only model format would remove the whole problem, but that is
a format change and is explicitly *not* what this note is asking for.

**Until then, `--journal` is the switch, and recovery of EARLIER sessions still
works with the journal off** — discovery runs before `begin()` and is not gated
on it, so a session that crashed while journalling was on is still offered back.
