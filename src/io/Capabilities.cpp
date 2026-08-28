#include "io/Capabilities.hpp"

#include <string>

#include "io/OiioBackend.hpp"

namespace np {
namespace {

// --- The one written-down table in this module ---------------------------
//
// stb_image / stb_image_write have no capability query of any kind: there is
// no "which depths does the TGA writer support" entry point to ask, so the
// answer has to be written down. It is written down *here, once*, and it is
// character for character the table io/Export.cpp's original capsFor()
// already carried (PNG 16-bit because io/Export's encodePng16() exists, the
// other three 8-bit because stb_image_write emits 24/32-bit-per-*pixel*
// files for them; JPEG alpha-less because stbi_write_jpg_core reads only
// offsets 0/1/2 of each pixel) -- moved, not rewritten, so step 1's
// behaviour is unchanged by construction rather than by re-derivation.
//
// **PNG/JPEG/TGA/BMP are deliberately NOT routed through OpenImageIO even
// though it is linked into every build.** OpenImageIO can write all four, so
// this was a real fork. Keeping stb is the choice that makes PRD I1 ("read
// and write PNG, JPEG, TGA, BMP with no optional dependency") a structural
// property instead of a claim: these four take byte-for-byte the same code
// path regardless of what OpenImageIO does, so nothing OpenImageIO does can
// regress them, and --selftest's existing export section is testing that
// same stb encoder directly. The cost is that a 32-bit-float PNG is not offered
// (OpenImageIO would not write one either -- probed: its PNG writer
// substitutes uint8 for FLOAT) and that OpenImageIO's slightly different
// PNG/JPEG tuning is not available. Both are trivial next to a P0 guarantee
// that cannot rot.
struct StbCapability {
  ImageFormat format;
  bool hasAlpha;
  bool uint8;
  bool uint16;
};

constexpr StbCapability kStbCapabilities[] = {
    {ImageFormat::Png, true, true, true},
    {ImageFormat::Jpeg, false, true, false},
    {ImageFormat::Tga, true, true, false},
    {ImageFormat::Bmp, true, true, false},
};

FormatCapability stbCapability(const StbCapability& s) {
  FormatCapability cap;
  cap.format = s.format;
  cap.backend = FormatBackend::Stb;
  cap.canRead = true;   // io/ImageDecode's stb-backed decodeImageLinear()
  cap.canWrite = true;  // io/Export's stb_image_write path
  cap.hasAlpha = s.hasAlpha;
  cap.writableDepths[static_cast<size_t>(ExportBitDepth::UInt8)] = s.uint8;
  cap.writableDepths[static_cast<size_t>(ExportBitDepth::UInt16)] = s.uint16;
  // Half and Float32 stay false: none of these four formats stores IEEE
  // floating-point samples at all, so a float request is refused by name
  // rather than quietly rounded into an integer -- PRD B6 applied to the
  // depths this step newly makes expressible.
  return cap;
}

// The answer for a format the *linked* OpenImageIO turns out not to have.
// This is the case a build-time-only capability table gets wrong, so the
// message says out loud where the answer came from.
std::string missingPluginReason(ImageFormat format) {
  std::string reason = std::string(imageFormatName(format)) +
                       " is not available in this build: the linked OpenImageIO (version " +
                       oiioVersionString() + ") reports no '" + imageFormatOiioName(format) +
                       "' entry in its runtime format list, so the plugin that would handle "
                       "it was not built into this OpenImageIO. Its actual format list is: " +
                       oiioFormatList() + ".";
  if (format == ImageFormat::CameraRaw) {
    reason +=
        " That is a deliberate exclusion rather than an accident: this project's OpenImageIO "
        "was built without LibRaw, to keep LibRaw and its transitive dependency weight out of "
        "the build. PRD I2 (camera raw, P1) therefore remains open here, and PLAN.md Phase 4 "
        "step 2's \"camera raw\" is the one item on its list this build does not provide. "
        "Note where this answer comes from: OpenImageIO was asked at run time. OpenImageIO "
        "being linked does not imply any particular plugin set, which is exactly why PRD I3 "
        "specifies a runtime capability query.";
  }
  return reason;
}

// io/PsdImport reads PSD directly, with no OpenImageIO involvement at all --
// see io/Capabilities.hpp's own comment on `Psd` and io/PsdImport.hpp's
// header for why that module exists. So, like the four stb formats above,
// PSD's answer does not come from asking OpenImageIO anything: it is
// written down here, once, and it is the same answer in every build
// configuration, which is the whole point (io/PsdImport has no optional
// dependency to be absent).
//
// `canWrite` stays `false`: PSD export is separate, unstarted work (this
// module is read-only, matching the pre-existing "`Psd` is read-only here"
// note on the `ImageFormat` enum itself), so `writableDepths` is left at its
// default all-`false` and `unavailableReason` stays empty -- `canRead` is
// `true`, so the "both false" precondition that field's own contract
// requires never applies to this format.
FormatCapability psdCapability() {
  FormatCapability cap;
  cap.format = ImageFormat::Psd;
  cap.backend = FormatBackend::Native;
  cap.canRead = true;
  cap.canWrite = false;
  // A layer's own alpha channel (id -1) is read when present -- see
  // io/PsdImport.cpp's channel walk.
  cap.hasAlpha = true;
  return cap;
}

FormatCapability oiioCapability(ImageFormat format) {
  FormatCapability cap;
  cap.format = format;
  const OiioFormatProbe probe = oiioProbeFormat(format);
  cap.canRead = probe.canRead;
  cap.canWrite = probe.canWrite;
  cap.hasAlpha = probe.hasAlpha;
  cap.writableDepths = probe.writableDepths;
  cap.backend = (cap.canRead || cap.canWrite) ? FormatBackend::Oiio : FormatBackend::None;
  if (!cap.canRead && !cap.canWrite) cap.unavailableReason = missingPluginReason(format);
  return cap;
}

const std::vector<FormatCapability>& capabilityTable() {
  // Computed once, on first query, and never at startup -- see
  // io/Capabilities.hpp on why that matters for PLAN.md step 6's lazy-init
  // requirement. Function-local static initialisation is thread-safe in
  // C++11 and later, which is the only synchronisation this needs: the
  // table is const after construction.
  static const std::vector<FormatCapability> table = [] {
    std::vector<FormatCapability> t;
    t.reserve(kImageFormatCount);
    for (size_t i = 0; i < kImageFormatCount; ++i) {
      const ImageFormat format = static_cast<ImageFormat>(i);
      const StbCapability* stb = nullptr;
      for (const StbCapability& s : kStbCapabilities)
        if (s.format == format) stb = &s;
      if (stb) {
        t.push_back(stbCapability(*stb));
      } else if (format == ImageFormat::Psd) {
        t.push_back(psdCapability());
      } else {
        t.push_back(oiioCapability(format));
      }
    }
    return t;
  }();
  return table;
}

}  // namespace

const FormatCapability& formatCapability(ImageFormat format) {
  const std::vector<FormatCapability>& table = capabilityTable();
  const size_t index = static_cast<size_t>(format);
  return table[index < table.size() ? index : 0];
}

const std::vector<FormatCapability>& allFormatCapabilities() { return capabilityTable(); }

std::string formatsThatCanWriteDepth(ExportBitDepth depth) {
  std::string list;
  for (const FormatCapability& cap : capabilityTable()) {
    if (!cap.canWriteDepth(depth)) continue;
    if (!list.empty()) list += ", ";
    list += imageFormatName(cap.format);
  }
  return list;
}

bool oiioBackendCompiledIn() { return true; }

std::string imageBackendSummary() {
  return "image backends: stb (PNG/JPEG/TGA/BMP) + io/PsdImport (PSD, layered, "
         "dependency-free) + OpenImageIO " +
         oiioVersionString() + " [" + oiioFormatList() + "]";
}

}  // namespace np
