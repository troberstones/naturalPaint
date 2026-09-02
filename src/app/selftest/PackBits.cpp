#include "app/selftest/Support.hpp"

#include <cstdint>
#include <vector>

#include "io/PackBits.hpp"

namespace np {

// io/PackBits: the refusal contract, on hand-built byte streams.
//
// `decodePackBits()` is reached from two importers (io/AbrBrushes for `samp`
// tips, io/PsdImport for RLE scanlines) and its header makes a strong claim
// about hostile input: it "cannot read past `end`, because every byte access
// is checked against it first; it can only decode FEWER than `expected` bytes
// and report the shortfall, never more." Until this section existed that claim
// had no test of its own -- both importers exercise the function only through
// well-formed files, which is exactly the input that cannot distinguish a
// checked decoder from an unchecked one.
//
// **What this section does NOT cover, stated so nobody reads it as more than
// it is.** The literal-copy bound is written `count > dataEnd - p` rather than
// `p + count > dataEnd` (see that line's own comment, and
// docs/architecture-review.md P2-2). The difference between those two forms is
// visible only when `p + count` overflows `size_t` -- and `count` is at most
// 128 here, so reaching it would need `p` within 128 of SIZE_MAX, which no
// real buffer offset can be. **The overflow shape is therefore not testable
// through this interface, and none of the assertions below would go red if the
// bound were written the wrapping way.** That is not an argument for writing it
// the wrapping way; it is P2-2's own argument restated -- the check has to be
// correct by construction, because nothing downstream will catch it if it is
// not. A merge nearly reverted this exact line, and what caught it was reading,
// not this file.
//
// What IS covered below: every reachable way a malformed stream can end, and
// the guarantee that a refusal is a refusal rather than a short buffer.
bool runPackBitsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] packbits: the refusal contract on malformed streams\n");

  // A stream is: `height` big-endian u16 row lengths, then that many bytes of
  // PackBits control/data pairs. Written out by hand so each case's defect is
  // visible in the literal rather than hidden in a builder.
  auto decode = [](const std::vector<uint8_t>& body, uint32_t height, size_t expected,
                   std::vector<uint8_t>& out) {
    return decodePackBits(body, 0, body.size(), height, expected, out);
  };

  std::vector<uint8_t> out;

  // --- 1. Well formed: a literal, and a run --------------------------------
  {
    // rowlen 3; control 0x01 = "the next 2 bytes verbatim"; 'A','B'.
    const std::vector<uint8_t> lit{0x00, 0x03, 0x01, 'A', 'B'};
    check(decode(lit, 1, 2, out), "packbits: a well-formed literal decodes");
    check(out.size() == 2 && out[0] == 'A' && out[1] == 'B',
          "packbits: ...to exactly the bytes the literal named");

    // rowlen 2; control 0xFE = -2 = "repeat the next byte 3 times"; 'Z'.
    const std::vector<uint8_t> run{0x00, 0x02, 0xFE, 'Z'};
    check(decode(run, 1, 3, out), "packbits: a well-formed run decodes");
    check(out.size() == 3 && out[0] == 'Z' && out[1] == 'Z' && out[2] == 'Z',
          "packbits: ...expanding to the repeat count, not the byte count");

    // 0x80 is PackBits' own documented no-op control byte, not a run of -128.
    const std::vector<uint8_t> nop{0x00, 0x03, 0x80, 0x00, 'Q'};
    check(decode(nop, 1, 1, out) && out.size() == 1 && out[0] == 'Q',
          "packbits: 0x80 is a NOP, and decoding continues past it");
  }

  // --- 2. A literal that claims more bytes than the region holds -----------
  //
  // The case the bound exists for. `count` is 6 with 2 bytes left, so the
  // copy would run 4 bytes past `dataEnd` -- into the caller's buffer, which
  // for io/AbrBrushes is a whole `.abr` file and not the tip being decoded.
  {
    const std::vector<uint8_t> over{0x00, 0x03, 0x05, 'A', 'B'};
    check(!decode(over, 1, 6, out),
          "packbits: a literal claiming more bytes than remain is REFUSED");
  }

  // --- 3. A run whose repeated byte is not there ---------------------------
  {
    const std::vector<uint8_t> hanging{0x00, 0x01, 0xFE};
    check(!decode(hanging, 1, 3, out),
          "packbits: a run with no byte left to repeat is REFUSED");
  }

  // --- 4. The row-length table itself --------------------------------------
  {
    // Three rows promised, four bytes present: the table cannot even be read.
    const std::vector<uint8_t> shortTable{0x00, 0x01, 0x00, 0x01};
    check(!decode(shortTable, 3, 1, out),
          "packbits: a row-length table that runs past the end is REFUSED");

    // The table reads, but its total names more compressed bytes than exist.
    const std::vector<uint8_t> bigTotal{0x00, 0x09, 0x01};
    check(!decode(bigTotal, 1, 2, out),
          "packbits: a row total larger than the region is REFUSED");
  }

  // --- 5. A refusal is a refusal, not a short buffer -----------------------
  //
  // The header's "report the shortfall, never more" half. A decoder that
  // returned `true` with a partly-filled buffer would hand io/AbrBrushes a tip
  // that is a picture of a bug rather than a picture of a brush.
  {
    // Decodes exactly one byte, but five were expected.
    const std::vector<uint8_t> shortfall{0x00, 0x02, 0x00, 'A'};
    check(!decode(shortfall, 1, 5, out),
          "packbits: decoding FEWER bytes than expected is a refusal, not a short buffer");
  }

  // --- 6. It never writes more than `expected` -----------------------------
  //
  // The other direction, and the one that bounds the DESTINATION rather than
  // the source: a run long enough to overflow the caller's buffer stops at
  // `expected` instead. This one succeeds -- the stream was well formed and
  // produced the bytes asked for -- so it is the case that shows the cap is a
  // cap and not an error path.
  {
    // 0xFB = -5 = repeat 6 times, but only 2 bytes were asked for.
    const std::vector<uint8_t> longRun{0x00, 0x02, 0xFB, 'Q'};
    check(decode(longRun, 1, 2, out) && out.size() == 2,
          "packbits: a run longer than `expected` stops at `expected`, never past it");
  }

  return ok;
}

}  // namespace np
