#include "io/Export.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "color/Space.hpp"
#include "core/Composite.hpp"
#include "core/Layer.hpp"
#include "core/Premultiply.hpp"
#include "core/Tile.hpp"
#include "core/TileStore.hpp"
#include "io/Capabilities.hpp"

#include "io/OiioBackend.hpp"

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

// --- Where "what can this format actually carry" now lives ---------------
//
// It used to be a `capsFor()` table right here, covering exactly PRD I1's
// four stb formats and their two possible depths. That table is now
// io/Capabilities' kStbCapabilities, moved verbatim -- because from step 2
// on the answer is no longer knowable at compile time: whether EXR is
// writable, and at which sample types, depends on which OpenImageIO this
// binary linked against and which plugins that OpenImageIO was built with.
// io/Capabilities discovers it at run time (PRD I3) and everything below
// reads from that one query, so no code path here can believe something the
// capability report does not.
//
// Whether a format wants *associated* (premultiplied) alpha. True for EXR
// only, per the OpenEXR spec; see io/Export.hpp's Alpha section for the
// measured reason this is our job rather than OpenImageIO's, and
// io/OiioBackend.cpp for the un-association on the way back in.
bool formatWantsAssociatedAlpha(ImageFormat format) { return format == ImageFormat::Exr; }

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
  return flattenDocumentToLinear(doc, nullptr);
}

DecodedImage flattenDocumentToLinear(const Document& doc, std::vector<std::string>* warningsOut) {
  DecodedImage out;
  if (doc.width <= 0 || doc.height <= 0) return out;  // valid() == false

  const uint32_t w = static_cast<uint32_t>(doc.width);
  const uint32_t h = static_cast<uint32_t>(doc.height);
  const size_t sampleCount = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;

  // The whole layer walk now lives in core/Composite -- real `over`
  // compositing in linear light, on premultiplied values, honouring
  // `visible` and `opacity` (PLAN.md Phase 5 step 1). It used to be a plain
  // sum written out inline here, correct only while "at most one layer holds
  // painted content at a given point" held; multi-layer documents are exactly
  // what breaks that, so the sum went with it.
  //
  // The walk is in `core/` rather than here for the same reason the flattener
  // itself is not reimplemented in io/NpaintFile: compositing a Document is a
  // domain operation, not a file-format one, and core/Probe needs the same
  // arithmetic to answer "what colour is at this pixel". There must be exactly
  // one `over` in this binary.
  //
  // This function keeps what is genuinely io/Export's: the un-premultiply that
  // turns the composite into DecodedImage's straight-alpha contract.
  const std::vector<float> premultiplied = compositeDocumentPremultiplied(doc, warningsOut);

  out.width = w;
  out.height = h;
  out.pixels.resize(sampleCount);
  for (size_t i = 0; i < sampleCount; i += 4) {
    const std::array<float, 4> straight = unpremultiply(std::array<float, 4>{
        premultiplied[i + 0], premultiplied[i + 1], premultiplied[i + 2], premultiplied[i + 3]});
    out.pixels[i + 0] = straight[0];
    out.pixels[i + 1] = straight[1];
    out.pixels[i + 2] = straight[2];
    out.pixels[i + 3] = straight[3];
  }
  return out;
}

// Every refusal check encodeLinearImage() applies, in its exact order, with
// the two that need data made optional -- see io/Export.hpp for why this is
// the *only* copy of these strings rather than a preview of them.
std::string exportRefusalReason(ImageFormat format, ExportTargetSpace targetSpace,
                                ExportBitDepth bitDepth, const WorkingSpace* sourceSpace,
                                const DecodedImage* img) {
  // --- Primaries: converted by nobody, so a mismatch is refused ----------
  //
  // See io/Export.hpp's scope-decision section. Silently ignoring this is
  // the one outcome that is definitely wrong: the file would be labelled
  // (and read) as the target space while carrying values that were never
  // converted to it.
  const Primaries targetPrimaries = exportTargetPrimaries(targetSpace);
  if (sourceSpace != nullptr && !primariesMatch(sourceSpace->primaries, targetPrimaries)) {
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
        static_cast<double>(sourceSpace->primaries.redX),
        static_cast<double>(sourceSpace->primaries.redY),
        static_cast<double>(sourceSpace->primaries.greenX),
        static_cast<double>(sourceSpace->primaries.greenY),
        static_cast<double>(sourceSpace->primaries.blueX),
        static_cast<double>(sourceSpace->primaries.blueY),
        static_cast<double>(sourceSpace->primaries.whiteX),
        static_cast<double>(sourceSpace->primaries.whiteY), exportTargetSpaceName(targetSpace),
        static_cast<double>(targetPrimaries.redX), static_cast<double>(targetPrimaries.redY),
        static_cast<double>(targetPrimaries.greenX), static_cast<double>(targetPrimaries.greenY),
        static_cast<double>(targetPrimaries.blueX), static_cast<double>(targetPrimaries.blueY),
        static_cast<double>(targetPrimaries.whiteX), static_cast<double>(targetPrimaries.whiteY));
    return buf;
  }

  // --- Can this build write this format at all? (PRD I3) -----------------
  //
  // Asked, never assumed: the answer depends on which plugins the linked
  // OpenImageIO actually has. The refusal quotes the
  // capability query's own reason, so a caller learns whether the format is
  // read-only here, or its backend absent, or its plugin missing -- rather
  // than receiving a bare "unsupported".
  const FormatCapability& caps = formatCapability(format);
  if (!caps.canWrite) {
    std::string message = "export refused: this build cannot write ";
    message += imageFormatName(format);
    message += ". ";
    if (!caps.unavailableReason.empty()) {
      message += caps.unavailableReason;
    } else if (caps.canRead) {
      message += imageFormatName(format);
      message +=
          " can be read by this build but not written -- there is no writer for it here. ";
      if (format == ImageFormat::Psd) {
        message +=
            "PLAN.md Phase 4 step 2 asks for flattened PSD *read* only; PSD export is phase "
            "15, and the OpenImageIO linked here has no PSD writer at all (its "
            "output_format_list contains no 'psd' entry).";
      }
    }
    return message;
  }

  // --- Bit depth: honoured exactly, or refused by name (PRD B6) ----------
  //
  // "Can this format carry this depth" is now a runtime answer too, and one
  // that catches a genuinely dangerous case: OpenImageIO accepts a request
  // to write an 8-bit EXR, or a half TIFF, and silently writes half and
  // float respectively. io/Capabilities probes for exactly that substitution
  // and reports the depth unwritable, so the request is refused here rather
  // than succeeding at the wrong depth.
  if (!caps.canWriteDepth(bitDepth)) {
    int maxBits = 0;
    std::string writable;
    for (size_t i = 0; i < kExportBitDepthCount; ++i) {
      const ExportBitDepth d = static_cast<ExportBitDepth>(i);
      if (!caps.canWriteDepth(d)) continue;
      maxBits = std::max(maxBits, exportBitDepthBits(d));
      if (!writable.empty()) writable += ", ";
      writable += exportBitDepthName(d);
    }
    const std::string elsewhere = formatsThatCanWriteDepth(bitDepth);
    char buf[1024];
    std::snprintf(
        buf, sizeof(buf),
        "export refused: %s export was requested for %s, but %s writes at most %d bits per "
        "channel in this build (writable depths here: %s). Nothing was written -- silently "
        "writing %d bits and reporting success would be exactly the truncation PRD B6 "
        "forbids. Formats this build can write at %s: %s.",
        exportBitDepthName(bitDepth), imageFormatName(format), imageFormatName(format), maxBits,
        writable.empty() ? "none" : writable.c_str(), maxBits, exportBitDepthName(bitDepth),
        elsewhere.empty() ? "none -- no format available in this build writes at that depth"
                          : elsewhere.c_str());
    return buf;
  }

  // --- Alpha: a format with no alpha channel refuses a translucent image -
  if (!caps.hasAlpha && img != nullptr) {
    uint32_t x = 0, y = 0;
    float alpha = 1.0f;
    if (findFirstTranslucentPixel(*img, &x, &y, &alpha)) {
      char buf[512];
      std::snprintf(buf, sizeof(buf),
                    "export refused: %s has no alpha channel, but the flattened document is "
                    "not fully opaque (first partially transparent pixel at x=%u, y=%u, "
                    "alpha=%.4f). Exporting would silently discard transparency. Export PNG, "
                    "TGA or BMP, which carry alpha, or composite the document onto an opaque "
                    "background first.",
                    imageFormatName(format), x, y, static_cast<double>(alpha));
      return buf;
    }
  }

  return std::string();
}

ExportResult encodeLinearImage(const DecodedImage& img, const WorkingSpace& sourceSpace,
                               ImageFormat format, ExportTargetSpace targetSpace,
                               ExportBitDepth bitDepth) {
  if (!img.valid()) {
    return failure("export refused: there is nothing to encode (the image to export has zero "
                   "width or height, or a pixel buffer whose size does not match it).");
  }

  // Every "would this be refused" check, in one call, with both optional
  // arguments supplied -- so nothing about a real encode is checked more
  // loosely than what the Export As dialog previews, and nothing is checked
  // twice in two places that could disagree. See io/Export.hpp.
  const std::string refusal =
      exportRefusalReason(format, targetSpace, bitDepth, &sourceSpace, &img);
  if (!refusal.empty()) return failure(refusal);

  const FormatCapability& caps = formatCapability(format);

  // --- Encode + quantize -------------------------------------------------
  //
  // RGB through the target space's transfer function; alpha straight
  // through, never curved (alpha is opacity, not light). Then clamp and
  // quantize -- see io/Export.hpp on why the [0,1] clamp is an
  // export-policy decision made here rather than in color/Space, and why it
  // applies to integer depths only.
  const size_t texelCount = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
  ExportResult result;

  // --- The OpenImageIO-backed path (PLAN.md step 2) -----------------------
  //
  // Kept as a wholly separate branch from the stb path below rather than
  // merged into it. PRD I1's four formats therefore take byte-for-byte the
  // same code regardless of what OpenImageIO does -- there is no shared
  // "which encoder" branch that could regress them, which is what makes "I1
  // needs no optional dependency" checkable rather than asserted.
  if (caps.backend == FormatBackend::Oiio) {
    const int channels = caps.hasAlpha ? 4 : 3;
    const bool isFloat = exportBitDepthIsFloat(bitDepth);
    const bool associate = formatWantsAssociatedAlpha(format);
    std::vector<float> samples(texelCount * static_cast<size_t>(channels));
    for (size_t i = 0; i < texelCount; ++i) {
      const float* src = &img.pixels[i * 4];
      const float a = src[3];
      float* dst = &samples[i * static_cast<size_t>(channels)];
      for (int c = 0; c < 3; ++c) {
        // Association happens here, in linear light, *before* the transfer
        // function -- see io/Export.hpp's Alpha section.
        const float linear = associate ? src[c] * a : src[c];
        const float encoded = encodeRgbChannel(targetSpace, linear);
        // A float file keeps values outside [0,1]; an integer one has
        // nowhere to put them. Nothing is quantized here either way:
        // OpenImageIO converts float -> the file's sample type, and for the
        // integer types that conversion is the same round-to-nearest scale
        // io/ImageDecode's `sample / max` reads back.
        dst[c] = isFloat ? encoded : std::fmin(std::fmax(encoded, 0.0f), 1.0f);
      }
      if (channels >= 4) dst[3] = isFloat ? a : std::fmin(std::fmax(a, 0.0f), 1.0f);
    }
    std::string oiioError;
    if (!oiioEncodeToMemory(samples, img.width, img.height, channels, format, bitDepth,
                            &result.bytes, &oiioError)) {
      return failure(std::move(oiioError));
    }
    result.ok = true;
    return result;
  }

  if (bitDepth == ExportBitDepth::UInt16) {
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
    case ImageFormat::Png:
      wrote = stbi_write_png_to_func(&appendToVector, &result.bytes, w, h, 4, samples.data(),
                                     w * 4);
      break;
    case ImageFormat::Jpeg:
      // comp = 4 is accepted by stb's JPEG encoder (it strides by 4 and
      // reads offsets 0/1/2); the alpha check above already guaranteed
      // every alpha here is 1, so nothing is being discarded.
      wrote = stbi_write_jpg_to_func(&appendToVector, &result.bytes, w, h, 4, samples.data(),
                                     kJpegQuality);
      break;
    case ImageFormat::Tga:
      wrote = stbi_write_tga_to_func(&appendToVector, &result.bytes, w, h, 4, samples.data());
      break;
    case ImageFormat::Bmp:
      wrote = stbi_write_bmp_to_func(&appendToVector, &result.bytes, w, h, 4, samples.data());
      break;
    // Unreachable: every other ImageFormat is either OIIO-backed (handled
    // above) or not writable at all (refused by the capability check). Named
    // individually rather than with a `default:` so that adding a format to
    // the enum is a compiler error here until someone decides which encoder
    // writes it.
    case ImageFormat::Exr:
    case ImageFormat::Tiff:
    case ImageFormat::Hdr:
    case ImageFormat::Dpx:
    case ImageFormat::Psd:
    case ImageFormat::CameraRaw:
      return failure("export refused: no stb encoder exists for " +
                     std::string(imageFormatName(format)) +
                     " -- this is an internal dispatch error, not a rejected request.");
  }

  if (!wrote || result.bytes.empty()) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "export refused: the %s encoder (stb_image_write) failed on a %dx%d image -- "
                  "this is an internal encoder failure, not a rejected request.",
                  imageFormatName(format), w, h);
    return failure(buf);
  }
  result.ok = true;
  return result;
}

ExportResult exportDocument(const Document& doc, ImageFormat format,
                            ExportTargetSpace targetSpace, ExportBitDepth bitDepth) {
  std::vector<std::string> warnings;
  const DecodedImage flat = flattenDocumentToLinear(doc, &warnings);
  if (!flat.valid()) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "export refused: the document has no pixels to export (canvas is %dx%d).",
                  doc.width, doc.height);
    return failure(buf);
  }
  ExportResult result = encodeLinearImage(flat, doc.workingSpace, format, targetSpace, bitDepth);
  // Carried on a refusal too. A user whose EXR export was refused for a depth
  // reason still needs to know the composite it would have written is an
  // approximation, or they will fix the depth and be none the wiser.
  result.warnings = std::move(warnings);
  return result;
}

bool exportDocumentToFile(const Document& doc, const std::string& path, ImageFormat format,
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

std::vector<uint8_t> encodePng8Rgba(uint32_t width, uint32_t height, const uint8_t* rgba) {
  if (width == 0 || height == 0 || rgba == nullptr) return {};
  std::vector<uint8_t> out;
  if (stbi_write_png_to_func(&appendToVector, &out, static_cast<int>(width),
                             static_cast<int>(height), 4, rgba,
                             static_cast<int>(width) * 4) == 0)
    return {};
  return out;
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
