#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// PLAN.md "Phase 2 -- See a file", step 6 reads: "io/ImageIO + io/StbBackend
// -- read PNG/JPEG/TGA/BMP, decode to linear rgba16float, write into tiles."
// That full step depends on core/TileStore and core/Document/Layer, which
// don't exist yet (a separate track is building TileStore concurrently).
//
// This header is deliberately just the decode half of that step: file bytes
// in, linear-light float RGBA pixels out, nothing more. It does NOT pack to
// half float, does NOT premultiply alpha, and does NOT know what a tile is.
// That remaining work -- pack/premultiply/tile-write -- is io/ImageIO's job
// once TileStore/Document exist to receive it; this module is meant to be a
// building block that step calls into, not a preview of it.
namespace np {

// A decoded image: linear-light float RGBA, straight (non-premultiplied)
// alpha, row-major top-to-bottom, no row padding.
struct DecodedImage {
  uint32_t width = 0;
  uint32_t height = 0;
  // width * height * 4 floats: R, G, B, A per pixel.
  std::vector<float> pixels;

  bool valid() const {
    return width > 0 && height > 0 &&
           pixels.size() == static_cast<size_t>(width) * height * 4;
  }
};

// Decodes PNG/JPEG/TGA/BMP bytes already in memory to linear-light float
// RGBA. Source bit depth (8- or 16-bit) is auto-detected. RGB channels are
// assumed sRGB-encoded and decoded to linear via color::srgbDecode -- see
// the long comment above decodeChannelToLinear() in ImageDecode.cpp for why
// that assumption is the right one for now and where it stops being valid.
// Alpha, if the source has one, passes through unencoded (alpha is opacity,
// not light -- it is never gamma-encoded); sources without an alpha channel
// decode as fully opaque (alpha = 1.0), matching stb_image's own fill
// behaviour when a 4th channel is requested from a 3-channel source.
//
// PLAN.md Phase 4 step 2 adds a *fallback*, not an interception: when
// stb_image declines the bytes and this binary was built with NP_USE_OIIO,
// io/OiioBackend gets a second attempt, which is what makes EXR, TIFF, HDR,
// DPX and flattened PSD open through this same function (and therefore
// through io/ImageIO's openImageAsDocument()/placeImageAsLayer(), unchanged).
// The order matters and is deliberate: PNG/JPEG/TGA/BMP always take the stb
// path first, in both build configurations, so PRD I1's four formats decode
// identically whether or not OpenImageIO is linked in.
//
// Returns a DecodedImage with width == 0 (valid() == false) on failure. If
// `errorOut` is non-null, it receives a short description of what went
// wrong (from stb_image's stbi_failure_reason(), plus OpenImageIO's own
// reason when that backend is present and also declined).
DecodedImage decodeImageLinear(const uint8_t* fileData, size_t fileSize,
                                std::string* errorOut = nullptr);

}  // namespace np
