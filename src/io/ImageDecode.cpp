#include "io/ImageDecode.hpp"

#include "color/Space.hpp"
#include "stb_image.h"

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
// This is a deliberate, documented assumption, not a guarantee about every
// file this will ever open: a JPEG tagged Adobe RGB, or a PNG carrying an
// embedded ICC profile with a different curve, would decode wrong under it.
// Handling that means reading and adapting from whatever profile the file
// actually carries -- stb_image doesn't expose ICC or gAMA chunks at all, so
// it's out of scope here -- and is left for whoever adds colour-profile-
// aware import later. Written down here so that work starts from a decision
// someone can go verify or override, not from reverse-engineering "why does
// this image look slightly off" purely from decode output.
inline float decodeChannelToLinear(float encoded01) { return srgbDecode(encoded01); }

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
  return decodeFromMemoryImpl(fileData, fileSize, errorOut);
}

}  // namespace np
