#include "io/PsdExport.hpp"

#include "io/PsdLayerSection.hpp"

#include <cmath>
#include <cstdio>
#include <span>

#include "color/Space.hpp"
#include "io/Export.hpp"
#include "io/PackBits.hpp"

namespace np {
namespace {

// PSD's own canvas ceiling, and the reason PSB exists. io/PsdImport.cpp:1049
// refuses anything past it on read; this is the same number on the way out, and
// it is spelled here rather than shared because the reader's copy is a bound on
// a parsed field and this one is a bound on a document -- two different
// questions that happen to have the same answer.
constexpr int32_t kMaxCanvasEdge = 30000;

// The composite's channel count: R, G, B and the composite alpha. Not a
// document property in this build -- there are no spot channels -- so it is a
// constant here rather than a computed one that could only ever be 4.
constexpr uint16_t kCompositeChannelCount = 4;

// PSD compression words. 0 is raw, 1 is PackBits per scanline. This writer
// always writes 1: raw would be simpler and is legal, but a flat 8-bit
// composite of a painting compresses several-fold under PackBits and the
// encoder is already written and asserted as decodePackBits()'s inverse.
constexpr uint16_t kCompressionRle = 1;

// Round-half-away-from-zero to 8 bits, with the NaN behaviour io/Export.cpp's
// quantize() has and for the same reason: written as `!(v > 0)` rather than
// std::max so a NaN (which compares false against everything) lands on 0
// instead of propagating into an undefined float-to-integer conversion.
//
// This is deliberately the same arithmetic as io/Export.cpp's, not a second
// reading of it -- a PSD and a PNG exported from one document should carry
// identical bytes, and they do only if both quantise the same way.
uint8_t quantize8(float v) {
  if (!(v > 0.0f)) return 0;
  if (v >= 1.0f) return 255;
  return static_cast<uint8_t>(v * 255.0f + 0.5f);
}

// A float formatted for a warning: short, and never in scientific notation
// where a plain decimal reads better. Used only for the clipping report.
std::string shortFloat(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
  return std::string(buf);
}

// "1 colour sample was" / "7 colour samples were". A warning a user reads is
// not the place to save four lines with "1 sample(s)".
std::string sampleCount(uint64_t n) {
  return std::to_string(n) + (n == 1 ? " colour sample was" : " colour samples were");
}

}  // namespace

std::string psdContainerRefusal(const Document& doc) {
  if (doc.width <= 0 || doc.height <= 0) {
    return "cannot write a PSD of a " + std::to_string(doc.width) + "x" +
           std::to_string(doc.height) +
           " document: PSD's file header has no representation for a zero-size canvas, and "
           "this build's own PSD reader refuses one on the way back in.";
  }
  if (doc.width > kMaxCanvasEdge || doc.height > kMaxCanvasEdge) {
    return "cannot write a PSD of a " + std::to_string(doc.width) + "x" +
           std::to_string(doc.height) + " document: PSD's own limit is " +
           std::to_string(kMaxCanvasEdge) +
           "px on each edge (that limit is what PSB, the Large Document Format, exists to "
           "raise, and this build writes neither PSB nor a truncated PSD).";
  }
  return std::string();
}

void writePsdFileHeader(PsdWriter& w, int32_t width, int32_t height, uint16_t channelCount) {
  w.fourcc("8BPS");
  // Version 1. Never 2 -- that is PSB, which io/PsdImport.cpp refuses by name
  // on read and which nothing here can write.
  w.u16(1);
  w.zeros(6);  // reserved, and Photoshop checks that it is zero
  w.u16(channelCount);
  // Height before width. Adobe's order, and the inverse of the
  // `c.u32(height) && c.u32(width)` pair in io/PsdImport.cpp's header parse.
  w.u32(static_cast<uint32_t>(height));
  w.u32(static_cast<uint32_t>(width));
  w.u16(8);  // depth -- 8 only, see io/PsdExport.hpp's "no 16-bit"
  w.u16(3);  // colour mode: RGB
}

bool writeMergedImageData(PsdWriter& w, const DecodedImage& flat, PsdClipReport& clipOut) {
  if (!flat.valid()) return false;

  const size_t width = flat.width;
  const size_t height = flat.height;

  // --- Planar 8-bit channels ---------------------------------------------
  //
  // All of R, then all of G, then all of B, then all of A. Built whole rather
  // than row-at-a-time because the row-count table for every channel precedes
  // every compressed row, so nothing can be written until every row's
  // compressed length is known anyway.
  std::vector<std::vector<uint8_t>> planes(kCompositeChannelCount);
  for (std::vector<uint8_t>& p : planes) p.resize(width * height);

  for (size_t i = 0, n = width * height; i < n; ++i) {
    const float* px = &flat.pixels[i * 4];
    // Alpha is opacity, not light: quantised, never gamma-encoded, and clamped
    // without a warning (io/PsdExport.hpp says why).
    const uint8_t alphaByte = quantize8(px[3]);
    planes[3][i] = alphaByte;
    // The matte is computed against the alpha byte the file will CARRY, not
    // against the float it came from, so a reader's un-matte -- which has only
    // the byte -- inverts this exactly rather than to within a second rounding.
    const float a = static_cast<float>(alphaByte) / 255.0f;

    for (int c = 0; c < 3; ++c) {
      const float linear = px[c];
      // srgbEncode is monotonic and fixes 1.0, so "linear > 1" and "encoded
      // > 1" are the same question asked either side of the curve
      // (color/Space.hpp says so in as many words). The clip is counted here,
      // where the linear value that caused it is still in hand.
      const float encoded = srgbEncode(linear);
      if (encoded > 1.0f) {
        ++clipOut.clippedHigh;
        if (linear > clipOut.largestClipped) clipOut.largestClipped = linear;
      } else if (!(encoded >= 0.0f)) {  // catches negatives AND NaN
        ++clipOut.clippedLow;
        if (linear < clipOut.mostNegativeClipped) clipOut.mostNegativeClipped = linear;
      }
      // Clamped BEFORE the matte, not by quantize8() after it: a value above
      // white would otherwise be matted from a number the file cannot carry
      // and land somewhere other than 255.
      const float display = encoded > 1.0f ? 1.0f : (encoded >= 0.0f ? encoded : 0.0f);
      // **The merged composite is matted on white.** See io/PsdExport.hpp's
      // "The matte nobody documents" -- the merged Image Data Section is a
      // PREVIEW, and every reader of one un-mattes it from white. A per-LAYER
      // channel is NOT matted; do not copy this line into one.
      planes[c][i] = quantize8(display * a + (1.0f - a));
    }
  }

  // --- PackBits, one row at a time ---------------------------------------
  //
  // One row at a time because the table needs each row's compressed length
  // separately, which is exactly the reason io/PackBits.hpp's encoder takes a
  // row rather than a buffer. Runs never straddle a row boundary here because
  // a row is all the encoder is ever given -- the property decodePackBits()'s
  // single-pass framing relies on.
  std::vector<std::vector<uint8_t>> rows;
  rows.reserve(static_cast<size_t>(kCompositeChannelCount) * height);
  for (const std::vector<uint8_t>& plane : planes) {
    for (size_t y = 0; y < height; ++y) {
      std::vector<uint8_t> packed =
          encodePackBits(std::span<const uint8_t>(plane.data() + y * width, width));
      // The row-count table has a u16 per row and nothing else. A length past
      // 65,535 would be written truncated and every byte after it misread, so
      // it is refused instead. Unreachable for any canvas PSD permits --
      // 30,000 raw bytes grow to at most 30,235 -- and checked anyway.
      if (packed.size() > 0xFFFFu) return false;
      rows.push_back(std::move(packed));
    }
  }

  w.u16(kCompressionRle);
  // The whole table first -- every row of every channel, consecutively. This
  // is the merged composite's framing and it is NOT a per-layer channel's,
  // where each channel carries its own compression word and its own table.
  for (const std::vector<uint8_t>& r : rows) w.u16(static_cast<uint16_t>(r.size()));
  for (const std::vector<uint8_t>& r : rows) w.raw(r);
  return true;
}

namespace {

// The two public entry points differ in exactly one section, so they are one
// function with one flag rather than two bodies that have to be kept in step.
// Everything else -- the size refusal, the flatten, the header, the merged
// image data, the clip warnings, the structural check -- is identical, and a
// second copy of it would be a second place for the Image Data Section to be
// forgotten.
PsdExportResult writePsd(const Document& doc, bool layered) {
  PsdExportResult result;

  auto refuse = [&result](std::string message) {
    result.ok = false;
    result.error = std::move(message);
    result.bytes.clear();  // a refusal is total -- never a partial file
    return result;
  };

  // Size first, before the flatten and before a byte. Compositing a
  // 30,001-square document would ask for 14 GiB of float before anything got
  // round to refusing it.
  const std::string refusal = psdContainerRefusal(doc);
  if (!refusal.empty()) return refuse(refusal);

  // `flattenDocumentToLinear()` already returns STRAIGHT alpha -- it
  // composites premultiplied and calls unpremultiply() over every texel before
  // returning (io/Export.cpp:150). Nothing here un-premultiplies again.
  // Its warnings name every blend mode this build approximated as `over`;
  // they are the caller's to see, so they are passed through rather than
  // dropped.
  const DecodedImage flat = flattenDocumentToLinear(doc, &result.warnings);
  if (!flat.valid()) {
    return refuse("cannot write a PSD: flattening the document produced no image.");
  }

  PsdWriter w;
  writePsdFileHeader(w, doc.width, doc.height, kCompositeChannelCount);
  w.u32(0);  // Colour Mode Data -- only Indexed and Duotone carry any
  w.u32(0);  // Image Resources -- legal as zero, see the header
  if (layered) {
    // Tier 2. The section writer owns its own `u32` length, the layer info
    // sub-section, every record, every record's channel data, and the
    // zero-length global layer mask info -- so this call IS the section.
    const PsdLayerSectionResult section = writePsdLayerAndMaskInfo(w, doc);
    if (!section.ok) return refuse("cannot write a layered PSD: " + section.error);
    // Warnings from the layer section are the caller's to see: every one of
    // them names a layer and what it lost.
    result.warnings.insert(result.warnings.end(), section.warnings.begin(),
                           section.warnings.end());
  } else {
    w.u32(0);  // Layer and Mask Information -- tier 1 has no layers
  }

  PsdClipReport clip;
  if (!writeMergedImageData(w, flat, clip)) {
    return refuse(
        "cannot write a PSD: the merged image data could not be encoded (a compressed "
        "scanline would not fit the 16-bit length its row-count table has for it).");
  }

  // PRD I11: a save that loses data names exactly what, with numbers.
  if (clip.clippedHigh > 0) {
    result.warnings.push_back(
        sampleCount(clip.clippedHigh) + " brighter than white and clip at 255 in an 8-bit "
        "PSD (the brightest was " + shortFloat(clip.largestClipped) + " in linear light). "
        "This build writes PSD 8-bit only; a float or 16-bit format would carry them.");
  }
  if (clip.clippedLow > 0) {
    result.warnings.push_back(
        sampleCount(clip.clippedLow) + " below zero and clip at 0 in an 8-bit PSD (the most "
        "negative was " + shortFloat(clip.mostNegativeClipped) + " in linear light).");
  }

  // The last refusal, and the one only the bytes can answer. `ok()` goes false
  // on a mis-sized fourcc, a bad backpatch marker, or a section past 4 GiB --
  // any of which means these bytes are structurally invalid, so they are
  // discarded rather than written.
  if (!w.ok()) {
    return refuse(
        "cannot write a PSD: the byte writer reported a structural error while building the "
        "file, so no bytes are written rather than an invalid file.");
  }

  result.ok = true;
  result.bytes = w.take();
  return result;
}

}  // namespace

PsdExportResult writeFlattenedPsd(const Document& doc) { return writePsd(doc, false); }

// **The composite is written here too, and it is not redundant.** A PSD whose
// Image Data Section is absent or blank opens BLANK in every application that
// does not parse the layer section -- which is most of them, and is the whole
// reason Photoshop's own "Maximize Compatibility" option exists. This was not
// a theoretical concern during development: the layer section's own test
// wrapper omitted this section, and psd-tools refused the resulting file
// outright ("Failed to read data section"). Layered output goes through the
// same `flattenDocumentToLinear()` call the flattened path uses, so the two
// tiers cannot disagree about what the picture is.
PsdExportResult writeLayeredPsd(const Document& doc) { return writePsd(doc, true); }

}  // namespace np
