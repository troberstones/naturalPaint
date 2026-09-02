#include "io/PackBits.hpp"

namespace np {
namespace {

// Local rather than shared with io/AbrBrushes: two lines, and a dependency
// edge between two format readers to save them would be worse than the
// duplication. Refuses rather than wrapping, same as every bounds read in
// this directory.
bool readU16(std::span<const uint8_t> b, size_t at, uint16_t& out) noexcept {
  if (at > b.size() || b.size() - at < 2) return false;
  out = static_cast<uint16_t>((b[at] << 8) | b[at + 1]);
  return true;
}

}  // namespace

bool decodePackBits(std::span<const uint8_t> body, size_t off, size_t end, uint32_t height,
                    size_t expected, std::vector<uint8_t>& out) noexcept {
  if (off > end || end > body.size()) return false;

  // The row-length table: `height` u16s, big-endian, summed for the total
  // compressed byte count -- `abrupng`'s own `read_rle_data()` does the same
  // ("We just need the total length"), which is what makes decoding as one
  // stream rather than `height` separate calls correct rather than merely
  // convenient (see this function's own comment above).
  if (static_cast<uint64_t>(height) * 2u > end - off) return false;
  uint64_t total = 0;
  size_t p = off;
  for (uint32_t i = 0; i < height; ++i) {
    uint16_t rowLen = 0;
    if (!readU16(body, p, rowLen)) return false;
    total += rowLen;
    p += 2;
  }
  if (total > end - p) return false;
  const size_t dataEnd = p + static_cast<size_t>(total);

  out.clear();
  out.reserve(expected);
  while (p < dataEnd && out.size() < expected) {
    const int8_t n = static_cast<int8_t>(body[p]);
    ++p;
    if (n == -128) {
      continue;  // NOP: PackBits' own no-op control byte
    } else if (n < 0) {
      // Run: repeat the next byte (-n + 1) times.
      if (p >= dataEnd) return false;
      const size_t count = static_cast<size_t>(-static_cast<int>(n) + 1);
      const uint8_t b = body[p];
      ++p;
      for (size_t k = 0; k < count && out.size() < expected; ++k) out.push_back(b);
    } else {
      // Literal: the next (n + 1) bytes, verbatim.
      const size_t count = static_cast<size_t>(n) + 1;
      // Written as a SUBTRACTION, not `p + count > dataEnd`. That addition is
      // one of the four sites docs/architecture-review.md P2-2 names by name
      // (io/AbrBrushes.hpp's `checkedAdd()` comment lists it as "`p + count >
      // dataEnd` in `decodePackBits()`"), and it followed this function out of
      // io/AbrBrushes.cpp when the function was extracted here on a branch
      // that had forked before P2-2 landed -- so the merge that brought this
      // file in was one careless resolution away from restoring the wrapping
      // add that P2-2 removed.
      //
      // Subtraction rather than `checkedAdd()` because the dependency runs the
      // other way: io/AbrBrushes.cpp includes this file, not the reverse. It
      // is exact here without a helper: the loop is entered only while
      // `p < dataEnd` and `p` has advanced by exactly one since, so
      // `p <= dataEnd` holds and `dataEnd - p` cannot underflow.
      if (count > dataEnd - p) return false;
      out.insert(out.end(), body.data() + p, body.data() + p + count);
      p += count;
    }
  }
  return out.size() == expected;
}

}  // namespace np
