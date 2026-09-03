#include "io/ImageDecode.hpp"

#include "color/Gamut.hpp"
#include "color/Space.hpp"
#include "io/SourceGamut.hpp"
#include "stb_image.h"

#include "io/OiioBackend.hpp"

// NOTE on STB_IMAGE_IMPLEMENTATION: paint/Palette.cpp is the one translation
// unit that defines STB_IMAGE_IMPLEMENTATION (it needed the Mixbox LUT PNG
// before this file existed) -- that macro may only be defined once across the
// whole binary, since it's what makes stb_image.h emit actual function
// bodies rather than just declarations. This file includes stb_image.h with
// no implementation macro and links against the bodies Palette.cpp already
// compiled in. Palette.cpp also had to widen its STBI_ONLY_PNG restriction
// to cover JPEG/BMP/TGA too (it was previously PNG-only, which preprocessed
// every other decoder's code out of the shared implementation) -- see the
// comment there.
namespace np {
namespace {

// --- Colour-encoding assumption ----------------------------------------
//
// PNG/JPEG/TGA/BMP carry no colour-management metadata that stb_image
// surfaces here (no ICC profile, no gAMA-chunk handling) -- so there is no
// signal in the file itself to decode against. The universal convention for
// an untagged 8-/16-bit image file from a web browser, phone camera, or
// desktop screenshot tool is that it is sRGB-encoded, and this codebase's
// one working space today (kRec709Primaries, color/Space.hpp) already shares
// sRGB's primaries and white point (D65) -- so decoding with the sRGB
// transfer function is not just a convenient default, it is the choice
// consistent with the working space that actually exists.
//
// **Amended: the PRIMARIES half of this is no longer an assumption.** The
// paragraph above used to end "left for whoever adds colour-profile-aware
// import later", and the gap it named was not a small one: macOS -- the only
// platform this ships on -- writes **Display P3** by default from the camera,
// the screenshot key and the display, so the most common file a user drags in
// was decoded as though its primaries were Rec.709. A saturated P3 red is
// misplaced by more than 20% that way, silently, and irreversibly once
// painted over.
//
// `io/SourceGamut` now reads the file's own gamut out of the container (a
// PNG `iCCP`/`sRGB`/`cHRM` chunk, a JPEG `APP2` ICC profile) and
// `applySourceGamut()` below converts the decoded linear RGB into the
// working space. stb_image still exposes none of this -- that is precisely
// why the gamut is read from the bytes rather than from the decoder; see
// io/SourceGamut.hpp.
//
// **What remains an assumption is the transfer function**, and it is stated
// here with its size rather than left open: an Adobe RGB (1998) file is
// gamma 2.2, not sRGB's curve, so its tones are still slightly off after its
// primaries have been corrected. The two curves differ by under 1% across
// most of the range, against the >20% gamut error above, and closing it needs
// a per-file transfer model this codebase does not have. A file whose gamut
// is unreadable -- a look-up-table ICC profile, an untagged JPEG -- falls
// back to exactly the behaviour described above, so this can only match or
// improve on the old baseline, never do worse.
inline float decodeChannelToLinear(float encoded01) { return srgbDecode(encoded01); }

// Converts `img`'s linear RGB from whatever gamut `data` declares into the
// working space, in place. A no-op -- not merely a cheap one, but literally
// zero passes over the pixels -- when the file declares nothing, declares
// something unusable, or declares a gamut that is already Rec.709.
//
// **Alpha is not touched.** It is opacity, not light: the same rule
// io/ImageDecode.hpp and core/Probe.hpp already state for the transfer
// function applies unchanged to a gamut conversion.
//
// **Run on straight (un-premultiplied) alpha**, which is what a DecodedImage
// holds by that struct's own contract -- io/ImageIO premultiplies later. That
// ordering matters: a matrix applied to premultiplied colour is only
// equivalent because the matrix is linear and alpha is a scalar, but the
// negative out-of-gamut values this produces would then be scaled by alpha
// and the two operations would have to be reasoned about together. Doing it
// here, before premultiply, keeps each step's contract intact.
void applySourceGamut(DecodedImage& img, const uint8_t* data, size_t size) {
  if (!img.valid()) return;
  const SourceGamut gamut = sniffSourceGamut(data, size);
  if (!gamut.known) return;

  const std::optional<ColorMat3> convert =
      gamutConversion(gamut.rgbToXyzD50, rec709RgbToXyzD50());
  if (!convert) return;
  // An sRGB/Rec.709 file -- still the overwhelming majority -- lands here and
  // pays nothing beyond the chunk walk.
  //
  // **1e-3, chosen against the storage and not against the arithmetic.** The
  // tiles are `HALF`, whose relative precision near 1.0 is about 1e-3, so a
  // matrix closer to the identity than that cannot change a stored texel:
  // applying it is provably a no-op and skipping it is not an approximation.
  //
  // That threshold is also what the common case needs. A PNG carrying an
  // **sRGB ICC profile** -- the most frequent tagged file there is -- gives a
  // conversion that is the identity to about 3e-4, not to 1e-4: the profile's
  // colorants come from the CIE-tabulated D65 white while `kRec709Primaries`
  // holds xy to four places, and app/selftest/Gamut.cpp section 1 measures
  // that same ~2e-4 gap against the published matrices. At a 1e-4 tolerance
  // those files fell through to the full pixel pass and came out at
  // (1.00003, 0.00001, 0.00001) instead of (1, 0, 0) -- harmless in `HALF`,
  // but a per-pixel pass bought nothing, and "an sRGB file is bit-identical
  // to an untagged one" is a much easier property to keep than "differs by
  // less than a rounding step". Nothing real sits between the two
  // tolerances: the nearest gamut this build names differs from Rec.709 by
  // 0.22 in the red term, three orders of magnitude away.
  if (colorMat3NearIdentity(*convert, 1e-3f)) return;

  const size_t texels = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
  for (size_t i = 0; i < texels; ++i) {
    float* px = &img.pixels[i * 4];
    const std::array<float, 3> out = colorMat3Apply(*convert, {px[0], px[1], px[2]});
    px[0] = out[0];
    px[1] = out[1];
    px[2] = out[2];
  }
}

DecodedImage decodeFromMemoryImpl(const uint8_t* data, size_t size, std::string* errorOut) {
  DecodedImage out;
  if (!data || size == 0) {
    if (errorOut) *errorOut = "empty input";
    return out;
  }

  int w = 0, h = 0, srcChannels = 0;
  const bool is16 = stbi_is_16_bit_from_memory(data, static_cast<int>(size)) != 0;

  // Always request 4 channels (RGBA): stb_image fills a synthesized alpha
  // channel as fully opaque (255 / 65535) for sources that don't have one,
  // so nothing below needs its own "no alpha" branch.
  if (is16) {
    stbi_us* px =
        stbi_load_16_from_memory(data, static_cast<int>(size), &w, &h, &srcChannels, 4);
    if (!px) {
      if (errorOut) *errorOut = stbi_failure_reason();
      return out;
    }
    out.width = static_cast<uint32_t>(w);
    out.height = static_cast<uint32_t>(h);
    out.pixels.resize(static_cast<size_t>(w) * h * 4);
    constexpr float kMax = 65535.0f;
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
      const stbi_us* s = px + i * 4;
      float* d = out.pixels.data() + i * 4;
      d[0] = decodeChannelToLinear(s[0] / kMax);
      d[1] = decodeChannelToLinear(s[1] / kMax);
      d[2] = decodeChannelToLinear(s[2] / kMax);
      d[3] = s[3] / kMax;  // alpha: linear opacity, never sRGB-encoded
    }
    stbi_image_free(px);
  } else {
    unsigned char* px =
        stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &srcChannels, 4);
    if (!px) {
      if (errorOut) *errorOut = stbi_failure_reason();
      return out;
    }
    out.width = static_cast<uint32_t>(w);
    out.height = static_cast<uint32_t>(h);
    out.pixels.resize(static_cast<size_t>(w) * h * 4);
    constexpr float kMax = 255.0f;
    for (size_t i = 0, n = static_cast<size_t>(w) * h; i < n; ++i) {
      const unsigned char* s = px + i * 4;
      float* d = out.pixels.data() + i * 4;
      d[0] = decodeChannelToLinear(s[0] / kMax);
      d[1] = decodeChannelToLinear(s[1] / kMax);
      d[2] = decodeChannelToLinear(s[2] / kMax);
      d[3] = s[3] / kMax;
    }
    stbi_image_free(px);
  }

  return out;
}

}  // namespace

DecodedImage decodeImageLinear(const uint8_t* fileData, size_t fileSize, std::string* errorOut) {
  // stb first, always. PRD I1's four formats therefore decode through
  // exactly the same code regardless of what OpenImageIO does -- OpenImageIO
  // is a *fallback*, never an interception, so it cannot change what an
  // already-supported file decodes to. (io/Capabilities.cpp's
  // kStbCapabilities comment makes the same argument for the write side.)
  DecodedImage viaStb = decodeFromMemoryImpl(fileData, fileSize, errorOut);
  if (viaStb.valid()) {
    // The gamut comes from the container, not from stb -- which is the whole
    // reason it can be applied to the stb path at all. See io/SourceGamut.hpp.
    applySourceGamut(viaStb, fileData, fileSize);
    return viaStb;
  }

  // PLAN.md Phase 4 step 2's read half. Everything stb declines and the
  // linked OpenImageIO accepts -- EXR, TIFF, HDR, DPX, flattened PSD --
  // arrives here, which is why openImageAsDocument() and placeImageAsLayer()
  // gained those formats without a new entry point or a call-site change:
  // they already call this function. (PLAN.md step 4's "PSD *read* arrives
  // free here too" is this, one step earlier than the plan expected.)
  std::string oiioError;
  DecodedImage viaOiio = oiioDecodeToLinear(fileData, fileSize, &oiioError);
  if (viaOiio.valid()) {
    if (errorOut) errorOut->clear();
    // The same call on the fallback path, for the same reason and with the
    // same bytes: one gamut answer for both decoders, so a file's colour
    // cannot depend on which one happened to accept it. In practice this
    // finds nothing today -- the formats that reach OpenImageIO are the ones
    // stb declined, and neither this module's PNG nor its JPEG reader applies
    // to an EXR or a DPX -- but wiring it here rather than only on the stb
    // path is what keeps that true if the routing ever changes.
    applySourceGamut(viaOiio, fileData, fileSize);
    return viaOiio;
  }
  // Both declined: report both reasons, not just stb's, so "why won't this
  // open" is answerable from the message alone.
  if (errorOut) *errorOut += " (OpenImageIO also declined it: " + oiioError + ")";

  return viaStb;
}

}  // namespace np
