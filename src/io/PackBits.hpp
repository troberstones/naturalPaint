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


// --- The encoder ----------------------------------------------------------
//
// Added for io/PsdExport (PLAN.md phase 15), and living here rather than in
// the writer for the reason this module exists at all: a second copy of
// PackBits is a second place for an off-by-one to live. `decodePackBits()`
// above is its inverse, and app/selftest/PsdWrite.cpp asserts exactly that
// on every stream this encoder can produce -- a round trip through the
// function two real importers already depend on is a stronger check than any
// hand-authored expected-bytes fixture.
//
// Encodes ONE row. Photoshop's framing needs each row's compressed length
// separately (the row-count table precedes the compressed bytes for a whole
// channel), so a whole-buffer entry point would have to return those lengths
// anyway and the caller would still loop.
//
// The packet forms, and the one byte that must never be emitted: a literal
// packet is a count byte `n-1` in 0..127 followed by `n` bytes; a repeat
// packet is `257-n` for `n` in 2..128 followed by the single repeated byte;
// **128 (0x80) is a no-op in Adobe's own reader and this encoder never emits
// it**. Runs never straddle a row boundary here because a row is all this
// function is ever given -- which is the property `decodePackBits()`'s own
// single-pass framing relies on.
//
// **A compressed row can be LARGER than the raw row** -- worst case
// `n + ceil(n/128)`, an incompressible row. That is expected and correct;
// callers must not "fall back to raw" for one row, because PSD's compression
// word is per channel, not per row.
std::vector<uint8_t> encodePackBits(std::span<const uint8_t> row);

}  // namespace np
