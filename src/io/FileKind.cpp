#include "io/FileKind.hpp"

#include <algorithm>
#include <cstring>
#include <optional>

// io/FileKind -- implementation. Every decision is argued in FileKind.hpp;
// this file holds the byte matching, and comments only where the bytes are not
// self-explanatory.

namespace np {

namespace {

// True when `data[0..n)` begins with the `n`-byte literal `sig`, bounds-checked
// against `size` first. Every signature test below goes through this rather
// than doing its own arithmetic, so there is one place where the length check
// can be wrong and it is three lines long.
bool startsWith(const uint8_t* data, size_t size, const void* sig, size_t n) {
  return size >= n && std::memcmp(data, sig, n) == 0;
}

// OpenEXR's magic number: 20000630 stored little-endian, so 76 2F 31 01 on
// disk. Followed by a four-byte version field (one version byte, three flag
// bytes) that this module does not need to interpret -- the multi-part flag
// changes what comes *after* the first part's header, not where that header
// starts, and the walk below stops at the end of part 0 either way.
constexpr uint8_t kExrMagic[4] = {0x76, 0x2F, 0x31, 0x01};

// The attribute name `saveNpaint()` stamps on part 0 (io/NpaintFile.cpp's
// `kAttrVersion`). Written out here rather than included from there because
// io/NpaintFile.cpp keeps it in an anonymous namespace and because this module
// deliberately does not depend on io/NpaintFile at all -- the sniff has to work
// in a build where NpaintFile refuses everything (NP_USE_OIIO=OFF), so that a
// `.npaint` in that build is refused *as a document this build cannot read*
// rather than mistaken for a picture and half-decoded into one flat layer.
//
// If that constant were ever renamed, the failure is not silent: a `.npaint`
// would start sniffing as a plain OpenEXR, and
// app/selftest/OpenAnyFile.cpp section B saves a real `.npaint` through
// `saveNpaint()` and asserts this function calls it a document. The two are
// tied together by that assertion rather than by a shared header.
constexpr char kNpaintMarker[] = "np:version";
constexpr size_t kNpaintMarkerLen = sizeof(kNpaintMarker) - 1;

// Walks part 0's attribute names looking for `np:version`.
//
// The OpenEXR header format, which is what makes this short: after the eight
// magic/version bytes, a part's header is a sequence of attributes, and the
// sequence ends with a zero-length name (a single 0x00 byte). Each attribute is
//
//     name  null-terminated string
//     type  null-terminated string
//     size  int32, little-endian
//     value `size` bytes
//
// so skipping an attribute whose type this code knows nothing about is exactly
// "skip `size` bytes", which is why no attribute-type table is needed here.
//
// **It cannot read past `data + size`.** Every string scan stops at `size`, the
// four size bytes are bounds-checked before they are read, and the value skip
// is checked as `valueSize > size - p` rather than `p + valueSize > size` so
// that a hostile 0xFFFFFFFF size cannot wrap. Anything malformed returns false,
// which routes the file to the image reader -- the conservative direction: a
// truncated `.npaint` then fails as a damaged image rather than being handed to
// a document reader that would report something less clear.
//
// The iteration cap is 4096 attributes. A real part 0 written by this
// application has under twenty (io/NpaintFile.cpp's `np:*` set plus OpenEXR's
// required attributes plus whatever PRD I10 carry came in with it), so 4096 is
// two orders of magnitude of headroom; it exists only so that a file whose
// bytes happen to form a cycle of zero-length skips cannot spin.
bool exrHeaderHasNpaintMarker(const uint8_t* data, size_t size) {
  size_t p = 8;  // past magic (4) + version field (4)
  for (int attribute = 0; attribute < 4096; ++attribute) {
    // --- name ---
    const size_t nameStart = p;
    while (p < size && data[p] != 0) ++p;
    if (p >= size) return false;  // ran off the end mid-name
    const size_t nameLen = p - nameStart;
    ++p;                          // past the terminating NUL
    if (nameLen == 0) return false;  // zero-length name: end of part 0's header

    if (nameLen == kNpaintMarkerLen &&
        std::memcmp(data + nameStart, kNpaintMarker, kNpaintMarkerLen) == 0)
      return true;

    // --- type ---
    const size_t typeStart = p;
    while (p < size && data[p] != 0) ++p;
    if (p >= size) return false;
    if (p == typeStart) return false;  // an attribute with no type is malformed
    ++p;

    // --- size, then skip the value ---
    if (size - p < 4) return false;
    const uint32_t valueSize = static_cast<uint32_t>(data[p]) |
                               (static_cast<uint32_t>(data[p + 1]) << 8) |
                               (static_cast<uint32_t>(data[p + 2]) << 16) |
                               (static_cast<uint32_t>(data[p + 3]) << 24);
    p += 4;
    if (valueSize > size - p) return false;  // subtraction, never addition
    p += valueSize;
  }
  return false;
}

FileSniff image(const char* signature, std::optional<ImageFormat> format) {
  FileSniff s;
  s.kind = FileKind::Image;
  s.signature = signature;
  s.format = format;
  return s;
}

// --- SVG: a structural scan, not a substring search -------------------------
//
// The full argument for why this is a scan of the grammar rather than a
// search for the string `<svg` is in FileKind.hpp; this is the walk itself.

// A few kilobytes -- generous for any real SVG's prologue (an XML
// declaration, a handful of comments, a doctype), and small enough that a
// file with an "unreasonable" prologue costs this function almost nothing to
// give up on. Never reads past `min(size, kSvgMaxScanBytes)`, which is itself
// never past `size`.
constexpr size_t kSvgMaxScanBytes = 4096;

// How many `<?...?>` / `<!--...-->` / `<!DOCTYPE ...>` constructs the scan
// will skip before giving up. A real document has at most a few (one XML
// declaration, maybe a comment, maybe a doctype); this is two orders of
// magnitude of headroom so that only a pathological or hostile prologue ever
// hits it, and hitting it answers `Unknown` rather than looping.
constexpr int kSvgMaxPrologueConstructs = 64;

bool isXmlSpace(uint8_t c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// XML name characters this module actually needs to tell `svg` and `*:svg`
// apart from every other element name. Not the full XML `Name` production
// (which also admits a long tail of Unicode ranges) -- this only ever
// compares the collected run against the fixed strings `"svg"` and `":svg"`,
// so under-recognising a Unicode name character just means that name is
// read as ending one byte earlier than XML would say, which cannot turn a
// non-`svg` name into a match and therefore cannot manufacture a false
// positive.
bool isXmlNameChar(uint8_t c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '_' || c == '.' || c == ':';
}

// Skips one `<?...?>` processing instruction (the XML declaration is one:
// `<?xml version="1.0"?>`) starting at `data[p]`. `p` must already point at
// the leading `<`. Returns the offset just past the closing `?>`, or
// `std::nullopt` if `data[p..limit)` is not `<?...?>` at all, or is but never
// closes within `limit` -- the caller treats both the same way, as "this is
// not SVG", which is correct: real SVG never emits an unterminated
// processing instruction.
std::optional<size_t> skipProcessingInstruction(const uint8_t* data, size_t limit,
                                                size_t p) {
  if (p + 1 >= limit || data[p] != '<' || data[p + 1] != '?') return std::nullopt;
  size_t q = p + 2;
  while (q + 1 < limit) {
    if (data[q] == '?' && data[q + 1] == '>') return q + 2;
    ++q;
  }
  return std::nullopt;
}

// Skips one `<!--...-->` comment. Same contract as the function above.
std::optional<size_t> skipComment(const uint8_t* data, size_t limit, size_t p) {
  if (p + 3 >= limit || data[p] != '<' || data[p + 1] != '!' || data[p + 2] != '-' ||
      data[p + 3] != '-')
    return std::nullopt;
  size_t q = p + 4;
  while (q + 2 < limit) {
    if (data[q] == '-' && data[q + 1] == '-' && data[q + 2] == '>') return q + 3;
    ++q;
  }
  return std::nullopt;
}

// Skips one `<!DOCTYPE ...>`, tracking bracket depth and quote state so a
// `>` (or a `[`/`]`) inside a quoted string in the internal subset, or inside
// the subset's own `[...]` brackets, does not end the construct early --
// FileKind.hpp's fourth stated limit says this is handled rather than
// assumed away, and this is the handling. Same not-SVG-either-way contract on
// failure as the two functions above.
std::optional<size_t> skipDoctype(const uint8_t* data, size_t limit, size_t p) {
  static constexpr char kDoctype[] = "<!DOCTYPE";
  constexpr size_t kDoctypeLen = sizeof(kDoctype) - 1;
  if (p + kDoctypeLen > limit || std::memcmp(data + p, kDoctype, kDoctypeLen) != 0)
    return std::nullopt;

  size_t q = p + kDoctypeLen;
  int bracketDepth = 0;
  char quote = 0;
  while (q < limit) {
    const uint8_t c = data[q];
    if (quote != 0) {
      if (c == static_cast<uint8_t>(quote)) quote = 0;
    } else if (c == '\'' || c == '"') {
      quote = static_cast<char>(c);
    } else if (c == '[') {
      ++bracketDepth;
    } else if (c == ']') {
      if (bracketDepth > 0) --bracketDepth;
    } else if (c == '>' && bracketDepth == 0) {
      return q + 1;
    }
    ++q;
  }
  return std::nullopt;
}

// The whole scan FileKind.hpp describes. `data` is not null and `size > 0` --
// checked once, by `sniffFileKind()`, before either signature test runs.
bool sniffIsSvg(const uint8_t* data, size_t size) {
  const size_t limit = std::min(size, kSvgMaxScanBytes);
  size_t p = 0;

  // 1. An optional UTF-8 BOM.
  if (limit >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) p = 3;

  auto skipSpace = [&]() {
    while (p < limit && isXmlSpace(data[p])) ++p;
  };

  // 2. Leading whitespace, then 3. every complete prologue construct and the
  // whitespace between them.
  skipSpace();
  for (int construct = 0; construct < kSvgMaxPrologueConstructs; ++construct) {
    if (p >= limit || data[p] != '<') break;  // nothing more to skip
    std::optional<size_t> next = skipProcessingInstruction(data, limit, p);
    if (!next) next = skipComment(data, limit, p);
    if (!next) next = skipDoctype(data, limit, p);
    if (!next) {
      // Starts with `<` but is none of the three known prologue constructs --
      // either the root element (handled below) or some other `<!...`
      // construct this scan does not recognise. Either way, stop skipping;
      // the element-tag check that follows is the only thing that can still
      // say SVG.
      break;
    }
    p = *next;
    skipSpace();
  }

  // 4. What is left must be an element start tag, and it must be `svg` or
  // end in `:svg`. Reaching the end of the buffer here, or finding `<!` or
  // `<?` or `</` instead of a name, is "not SVG" -- never a crash and never a
  // read past `limit`.
  if (p >= limit || data[p] != '<') return false;
  ++p;
  if (p >= limit || data[p] == '?' || data[p] == '!' || data[p] == '/') return false;

  const size_t nameStart = p;
  while (p < limit && isXmlNameChar(data[p])) ++p;
  const size_t nameLen = p - nameStart;
  if (nameLen == 0) return false;

  if (nameLen == 3 && std::memcmp(data + nameStart, "svg", 3) == 0) return true;
  if (nameLen > 4 && std::memcmp(data + nameStart + nameLen - 4, ":svg", 4) == 0)
    return true;
  return false;
}

}  // namespace

const char* fileKindName(FileKind kind) {
  switch (kind) {
    case FileKind::NpaintDocument: return "naturalPaint document";
    case FileKind::Image: return "image";
    case FileKind::Vector: return "vector image";
    case FileKind::Unknown: return "unrecognised";
  }
  return "unrecognised";
}

FileSniff sniffFileKind(const uint8_t* data, size_t size) {
  FileSniff unknown;
  if (data == nullptr || size == 0) return unknown;

  // --- OpenEXR first, because it is the one signature that can mean two
  // different things and only one of them is a picture ----------------------
  if (startsWith(data, size, kExrMagic, 4)) {
    if (exrHeaderHasNpaintMarker(data, size)) {
      FileSniff s;
      s.kind = FileKind::NpaintDocument;
      s.signature = "naturalPaint document (OpenEXR)";
      s.format = ImageFormat::Exr;
      return s;
    }
    return image("OpenEXR", ImageFormat::Exr);
  }

  // --- the rest, leading magic only ---------------------------------------
  //
  // Ordered by how distinctive the signature is rather than by how common the
  // format is: an eight-byte match cannot collide, a two-byte one can, so the
  // long ones are tested first and "BM" is tested last among the fixed magics.
  static constexpr uint8_t kPng[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (startsWith(data, size, kPng, 8)) return image("PNG", ImageFormat::Png);

  if (startsWith(data, size, "8BPS", 4)) return image("Photoshop PSD", ImageFormat::Psd);
  if (startsWith(data, size, "#?RADIANCE", 10) || startsWith(data, size, "#?RGBE", 6))
    return image("Radiance HDR", ImageFormat::Hdr);
  // DPX writes its magic big-endian or little-endian depending on the byte
  // order of the rest of the file; both spellings are the same format.
  if (startsWith(data, size, "SDPX", 4) || startsWith(data, size, "XPDS", 4))
    return image("DPX", ImageFormat::Dpx);
  // Cineon, DPX's predecessor. OpenImageIO's `format_list` on this build
  // contains "cineon", so it is recognised even though io/Capabilities has no
  // enumerator for it -- which is precisely the case `FileSniff::format` being
  // optional exists for.
  {
    static constexpr uint8_t kCineon[4] = {0x80, 0x2A, 0x5F, 0xD7};
    if (startsWith(data, size, kCineon, 4)) return image("Cineon", std::nullopt);
  }
  if (startsWith(data, size, "GIF87a", 6) || startsWith(data, size, "GIF89a", 6))
    return image("GIF", std::nullopt);
  // WebP is a RIFF container: "RIFF", four size bytes, then "WEBP".
  if (startsWith(data, size, "RIFF", 4) && size >= 12 &&
      std::memcmp(data + 8, "WEBP", 4) == 0)
    return image("WebP", std::nullopt);
  // TIFF, both byte orders. Also matched by most camera raws (CR2, NEF, ARW,
  // DNG are TIFF containers) -- see FileKind.hpp's second stated limit.
  {
    static constexpr uint8_t kTiffLE[4] = {0x49, 0x49, 0x2A, 0x00};
    static constexpr uint8_t kTiffBE[4] = {0x4D, 0x4D, 0x00, 0x2A};
    if (startsWith(data, size, kTiffLE, 4) || startsWith(data, size, kTiffBE, 4))
      return image("TIFF", ImageFormat::Tiff);
  }
  // JPEG: SOI (FF D8) followed by any marker (FF xx). Three bytes rather than
  // two, because FF D8 alone is two bytes of a bit pattern that turns up at the
  // front of plenty of non-JPEG binaries.
  {
    static constexpr uint8_t kJpeg[3] = {0xFF, 0xD8, 0xFF};
    if (startsWith(data, size, kJpeg, 3)) return image("JPEG", ImageFormat::Jpeg);
  }
  if (startsWith(data, size, "BM", 2)) return image("BMP", ImageFormat::Bmp);
  // Netpbm: 'P' then one of 1-7 then ASCII whitespace. The whitespace byte is
  // what keeps this from matching every file that happens to start with "P1".
  if (size >= 3 && data[0] == 'P' && data[1] >= '1' && data[1] <= '7' &&
      (data[2] == ' ' || data[2] == '\t' || data[2] == '\n' || data[2] == '\r'))
    return image("Netpbm", std::nullopt);

  // --- TGA, from the back ---------------------------------------------------
  //
  // The only format here with nothing usable at the front (FileKind.hpp's first
  // stated limit). TGA 2.0 ends with a 26-byte footer whose last eighteen bytes
  // are the literal below, NUL included.
  {
    static constexpr char kTgaFooter[] = "TRUEVISION-XFILE.";
    constexpr size_t kTgaFooterLen = sizeof(kTgaFooter);  // 18, counting the NUL
    if (size >= kTgaFooterLen &&
        std::memcmp(data + size - kTgaFooterLen, kTgaFooter, kTgaFooterLen) == 0)
      return image("TGA", ImageFormat::Tga);
  }

  // --- SVG, last: a structural scan rather than a magic number --------------
  //
  // Tried after every fixed-byte signature above, not before -- none of them
  // can collide with it (a magic number starts with a high bit or a
  // non-printable byte; SVG starts with a BOM, whitespace, or `<`), so the
  // order is purely "cheapest checks first". See FileKind.hpp for what the
  // scan does and why a substring search on `<svg` is not good enough.
  if (sniffIsSvg(data, size)) {
    FileSniff s;
    s.kind = FileKind::Vector;
    s.signature = "SVG";
    // No `ImageFormat`: io/Capabilities' enum names the raster formats this
    // application draws in its UI, and SVG is not one of them -- exactly the
    // case `FileSniff::format` being optional exists for (FileKind.hpp).
    s.format = std::nullopt;
    return s;
  }

  return unknown;
}

}  // namespace np
