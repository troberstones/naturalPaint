#include "io/PsdWrite.hpp"

#include <cstring>

namespace np {
namespace {

// A placeholder written by beginLengthU32(). Any value would do for the
// arithmetic -- the four bytes are overwritten -- but a recognisable one
// makes an unclosed section visible in a hex dump of a buffer that was
// taken while still incomplete, rather than reading as a legitimate zero
// length.
constexpr uint32_t kLengthPlaceholder = 0xFFFFFFFFu;

constexpr uint16_t kReplacement = 0xFFFDu;

}  // namespace

void PsdWriter::u8(uint8_t v) { bytes_.push_back(v); }

void PsdWriter::u16(uint16_t v) {
  bytes_.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  bytes_.push_back(static_cast<uint8_t>(v & 0xFFu));
}

void PsdWriter::u32(uint32_t v) {
  bytes_.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
  bytes_.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
  bytes_.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  bytes_.push_back(static_cast<uint8_t>(v & 0xFFu));
}

// The two signed writers go through the unsigned ones by conversion rather
// than by shifting the signed value directly: right-shifting a negative
// signed integer is implementation-defined, and every one of PSD's signed
// fields (a layer's rectangle, a mask's rectangle) is routinely negative in
// real files -- a layer dragged off the top-left of the canvas has a
// negative `top` and `left`, which is not an edge case but the normal
// result of moving a layer. Conversion to the unsigned type of the same
// width is well-defined two's-complement wrapping, which is exactly the bit
// pattern the format wants, and is what io/PsdImport.cpp's `Cursor::i32()`
// reads back.
void PsdWriter::i16(int16_t v) { u16(static_cast<uint16_t>(v)); }

void PsdWriter::i32(int32_t v) { u32(static_cast<uint32_t>(v)); }

void PsdWriter::fourcc(const char* key) {
  if (key == nullptr || std::strlen(key) != 4) {
    ok_ = false;
    return;
  }
  bytes_.insert(bytes_.end(), key, key + 4);
}

void PsdWriter::raw(std::span<const uint8_t> b) {
  bytes_.insert(bytes_.end(), b.begin(), b.end());
}

void PsdWriter::zeros(size_t count) { bytes_.insert(bytes_.end(), count, uint8_t{0}); }

size_t PsdWriter::beginLengthU32() {
  const size_t marker = bytes_.size();
  u32(kLengthPlaceholder);
  return marker;
}

void PsdWriter::endLengthU32(size_t marker) {
  // The marker must name four bytes this writer actually reserved. A caller
  // that passes a stale or invented offset would otherwise silently
  // overwrite four bytes of real content with a length -- a corruption that
  // produces a file which parses for a while and then does not.
  if (marker + 4 > bytes_.size()) {
    ok_ = false;
    return;
  }
  const size_t written = bytes_.size() - (marker + 4);
  if (written > 0xFFFFFFFFull) {
    ok_ = false;
    return;
  }
  const uint32_t v = static_cast<uint32_t>(written);
  bytes_[marker] = static_cast<uint8_t>((v >> 24) & 0xFFu);
  bytes_[marker + 1] = static_cast<uint8_t>((v >> 16) & 0xFFu);
  bytes_[marker + 2] = static_cast<uint8_t>((v >> 8) & 0xFFu);
  bytes_[marker + 3] = static_cast<uint8_t>(v & 0xFFu);
}

size_t PsdWriter::padTo(size_t multiple, size_t sectionStart) {
  if (multiple == 0 || sectionStart > bytes_.size()) {
    ok_ = false;
    return 0;
  }
  const size_t written = bytes_.size() - sectionStart;
  const size_t remainder = written % multiple;
  if (remainder == 0) return 0;
  const size_t pad = multiple - remainder;
  zeros(pad);
  return pad;
}

void PsdWriter::pascalString(const std::string& utf8, size_t padMultiple) {
  // Truncate to 255 bytes, but never mid-sequence: a UTF-8 continuation
  // byte is 10xxxxxx, so walking back off any continuation byte lands on
  // the lead byte of the character being cut and drops the whole character.
  size_t len = utf8.size() < 255 ? utf8.size() : 255;
  while (len > 0 && (static_cast<uint8_t>(utf8[len]) & 0xC0u) == 0x80u) --len;

  const size_t fieldStart = bytes_.size();
  u8(static_cast<uint8_t>(len));
  bytes_.insert(bytes_.end(), utf8.begin(), utf8.begin() + static_cast<ptrdiff_t>(len));
  // The pad is measured from the LENGTH BYTE, not from the text -- the
  // whole field including its own length byte is what must reach the
  // multiple. io/PsdImport.cpp:614 computes `consumedSoFar = 1 + nameLen`
  // before rounding, and this is the same arithmetic from the other side.
  padTo(padMultiple, fieldStart);
}

void PsdWriter::unicodeString(const std::string& utf8) {
  const std::vector<uint16_t> units = utf8ToUtf16(utf8);
  u32(static_cast<uint32_t>(units.size()));
  for (const uint16_t unit : units) u16(unit);
}

std::vector<uint16_t> utf8ToUtf16(const std::string& utf8) {
  std::vector<uint16_t> out;
  out.reserve(utf8.size());

  const size_t n = utf8.size();
  size_t i = 0;
  while (i < n) {
    const uint8_t lead = static_cast<uint8_t>(utf8[i]);
    uint32_t cp = 0;
    size_t extra = 0;
    uint32_t lowest = 0;  // the smallest code point this length may encode,
                          // which is how an overlong form is detected

    if (lead < 0x80u) {
      cp = lead;
      extra = 0;
      lowest = 0;
    } else if ((lead & 0xE0u) == 0xC0u) {
      cp = lead & 0x1Fu;
      extra = 1;
      lowest = 0x80u;
    } else if ((lead & 0xF0u) == 0xE0u) {
      cp = lead & 0x0Fu;
      extra = 2;
      lowest = 0x800u;
    } else if ((lead & 0xF8u) == 0xF0u) {
      cp = lead & 0x07u;
      extra = 3;
      lowest = 0x10000u;
    } else {
      // A continuation byte with no lead, or one of the 5-/6-byte forms
      // UTF-8 has not permitted since 2003. One replacement character, one
      // byte consumed -- never a resynchronisation loop that could consume
      // the rest of the string on a single bad byte.
      out.push_back(kReplacement);
      ++i;
      continue;
    }

    // The sequence needs bytes `i` through `i + extra` inclusive, so it is
    // truncated exactly when `i + extra` is not a valid index. Written as
    // `i + extra >= n` rather than `i + extra > n - 1` because `n` is a
    // size_t and `n - 1` underflows on an empty string.
    if (i + extra >= n) {
      out.push_back(kReplacement);
      ++i;
      continue;
    }

    bool valid = true;
    for (size_t k = 1; k <= extra; ++k) {
      const uint8_t cont = static_cast<uint8_t>(utf8[i + k]);
      if ((cont & 0xC0u) != 0x80u) {
        valid = false;
        break;
      }
      cp = (cp << 6) | (cont & 0x3Fu);
    }

    // Three ways a well-formed-looking sequence is still invalid, and all
    // three have been used as attacks on decoders that accept them: an
    // overlong form (a code point encoded in more bytes than it needs, so
    // that a naive check for a forbidden ASCII byte misses it), a UTF-16
    // surrogate half encoded as if it were a character (CESU-8), and a
    // value past U+10FFFF.
    if (!valid || cp < lowest || (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu) {
      out.push_back(kReplacement);
      ++i;
      continue;
    }

    if (cp < 0x10000u) {
      out.push_back(static_cast<uint16_t>(cp));
    } else {
      // Astral plane: one code point becomes a surrogate PAIR, and the
      // `luni` count field counts both. This is the arithmetic
      // io/PsdImport.hpp lists as never having been checked against a real
      // Photoshop file, so it is asserted directly rather than only
      // round-tripped.
      const uint32_t v = cp - 0x10000u;
      out.push_back(static_cast<uint16_t>(0xD800u + (v >> 10)));
      out.push_back(static_cast<uint16_t>(0xDC00u + (v & 0x3FFu)));
    }
    i += extra + 1;
  }
  return out;
}

}  // namespace np
