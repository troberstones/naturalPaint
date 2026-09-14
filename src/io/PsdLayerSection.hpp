#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/Blend.hpp"
#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "io/PsdWrite.hpp"

// io/PsdLayerSection -- PSD's Layer and Mask Information section, written.
// One PSD layer record per naturalPaint `Layer`, plus that record's channel
// image data. Tier 2 of docs/psd-export.md.
//
// --- What this inverts, and why that is not the same as guessing ----------
//
// Every field below is a field `io/PsdImport.cpp`'s `readLayerRecord()`
// already parses, and that parse was checked layer-for-layer against three
// real Photoshop files with psd-tools 1.18.0 as an oracle (io/PsdImport.hpp
// records the comparison). So this module is not implementing a format from
// a specification -- it is inverting a function whose forward direction has
// been measured against Photoshop's own output. Where a decision could not
// be settled that way it says so by name below, rather than reading as
// though it had been.
//
// It writes bytes through `PsdWriter` (io/PsdWrite.hpp) and compresses rows
// through `encodePackBits()` (io/PackBits.hpp). It contains **no** big-endian
// arithmetic, no backpatching, no Pascal-string padding and no UTF-16
// conversion of its own, for the reason io/PsdWrite.hpp's own header argues:
// a second copy of any of those is a second place for an off-by-one to live.
//
// --- The three facts that are inverted from the obvious reading -----------
//
//  1. **Stacking order: no reversal, anywhere.** `Document::layers` index 0
//     is the bottom of the stack, and the FIRST layer record in a PSD is
//     also the bottom (io/PsdImport.hpp settles that against psd-tools'
//     compositor and it was confirmed on all three real files, `Background`
//     at index 0). This writer therefore appends records in plain
//     `Document::layers` order. A reversal is the natural first guess in
//     both directions and is wrong in both.
//
//  2. **Flags bit 1 SET means HIDDEN.** Adobe's own table calls the bit
//     "visible", which a literal reading takes as "set means visible".
//     psd-tools computes `visible = not bool(flags & 2)` -- with the comment
//     "# why 'not'?" at the exact line -- and io/PsdImport.cpp:496 follows
//     that inversion (`kFlagHidden = 0x02`), confirmed on real files by
//     hidden-layer counts of exactly 1 and 12 where the wrong way round
//     would have said 86 and 41. **A wrong-way writer produces a file that
//     round-trips through our own reader perfectly and shows every layer
//     backwards in Photoshop**, so the round-trip assertions in
//     app/selftest/PsdLayerSection.cpp cannot catch it and do not claim to:
//     the psd-tools check on a written file is what covers this bit, and
//     that check is recorded in this landing's report rather than left as
//     an intention.
//
//  3. **The channel table lists alpha FIRST.** Order is `-1, 0, 1, 2`, not
//     `0, 1, 2, -1`. This was **read off the real files rather than
//     assumed**: psd-tools' `channel_info` for every layer of
//     `Lineart4_crop.psd` that has transparency lists
//     `(TRANSPARENCY_MASK -1), (CHANNEL_0), (CHANNEL_1), (CHANNEL_2)` in
//     that order, and its `Background` layer -- fully opaque -- carries only
//     the three colour channels and no `-1` at all. Our own reader is
//     order-agnostic (it dispatches on the id), so a round trip cannot see
//     this either; it matters only to Photoshop and to every other external
//     reader, which is why it is matched to observed output rather than
//     chosen.
//
//     This writer always emits the `-1` channel, including for a
//     fully-opaque layer where Photoshop would omit it. That is legal (the
//     id is what a reader dispatches on, not the position) and it keeps one
//     code path instead of two; the cost is a fully-opaque layer carrying a
//     constant-255 channel that PackBits compresses to almost nothing.
//
// --- Layer rectangles: tight, and clipped to the canvas -------------------
//
// The rect written is the bounding box of the layer's **occupied tiles**
// (tiles holding at least one sample with alpha > 0), clipped to
// `[0, doc.width) x [0, doc.height)`. Not the full canvas: a Photoshop
// document whose layers are each stored at full canvas size with a
// hand-sized patch of content is what cost a measured import 6.2 GB
// (io/PsdImport.cpp's `writeLayerPixelsAt()` comment), and a writer has the
// opposite freedom.
//
// Tile granularity, not pixel granularity, and that is the plan's own
// wording rather than a shortcut discovered here: the bound is at worst 127
// pixels loose on each side, which is negligible against a full canvas and
// costs one scan instead of two.
//
// **Clipping to the canvas is a deliberate loss and is warned about.** PSD
// itself permits a layer rect wholly or partly off-canvas -- io/PsdImport.cpp
// reads negative `top`/`left` and has a `--selftest` fixture for it -- so
// this is a first-landing simplification, not a format limit. A layer with
// content outside the canvas gets a warning naming it and the number of
// samples dropped, per PRD I11.
//
// A layer with no occupied tiles at all gets an **empty** rect
// (`top == bottom`, `left == right`) and four channels of declared length
// zero. That is legal, it is what Photoshop writes for an empty layer, and
// io/PsdImport.cpp reads it back without decoding anything (its channel walk
// reaches `if (pixelCount == 0) continue` before any decode).
//
// --- Colour ---------------------------------------------------------------
//
// A `TileStore` holds **premultiplied linear** RGBA (core/TileStore.hpp).
// PSD wants **straight (unassociated), sRGB-encoded 8-bit**. So each sample
// is un-premultiplied (guarded at `a == 0`, where the straight colour is
// undefined and 0 is written), clamped to [0, 1], and passed through
// `color::srgbEncode()` -- the exact inverse of the `srgbDecode()`
// io/PsdImport.cpp applies on the way in. Alpha is linear opacity and is
// never gamma-encoded, matching the reader.
//
// **The clamp is a warning with a number in it**, the precedent
// io/ExportAs.hpp sets ("which highlight value an integer depth will clip"):
// a layer carrying scene-referred values above 1.0 says how many samples
// were clipped rather than quietly flattening them.
//
// **8-bit only.** docs/psd-export.md's own finding: a 16-bit layered PSD
// does not put its layers in the layer info section at all -- Photoshop
// writes them into an `Lr16` block and leaves the ordinary layer info length
// at zero, and there is no `Lr16` case in our reader. Writing 16-bit samples
// into an ordinary layer info section produces a file Photoshop reads as
// corrupt and our own reader reads as flat. Nothing here takes a bit depth
// parameter, so there is no 16-bit path to get wrong and no `65535` constant
// for a later landing to have to find.
//
// --- Masks, groups and blend keys: written elsewhere, wired in here -------
//
// This module landed alongside two others and deliberately did not
// anticipate either one's shape. All three are now wired together:
//
// **Masks and groups** are `io/PsdLayerExtras`' free functions --
// `psdMaskRect()`, `writePsdMaskBlock()`, `encodePsdMaskChannel()`,
// `planPsdRecords()`, `writePsdLsctBlock()`. They reach a record through
// two seams that are plain pre-serialised byte buffers, so neither module
// compiles against the other's types: `PsdLayerRecord::maskBlock` (the
// 20-byte mask record, or empty for `u32 0`) and
// `PsdLayerRecord::extraBlocks` (already-framed `8BIM` blocks, which is
// where an `lsct` divider or header arrives).
//
// **Blend keys** are `io/PsdBlendKeys`' shared table -- the same 26 rows
// io/PsdImport reads on the way in. A three-row stub stood here while that
// table was being promoted out of io/PsdImport.cpp's anonymous namespace;
// it is gone.
//
// **No `curv`, no `levl`, ever.** PLAN.md:640: this codebase's curves live in
// the shaper log domain, and a `curv` block would be read by Photoshop as a
// plain tone curve and be silently, confidently wrong. An Adjustment layer
// exports as pixels or as a warning, never as translated parameters.
//
// --- Vector layers: geometry AND a raster, which is not belt and braces ---
//
// docs/psd-vector-shapes.md's S1 is closed here. A `LayerKind::Vector` layer
// used to be the one kind that produced a record with nothing in it at all --
// the composite came from `flattenDocumentToLinear()`, which materialises
// vector layers, while the record came from the raw `Layer`, which has no
// tiles by design (core/VectorRaster.hpp section 1). Open the file anywhere
// and the picture was right; open its layers and the artwork was gone.
//
// Such a layer now writes **both** halves, which is what Photoshop itself does
// with Maximize Compatibility on -- verified on real files rather than taken
// from the documentation, since `testNonSquareWithShapesOffPage.psd`'s shape
// layers carry real rasters while `App Icon Template.psd`'s are 0x0:
//
//   * a genuine `vsms` + `SoCo` shape layer (io/PsdVectorWrite, which inverts
//     io/PsdVectorPath's decoder and io/PsdVectorStyle's `readClrColor()`), so
//     a reader that understands shapes gets editable geometry back; and
//   * ordinary channel data, rasterised through the same
//     `rasterizeVectorLayer()` the compositor reaches through
//     `MaterializedDocument`, so a reader that does not gets correct pixels.
//
// Neither half silently loses artwork, and because the raster and the merged
// composite come from one rasteriser they cannot disagree about the picture.
//
// What a shape layer written here still loses is per-shape rather than
// per-kind, so it is warned about with numbers rather than by one sentence:
// shapes past the first (PSD's shape layer is exactly one path), a stroke or a
// clip (both need a `vstk` descriptor this build does not write), and a fill
// colour's alpha (PSD's shape colour has no alpha field). io/PsdVectorWrite.hpp
// adds the one loss that is invisible from here -- a NonZero compound's hole
// comes back labelled Union rather than Subtract, which draws identically.
//
// --- `lclr`: the sheet colour, and the label that is refused ---------------
//
// `Layer::colorLabel` writes an `lclr` block through io/PsdLayerExtras. An
// empty label writes no block, which is the format's own "no label"; a label
// outside `kLayerColorLabelNames` writes no block either and warns naming it,
// the same rule `psdBlendKeyFor()` already follows for a blend mode PSD has no
// key for.

namespace np {

// One channel of one layer record: its PSD id and the whole channel block,
// **including its own leading `u16` compression word**.
//
// The block carries the compression word rather than having it written at
// emit time because the record's channel table has to declare a length that
// includes it (io/PsdImport.cpp slices exactly `ch.length` bytes and hands
// the slice to `decodeChannelData()`, which reads the compression word out
// of the front of it). Keeping the two together makes the length one
// `data.size()` rather than an arithmetic relationship two places have to
// agree about.
//
// An empty `data` means a declared length of zero -- the empty-layer case.
struct PsdChannelBlock {
  int16_t id = 0;  // -2 mask, -1 alpha, 0/1/2 = R/G/B
  std::vector<uint8_t> data;
};

// Everything one layer record needs, resolved out of a `Layer` and ready to
// serialise. Split out from the writing so that the mask and group work can
// add to a built record without editing the writer -- see this header's
// "what this landing deliberately does not do".
struct PsdLayerRecord {
  int32_t top = 0, left = 0, bottom = 0, right = 0;

  std::string name;

  // Four characters, and the trailing space in a three-letter key is real:
  // Photoshop pads `"mul "` and `"lum "` with a SPACE, never a NUL, and
  // io/PsdImport.cpp's `fourccEquals()` compares all four bytes.
  std::string blendKey = "norm";

  uint8_t opacity = 255;
  bool clipped = false;
  bool hidden = false;

  // In the order they will be written, which is the order the channel image
  // data must follow. `buildPsdLayerRecord()` produces `-1, 0, 1, 2`.
  std::vector<PsdChannelBlock> channels;

  // --- The two seams the mask/group work is wired into --------------------

  // The CONTENT of the layer-mask block in extra data, without its own `u32`
  // length prefix -- so empty means "write `u32 0`", which is this landing's
  // only case, and a 20-byte buffer is the plain mask record
  // io/PsdImport.cpp reads (`maskLen == 20`). Anything other than 0 or 20
  // bytes is refused BY OUR OWN READER by name, so do not put one here.
  std::vector<uint8_t> maskBlock;

  // Additional Layer Information blocks appended after this record's `luni`,
  // already framed as `8BIM` + 4-char key + `u32` length + data, and already
  // padded to an even length each. Raw bytes rather than a typed list
  // precisely so that whatever produces an `lsct` block does not have to
  // share a type with this module.
  //
  // **Each block's declared length must match its data exactly and the total
  // must be even**: io/PsdImport.cpp walks this region block by block
  // (`while (ec.remaining() >= 12)`) and a stray pad byte it did not expect
  // desynchronises the walk into reading a length out of the middle of a
  // key.
  std::vector<uint8_t> extraBlocks;
};

// The blend key for a `core::BlendMode`, as a convenience over
// io/PsdBlendKeys' shared table.
//
// **This was a three-row stub while the table was being promoted out of
// io/PsdImport.cpp's anonymous namespace; it is now wired to the real
// thing** -- the same 26 rows the importer reads on the way in, so a file
// this build writes and reads back cannot disagree with itself. The only
// thing this adds over `np::psdBlendKeyFor(mode)` is turning that
// function's `nullptr` (a mode with no Photoshop key at all, today exactly
// `BlendMode::Mix`) into `"norm"` plus an `exactMatch` of false, which is
// the shape this module's caller already warns on.
const char* psdBlendKeyFor(BlendMode mode, bool& exactMatch);

// Resolves one `Layer` into a serialisable record.
//
// Returns false only for a layer that must not become a record at all --
// today, exactly `LayerKind::Group` (see this header's scope note). Every
// other kind produces a record, and every kind that loses something on the
// way produces a warning naming the layer and what was lost (PRD I11):
// Pigment its latents, Adjustment its op stack, Strokes its dab records,
// Text its editability, Flats its fill table. A kind with no `rgbTiles` to
// rasterise from produces an EMPTY record and says so -- this build has no
// per-layer pigment resolve to call here, and "exported empty, here is why"
// is the honest form of that.
//
// **Vector is no longer one of them**: it rasterises here and writes its
// geometry as well, per this header's own section above.
bool buildPsdLayerRecord(const Layer& layer, const Document& doc, PsdLayerRecord& out,
                         std::vector<std::string>& warnings);

// Writes one record's fixed part plus its extra data (mask block, blending
// ranges, Pascal name, `luni`, then `extraBlocks`). Does NOT write channel
// image data -- that lives after every record, which is why it is a second
// function.
void writePsdLayerRecord(PsdWriter& w, const PsdLayerRecord& rec);

// Writes one record's channel image data, in the record's own channel order.
void writePsdLayerChannelData(PsdWriter& w, const PsdLayerRecord& rec);

struct PsdLayerSectionOptions {
  // Writes the layer count NEGATIVE, which is PSD's way of saying "the
  // first alpha channel of the merged composite holds the composite's own
  // transparency". Our own reader takes the absolute value and never looks
  // at the sign (io/PsdImport.cpp:1122), so this is invisible to a round
  // trip and matters only to Photoshop -- and it is a claim about the Image
  // Data Section, which this module does not write. It defaults true
  // because the container this section is nested in writes 4 channels
  // (RGB + composite alpha); a container that writes 3 must set it false.
  bool compositeCarriesTransparency = true;
};

struct PsdLayerSectionResult {
  bool ok = false;

  // Set only when `!ok`. A refusal here is total -- io/Descriptor.hpp's "a
  // refusal is total", io/NpaintFile's "no half-built document" -- and the
  // caller must not write the bytes accumulated so far to a file: a section
  // that declared a length it did not then fill is exactly the corruption
  // that produces a file which parses for a while and then does not.
  std::string error;

  // Non-fatal, and every one of them names a layer: an unmapped blend mode,
  // a kind whose latents/ops/dabs/editability could not be carried, a
  // clipped highlight with a count, content dropped off the canvas edge, a
  // group whose structure was not carried.
  std::vector<std::string> warnings;

  // How many records were actually written. Fewer than `doc.layers.size()`
  // whenever a Group was skipped.
  size_t layersWritten = 0;
};

// Writes the whole Layer and Mask Information section -- its own `u32`
// length, the layer info sub-section (length, `i16` count, every record,
// then every record's channel data, padded to even), and a zero-length
// global layer mask info block.
//
// The caller supplies the writer, because this section sits between the
// Image Resources and Image Data sections of a file this module does not
// own.
PsdLayerSectionResult writePsdLayerAndMaskInfo(PsdWriter& w, const Document& doc,
                                               const PsdLayerSectionOptions& options = {});

}  // namespace np
