#include "io/Export.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "color/Space.hpp"
#include "core/Layer.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"

// NOTE on STB_IMAGE_WRITE_IMPLEMENTATION: this file is the one translation
// unit that defines it, so this is where stb_image_write.h's function bodies
// (including stbi_zlib_compress(), which encodePng16() below needs and which
// is *not* declared in the header-only portion of that file) are compiled
// in. That macro may only be defined once across the whole binary.
//
// It used to live in app/SelfTest.cpp, back when nothing but --selftest
// wrote an image. Now that PRD B6's export path is production code, the
// implementation belongs in the production module and app/SelfTest.cpp
// includes stb_image_write.h with no implementation macro, linking against
// the bodies compiled here -- the same arrangement paint/Palette.cpp and
// io/ImageDecode.cpp already have for stb_image.h's STB_IMAGE_IMPLEMENTATION
// (see the note at the top of io/ImageDecode.cpp).
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace np {
namespace {

// --- Format capability table --------------------------------------------
//
// The single place "what can this format actually carry" is written down.
// Every refusal below reads from here rather than re-deriving the answer,
// so there is no way for one code path to believe TGA is 16-bit-capable
// while another believes it isn't.
//
//   maxBits -- bits per channel this build can actually write for the
//     format. PNG is 16 because encodePng16() exists (PRD B6, and PRD I1's
//     "no optional dependency" is why it had to be hand-rolled rather than
//     delegated to a second library). JPEG is 8 by format definition
//     (baseline JPEG is 8-bit-per-sample). TGA and BMP are 8 because
//     stb_image_write writes 24/32-bit-per-*pixel* files for them -- i.e.
//     8 bits per channel -- and there is no deeper writer here.
//
//   hasAlpha -- whether the written file has a place to put the alpha
//     channel at all. stb_image_write's PNG (comp=4), TGA (comp=4 -> 32-bit
//     with an 8-bit alpha field) and BMP (comp=4 -> a BITMAPV4HEADER
//     BI_BITFIELDS 32bpp file with an 0xff000000 alpha mask) all do. Its
//     JPEG encoder reads only offsets 0/1/2 of each pixel and discards the
//     fourth outright -- see stbi_write_jpg_core's `ofsG`/`ofsB`.
struct FormatCaps {
  int maxBits;
  bool hasAlpha;
};

FormatCaps capsFor(ExportFormat format) {
  switch (format) {
    case ExportFormat::Png: return {16, true};
    case ExportFormat::Jpeg: return {8, false};
    case ExportFormat::Tga: return {8, true};
    case ExportFormat::Bmp: return {8, true};
  }
  return {8, false};
}

// JPEG quality passed to stb_image_write. Not an export parameter: PRD I5
// asks for target colour space and bit depth to be explicit, and PLAN.md
// step 7's "Export As" is where format-specific knobs and saveable presets
// belong. 95 is chosen as "visually lossless for a first pass" rather than
// stb's default; recorded here so it is a decision someone can find and
// change, not an invisible constant.
constexpr int kJpegQuality = 95;

// stb_image_write's write_to_func callback: append `size` bytes to the
// std::vector<uint8_t> passed as `context`. Same shape as the one
// app/SelfTest.cpp uses for its own in-memory fixtures -- everything here
// encodes to memory first and only then, in exportDocumentToFile(),
// touches the filesystem, so a refused request can never leave a
// half-written file behind.
void appendToVector(void* context, void* data, int size) {
  auto* v = static_cast<std::vector<uint8_t>*>(context);
  const auto* b = static_cast<const uint8_t*>(data);
  v->insert(v->end(), b, b + size);
}

// straight[i] = premultiplied[i] / a for the RGB channels, guarding a <= 0
// (fully transparent -- RGB is arbitrary under premultiplied alpha, and 0
// is the convention core::Tile's own value-initialization already uses for
// an untouched texel).
//
// This is deliberately the *same* guard shape as core/Probe.cpp's
// unpremultiply(), not a second, subtly different one: both undo
// io/ImageIO.cpp's `rgb *= a` at a read boundary, and if the two ever
// disagreed about the alpha == 0 case, a probe and an export of the same
// pixel would report different colours. It is duplicated rather than
// hoisted into a shared header purely because promoting it would mean
// editing core/Probe, which is outside this step's scope; if a third
// caller appears, that promotion is the right move.
std::array<float, 4> unpremultiply(const std::array<float, 4>& premultiplied) {
  const float a = premultiplied[3];
  if (a <= 0.0f) return {0.0f, 0.0f, 0.0f, 0.0f};
  return {premultiplied[0] / a, premultiplied[1] / a, premultiplied[2] / a, a};
}

// Chromaticity coordinates are authored constants in this codebase
// (color/Space.hpp's kRec709Primaries), so exact equality would work today.
// The small epsilon is for the case that is coming rather than the case
// that exists: a working space read back from a file's `chromaticities`
// attribute (docs/document-format.md) or round-tripped through a UI field
// would be bit-identical only by luck, and rejecting an export because a
// primary landed one ULP away would be a false alarm. 1e-6 is far below any
// real difference between gamuts -- Rec.709's and ACEScg's red x differ by
// 0.073 -- so it cannot mask a genuine mismatch.
bool primariesMatch(const Primaries& a, const Primaries& b) {
  constexpr float kEps = 1e-6f;
  auto same = [](float x, float y) { return std::fabs(x - y) <= kEps; };
  return same(a.redX, b.redX) && same(a.redY, b.redY) && same(a.greenX, b.greenX) &&
         same(a.greenY, b.greenY) && same(a.blueX, b.blueX) && same(a.blueY, b.blueY) &&
         same(a.whiteX, b.whiteX) && same(a.whiteY, b.whiteY);
}

// The RGB transfer function for a target space. Alpha never comes through
// here -- alpha is opacity, not light (io/ImageDecode.hpp, core/Probe.hpp)
// -- which is enforced structurally by encodeChannels() below only ever
// calling this for channels 0..2.
float encodeRgbChannel(ExportTargetSpace space, float linear) {
  switch (space) {
    case ExportTargetSpace::Rec709Linear: return linear;
    case ExportTargetSpace::Rec709Srgb: return srgbEncode(linear);
    case ExportTargetSpace::Rec709Bt709: return rec709Encode(linear);
  }
  return linear;
}

// Clamp to [0, 1] and quantize to `maxValue` full scale, rounding
// half-away-from-zero. Written as `!(v > 0)` rather than std::max so a NaN
// (which compares false against everything) lands on 0 instead of
// propagating into an undefined float->integer conversion.
uint32_t quantize(float v, uint32_t maxValue) {
  if (!(v > 0.0f)) return 0;
  if (v >= 1.0f) return maxValue;
  return static_cast<uint32_t>(v * static_cast<float>(maxValue) + 0.5f);
}

// Finds the first pixel with alpha < 1, for the JPEG-has-no-alpha refusal's
// error string. Returning *which* pixel (rather than just "there is one")
// is the difference between an error a user can act on and one they have to
// go hunting behind.
bool findFirstTranslucentPixel(const DecodedImage& img, uint32_t* xOut, uint32_t* yOut,
                               float* alphaOut) {
  for (uint32_t y = 0; y < img.height; ++y) {
    for (uint32_t x = 0; x < img.width; ++x) {
      const float a = img.pixels[(static_cast<size_t>(y) * img.width + x) * 4 + 3];
      if (a < 1.0f) {
        *xOut = x;
        *yOut = y;
        *alphaOut = a;
        return true;
      }
    }
  }
  return false;
}

ExportResult failure(std::string message) {
  ExportResult r;
  r.ok = false;
  r.error = std::move(message);
  return r;
}

}  // namespace

DecodedImage flattenDocumentToLinear(const Document& doc) {
  DecodedImage out;
  if (doc.width <= 0 || doc.height <= 0) return out;  // valid() == false

  const uint32_t w = static_cast<uint32_t>(doc.width);
  const uint32_t h = static_cast<uint32_t>(doc.height);
  const size_t sampleCount = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;

  // Accumulates in premultiplied space -- see the header comment for why
  // that has to happen before un-premultiplying, not after. Zero-filled: an
  // untouched pixel is transparent black, exactly what core::Tile gives an
  // unwritten texel, so nothing needs a separate "was anything here" flag.
  std::vector<float> premultiplied(sampleCount, 0.0f);

  for (const Layer& layer : doc.layers) {
    if (layer.kind != LayerKind::RGB || !layer.rgbTiles.has_value()) continue;

    // Iterates the tiles that exist, never a grid across the canvas
    // (TileStore's own begin()/end()), so an empty or sparsely painted
    // document costs nothing per unpainted tile.
    for (const auto& [coord, tile] : *layer.rgbTiles) {
      const PixelCoord origin = tileOrigin(coord);
      for (int32_t ty = 0; ty < kTileSize; ++ty) {
        const int32_t docY = origin.y + ty;
        if (docY < 0 || docY >= doc.height) continue;  // clipped to the canvas
        for (int32_t tx = 0; tx < kTileSize; ++tx) {
          const int32_t docX = origin.x + tx;
          if (docX < 0 || docX >= doc.width) continue;

          const std::array<float, 4> px = tile.readPixel(PixelCoord{tx, ty});
          float* dst = &premultiplied[(static_cast<size_t>(docY) * w +
                                        static_cast<size_t>(docX)) * 4];
          // Plain sum, not Porter-Duff "over" -- core/Probe.cpp's
          // sampleAllLayers path made and documented this same decision for
          // the same reason (no compositing implementation exists yet, and
          // at most one layer holds content at a given point today).
          dst[0] += px[0];
          dst[1] += px[1];
          dst[2] += px[2];
          dst[3] += px[3];
        }
      }
    }
  }

  out.width = w;
  out.height = h;
  out.pixels.resize(sampleCount);
  for (size_t i = 0; i < sampleCount; i += 4) {
    const std::array<float, 4> straight = unpremultiply(
        {premultiplied[i + 0], premultiplied[i + 1], premultiplied[i + 2], premultiplied[i + 3]});
    out.pixels[i + 0] = straight[0];
    out.pixels[i + 1] = straight[1];
    out.pixels[i + 2] = straight[2];
    out.pixels[i + 3] = straight[3];
  }
  return out;
}

ExportResult encodeLinearImage(const DecodedImage& img, const WorkingSpace& sourceSpace,
                               ExportFormat format, ExportTargetSpace targetSpace,
                               ExportBitDepth bitDepth) {
  if (!img.valid()) {
    return failure("export refused: there is nothing to encode (the image to export has zero "
                   "width or height, or a pixel buffer whose size does not match it).");
  }

  // --- Primaries: converted by nobody, so a mismatch is refused ----------
  //
  // See io/Export.hpp's scope-decision section. Silently ignoring this is
  // the one outcome that is definitely wrong: the file would be labelled
  // (and read) as the target space while carrying values that were never
  // converted to it.
  const Primaries targetPrimaries = exportTargetPrimaries(targetSpace);
  if (!primariesMatch(sourceSpace.primaries, targetPrimaries)) {
    // 1024, not 512: this message interpolates sixteen chromaticity
    // coordinates plus the target space's full name, and the compiler's
    // -Wformat-truncation can prove 512 is too small for the format string
    // alone. Sized so the error is never silently clipped -- an error that
    // stops mid-sentence is exactly the kind of unhelpful failure this
    // module exists to avoid.
    char buf[1024];
    std::snprintf(
        buf, sizeof(buf),
        "export refused: the document's working space has primaries (red xy %.4f,%.4f; green "
        "xy %.4f,%.4f; blue xy %.4f,%.4f; white xy %.4f,%.4f) that differ from target space "
        "%s (red xy %.4f,%.4f; green xy %.4f,%.4f; blue xy %.4f,%.4f; white xy %.4f,%.4f). "
        "This build converts transfer functions only -- no RGB<->XYZ primaries-conversion "
        "matrix exists yet (color/Space.hpp) -- and writing the pixels out unconverted under "
        "the target's name would misreport their colour. Pick a target space with matching "
        "primaries, or convert the document's working space first.",
        static_cast<double>(sourceSpace.primaries.redX),
        static_cast<double>(sourceSpace.primaries.redY),
        static_cast<double>(sourceSpace.primaries.greenX),
        static_cast<double>(sourceSpace.primaries.greenY),
        static_cast<double>(sourceSpace.primaries.blueX),
        static_cast<double>(sourceSpace.primaries.blueY),
        static_cast<double>(sourceSpace.primaries.whiteX),
        static_cast<double>(sourceSpace.primaries.whiteY), exportTargetSpaceName(targetSpace),
        static_cast<double>(targetPrimaries.redX), static_cast<double>(targetPrimaries.redY),
        static_cast<double>(targetPrimaries.greenX), static_cast<double>(targetPrimaries.greenY),
        static_cast<double>(targetPrimaries.blueX), static_cast<double>(targetPrimaries.blueY),
        static_cast<double>(targetPrimaries.whiteX), static_cast<double>(targetPrimaries.whiteY));
    return failure(buf);
  }

  // --- Bit depth: honoured exactly, or refused by name (PRD B6) ----------
  const FormatCaps caps = capsFor(format);
  const int requestedBits = exportBitDepthBits(bitDepth);
  if (requestedBits > caps.maxBits) {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "export refused: %d-bit-per-channel export was requested for %s, but %s "
                  "carries at most %d bits per channel in this build. Nothing was written -- "
                  "silently writing %d bits and reporting success would be exactly the "
                  "truncation PRD B6 forbids. Request %d-bit, or export PNG, which is the only "
                  "format here that writes 16 bits per channel.",
                  requestedBits, exportFormatName(format), exportFormatName(format),
                  caps.maxBits, caps.maxBits, caps.maxBits);
    return failure(buf);
  }

  // --- Alpha: a format with no alpha channel refuses a translucent image -
  if (!caps.hasAlpha) {
    uint32_t x = 0, y = 0;
    float alpha = 1.0f;
    if (findFirstTranslucentPixel(img, &x, &y, &alpha)) {
      char buf[512];
      std::snprintf(buf, sizeof(buf),
                    "export refused: %s has no alpha channel, but the flattened document is "
                    "not fully opaque (first partially transparent pixel at x=%u, y=%u, "
                    "alpha=%.4f). Exporting would silently discard transparency. Export PNG, "
                    "TGA or BMP, which carry alpha, or composite the document onto an opaque "
                    "background first.",
                    exportFormatName(format), x, y, static_cast<double>(alpha));
      return failure(buf);
    }
  }

  // --- Encode + quantize -------------------------------------------------
  //
  // RGB through the target space's transfer function; alpha straight
  // through, never curved (alpha is opacity, not light). Then clamp and
  // quantize -- see io/Export.hpp on why the [0,1] clamp is an
  // export-policy decision made here rather than in color/Space.
  const size_t texelCount = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
  ExportResult result;

  if (bitDepth == ExportBitDepth::Sixteen) {
    std::vector<uint16_t> samples(texelCount * 4);
    for (size_t i = 0; i < texelCount; ++i) {
      const float* src = &img.pixels[i * 4];
      for (int c = 0; c < 3; ++c)
        samples[i * 4 + c] =
            static_cast<uint16_t>(quantize(encodeRgbChannel(targetSpace, src[c]), 65535u));
      samples[i * 4 + 3] = static_cast<uint16_t>(quantize(src[3], 65535u));
    }
    // Only PNG reaches here; the depth check above refused every other
    // format for 16-bit, and this is the writer that makes PNG the
    // exception (io/Export.hpp's encodePng16, promoted out of
    // app/SelfTest.cpp for exactly this).
    result.bytes = encodePng16(img.width, img.height, samples.data());
    if (result.bytes.empty()) {
      return failure("export refused: the 16-bit PNG writer produced no bytes for a "
                     "valid-looking image -- this is an internal encoder failure, not a "
                     "rejected request.");
    }
    result.ok = true;
    return result;
  }

  std::vector<uint8_t> samples(texelCount * 4);
  for (size_t i = 0; i < texelCount; ++i) {
    const float* src = &img.pixels[i * 4];
    for (int c = 0; c < 3; ++c)
      samples[i * 4 + c] =
          static_cast<uint8_t>(quantize(encodeRgbChannel(targetSpace, src[c]), 255u));
    samples[i * 4 + 3] = static_cast<uint8_t>(quantize(src[3], 255u));
  }

  const int w = static_cast<int>(img.width);
  const int h = static_cast<int>(img.height);
  int wrote = 0;
  switch (format) {
    case ExportFormat::Png:
      wrote = stbi_write_png_to_func(&appendToVector, &result.bytes, w, h, 4, samples.data(),
                                     w * 4);
      break;
    case ExportFormat::Jpeg:
      // comp = 4 is accepted by stb's JPEG encoder (it strides by 4 and
      // reads offsets 0/1/2); the alpha check above already guaranteed
      // every alpha here is 1, so nothing is being discarded.
      wrote = stbi_write_jpg_to_func(&appendToVector, &result.bytes, w, h, 4, samples.data(),
                                     kJpegQuality);
      break;
    case ExportFormat::Tga:
      wrote = stbi_write_tga_to_func(&appendToVector, &result.bytes, w, h, 4, samples.data());
      break;
    case ExportFormat::Bmp:
      wrote = stbi_write_bmp_to_func(&appendToVector, &result.bytes, w, h, 4, samples.data());
      break;
  }

  if (!wrote || result.bytes.empty()) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "export refused: the %s encoder (stb_image_write) failed on a %dx%d image -- "
                  "this is an internal encoder failure, not a rejected request.",
                  exportFormatName(format), w, h);
    return failure(buf);
  }
  result.ok = true;
  return result;
}

ExportResult exportDocument(const Document& doc, ExportFormat format,
                            ExportTargetSpace targetSpace, ExportBitDepth bitDepth) {
  const DecodedImage flat = flattenDocumentToLinear(doc);
  if (!flat.valid()) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "export refused: the document has no pixels to export (canvas is %dx%d).",
                  doc.width, doc.height);
    return failure(buf);
  }
  return encodeLinearImage(flat, doc.workingSpace, format, targetSpace, bitDepth);
}

bool exportDocumentToFile(const Document& doc, const std::string& path, ExportFormat format,
                          ExportTargetSpace targetSpace, ExportBitDepth bitDepth,
                          std::string* errorOut) {
  const ExportResult encoded = exportDocument(doc, format, targetSpace, bitDepth);
  if (!encoded.ok) {
    if (errorOut) *errorOut = encoded.error;
    return false;
  }

  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    if (errorOut) *errorOut = "export failed: could not open '" + path + "' for writing.";
    return false;
  }
  const size_t written = std::fwrite(encoded.bytes.data(), 1, encoded.bytes.size(), f);
  const bool closedOk = std::fclose(f) == 0;
  if (written != encoded.bytes.size() || !closedOk) {
    if (errorOut)
      *errorOut = "export failed: '" + path + "' was opened but not fully written.";
    return false;
  }
  return true;
}

std::vector<uint8_t> encodePng16(uint32_t width, uint32_t height, const uint16_t* rgba) {
  if (width == 0 || height == 0 || rgba == nullptr) return {};

  auto crc32 = [](const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
      crc ^= data[i];
      for (int k = 0; k < 8; ++k) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
  };
  auto pushU32BE = [](std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 24));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
  };
  auto appendChunk = [&](std::vector<uint8_t>& png, const char* tag,
                         const std::vector<uint8_t>& data) {
    pushU32BE(png, static_cast<uint32_t>(data.size()));
    std::vector<uint8_t> typeAndData(tag, tag + 4);
    typeAndData.insert(typeAndData.end(), data.begin(), data.end());
    png.insert(png.end(), typeAndData.begin(), typeAndData.end());
    pushU32BE(png, crc32(typeAndData.data(), typeAndData.size()));
  };

  // Raw scanlines: one filter-type byte (0 = None) per row, then w*4 16-bit
  // big-endian samples -- PNG's on-the-wire sample order for >8-bit depth.
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 4 * 2));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    for (uint32_t x = 0; x < width; ++x) {
      for (int c = 0; c < 4; ++c) {
        const uint16_t v = rgba[(static_cast<size_t>(y) * width + x) * 4 + c];
        raw.push_back(static_cast<uint8_t>(v >> 8));
        raw.push_back(static_cast<uint8_t>(v & 0xFF));
      }
    }
  }

  int compressedLen = 0;
  unsigned char* compressed =
      stbi_zlib_compress(raw.data(), static_cast<int>(raw.size()), &compressedLen, 8);
  if (!compressed) return {};
  std::vector<uint8_t> idat(compressed, compressed + compressedLen);
  std::free(compressed);  // stb_image_write's default allocator is malloc/free

  std::vector<uint8_t> ihdr;
  pushU32BE(ihdr, width);
  pushU32BE(ihdr, height);
  ihdr.push_back(16);  // bit depth
  ihdr.push_back(6);   // color type: truecolor + alpha
  ihdr.push_back(0);   // compression method (only one exists)
  ihdr.push_back(0);   // filter method (only one exists)
  ihdr.push_back(0);   // interlace: none

  std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};  // PNG signature
  appendChunk(png, "IHDR", ihdr);
  appendChunk(png, "IDAT", idat);
  appendChunk(png, "IEND", {});
  return png;
}

}  // namespace np
