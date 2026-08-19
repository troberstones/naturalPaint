#include "io/ExportAs.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "color/Space.hpp"
#include "ops/Resample.hpp"

// The preset file's schema, which is the only part of this module that is a
// compatibility commitment:
//
//   {
//     "version": 1,
//     "presets": [
//       {
//         "name": "Web preview",
//         "format": "png",          // io/Capabilities' ImageFormat
//         "space": "rec709-srgb",   // io/Export's ExportTargetSpace
//         "depth": "uint8",         // io/Capabilities' ExportBitDepth
//         "resize": "fit-within",   // ExportResizeMode
//         "percent": 100,
//         "maxWidth": 2048,
//         "maxHeight": 2048
//       }
//     ]
//   }
//
// Every field is written on every save, including the two the current
// `resize` mode does not read -- so flipping a preset between Percentage and
// Fit within does not quietly discard the other mode's numbers, and a
// hand-edited file has every knob visible rather than only the ones that
// happened to be in use.
//
// `version` is read and ignored today. It exists so that a future schema
// change has somewhere to announce itself; a reader that needed it and did
// not have it would be stuck guessing.
namespace np {
namespace {

// --- A tiny JSON reader, for exactly this schema -------------------------
//
// Same call this project already made once, for the same reasons, and
// app/Keymap.cpp's own version says them at length: there is no JSON library
// vendored anywhere here (checked third_party/ and cmake/), the schema is
// small and fixed, and hand-rolling beats taking a dependency for it.
//
// Deliberately a second copy rather than a promotion of Keymap.cpp's: that
// one is `static` inside its translation unit, parses a different shape, and
// hoisting it means editing app/Keymap, which is not this step's business.
// io/Export.cpp's duplicated unpremultiply() carries the identical note and
// the identical trigger -- a *third* consumer is when this becomes a shared
// header rather than a judgement call.
class JsonReader {
 public:
  JsonReader(std::string_view text, std::string_view label) : s_(text), label_(label) {}

  bool failed() const { return failed_; }
  const std::string& error() const { return error_; }

  void fail(const std::string& what) {
    if (failed_) return;
    failed_ = true;
    error_ = std::string(label_) + ": " + what + " (at byte " + std::to_string(i_) + ")";
  }

  void skipWs() {
    while (i_ < s_.size() &&
           (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r'))
      ++i_;
  }

  char peek() {
    skipWs();
    return i_ < s_.size() ? s_[i_] : '\0';
  }

  bool expect(char c) {
    skipWs();
    if (i_ >= s_.size() || s_[i_] != c) {
      fail(std::string("expected '") + c + "'");
      return false;
    }
    ++i_;
    return true;
  }

  bool parseString(std::string* out) {
    skipWs();
    if (i_ >= s_.size() || s_[i_] != '"') {
      fail("expected a string");
      return false;
    }
    ++i_;
    out->clear();
    while (i_ < s_.size() && s_[i_] != '"') {
      char c = s_[i_++];
      if (c == '\\' && i_ < s_.size()) {
        const char e = s_[i_++];
        switch (e) {
          case '"': *out += '"'; break;
          case '\\': *out += '\\'; break;
          case '/': *out += '/'; break;
          case 'n': *out += '\n'; break;
          case 't': *out += '\t'; break;
          // Sufficient for preset names and the fixed token vocabulary; a
          // \uXXXX escape is not something this writer ever emits.
          default: *out += e; break;
        }
      } else {
        *out += c;
      }
    }
    if (i_ >= s_.size()) {
      fail("unterminated string");
      return false;
    }
    ++i_;  // closing quote
    return true;
  }

  bool parseNumber(double* out) {
    skipWs();
    const size_t start = i_;
    while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' ||
                              s_[i_] == '+' || s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E'))
      ++i_;
    if (i_ == start) {
      fail("expected a number");
      return false;
    }
    *out = std::strtod(std::string(s_.substr(start, i_ - start)).c_str(), nullptr);
    return true;
  }

  // Consumes and discards any value, so an unrecognised field is skipped
  // rather than fatal -- a newer build's extra key must not stop an older
  // one from reading the presets it does understand.
  bool skipValue(int depth = 0) {
    if (depth > 16) {
      fail("value nested too deeply");
      return false;
    }
    const char c = peek();
    if (c == '"') {
      std::string ignored;
      return parseString(&ignored);
    }
    if (c == '{') {
      if (!expect('{')) return false;
      if (peek() == '}') return expect('}');
      for (;;) {
        std::string key;
        if (!parseString(&key) || !expect(':') || !skipValue(depth + 1)) return false;
        if (peek() == ',') {
          ++i_;
          continue;
        }
        break;
      }
      return expect('}');
    }
    if (c == '[') {
      if (!expect('[')) return false;
      if (peek() == ']') return expect(']');
      for (;;) {
        if (!skipValue(depth + 1)) return false;
        if (peek() == ',') {
          ++i_;
          continue;
        }
        break;
      }
      return expect(']');
    }
    if (c == 't' || c == 'f' || c == 'n') {
      while (i_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[i_]))) ++i_;
      return true;
    }
    double ignored = 0.0;
    return parseNumber(&ignored);
  }

  void consume() { ++i_; }

 private:
  std::string_view s_;
  std::string_view label_;
  size_t i_ = 0;
  bool failed_ = false;
  std::string error_;
};

bool asciiEqualNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const auto ca = static_cast<unsigned char>(a[i]);
    const auto cb = static_cast<unsigned char>(b[i]);
    if (std::tolower(ca) != std::tolower(cb)) return false;
  }
  return true;
}

std::string trimmed(std::string_view s) {
  size_t b = 0, e = s.size();
  auto isSpace = [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
  };
  while (b < e && isSpace(s[b])) ++b;
  while (e > b && isSpace(s[e - 1])) --e;
  return std::string(s.substr(b, e - b));
}

std::string escapeJson(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 4);
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

// Empty when the name is usable; otherwise the specific reason, in this
// codebase's refusal style (name the thing, name the limit, name the fix).
std::string presetNameProblem(std::string_view rawName) {
  const std::string name = trimmed(rawName);
  if (name.empty()) {
    return "preset refused: a preset needs a name, and this one is empty or whitespace only. "
           "The name is how it is picked out of the preset menu again.";
  }
  if (name.size() > ExportPresetStore::kMaxPresetNameLength) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "preset refused: the name is %zu characters; the limit is %zu. A name longer "
                  "than the menu can show is a name nobody can tell apart from its neighbour.",
                  name.size(), ExportPresetStore::kMaxPresetNameLength);
    return buf;
  }
  for (size_t i = 0; i < name.size(); ++i) {
    const auto c = static_cast<unsigned char>(name[i]);
    if (c < 0x20 || c == 0x7F) {
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "preset refused: the name contains a control character (0x%02X) at position "
                    "%zu. It would not render in the preset menu and would not survive the "
                    "preset file intact.",
                    static_cast<unsigned>(c), i);
      return buf;
    }
  }
  return std::string();
}

ExportResult exportFailure(std::string message) {
  ExportResult r;
  r.ok = false;
  r.error = std::move(message);
  return r;
}

bool sizeFailure(std::string* errorOut, uint32_t* outW, uint32_t* outH, std::string message) {
  if (outW) *outW = 0;
  if (outH) *outH = 0;
  if (errorOut) *errorOut = std::move(message);
  return false;
}

uint32_t scaleAxis(uint32_t src, double scale) {
  const double v = std::floor(static_cast<double>(src) * scale + 0.5);
  if (v < 1.0) return 1u;
  if (v > static_cast<double>(src)) return src;
  return static_cast<uint32_t>(v);
}

}  // namespace

// --- Tokens ---------------------------------------------------------------

const char* exportFormatToken(ImageFormat format) {
  switch (format) {
    case ImageFormat::Png: return "png";
    case ImageFormat::Jpeg: return "jpeg";
    case ImageFormat::Tga: return "tga";
    case ImageFormat::Bmp: return "bmp";
    case ImageFormat::Exr: return "exr";
    case ImageFormat::Tiff: return "tiff";
    case ImageFormat::Hdr: return "hdr";
    case ImageFormat::Dpx: return "dpx";
    case ImageFormat::Psd: return "psd";
    case ImageFormat::CameraRaw: return "camera-raw";
  }
  return "png";
}

const char* exportTargetSpaceToken(ExportTargetSpace space) {
  switch (space) {
    case ExportTargetSpace::Rec709Linear: return "rec709-linear";
    case ExportTargetSpace::Rec709Srgb: return "rec709-srgb";
    case ExportTargetSpace::Rec709Bt709: return "rec709-bt709";
  }
  return "rec709-srgb";
}

const char* exportBitDepthToken(ExportBitDepth depth) {
  switch (depth) {
    case ExportBitDepth::UInt8: return "uint8";
    case ExportBitDepth::UInt16: return "uint16";
    case ExportBitDepth::Half: return "half";
    case ExportBitDepth::Float32: return "float32";
  }
  return "uint8";
}

const char* exportResizeModeToken(ExportResizeMode mode) {
  switch (mode) {
    case ExportResizeMode::None: return "none";
    case ExportResizeMode::Percent: return "percent";
    case ExportResizeMode::FitWithin: return "fit-within";
  }
  return "none";
}

bool exportFormatFromToken(std::string_view token, ImageFormat* out) {
  for (std::size_t i = 0; i < kImageFormatCount; ++i) {
    const auto f = static_cast<ImageFormat>(i);
    if (token == exportFormatToken(f)) {
      if (out) *out = f;
      return true;
    }
  }
  return false;
}

bool exportTargetSpaceFromToken(std::string_view token, ExportTargetSpace* out) {
  for (int i = 0; i < 3; ++i) {
    const auto s = static_cast<ExportTargetSpace>(i);
    if (token == exportTargetSpaceToken(s)) {
      if (out) *out = s;
      return true;
    }
  }
  return false;
}

bool exportBitDepthFromToken(std::string_view token, ExportBitDepth* out) {
  for (std::size_t i = 0; i < kExportBitDepthCount; ++i) {
    const auto d = static_cast<ExportBitDepth>(i);
    if (token == exportBitDepthToken(d)) {
      if (out) *out = d;
      return true;
    }
  }
  return false;
}

bool exportResizeModeFromToken(std::string_view token, ExportResizeMode* out) {
  for (std::size_t i = 0; i < kExportResizeModeCount; ++i) {
    const auto m = static_cast<ExportResizeMode>(i);
    if (token == exportResizeModeToken(m)) {
      if (out) *out = m;
      return true;
    }
  }
  return false;
}

// --- Size resolution ------------------------------------------------------

bool resolveExportSize(const ExportResize& resize, uint32_t srcWidth, uint32_t srcHeight,
                       uint32_t* outWidth, uint32_t* outHeight, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (outWidth == nullptr || outHeight == nullptr) return false;
  *outWidth = 0;
  *outHeight = 0;

  if (srcWidth == 0 || srcHeight == 0) {
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "resize refused: the source is %ux%u, so there is no size to scale.", srcWidth,
                  srcHeight);
    return sizeFailure(errorOut, outWidth, outHeight, buf);
  }

  switch (resize.mode) {
    case ExportResizeMode::None:
      *outWidth = srcWidth;
      *outHeight = srcHeight;
      return true;

    case ExportResizeMode::Percent: {
      // `!(x > 0)` rather than `x <= 0` so a NaN -- which compares false
      // against everything -- is refused rather than sliding through into
      // the multiply, the same shape io/Export.cpp's quantize() uses.
      if (!(resize.percent > 0.0f)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "resize refused: %.4g%% is not a size. Enter a percentage above 0 and at "
                      "most 100.",
                      static_cast<double>(resize.percent));
        return sizeFailure(errorOut, outWidth, outHeight, buf);
      }
      if (resize.percent > 100.0f) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "resize refused: %.4g%% would enlarge the image, and this build resamples "
                      "downwards only -- upscaling is decided by a reconstruction filter nobody "
                      "has chosen yet (PLAN.md phase 6's transform op). Use 100%% or less, or "
                      "'Fit within', which clamps to 1:1 instead of enlarging.",
                      static_cast<double>(resize.percent));
        return sizeFailure(errorOut, outWidth, outHeight, buf);
      }
      const double scale = static_cast<double>(resize.percent) / 100.0;
      *outWidth = scaleAxis(srcWidth, scale);
      *outHeight = scaleAxis(srcHeight, scale);
      return true;
    }

    case ExportResizeMode::FitWithin: {
      if (resize.maxWidth == 0 || resize.maxHeight == 0) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "resize refused: the fit-within box is %ux%u; both sides must be at least "
                      "1 pixel.",
                      resize.maxWidth, resize.maxHeight);
        return sizeFailure(errorOut, outWidth, outHeight, buf);
      }
      // Clamped at 1: fitting a small document into a large box is a
      // no-op, never an enlargement. See ExportAs.hpp on why this is
      // deliberately different from Percent's refusal.
      const double sx = static_cast<double>(resize.maxWidth) / static_cast<double>(srcWidth);
      const double sy = static_cast<double>(resize.maxHeight) / static_cast<double>(srcHeight);
      const double scale = std::min(1.0, std::min(sx, sy));
      *outWidth = scaleAxis(srcWidth, scale);
      *outHeight = scaleAxis(srcHeight, scale);
      return true;
    }
  }
  return sizeFailure(errorOut, outWidth, outHeight,
                     "resize refused: unrecognised resize mode (internal error).");
}

// --- What this build will offer ------------------------------------------

std::string exportRequestAvailability(const ExportRequest& request) {
  // No document, so no primaries and no pixels: exactly the two checks
  // exportRefusalReason() makes optional. What is left -- can this build
  // write this format, at this depth -- is the whole question a preset
  // answers on its own.
  return exportRefusalReason(request.format, request.targetSpace, request.bitDepth, nullptr,
                             nullptr);
}

std::vector<ImageFormat> offerableExportFormats() {
  std::vector<ImageFormat> out;
  for (const FormatCapability& c : allFormatCapabilities())
    if (c.canWrite) out.push_back(c.format);
  return out;
}

std::vector<ExportBitDepth> offerableExportDepths(ImageFormat format) {
  std::vector<ExportBitDepth> out;
  const FormatCapability& c = formatCapability(format);
  for (std::size_t i = 0; i < kExportBitDepthCount; ++i) {
    const auto d = static_cast<ExportBitDepth>(i);
    if (c.canWriteDepth(d)) out.push_back(d);
  }
  return out;
}

// --- Validation -----------------------------------------------------------

ExportValidation validateExportRequest(const ExportRequest& request, uint32_t srcWidth,
                                       uint32_t srcHeight, const WorkingSpace* sourceSpace,
                                       const DecodedImage* img) {
  ExportValidation v;

  // 1. Everything io/Export would refuse, in io/Export's own words. Asked
  // first because a format this build cannot write makes every other
  // question moot.
  const std::string refusal =
      exportRefusalReason(request.format, request.targetSpace, request.bitDepth, sourceSpace, img);
  if (!refusal.empty()) {
    v.error = refusal;
    return v;
  }

  // 2. The resize, in ops/Resample's and resolveExportSize's own words.
  std::string sizeError;
  if (!resolveExportSize(request.resize, srcWidth, srcHeight, &v.outWidth, &v.outHeight,
                         &sizeError)) {
    v.error = sizeError;
    return v;
  }

  v.ok = true;

  // 3. PRD I11's warnings: the losses that are legal, each named with a
  // number rather than an adjective.
  char buf[512];

  if (v.outWidth != srcWidth || v.outHeight != srcHeight) {
    const double srcPx = static_cast<double>(srcWidth) * static_cast<double>(srcHeight);
    const double dstPx = static_cast<double>(v.outWidth) * static_cast<double>(v.outHeight);
    std::snprintf(buf, sizeof(buf),
                  "Resize %ux%u -> %ux%u discards %.1f%% of the pixels (%.0f of %.0f). The "
                  "exported file is a downsample, prefiltered by an area average in linear "
                  "light; the document itself is unchanged.",
                  srcWidth, srcHeight, v.outWidth, v.outHeight,
                  srcPx > 0.0 ? (1.0 - dstPx / srcPx) * 100.0 : 0.0, srcPx - dstPx, srcPx);
    v.warnings.emplace_back(buf);
  }

  if (request.bitDepth == ExportBitDepth::UInt8) {
    std::snprintf(buf, sizeof(buf),
                  "Bit depth 8-bit integer stores 256 levels per channel. The working space is "
                  "16-bit half float (core/Half.hpp), so this export quantises; %s.",
                  formatCapability(request.format).canWriteDepth(ExportBitDepth::UInt16)
                      ? "16-bit integer in this same format keeps 65536"
                      : "a format that can carry 16 bits would keep 65536");
    v.warnings.emplace_back(buf);
  }

  if (request.targetSpace == ExportTargetSpace::Rec709Linear &&
      !exportBitDepthIsFloat(request.bitDepth)) {
    // Derived at run time from color/Space's own curve rather than quoted
    // from memory: the darkest code of an sRGB-encoded file covers
    // srgbDecode(1/max) of linear range, while the darkest code of a
    // *linear* file of the same depth covers 1/max. The ratio is the sRGB
    // curve's near-black slope.
    const float maxCode = request.bitDepth == ExportBitDepth::UInt8 ? 255.0f : 65535.0f;
    const float linearStep = 1.0f / maxCode;
    const float srgbStep = srgbDecode(linearStep);
    std::snprintf(buf, sizeof(buf),
                  "Colour space Rec709Linear at an integer depth bands in the shadows: the "
                  "darkest code covers %.3e of linear range against %.3e for the same depth in "
                  "Rec709Srgb -- %.1fx coarser near black. Correct if something downstream "
                  "applies its own curve; wrong if this file is going to be looked at.",
                  static_cast<double>(linearStep), static_cast<double>(srgbStep),
                  srgbStep > 0.0f ? static_cast<double>(linearStep / srgbStep) : 0.0);
    v.warnings.emplace_back(buf);
  }

  if (img != nullptr && img->valid() && !exportBitDepthIsFloat(request.bitDepth)) {
    // Measured on the *pre-resize* image, which is conservative in the
    // right direction: an area average can only pull a value towards its
    // neighbours, so a downscale never raises the maximum. A highlight
    // named here might survive the downscale below 1.0; one not named here
    // cannot appear.
    float maxLinear = 0.0f;
    uint32_t hx = 0, hy = 0;
    for (uint32_t y = 0; y < img->height; ++y) {
      for (uint32_t x = 0; x < img->width; ++x) {
        const float* p = &img->pixels[(static_cast<size_t>(y) * img->width + x) * 4];
        for (int c = 0; c < 3; ++c) {
          if (p[c] > maxLinear) {
            maxLinear = p[c];
            hx = x;
            hy = y;
          }
        }
      }
    }
    if (maxLinear > 1.0f) {
      std::snprintf(buf, sizeof(buf),
                    "Highlights: the flattened document reaches %.4f in linear light (first at "
                    "x=%u, y=%u), and an integer depth has no representation above full scale, "
                    "so everything above 1.0 clips to white. Export at half or 32-bit float to "
                    "keep it.",
                    static_cast<double>(maxLinear), hx, hy);
      v.warnings.emplace_back(buf);
    }
  }

  if (!formatCapability(request.format).hasAlpha) {
    std::snprintf(buf, sizeof(buf),
                  "%s has no alpha channel. Nothing is lost here -- a document that was not "
                  "fully opaque would have been refused outright -- but transparency added "
                  "later will not survive this preset.",
                  imageFormatName(request.format));
    v.warnings.emplace_back(buf);
  }

  if (request.format == ImageFormat::Jpeg) {
    v.warnings.emplace_back(
        "JPEG is lossy: the pixels read back will not be the pixels written, at any bit depth. "
        "io/Export writes at quality 95 (its kJpegQuality), which is not adjustable yet.");
  }

  return v;
}

// --- Presets --------------------------------------------------------------

bool ExportPresetStore::loadFromString(std::string_view json, std::string_view sourceLabel) {
  presets_.clear();
  problems_.clear();
  error_.clear();

  JsonReader r(json, sourceLabel);
  if (!r.expect('{')) {
    error_ = r.error();
    return false;
  }

  std::vector<ExportPreset> loaded;
  bool sawPresets = false;

  if (r.peek() != '}') {
    for (;;) {
      std::string key;
      if (!r.parseString(&key) || !r.expect(':')) break;

      if (key == "presets") {
        sawPresets = true;
        if (!r.expect('[')) break;
        if (r.peek() == ']') {
          r.consume();
        } else {
          for (;;) {
            if (!r.expect('{')) break;
            std::string name;
            std::string formatToken, spaceToken, depthToken, resizeToken;
            ExportRequest req;
            bool objectOk = true;
            if (r.peek() != '}') {
              for (;;) {
                std::string field;
                if (!r.parseString(&field) || !r.expect(':')) {
                  objectOk = false;
                  break;
                }
                double number = 0.0;
                if (field == "name") {
                  if (!r.parseString(&name)) objectOk = false;
                } else if (field == "format") {
                  if (!r.parseString(&formatToken)) objectOk = false;
                } else if (field == "space") {
                  if (!r.parseString(&spaceToken)) objectOk = false;
                } else if (field == "depth") {
                  if (!r.parseString(&depthToken)) objectOk = false;
                } else if (field == "resize") {
                  if (!r.parseString(&resizeToken)) objectOk = false;
                } else if (field == "percent") {
                  if (r.parseNumber(&number))
                    req.resize.percent = static_cast<float>(number);
                  else
                    objectOk = false;
                } else if (field == "maxWidth") {
                  if (r.parseNumber(&number))
                    req.resize.maxWidth = number > 0.0 ? static_cast<uint32_t>(number) : 0u;
                  else
                    objectOk = false;
                } else if (field == "maxHeight") {
                  if (r.parseNumber(&number))
                    req.resize.maxHeight = number > 0.0 ? static_cast<uint32_t>(number) : 0u;
                  else
                    objectOk = false;
                } else if (!r.skipValue()) {
                  objectOk = false;
                }
                if (!objectOk) break;
                if (r.peek() == ',') {
                  r.consume();
                  continue;
                }
                break;
              }
            }
            if (!objectOk || !r.expect('}')) {
              objectOk = false;
              break;
            }

            // Field-level problems skip *this* preset and keep the rest --
            // see ExportAs.hpp on why an unrecognised token is not a broken
            // file, and what it costs.
            std::string problem = presetNameProblem(name);
            if (problem.empty() && !exportFormatFromToken(formatToken, &req.format))
              problem = "preset '" + name + "' skipped: unrecognised format token '" +
                        formatToken + "'.";
            if (problem.empty() && !exportTargetSpaceFromToken(spaceToken, &req.targetSpace))
              problem = "preset '" + name + "' skipped: unrecognised colour space token '" +
                        spaceToken + "'.";
            if (problem.empty() && !exportBitDepthFromToken(depthToken, &req.bitDepth))
              problem =
                  "preset '" + name + "' skipped: unrecognised bit depth token '" + depthToken +
                  "'.";
            if (problem.empty() && !exportResizeModeFromToken(resizeToken, &req.resize.mode))
              problem = "preset '" + name + "' skipped: unrecognised resize mode token '" +
                        resizeToken + "'.";
            if (problem.empty()) {
              const std::string trimmedName = trimmed(name);
              for (const ExportPreset& existing : loaded) {
                if (asciiEqualNoCase(existing.name, trimmedName)) {
                  error_ = std::string(sourceLabel) + ": two presets are both named '" +
                           trimmedName +
                           "' (names are compared without regard to case). A preset menu with "
                           "two identical entries cannot be used; rename one.";
                  presets_.clear();
                  problems_.clear();
                  return false;
                }
              }
              ExportPreset p;
              p.name = trimmedName;
              p.request = req;
              loaded.push_back(std::move(p));
            } else {
              problems_.push_back(std::move(problem));
            }

            if (r.peek() == ',') {
              r.consume();
              continue;
            }
            break;
          }
          if (!r.failed() && !r.expect(']')) break;
        }
      } else if (!r.skipValue()) {
        break;
      }

      if (r.failed()) break;
      if (r.peek() == ',') {
        r.consume();
        continue;
      }
      break;
    }
  }

  if (!r.failed()) r.expect('}');
  if (r.failed()) {
    error_ = r.error();
    presets_.clear();
    problems_.clear();
    return false;
  }
  if (!sawPresets) {
    error_ = std::string(sourceLabel) +
             ": no \"presets\" array -- this is not an export preset file. Nothing was loaded.";
    problems_.clear();
    return false;
  }

  presets_ = std::move(loaded);
  return true;
}

std::string ExportPresetStore::serialize() const {
  std::ostringstream out;
  out << "{\n  \"version\": 1,\n  \"presets\": [\n";
  for (size_t i = 0; i < presets_.size(); ++i) {
    const ExportPreset& p = presets_[i];
    char numbers[128];
    std::snprintf(numbers, sizeof(numbers), "%.6g", static_cast<double>(p.request.resize.percent));
    out << "    {\n"
        << "      \"name\": \"" << escapeJson(p.name) << "\",\n"
        << "      \"format\": \"" << exportFormatToken(p.request.format) << "\",\n"
        << "      \"space\": \"" << exportTargetSpaceToken(p.request.targetSpace) << "\",\n"
        << "      \"depth\": \"" << exportBitDepthToken(p.request.bitDepth) << "\",\n"
        << "      \"resize\": \"" << exportResizeModeToken(p.request.resize.mode) << "\",\n"
        << "      \"percent\": " << numbers << ",\n"
        << "      \"maxWidth\": " << p.request.resize.maxWidth << ",\n"
        << "      \"maxHeight\": " << p.request.resize.maxHeight << "\n"
        << (i + 1 < presets_.size() ? "    },\n" : "    }\n");
  }
  out << "  ]\n}\n";
  return out.str();
}

bool ExportPresetStore::loadFromFile(const std::string& path) {
  presets_.clear();
  problems_.clear();
  error_.clear();

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    // Not an error: a user who has never saved a preset. Returning false
    // here would make every first run look like a failure.
    return true;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error_ = "export presets: '" + path + "' exists but could not be opened for reading.";
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return loadFromString(buffer.str(), path);
}

bool ExportPresetStore::saveToFile(const std::string& path, std::string* errorOut) const {
  if (errorOut) errorOut->clear();
  const std::filesystem::path p(path);
  std::error_code ec;
  if (p.has_parent_path() && !p.parent_path().empty()) {
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec && !std::filesystem::exists(p.parent_path())) {
      if (errorOut)
        *errorOut = "export presets: could not create the directory '" +
                    p.parent_path().string() + "' (" + ec.message() + ").";
      return false;
    }
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (errorOut) *errorOut = "export presets: could not open '" + path + "' for writing.";
    return false;
  }
  const std::string text = serialize();
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.close();
  if (!out) {
    if (errorOut) *errorOut = "export presets: '" + path + "' was opened but not fully written.";
    return false;
  }
  return true;
}

bool ExportPresetStore::savePreset(const ExportPreset& preset, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  const std::string problem = presetNameProblem(preset.name);
  if (!problem.empty()) {
    if (errorOut) *errorOut = problem;
    return false;
  }
  const std::string name = trimmed(preset.name);
  for (ExportPreset& existing : presets_) {
    if (asciiEqualNoCase(existing.name, name)) {
      existing.name = name;  // adopt the caller's capitalisation
      existing.request = preset.request;
      return true;
    }
  }
  ExportPreset stored;
  stored.name = name;
  stored.request = preset.request;
  presets_.push_back(std::move(stored));
  return true;
}

bool ExportPresetStore::removePreset(std::string_view name) {
  const std::string wanted = trimmed(name);
  for (size_t i = 0; i < presets_.size(); ++i) {
    if (asciiEqualNoCase(presets_[i].name, wanted)) {
      presets_.erase(presets_.begin() + static_cast<std::ptrdiff_t>(i));
      return true;
    }
  }
  return false;
}

const ExportPreset* ExportPresetStore::find(std::string_view name) const {
  const std::string wanted = trimmed(name);
  for (const ExportPreset& p : presets_)
    if (asciiEqualNoCase(p.name, wanted)) return &p;
  return nullptr;
}

std::string defaultExportPresetsPath() {
  // An explicit override first, so a test (or a second profile) never has to
  // touch the real one.
  if (const char* explicitPath = std::getenv("NP_EXPORT_PRESETS")) {
    if (*explicitPath != '\0') return explicitPath;
  }
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  if (home && *home)
    return std::string(home) + "/Library/Application Support/naturalPaint/export-presets.json";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
    if (*xdg != '\0') return std::string(xdg) + "/naturalPaint/export-presets.json";
  }
  if (home && *home) return std::string(home) + "/.config/naturalPaint/export-presets.json";
#endif
  // No HOME at all (a stripped environment). The working directory is a poor
  // place for user settings, but it is a real, writable one, and silently
  // returning an empty path would turn "save preset" into a mystery.
  return "export-presets.json";
}

// --- The composed operation ----------------------------------------------

ExportResult exportDocumentWithRequest(const Document& doc, const ExportRequest& request) {
  DecodedImage flat = flattenDocumentToLinear(doc);
  if (!flat.valid()) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "export refused: the document has no pixels to export (canvas is %dx%d).",
                  doc.width, doc.height);
    return exportFailure(buf);
  }

  uint32_t outWidth = 0, outHeight = 0;
  std::string sizeError;
  if (!resolveExportSize(request.resize, flat.width, flat.height, &outWidth, &outHeight,
                         &sizeError)) {
    return exportFailure(std::move(sizeError));
  }

  if (outWidth != flat.width || outHeight != flat.height) {
    // Here, between the flatten and the encode, is what makes the resize a
    // linear-light operation structurally rather than by convention:
    // flattenDocumentToLinear()'s output is linear by contract and
    // encodeLinearImage() has not applied a transfer function yet. See
    // ops/Resample.hpp.
    std::vector<float> resized;
    std::string resizeError;
    if (!resampleAreaAverage(flat.pixels.data(), flat.width, flat.height, outWidth, outHeight,
                             &resized, &resizeError)) {
      return exportFailure(std::move(resizeError));
    }
    flat.pixels = std::move(resized);
    flat.width = outWidth;
    flat.height = outHeight;
  }

  return encodeLinearImage(flat, doc.workingSpace, request.format, request.targetSpace,
                           request.bitDepth);
}

bool exportDocumentWithRequestToFile(const Document& doc, const std::string& path,
                                     const ExportRequest& request, std::string* errorOut) {
  const ExportResult encoded = exportDocumentWithRequest(doc, request);
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
    if (errorOut) *errorOut = "export failed: '" + path + "' was opened but not fully written.";
    return false;
  }
  return true;
}

}  // namespace np
