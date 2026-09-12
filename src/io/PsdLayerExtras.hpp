#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "io/PsdWrite.hpp"

// io/PsdLayerExtras -- the two things a PSD layer record carries that are not
// the layer itself: a raster **mask** (a 20-byte block in extra data plus a
// channel with id `-2`), and a **group** (a pair of synthetic records around
// the members, tagged `lsct`).
//
// PLAN.md phase 15, docs/psd-export.md's "Masks -- channel -2 and the 20-byte
// record" and "Groups (lsct) -- and the order that inverts".
//
// --- Why this is a module of free functions and not part of the record writer
//
// io/PsdLayerSection writes the layer records and their channel data and
// assumes neither masks nor groups exist. This module writes the pieces that
// change that, as standalone functions taking a `PsdWriter&`, and the two are
// wired together by hand at the call site. That split is deliberate: an
// earlier draft of docs/psd-export.md had this file editing that one's
// `writeLayerRecord()`, and this project has already lost a `break;`, a
// `|| tonal` and an `if` to a line-level union resolution of two branches
// that both edited one function. A file boundary is the one merge conflict a
// three-way merge cannot silently resolve wrongly.
//
// Nothing here reads or writes a file. Every function is pure over a
// `core::Layer` / `core::Document` plus a `PsdWriter`, which is what makes
// all of it reachable from `--selftest` with no GPU, no window and no disk.
//
// ==========================================================================
// (1) MASKS -- and the two ways a coverage default inverts
// ==========================================================================
//
// A `core::Layer`'s mask is a `MaskTileStore` of binary16 coverage. PSD's is
// an 8-bit channel with id `-2` over a rectangle declared in a 20-byte block
// in the record's extra data, plus **a default colour byte governing every
// texel outside that rectangle**.
//
// **The rect is the tight bounds of coverage that is NOT 1.0, and the default
// colour is 255.** Both halves of that sentence are the inverse of what a
// colour channel would do, and both come from the same property:
// core/Mask.hpp's `MaskTile` is the one tile type in this codebase whose
// default fill is not zero -- an absent mask tile means **reveal**, because
// 1 is the identity of the multiply a mask feeds. So:
//
//   * The empty-tile skip inverts. `cacbd12`'s RGB rule ("skip a tile whose
//     samples are all zero") becomes "skip a tile that is `isFullyRevealed()`"
//     -- exactly the rule io/NpaintFile already applies on the way out to a
//     `.npaint`, restated here for a different container.
//   * The default colour byte must be **255**. A 0 there means every texel
//     outside the rect is HIDDEN, which for a small hidden patch on a large
//     layer would erase the whole layer in Photoshop while our own reader,
//     which does not read masks yet, would notice nothing. docs/psd-import-gaps.md
//     section 1 measures ten real masks and all ten write 255; and
//     `naturalpaint-selection-semantics` is the last time this project shipped
//     a coverage default that was the inverse of a layer mask's.
//
// **The mask channel is sized by the MASK rect, not the layer rect.** The two
// are different on every one of the ten real masks docs/psd-import-gaps.md
// section 1 dumps (5000x2559 layers carrying 160x105, 567x919, 1114x1506
// masks). Using the layer rect writes the right number of *bytes* for the
// wrong number of *texels* and desynchronises nothing, because each channel
// declares its own length -- it simply produces a mask that is garbage, with
// no error anywhere.
//
// **Coordinates are absolute and the flags byte is zero.** The mask record's
// flags bit 0 means "rect is relative to the layer's own origin", and it is
// set on all ten real masks -- but every masked layer in those files sits at
// origin (0,0), where relative and absolute coincide, so nothing has ever
// confirmed a reader's or a writer's handling of that flag. Writing absolute
// is the form this project can actually stand behind.
//
// **A mask block larger than 20 bytes is never emitted.** Past 20, a second
// "real" (vector-derived) mask follows; io/PsdImport.cpp refuses that shape by
// name, and this writer must not produce a file its own reader refuses.
//
// What is deliberately NOT here: mask *density*, *feather*, and the "mask
// disabled" flag (bit 1). `core::Layer` has no field for any of the three --
// a mask here is one coverage store and nothing else (core/Mask.hpp: "a layer
// mask is per-texel opacity, and nothing else") -- so there is nothing to
// write, rather than a default being chosen for a feature that exists.
//
// ==========================================================================
// (2) GROUPS -- the record order that inverts, and it does so silently
// ==========================================================================
//
// PSD's flat record list runs **bottom-first**, and a group is a pair of
// synthetic empty records with the members between them:
//
//     divider  (lsct type 3)   <- FIRST; opens the group from below
//     member, member, ...      <- in Document::layers order
//     header   (lsct type 1/2) <- LAST; closes the group and NAMES it
//
// docs/psd-import-gaps.md section 3 verified that against two real Photoshop
// files (`Testforautoflats 2.psd` and `Peter_...fire.psd`, three groups each).
// Written the other way round, every group's membership inverts and **no
// reader anywhere reports an error** -- the file is structurally valid and
// semantically backwards. It is one of that document's three
// "opens-without-error-and-is-confidently-wrong" traps.
//
// This lines up with `Document::layers` without any reversal:
// `core::LayerSetOps`' `GroupLayers` splices the members in and then inserts
// the Group layer at `insertPos + members.size()`, i.e. **immediately above**
// its own members, and index 0 is the bottom of the stack. So a single
// bottom-to-top walk of `Document::layers` emits records in PSD's own order,
// which is what `planPsdRecords()` below does.
//
// The header's blend key is **`pass`**. `core/Composite.hpp` makes a group a
// pass-through fold rather than an isolated one, and all six groups in the two
// real files are `pass` too, so this is the truthful key rather than a default
// that happens to be common.
//
// **Nesting depth is 0 in every file this project has ever looked at.** The
// planner below implements arbitrary nesting -- the stack gives it for free --
// but `app/selftest/PsdLayerExtras.cpp` says plainly, in the assertion text
// itself, that its depth-2 case is a hand-built fixture and not evidence from
// a real file.
//
// ==========================================================================
// (3) THE SHEET COLOUR -- `lclr`, and the label this writer refuses to guess
// ==========================================================================
//
// Eight bytes: a `uint16` index, then six zero bytes. Index 0 is unlabelled
// and 1..7 are Photoshop's own menu order, which is exactly the order of
// `core/Layer.hpp`'s `kLayerColorLabelNames` -- so the name at array position
// *i* writes as index *i* + 1. Real bytes from `App Icon Template.psd`: red is
// `0001000000000000`, yellow `0003000000000000`, green `0004000000000000`.
//
// Two cases that are not the same, and conflating them is the whole reason
// this is a function rather than an index lookup at the call site:
//
//   * **An empty label writes NO BLOCK AT ALL.** That is the format's own
//     "no label", and it keeps the bytes of a label-free document exactly
//     what they were before this block existed.
//   * **A label outside the seven writes no block either, and warns naming
//     it.** `Layer::colorLabel` is deliberately an open set (core/Layer.hpp:
//     "not a closed set the format enforces"), so a future build's `"teal"`
//     is legal data arriving through a `.npaint`. Guessing an index for it
//     would put a colour on the layer that nobody chose. This is the same
//     rule `psdBlendKeyFor()` already follows for a blend mode PSD has no key
//     for.

namespace np {

// --- Masks ----------------------------------------------------------------

// The 20-byte mask block's rectangle, in **absolute document coordinates**,
// with `bottom`/`right` exclusive -- the same convention io/PsdImport.cpp
// reads back (`width = right - left`).
//
// A struct rather than four out-parameters because top/left/bottom/right is
// exactly the argument order a caller gets wrong once and never notices: two
// of the four transpositions still describe a non-inverted rectangle.
struct PsdMaskRect {
  int32_t top = 0;
  int32_t left = 0;
  int32_t bottom = 0;
  int32_t right = 0;

  int32_t width() const noexcept { return right - left; }
  int32_t height() const noexcept { return bottom - top; }
};

// The default colour byte this writer always emits: reveal everything outside
// the mask rect. See section 1 above for why the other value is not merely a
// different choice but a silent erasure.
inline constexpr uint8_t kPsdMaskDefaultReveal = 255;

// The only mask-block size this writer emits. `0` (no mask) is the other legal
// value it produces; anything else means a second vector-derived mask, which
// io/PsdImport.cpp refuses by name.
inline constexpr uint32_t kPsdMaskBlockSize = 20;

// The tight bounds of coverage that is **not** 1.0, in absolute document
// coordinates.
//
// Returns **false** -- meaning "write `u32 0` for the mask block and emit no
// channel `-2` at all" -- when the layer has no mask, when the mask store is
// empty, or when every allocated tile reveals everywhere. Those three are
// indistinguishable in the composite (core/Mask.hpp's "Absent, all-1.0 and
// all-0.0 are three different things" makes exactly this point about the first
// two), and a mask that reveals everything is a mask no other application
// needs to be told about.
//
// **Deliberately not clipped to the canvas.** The caller has the `Document`
// and this does not; clipping here would silently turn a hidden patch that
// hangs off the canvas edge into a revealed one, and while nothing composites
// an off-canvas texel today, an export that quietly changes coverage is the
// wrong default for a function whose whole job is to describe what is there.
// A caller that wants the clip can intersect the result itself.
//
// The per-tile fast path is `MaskTile::isFullyRevealed()`, a word comparison;
// the per-texel test is `readCoverage() != 1.0f`, a value comparison. The two
// agree in the direction that matters: all-`kRevealWord` implies all-1.0, so
// the fast skip can never drop a tile the slow scan would have kept.
bool psdMaskRect(const Layer& layer, PsdMaskRect& out);

// Writes the layer record's extra-data mask block: `u32 0` when `rect` is
// null, otherwise `u32 20` followed by exactly
//
//     i32 top, left, bottom, right   (absolute document coords)
//     u8  default colour             (255 -- kPsdMaskDefaultReveal)
//     u8  flags                      (0)
//     u16 padding                    (zero -- what makes the record 20 bytes)
//
// `rect` is a pointer rather than an `optional` so the call site reads as the
// two-state thing it is, and so that "no mask" is still a call: the block is
// mandatory in every layer record, and a caller that forgets it desynchronises
// every field after it.
//
// **The same `psdMaskRect()` answer must drive the channel table.** A record
// that writes this block but no channel `-2` (or the reverse) is malformed;
// call `psdMaskRect()` once and let its result decide both.
void writePsdMaskBlock(PsdWriter& w, const PsdMaskRect* rect);

// Channel `-2`'s complete payload: the `u16` compression word (`1`, RLE),
// then PackBits' row-count table for `rect.height()` rows, then the rows.
//
// The compression word is **included** on purpose. A layer record declares
// each channel's length as "the channel's own 2-byte compression word plus its
// data" (docs/psd-export.md's record table), so returning the whole block lets
// the caller write `w.raw(block)` and declare `block.size()` -- one number,
// from one place, instead of an addition the caller has to remember.
//
// Samples are `round(coverage * 255)` of `core::maskCoverage()` -- the same
// leaf core/Composite and the eyedropper both read a masked texel through, so
// an exported mask cannot disagree with the one on screen. Outside the
// allocated tiles that call answers 1.0, which encodes as 255 and matches the
// default colour byte, so the rect's own interior stays consistent with
// everything outside it.
std::vector<uint8_t> encodePsdMaskChannel(const Layer& layer, const PsdMaskRect& rect);

// --- Groups ---------------------------------------------------------------

// What one PSD layer record is, in a plan produced by `planPsdRecords()`.
enum class PsdRecordRole {
  // An ordinary record for `Document::layers[layerIndex]`.
  kLayer,
  // The synthetic **bounding section divider** that opens a group from below
  // (`lsct` type 3). `layerIndex` names the Group layer it belongs to, so a
  // caller can put that group's name in a warning; the record itself is
  // Photoshop's `</Layer group>` (see `kPsdGroupDividerName`), empty rect,
  // zero-length channels.
  kGroupDivider,
  // The synthetic record that **closes and names** a group (`lsct` type 1 or
  // 2). `layerIndex` is the Group layer, whose `name` this record carries.
  kGroupHeader,
};

struct PsdRecordPlan {
  PsdRecordRole role = PsdRecordRole::kLayer;
  // Always a valid index into `Document::layers`. For the two synthetic roles
  // it is the Group layer's own index -- not a member's.
  size_t layerIndex = 0;
};

// The name Photoshop gives a divider record. Our own importer currently shows
// it verbatim in the layer panel for an unimported group, which is how
// docs/psd-import-gaps.md section 3 knows the exact spelling.
inline constexpr const char* kPsdGroupDividerName = "</Layer group>";

// The flat PSD record order for `doc`, bottom-first, with every
// `LayerKind::Group` expanded into its divider (before its members) and its
// header (after them).
//
// A document with no groups plans to exactly one `kLayer` entry per layer, in
// `Document::layers` order, and the writer reverses nothing --
// io/PsdImport.hpp settled that direction against psd-tools' own compositor
// (`bottom = psd[0]`) and confirmed it on all three real files.
//
// **Returns false, with `error` naming the layer, for a document whose group
// structure has no PSD record order at all**, rather than emitting a plausible
// one. `io/Descriptor.hpp`'s rule -- a refusal is total. Three shapes:
//
//   * **members that are not contiguous with their group**, e.g. a top-level
//     layer sitting between two members. PSD encloses a group's members
//     between two records and has no way to say "these two but not the one in
//     between";
//   * **a group whose members sit above its own row**, which would leave a
//     divider that never closes;
//   * **a cycle** in the `parent`/`groupTag` chain. `core::groupAncestry()`
//     answers a cycle by hiding the layer; here there is nothing to hide, so
//     it is a refusal.
//
// A **dangling** `parent` -- a tag naming no live Group -- is not a refusal:
// the layer plans as top-level, which is exactly what `core::groupAncestry()`
// already does with one ("dangling tag: no more ancestors"). `Layer::parent`
// is carried verbatim through this codebase by anything that does not resolve
// it (PRD I10), so a document from a foreign build can legitimately hold one,
// and refusing to save it would be worse than exporting what the compositor
// itself shows.
bool planPsdRecords(const Document& doc, std::vector<PsdRecordPlan>& out, std::string& error);

// Writes a complete `8BIM`/`lsct` additional-layer-information block for one
// synthetic group record: the signature, the key, the backpatched `u32`
// length, and the payload.
//
//   `PsdRecordRole::kGroupDivider` -> length 4, `u32` type 3. No blend key,
//     which is what both real files write.
//   `PsdRecordRole::kGroupHeader`  -> length 16, `u32` type (`1` open /
//     `2` closed), `8BIM`, `pass`, `u32` sub-type 0. Byte for byte the
//     header block docs/psd-import-gaps.md section 3 dumped from a real file.
//
// A 12-byte header (type plus blend key, no sub-type) is equally legal by the
// published spec and our own reader accepts it -- `blockData.size() >= 12` is
// its test -- but 16 is the length Photoshop actually writes, and matching a
// verified byte sequence beats matching a permissive reader.
//
// `openFolder` is `true` for type 1 and `false` for type 2. **The document
// carries no such state**: whether a group is collapsed lives in the layers
// panel (`app/LayerPanel.hpp`'s `collapsedGroupTags`), not on `core::Layer`,
// so an export from a `Document` alone has nothing to consult and should pass
// `true`. The parameter exists so a caller that *does* have the UI's set can
// pass the truth rather than this module inventing one.
//
// `PsdRecordRole::kLayer` writes **nothing at all**, and that is a defined
// answer rather than an unchecked misuse: an ordinary layer record has no
// `lsct` block, so a caller walking a plan can call this once per entry
// unconditionally instead of branching. There is deliberately no error case
// here -- the writer's `ok()` is untouched on every role.
void writePsdLsctBlock(PsdWriter& w, PsdRecordRole role, bool openFolder);

// --- Additional Layer Information framing ---------------------------------

// One complete `8BIM` + four-character key + `u32` length + payload block,
// padded to an even total.
//
// `PsdLayerRecord::extraBlocks` is a plain byte buffer whose blocks
// io/PsdImport walks by their declared lengths with no even-rounding of its
// own, so **a block whose payload is odd must carry the pad byte INSIDE its
// declared length**, not after it: a stray byte the walk did not expect
// desynchronises it into reading a length out of the middle of the next
// block's key. That is one rule, stated once, rather than at each producer --
// `writePsdLsctBlock()` above predates it and frames its own two fixed-size
// (and therefore even) payloads inline.
void writePsdTaggedBlock(PsdWriter& w, const char* key, std::span<const uint8_t> payload);

// --- The sheet colour -----------------------------------------------------

// Photoshop's 1..7 index for `colorLabel`, per section 3 above.
//
// Returns **false, with `index` untouched, for any label that must not become
// a block**: the empty label and any name outside `kLayerColorLabelNames`.
// The caller tells the two apart by asking whether the label was empty --
// which it has in hand -- rather than by a second out-parameter, because the
// only difference between them is whether a warning is warranted.
bool psdLayerColorLabelIndex(const std::string& colorLabel, uint16_t& index);

// The `lclr` payload: the `u16` index then six zero bytes. Eight bytes, so it
// is even and needs no pad of its own.
std::vector<uint8_t> encodePsdLclrBlock(uint16_t index);

}  // namespace np
