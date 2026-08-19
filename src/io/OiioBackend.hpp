#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "io/Capabilities.hpp"
#include "io/ImageDecode.hpp"
#include "io/NpaintFile.hpp"

// io/OiioBackend (PLAN.md "Phase 4 -- Write it out", step 2: "`io/OiioBackend`
// behind `NP_USE_OIIO` -- EXR, TIFF, HDR, DPX, flattened PSD, camera raw").
//
// The OpenImageIO-backed read and write path. Everything in this header is
// declared unconditionally and *defined* only in OiioBackend.cpp, which
// src/CMakeLists.txt compiles into the target only when NP_USE_OIIO is ON.
// With it OFF this header is never included by anything (io/Capabilities.cpp
// and io/Export.cpp both guard their `#include` with `#if defined(NP_USE_OIIO)`)
// and the binary contains no OpenImageIO symbol at all -- PRD I3's "the core
// builds and runs without OIIO", enforced by the link step rather than by
// convention.
//
// **No OpenImageIO header is included here.** Every type crossing this
// boundary is `std`, `np::DecodedImage`, `np::ImageFormat` or one of
// io/NpaintFile's part/attribute structs. That is a hard
// rule, not a style preference: OiioBackend.cpp is the only translation unit
// in this project that may `#include <OpenImageIO/...>`, so an accidental
// OIIO dependency cannot leak into a module that has to keep compiling with
// the backend switched off.
//
// --- What of step 2's list actually landed -------------------------------
//
// EXR, TIFF, HDR, DPX and flattened-PSD read: yes.
// **Camera raw: NO -- deliberately, and not by this module's choice.** The
// OpenImageIO this project links against was built without LibRaw, to keep
// the dependency from dragging in LibRaw and its own transitive weight. The
// consequence is measurable rather than assumed: this build's own
// `format_list` attribute reads
//
//   openexr,tiff,jpeg,bmp,cineon,dds,dpx,fits,gif,hdr,ico,iff,null,png,pnm,
//   psd,rla,sgi,softimage,targa,term,zfile
//
// with no "raw" entry, so io/Capabilities reports camera raw unsupported
// *in the OIIO build too*. PRD I2 (camera raw) is P1 and stays open. This
// module does not paper over it, and --selftest asserts the unsupported
// answer specifically, because a capability query that answered "yes" here
// would be a query in name only.
//
// --- Colour and alpha conventions, both measured against this build ------
//
// Two conventions have to be pinned down at this boundary, and both were
// determined by round-tripping real files through this exact OpenImageIO
// rather than from memory of what the formats "usually" do.
//
// 1. **Linearity is decided by the file's sample type, not its extension.**
//    A float- or half-typed file (EXR, HDR, float TIFF/DPX) is linear-light
//    by convention and is read straight through with no transfer function.
//    An integer-typed file (8/16-bit TIFF, DPX, PSD) is assumed sRGB-encoded
//    and decoded through color/Space's srgbDecode() -- the identical,
//    already-documented assumption io/ImageDecode.cpp makes for untagged
//    PNG/JPEG/TGA/BMP, reused rather than re-decided. Measured caveat:
//    OpenImageIO's `oiio:ColorSpace` attribute, which would be the better
//    signal, is *absent* on every EXR/TIFF/DPX/PSD this build produced or
//    read (only its PNG reader sets it, to "srgb_rec709_scene"), so there is
//    nothing more authoritative available to consult. Known limitation, same
//    shape as ImageDecode's: a genuinely log-encoded DPX (the normal case in
//    a film pipeline) decodes as if it were sRGB. Fixing that means a real
//    OCIO config, which is nowhere in this codebase yet.
//
// 2. **EXR carries associated (premultiplied) alpha; everything else here
//    carries straight alpha.** This matters because OpenImageIO does *not*
//    do the conversion for us on either side for EXR/TIFF/DPX -- verified by
//    writing straight (0.8, 0.4, 0.2, a=0.5) and reading back exactly that.
//    So the choice is ours to make, and it is made per the OpenEXR spec, in
//    which alpha is premultiplied. io/Export multiplies RGB by alpha *in
//    linear light, before the transfer function* (association is a
//    linear-light operation; associating after a curve would not survive a
//    reader that un-associates in linear), and oiioDecodeToLinear() below
//    un-associates on the way back in with the same `a <= 0 -> {0,0,0,0}`
//    guard core/Probe.cpp and io/Export.cpp already use. This is also what
//    docs/document-format.md's step-4 native container wants: it stores the
//    working space's premultiplied `rgba16float` tiles as EXR HALF channels
//    "byte-identical, no conversion", which is only true if EXR is the
//    associated-alpha side of this boundary.
namespace np {

// What one format's writer can actually do, discovered by asking the linked
// OpenImageIO rather than by consulting a table. io/Capabilities.cpp turns
// this into a FormatCapability; nothing else calls it.
struct OiioFormatProbe {
  // Present in OpenImageIO's `input_format_list` / `output_format_list`.
  bool canRead = false;
  bool canWrite = false;
  // Whether a 4-channel (RGBA) image opens at all. False for HDR, whose
  // writer answers "hdr does not support 4-channel images" -- discovered,
  // not written down.
  bool hasAlpha = false;
  // Indexed by static_cast<size_t>(ExportBitDepth). True only when a real
  // 1x1 open at that sample type reports back the *same* sample type. See
  // io/Capabilities.hpp on why "OpenImageIO accepted it" is not the same
  // question as "OpenImageIO will write it".
  std::array<bool, kExportBitDepthCount> writableDepths{};
};

// Probes one format end to end. `format` must be one of the OIIO-backed
// members of ImageFormat (imageFormatOiioName() non-empty); anything else
// returns an all-false probe.
//
// The write probe genuinely opens an in-memory writer, writes one texel and
// closes it, for each depth. It never touches the filesystem (OpenImageIO's
// Filesystem::IOVecOutput proxy) and never leaves the bytes anywhere.
OiioFormatProbe oiioProbeFormat(ImageFormat format);

// True when `name` (an OpenImageIO format name, e.g. "raw") appears in the
// linked library's runtime `format_list`.
bool oiioFormatPresent(const char* name);

// "3.1.16.0" -- OpenImageIO's own `version` attribute, not a compile-time
// constant, so it reports the library actually loaded.
std::string oiioVersionString();

// The linked library's `format_list` attribute, verbatim.
std::string oiioFormatList();

// Encodes an already-transfer-function-encoded, `channels`-wide float image
// into `format` file bytes at `depth`.
//
// `samples` holds width*height*channels floats, row-major top-to-bottom.
// io/Export has already applied the target space's transfer function to RGB,
// already associated alpha where this backend's conventions call for it, and
// already clamped (or deliberately not clamped) to [0,1]. This function's
// job is purely "turn these numbers into that file format at that sample
// depth" -- it makes no colour decisions of its own.
//
// Returns false with a specific message in `*errorOut` when the writer
// cannot be created, the open fails, OpenImageIO would substitute a
// different sample type than the one requested (which io/Capabilities'
// probe should already have caught -- this is the belt-and-braces check at
// the point the real file is produced), or the write itself fails. `*out` is
// left untouched on failure, so a refused request can never produce partial
// bytes.
bool oiioEncodeToMemory(const std::vector<float>& samples, uint32_t width, uint32_t height,
                        int channels, ImageFormat format, ExportBitDepth depth,
                        std::vector<uint8_t>* out, std::string* errorOut);

// Decodes any format the linked OpenImageIO can read, from bytes already in
// memory, to io/ImageDecode's DecodedImage contract: linear-light float
// RGBA, straight (non-premultiplied) alpha, row-major top-to-bottom.
//
// The format is detected from the *content*, not from a filename -- verified
// against this build, which opens an in-memory EXR correctly even when
// handed a filename of "blob.dat". So this has the same signature shape as
// decodeImageLinear() and needs no extension hint from the caller.
//
// Returns a DecodedImage with width == 0 (valid() == false) on failure, with
// `*errorOut` set from OpenImageIO's own error string. This is the function
// io/ImageDecode.cpp falls back to when stb_image declines the bytes, which
// is what makes opening an EXR or a flattened PSD work through the existing
// openImageAsDocument() path with no new entry point.
DecodedImage oiioDecodeToLinear(const uint8_t* fileData, std::size_t fileSize,
                                std::string* errorOut);

// --- Multi-part tiled EXR, for the native `.npaint` container -------------
//
// PLAN.md Phase 4 step 4 / PRD I4. These two functions are deliberately
// *dumb*: they move parts, channels, attributes and bytes between a file and
// io/NpaintFile's structs, and they make no decision about what a part
// means. Every format decision -- which parts are layers, which attributes
// are recognised, what part 0 contains, which compressors are allowed --
// lives in io/NpaintFile.cpp, which compiles in both build configurations.
// The split is the same one the rest of this header already keeps: the OIIO
// translation unit knows OpenImageIO, and nothing else does.
//
// docs/document-format.md's claim that native save "needs *zero* bespoke
// writer code" is what these are: `ImageOutput::open(path, nsubimages,
// specs)` plus one `write_image` per part.

// Everything one multi-part write needs. The attributes that
// docs/document-format.md says "must match across all parts" --
// displayWindow, pixelAspectRatio, chromaticities -- are here, once, rather
// than per part, so they cannot disagree.
struct OiioExrWriteRequest {
  std::string path;
  // An OpenEXR compressor name, written to the `compression` attribute of
  // every part. io/NpaintFile has already refused the lossy ones (PRD I7);
  // this function does not re-litigate that, it just writes what it is
  // given.
  std::string compression = "zip";
  // The display window (the canvas). Origin is always (0,0) here -- nothing
  // in this codebase produces a document whose canvas starts elsewhere.
  int32_t displayWidth = 0;
  int32_t displayHeight = 0;
  // The standard `chromaticities` attribute (PRD I6): eight floats, in
  // OpenEXR's own order -- red x/y, green x/y, blue x/y, white x/y.
  bool hasChromaticities = false;
  std::array<float, 8> chromaticities{};
  // Part 0 first. Each part's `sampleTypeName` is an OpenImageIO TypeDesc
  // spelling ("half", "float", "uint8", ...) and its `rawPixels` must hold
  // exactly width * height * channelNames.size() * sizeof(that type) bytes,
  // row-major top-to-bottom.
  std::vector<NpaintRawPart> parts;
};

// Writes the request as a multi-part EXR. Returns false with a specific
// message in `*errorOut` -- naming the part, and OpenImageIO's own error
// text -- if the writer cannot be created, refuses multi-part output, or
// fails at any point. On failure the file is closed and removed, so a failed
// save never leaves a half-written document where a good one used to be.
bool oiioWriteMultiPartExr(const OiioExrWriteRequest& request, std::string* errorOut);

struct OiioExrReadResult {
  bool ok = false;
  std::string error;
  int32_t displayWidth = 0;
  int32_t displayHeight = 0;
  bool hasChromaticities = false;
  std::array<float, 8> chromaticities{};
  // Every part, in file order, pixels in the part's own sample type.
  std::vector<NpaintRawPart> parts;
  // One entry per attribute whose EXR type is not one of the four
  // docs/document-format.md permits (string / int / float / UINT8[n]) and
  // which therefore could not be carried. Reported rather than dropped in
  // silence -- that is the difference between a documented limit and a bug.
  std::vector<std::string> warnings;
};

// Reads every part of a multi-part (or single-part) EXR. `np:*` attributes
// come back on the part that carried them; container attributes
// (compression, chromaticities, the windows) are hoisted to the result or
// left behind, per io/NpaintFile's carry-scope rule.
OiioExrReadResult oiioReadMultiPartExr(const std::string& path);

}  // namespace np
