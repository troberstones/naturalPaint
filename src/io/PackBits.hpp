#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace np {

// io/PackBits -- Photoshop's per-scanline run-length encoding, in one place.
//
// **Why this is its own module rather than a helper inside one reader.** Two
// unrelated blocks of a `.abr` are compressed this way -- `samp`'s brush tip
// bitmaps (io/AbrBrushes.cpp) and `patt`'s pattern channels (io/PsPatterns.cpp)
// -- and PSD image data is a third if this project ever reads one. PackBits is
// short, it is entirely composed of bounds arithmetic, and it is exactly the
// kind of code that gets subtly rewritten rather than reused: a second copy
// would be a second place for an off-by-one to live, discoverable only by one
// of the two callers producing wrong pixels. It was lifted VERBATIM out of
// `io/AbrBrushes.cpp`'s anonymous namespace -- byte-for-byte the same
// arithmetic that has been decoding real Kyle Webster packs -- rather than
// rewritten for its new home.
//
// Reads no byte outside `body`, on any input whatsoever. This decodes a format
// that arrives from the internet; see io/Descriptor.hpp's header for the
// contract every parser in this directory holds itself to.

// The layout: `height` big-endian u16 compressed-byte-counts, then that many
// PackBits bytes, decoded as ONE continuous stream to exactly `expected` bytes.
//
// **Decoding as one stream rather than one call per row is deliberate, not a
// shortcut.** A PackBits run or literal never straddles Photoshop's own row
// boundaries in a well-formed file -- Adobe's own encoder does not emit one
// that does -- so per-row and single-pass decoding produce identical bytes for
// every well-formed file, and the single-pass form is what the openly-published
// `abrupng` reader this framing was cross-checked against does too. Where the
// two WOULD diverge -- a malformed stream whose run crosses a row boundary --
// this form still cannot read past `end`, because every byte access is checked
// against it first; it can only decode FEWER than `expected` bytes and report
// the shortfall, never more.
//
// `off` is where the row-length table begins and `end` bounds the whole
// compressed region. Returns false, with `out` left in an unspecified but valid
// state, when the stream is truncated, malformed, or decodes to a length other
// than `expected` -- refusing rather than returning a short buffer, because a
// half-decoded tip is a picture of a bug rather than a picture of a brush.
bool decodePackBits(std::span<const uint8_t> body, size_t off, size_t end, uint32_t height,
                    size_t expected, std::vector<uint8_t>& out) noexcept;

}  // namespace np
