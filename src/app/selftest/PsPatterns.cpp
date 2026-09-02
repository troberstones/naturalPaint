#include "app/selftest/Support.hpp"

#include <cstdio>

#include "app/selftest/DescFixture.hpp"
#include "io/PackBits.hpp"
#include "io/PsPatterns.hpp"

namespace np {
namespace {

// --- Fixture builder -------------------------------------------------------
//
// Every byte below is written by hand, the same discipline every other `.abr`
// section's fixtures follow (app/selftest/DescFixture.hpp's header): a real
// pack is somebody else's copyrighted work and nothing here is read from one.
//
// The framing being built is Adobe's published Pattern structure plus the
// Virtual Memory Array List. That it is published is why these fixtures can be
// asserted against a specification rather than against one file -- but the
// three things the specification does not say (the bogus `numberOfChannels`,
// the four-byte record alignment, the short tail) were measured off real packs,
// and each has its own section below because each is a way to get this wrong
// that still produces plausible output.

// One Virtual Memory Array entry: `written`, `length`, then -- inside `length`
// -- `u32 depth`, the rectangle, `u16 depth`, `u8 compression`, the data.
std::vector<uint8_t> buildChannel(int32_t width, int32_t height, uint16_t depth,
                                  uint8_t compression, const std::vector<uint8_t>& data) {
  DescFixture inner;
  inner.u32v(depth);
  inner.u32v(0).u32v(0).u32v(static_cast<uint32_t>(height)).u32v(static_cast<uint32_t>(width));
  inner.u16v(depth);
  inner.u8v(compression);
  for (const uint8_t b : data) inner.u8v(b);

  DescFixture out;
  out.u32v(1);  // written
  out.u32v(static_cast<uint32_t>(inner.bytes.size()));
  for (const uint8_t b : inner.bytes) out.u8v(b);
  return out.bytes;
}

// One whole pattern record, including its own 4-byte length prefix.
//
// `declaredChannels` is written into the `numberOfChannels` field verbatim, so
// a test can put the 24 that real files carry there and prove the reader does
// not use it. `shortTail` appends the four unexplained bytes every real record
// ends with.
std::vector<uint8_t> buildPatternRecord(uint32_t mode, uint16_t width, uint16_t height,
                                        const char* name, const char* id,
                                        const std::vector<std::vector<uint8_t>>& channels,
                                        uint32_t declaredChannels, bool shortTail) {
  DescFixture body;
  body.u32v(1);     // version
  body.u32v(mode);  // image mode
  body.u16v(height);
  body.u16v(width);
  body.unicode(name);  // u32 code-unit count + UTF-16BE, NUL included

  size_t idLen = 0;
  for (const char* p = id; *p != '\0'; ++p) ++idLen;
  body.u8v(static_cast<unsigned>(idLen));  // a Pascal string, NOT a '$' sigil
  for (const char* p = id; *p != '\0'; ++p) body.u8v(static_cast<unsigned char>(*p));

  DescFixture vma;
  vma.u32v(0).u32v(0).u32v(height).u32v(width);  // the list's own rectangle
  vma.u32v(declaredChannels);
  for (const auto& c : channels)
    for (const uint8_t b : c) vma.u8v(b);
  if (shortTail) vma.u32v(0);

  body.u32v(3);  // VMA list version
  body.u32v(static_cast<uint32_t>(vma.bytes.size()));
  for (const uint8_t b : vma.bytes) body.u8v(b);

  DescFixture rec;
  rec.u32v(static_cast<uint32_t>(body.bytes.size()));
  for (const uint8_t b : body.bytes) rec.u8v(b);
  return rec.bytes;
}

// Concatenates records with the block's own FOUR-byte alignment between them.
std::vector<uint8_t> buildPattSection(const std::vector<std::vector<uint8_t>>& records) {
  std::vector<uint8_t> out;
  for (const auto& rec : records) {
    out.insert(out.end(), rec.begin(), rec.end());
    while (out.size() % 4 != 0) out.push_back(0);
  }
  return out;
}

}  // namespace

bool runPsPatternsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const std::vector<uint8_t> grey4x3 = {10, 20, 30, 40, 50,  60,
                                        70, 80, 90, 100, 110, 120};

  // ==========================================================================
  std::printf("  -- A. record framing --\n");
  // ==========================================================================
  {
    const auto rec = buildPatternRecord(
        /*mode=*/1, 4, 3, "Extra Heavy Canvas", "77f45758-b4c9-11d5-8d4a-c3e50c023def",
        {buildChannel(4, 3, 8, 0, grey4x3)}, /*declaredChannels=*/24, /*shortTail=*/true);
    const PsPatternResult r = parseAbrPatterns(buildPattSection({rec}));

    check(r.patterns.size() == 1 && r.skipped == 0 && !r.truncated,
          "patt: one well-formed greyscale record decodes to one pattern");
    if (r.patterns.size() == 1) {
      check(r.patterns[0].id == "77f45758-b4c9-11d5-8d4a-c3e50c023def",
            "patt: the id is the Pascal string's bytes, its LENGTH byte consumed");
      check(r.patterns[0].name == "Extra Heavy Canvas",
            "patt: the Unicode name transcodes to UTF-8 without its NUL");
      check(r.patterns[0].width == 4 && r.patterns[0].height == 3,
            "patt: horizontal is width and vertical is height, not transposed");
      check(r.patterns[0].height8 == grey4x3,
            "patt: raw channel bytes arrive verbatim, in order");
    }
  }

  // ==========================================================================
  std::printf("  -- B. numberOfChannels is not a channel count --\n");
  // ==========================================================================
  {
    // **The single most likely way to get this parser wrong.** Every real
    // record declares 24 and carries one or three written channels.
    //
    // Two assertions, in opposite directions, because ONE does not discriminate
    // -- and this was found by sabotage, not by inspection. Asserting that
    // declaring 1 and declaring 24 decode identically passes even for a reader
    // that loops `numberOfChannels` times, because 24 is an over-count and the
    // inner bound stops it first. That assertion was written, measured to
    // survive the sabotage it was written to catch, and replaced.
    //
    // Direction one: declare FEWER channels than are present. A reader that
    // trusts the field decodes nothing and skips the record; one that ignores
    // it decodes the pattern.
    const auto underCount = buildPatternRecord(
        1, 4, 3, "under", "id-0", {buildChannel(4, 3, 8, 0, grey4x3)}, /*declaredChannels=*/0,
        true);
    const PsPatternResult u = parseAbrPatterns(buildPattSection({underCount}));
    check(u.patterns.size() == 1 && u.patterns[0].height8 == grey4x3,
          "patt: a record declaring 0 channels still decodes the one it carries");

    // Direction two: declare MORE, as every real record does, and put a second
    // record after it. A reader bounded by the field rather than by the VMA
    // list's own length runs past the first record and never finds the second.
    const auto overCount = buildPatternRecord(1, 4, 3, "over", "id-0",
                                             {buildChannel(4, 3, 8, 0, grey4x3)}, 24, true);
    const auto two = buildPattSection({overCount, buildPatternRecord(1, 4, 3, "second", "id-1",
                                                                {buildChannel(4, 3, 8, 0, grey4x3)},
                                                                24, true)});
    const PsPatternResult r = parseAbrPatterns(two);
    check(r.patterns.size() == 2 && r.patterns[1].name == "second",
          "patt: the VMA list's own length bounds the walk, so record 2 is found");
  }

  // ==========================================================================
  std::printf("  -- C. four-byte record alignment --\n");
  // ==========================================================================
  {
    // A name chosen so the first record's length is NOT a multiple of four.
    // A walk that advances by `length` alone lands one to three bytes early
    // and reads the next record's version word out of the middle of the
    // previous one -- which decodes to a plausible-looking name rather than
    // failing, and is exactly how this presents when it is wrong.
    const auto first = buildPatternRecord(1, 4, 3, "odd", "abc", {buildChannel(4, 3, 8, 0, grey4x3)},
                                          24, true);
    const auto second = buildPatternRecord(1, 4, 3, "aligned", "def",
                                           {buildChannel(4, 3, 8, 0, grey4x3)}, 24, true);
    const PsPatternResult r = parseAbrPatterns(buildPattSection({first, second}));
    check(r.patterns.size() == 2 && r.patterns[0].name == "odd" &&
              r.patterns[1].name == "aligned",
          "patt: records are 4-byte aligned, so an odd-length one does not desync");
  }

  // ==========================================================================
  std::printf("  -- D. modes, depth and compression --\n");
  // ==========================================================================
  {
    // RGB collapses to luminance, because paper tooth is a scalar height field
    // (brush/Grain's G is one number). Rec.601 integer weights: (77r + 150g +
    // 29b) >> 8.
    const std::vector<uint8_t> r4 = {255, 0, 0, 128};
    const std::vector<uint8_t> g4 = {0, 255, 0, 128};
    const std::vector<uint8_t> b4 = {0, 0, 255, 128};
    const auto rec = buildPatternRecord(
        3, 4, 1, "rgb", "rgb-id",
        {buildChannel(4, 1, 8, 0, r4), buildChannel(4, 1, 8, 0, g4), buildChannel(4, 1, 8, 0, b4)},
        24, true);
    const PsPatternResult res = parseAbrPatterns(buildPattSection({rec}));
    check(res.patterns.size() == 1 && res.patterns[0].height8.size() == 4,
          "patt: an RGB (mode 3) record decodes its three channels");
    if (res.patterns.size() == 1) {
      const auto& h = res.patterns[0].height8;
      check(h[0] == static_cast<uint8_t>((255u * 77u) >> 8) &&
                h[1] == static_cast<uint8_t>((255u * 150u) >> 8) &&
                h[2] == static_cast<uint8_t>((255u * 29u) >> 8) &&
                h[3] == static_cast<uint8_t>((128u * 77u + 128u * 150u + 128u * 29u) >> 8),
            "patt: RGB collapses to Rec.601 luminance, per channel, exactly");
    }

    // The same pixels through the PackBits path must equal the raw path --
    // the shared encoder in DescFixture.hpp is the decoder's mirror.
    const auto rawRec = buildPatternRecord(1, 4, 3, "raw", "raw-id",
                                           {buildChannel(4, 3, 8, 0, grey4x3)}, 24, true);
    const auto rleRec =
        buildPatternRecord(1, 4, 3, "rle", "rle-id",
                           {buildChannel(4, 3, 8, 1, packBitsLiteralRows(grey4x3, 4, 3))}, 24, true);
    const PsPatternResult raw = parseAbrPatterns(buildPattSection({rawRec}));
    const PsPatternResult rle = parseAbrPatterns(buildPattSection({rleRec}));
    check(raw.patterns.size() == 1 && rle.patterns.size() == 1 &&
              raw.patterns[0].height8 == rle.patterns[0].height8,
          "patt: a PackBits channel decodes to the identical bytes as a raw one");

    // 16-bit is refused by name rather than reinterpreted -- the same call
    // parseAbrSampledTips() makes for a sampled tip, on the same evidence
    // (every channel in every real pack examined is 8-bit).
    const auto deep = buildPatternRecord(1, 4, 3, "deep", "deep-id",
                                         {buildChannel(4, 3, 16, 0, grey4x3)}, 24, true);
    const PsPatternResult d = parseAbrPatterns(buildPattSection({deep}));
    check(d.patterns.empty() && d.skipped == 1,
          "patt: a 16-bit channel is SKIPPED and counted, not reinterpreted");

    // An unsupported image mode is skipped whole, not half-decoded.
    const auto cmyk = buildPatternRecord(4, 4, 3, "cmyk", "cmyk-id",
                                         {buildChannel(4, 3, 8, 0, grey4x3)}, 24, true);
    const PsPatternResult c = parseAbrPatterns(buildPattSection({cmyk}));
    check(c.patterns.empty() && c.skipped == 1,
          "patt: an image mode this reader cannot use is skipped and counted");
  }

  // ==========================================================================
  std::printf("  -- E. the block's own edges --\n");
  // ==========================================================================
  {
    // threeOtherBrushes.abr really does carry `8BIM patt` with length 0. That
    // is "no patterns", not "broken file", and a reader that refuses it
    // refuses a pack that opens fine in Photoshop.
    const PsPatternResult empty = parseAbrPatterns({});
    check(empty.patterns.empty() && empty.skipped == 0 && !empty.truncated,
          "patt: a zero-length block is zero patterns, not an error");

    // Every truncation of a well-formed block: none may read out of bounds,
    // none may invent a pattern, and each must decode a prefix of what the
    // whole block decodes. Same shape as AbrSampledTips' own truncation loop.
    const auto whole = buildPattSection(
        {buildPatternRecord(1, 4, 3, "one", "id-1", {buildChannel(4, 3, 8, 0, grey4x3)}, 24, true),
         buildPatternRecord(1, 4, 3, "two", "id-2", {buildChannel(4, 3, 8, 0, grey4x3)}, 24, true)});
    const PsPatternResult full = parseAbrPatterns(whole);
    bool everyPrefixSane = true;
    for (size_t n = 0; n < whole.size(); ++n) {
      const PsPatternResult part =
          parseAbrPatterns(std::span<const uint8_t>(whole.data(), n));
      if (part.patterns.size() > full.patterns.size()) everyPrefixSane = false;
      for (size_t i = 0; i < part.patterns.size(); ++i)
        if (part.patterns[i].id != full.patterns[i].id) everyPrefixSane = false;
    }
    check(full.patterns.size() == 2 && everyPrefixSane,
          "patt: every truncation decodes a prefix of the whole, and never more");

    // The four-byte short tail every real record carries. Without it the walk
    // must still stop cleanly rather than reporting the record malformed.
    const auto noTail = buildPatternRecord(1, 4, 3, "notail", "id-3",
                                           {buildChannel(4, 3, 8, 0, grey4x3)}, 24, false);
    const PsPatternResult t = parseAbrPatterns(buildPattSection({noTail}));
    check(t.patterns.size() == 1 && t.skipped == 0,
          "patt: a record with no short tail decodes the same as one with it");
  }

  std::printf("[selftest] ps patterns %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
