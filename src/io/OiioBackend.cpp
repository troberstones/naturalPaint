#include "io/OiioBackend.hpp"

#include <algorithm>
#include <cstring>

#include "color/Space.hpp"

// The one and only translation unit in this project that may include an
// OpenImageIO header -- see io/OiioBackend.hpp's "no OpenImageIO header is
// included here" rule and why it is a hard rule rather than a preference.
// src/CMakeLists.txt compiles this file into the target only when
// NP_USE_OIIO is ON, so with the option OFF neither these includes nor the
// symbols they pull in exist anywhere in the binary.
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imageio.h>
#include <OpenImageIO/typedesc.h>

namespace np {
namespace {

// The filename handed to OpenImageIO. For writing it is load-bearing: the
// extension is what selects the output plugin (there is no
// "create by format name" entry point that also takes an IOProxy). For
// reading it is not -- verified against this build, an in-memory EXR opens
// correctly through a proxy even when the name is "blob.dat", because
// OpenImageIO falls back to content sniffing. Nothing is ever created on
// disk under either name; every byte goes through a
// Filesystem::IOVecOutput / IOMemReader proxy.
std::string memoryFilename(ImageFormat format) {
  return std::string("np-memory-image.") + imageFormatExtension(format);
}

OIIO::TypeDesc typeDescFor(ExportBitDepth depth) {
  switch (depth) {
    case ExportBitDepth::UInt8: return OIIO::TypeDesc::UINT8;
    case ExportBitDepth::UInt16: return OIIO::TypeDesc::UINT16;
    case ExportBitDepth::Half: return OIIO::TypeDesc::HALF;
    case ExportBitDepth::Float32: return OIIO::TypeDesc::FLOAT;
  }
  return OIIO::TypeDesc::UINT8;
}

// OpenImageIO's list attributes are comma-separated format names. Matching
// on whole entries rather than substrings matters: "raw" is a substring of
// nothing here today, but "png" is a substring of nothing while "pnm" and
// "png" are both present and a sloppy find() would be a bug waiting for the
// first format whose name contains another's.
bool listContainsFormat(const std::string& list, const char* name) {
  const std::string needle(name);
  size_t pos = 0;
  while (pos <= list.size()) {
    const size_t comma = list.find(',', pos);
    const size_t end = (comma == std::string::npos) ? list.size() : comma;
    if (list.compare(pos, end - pos, needle) == 0) return true;
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  return false;
}

std::string attributeString(const char* name) {
  std::string value;
  OIIO::getattribute(name, value);
  return value;
}

// Opens a real 1x1 writer for `format` at `type`, writes its single texel,
// and closes it -- entirely into a discarded in-memory buffer.
//
// Two things are being discovered, neither of which OpenImageIO exposes as a
// queryable attribute:
//
//  * whether the format accepts `channels` channels at all (this is how HDR
//    reports that it has no alpha: its writer answers "hdr does not support
//    4-channel images" at open), and
//  * whether it will write the sample type that was asked for, or silently
//    substitute another. `out->spec().format` after a successful open is
//    OpenImageIO's own statement of what it is about to write, and against
//    this build it differs from the request for EXR+UINT8 (-> half),
//    TIFF+HALF (-> float) and DPX+HALF (-> float). Treating "open
//    succeeded" as "the depth is supported" would therefore hand PRD B6 a
//    silently truncated file and report success.
//
// The pixel is actually written rather than opening and closing an empty
// file, because some plugins complain about a file with no image data (this
// build's libpng prints "No IDATs written into file" to stderr for exactly
// that) -- a probe that pollutes stderr would be its own small bug.
bool probeWrite(ImageFormat format, int channels, OIIO::TypeDesc type, bool* exactTypeOut) {
  if (exactTypeOut) *exactTypeOut = false;
  const std::string name = memoryFilename(format);
  std::vector<unsigned char> scratch;
  OIIO::Filesystem::IOVecOutput proxy(scratch);
  auto out = OIIO::ImageOutput::create(name, &proxy);
  if (!out) {
    (void)OIIO::geterror();  // drain, so a later real error can't inherit it
    return false;
  }
  const OIIO::ImageSpec spec(1, 1, channels, type);
  if (!out->open(name, spec)) {
    (void)out->geterror();
    return false;
  }
  if (exactTypeOut) *exactTypeOut = (out->spec().format == type);
  const std::vector<float> texel(static_cast<size_t>(channels), 0.5f);
  const bool wrote = out->write_image(OIIO::TypeDesc::FLOAT, texel.data());
  if (!wrote) (void)out->geterror();
  if (!out->close()) (void)out->geterror();
  return wrote;
}

}  // namespace

bool oiioFormatPresent(const char* name) {
  if (!name || !*name) return false;
  return listContainsFormat(attributeString("format_list"), name);
}

std::string oiioVersionString() { return attributeString("version"); }

std::string oiioFormatList() { return attributeString("format_list"); }

OiioFormatProbe oiioProbeFormat(ImageFormat format) {
  OiioFormatProbe probe;
  const char* oiioName = imageFormatOiioName(format);
  if (!oiioName || !*oiioName) return probe;

  // Read and write support are separate attributes in OpenImageIO, and the
  // difference is real rather than theoretical for this step: against this
  // build "psd" appears in input_format_list and NOT in output_format_list,
  // which is exactly PLAN.md step 2's "flattened PSD" being a read-only
  // capability and PSD *export* still belonging to phase 15. Nothing here
  // has to encode that rule -- asking the library produces it.
  probe.canRead = listContainsFormat(attributeString("input_format_list"), oiioName);
  probe.canWrite = listContainsFormat(attributeString("output_format_list"), oiioName);
  if (!probe.canWrite) return probe;

  // FLOAT is the probe type for the channel-count question because every
  // OIIO-backed format in scope here accepts it (an integer-only format
  // would coerce it, which is a depth answer, not a channel answer, and is
  // asked separately below).
  probe.hasAlpha = probeWrite(format, 4, OIIO::TypeDesc::FLOAT, nullptr);
  const int channels = probe.hasAlpha ? 4 : 3;

  for (size_t i = 0; i < kExportBitDepthCount; ++i) {
    const ExportBitDepth depth = static_cast<ExportBitDepth>(i);
    bool exact = false;
    const bool ok = probeWrite(format, channels, typeDescFor(depth), &exact);
    probe.writableDepths[i] = ok && exact;
  }
  return probe;
}

bool oiioEncodeToMemory(const std::vector<float>& samples, uint32_t width, uint32_t height,
                        int channels, ImageFormat format, ExportBitDepth depth,
                        std::vector<uint8_t>* out, std::string* errorOut) {
  auto fail = [&](std::string message) {
    if (errorOut) *errorOut = std::move(message);
    return false;
  };
  if (!out) return fail("internal: oiioEncodeToMemory called with no output buffer.");
  if (samples.size() != static_cast<size_t>(width) * height * static_cast<size_t>(channels))
    return fail("internal: oiioEncodeToMemory sample count does not match width*height*channels.");

  const std::string name = memoryFilename(format);
  const OIIO::TypeDesc type = typeDescFor(depth);

  // The bytes land here first and are moved into *out only on full success,
  // so a refused or failed write can never leave partial bytes with a
  // caller -- the same "nothing is written unless the encode succeeded in
  // full" property io/Export.hpp promises for exportDocumentToFile().
  std::vector<unsigned char> bytes;
  {
    OIIO::Filesystem::IOVecOutput proxy(bytes);
    auto writer = OIIO::ImageOutput::create(name, &proxy);
    if (!writer) {
      const std::string err = OIIO::geterror();
      return fail("export failed: OpenImageIO has no writer for " +
                  std::string(imageFormatName(format)) + " in this build (" + err + ").");
    }
    OIIO::ImageSpec spec(static_cast<int>(width), static_cast<int>(height), channels, type);
    if (!writer->open(name, spec)) {
      return fail("export failed: OpenImageIO's " + std::string(imageFormatName(format)) +
                  " writer refused to open a " + std::to_string(width) + "x" +
                  std::to_string(height) + " " + std::to_string(channels) +
                  "-channel image (" + writer->geterror() + ").");
    }
    // Belt and braces against PRD B6. io/Capabilities' probe should already
    // have refused this combination before we got here, but this is the
    // point where the real file is produced, and a silent substitution here
    // is precisely the failure B6 names. Checked against OpenImageIO's own
    // statement of what it is about to write, not against a table.
    if (writer->spec().format != type) {
      const std::string actual(writer->spec().format.c_str());
      writer->close();
      (void)writer->geterror();
      return fail("export refused: OpenImageIO's " + std::string(imageFormatName(format)) +
                  " writer would substitute sample type '" + actual + "' for the requested " +
                  exportBitDepthName(depth) +
                  ". Nothing was written -- writing a different depth than the one asked for "
                  "and reporting success is exactly the truncation PRD B6 forbids.");
    }
    if (!writer->write_image(OIIO::TypeDesc::FLOAT, samples.data())) {
      const std::string err = writer->geterror();
      writer->close();
      (void)writer->geterror();
      return fail("export failed: OpenImageIO's " + std::string(imageFormatName(format)) +
                  " writer failed while writing pixels (" + err + ").");
    }
    if (!writer->close()) {
      return fail("export failed: OpenImageIO's " + std::string(imageFormatName(format)) +
                  " writer failed to finish the file (" + writer->geterror() + ").");
    }
  }  // proxy destroyed here, so `bytes` is complete and no longer aliased

  if (bytes.empty()) {
    return fail("export failed: OpenImageIO's " + std::string(imageFormatName(format)) +
                " writer reported success but produced no bytes.");
  }
  *out = std::vector<uint8_t>(bytes.begin(), bytes.end());
  return true;
}

DecodedImage oiioDecodeToLinear(const uint8_t* fileData, std::size_t fileSize,
                                std::string* errorOut) {
  DecodedImage image;
  if (!fileData || fileSize == 0) {
    if (errorOut) *errorOut = "empty input";
    return image;
  }

  OIIO::Filesystem::IOMemReader reader(fileData, fileSize);
  OIIO::Filesystem::IOProxy* proxy = &reader;
  OIIO::ImageSpec config;
  // The documented way to hand OpenImageIO an in-memory buffer: the config
  // attribute holds a *pointer to the proxy pointer*, not the proxy object.
  // Getting that wrong is a straight segfault rather than an error return.
  config.attribute("oiio:ioproxy", OIIO::TypeDesc::PTR, &proxy);

  auto input = OIIO::ImageInput::open("np-memory-image", &config);
  if (!input) {
    if (errorOut) *errorOut = OIIO::geterror();
    return image;
  }

  const OIIO::ImageSpec& spec = input->spec();
  if (spec.width <= 0 || spec.height <= 0 || spec.nchannels <= 0) {
    if (errorOut) *errorOut = "OpenImageIO reported an image with no pixels";
    input->close();
    return image;
  }

  const std::string formatName = input->format_name();
  // See io/OiioBackend.hpp's conventions section: linearity follows the
  // file's sample type, and only EXR carries associated (premultiplied)
  // alpha in this codebase's conventions.
  const OIIO::TypeDesc::BASETYPE base =
      static_cast<OIIO::TypeDesc::BASETYPE>(spec.format.basetype);
  const bool sourceIsFloat = base == OIIO::TypeDesc::HALF || base == OIIO::TypeDesc::FLOAT ||
                             base == OIIO::TypeDesc::DOUBLE;
  const bool sourceIsAssociated = (formatName == "openexr");

  const int channels = std::min(spec.nchannels, 4);
  const size_t texelCount = static_cast<size_t>(spec.width) * static_cast<size_t>(spec.height);
  std::vector<float> raw(texelCount * static_cast<size_t>(channels));
  if (!input->read_image(0, 0, 0, channels, OIIO::TypeDesc::FLOAT, raw.data())) {
    if (errorOut) *errorOut = input->geterror();
    input->close();
    return image;
  }
  input->close();

  image.width = static_cast<uint32_t>(spec.width);
  image.height = static_cast<uint32_t>(spec.height);
  image.pixels.resize(texelCount * 4);
  for (size_t i = 0; i < texelCount; ++i) {
    const float* src = &raw[i * static_cast<size_t>(channels)];
    // Channel mapping, following stb_image's own "request 4, synthesize
    // what isn't there" behaviour that io/ImageDecode.cpp already relies on:
    // 1 channel replicates to RGB at full opacity, 2 channels are
    // grey+alpha, 3+ are RGB(+A).
    float r = src[0];
    float g = channels >= 3 ? src[1] : src[0];
    float b = channels >= 3 ? src[2] : src[0];
    float a = channels == 2 ? src[1] : (channels >= 4 ? src[3] : 1.0f);

    if (!sourceIsFloat) {
      // Alpha is opacity, not light -- never decoded through a transfer
      // function (io/ImageDecode.hpp, core/Probe.hpp).
      r = srgbDecode(r);
      g = srgbDecode(g);
      b = srgbDecode(b);
    }
    if (sourceIsAssociated) {
      // Un-associate in linear light, with the same a <= 0 -> {0,0,0,0}
      // guard core/Probe.cpp's and io/Export.cpp's unpremultiply() use: an
      // alpha-0 texel must contribute "no colour", not a divide by zero.
      if (a <= 0.0f) {
        r = g = b = 0.0f;
      } else {
        r /= a;
        g /= a;
        b /= a;
      }
    }
    float* dst = &image.pixels[i * 4];
    dst[0] = r;
    dst[1] = g;
    dst[2] = b;
    dst[3] = a;
  }
  return image;
}

}  // namespace np
