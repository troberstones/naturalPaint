#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "io/Descriptor.hpp"

namespace np {

// A little-endian-free Action Descriptor WRITER, for tests.
//
// Shared by two --selftest sections: io/Descriptor's own (which asserts whole
// parsed trees against fixtures it built field by field) and io/AbrBrushes'
// (which needs a valid `.abr` to import, and cannot ship one -- a real brush
// pack is somebody else's copyrighted work and megabytes besides).
//
// --- The fixture writer ---------------------------------------------------
//
// Every byte these sections parse is written here rather than read from a
// file. **That is a real weakness, stated rather than hidden**: a fixture
// proves the reader agrees with the format as documented, not that it agrees
// with Photoshop. io/AbrBrushes was additionally driven against a genuine
// 2.4 MB Kyle Webster `.abr` by hand during development (12 presets, all
// mapped) -- but that file is somebody else's copyrighted work, so it cannot
// live in the repository and cannot be what --selftest depends on.
//
// It is also what makes the *adversarial* half possible at all, which is the
// half that matters most: a corrupt file is not something you find lying
// around, it is something you build one field at a time. So the writer's whole
// job is that a fixture reads as intent -- `.key4("Dmtr").untf("#Pxl", 12.5)`
// -- and that a deliberately-broken one differs from the good one by exactly
// the line that broke it. A hex blob would hide that difference, and a fixture
// nobody can read is a fixture nobody notices is wrong.
struct DescFixture {
  std::vector<uint8_t> bytes;

  DescFixture& u8v(unsigned v) {
    bytes.push_back(static_cast<uint8_t>(v & 0xFFu));
    return *this;
  }
  DescFixture& u16v(unsigned v) {
    return u8v(v >> 8).u8v(v);
  }
  DescFixture& u32v(uint32_t v) {
    return u8v(v >> 24).u8v(v >> 16).u8v(v >> 8).u8v(v);
  }
  DescFixture& u64v(uint64_t v) {
    for (int i = 7; i >= 0; --i) u8v(static_cast<unsigned>((v >> (8 * i)) & 0xFFu));
    return *this;
  }
  // Big-endian IEEE-754 binary64, written as its bit pattern so the fixture
  // asks for exactly the double the reader must produce.
  DescFixture& f64v(double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return u64v(bits);
  }
  // Four literal bytes with no length in front: a type key, or a `UntF` unit.
  DescFixture& code(const char* fourCC) {
    for (int i = 0; i < 4; ++i) u8v(static_cast<unsigned char>(fourCC[i]));
    return *this;
  }
  // A Key in its zero-length form: "0 means four".
  DescFixture& key4(const char* fourCC) { return u32v(0).code(fourCC); }
  // A Key in its explicit-length form. Passing a four-character string here is
  // the case a reader that special-cases 4 gets wrong, and it is used below.
  DescFixture& keyN(const char* s) {
    const size_t n = std::strlen(s);
    u32v(static_cast<uint32_t>(n));
    for (size_t i = 0; i < n; ++i) u8v(static_cast<unsigned char>(s[i]));
    return *this;
  }
  // A UnicodeString from raw UTF-16 code units, written verbatim -- the form
  // the surrogate fixtures need.
  DescFixture& unicodeUnits(const std::vector<uint16_t>& units) {
    u32v(static_cast<uint32_t>(units.size()));
    for (const uint16_t unit : units) u16v(unit);
    return *this;
  }
  // A UnicodeString the way Photoshop writes one: ASCII plus a trailing NUL
  // that is counted in the length and must not come back in the string.
  DescFixture& unicode(const char* ascii) {
    std::vector<uint16_t> units;
    for (const char* p = ascii; *p != '\0'; ++p)
      units.push_back(static_cast<uint16_t>(static_cast<unsigned char>(*p)));
    units.push_back(0);
    return unicodeUnits(units);
  }

  // A descriptor header: class name, class id, item count.
  DescFixture& descriptor(const char* className, const char* classId, uint32_t items) {
    return unicode(className).key4(classId).u32v(items);
  }
  DescFixture& version() { return u32v(kActionDescriptorVersion); }

  DescFixture& textv(const char* s) { return code("TEXT").unicode(s); }
  DescFixture& untf(const char* unit, double v) { return code("UntF").code(unit).f64v(v); }
  DescFixture& doubv(double v) { return code("doub").f64v(v); }
  DescFixture& longv(int32_t v) { return code("long").u32v(static_cast<uint32_t>(v)); }
  DescFixture& compv(int64_t v) { return code("comp").u64v(static_cast<uint64_t>(v)); }
  DescFixture& boolv(bool v) { return code("bool").u8v(v ? 1u : 0u); }
  // An `enum` value: the type key, then a Key for its type id and a Key for
  // its value id -- `io/Descriptor.cpp`'s `readKey()` called twice, matching
  // `DescriptorRef::asEnumerated()`'s `{typeId, valueId}` pair. Both ids go
  // through `key4()`, the zero-length form, since every enum this build reads
  // (Photoshop's `BlnM`) is a plain four-character code.
  DescFixture& enumv(const char* typeId, const char* valueId) {
    return code("enum").key4(typeId).key4(valueId);
  }
  // An `enum` value whose VALUE id may be longer than four characters --
  // Photoshop's "second-generation" `BlnM` ids (`hardMix`, `linearHeight`,
  // `vividLight`...) are exactly this shape, and `enumv()` above cannot write
  // them: it forces the value id through `key4()`, which reads -- and
  // therefore writes -- exactly four bytes, silently TRUNCATING anything
  // longer (`"hardMix"` becomes the four bytes `"hard"`) rather than failing
  // loudly. That is `keyN()`'s own "a reader that special-cases 4 gets
  // wrong" case, hit here as a writer bug instead of a reader one -- found
  // by a fixture whose value id came back truncated in a real `--selftest`
  // run, not by inspection.
  //
  // Same shape as `enumv()` otherwise: `code("enum")`, the type id through
  // `key4()` (every `BlnM` TYPE id this build reads is the literal
  // four-character type "BlnM" itself, never long), then the VALUE id
  // through `keyN()`'s explicit-length form -- which `io/Descriptor.cpp`'s
  // `readKey()` reads identically to the zero-length form when the value
  // happens to be exactly four characters (`declared == 4` and `declared ==
  // 0` both resolve to `n == 4`), so this is a strict superset of `enumv()`,
  // not a second convention with its own edge cases.
  DescFixture& enumvLong(const char* typeId, const char* valueId) {
    code("enum").key4(typeId);
    return keyN(valueId);
  }
  DescFixture& tdta(const std::vector<uint8_t>& payload) {
    code("tdta").u32v(static_cast<uint32_t>(payload.size()));
    for (const uint8_t byte : payload) u8v(byte);
    return *this;
  }
  DescFixture& alis(const std::vector<uint8_t>& payload) {
    code("alis").u32v(static_cast<uint32_t>(payload.size()));
    for (const uint8_t byte : payload) u8v(byte);
    return *this;
  }

  // An `Objc` value: the type key, then a descriptor header. Items follow.
  DescFixture& objc(const char* className, const char* classId, uint32_t items) {
    return code("Objc").descriptor(className, classId, items);
  }
  // A `VlLs` value: the type key, then a count. Elements follow, each a bare
  // type-plus-value with NO key of its own -- which is exactly why
  // DescriptorRef::field() never matches a list element.
  DescFixture& vlls(uint32_t count) { return code("VlLs").u32v(count); }
};

// PackBits, as a test ENCODER -- the mirror of io/PackBits.cpp's decoder.
//
// Lives here rather than in one selftest TU because two of them now need it
// (AbrSampledTips for `samp`, PsPatterns for `patt`), and two encoders that
// could disagree about the row-length table would let a decoder bug hide
// behind a matching encoder bug in exactly one of them.
// PackBits-encodes `raw` (`width` x `height`) as one literal run per scanline
// -- valid for `width <= 128`, which is every fixture below. Good enough to
// prove the row-length table and the literal opcode; the run (negative n) and
// NOP (-128) opcodes are exercised separately, hand-built, where the decoder's
// handling of THOSE specifically is what a test is about.
inline std::vector<uint8_t> packBitsLiteralRows(const std::vector<uint8_t>& raw, uint32_t width,
                                               uint32_t height) {
  DescFixture rowLens;
  DescFixture stream;
  for (uint32_t y = 0; y < height; ++y) {
    DescFixture row;
    row.u8v(width - 1);  // literal opcode: `width` bytes follow, verbatim
    for (uint32_t x = 0; x < width; ++x) row.u8v(raw[y * width + x]);
    rowLens.u16v(static_cast<unsigned>(row.bytes.size()));
    for (const uint8_t b : row.bytes) stream.u8v(b);
  }
  DescFixture out;
  for (const uint8_t b : rowLens.bytes) out.u8v(b);
  for (const uint8_t b : stream.bytes) out.u8v(b);
  return out.bytes;
}


}  // namespace np
