#include "io/GimpBrush.hpp"

#include <cstring>

namespace np {
namespace {

bool readU32(std::span<const uint8_t> b, size_t at, uint32_t& out) noexcept {
  if (at > b.size() || b.size() - at < 4) return false;
  out = (static_cast<uint32_t>(b[at]) << 24) | (static_cast<uint32_t>(b[at + 1]) << 16) |
        (static_cast<uint32_t>(b[at + 2]) << 8) | static_cast<uint32_t>(b[at + 3]);
  return true;
}

// `('G' << 24) + ('I' << 16) + ('M' << 8) + 'P'`, as the standard states it.
constexpr uint32_t kGimpMagic = 0x47494D50u;

// Version 1 has no magic and no spacing, so its fixed header stops after
// `bytes`; version 2 adds both. Everything after the fixed part, up to
// `headerSize`, is the name.
constexpr uint32_t kHeaderV1 = 20;
constexpr uint32_t kHeaderV2 = 28;

// Decodes one `.gbr` starting at `at`, and reports where the next one begins
// -- which is what makes a `.gih`'s concatenated records walkable without the
// pipe reader needing to know anything about the brush layout.
bool readOneBrush(std::span<const uint8_t> b, size_t at, GimpBrushTip& out, size_t& next,
                  std::string& error) {
  uint32_t headerSize = 0, version = 0, width = 0, height = 0, depth = 0;
  if (!readU32(b, at, headerSize) || !readU32(b, at + 4, version) ||
      !readU32(b, at + 8, width) || !readU32(b, at + 12, height) ||
      !readU32(b, at + 16, depth)) {
    error = "truncated before the end of the brush header.";
    return false;
  }

  if (version != 1 && version != 2) {
    // Refused by name rather than parsed hopefully -- the same call
    // io/AbrBrushes.cpp makes for an `.abr` version it does not know. A
    // version 3 would be a different layout, and guessing at one is how a
    // parser reads arbitrary memory.
    error = "unsupported .gbr version " + std::to_string(version) + " (1 and 2 are read).";
    return false;
  }

  const uint32_t fixed = (version == 1) ? kHeaderV1 : kHeaderV2;
  if (headerSize < fixed) {
    error = "header size " + std::to_string(headerSize) + " is shorter than version " +
            std::to_string(version) + "'s fixed header.";
    return false;
  }
  if (version == 2) {
    uint32_t magic = 0, spacing = 0;
    if (!readU32(b, at + 20, magic) || !readU32(b, at + 24, spacing)) {
      error = "truncated inside the version 2 header.";
      return false;
    }
    // The magic is version 2's own integrity check and the one cheap way to
    // notice that a file is not what its extension claims. Version 1 has none,
    // which is why this is inside the branch rather than above it.
    if (magic != kGimpMagic) {
      error = "not a GIMP brush: the version 2 magic is absent.";
      return false;
    }
    out.haveSpacing = true;
    out.spacingPercent = spacing;
  }

  if (width == 0 || height == 0 || width > static_cast<uint32_t>(kMaxGimpBrushDimension) ||
      height > static_cast<uint32_t>(kMaxGimpBrushDimension)) {
    error = "brush dimensions " + std::to_string(width) + "x" + std::to_string(height) +
            " are degenerate or past this reader's cap.";
    return false;
  }
  // 1 = greyscale mask, 4 = RGBA. 2 and 3 are not in the standard; refused
  // rather than guessed, same as an unknown version.
  if (depth != 1 && depth != 4) {
    error = "unsupported colour depth " + std::to_string(depth) + " (1 and 4 are read).";
    return false;
  }

  // The name fills whatever is between the fixed header and `headerSize`. It
  // is NUL-terminated in every file the standard describes, so the terminator
  // is dropped rather than carried into a std::string that would then compare
  // unequal to its own displayed text.
  const size_t nameAt = at + fixed;
  const size_t nameLength = headerSize - fixed;
  if (nameAt > b.size() || b.size() - nameAt < nameLength) {
    error = "truncated inside the brush name.";
    return false;
  }
  out.name.assign(reinterpret_cast<const char*>(b.data()) + nameAt, nameLength);
  while (!out.name.empty() && out.name.back() == '\0') out.name.pop_back();

  const size_t pixelsAt = at + headerSize;
  const size_t texels = static_cast<size_t>(width) * static_cast<size_t>(height);
  const size_t pixelBytes = texels * depth;
  if (pixelsAt > b.size() || b.size() - pixelsAt < pixelBytes) {
    error = "truncated inside the pixel data.";
    return false;
  }

  out.width = static_cast<int32_t>(width);
  out.height = static_cast<int32_t>(height);
  out.alpha.resize(texels);
  if (depth == 1) {
    // A greyscale `.gbr` stores the MASK, so the byte is coverage directly.
    // See the header on why this is inference and what settles it.
    std::memcpy(out.alpha.data(), b.data() + pixelsAt, texels);
  } else {
    // RGBA: the alpha channel is the coverage and the colour is dropped. This
    // build's tip is a coverage mask (brush/Deposit.hpp's `BrushTipBitmap`),
    // so there is nowhere for per-texel colour to go -- Krita's "color image"
    // and "lightness map" tip modes are a real feature this does not have, and
    // dropping the colour silently would misrepresent a coloured brush as a
    // shaped one. Callers that care can say so; the reader cannot invent a
    // channel.
    for (size_t i = 0; i < texels; ++i) out.alpha[i] = b[pixelsAt + i * 4 + 3];
  }

  next = pixelsAt + pixelBytes;
  return true;
}

}  // namespace

GimpBrushResult readGimpBrush(std::span<const uint8_t> bytes) {
  GimpBrushResult result;
  GimpBrushTip tip;
  size_t next = 0;
  if (!readOneBrush(bytes, 0, tip, next, result.error)) return result;
  result.tips.push_back(std::move(tip));
  result.ok = true;
  return result;
}

GimpBrushResult readGimpBrushPipe(std::span<const uint8_t> bytes) {
  GimpBrushResult result;

  // Two text lines: the pipe's name, then its parameters. The count this
  // reader needs is the FIRST integer on the parameter line -- the standard's
  // example is `Fire 6 ncells:6 step:20 dim:1 cols:3 rows:2 rank0:6
  // selection:incremental`, where the leading 6 is the cell count and the
  // `key:value` pairs after it describe placement this build cannot use (see
  // the header). Parsed by hand rather than with a scanner, because a
  // `.gih`'s header is two lines and pulling in a tokeniser for it would be
  // more code than the whole reader.
  size_t p = 0;
  auto readLine = [&](std::string& out) {
    const size_t start = p;
    while (p < bytes.size() && bytes[p] != '\n') ++p;
    out.assign(reinterpret_cast<const char*>(bytes.data()) + start, p - start);
    if (p < bytes.size()) ++p;  // step over the newline
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return start < bytes.size();
  };

  std::string name, params;
  if (!readLine(name) || !readLine(params)) {
    result.error = "truncated before the end of the .gih header's two lines.";
    return result;
  }

  size_t q = 0;
  while (q < params.size() && (params[q] == ' ' || params[q] == '\t')) ++q;
  uint32_t cells = 0;
  bool haveDigits = false;
  while (q < params.size() && params[q] >= '0' && params[q] <= '9') {
    cells = cells * 10u + static_cast<uint32_t>(params[q] - '0');
    haveDigits = true;
    ++q;
    if (cells > 4096u) break;  // a hose with more cells than this is not one
  }
  if (!haveDigits || cells == 0) {
    result.error = "the .gih parameter line does not begin with a cell count.";
    return result;
  }

  for (uint32_t i = 0; i < cells; ++i) {
    GimpBrushTip tip;
    size_t next = 0;
    if (!readOneBrush(bytes, p, tip, next, result.error)) {
      // Partial is refused rather than returned: a hose whose cells stop
      // arriving halfway is a truncated file, and half a hose is not a
      // shorter hose -- it is a picture of a bug. Same call
      // io/PackBits.cpp's decoder makes about a short decode.
      result.tips.clear();
      result.error = "cell " + std::to_string(i + 1) + " of " + std::to_string(cells) + ": " +
                     result.error;
      return result;
    }
    // The cell's own name is usually empty or repeated; number them so the dab
    // library has distinct ids without inventing a naming scheme per file.
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), " %02u", i + 1);
    tip.name = (tip.name.empty() ? name : tip.name) + suffix;
    result.tips.push_back(std::move(tip));
    p = next;
  }

  result.ok = true;
  return result;
}

}  // namespace np
