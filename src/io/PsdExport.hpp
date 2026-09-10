#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "io/ImageDecode.hpp"
#include "io/PsdWrite.hpp"

// io/PsdExport -- the PSD container and its flattened composite.
//
// PLAN.md phase 15 asks for "flattened PSD first (small), then simply-layered".
// This module is the first half: a complete, legal, flat `.psd` -- file header,
// colour mode data, image resources, an empty layer section, and the Image Data
// Section carrying the whole document composited down. docs/psd-export.md calls
// it tier 1.
//
// ==========================================================================
// Why a writer can be confident about a format nobody here has a spec for
// ==========================================================================
//
// Every field below is written to be read back by `io/PsdImport.cpp`, and that
// parse was checked layer-for-layer against three real Photoshop files with
// psd-tools 1.18.0 as an oracle (io/PsdImport.hpp's header records the
// comparison). So this is not a writer guessing at a format from prose: it is
// the inverse of a function whose forward direction has been measured against
// Photoshop's own output. Where this header says "verified", it means that
// chain -- and where it says "checked here", it means this module's own
// selftest went and looked, which is a weaker claim and is marked as one.
//
// The byte primitives are `io/PsdWrite`'s and the RLE is `io/PackBits`'s. This
// module writes no big-endian arithmetic, no backpatch arithmetic and no second
// PackBits encoder; docs/psd-export.md and io/PackBits.hpp both argue that a
// second copy of either is a second place for an off-by-one to live.
//
// ==========================================================================
// What is written, exactly
// ==========================================================================
//
// Five sections, in this order, every field big-endian:
//
//   File Header            26 bytes, no length prefix
//     "8BPS" | u16 1 | 6 zero bytes | u16 4 channels | u32 height |
//     u32 width | u16 8 depth | u16 3 colour mode (RGB)
//   Colour Mode Data       u32 0     -- only Indexed and Duotone carry any
//   Image Resources        u32 0     -- see "What this landing does not do"
//   Layer and Mask Info    u32 0     -- tier 1 has no layers
//   Image Data             u16 1 (RLE), then the merged composite
//
// **Height precedes width in the file header.** That is Adobe's order and it
// is the opposite of every other place in this codebase; `PsdImport.cpp`'s
// `c.u32(height) && c.u32(width)` is the line this inverts.
//
// --- The Image Data Section's framing, which is not the layer framing ------
//
// This is the one piece docs/psd-export.md states loosely enough to get wrong,
// and getting it wrong opens the file as noise rather than as an error.
//
// For the MERGED composite the RLE row-count table covers **every row of every
// channel, consecutively** -- `height * channelCount` big-endian u16 entries in
// one run -- and only then come all the compressed rows. A per-layer channel is
// framed differently: each channel there carries its own u16 compression word
// and its own table. Tier 1 writes only the merged form; a tier 2 layer section
// must not copy this shape into a channel block.
//
// `decodePackBits()` reads exactly this framing from the other side (an
// `n`-entry table then one continuous compressed stream), which is why the
// selftest can assert the whole section with one call and no hand-written
// expectation: it hands the decoder `height * 4` as the table length and
// `width * height * 4` as the expected byte count, and gets the four planes
// back end to end.
//
// Channels are **planar**: all of R, then all of G, then all of B, then all of
// A. Not interleaved, and not per-row interleaved either.
//
// **A compressed row may be LARGER than the raw row** (io/PackBits.hpp's
// documented `n + ceil(n/128)` worst case). That is expected. This writer never
// falls back to raw for a single row, because PSD's compression word is per
// section, not per row -- a raw row inside an RLE section is unreadable.
//
// ==========================================================================
// Colour, and alpha, and which of them was checked rather than assumed
// ==========================================================================
//
// RGB samples are `color::srgbEncode()` of the linear working value, quantised
// to 8 bits by round-half-away-from-zero (`floor(v * 255 + 0.5)`) -- the same
// rounding `io/Export.cpp`'s `quantize()` uses, so a PSD and a PNG written from
// one document carry identical bytes. srgbEncode is the exact inverse of the
// `srgbDecode()` `io/PsdImport.cpp` applies on the way in, so the pair is a
// round trip rather than two independent readings of the sRGB curve.
//
// **Alpha is opacity, not light: it is never gamma-encoded**, only scaled and
// quantised. Same rule io/ImageDecode.hpp and core/Probe.hpp already hold.
//
// **Alpha is straight (unassociated).** This was CHECKED, not assumed:
// `flattenDocumentToLinear()` (io/Export.cpp:150) composites in premultiplied
// space and then calls `unpremultiply()` over every texel before returning, so
// what it hands back is ALREADY straight -- `DecodedImage`'s stated contract.
// This module therefore un-premultiplies nothing; a second division here would
// straighten an already-straight image and wash out every translucent pixel.
// The `a <= 0 -> {0,0,0,0}` guard lives in core/Premultiply.hpp, upstream of
// here, which is where the divide-by-zero is already handled. io/Export.cpp:55
// (`formatWantsAssociatedAlpha`) names EXR as the one format that wants the
// premultiplied form; PSD is not it, and this module reuses that convention
// rather than inventing a second one.
//
// --- Clipping is a warning with a number in it ----------------------------
//
// Linear working values can legitimately exceed 1.0 -- color/Space.hpp's
// transfer functions deliberately do not clamp, because "whether to clamp is a
// display/export policy decision". Eight bits has no representation above full
// scale, so highlights clip. That is a real loss of data and PRD I11 says a
// save that loses data names exactly what it lost, so the result carries a
// warning naming **how many samples clipped and the largest linear value that
// did** -- the precedent io/ExportAs.hpp sets ("which highlight value an
// integer depth will clip"). Negative linear values clip at 0 and are counted
// and named separately, because they mean something different (an out-of-gamut
// or over-sharpened result, not a highlight).
//
// Alpha is clamped to [0,1] without a warning. An alpha outside that range is
// not a colour value that a wider file could have carried; nothing in this
// build can produce one, and if a `.npaint` ever did, "your opacity was 1.3"
// is not a sentence about lost highlights.
//
// ==========================================================================
// What is refused, totally, and before a byte is emitted
// ==========================================================================
//
// "A refusal is total" is this directory's standing discipline (io/Descriptor.hpp,
// io/NpaintFile). A refused export produces `ok == false`, a specific `error`,
// and **an empty `bytes`** -- never a partially-valid file, and never a file
// with a plausible header over truncated pixels.
//
// Three refusals, each by name:
//
//  1. **A canvas wider or taller than 30,000.** PSD's own ceiling, and the
//     reason PSB exists. `PsdImport.cpp:1049` enforces it on read and refuses
//     PSB (version 2) by name; a writer that emitted a 40,000px PSD would be
//     writing a file this project could not open. Truncating instead would be
//     worse: the user would get a file, and it would be the wrong picture.
//
//  2. **A zero-or-negative-size document.** `PsdImport.cpp:1046` refuses one on
//     read; `flattenDocumentToLinear()` returns an invalid image for one.
//
//  3. **A `PsdWriter` that ends `!ok()`.** That flag goes false on a
//     programming error -- a mis-sized fourcc, a bad backpatch marker, a
//     section past 4 GiB that cannot state its own length in a u32. Any of
//     them means the bytes in hand are structurally invalid, so they are
//     discarded rather than written.
//
// The size checks run **before** the flatten, deliberately and not incidentally:
// compositing a 30,001-square document would ask for 14 GiB of float before
// anything got round to refusing it. `psdContainerRefusal()` below is that
// check, exposed so a caller (an export dialog, a capability gate) can ask
// "would this be refused" without building the pixels.
//
// ==========================================================================
// What this landing deliberately does not do
// ==========================================================================
//
// * **No 16-bit.** `depth` is 8 and there is no path to anything else, on
//   purpose. docs/psd-export.md's central finding is that Photoshop puts a
//   16-bit document's layers in an `Lr16` additional-layer-info block, and
//   `grep -n Lr16 src/io/PsdImport.cpp` finds no case for it -- so a 16-bit
//   layered PSD reads back through our own importer as flat. Writing 16-bit
//   samples into an ordinary section produces a file Photoshop calls corrupt.
//   There is deliberately no `65535` constant anywhere in this module: a later
//   16-bit landing should have to add one consciously, next to the `Lr16`
//   writer it also has to add. (Photoshop's 16-bit range is 0..32768 in any
//   case, not 65535 -- another reason not to leave a half-right constant here.)
//
// * **No layer section.** `writeFlattenedPsd()` writes a zero-length Layer and
//   Mask Information section. That is a legal flat PSD, and it is exactly the
//   `noLayerData` case `app/OpenAnyFile.cpp` already routes to the flattened
//   decode path -- so **tier 1's own output does not round-trip through
//   `importPsd()`; it round-trips through the OpenImageIO fallback.** A test
//   that calls `importPsd()` on this output and gets `noLayerData` has not
//   found a bug, it has confirmed the design. Tier 2 (io/PsdLayerSection) fills
//   this section in and keeps the Image Data Section exactly as written here --
//   which is not optional there either, because a PSD with no merged composite
//   opens blank in every application that does not parse layers, and that is
//   the whole reason Photoshop's "Maximize Compatibility" prompt exists.
//
// * **No image resources.** Legal as zero. Two are worth a follow-up and are
//   not here: `1005` ResolutionInfo (without it Photoshop shows 72 dpi) and
//   `1039` an ICC profile. Neither changes a pixel; both change what another
//   application believes about the pixels.
//
// * **No RGB-mode alternatives.** Greyscale, CMYK and Lab documents do not
//   exist in this build; colour mode 3 is written unconditionally rather than
//   switched on something that has one value.

namespace np {

// The result of an export attempt. Same shape and same wording discipline as
// io/Export.hpp's `ExportResult` and io/NpaintFile's `NpaintSaveResult`,
// deliberately rather than a third vocabulary for the same idea.
struct PsdExportResult {
  bool ok = false;
  // Non-empty exactly when !ok.
  std::string error;
  // Non-fatal. Names what the file could not carry, with numbers (PRD I11):
  // the blend modes `flattenDocumentToLinear()` approximated as `over`, passed
  // through verbatim from its own `warningsOut`, plus this module's own 8-bit
  // clipping report.
  std::vector<std::string> warnings;
  // Valid only when `ok`. Empty when not -- see the refusal section above.
  std::vector<uint8_t> bytes;
};

// Every reason `writeFlattenedPsd()` would refuse `doc`, asked without
// compositing anything. Returns the empty string when it would not refuse;
// otherwise the exact message it would fail with.
//
// This is not a second implementation of those checks -- `writeFlattenedPsd()`
// calls this one, the same arrangement io/Export.hpp's `exportRefusalReason()`
// has with `encodeLinearImage()`, and for the same reason: a preview of a
// refusal that can disagree with the refusal is worse than no preview.
//
// Note it cannot see the third refusal (a `PsdWriter` ending `!ok()`), which is
// a property of the bytes and not of the document. Nothing a caller can do
// about that one in advance, which is why it is not modelled here.
std::string psdContainerRefusal(const Document& doc);

// The 26-byte File Header. `channelCount` is 4 for the RGB+alpha composite this
// module writes; it is a parameter because a tier 2 writer building the same
// header for a document with spot channels would otherwise have a second copy
// of the field order -- and the field order (height before width) is the half
// of it that is easy to get backwards.
//
// Writes unconditionally. The caller has already refused an out-of-range size;
// this is a byte-layout function, not a policy one.
void writePsdFileHeader(PsdWriter& w, int32_t width, int32_t height, uint16_t channelCount);

// How much a document lost on its way to 8 bits. Counted over RGB samples only
// (see the alpha note above), across the whole canvas.
struct PsdClipReport {
  // Samples whose sRGB-encoded value exceeded 1.0 and were written as 255.
  uint64_t clippedHigh = 0;
  // The largest LINEAR value among them -- reported linear rather than encoded
  // because that is the number a user recognises from the eyedropper and from
  // io/ExportAs.hpp's own highlight wording.
  float largestClipped = 0.0f;
  // Samples whose value was below 0.0 (or NaN) and were written as 0.
  uint64_t clippedLow = 0;
  // The most negative linear value among them.
  float mostNegativeClipped = 0.0f;

  bool anything() const { return clippedHigh > 0 || clippedLow > 0; }
};

// The Image Data Section: the u16 compression word, the `height * 4` row-count
// table, then every compressed row. `flat` must be a straight-alpha,
// linear-light RGBA image the size of the canvas -- i.e. exactly what
// `flattenDocumentToLinear()` returns.
//
// Returns false, having written nothing useful, when `flat` is not valid() or
// when a compressed row would not fit the u16 the row-count table has for it.
// The second cannot happen for any canvas PSD itself permits (30,000 raw bytes
// grow to at most 30,235, well inside 65,535) and is checked anyway, because
// the alternative to checking is a silently truncated length field.
//
// Exposed for tier 2, which writes this identical section after its layer
// section from the same `flattenDocumentToLinear()` call.
bool writeMergedImageData(PsdWriter& w, const DecodedImage& flat, PsdClipReport& clipOut);

// Flatten `doc` and write a complete flat 8-bit RGB `.psd`.
//
// The whole file is built in memory and handed back; nothing here touches the
// filesystem, the same arrangement io/Export.cpp has, and for the same reason:
// a refused request can never leave a half-written file behind.
PsdExportResult writeFlattenedPsd(const Document& doc);

}  // namespace np
