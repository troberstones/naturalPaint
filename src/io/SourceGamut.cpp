#include "io/SourceGamut.hpp"

#include <cstring>

#include "stb_image.h"

namespace np {
namespace {

// Every read below goes through these. The input is a file the user was handed
// by someone else, parsed before anything has validated it, so "the chunk
// length said 4 GB" and "the tag offset points past the end" are inputs to
// handle, not conditions to assume away.
bool readU32Be(const uint8_t* data, size_t size, size_t at, uint32_t* out) {
  if (at + 4 > size) return false;
  *out = (static_cast<uint32_t>(data[at]) << 24) | (static_cast<uint32_t>(data[at + 1]) << 16) |
         (static_cast<uint32_t>(data[at + 2]) << 8) | static_cast<uint32_t>(data[at + 3]);
  return true;
}

bool readU16Be(const uint8_t* data, size_t size, size_t at, uint16_t* out) {
  if (at + 2 > size) return false;
  *out = static_cast<uint16_t>((static_cast<uint32_t>(data[at]) << 8) | data[at + 1]);
  return true;
}

bool tagIs(const uint8_t* data, size_t size, size_t at, const char (&sig)[5]) {
  return at + 4 <= size && std::memcmp(data + at, sig, 4) == 0;
}

// s15Fixed16Number -> float. ICC's XYZ tags store three of these.
float s15Fixed16(uint32_t raw) {
  return static_cast<float>(static_cast<int32_t>(raw)) / 65536.0f;
}

constexpr uint8_t kPngSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};

bool looksLikePng(const uint8_t* data, size_t size) {
  return size >= 8 && std::memcmp(data, kPngSignature, 8) == 0;
}

bool looksLikeJpeg(const uint8_t* data, size_t size) {
  return size >= 2 && data[0] == 0xFF && data[1] == 0xD8;
}

// --- PNG -------------------------------------------------------------------

SourceGamut pngGamut(const uint8_t* data, size_t size) {
  SourceGamut best;
  std::optional<ColorMat3> fromChrm;
  bool sawSrgbChunk = false;

  size_t at = 8;  // past the signature
  // Bounded by the data itself: every iteration either advances past a
  // well-formed chunk header or stops. A length that would overflow `at` or
  // run past the end ends the walk rather than wrapping.
  while (at + 8 <= size) {
    uint32_t length = 0;
    if (!readU32Be(data, size, at, &length)) break;
    const size_t typeAt = at + 4;
    const size_t dataAt = at + 8;
    if (length > size || dataAt + length > size) break;

    if (tagIs(data, size, typeAt, "IDAT") || tagIs(data, size, typeAt, "IEND")) {
      // Colour chunks are all required to precede the image data, so there is
      // nothing left to find. Stopping here also means a large PNG is not
      // walked chunk by chunk to its end for no reason.
      break;
    }

    if (tagIs(data, size, typeAt, "iCCP")) {
      // Profile name (1-79 bytes), a NUL, then one compression-method byte,
      // then the deflated profile.
      size_t nul = dataAt;
      const size_t end = dataAt + length;
      while (nul < end && data[nul] != 0) ++nul;
      if (nul + 2 <= end) {
        const uint8_t method = data[nul + 1];
        const size_t zAt = nul + 2;
        const size_t zLen = end - zAt;
        if (method == 0 && zLen > 0) {
          // stb_image's own zlib decoder -- already linked, already used for
          // every PNG this build opens, so this costs no dependency. It
          // allocates; `stbi_image_free` is the matching release.
          int outLen = 0;
          char* inflated = stbi_zlib_decode_malloc(reinterpret_cast<const char*>(data + zAt),
                                                   static_cast<int>(zLen), &outLen);
          if (inflated != nullptr) {
            if (outLen > 0) {
              const std::optional<ColorMat3> m = iccRgbToXyzD50(
                  reinterpret_cast<const uint8_t*>(inflated), static_cast<size_t>(outLen));
              if (m) {
                best.known = true;
                best.rgbToXyzD50 = *m;
                best.source = "PNG iCCP";
              }
            }
            stbi_image_free(inflated);
          }
        }
      }
      // An embedded profile is the most specific statement a PNG can make, so
      // once one has been understood nothing later overrides it.
      if (best.known) return best;
    } else if (tagIs(data, size, typeAt, "sRGB")) {
      sawSrgbChunk = true;
    } else if (tagIs(data, size, typeAt, "cHRM") && length >= 32) {
      // Eight big-endian u32s, each 100000x the coordinate, in the order
      // white x/y, red x/y, green x/y, blue x/y.
      uint32_t v[8] = {};
      bool okAll = true;
      for (int i = 0; i < 8; ++i)
        okAll = okAll && readU32Be(data, size, dataAt + static_cast<size_t>(i) * 4, &v[i]);
      if (okAll) {
        Primaries p;
        p.whiteX = static_cast<float>(v[0]) / 100000.0f;
        p.whiteY = static_cast<float>(v[1]) / 100000.0f;
        p.redX = static_cast<float>(v[2]) / 100000.0f;
        p.redY = static_cast<float>(v[3]) / 100000.0f;
        p.greenX = static_cast<float>(v[4]) / 100000.0f;
        p.greenY = static_cast<float>(v[5]) / 100000.0f;
        p.blueX = static_cast<float>(v[6]) / 100000.0f;
        p.blueY = static_cast<float>(v[7]) / 100000.0f;
        fromChrm = rgbToXyzD50(p);
      }
    }

    // length + 12 = 4 (length) + 4 (type) + length + 4 (CRC).
    at = dataAt + length + 4;
  }

  // An explicit `sRGB` chunk beats `cHRM`, per the PNG specification's own
  // precedence. Both are "no conversion needed" for an sRGB file, but saying
  // which chunk answered keeps the two distinguishable in diagnostics.
  if (sawSrgbChunk) {
    best.known = true;
    best.rgbToXyzD50 = rec709RgbToXyzD50();
    best.source = "PNG sRGB";
    return best;
  }
  if (fromChrm) {
    best.known = true;
    best.rgbToXyzD50 = *fromChrm;
    best.source = "PNG cHRM";
  }
  return best;
}

}  // namespace

std::vector<uint8_t> jpegIccProfile(const uint8_t* data, size_t size) {
  static constexpr char kMarker[] = "ICC_PROFILE";
  // Segment payloads, indexed by their 1-based sequence number. A profile may
  // be split across up to 255 APP2 segments, and they are not required to
  // appear in order -- so they are collected and then concatenated by
  // sequence, rather than appended as encountered.
  std::vector<std::vector<uint8_t>> chunks(256);
  size_t highest = 0;

  size_t at = 2;  // past SOI
  while (at + 4 <= size) {
    if (data[at] != 0xFF) {
      // Not at a marker. A well-formed JPEG never lands here before SOS; a
      // malformed one is abandoned rather than resynchronised, since the goal
      // is a colour hint and not a decoder.
      break;
    }
    const uint8_t marker = data[at + 1];
    // Standalone markers carry no length.
    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
      at += 2;
      continue;
    }
    // Start of scan: the entropy-coded data follows and there are no more
    // metadata segments worth walking.
    if (marker == 0xDA || marker == 0xD9) break;

    uint16_t segLen = 0;
    if (!readU16Be(data, size, at + 2, &segLen) || segLen < 2) break;
    const size_t payloadAt = at + 4;
    const size_t payloadLen = static_cast<size_t>(segLen) - 2;
    if (payloadAt + payloadLen > size) break;

    if (marker == 0xE2 && payloadLen >= 14 &&
        std::memcmp(data + payloadAt, kMarker, sizeof(kMarker)) == 0) {
      const uint8_t seq = data[payloadAt + 12];
      const size_t bodyAt = payloadAt + 14;
      const size_t bodyLen = payloadLen - 14;
      if (seq >= 1 && bodyLen > 0) {
        chunks[seq].assign(data + bodyAt, data + bodyAt + bodyLen);
        if (seq > highest) highest = seq;
      }
    }
    at = payloadAt + payloadLen;
  }

  std::vector<uint8_t> profile;
  for (size_t i = 1; i <= highest; ++i)
    profile.insert(profile.end(), chunks[i].begin(), chunks[i].end());
  return profile;
}

std::optional<ColorMat3> iccRgbToXyzD50(const uint8_t* icc, size_t size) {
  // 128-byte header, then a u32 tag count, then 12 bytes per tag entry.
  if (icc == nullptr || size < 132) return std::nullopt;

  // Header offset 16 is the data colour space. Only RGB profiles have the
  // colorant tags this reads; a CMYK or Gray profile is declined by signature
  // rather than by failing to find them, so the reason is the true one.
  if (!tagIs(icc, size, 16, "RGB ")) return std::nullopt;

  uint32_t tagCount = 0;
  if (!readU32Be(icc, size, 128, &tagCount)) return std::nullopt;
  // A hostile count would otherwise drive the loop bound below. 4096 is far
  // above any real profile's tag count and keeps the walk finite.
  if (tagCount == 0 || tagCount > 4096) return std::nullopt;
  if (132 + static_cast<size_t>(tagCount) * 12 > size) return std::nullopt;

  // The three colorant tags, in fixed order so the matrix columns cannot be
  // assembled in the wrong one.
  const char* wanted[3] = {"rXYZ", "gXYZ", "bXYZ"};
  std::array<std::array<float, 3>, 3> colorants{};
  bool found[3] = {false, false, false};

  for (uint32_t i = 0; i < tagCount; ++i) {
    const size_t entry = 132 + static_cast<size_t>(i) * 12;
    uint32_t offset = 0;
    uint32_t length = 0;
    if (!readU32Be(icc, size, entry + 4, &offset)) return std::nullopt;
    if (!readU32Be(icc, size, entry + 8, &length)) return std::nullopt;

    for (int w = 0; w < 3; ++w) {
      if (found[w]) continue;
      if (std::memcmp(icc + entry, wanted[w], 4) != 0) continue;
      // An XYZType element: 4-byte type signature "XYZ ", 4 reserved bytes,
      // then three s15Fixed16 numbers.
      if (length < 20 || offset > size || offset + 20 > size) continue;
      if (!tagIs(icc, size, offset, "XYZ ")) continue;
      uint32_t xyz[3] = {};
      bool okAll = true;
      for (int c = 0; c < 3; ++c)
        okAll = okAll && readU32Be(icc, size, offset + 8 + static_cast<size_t>(c) * 4, &xyz[c]);
      if (!okAll) continue;
      colorants[static_cast<size_t>(w)] = {s15Fixed16(xyz[0]), s15Fixed16(xyz[1]),
                                          s15Fixed16(xyz[2])};
      found[w] = true;
    }
  }

  // A profile missing any colorant is not a matrix-shaper profile -- an
  // `A2B0` look-up-table profile is the common case -- and this module's
  // contract is to decline rather than guess.
  if (!found[0] || !found[1] || !found[2]) return std::nullopt;

  // Columns are the colorants: column 0 is where pure red lands in XYZ(D50).
  ColorMat3 m;
  m.m = {colorants[0][0], colorants[1][0], colorants[2][0],
         colorants[0][1], colorants[1][1], colorants[2][1],
         colorants[0][2], colorants[1][2], colorants[2][2]};

  // A profile whose colorants are degenerate would produce a singular matrix
  // that `gamutConversion()` would later refuse anyway; refusing it here
  // means the caller's fallback is chosen on the honest ground of "this
  // profile is unusable" rather than deep in the arithmetic.
  if (!colorMat3Inverse(m)) return std::nullopt;
  return m;
}

SourceGamut sniffSourceGamut(const uint8_t* data, size_t size) {
  if (data == nullptr || size == 0) return {};
  if (looksLikePng(data, size)) return pngGamut(data, size);
  if (looksLikeJpeg(data, size)) {
    const std::vector<uint8_t> profile = jpegIccProfile(data, size);
    if (!profile.empty()) {
      const std::optional<ColorMat3> m = iccRgbToXyzD50(profile.data(), profile.size());
      if (m) {
        SourceGamut g;
        g.known = true;
        g.rgbToXyzD50 = *m;
        g.source = "JPEG ICC";
        return g;
      }
    }
  }
  return {};
}

}  // namespace np
