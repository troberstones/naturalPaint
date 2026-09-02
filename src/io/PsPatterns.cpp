#include "io/PsPatterns.hpp"

#include <algorithm>
#include <cstring>

#include "io/PackBits.hpp"

namespace np {
namespace {

// Bounds reads that refuse rather than wrap, matching every other parser in
// this directory. Kept local for the reason io/PackBits.cpp's own copy states.
bool readU8(std::span<const uint8_t> b, size_t at, uint8_t& out) noexcept {
  if (at >= b.size()) return false;
  out = b[at];
  return true;
}

bool readU16(std::span<const uint8_t> b, size_t at, uint16_t& out) noexcept {
  if (at > b.size() || b.size() - at < 2) return false;
  out = static_cast<uint16_t>((b[at] << 8) | b[at + 1]);
  return true;
}

bool readU32(std::span<const uint8_t> b, size_t at, uint32_t& out) noexcept {
  if (at > b.size() || b.size() - at < 4) return false;
  out = (static_cast<uint32_t>(b[at]) << 24) | (static_cast<uint32_t>(b[at + 1]) << 16) |
        (static_cast<uint32_t>(b[at + 2]) << 8) | static_cast<uint32_t>(b[at + 3]);
  return true;
}

bool readI32(std::span<const uint8_t> b, size_t at, int32_t& out) noexcept {
  uint32_t raw = 0;
  if (!readU32(b, at, raw)) return false;
  out = static_cast<int32_t>(raw);
  return true;
}

// Photoshop image modes, only the ones this reader can turn into a height
// field. The others are refused by name rather than guessed at.
constexpr uint32_t kModeGrayscale = 1;
constexpr uint32_t kModeIndexed = 2;
constexpr uint32_t kModeRgb = 3;

// One channel's decoded bytes, plus where the walk should resume.
struct Channel {
  std::vector<uint8_t> bytes;
  int32_t width = 0;
  int32_t height = 0;
};

// A UTF-16BE string with a u32 code-unit count, transcoded to UTF-8.
//
// Deliberately simple next to io/Descriptor's own decoder: a pattern name is
// display text, so a lone surrogate becomes U+FFFD and the walk continues,
// rather than the record being refused over its label. The bytes that matter
// -- the id and the pixels -- are read separately and are not text at all.
bool readUnicodeName(std::span<const uint8_t> b, size_t at, size_t limit, std::string& out,
                     size_t& next) {
  uint32_t units = 0;
  if (!readU32(b, at, units)) return false;
  const size_t bytes = static_cast<size_t>(units) * 2u;
  if (at + 4 > limit || limit - (at + 4) < bytes) return false;

  out.clear();
  out.reserve(units);
  size_t p = at + 4;
  for (uint32_t i = 0; i < units; ++i, p += 2) {
    uint32_t cp = (static_cast<uint32_t>(b[p]) << 8) | b[p + 1];
    if (cp == 0) continue;  // Photoshop NUL-terminates; drop, do not encode
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units) {
      const uint32_t lo = (static_cast<uint32_t>(b[p + 2]) << 8) | b[p + 3];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        ++i;
        p += 2;
      } else {
        cp = 0xFFFD;
      }
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }
  next = at + 4 + bytes;
  return true;
}

// One Virtual Memory Array entry: `u32 written`, `u32 length`, then -- inside
// `length` -- `u32 depth`, a rectangle, `u16 depth`, `u8 compression`, data.
//
// `length` is 23 + the data bytes exactly, verified against two real records
// (22523 = 23 + 150*150 and 810023 = 23 + 900*900). An entry with `written`
// or `length` zero is an eight-byte skip, not a channel.
//
// Returns false only for framing this reader will not walk past; a channel it
// merely cannot USE (wrong depth, disagreeing rectangle) returns true with an
// empty `out`, so the walk keeps its place and the record is skipped as a
// whole rather than the block being abandoned.
bool readChannel(std::span<const uint8_t> b, size_t at, size_t limit, Channel& out, size_t& next) {
  uint32_t written = 0, length = 0;
  if (!readU32(b, at, written) || !readU32(b, at + 4, length)) return false;
  if (written == 0 || length == 0) {
    next = at + 8;
    return true;
  }
  const size_t dataStart = at + 8;
  if (dataStart > limit || limit - dataStart < length) return false;
  const size_t entryEnd = dataStart + length;
  next = entryEnd;

  int32_t top = 0, left = 0, bottom = 0, right = 0;
  uint16_t depth = 0;
  uint8_t compression = 0;
  // 4 (u32 depth) + 16 (rect) + 2 (u16 depth) + 1 (compression) = 23.
  if (length < 23) return false;
  if (!readI32(b, dataStart + 4, top) || !readI32(b, dataStart + 8, left) ||
      !readI32(b, dataStart + 12, bottom) || !readI32(b, dataStart + 16, right) ||
      !readU16(b, dataStart + 20, depth) || !readU8(b, dataStart + 22, compression))
    return false;

  const int64_t w = static_cast<int64_t>(right) - left;
  const int64_t h = static_cast<int64_t>(bottom) - top;
  // 8-bit only, and the same reasoning `parseAbrSampledTips()` gives for its
  // own depth check: every channel in every real pack examined is 8-bit, and a
  // different depth is refused by name rather than reinterpreted. Not a
  // framing failure -- the walk keeps its place and the record is skipped.
  if (w <= 0 || h <= 0 || w > kMaxPatternDimension || h > kMaxPatternDimension || depth != 8)
    return true;

  const size_t expected = static_cast<size_t>(w) * static_cast<size_t>(h);
  if (compression == 0) {
    if (entryEnd - (dataStart + 23) < expected) return false;
    out.bytes.assign(b.begin() + static_cast<ptrdiff_t>(dataStart + 23),
                     b.begin() + static_cast<ptrdiff_t>(dataStart + 23 + expected));
  } else if (compression == 1) {
    if (!decodePackBits(b, dataStart + 23, entryEnd, static_cast<uint32_t>(h), expected,
                        out.bytes))
      return false;
  } else {
    return true;  // zip; not a framing failure, just one this build cannot use
  }
  out.width = static_cast<int32_t>(w);
  out.height = static_cast<int32_t>(h);
  return true;
}

}  // namespace

PsPatternResult parseAbrPatterns(std::span<const uint8_t> patt) {
  PsPatternResult result;

  size_t off = 0;
  while (off + 4 <= patt.size()) {
    uint32_t recordLength = 0;
    if (!readU32(patt, off, recordLength)) break;
    if (recordLength == 0) break;  // the block's own natural end
    const size_t recordStart = off + 4;
    if (patt.size() - recordStart < recordLength) {
      result.truncated = true;
      break;
    }
    const size_t recordEnd = recordStart + recordLength;

    // Records are FOUR-byte aligned. A walk that advanced by `recordLength`
    // alone drifted after the second record in every real pack tried and began
    // decoding UTF-16 out of the middle of pixel data -- which produces a
    // plausible-looking name, not a crash. See this module's header.
    off = recordEnd + ((4 - recordEnd % 4) % 4);

    uint32_t version = 0, mode = 0;
    uint16_t vertical = 0, horizontal = 0;
    if (!readU32(patt, recordStart, version) || !readU32(patt, recordStart + 4, mode) ||
        !readU16(patt, recordStart + 8, vertical) || !readU16(patt, recordStart + 10, horizontal)) {
      result.truncated = true;
      break;
    }

    PsPattern pattern;
    size_t p = 0;
    if (!readUnicodeName(patt, recordStart + 12, recordEnd, pattern.name, p)) {
      result.truncated = true;
      break;
    }

    uint8_t idLength = 0;
    if (!readU8(patt, p, idLength) || recordEnd - (p + 1) < idLength) {
      result.truncated = true;
      break;
    }
    // A Pascal string, NOT a `$` sigil followed by 36 fixed bytes. 0x24 is
    // both '$' and the length 36, which is why reading it the wrong way has
    // worked so far on every file whose ids happen to be UUIDs.
    pattern.id.assign(reinterpret_cast<const char*>(patt.data()) + p + 1, idLength);
    p += 1u + idLength;

    // An indexed-colour pattern carries a 768-byte palette here. None was
    // observed in any real pack; the offset is skipped so the VMA list is
    // still found, and the record is then skipped below for its mode.
    if (mode == kModeIndexed) p += 768;

    // --- Virtual Memory Array List ---
    uint32_t vmaVersion = 0, vmaLength = 0;
    if (!readU32(patt, p, vmaVersion) || !readU32(patt, p + 4, vmaLength)) {
      result.truncated = true;
      break;
    }
    // The one reliable bound. `numberOfChannels`, four bytes further on, reads
    // 24 in every record measured and is not a count -- see the header.
    const size_t vmaEnd = std::min(p + 8 + static_cast<size_t>(vmaLength), recordEnd);
    size_t cp = p + 8 + 16 + 4;  // past the rectangle and the bogus channel count

    std::vector<Channel> channels;
    bool framingOk = true;
    while (cp + 8 <= vmaEnd) {
      Channel channel;
      size_t next = 0;
      if (!readChannel(patt, cp, vmaEnd, channel, next) || next <= cp) {
        // A record's last written channel is followed by a four-byte short
        // tail in every real pack. Reaching it is the normal end of the walk,
        // not a malformed file -- so stop without marking anything truncated.
        framingOk = (vmaEnd - cp) <= 4;
        break;
      }
      cp = next;
      if (!channel.bytes.empty()) channels.push_back(std::move(channel));
    }

    if (!framingOk) {
      ++result.skipped;
      continue;
    }
    if (mode != kModeGrayscale && mode != kModeRgb) {
      ++result.skipped;
      continue;
    }

    const size_t needed = (mode == kModeRgb) ? 3u : 1u;
    if (channels.size() < needed || channels[0].width != static_cast<int32_t>(horizontal) ||
        channels[0].height != static_cast<int32_t>(vertical)) {
      ++result.skipped;
      continue;
    }

    pattern.width = channels[0].width;
    pattern.height = channels[0].height;
    const size_t texels = channels[0].bytes.size();

    if (mode == kModeGrayscale) {
      pattern.height8 = std::move(channels[0].bytes);
    } else {
      pattern.height8.resize(texels);
      // Collapse to luminance on the way in. Paper tooth is a scalar height
      // field -- `brush/Grain`'s whole formula is `F = clamp(P*S*O1 - G, 0, 1)`
      // with G a single height -- so carrying two more channels would be
      // carrying data nothing samples. Rec.601 weights, integer, because this
      // is a height map and not a colour conversion.
      if (channels[1].bytes.size() != texels || channels[2].bytes.size() != texels) {
        ++result.skipped;
        continue;
      }
      for (size_t i = 0; i < texels; ++i) {
        const uint32_t r = channels[0].bytes[i], g = channels[1].bytes[i],
                       b = channels[2].bytes[i];
        pattern.height8[i] = static_cast<uint8_t>((r * 77u + g * 150u + b * 29u) >> 8);
      }
    }

    result.patterns.push_back(std::move(pattern));
  }

  return result;
}

}  // namespace np
