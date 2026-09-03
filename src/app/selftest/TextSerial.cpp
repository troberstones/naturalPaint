#include "app/selftest/Support.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>

#include "core/Path.hpp"
#include "core/TextContent.hpp"
#include "core/VectorShape.hpp"
#include "io/TextSerial.hpp"
#include "text/Shaper.hpp"

namespace np {

// io/TextSerial -- the `np:text` carrier for a `LayerKind::Text` layer's
// content (PLAN.md phase 14; PRD K1-K3, I10, I11). io/PathSerial's sibling
// for a `TextContent` instead of a shape list: same hex-string carrier
// (docs/document-format.md's measured warning that an array-typed EXR header
// attribute is silently absent on readback), same bit-pattern floats, same
// version-in-the-prefix rule.
//
// **Deliberately headless, GPU-free, writes no files, and NOT guarded on
// `shaperAvailable()`.** A serialiser has no platform dependency -- it never
// calls `text/Shaper.hpp`'s `shapeText()` or `glyphPath()`, only reads and
// writes the plain-data fields of a `TextContent` the caller already built --
// so gating this section on the platform shaper existing would be asserting
// a dependency this code does not have, the same reason app/selftest/
// PathRaster.cpp's serial-format section is not gated on a GPU.
//
// Because `core/TextContent.cpp` is a sibling track's work-in-progress at
// this branch's base commit, every fixture below is built by field
// assignment on the aggregate, never by calling `makeTextContent()` --
// this file only needs the struct's fields to exist, which the header
// already declares.
//
// ==========================================================================
// Why the round trip is asserted field by field, twice
// ==========================================================================
//
// Section 1 builds one `TextContent` with every field at a distinctive
// value and checks each field explicitly -- not through one struct-wide
// equality helper, because a helper this file also wrote could skip the
// same field the encoder skips, and the two omissions would agree.
//
// Section 2 asks a narrower question the same way io/PathSerial's own
// section could not: for EACH field, does changing ONLY that field (leaving
// every other field at section 1's already-distinctive baseline) survive
// the wire. That is what catches a field the encoder writes once from a
// stale copy, or a field the decoder never reads back at all -- either bug
// would still pass section 1 if the baseline value happened to be the one
// both sides agree on by coincidence, but cannot pass a per-field mutation.

namespace {

bool bitEqualF32(float a, float b) {
  uint32_t ba = 0, bb = 0;
  std::memcpy(&ba, &a, sizeof(ba));
  std::memcpy(&bb, &b, sizeof(bb));
  return ba == bb;
}

bool bitEqualPoint(const PathPoint& a, const PathPoint& b) {
  return bitEqualF32(a.x, b.x) && bitEqualF32(a.y, b.y);
}

bool bitEqualPaint(const Paint& a, const Paint& b) {
  if (a.on != b.on) return false;
  for (size_t i = 0; i < 4; ++i)
    if (!bitEqualF32(a.rgba[i], b.rgba[i])) return false;
  return true;
}

bool bitEqualFloats(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (!bitEqualF32(a[i], b[i])) return false;
  return true;
}

float floatFromBits(uint32_t bits) {
  float v = 0.0f;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

// Every field set to a distinctive, non-default value -- shared by the full
// round trip and the per-field table below, so both start from the same
// fully-populated fixture rather than two hand-typed copies that could drift
// apart and silently stop testing what they claim to.
TextContent fullTextContent() {
  TextContent t;
  // "café" (é is a 2-byte UTF-8 sequence: 0xC3 0xA9), "日本語" (three
  // 3-byte sequences), "😀" (4-byte, U+1F600) -- one witness per UTF-8
  // sequence length, so a decoder that only forwards ASCII-range bytes
  // correctly cannot pass this by accident.
  t.utf8 = "caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xF0\x9F\x98\x80";
  t.style.fontFamily = "Georgia";
  t.style.sizePx = 37.5f;
  t.style.tracking = 1.25f;
  t.style.leading = 2.75f;
  t.style.bold = true;
  t.style.italic = true;
  t.frame.width = 200.0f;
  t.frame.height = 150.0f;
  t.align = TextAlign::Right;
  t.origin = PathPoint{12.5f, 34.75f};
  t.fill.on = true;
  t.fill.rgba = {0.1f, 0.2f, 0.3f, 0.4f};
  t.stroke.on = true;
  t.stroke.rgba = {0.5f, 0.6f, 0.7f, 0.8f};
  t.strokeStyle.width = 2.5f;
  t.strokeStyle.cap = LineCap::Round;
  t.strokeStyle.join = LineJoin::Bevel;
  t.strokeStyle.miterLimit = 6.25f;
  t.strokeStyle.dashes = {1.5f, 2.5f, 0.5f};
  t.strokeStyle.dashOffset = 0.75f;
  return t;
}

}  // namespace

bool runTextSerialTest() {
  bool ok = true;
  auto check = [&](bool cond, const std::string& what) {
    std::printf("  %-70s %s\n", what.c_str(), cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] io/TextSerial: the `np:text` carrier for a LayerKind::Text layer's "
              "content -- io/PathSerial's sibling for a TextContent instead of a shape list\n");

  // --- 1. Full round trip, checked field by field -------------------------
  {
    const TextContent t = fullTextContent();
    const std::string encoded = serializeTextContent(t);
    check(encoded.rfind(kTextContentSerialPrefix, 0) == 0,
          "serial: the value begins with the version prefix, so it is read before the payload");

    TextContent back;
    std::string why;
    const bool decoded = deserializeTextContent(encoded, &back, &why);
    check(decoded, "serial: and decodes cleanly");
    if (!decoded) std::printf("      decode error: %s\n", why.c_str());

    check(back.utf8 == t.utf8, "field: utf8 (2/3/4-byte UTF-8 sequences) survives byte-for-byte");
    check(back.style.fontFamily == t.style.fontFamily, "field: style.fontFamily survives");
    check(bitEqualF32(back.style.sizePx, t.style.sizePx), "field: style.sizePx survives bit-exact");
    check(bitEqualF32(back.style.tracking, t.style.tracking),
          "field: style.tracking survives bit-exact");
    check(bitEqualF32(back.style.leading, t.style.leading),
          "field: style.leading survives bit-exact");
    check(back.style.bold == t.style.bold, "field: style.bold survives");
    check(back.style.italic == t.style.italic, "field: style.italic survives");
    check(bitEqualF32(back.frame.width, t.frame.width), "field: frame.width survives bit-exact");
    check(bitEqualF32(back.frame.height, t.frame.height),
          "field: frame.height survives bit-exact");
    check(back.align == t.align, "field: align survives");
    check(bitEqualPoint(back.origin, t.origin), "field: origin survives bit-exact");
    check(bitEqualPaint(back.fill, t.fill), "field: fill (on + rgba) survives bit-exact");
    check(bitEqualPaint(back.stroke, t.stroke), "field: stroke (on + rgba) survives bit-exact");
    check(bitEqualF32(back.strokeStyle.width, t.strokeStyle.width),
          "field: strokeStyle.width survives bit-exact");
    check(back.strokeStyle.cap == t.strokeStyle.cap, "field: strokeStyle.cap survives");
    check(back.strokeStyle.join == t.strokeStyle.join, "field: strokeStyle.join survives");
    check(bitEqualF32(back.strokeStyle.miterLimit, t.strokeStyle.miterLimit),
          "field: strokeStyle.miterLimit survives bit-exact");
    check(bitEqualFloats(back.strokeStyle.dashes, t.strokeStyle.dashes),
          "field: strokeStyle.dashes survive bit-exact, including their count");
    check(bitEqualF32(back.strokeStyle.dashOffset, t.strokeStyle.dashOffset),
          "field: strokeStyle.dashOffset survives bit-exact");
  }

  // --- 2. Every field is actually carried: mutate one, round-trip, assert -
  {
    struct FieldCase {
      const char* name;
      std::function<void(TextContent&)> mutate;
      std::function<bool(const TextContent&, const TextContent&)> survived;  // (mutated, back)
    };

    const std::vector<FieldCase> cases = {
        {"utf8",
         [](TextContent& c) { c.utf8 = "changed \xE2\x82\xAC value"; },  // euro sign, 3-byte
         [](const TextContent& a, const TextContent& b) { return a.utf8 == b.utf8; }},
        {"style.fontFamily",
         [](TextContent& c) { c.style.fontFamily = "Times New Roman"; },
         [](const TextContent& a, const TextContent& b) {
           return a.style.fontFamily == b.style.fontFamily;
         }},
        {"style.sizePx",
         [](TextContent& c) { c.style.sizePx = 99.5f; },
         [](const TextContent& a, const TextContent& b) {
           return bitEqualF32(a.style.sizePx, b.style.sizePx);
         }},
        {"style.tracking",
         [](TextContent& c) { c.style.tracking = 8.25f; },
         [](const TextContent& a, const TextContent& b) {
           return bitEqualF32(a.style.tracking, b.style.tracking);
         }},
        {"style.leading",
         [](TextContent& c) { c.style.leading = 9.75f; },
         [](const TextContent& a, const TextContent& b) {
           return bitEqualF32(a.style.leading, b.style.leading);
         }},
        {"style.bold",
         [](TextContent& c) { c.style.bold = false; },  // baseline is true
         [](const TextContent& a, const TextContent& b) { return a.style.bold == b.style.bold; }},
        {"style.italic",
         [](TextContent& c) { c.style.italic = false; },  // baseline is true
         [](const TextContent& a, const TextContent& b) {
           return a.style.italic == b.style.italic;
         }},
        {"frame.width",
         [](TextContent& c) { c.frame.width = 321.0f; },
         [](const TextContent& a, const TextContent& b) {
           return bitEqualF32(a.frame.width, b.frame.width);
         }},
        {"frame.height",
         [](TextContent& c) { c.frame.height = 111.5f; },
         [](const TextContent& a, const TextContent& b) {
           return bitEqualF32(a.frame.height, b.frame.height);
         }},
        {"align",
         [](TextContent& c) { c.align = TextAlign::Justified; },  // baseline is Right
         [](const TextContent& a, const TextContent& b) { return a.align == b.align; }},
        {"origin",
         [](TextContent& c) { c.origin = PathPoint{-5.5f, 100.25f}; },
         [](const TextContent& a, const TextContent& b) { return bitEqualPoint(a.origin, b.origin); }},
        {"fill.on",
         [](TextContent& c) { c.fill.on = false; },  // baseline is true
         [](const TextContent& a, const TextContent& b) { return a.fill.on == b.fill.on; }},
        {"fill.rgba",
         [](TextContent& c) { c.fill.rgba = {0.91f, 0.82f, 0.73f, 0.64f}; },
         [](const TextContent& a, const TextContent& b) { return bitEqualPaint(a.fill, b.fill); }},
        {"stroke.on",
         [](TextContent& c) { c.stroke.on = false; },  // baseline is true
         [](const TextContent& a, const TextContent& b) { return a.stroke.on == b.stroke.on; }},
        {"stroke.rgba",
         [](TextContent& c) { c.stroke.rgba = {0.15f, 0.25f, 0.35f, 0.45f}; },
         [](const TextContent& a, const TextContent& b) {
           return bitEqualPaint(a.stroke, b.stroke);
         }},
        {"strokeStyle.width",
         [](TextContent& c) { c.strokeStyle.width = 9.125f; },
         [](const TextContent& a, const TextContent& b) {
           return bitEqualF32(a.strokeStyle.width, b.strokeStyle.width);
         }},
        {"strokeStyle.cap",
         [](TextContent& c) { c.strokeStyle.cap = LineCap::Square; },  // baseline is Round
         [](const TextContent& a, const TextContent& b) {
           return a.strokeStyle.cap == b.strokeStyle.cap;
         }},
        {"strokeStyle.join",
         [](TextContent& c) { c.strokeStyle.join = LineJoin::Miter; },  // baseline is Bevel
         [](const TextContent& a, const TextContent& b) {
           return a.strokeStyle.join == b.strokeStyle.join;
         }},
        {"strokeStyle.miterLimit",
         [](TextContent& c) { c.strokeStyle.miterLimit = 3.125f; },
         [](const TextContent& a, const TextContent& b) {
           return bitEqualF32(a.strokeStyle.miterLimit, b.strokeStyle.miterLimit);
         }},
        {"strokeStyle.dashes",
         [](TextContent& c) { c.strokeStyle.dashes = {4.5f, 0.25f}; },  // different count too
         [](const TextContent& a, const TextContent& b) {
           return bitEqualFloats(a.strokeStyle.dashes, b.strokeStyle.dashes);
         }},
        {"strokeStyle.dashOffset",
         [](TextContent& c) { c.strokeStyle.dashOffset = 1.125f; },
         [](const TextContent& a, const TextContent& b) {
           return bitEqualF32(a.strokeStyle.dashOffset, b.strokeStyle.dashOffset);
         }},
    };

    for (const FieldCase& fc : cases) {
      TextContent base = fullTextContent();
      fc.mutate(base);
      const std::string encoded = serializeTextContent(base);
      TextContent back;
      std::string why;
      const bool decoded = deserializeTextContent(encoded, &back, &why);
      check(decoded && fc.survived(base, back),
            std::string("field-table: ") + fc.name + " is carried through the wire");
    }
  }

  // --- 3. The prefix --------------------------------------------------------
  {
    const std::string encoded = serializeTextContent(fullTextContent());
    TextContent dummy;
    std::string why;

    // A future tag on a payload that is otherwise PERFECTLY VALID -- swapping
    // only the prefix, per io/PathSerial's own precedent for why this is the
    // one test that can only be refused BY the version gate.
    const std::string futureTagged =
        "nptext2:" + encoded.substr(std::strlen(kTextContentSerialPrefix));
    check(!deserializeTextContent(futureTagged, &dummy, &why),
          "serial: a FUTURE version is refused even when its payload is otherwise valid");
    check(why.find("nptext1:") != std::string::npos,
          "serial: and the refusal names the version this build speaks");

    check(!deserializeTextContent("0011223344", &dummy, &why),
          "serial: a value with no recognisable prefix at all is refused");
    check(!deserializeTextContent("", &dummy, &why), "serial: an empty value is refused outright");
    check(!deserializeTextContent(kTextContentSerialPrefix, &dummy, &why),
          "serial: the right prefix with NO payload after it is refused as truncated, not "
          "read as an empty TextContent");
  }

  // --- 4. Malformed payloads are refused, not crashed -----------------------
  {
    TextContent dummy;
    std::string why;
    check(!deserializeTextContent("nptext1:abc", &dummy, &why),
          "serial: an odd hex length is refused as truncated");
    check(!deserializeTextContent("nptext1:zz", &dummy, &why),
          "serial: a non-hex character is refused");

    const std::string encoded = serializeTextContent(fullTextContent());
    // Chop the payload back by several amounts, always leaving the version
    // prefix intact, so each attempt is a truncation of an otherwise-valid
    // record rather than a second test of the prefix check.
    for (const size_t chop : {2u, 4u, 8u, 20u, 60u}) {
      if (chop >= encoded.size()) continue;
      const std::string truncated = encoded.substr(0, encoded.size() - chop);
      const bool refused = !deserializeTextContent(truncated, &dummy, &why);
      check(refused, "serial: truncating the last " + std::to_string(chop) +
                         " hex characters is refused, not crashed");
    }

    // The allocation-bomb case requirement 1 exists for: a declared utf8
    // length of 0xFFFFFFFF with nothing behind it. Must be refused before a
    // single byte is reserved for the string.
    check(!deserializeTextContent("nptext1:ffffffff", &dummy, &why),
          "serial: a utf8 length far larger than the remaining bytes is refused before "
          "allocating");
    check(why.find("remaining") != std::string::npos,
          "serial: and the refusal names the bound that was violated");
  }

  // --- 5. Empty content round-trips -----------------------------------------
  {
    const TextContent empty;  // every field at TextContent's own default
    const std::string encoded = serializeTextContent(empty);
    check(encoded.rfind(kTextContentSerialPrefix, 0) == 0,
          "empty: still begins with the version prefix");
    check(encoded.size() > std::strlen(kTextContentSerialPrefix),
          "empty: and is not just the bare prefix -- it is a well-formed zero-length payload");

    TextContent back;
    std::string why;
    check(deserializeTextContent(encoded, &back, &why), "empty: and decodes cleanly");
    check(back.utf8.empty(), "empty: utf8 comes back empty, not garbage");
    check(back.style.fontFamily == empty.style.fontFamily,
          "empty: style.fontFamily comes back as the struct default");
    check(bitEqualF32(back.style.sizePx, empty.style.sizePx),
          "empty: style.sizePx comes back as the struct default, bit-exact");
    check(back.align == TextAlign::Left, "empty: align comes back as Left, the struct default");
    check(!back.fill.on && !back.stroke.on,
          "empty: fill and stroke both come back off, so an untouched Text layer paints nothing");
    check(back.strokeStyle.dashes.empty(), "empty: no dashes come back for none written");
  }

  // --- 6. Exactness of the hex encoding --------------------------------------
  //
  // `1.0f` plus one ulp: bit pattern 0x3F800001, decimal value
  // 1.00000011920928955078125. Chosen, not guessed, because it fails the
  // self-check just below -- printing it to 6 decimal places and reparsing
  // rounds it back to plain 1.0f, which is exactly the kind of *nearly*
  // exact round trip io/TextSerial.hpp's header says a decimal rendering
  // cannot be trusted for.
  {
    const float witness = floatFromBits(0x3F800001u);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(witness));
    const float reparsedViaDecimal = std::strtof(buf, nullptr);
    check(!bitEqualF32(reparsedViaDecimal, witness),
          "exactness: self-check -- the chosen witness genuinely loses its last bit through a "
          "6-decimal-digit round trip, so this is not a vacuous witness");

    TextContent t = fullTextContent();
    t.style.leading = witness;
    const std::string encoded = serializeTextContent(t);
    TextContent back;
    std::string why;
    const bool decoded = deserializeTextContent(encoded, &back, &why);
    check(decoded && bitEqualF32(back.style.leading, witness),
          "exactness: a float that would lose precision as decimal survives bit-identically "
          "through hex");
  }

  return ok;
}

}  // namespace np
