#include "io/OiioBackend.hpp"

#include <type_traits>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <filesystem>
#include <system_error>

#include <unistd.h>

#include "color/Space.hpp"
#include "core/Tile.hpp"

// The one and only translation unit in this project that may include an
// OpenImageIO header -- see io/OiioBackend.hpp's "no OpenImageIO header is
// included here" rule and why it is a hard rule rather than a preference.
// src/CMakeLists.txt compiles this file into the target only when
// NP_USE_OIIO is ON, so with the option OFF neither these includes nor the
// symbols they pull in exist anywhere in the binary.
#include <OpenImageIO/filesystem.h>
#include <OpenImageIO/imagecache.h>
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
  // Built with append rather than `std::string(...) + ext`: the fused
  // literal-plus-append form is a known GCC false-positive trigger for
  // `-Wstringop-overflow` under LTO's whole-program string-size analysis
  // (it mis-sizes the temporary's SSO buffer against the concatenated
  // result's length and reports a bogus overflow at this call's one caller,
  // probeWrite()). This form is behaviourally identical and does not
  // trip it.
  std::string name = "np-memory-image.";
  name += imageFormatExtension(format);
  return name;
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

namespace {

// A byte buffer spilled to a uniquely named file under the system temp
// directory, removed when this goes out of scope. Only oiioDecodeToLinear()'s
// proxy-refusal retry uses it; see the comment there.
class SpilledBytes {
 public:
  ~SpilledBytes() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove(path_, ec);
    }
  }
  bool write(const uint8_t* data, std::size_t size) {
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) return false;
    path_ = (dir / ("np-memory-image-" + std::to_string(static_cast<long>(::getpid())) + "-" +
                    std::to_string(counter.fetch_add(1)) + ".bin"))
                .string();
    std::FILE* f = std::fopen(path_.c_str(), "wb");
    if (f == nullptr) {
      path_.clear();
      return false;
    }
    const bool ok = std::fwrite(data, 1, size, f) == size;
    std::fclose(f);
    if (!ok) {
      std::filesystem::remove(path_, ec);
      path_.clear();
    }
    return ok;
  }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

}  // namespace

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

  // The proxy is ALSO passed as ImageInput::open()'s dedicated `ioproxy`
  // parameter, not only through the config attribute above. Measured against
  // this build (OpenImageIO 2.4.17, Ubuntu 24.04): with an extension-less
  // name and the proxy given only via the config attribute, open() reports
  // "Image \"np-memory-image\" does not exist" and never reaches the
  // content-sniffing fallback across plugins -- it treats the bare name as a
  // literal (missing) file on disk instead of realising the read is meant to
  // come from the proxy. Passing the same proxy through the `ioproxy`
  // parameter as well makes open() aware up front that there is no real file
  // to stat, and it falls through to trying every plugin against the
  // in-memory bytes, exactly as the comment on memoryFilename() above
  // describes. Both the parameter and the attribute are kept: whichever
  // OpenImageIO version is linked (this one, or the newer from-source build
  // this project also targets), one or both are honoured, and neither is
  // harmful for the other to also see.
  auto input = OIIO::ImageInput::open("np-memory-image", &config, &reader);
  // **A plugin that claims proxy support and then reads past the proxy's
  // end.** OpenImageIO 2.4.17's PSD reader answers `supports("ioproxy")`
  // and still fails a 52-byte flattened PSD through an IOMemReader with
  // "hit end of file in psd reader", while the same bytes open from disk
  // (measured; both open paths probed against Ubuntu 24.04's package). So a
  // proxy refusal is retried once through a temporary file, which is the
  // one read path every plugin of every version handles. Nothing about the
  // result differs -- the bytes are the same -- and a build whose plugins
  // all honour the proxy (the macOS from-source OpenImageIO) never reaches
  // this branch. The proxy's own error is what is reported if the retry
  // fails too, since that is the path that was meant to work.
  SpilledBytes spill;
  if (!input) {
    const std::string proxyError = OIIO::geterror();
    if (spill.write(fileData, fileSize)) input = OIIO::ImageInput::open(spill.path());
    if (!input) {
      if (errorOut) *errorOut = proxyError;
      return image;
    }
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

// --- Multi-part tiled EXR ------------------------------------------------

namespace {

// The four attribute types docs/document-format.md permits, and nothing
// else: "Use only OIIO-representable attribute types: `string`, `int`,
// `float`, and `UINT8[n]` for blobs. This avoids registering custom EXR
// attribute types, which OIIO would otherwise skip on read."
//
// Constructed from BASETYPE rather than spelled with OIIO's TypeString /
// TypeInt / TypeFloat aliases so the aggregate, vector-semantics and array
// length are all explicitly the scalar defaults -- a `float[2]` attribute
// must NOT compare equal to the scalar float case below, because carrying it
// through as a single float would silently lose its second element.
const OIIO::TypeDesc kAttrString(OIIO::TypeDesc::STRING);
const OIIO::TypeDesc kAttrInt(OIIO::TypeDesc::INT32);
const OIIO::TypeDesc kAttrFloat(OIIO::TypeDesc::FLOAT);

// Converts one OpenImageIO header attribute into an NpaintAttribute.
// Returns false for any type outside the four above -- the caller turns that
// into a warning naming the attribute and its type, rather than dropping it
// without a word.
bool attributeFromOiio(const OIIO::ParamValue& p, NpaintAttribute* out) {
  const OIIO::TypeDesc t = p.type();
  out->name = p.name().string();
  if (t == kAttrString) {
    out->type = NpaintAttribute::Type::String;
    out->stringValue = p.get_string();
    return true;
  }
  if (t == kAttrInt) {
    out->type = NpaintAttribute::Type::Int;
    out->intValue = static_cast<int32_t>(p.get_int());
    return true;
  }
  if (t == kAttrFloat) {
    out->type = NpaintAttribute::Type::Float;
    out->floatValue = p.get_float();
    return true;
  }
  if (t.basetype == OIIO::TypeDesc::UINT8 && t.aggregate == OIIO::TypeDesc::SCALAR &&
      t.arraylen > 0) {
    out->type = NpaintAttribute::Type::Blob;
    const auto* bytes = static_cast<const uint8_t*>(p.data());
    out->blobValue.assign(bytes, bytes + static_cast<size_t>(t.arraylen));
    return true;
  }
  return false;
}

void attributeToOiio(const NpaintAttribute& a, OIIO::ImageSpec* spec) {
  switch (a.type) {
    case NpaintAttribute::Type::String: spec->attribute(a.name, a.stringValue); return;
    case NpaintAttribute::Type::Int:
      spec->attribute(a.name, static_cast<int>(a.intValue));
      return;
    case NpaintAttribute::Type::Float: spec->attribute(a.name, a.floatValue); return;
    case NpaintAttribute::Type::Blob:
      spec->attribute(a.name,
                      OIIO::TypeDesc(OIIO::TypeDesc::UINT8, static_cast<int>(a.blobValue.size())),
                      a.blobValue.data());
      return;
  }
}

}  // namespace

bool oiioWriteMultiPartExr(const OiioExrWriteRequest& request, std::string* errorOut) {
  auto fail = [&](std::string message) {
    if (errorOut) *errorOut = std::move(message);
    // Never leave a partial document behind. A failed save that also
    // destroyed the previous file would be the worst outcome of the two.
    std::remove(request.path.c_str());
    return false;
  };
  if (request.parts.empty()) return fail("internal: multi-part EXR write with no parts.");

  // Measured constraint: this OpenImageIO cannot write a multi-part EXR
  // whose parts disagree about being tiled -- it opens the file and then
  // throws "Can't build a TiledOutputFile from a type-mismatched part" on
  // the first mismatch. io/NpaintFile refuses this case up front with a
  // message that names the document's own part; this is the belt-and-braces
  // check at the point the file is produced, so a caller that bypassed it
  // gets a sentence rather than an OpenEXR exception string.
  const bool firstIsTiled = request.parts[0].tileWidth > 0 && request.parts[0].tileHeight > 0;
  for (const NpaintRawPart& part : request.parts) {
    const bool tiled = part.tileWidth > 0 && part.tileHeight > 0;
    if (tiled != firstIsTiled) {
      return fail(std::string("save failed: part '") + part.name + "' is " +
                  (tiled ? "tiled" : "scanline") + " while part '" + request.parts[0].name +
                  "' is " + (firstIsTiled ? "tiled" : "scanline") +
                  ", and OpenEXR multi-part output through this OpenImageIO requires every "
                  "part to agree.");
    }
  }

  std::vector<OIIO::ImageSpec> specs;
  specs.reserve(request.parts.size());
  for (const NpaintRawPart& part : request.parts) {
    const OIIO::TypeDesc type(part.sampleTypeName);
    if (type == OIIO::TypeDesc::UNKNOWN) {
      return fail("save failed: part '" + part.name + "' names sample type '" +
                  part.sampleTypeName + "', which OpenImageIO does not recognise.");
    }
    const size_t channels = part.channelNames.size();
    const size_t expected = static_cast<size_t>(part.width) * static_cast<size_t>(part.height) *
                            channels * type.size();
    if (part.rawPixels.size() != expected) {
      return fail("internal: part '" + part.name + "' carries " +
                  std::to_string(part.rawPixels.size()) + " pixel bytes but its " +
                  std::to_string(part.width) + "x" + std::to_string(part.height) + " " +
                  std::to_string(channels) + "-channel " + part.sampleTypeName +
                  " data window needs " + std::to_string(expected) + ".");
    }

    OIIO::ImageSpec spec(part.width, part.height, static_cast<int>(channels), type);
    spec.x = part.x;
    spec.y = part.y;
    // The display window is one property of the whole file, not of a part --
    // docs/document-format.md §2: "Some attributes must match across all
    // parts -- displayWindow, pixelAspectRatio, chromaticities."
    spec.full_x = 0;
    spec.full_y = 0;
    spec.full_width = request.displayWidth;
    spec.full_height = request.displayHeight;
    spec.tile_width = part.tileWidth;
    spec.tile_height = part.tileHeight;
    spec.channelnames.assign(part.channelNames.begin(), part.channelNames.end());
    spec.alpha_channel = -1;
    for (size_t c = 0; c < channels; ++c) {
      if (part.channelNames[c] == "A") spec.alpha_channel = static_cast<int>(c);
    }
    // EXR requires a unique `name` on every part of a multi-part file;
    // OpenImageIO takes it from this attribute.
    spec.attribute("name", part.name);
    spec.attribute("compression", request.compression);
    if (request.hasChromaticities) {
      spec.attribute("chromaticities", OIIO::TypeDesc(OIIO::TypeDesc::FLOAT, 8),
                     request.chromaticities.data());
    }
    for (const NpaintAttribute& a : part.attributes) attributeToOiio(a, &spec);
    specs.push_back(std::move(spec));
  }

  // The writer is selected by *format name*, not by the path's extension.
  //
  // This is load-bearing rather than stylistic, and it was measured: this
  // OpenImageIO's `ImageOutput::create("/tmp/x.npaint")` returns null with
  // "could not find a format writer ... Is it a file format that
  // OpenImageIO doesn't know about?", because `.npaint` is (of course) not
  // in its extension table. `ImageOutput::create("openexr")` takes the
  // string as a format name instead, and the subsequent `open()` uses the
  // real path.
  //
  // Doing it unconditionally -- rather than trying the path first and
  // falling back -- is what makes PRD I8 ("`.npaint` and `.exr` are the same
  // container, so pipeline handoff is a rename") true by construction: the
  // same plugin, with the same settings, writes both, so the two files are
  // byte-identical and a rename really is all it is.
  auto out = OIIO::ImageOutput::create("openexr");
  if (!out) {
    return fail("save failed: this OpenImageIO build has no OpenEXR writer (" +
                OIIO::geterror() +
                "). The native document format is OpenEXR (PRD I4), so there is nothing to "
                "fall back to.");
  }
  // Only "multiimage" is checked. This build's OpenEXR writer reports
  // `supports("appendsubimage") == 0` -- measured -- yet the sequence below
  // (declare all N specs up front, then AppendSubimage for parts 1..N-1)
  // works and is the documented multi-part idiom. "appendsubimage" means
  // "subimages can be appended one at a time without declaring the count in
  // advance", which is a different capability and not one this writer needs.
  // Gating on it would refuse a file format that demonstrably works.
  if (!out->supports("multiimage")) {
    return fail("save failed: this OpenImageIO's OpenEXR writer does not support multi-part "
                "output, which the native `.npaint` format requires (one part per layer -- "
                "PRD I4).");
  }
  if (!out->open(request.path, static_cast<int>(specs.size()), specs.data())) {
    return fail("save failed: OpenImageIO refused to open '" + request.path + "' for " +
                std::to_string(specs.size()) + "-part output (" + out->geterror() + ").");
  }
  for (size_t i = 0; i < specs.size(); ++i) {
    if (i > 0 && !out->open(request.path, specs[i], OIIO::ImageOutput::AppendSubimage)) {
      return fail("save failed: OpenImageIO refused to append part '" + request.parts[i].name +
                  "' (" + out->geterror() + ").");
    }
    if (!out->write_image(OIIO::TypeDesc(request.parts[i].sampleTypeName),
                          request.parts[i].rawPixels.data())) {
      const std::string err = out->geterror();
      out->close();
      return fail("save failed: OpenImageIO failed writing part '" + request.parts[i].name +
                  "' (" + err + ").");
    }
  }
  if (!out->close()) {
    return fail("save failed: OpenImageIO failed to finish '" + request.path + "' (" +
                out->geterror() + ").");
  }
  return true;
}

OiioExrReadResult oiioReadMultiPartExr(const std::string& path) {
  OiioExrReadResult result;
  auto in = OIIO::ImageInput::open(path);
  if (!in) {
    result.error = "load failed: OpenImageIO could not open '" + path + "' (" +
                   OIIO::geterror() + ").";
    return result;
  }

  for (int sub = 0;; ++sub) {
    if (!in->seek_subimage(sub, 0)) break;
    const OIIO::ImageSpec& spec = in->spec();
    if (spec.width <= 0 || spec.height <= 0 || spec.nchannels <= 0) {
      result.error = "load failed: part " + std::to_string(sub) + " of '" + path +
                     "' reports no pixels.";
      in->close();
      return result;
    }
    if (sub == 0) {
      result.displayWidth = spec.full_width;
      result.displayHeight = spec.full_height;
      float chroma[8] = {};
      if (spec.getattribute("chromaticities", OIIO::TypeDesc(OIIO::TypeDesc::FLOAT, 8), chroma)) {
        result.hasChromaticities = true;
        std::copy(chroma, chroma + 8, result.chromaticities.begin());
      }
    }

    NpaintRawPart part;
    part.name = spec.get_string_attribute("name");
    part.x = spec.x;
    part.y = spec.y;
    part.width = spec.width;
    part.height = spec.height;
    part.tileWidth = spec.tile_width;
    part.tileHeight = spec.tile_height;
    part.channelNames.assign(spec.channelnames.begin(), spec.channelnames.end());
    part.sampleTypeName = spec.format.c_str();

    for (const OIIO::ParamValue& p : spec.extra_attribs) {
      const std::string name = p.name().string();
      // Only `np:*` crosses this boundary. Everything else in an EXR header
      // is the container's own (`compression`, `chromaticities`, the
      // windows) or OpenImageIO's bookkeeping (`oiio:*`), and io/NpaintFile
      // regenerates all of it -- see NpaintCarry::documentAttributes on why
      // writing those back verbatim would fight the writer rather than
      // preserve the document.
      if (name.rfind("np:", 0) != 0) continue;
      NpaintAttribute attr;
      if (attributeFromOiio(p, &attr)) {
        part.attributes.push_back(std::move(attr));
      } else {
        result.warnings.push_back(
            "attribute '" + name + "' on part '" + part.name + "' has EXR type '" +
            std::string(p.type().c_str()) +
            "', which is not one of the four the native format permits (string, int, float, "
            "uint8[n]); it could not be carried through and will be absent from the next save.");
      }
    }

    const size_t bytes = static_cast<size_t>(spec.width) * static_cast<size_t>(spec.height) *
                         static_cast<size_t>(spec.nchannels) * spec.format.size();
    part.rawPixels.resize(bytes);
    // Read in the part's *own* sample type. A float intermediate would be
    // exact for half but not for uint32, and this buffer's whole purpose is
    // to be written back unchanged.
    if (!in->read_image(sub, 0, 0, spec.nchannels, spec.format, part.rawPixels.data())) {
      result.error = "load failed: OpenImageIO failed reading part '" + part.name + "' of '" +
                     path + "' (" + in->geterror() + ").";
      in->close();
      return result;
    }
    result.parts.push_back(std::move(part));
  }
  in->close();

  if (result.parts.empty()) {
    result.error = "load failed: '" + path + "' contains no image parts.";
    return result;
  }
  result.ok = true;
  return result;
}

// --- ImageCache -----------------------------------------------------------

namespace {

// The one cache, created on first use and never before.
//
// A function-local static rather than a namespace-scope object on purpose: a
// namespace-scope `std::shared_ptr<ImageCache>` would be constructed during
// static initialisation, i.e. before `main()`, which would put a cost on
// every run of the binary including the ones that never open a file. That is
// exactly the mistake PLAN.md Phase 4 step 6's correction block records
// (OIIO's own init is already lazy; the cost that is real is dyld's) and
// there is no reason to add a new one on top of it.
//
// `create(false)` gives a private cache rather than OpenImageIO's shared
// singleton. The shared one is global to the process across every library
// that links OIIO, so its budget -- the number this whole step turns on --
// could be changed by code this project does not own.
// A template rather than `if constexpr` in the function above: outside a
// template both branches of an `if constexpr` must still compile, and the
// raw-pointer branch does not against a shared_ptr-returning `create()`.
template <class Created>
std::shared_ptr<OIIO::ImageCache> adoptImageCache(Created created) {
  if constexpr (std::is_pointer_v<Created>) {
    return std::shared_ptr<OIIO::ImageCache>(created,
                                             [](OIIO::ImageCache* c) { OIIO::ImageCache::destroy(c); });
  } else {
    return created;
  }
}

OIIO::ImageCache* tileCache(bool createIfAbsent) {
  static std::shared_ptr<OIIO::ImageCache> cache;
  if (!cache && createIfAbsent) {
    // OpenImageIO 2.5+ returns a shared_ptr here; 2.4 (Ubuntu 24.04's
    // package) returns a raw pointer that must go back through
    // ImageCache::destroy(). Both are accepted so the version is a property
    // of the machine, not of this file.
    cache = adoptImageCache(OIIO::ImageCache::create(false));
    // autotile off: an untiled source is not cached tile-wise at all, it is
    // cached whole. io/TileResidency refuses untiled sources for that reason
    // (and for the measured per-tile cost of the alternative), so turning
    // autotile on here would only make the refusal look unnecessary while
    // changing nothing about the numbers behind it.
    cache->attribute("autotile", 0);
    // automip off: nothing this build writes carries a mip pyramid, and the
    // only pyramid in this codebase is ui/NaturalPaintUI's display-side one.
    // Synthesising levels here would spend budget on data no caller asks for.
    cache->attribute("automip", 0);
    // Errors belong to the caller that asked for the fetch, not to stderr.
    cache->attribute("failure_retries", 0);
  }
  return cache.get();
}

void applyBudget(OIIO::ImageCache* cache, std::size_t budgetBytes) {
  const float mb = static_cast<float>(static_cast<double>(budgetBytes) / (1024.0 * 1024.0));
  cache->attribute("max_memory_MB", mb);
}

// The ImageCache's own `imagespec()` does NOT report an untiled source's
// *native* tile shape -- measured against this build (OpenImageIO 2.4.17):
// with `autotile` left at 0 (this cache's own setting, above), the cache
// spec's tile_width/tile_height come back equal to the full image's
// width/height for a genuinely scanline-stored file, never 0x0. That is
// documented ImageCache behaviour ("an untiled image will be read and cached
// as one single tile of the full image resolution"), not a bug in the
// library, but io/TileResidency's refusal test relies on 0x0 meaning
// "scanline" -- a real 128x128-tiled source only reports 128x128, which
// looks identical to a 128x128 *scanline* image reported through the cache.
// So tiled-ness is asked of a plain `ImageInput` opened directly on the
// file, which reports the format's actual on-disk layout (0x0 for scanline,
// the real tile size otherwise) regardless of any cache setting -- this is
// metadata only, a header read, not a pixel read, and happens once per
// `oiioTileCacheOpen()` call rather than per tile fetch.
bool nativeTileShape(const std::string& path, int32_t subimage, int32_t miplevel,
                     int32_t* tileWidth, int32_t* tileHeight) {
  auto input = OIIO::ImageInput::open(path);
  if (!input) return false;
  if ((subimage != 0 || miplevel != 0) && !input->seek_subimage(subimage, miplevel)) {
    input->close();
    return false;
  }
  *tileWidth = input->spec().tile_width;
  *tileHeight = input->spec().tile_height;
  input->close();
  return true;
}

}  // namespace

OiioTileCacheOpen oiioTileCacheOpen(const std::string& path, int32_t subimage,
                                    int32_t miplevel, std::size_t budgetBytes) {
  OiioTileCacheOpen out;
  OIIO::ImageCache* cache = tileCache(true);
  applyBudget(cache, budgetBytes);

  const OIIO::ustring file(path);
  const OIIO::ImageSpec* spec = cache->imagespec(file, subimage, miplevel);
  if (!spec) {
    out.error = cache->geterror();
    if (out.error.empty()) {
      out.error = "OpenImageIO's ImageCache could not describe subimage " +
                  std::to_string(subimage) + " miplevel " + std::to_string(miplevel) +
                  " of '" + path + "'.";
    }
    return out;
  }

  int subimages = 0;
  if (!cache->get_image_info(file, subimage, miplevel, OIIO::ustring("subimages"),
                             OIIO::TypeDesc::INT, &subimages)) {
    (void)cache->geterror();
    subimages = 0;
  }

  out.ok = true;
  out.dataX = spec->x;
  out.dataY = spec->y;
  out.dataWidth = spec->width;
  out.dataHeight = spec->height;
  // NOT spec->tile_width/tile_height -- see nativeTileShape()'s comment
  // above. The cache's own spec reports its *effective* tile shape, which
  // for a scanline source (with autotile off, as this cache is configured)
  // equals the full image resolution rather than 0x0, indistinguishable from
  // a genuinely tiled source whose tiles happen to be that size. Falling
  // back to the cache's values on a native-open failure is deliberately
  // conservative: it means "assume tiled" rather than "assume scanline",
  // which only makes this function *more* willing to serve a cached
  // residency, never less -- the refusal path this feeds is a safety check,
  // not a correctness one.
  if (!nativeTileShape(path, subimage, miplevel, &out.tileWidth, &out.tileHeight)) {
    out.tileWidth = spec->tile_width;
    out.tileHeight = spec->tile_height;
  }
  out.channels = spec->nchannels;
  out.sampleTypeName = spec->format.c_str();
  out.subimageCount = subimages;
  return out;
}

bool oiioTileCacheFetchHalfRgba(const std::string& path, int32_t subimage, int32_t miplevel,
                                int32_t x, int32_t y, uint16_t* out, std::string* errorOut) {
  OIIO::ImageCache* cache = tileCache(false);
  if (!cache) {
    if (errorOut) {
      *errorOut =
          "tile fetch failed: no ImageCache exists -- oiioTileCacheOpen() must succeed "
          "before a tile of '" + path + "' can be fetched.";
    }
    return false;
  }
  if (!out) {
    if (errorOut) *errorOut = "tile fetch failed: null destination buffer.";
    return false;
  }

  const OIIO::ustring file(path);
  const bool got = cache->get_pixels(file, subimage, miplevel, x, x + kTileSize, y,
                                     y + kTileSize, 0, 1, 0, Tile::kChannels,
                                     OIIO::TypeDesc::HALF, out);
  if (!got) {
    if (errorOut) {
      std::string message = cache->geterror();
      if (message.empty()) message = "OpenImageIO reported no reason.";
      *errorOut = "tile fetch failed at (" + std::to_string(x) + "," + std::to_string(y) +
                  ") in subimage " + std::to_string(subimage) + " of '" + path + "': " +
                  message;
    }
    return false;
  }
  return true;
}

bool oiioTileCacheStatistics(OiioTileCacheStats* out) {
  OIIO::ImageCache* cache = tileCache(false);
  if (!cache || !out) return false;

  auto readInt64 = [cache](const char* name) {
    int64_t value = 0;
    if (!cache->getattribute(name, OIIO::TypeDesc::INT64, &value)) (void)cache->geterror();
    return value;
  };
  auto readInt = [cache](const char* name) {
    int value = 0;
    if (!cache->getattribute(name, OIIO::TypeDesc::INT, &value)) (void)cache->geterror();
    return value;
  };

  out->memoryUsedBytes = readInt64("stat:cache_memory_used");
  out->imageSizeBytes = readInt64("stat:image_size");
  out->tilesCreated = readInt("stat:tiles_created");
  out->tilesCurrent = readInt("stat:tiles_current");
  out->tilesPeak = readInt("stat:tiles_peak");

  float budgetMb = 0.0f;
  if (!cache->getattribute("max_memory_MB", budgetMb)) (void)cache->geterror();
  out->budgetBytes = static_cast<int64_t>(static_cast<double>(budgetMb) * 1024.0 * 1024.0);
  return true;
}

void oiioTileCacheInvalidate(const std::string& path) {
  OIIO::ImageCache* cache = tileCache(false);
  if (!cache) return;
  cache->invalidate(OIIO::ustring(path), true);
}

bool oiioTileCacheSetBudget(std::size_t budgetBytes) {
  OIIO::ImageCache* cache = tileCache(false);
  if (!cache) return false;
  applyBudget(cache, budgetBytes);
  return true;
}

}  // namespace np
