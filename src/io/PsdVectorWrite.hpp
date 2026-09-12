#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "core/Path.hpp"
#include "io/PsdWrite.hpp"

// io/PsdVectorWrite -- a Photoshop shape layer, WRITTEN. The inverse of
// io/PsdVectorPath's `decodePsdPathRecords()` and of io/PsdVectorStyle's
// `readClrColor()`, closing docs/psd-vector-shapes.md's S1.
//
// --- What S1 was, and why both halves are written -------------------------
//
// `writePsd()` takes its composite from `flattenDocumentToLinear()`, which
// materialises Vector layers, and hands the RAW document to the layer
// section. So a PSD written from a document with shape layers used to carry a
// merged image that shows the shape and a layer record that is empty: open
// the file anywhere and the picture is right, open its layers and the artwork
// is gone.
//
// docs/psd-vector-shapes.md offers two closures -- rasterise the layer into
// its record, or write genuine `vsms` + `SoCo` geometry. **Both are written**,
// because that is what Photoshop itself does with Maximize Compatibility on,
// and it was verified here rather than taken from the documentation: the
// shape layers in `testNonSquareWithShapesOffPage.psd` carry real rasters,
// while the ones in `App Icon Template.psd` -- saved without it -- are 0x0. A
// reader that understands shapes gets editable geometry; one that does not
// gets correct pixels; neither silently loses artwork.
//
// **The geometry is written here, the raster in io/PsdLayerSection**, because
// the raster is just an ordinary channel block built from an ordinary
// `TileStore` -- the one `core/VectorRaster.hpp`'s `rasterizeVectorLayer()`
// already produces for the compositor. Nothing in this module rasterises, so
// there is no second rasteriser to drift from the one on screen.
//
// --- `vsms`, not `vmsk`, and the reason is the stroke --------------------
//
// Photoshop writes both keys on a modern shape layer and they carry
// byte-identical payloads; io/PsdImport decodes either (first one wins). This
// writer emits `vsms` alone.
//
// `vmsk` is also how a VECTOR MASK on an ordinary raster layer is stored --
// outline plus real pixels, which docs/psd-vector-shapes.md S4 is about -- so
// a reader that does not understand shape layers can legitimately read a
// `vmsk` as "clip this layer's pixels to this outline". The raster this
// writer stores beside the geometry includes the outer half of a centred
// stroke, which such a reader would then clip away. One key, one meaning,
// and the half-stroke survives.
//
// --- The fill rule, and the one thing it does not carry ------------------
//
// PSD has no fill-rule field. The rule is carried by the per-subpath boolean
// operation, and `composePsdSubPaths()` folds those back into a rule, so the
// encoder inverts that fold:
//
//     FillRule::NonZero  -> every subpath written as Union   (1)
//     FillRule::EvenOdd  -> every subpath written as Exclude (0)
//
// Both re-import to the same *rendering*, which is the property
// app/selftest/PsdExport.cpp asserts by rasterising rather than by comparing
// enum values.
//
// **What is not preserved is a label, not a pixel.** A NonZero compound whose
// hole is a reversed subpath comes back labelled Union rather than Subtract:
// the winding does the work, so it draws identically, but a round trip
// through Photoshop's own UI would show "Combine" where the original said
// "Subtract". Also not preserved: a *single*-subpath EvenOdd path comes back
// NonZero, because `composePsdSubPaths()` only takes a rule vote from
// subpaths 1..n-1 -- identical rendering for any subpath that does not
// self-intersect, and this codebase has no way to produce a rule difference
// on one subpath that it could then lose.
//
// --- The Action Descriptor writer ----------------------------------------
//
// io/Descriptor PARSES Photoshop's versioned Action Descriptors and nothing
// in the tree wrote one before this. The primitives below are exactly enough
// to emit `SoCo`, and they are here rather than in io/Descriptor so that a
// future writer of `vscg`, `vstk` or `lfx2` extends this file instead of
// starting over: such a writer adds `bool`, `long`, `UntF`, `enum` and `VlLs`
// items beside `writePsdDescriptorDouble()` and needs nothing else.
//
// Two quirks, both of which io/Descriptor.hpp documents from the reading side
// and both of which a writer gets wrong by being reasonable:
//
//  * **A `Key` of four characters is written with a length of ZERO**, not 4.
//    Both are legal and our own reader takes both, but zero is what Photoshop
//    writes, and the keys here are space-padded four-character ones (`"Clr "`,
//    `"Rd  "`, `"Grn "`, `"Bl  "`) where a trailing space is real data.
//  * **A UnicodeString's trailing NUL is INSIDE its code-unit count.** An
//    empty class name is therefore `count = 1` and one zero unit, not
//    `count = 0`. io/PsdWrite's `unicodeString()` deliberately writes no NUL
//    (a layer name must not grow one per round trip); a descriptor's class
//    name must, so this module writes its own.
//
// With those two, `encodePsdSolidColorBlock()` is **byte-identical** to the
// real `SoCo` block Photoshop wrote for `App Icon Shape`, which
// app/selftest/PsdExport.cpp asserts against that block's own hex.
namespace np {

// --- Action Descriptor primitives ----------------------------------------

// A `Key`: `u32 length` then the text. A four-character key writes length 0
// and then its four bytes -- see this header's "zero means four".
void writePsdDescriptorKey(PsdWriter& w, std::string_view key);

// A Photoshop UnicodeString, with the trailing NUL counted -- see above.
void writePsdDescriptorUnicodeString(PsdWriter& w, std::string_view utf8);

// A descriptor's own head: class name (empty in everything here), class id,
// item count. Used for the root and for every nested `Objc` alike, because
// the framing is identical -- which is why `writePsdDescriptorObjectItem()`
// below is this plus two fields and not a second layout.
void writePsdDescriptorHead(PsdWriter& w, std::string_view classId, uint32_t itemCount);

// One item whose value is a nested descriptor: the item's key, the `Objc`
// type code, then that descriptor's head. The caller writes the nested items
// immediately after.
void writePsdDescriptorObjectItem(PsdWriter& w, std::string_view key, std::string_view classId,
                                  uint32_t itemCount);

// One `doub` item: key, `doub`, then an IEEE-754 binary64 big-endian.
void writePsdDescriptorDouble(PsdWriter& w, std::string_view key, double value);

// --- `vsms`: the path record stream --------------------------------------

// The version word every `vsms`/`vmsk` block measured here carries, and the
// flags word beside it. Read off real blocks rather than chosen: both
// `App Icon Template.psd` and `testNonSquareWithShapesOffPage.psd` write 3
// and 0, and io/PsdVectorPath's decoder reads past both without validating
// them, so a wrong value here would be invisible to a round trip.
inline constexpr uint32_t kPsdPathBlockVersion = 3;
inline constexpr uint32_t kPsdPathBlockFlags = 0;

// The worst-case round-trip error of one coordinate on an axis of
// `dimension` pixels, in pixels.
//
// 8.24 fixed point divides the axis into 2^24 steps and the encoder rounds to
// the nearest, so the quantisation alone is half a step; the decoder's
// `float` result then carries up to one ulp of `dimension` on top, which at
// 2^-24 relative is the same order again. Two steps covers both -- 1.2e-4 px
// on a 1024-px axis. Exposed so a test asserts the bound the encoding
// actually has rather than a round number somebody liked.
inline float psdPathCoordTolerance(int32_t dimension) noexcept {
  return 2.0f * static_cast<float>(dimension) / 16777216.0f;
}

struct PsdVectorShapeMask {
  // The whole `vsms` payload, including its 8-byte header, padded to a
  // multiple of 4 the way Photoshop pads it (`192 = 8 + 7*26 + 2`).
  // io/PsdVectorPath's decoder takes the record count as a floor division and
  // discards exactly that remainder.
  std::vector<uint8_t> bytes;

  // How many coordinates saturated the int32 fixed-point field. Reachable
  // only by a point more than 128 document-dimensions off-canvas (the field
  // spends 24 bits on the fraction, so |raw| < 2^31 means |coord| < 128 *
  // dimension). Saturated rather than wrapped, because wrapping puts the
  // point somewhere plausible and wrong; the caller names the count.
  size_t saturatedCoords = 0;

  // How many subpaths were written. Zero for a path with nothing in it, in
  // which case `bytes` is empty and no block should be emitted at all.
  size_t subpathsWritten = 0;
};

// Encodes `path` as a `vsms` payload against a `docWidth` x `docHeight`
// canvas.
//
// Coordinates go out as signed 8.24 fixed point, **vertical over height and
// horizontal over width** and y before x in each pair -- the field order
// io/PsdVectorPath.hpp names as the one most likely to be transposed from
// memory. A knot's six fields are (in.y, in.x, pt.y, pt.x, out.y, out.x).
//
// A subpath of fewer than two anchors writes nothing: it encloses no area
// (`pathIsEmpty()`'s own test) and a one-knot subpath is a shape Photoshop's
// own tools cannot make.
//
// Returns an empty `bytes` for a path with no writable subpath, so a caller
// can test `bytes.empty()` instead of duplicating the emptiness rule.
PsdVectorShapeMask encodePsdVectorShapeMask(const Path& path, int32_t docWidth,
                                            int32_t docHeight);

// --- `SoCo`: the solid fill colour ---------------------------------------

// The `SoCo` payload for one linear, straight-alpha colour: a versioned
// Action Descriptor of class `null` carrying `Clr ` as an `Objc` of class
// `RGBC` with `Rd  `, `Grn ` and `Bl  ` as `doub` in 0..255.
//
// The channels go out sRGB-ENCODED and scaled by 255, the exact inverse of
// io/PsdVectorStyle's `readClrColor()` (`srgbDecode(value / 255)`). Alpha is
// **dropped**: PSD's shape colour has no alpha field at all, and a shape's
// transparency lives in the layer's own opacity. A caller with a colour whose
// alpha is not 1 should say so; this function cannot.
std::vector<uint8_t> encodePsdSolidColorBlock(const std::array<float, 4>& linearRgba);

}  // namespace np
