#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

// io/Capabilities (PLAN.md "Phase 4 -- Write it out", step 3: "Capability
// query -- format support is discovered at runtime; the core builds and runs
// without OIIO (PRD I3)").
//
// PRD I3 (P0) reads, in full: "Format support is a runtime capability query,
// not a build-time hard requirement." This header takes that literally, and
// the literal reading is load-bearing rather than pedantic. The OpenImageIO
// this project links against is a deliberately stripped-down local build
// (no LibRaw, no ffmpeg, no HEIF/JPEG-XL/WebP), so ANY table of the form
//
//     if NP_USE_OIIO then { EXR, TIFF, HDR, DPX, PSD, camera raw }
//
// would be factually wrong for this very build: camera raw is not there.
// A hardcoded table is not a capability query, it is a guess that happens to
// be checked at compile time -- and PLAN.md's own §1.5 lesson ("an
// unexercised build option is not a seam") is the same failure mode one
// level down. So:
//
//   * Whether a format exists at all is answered by asking the *linked*
//     OpenImageIO, at run time, through its own `format_list` /
//     `input_format_list` / `output_format_list` attributes.
//   * Which sample depths a format can be written at without OpenImageIO
//     silently substituting a different one is answered by *actually
//     opening a 1x1 writer for that format at that depth* and comparing
//     what OpenImageIO says it will write against what was asked for.
//     Nothing about that answer is written down here.
//   * Only the four stb-backed formats (PRD I1's PNG/JPEG/TGA/BMP) carry a
//     written-down table, because stb_image_write has no query API of any
//     kind to ask -- see kStbCapabilities in Capabilities.cpp, which is
//     exactly io/Export.cpp's original capsFor() table, moved rather than
//     rewritten.
//
// The depth probe is not decoration. Measured against this build, requesting
// an 8-bit EXR yields a `half` file, a half TIFF yields a `float` file, and
// a 16-bit TGA yields an 8-bit file -- OpenImageIO coerces silently and
// reports success. That is precisely the truncation PRD B6 ("Bit depth is
// preserved end to end; 16- and 32-bit files never silently truncate to 8")
// exists to forbid, so this module discovers the coercion up front and
// io/Export refuses the request by name instead of writing the wrong file
// and reporting success.
//
// --- The core builds and runs without OIIO -------------------------------
//
// The second half of step 3. Nothing in this header (or in io/Export.hpp,
// or anywhere else outside io/OiioBackend.cpp) includes an OpenImageIO
// header or names an OpenImageIO symbol. With NP_USE_OIIO=OFF,
// io/OiioBackend.cpp is not compiled and not linked, every OIIO-backed
// format reports `canRead == canWrite == false`, and `unavailableReason`
// says so in a sentence that names the build option rather than leaving a
// caller with a bare failure. The four stb formats answer identically in
// both configurations -- byte for byte the same capability struct -- which
// is what makes "PRD I1 needs no optional dependency" a property of the
// code rather than a claim in a document.
namespace np {

// Every image format this application can name. Deliberately a superset of
// what can be *written*: `Psd` is read-only here (PLAN.md step 2 asks for
// "flattened PSD" read; PSD *export* is phase 15, and the OpenImageIO
// linked here has no PSD writer at all -- its `output_format_list` does not
// contain "psd", which is a runtime fact this module checks rather than an
// assumption), and `CameraRaw` is read-only by nature.
//
// `CameraRaw` is listed even though this build cannot open a single camera
// raw file. That is the point: PRD I2 asks for camera raw, and the honest
// answer -- "this application knows what you are asking for, and this build
// cannot do it, and here is exactly why" -- requires the format to be
// nameable. Silently omitting it from the enum would make the capability
// query unable to answer the one question that proves it is a real query.
enum class ImageFormat {
  // PRD I1's four, stb-backed in every configuration.
  Png,
  Jpeg,
  Tga,
  Bmp,
  // PLAN.md step 2's OIIO-backed formats.
  Exr,
  Tiff,
  Hdr,
  Dpx,
  // Read-only.
  Psd,
  CameraRaw,
};

inline constexpr std::size_t kImageFormatCount = 10;

// Bits per channel, and how those bits are interpreted, in the written file.
//
// This enum was `{Eight, Sixteen}` when io/Export landed (step 1), back when
// every writable format here was integer-only and "16" could only mean one
// thing. It cannot stay that way now that EXR and HDR are in scope:
// "sixteen" in an EXR context means IEEE 754 binary16 *half float*, which is
// a completely different set of representable values from 16-bit unsigned
// integer -- same bit count, different numbers, different maximum (65504 vs
// 1.0-full-scale), and only one of the two can carry a value above full
// scale at all. Overloading one name across both would be exactly the kind
// of silent ambiguity PRD B6 and this module's "explicitly, never silently"
// discipline exist to prevent, so each case names its interpretation as well
// as its width.
//
// Still an enum rather than an int-plus-a-flag, for step 1's original
// reason: these four are the only expressible requests, so a caller cannot
// ask for 10 or 12 bits (which DPX genuinely has, and which nothing here
// writes) and silently receive a nearest fit.
enum class ExportBitDepth {
  // 8-bit unsigned integer, [0,1] mapped to [0,255].
  UInt8,
  // 16-bit unsigned integer, [0,1] mapped to [0,65535].
  UInt16,
  // IEEE 754 binary16 ("half"): 1 sign, 5 exponent, 10 stored mantissa
  // bits. Carries values above 1.0 (up to 65504) and below 0.0. This is the
  // working space's own storage format (core/Half.hpp, `rgba16float` tiles)
  // and EXR's native channel type, which is why docs/document-format.md
  // picks it for the native `.npaint` container: "HALF channels --
  // byte-identical, no conversion".
  Half,
  // IEEE 754 binary32 ("float").
  Float32,
};

inline constexpr std::size_t kExportBitDepthCount = 4;

// Which code actually reads or writes a format. Reported rather than
// inferred, because "PNG works" is true in both build configurations but
// for the same reason in each: PNG is stb-backed either way. See
// Capabilities.cpp's kStbCapabilities comment for why PNG/JPEG/TGA/BMP are
// deliberately NOT routed through OpenImageIO when it is available.
enum class FormatBackend {
  None,
  Stb,
  Oiio,
};

inline const char* imageFormatName(ImageFormat f) {
  switch (f) {
    case ImageFormat::Png: return "PNG";
    case ImageFormat::Jpeg: return "JPEG";
    case ImageFormat::Tga: return "TGA";
    case ImageFormat::Bmp: return "BMP";
    case ImageFormat::Exr: return "EXR";
    case ImageFormat::Tiff: return "TIFF";
    case ImageFormat::Hdr: return "HDR";
    case ImageFormat::Dpx: return "DPX";
    case ImageFormat::Psd: return "PSD";
    case ImageFormat::CameraRaw: return "camera raw";
  }
  return "?";
}

// The conventional filename extension, without a dot. Used to build the
// filename OpenImageIO's plugin lookup keys off -- see io/OiioBackend.cpp.
inline const char* imageFormatExtension(ImageFormat f) {
  switch (f) {
    case ImageFormat::Png: return "png";
    case ImageFormat::Jpeg: return "jpg";
    case ImageFormat::Tga: return "tga";
    case ImageFormat::Bmp: return "bmp";
    case ImageFormat::Exr: return "exr";
    case ImageFormat::Tiff: return "tif";
    case ImageFormat::Hdr: return "hdr";
    case ImageFormat::Dpx: return "dpx";
    case ImageFormat::Psd: return "psd";
    // No single extension: CR2/NEF/ARW/DNG/RAF/ORF and a dozen more are all
    // "camera raw". Nothing looks this up for camera raw today -- the
    // capability query answers "unsupported" long before an extension is
    // needed -- and inventing one would imply a support level this build
    // does not have.
    case ImageFormat::CameraRaw: return "";
  }
  return "";
}

// The name OpenImageIO itself uses in its `format_list` attribute, which is
// NOT always the extension: EXR is "openexr", TGA is "targa", TIFF is
// "tiff" while its extension is "tif". Returns "" for formats never looked
// up through OpenImageIO. The literal strings were read out of this build's
// own `format_list`, not recalled: "openexr,tiff,jpeg,bmp,cineon,dds,dpx,
// fits,gif,hdr,ico,iff,null,png,pnm,psd,rla,sgi,softimage,targa,term,zfile".
inline const char* imageFormatOiioName(ImageFormat f) {
  switch (f) {
    case ImageFormat::Exr: return "openexr";
    case ImageFormat::Tiff: return "tiff";
    case ImageFormat::Hdr: return "hdr";
    case ImageFormat::Dpx: return "dpx";
    case ImageFormat::Psd: return "psd";
    // OpenImageIO's LibRaw-backed plugin registers itself as "raw". Named
    // here precisely so the capability query can look for it and report,
    // truthfully, that this build does not have it.
    case ImageFormat::CameraRaw: return "raw";
    case ImageFormat::Png:
    case ImageFormat::Jpeg:
    case ImageFormat::Tga:
    case ImageFormat::Bmp: return "";
  }
  return "";
}

inline const char* exportBitDepthName(ExportBitDepth d) {
  switch (d) {
    case ExportBitDepth::UInt8: return "8-bit integer";
    case ExportBitDepth::UInt16: return "16-bit integer";
    case ExportBitDepth::Half: return "16-bit half float";
    case ExportBitDepth::Float32: return "32-bit float";
  }
  return "?";
}

inline int exportBitDepthBits(ExportBitDepth d) {
  switch (d) {
    case ExportBitDepth::UInt8: return 8;
    case ExportBitDepth::UInt16: return 16;
    case ExportBitDepth::Half: return 16;
    case ExportBitDepth::Float32: return 32;
  }
  return 8;
}

// True for the depths that store an IEEE floating-point number rather than
// a fraction of full scale. The single question io/Export asks to decide
// whether the [0,1] export clamp applies: an integer file has no
// representation for a value above full scale, a float file does, and
// clamping one because the other needs it would throw away exactly the
// highlight data EXR exists to keep.
inline bool exportBitDepthIsFloat(ExportBitDepth d) {
  return d == ExportBitDepth::Half || d == ExportBitDepth::Float32;
}

// What this binary, right now, can actually do with one format.
//
// `unavailableReason` is non-empty exactly when both `canRead` and
// `canWrite` are false, and always names the specific cause -- the build
// option, or the absent OpenImageIO plugin, and in the camera-raw case why
// that plugin is absent. That specificity is the requirement, not a nicety:
// PRD I11's "A save that would lose data names exactly what, rather than
// degrading silently" applied to the format-support question, and the same
// discipline io/Export.hpp already documents for depth and alpha refusals.
struct FormatCapability {
  ImageFormat format = ImageFormat::Png;
  FormatBackend backend = FormatBackend::None;
  bool canRead = false;
  bool canWrite = false;
  // Whether the written file has a place to put the alpha channel at all.
  // For OIIO-backed formats this is discovered by asking the writer to open
  // a 4-channel image and seeing whether it refuses -- which is how HDR
  // (Radiance RGBE, 3 channels by format definition) answers false without
  // anyone writing that fact down.
  bool hasAlpha = false;
  // Indexed by static_cast<size_t>(ExportBitDepth). True only when the
  // format can be written at that depth *without the backend silently
  // substituting a different one* -- see this header's opening comment.
  std::array<bool, kExportBitDepthCount> writableDepths{};
  std::string unavailableReason;

  bool canWriteDepth(ExportBitDepth d) const {
    return canWrite && writableDepths[static_cast<std::size_t>(d)];
  }
};

// The capability set for one format. The whole table is computed once, on
// first call, and cached in a function-local static -- so the OpenImageIO
// probe (which opens and closes a 1x1 in-memory writer per format per
// depth) runs at most once per process, and never at startup. Deliberately
// consistent with PLAN.md step 6's "Lazy OIIO init -- on first file open,
// not at startup, so PRD A2 holds", which this does not implement but must
// not preclude: nothing here runs until something asks a question.
const FormatCapability& formatCapability(ImageFormat format);

// Every format's capability, in ImageFormat declaration order. The query a
// UI would enumerate to build a format menu, and what --selftest walks.
const std::vector<FormatCapability>& allFormatCapabilities();

// A comma-separated list of the formats this build can write at `depth`,
// e.g. "PNG, TIFF, DPX" for UInt16 -- or an empty string when none can.
// Used to turn a refusal into an actionable one ("request 8-bit, or export
// one of these") rather than a dead end, and computed from the live
// capability table so it can never drift from what is actually supported.
std::string formatsThatCanWriteDepth(ExportBitDepth depth);

// Whether io/OiioBackend was compiled into this binary at all (i.e. whether
// NP_USE_OIIO was ON). Distinct from "EXR works": a build with OIIO linked
// could still lack a given plugin, which is the entire reason the rest of
// this header exists. Exposed so callers can tell the two failure modes
// apart in a message.
bool oiioBackendCompiledIn();

// One line naming the backend situation, for --selftest output and any
// future diagnostics panel. With OIIO: its version and its runtime format
// list, verbatim from the library. Without: a sentence saying so.
std::string imageBackendSummary();

}  // namespace np
