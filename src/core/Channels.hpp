#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "core/SelectionMask.hpp"

// core/Channels (PLAN.md "Phase 7 -- Select and paste"; PRD E11, E12, E13).
// Named coverage channels stored in the document, the two conversions that
// join them to a selection, and quick mask.
//
// Three requirements land in one file because they are one idea seen three
// ways. E13 asks for alpha channels in the document; E11 asks that a selection
// can be saved into the document and restored, which *is* an alpha channel
// with a save and a load in front of it; E12 asks that a selection be editable
// as a paintable overlay, which is the same coverage store handed to a brush
// instead of to a deposit. Splitting them into three files would have produced
// three copies of the convention section below, which is the one part of this
// that is dangerous to get wrong.
//
// --- The convention: an absent tile is 0.0, the SELECTION default -----------
//
// core/SelectionMask.hpp spends forty lines on the fact that its defaults are
// the exact inverse of core/Mask.hpp's, and this file inherits the selection
// half of that argument **whole**, deliberately and without a hedge:
//
//   **A channel texel with no tile behind it is coverage 0.0** -- not
//   selected, not carrying alpha, black. The identical rule
//   `selectionTileCoverage()` states, for the identical reason.
//
// Three arguments, and the first is the one that decides it:
//
//  1. **An `AlphaChannel` holds a `SelectionTileStore`. It is not a similar
//     store, it is that store.** So `selectionTileCoverage()` -- the single
//     leaf core/SelectionMask.hpp insists every reader go through, precisely
//     so a deposit and a clear cannot disagree about a partial texel -- is
//     already a reader of channel tiles. Giving a channel the opposite default
//     would mean one function answering the same question two ways depending
//     on which owner the tile happened to be sitting in. That is the drift
//     core/SelectionMask.hpp names as a real risk in its own two-readers
//     section, and it would be *designed in* rather than stumbled into.
//
//  2. **It makes the round trip an identity instead of an inversion.** PRD E13
//     asks for both directions -- load a channel as a selection, save a
//     selection as a channel -- and asks that they round-trip exactly. Under a
//     shared default the pair is a store copy: exact by construction, with no
//     arithmetic to be wrong. Under the layer-mask default each direction
//     would have to complement every texel, and `255 - v` is exact but the
//     *sparsity* is not: an absent tile means 1.0 on that side, so complementing
//     a sparse channel would have to materialise a full tile of 255 everywhere
//     the document has no tile at all. A 4096x4096 channel would go from
//     however many tiles the user painted to 1024 of them, 16 MiB, on every
//     conversion.
//
//  3. **It is what a user of any other editor already believes.** Photoshop's
//     alpha channels are black-is-excluded, white-is-selected, and "load
//     channel as selection" selects the white. An absent tile is black.
//
// The cost of the choice, stated rather than left to be found: a channel and a
// *layer mask* now disagree about their absent tile, and the two are drawn
// identically as greyscale coverage. `channelFromLayerMask()` does not exist in
// this file for that reason -- when it is written it must complement, and it
// must say so in its name or its first line, because the conversion that looks
// like a copy is the one that is not.
//
// --- E11: a SAVED selection is document data; the ACTIVE one is not ---------
//
// These two are one word apart and conflating them makes every marquee
// undoable, so the distinction is stated here as well as at
// `app::OpenDocument::selection`, which is the other end of it.
//
//   **The active selection** -- `app::OpenDocument::selection` -- is SESSION
//     state. It is not in `Document`, it is not in a `core::History` snapshot,
//     and io/NpaintFile does not write it. That is not an omission: a
//     `HistoryEntry` holds a whole `Document` by value, so an active selection
//     inside `Document` would make Cmd+Z restore a marquee along with the
//     pixels and would make *drawing* a marquee an undoable act. No editor
//     behaves that way, and app/DocumentLifecycle.hpp argues it at length.
//
//   **A saved selection** -- an `AlphaChannel` in `Document::channels` -- is
//     DOCUMENT data. It has a name, it is written to the file as its own EXR
//     part, and it survives being closed and reopened. It is in history
//     snapshots, which is correct and is the *point*: "Save Selection" is a
//     deliberate command that changes the document, so undoing it must remove
//     the channel. The marquee is a gesture; the channel is a thing the user
//     made.
//
// So E11 is not in tension with `OpenDocument::selection`'s comment -- it is
// satisfied by `saveSelectionAsChannel()` and `loadChannelAsSelection()` below,
// which are the two explicit commands that cross between session and document.
// Nothing else may cross. A helper that quietly copied the active selection
// into `Document` on save would satisfy PRD E11's sentence and break the
// history rule at the same time, and would look like a convenience while doing
// it.
//
// --- E12: quick mask --------------------------------------------------------
//
// See `QuickMask` below. The short version: it is the same coverage store,
// paintable, and the only genuine decision in it is what happens at the
// boundary between "engaged and empty" and "no selection at all" -- which is
// the third of core/SelectionMask.hpp's three absences, and the one a UI has to
// be able to explain.
//
// --- Where the file format part lives ---------------------------------------
//
// io/NpaintFile owns the on-disk representation: one `S####` part per channel,
// `np:kind = "selection"`, a single `coverage` channel in HALF, exactly as
// docs/document-format.md sketched it before anything wrote one. That header
// carries the measurement showing HALF is a lossless carrier for this store's
// uint8 grid, and the backward-compatibility argument. Nothing about EXR
// appears in this file -- `core/` is the domain model, and the same rule that
// keeps `NpaintCarry` off `Document` keeps parts and attributes out of here.
namespace np {

struct Document;

// --- The channel -------------------------------------------------------------

// One named coverage channel stored in the document (PRD E13).
//
// A plain aggregate, matching `core::Selection` and `core::LayerComp`: the
// invariant that matters -- that no stored tile is entirely zero -- belongs to
// the functions that build one, not to every write, because a paint stroke
// legitimately passes through states where it is false. `compactChannel()`
// below is how it is restored, and it is called at the two places where the
// state stops being transient: a conversion out, and a save.
struct AlphaChannel {
  // User-facing, and **unique within a document**, unlike a layer name.
  //
  // Layers deliberately allow duplicates and get a synthetic `L####` part name
  // to be identified by (docs/document-format.md); channels do not, because the
  // only way anything refers to a channel is `loadChannelAsSelection(doc,
  // name)`. A duplicate name would make that call's answer depend on vector
  // order, which is not something a user can see. `uniqueChannelName()` is how
  // a caller gets a name that keeps the property, and `saveNpaint()` refuses a
  // document that has lost it rather than writing a file whose channels cannot
  // all be named.
  std::string name;

  // The coverage itself. **The same store type a `Selection` holds**, which is
  // what makes the two conversions below store copies rather than arithmetic --
  // see this header's convention section, which is the whole argument.
  SelectionTileStore tiles;
};

// Coverage at a document-space texel. `0.0` where the channel has no tile --
// this file's convention, and `selectionTileCoverage()`'s.
//
// Note there is no null-channel overload, and that asymmetry against
// `selectionCoverageAt()` is deliberate. A null *Selection* means "no
// restriction" and has to mean 1.0; a null channel is not a state anything can
// be in -- a channel either exists in `Document::channels` or does not exist at
// all -- so an overload would only invite the 1.0 default in through a door
// that should not be there.
float channelCoverageAt(const AlphaChannel& channel, PixelCoord docTexel) noexcept;

// True when the channel covers nothing anywhere: no tiles, or only tiles that
// are entirely zero. The channel analogue of `selectionSelectsNothing()`.
bool channelIsEmpty(const AlphaChannel& channel) noexcept;

// Drops every tile that is entirely zero, restoring core/SelectionMask's
// constructor invariant, and returns how many were dropped.
//
// A tile of all zeros is *indistinguishable* from an absent one under this
// file's convention, so keeping it costs 16 KiB to say nothing. That is not a
// micro-optimisation: painting a channel and then erasing it again would
// otherwise leave the erased tiles resident for the rest of the session, and
// every save-and-reopen would grow the resident set of a document nobody
// changed. The file reader drops them too, for the same reason and by the same
// rule the RGB unpacker uses for all-zero pixel tiles.
size_t compactChannel(AlphaChannel& channel);

// --- The two conversions (PRD E13) ------------------------------------------
//
// **Both are exact, and they are exact by construction rather than by care.**
// A `Selection` and an `AlphaChannel` hold the same `SelectionTileStore`, so
// each direction is a store copy -- copy-on-write, so it costs refcounts and
// not 16 KiB per tile -- and there is no quantisation stage in either one for a
// rounding to hide in. `--selftest` asserts the round trip texel for texel
// *and* tile for tile anyway, because "exact by construction" is a claim about
// the code as written and the assertion is what keeps it true.
//
// **Neither direction compacts**, which is what makes the round trip exact on
// the tile set and not merely on the coverage. A caller that wants the
// invariant restored calls `compactChannel()` and means it.
//
// The rejected alternative was a distinct `AlphaTile` type with its own texel
// layout. It buys nothing -- the quantity is the same 8-bit coverage, and
// Photoshop stores selections and alpha channels at the same depth for the same
// reason -- and it would have put a conversion, and therefore a potential
// rounding, on a path whose entire requirement is that it not round.
AlphaChannel channelFromSelection(const Selection& selection, std::string name);
Selection selectionFromChannel(const AlphaChannel& channel);

// --- Document-level: save and restore a selection (PRD E11) -----------------

// The channel with this name, or nullptr. Names are unique, so "the" is
// well-defined; if a document ever holds two the first wins and `saveNpaint()`
// refuses the file.
const AlphaChannel* findChannel(const Document& doc, std::string_view name);
AlphaChannel* findChannelForWrite(Document& doc, std::string_view name);

// The stem `uniqueChannelName()` uses when asked for a name with nothing in
// it. Photoshop's, because a user arriving from it should recognise what the
// command produced.
inline constexpr const char* kDefaultChannelNameStem = "Alpha";

// `desired` if the document has no channel by that name; otherwise `desired`
// with the smallest free " N" suffix from 2 up. An empty `desired` starts from
// `kDefaultChannelNameStem` and always takes a number, so the first one is
// "Alpha 1" rather than a bare "Alpha".
std::string uniqueChannelName(const Document& doc, std::string_view desired);

// PRD E11's save half: the active selection becomes a named channel in the
// document. Returns the index of the new channel in `doc.channels`.
//
// **Appends under a uniquified name; never replaces.** Save Selection twice
// with the same name typed and you get two channels, not one channel silently
// overwritten -- destroying a saved selection has to be a delete, which is a
// different command with a different confirmation. A caller that genuinely
// means "overwrite" has `findChannelForWrite()` and one assignment, which is
// short enough not to need wrapping and explicit enough to read at the call
// site.
//
// The selection is **compacted on the way in**: an all-zero tile the caller's
// booleans happened to leave behind is not worth persisting, and it is exactly
// what the file reader would drop on the next load anyway, so dropping it here
// makes save-then-reload an identity instead of nearly one.
size_t saveSelectionAsChannel(Document& doc, const Selection& selection, std::string name);

// PRD E11's restore half, and PRD E13's load direction: the named channel
// becomes a selection the caller can make active.
//
// `std::nullopt` when there is no such channel -- **not an empty `Selection`**.
// The difference is the whole of core/SelectionMask.hpp's three-absences
// section: an engaged-but-empty selection refuses every edit everywhere, so
// returning one for "you asked for a channel that is not there" would answer a
// lookup failure by disabling the editor. The caller sees the nullopt and
// leaves the active selection alone.
//
// A channel that exists but is entirely empty *does* come back as an engaged,
// empty `Selection`, and that is not the same case: the channel is real, the
// user saved it, and it selects nothing. That is a state the UI can name.
std::optional<Selection> loadChannelAsSelection(const Document& doc, std::string_view name);

// --- Quick mask (PRD E12) ---------------------------------------------------

// The active selection, opened up as a paintable overlay.
//
// A distinct type wrapping a `Selection` rather than a bare `Selection`,
// and the one line of value it adds is that it **cannot be assigned to
// `app::OpenDocument::selection` by accident**. Entering quick mask, painting,
// and then dropping the painted coverage straight into the active selection
// without going through `selectionFromQuickMask()` would skip the two decisions
// that function exists to make (compaction, and the empty case below) and would
// do it silently. A one-member struct is a smell only when the member is the
// whole meaning; here the *name* is.
//
// Nothing else is needed on it. The overlay's colour and opacity are
// presentation and belong to the panel this track is not building; the paint
// arithmetic is `combineSelections()` (core/SelectionOps), which already
// operates on exactly this store with exactly these semantics -- a quick-mask
// brush stroke is an Add and a quick-mask eraser is a Subtract, and inventing a
// second brush-into-coverage path here would have been a second set of edge
// rules to keep in step with the first.
struct QuickMask {
  Selection coverage;
};

// Enter quick mask from the active selection, which may be absent.
//
// **A null active selection gives an EMPTY mask**, not a full one, and this is
// the first of the two boundary decisions. Entering quick mask with nothing
// selected must present a blank overlay the user paints *into* -- that is what
// the command is for, and it is what every editor does. Reading the null as
// "coverage 1.0 everywhere" and materialising a full mask would be defensible
// from `selectionCoverageAt()`'s contract alone and is wrong here: it would
// hand the user a fully-painted overlay to erase, and it would allocate a tile
// per document tile to do it.
//
// Note the consequence, which is correct rather than a wart: entering from
// `selectAll()` and entering from `std::nullopt` produce *different* masks
// (full and empty) even though the two selections restrict nothing in exactly
// the same way. core/SelectionMask.hpp already decided that Select All and
// Deselect are deliberately distinguishable states, and quick mask is one of
// the places that distinction becomes visible.
QuickMask quickMaskFromSelection(const Selection* active);

// Leave quick mask: the painted overlay becomes the active selection again.
//
// **An entirely empty mask converts back to `std::nullopt` -- no selection --
// and not to an engaged, empty one.** The second boundary decision, and the
// one worth the paragraph:
//
//  * Read as "the user entered quick mask, changed their mind, and left
//    without painting", nullopt is the only answer that makes the round trip a
//    no-op. The alternative hands back a selection that refuses every edit
//    everywhere, with no marquee anywhere on screen to explain why -- which is
//    core/SelectionMask.hpp's own "why is nothing happening when I paint",
//    arrived at by pressing a key twice.
//  * Read as "the user deliberately erased the whole mask", nullopt is *also*
//    right: erasing a selection to nothing is Deselect, and Deselect gives no
//    selection.
//
// Both readings converge, which is what makes this a decision rather than a
// coin toss. The caller is expected to assign the result -- nullopt included --
// straight onto `OpenDocument::selection`.
//
// The mask is compacted on the way out, so a tile painted and then erased
// again does not come back as a selection tile full of zeros.
std::optional<Selection> selectionFromQuickMask(const QuickMask& mask);

// Write one texel of the overlay, allocating the tile if the write needs one.
//
// The minimal thing that makes the overlay *paintable* rather than merely
// convertible: a brush that lays down partial coverage, and an eraser that
// takes it away, both reduce to this. Values outside [0,1] clamp --
// `SelectionTile::writeCoverage()`'s rule, and its reasoning (an unsigned wrap
// would turn "just over fully selected" into "unselected").
//
// **Writing 0.0 where there is no tile allocates nothing.** Zero is what an
// absent tile already reads as, so the allocation would buy 16 KiB of agreeing
// with the default -- and an eraser dragged across empty space is exactly the
// gesture that would otherwise allocate the whole canvas.
void paintQuickMask(QuickMask& mask, PixelCoord docTexel, float coverage);

// Coverage at a document texel, 0.0 where nothing has been painted. Reads the
// overlay through the same leaf every other reader of this store uses.
float quickMaskCoverageAt(const QuickMask& mask, PixelCoord docTexel) noexcept;

}  // namespace np
