#pragma once

#include <string>
#include <string_view>

#include "core/StrokesContent.hpp"

// io/StrokesSerial -- the `np:dabs` carrier for a `LayerKind::Strokes`
// layer's content: every dab record, and the id allocator beside them.
//
// **This is the deferral io/NpaintFile.hpp reserved by name.** That header's
// list said: "A `strokes` part and its `np:dabs` blob. `LayerKind::Strokes`
// exists as an enum value and core/Layer.hpp calls it an 'inert placeholder';
// there is no Dab type, no dab list and no stroke record anywhere in `core/`.
// brush/StrokePath emits dabs into the *solver*, not into a document.
// Unblocked by a Strokes layer that actually holds dabs." core/StrokesContent
// is that layer's content, so the deferral is paid off here.
//
// io/FlatsSerial's and io/TextSerial's sibling for a `StrokesContent`, and
// deliberately the same carrier as both: a hex `string`, not the `<blob>`
// docs/document-format.md's table names.
// **That is not a preference, it is the measured constraint the other two
// carriers record**: this OpenImageIO drops array-typed EXR header attributes
// silently on write, so a dab list written as a blob would vanish on every
// save with nothing to notice it. Same bit-pattern floats, and the same
// version-in-the-prefix rule, so a payload from a build newer than this one
// is refused by name and carried verbatim rather than half-decoded (PRD I10).
//
// ==========================================================================
// The wire, and the one version bump it has had
// ==========================================================================
//
//     "npdabs1:" | "npdabs2:"  <hex>
//     u64 nextDabId, u32 count, then `count` records of
//       u64 id, u64 strokeId,
//       f32 x, y, radius, hardness, roundness, angle,
//       f32 edgePx                      <- npdabs2 only
//       f32 flow, f32 rgba[4], u8 source, f32 sourceDx, sourceDy
//
// little-endian, floats as IEEE-754 binary32 bit patterns: 69 bytes a record
// in `npdabs1`, 73 in `npdabs2`. The payload is exact-length (trailing bytes
// are refused), so a new field cannot simply be appended -- it is a new
// version, which is what the prefix exists for.
//
// **Why `npdabs2` was necessary.** `BrushTip::edgePx` (brush/Deposit.hpp §2)
// arrived after this carrier and floors a tip's antialiased rim at one pixel
// by default. `npdabs1` has no field for it, so `strokesRasterize()` replayed
// every record at the RUNNING build's default: a hardness-1 r=6 record saved
// by a build that drew it as 112 texels at exactly 1.0 re-rendered as 80 at
// 1.0 plus 32 fractional after an update, with nothing in the file changed
// (review finding 6). A stored dab whose footprint depends on the build that
// opens it is not a stored dab. `npdabs2` carries `DabRecord::edgePx`; an
// `npdabs1` payload reads every record back as `edgePx == 0` -- the exact
// hard rim those documents were painted with -- which restores their pixels
// to the texel. (It would also have stopped a per-brush `edgePx` from ever
// surviving into a record.)
//
// **Which version is written is decided by the CONTENT, not by the build**,
// io/TextSerial.hpp's `nptext2` rule for its reason: `npdabs1` whenever it is
// lossless -- every record's `edgePx` is +0.0f, bit for bit, which is what a
// v1 reader produces -- and `npdabs2` otherwise. So a document opened from an
// `npdabs1` file and saved again without recording anything new stays
// readable by the builds that wrote it; only a record that actually carries a
// rim (every dab recorded live since `edgePx`, at its default 1.0) pays the
// compatibility cost below. An empty dab list writes `npdabs1`.
//
// **What an OLDER build does with an `npdabs2` payload** -- one that reads
// `npdabs1` only, which is every build before this carrier's bump. It refuses
// the payload BY NAME, before decoding a byte (its reader checks the prefix
// first), and opens the Strokes layer with no dab records and a load warning
// saying the attribute is carried -- so the layer does not RENDER there.
// **But it does not keep that promise on save, and this is measured, not
// read:** those builds' io/NpaintFile writes a Strokes part's own content
// unconditionally and its carry replay drops a carried `np:dabs` whenever it
// has written its own, so saving from such a build replaces the `npdabs2`
// records with an EMPTY `npdabs1` list. (`--selftest`'s strokes layer
// section F2 carries a future-tagged payload through two saves; with
// io/NpaintFile.cpp reverted to the pre-bump writer that assertion is red.)
// This build fixes the writer for the NEXT bump -- an `npdabs3:` payload it
// cannot read is written back verbatim while the layer stays as it opened
// (`writesOwnStrokes` in io/NpaintFile.cpp) -- but it cannot fix a build that
// already shipped. The content-decided version rule above is what keeps the
// exposure to documents that actually carry a rim: an `npdabs1` document
// round-tripped through this build is still `npdabs1`.
//
// **What is deliberately NOT carried: the rasterised pixels.** They are
// derived -- from the records AND from the composite beneath the layer, which
// is not in this file and must not be (core/StrokesContent §2) -- so writing
// them would bake a copy of the layers underneath into the layer above them
// and defeat PRD D6 on the next reopen. This is the same argument
// io/FlatsSerial makes about the label field and io/TextSerial about glyph
// outlines, with one extra term.

namespace np {

// A hex string, `npdabs1:` or `npdabs2:` prefixed -- the header's
// content-decided version rule. Never fails.
std::string serializeStrokesContent(const StrokesContent& content);

// The inverse. On failure returns false, leaves `*contentOut` untouched, and
// -- when `errorOut` is non-null -- writes one sentence saying why: an
// unrecognised version tag (the sentence names both versions this build
// reads), a non-hex character, a truncated payload, or an implausible dab
// count (the allocation-bomb case). Accepts `npdabs1:` (every record's
// `edgePx` reads as 0) and `npdabs2:` (the stored value).
bool deserializeStrokesContent(std::string_view value, StrokesContent* contentOut,
                               std::string* errorOut);

}  // namespace np
