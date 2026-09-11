#include "app/selftest/Support.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "io/PackBits.hpp"
#include "io/PsdWrite.hpp"

namespace np {

// io/PsdWrite and io/PackBits' encoder: the byte-level half of PSD export
// (PLAN.md phase 15, docs/psd-export.md).
//
// **Why the PackBits assertions live here and not in
// app/selftest/PackBits.cpp.** That module tests the DECODER's refusal
// contract against hostile input -- a different question with a different
// shape. What is tested here is that the encoder and that decoder are
// inverses, which is a claim about the pair, and the decoder is the half
// that two real importers (io/AbrBrushes for `samp` tips, io/PsdImport for
// RLE scanlines) already depend on against real Kyle Webster packs and real
// Photoshop files. **Round-tripping against a function with that much
// real-world exposure is a materially stronger check than any table of
// expected bytes this file's own author could write**, because an
// expected-bytes fixture only ever proves the encoder agrees with the
// author's reading of the spec, and the decoder already demonstrably agrees
// with Adobe's.
//
// The expected-byte assertions that ARE here cover the three things a round
// trip cannot see: that 0x80 is never emitted (a decoder that treats it as
// a no-op round-trips a stream containing it), that the packet choice is
// the efficient one rather than merely a correct one, and the worst-case
// growth bound callers are told to expect.

namespace {

// The framing decodePackBits() expects: `height` big-endian u16 compressed
// row lengths, then the rows' compressed bytes end to end. This is what
// io/PsdExport writes around this encoder for a whole channel, so building
// it here exercises the same shape rather than a test-only one.
std::vector<uint8_t> frameRows(const std::vector<std::vector<uint8_t>>& rows) {
  std::vector<uint8_t> encoded;
  std::vector<std::vector<uint8_t>> packed;
  packed.reserve(rows.size());
  for (const std::vector<uint8_t>& row : rows) packed.push_back(encodePackBits(row));
  for (const std::vector<uint8_t>& p : packed) {
    encoded.push_back(static_cast<uint8_t>((p.size() >> 8) & 0xFFu));
    encoded.push_back(static_cast<uint8_t>(p.size() & 0xFFu));
  }
  for (const std::vector<uint8_t>& p : packed)
    encoded.insert(encoded.end(), p.begin(), p.end());
  return encoded;
}

// **The naive round trip is not enough, and a sabotage is what showed it.**
// `decodePackBits()` clamps its output at `expected` -- both its packet loop
// and its repeat loop carry an `out.size() < expected` condition. So an
// encoder that writes a repeat count ONE TOO LARGE decodes to the right
// bytes anyway on any row short enough that the over-count lands in the
// final packet: the decoder simply stops. Deliberately breaking
// `encodePackBits()` from `257 - run` to `256 - run` reddened the long rows
// (where the error accumulates and desynchronises) and left "a three-byte
// run round-trips" green, which is a test agreeing with a broken encoder.
//
// The second decode below is what closes that. A CORRECT stream decodes to
// exactly `flat.size()` bytes, so asking for more must fail *and* leave
// exactly that many bytes behind; an over-counting stream satisfies the
// larger request and returns true. That distinction is invisible to the
// first decode and is the whole reason this helper makes two.
bool roundTrips(const std::vector<std::vector<uint8_t>>& rows) {
  std::vector<uint8_t> flat;
  for (const std::vector<uint8_t>& row : rows) flat.insert(flat.end(), row.begin(), row.end());
  const std::vector<uint8_t> encoded = frameRows(rows);
  const uint32_t height = static_cast<uint32_t>(rows.size());

  std::vector<uint8_t> out;
  if (!decodePackBits(encoded, 0, encoded.size(), height, flat.size(), out)) return false;
  if (out != flat) return false;

  // The stream must hold exactly `flat.size()` bytes and not one more.
  std::vector<uint8_t> over;
  if (decodePackBits(encoded, 0, encoded.size(), height, flat.size() + 8, over)) return false;
  return over.size() == flat.size() && over == flat;
}

std::vector<uint8_t> repeated(uint8_t value, size_t count) {
  return std::vector<uint8_t>(count, value);
}

std::vector<uint8_t> counting(size_t count) {
  std::vector<uint8_t> v;
  v.reserve(count);
  for (size_t i = 0; i < count; ++i) v.push_back(static_cast<uint8_t>(i & 0xFFu));
  return v;
}

std::string hex(const std::vector<uint8_t>& b) {
  static const char* kDigits = "0123456789abcdef";
  std::string s;
  for (const uint8_t v : b) {
    s.push_back(kDigits[(v >> 4) & 0xF]);
    s.push_back(kDigits[v & 0xF]);
  }
  return s;
}

}  // namespace

bool runPsdWriteTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- A. PackBits encode/decode are inverses --------------------------

  check(roundTrips({{}}), "packbits: an empty row round-trips");
  check(roundTrips({{0x42}}), "packbits: a one-byte row round-trips");
  check(roundTrips({repeated(0xFF, 2)}), "packbits: a two-byte run round-trips");
  check(roundTrips({repeated(0x00, 3)}), "packbits: a three-byte run round-trips");
  check(roundTrips({repeated(0x7F, 128)}), "packbits: a 128-byte run round-trips");
  check(roundTrips({repeated(0x7F, 129)}), "packbits: a 129-byte run round-trips");
  check(roundTrips({repeated(0x01, 300)}), "packbits: a 300-byte run round-trips");
  check(roundTrips({counting(128)}), "packbits: 128 distinct bytes round-trip");
  check(roundTrips({counting(129)}), "packbits: 129 distinct bytes round-trip");
  check(roundTrips({counting(1000)}), "packbits: 1000 distinct bytes round-trip");

  {
    // A shape real image data actually has: flat background, a short run of
    // content, flat background again. Both packet kinds, and both
    // transitions between them, in one row.
    std::vector<uint8_t> row = repeated(0x20, 50);
    const std::vector<uint8_t> mid = counting(17);
    row.insert(row.end(), mid.begin(), mid.end());
    const std::vector<uint8_t> tail = repeated(0x20, 200);
    row.insert(row.end(), tail.begin(), tail.end());
    check(roundTrips({row}), "packbits: run/literal/run round-trips");
  }

  {
    // Alternating bytes: no run ever reaches three, so this is one long
    // literal and is the input that grows.
    std::vector<uint8_t> row;
    for (size_t i = 0; i < 500; ++i) row.push_back(static_cast<uint8_t>(i % 2 == 0 ? 0xAA : 0xBB));
    check(roundTrips({row}), "packbits: alternating bytes round-trip");
  }

  check(roundTrips({repeated(0x11, 64), counting(64), repeated(0x22, 64)}),
        "packbits: three rows round-trip through one table");

  // --- B. What a round trip cannot see ---------------------------------

  {
    // 0x80 is a no-op packet in Adobe's reader, so a stream containing one
    // round-trips perfectly while being a stream this encoder promises not
    // to produce. Only a direct scan catches it.
    bool sawNoOp = false;
    const std::vector<std::vector<uint8_t>> corpus = {
        repeated(0x80, 300), counting(1000), repeated(0x00, 129), {0x80, 0x80, 0x81}};
    for (const std::vector<uint8_t>& row : corpus) {
      const std::vector<uint8_t> enc = encodePackBits(row);
      // Walk the packets rather than the bytes: a literal packet's DATA may
      // legitimately contain 0x80, and only the count bytes are the claim.
      size_t p = 0;
      while (p < enc.size()) {
        const uint8_t count = enc[p];
        if (count == 0x80) sawNoOp = true;
        if (count < 128) {
          p += 1 + static_cast<size_t>(count) + 1;
        } else {
          p += 2;
        }
      }
    }
    check(!sawNoOp, "packbits: no packet ever has the no-op count 0x80");
  }

  {
    // The efficient choice, not merely a correct one: a 128-byte run is two
    // bytes, and 300 identical bytes are three packets (128 + 128 + 44),
    // six bytes.
    const std::vector<uint8_t> a = encodePackBits(repeated(0x7F, 128));
    const std::vector<uint8_t> b = encodePackBits(repeated(0x01, 300));
    check(a.size() == 2 && a[0] == 0x81 && a[1] == 0x7F,
          "packbits: a 128-run is exactly two bytes (0x81)");
    check(b.size() == 6, "packbits: 300 identical bytes are three repeat packets");
  }

  {
    // The growth bound io/PackBits.hpp promises callers, on the input that
    // reaches it. 128 distinct bytes cost one count byte; 1000 cost eight.
    const std::vector<uint8_t> a = encodePackBits(counting(128));
    const std::vector<uint8_t> b = encodePackBits(counting(1000));
    check(a.size() == 129, "packbits: 128 incompressible bytes grow to exactly 129");
    check(b.size() == 1000 + 8, "packbits: 1000 incompressible bytes grow by eight");
    check(a.size() <= 128 + (128 + 127) / 128 && b.size() <= 1000 + (1000 + 127) / 128,
          "packbits: growth stays inside the documented worst case");
  }

  // --- C. Big-endian integers, including negative rectangles ------------

  {
    PsdWriter w;
    w.u16(0x1234);
    w.u32(0xDEADBEEFu);
    check(hex(w.bytes()) == "1234deadbeef", "writer: u16/u32 are most-significant byte first");
    check(w.ok(), "writer: plain integer writes leave ok() true");
  }

  {
    // A layer dragged off the top-left of the canvas has a negative `top`
    // and `left`. This is the normal case for a real PSD rectangle, not an
    // edge case, and io/PsdImport.cpp reads it back with `Cursor::i32()`.
    PsdWriter w;
    w.i32(-1);
    w.i32(-5000);
    w.i16(-2);
    check(hex(w.bytes()) == "ffffffffffffec78fffe",
          "writer: negative i32/i16 are two's complement, not shifted");
  }

  // --- D. Backpatched section lengths -----------------------------------

  {
    PsdWriter w;
    const size_t outer = w.beginLengthU32();
    w.fourcc("8BIM");
    const size_t inner = w.beginLengthU32();
    w.u32(0x01020304u);
    w.endLengthU32(inner);
    w.endLengthU32(outer);
    // outer content: 4 (8BIM) + 4 (inner length) + 4 (payload) = 12.
    // inner content: 4. Neither length counts its own four bytes.
    check(hex(w.bytes()) == "0000000c3842494d0000000401020304",
          "writer: nested lengths exclude their own four bytes");
    check(w.ok(), "writer: a correctly closed nesting leaves ok() true");
  }

  {
    PsdWriter w;
    const size_t marker = w.beginLengthU32();
    w.endLengthU32(marker);
    check(w.bytes().size() == 4 && hex(w.bytes()) == "00000000",
          "writer: an empty section writes a zero length");
  }

  {
    PsdWriter w;
    w.endLengthU32(64);  // never reserved
    check(!w.ok(), "writer: backpatching an offset it never reserved fails ok()");
  }

  {
    PsdWriter w;
    w.fourcc("8BI");
    check(!w.ok(), "writer: a three-character fourcc fails rather than pads");
    PsdWriter w2;
    w2.fourcc("mul ");
    check(w2.ok() && hex(w2.bytes()) == "6d756c20",
          "writer: a space-padded key writes its space, not a NUL");
  }

  // --- E. Padding -------------------------------------------------------

  {
    PsdWriter w;
    const size_t start = w.size();
    w.u8(0x01);
    const size_t pad = w.padTo(4, start);
    check(pad == 3 && w.size() == 4, "writer: padTo(4) after one byte writes three zeros");
    const size_t start2 = w.size();
    w.u32(0);
    check(w.padTo(4, start2) == 0, "writer: padTo is a no-op when already aligned");
  }

  // --- F. The Pascal name, including its own length byte ----------------

  {
    // io/PsdImport.cpp:614 rounds `1 + nameLen` up to a multiple of 4. The
    // "1 +" is the half that is easy to lose, and losing it desynchronises
    // every field after the name in the record.
    PsdWriter w;
    w.pascalString("ab", 4);
    check(w.size() == 4 && hex(w.bytes()) == "02616200",
          "writer: a 2-byte name pads to 4 counting its length byte");
  }

  {
    PsdWriter w;
    w.pascalString("abc", 4);
    check(w.size() == 4 && hex(w.bytes()) == "03616263",
          "writer: a 3-byte name needs no padding at all");
  }

  {
    PsdWriter w;
    w.pascalString("", 4);
    check(w.size() == 4 && hex(w.bytes()) == "00000000",
          "writer: an empty name is still a padded four-byte field");
  }

  {
    PsdWriter w;
    w.pascalString("abcd", 4);
    check(w.size() == 8, "writer: a 4-byte name pads to eight, not four");
  }

  {
    // 254 ASCII bytes then a two-byte character: cutting at 255 would split
    // the character in half. The field must carry 254.
    std::string name(254, 'x');
    name += "\xC3\xA9";  // U+00E9
    PsdWriter w;
    w.pascalString(name, 4);
    check(w.bytes()[0] == 254, "writer: truncation drops a whole character, never half of one");
  }

  // --- G. luni: UTF-16BE code UNITS, not code points --------------------

  {
    PsdWriter w;
    w.unicodeString("A");
    check(hex(w.bytes()) == "000000010041", "writer: an ASCII luni is one unit");
  }

  {
    // U+1F600, astral plane: ONE code point, TWO code units, and the count
    // field counts units. io/PsdImport.hpp lists this as never having been
    // checked against a real Photoshop file -- this project has had no way
    // to produce one until now.
    PsdWriter w;
    w.unicodeString("\xF0\x9F\x98\x80");
    check(hex(w.bytes()) == "00000002d83dde00",
          "writer: an astral character is a surrogate pair counted as two");
  }

  {
    const std::vector<uint16_t> u = utf8ToUtf16("h\xC3\xA9llo");
    check(u.size() == 5 && u[1] == 0x00E9, "writer: a 2-byte UTF-8 character is one unit");
  }

  // --- H. Invalid UTF-8 becomes U+FFFD, never garbage or a refusal ------
  //
  // A layer name reaches a save from wherever the document came from, and a
  // save is not worth failing over a name. What IS worth avoiding is a
  // `luni` block that is not well-formed UTF-16, which every other reader
  // of the file would then have to guess about.

  {
    const std::vector<uint16_t> u = utf8ToUtf16("\xC0\x80");  // overlong NUL
    check(u.size() == 2 && u[0] == 0xFFFD && u[1] == 0xFFFD,
          "writer: an overlong form is replaced, not decoded to NUL");
  }

  {
    const std::vector<uint16_t> u = utf8ToUtf16("\xED\xA0\x80");  // U+D800 as CESU-8
    check(u.size() == 3 && u[0] == 0xFFFD,
          "writer: a surrogate encoded as UTF-8 is replaced, not passed through");
  }

  {
    const std::vector<uint16_t> u = utf8ToUtf16("\xE2\x82");  // truncated U+20AC
    check(u.size() == 2 && u[0] == 0xFFFD && u[1] == 0xFFFD,
          "writer: a truncated sequence is replaced without overrunning");
  }

  {
    const std::vector<uint16_t> u = utf8ToUtf16("\x80\x80\x80");
    check(u.size() == 3, "writer: stray continuation bytes consume one byte each");
  }

  {
    // The property that matters most about the replacement policy: one bad
    // byte must not eat the rest of the name.
    const std::vector<uint16_t> u = utf8ToUtf16("a\xFFz");
    check(u.size() == 3 && u[0] == 'a' && u[1] == 0xFFFD && u[2] == 'z',
          "writer: a bad byte costs one character, not the rest of the name");
  }

  check(utf8ToUtf16("").empty(), "writer: an empty name is zero code units");

  return ok;
}

}  // namespace np
