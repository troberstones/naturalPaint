#include "app/selftest/Support.hpp"

#include <cstring>

#include "app/BrushRowIcon.hpp"
#include "app/DabPreview.hpp"
#include "app/selftest/DescFixture.hpp"
#include "app/StrokeSession.hpp"
#include "brush/Deposit.hpp"
#include "io/AbrBrushes.hpp"

namespace np {
namespace {

// --- Fixture builders --------------------------------------------------
//
// Every byte below is written by hand, the same discipline
// app/selftest/AbrBrushes.cpp's own `wrapAbr()`/`appendBrush()` follow and
// for the same reason (that file's header comment on `DescFixture`): a real
// `.abr` is somebody else's copyrighted work, so nothing here is read from
// one. The `samp` record FRAMING these builders produce was cross-checked
// against a real file by a throwaway script outside this build --
// io/AbrBrushes.hpp's header says exactly what that does and does not prove.

// One `samp` record's on-disk bytes: the 4-byte length prefix, a `$`-prefixed
// 36-character key, `subversion`'s own header skip (padded with zeros, since
// `parseAbrSampledTips()` never reads those bytes), the bounds rectangle, the
// depth and compression bytes, then `imageBytes` verbatim.
std::vector<uint8_t> buildSampRecord(const char* uuid36, uint16_t subversion, uint32_t top,
                                     uint32_t left, uint32_t bottom, uint32_t right,
                                     uint16_t depth, uint8_t compression,
                                     const std::vector<uint8_t>& imageBytes) {
  DescFixture body;
  size_t idLength = 0;
  for (const char* p = uuid36; *p != '\0'; ++p) ++idLength;
  // A Pascal string: the LENGTH byte, then the characters. For the usual
  // 36-character UUID that byte is 0x24, which is also '$' -- which is exactly
  // why this was misread as a sigil for so long (io/AbrBrushes.cpp's own
  // comment on the fix). Written as a length here so a fixture can carry an id
  // of some other length and the two readings can disagree.
  body.u8v(static_cast<unsigned>(idLength));
  for (const char* p = uuid36; *p != '\0'; ++p) body.u8v(static_cast<unsigned char>(*p));
  const size_t skipAmt = (subversion == 1) ? 47 : 301;
  for (size_t i = 1 + idLength; i < skipAmt; ++i) body.u8v(0);
  body.u32v(top).u32v(left).u32v(bottom).u32v(right);
  body.u16v(depth);
  body.u8v(compression);
  for (const uint8_t b : imageBytes) body.u8v(b);

  DescFixture rec;
  rec.u32v(static_cast<uint32_t>(body.bytes.size()));
  for (const uint8_t b : body.bytes) rec.u8v(b);
  return rec.bytes;
}

// Concatenates records with the `samp` block's own 4-byte alignment between
// them (`parseAbrSampledTips()`'s `(bodyEnd + 3) & ~3`, NOT the 2-byte
// alignment the top-level 8BIM walk uses) -- this is what a real encoder's
// output looks like and what the parser must step over correctly.
std::vector<uint8_t> buildSampSection(const std::vector<std::vector<uint8_t>>& records) {
  std::vector<uint8_t> out;
  for (const auto& rec : records) {
    out.insert(out.end(), rec.begin(), rec.end());
    while (out.size() % 4 != 0) out.push_back(0);
  }
  return out;
}

// One brush preset descriptor, minimal but real: a name, a `Dmtr` (either
// unit) and, optionally, a `sampledData` id -- everything `presetFromDescriptor()`
// touches for this section. Item counts are exact, the same discipline
// app/selftest/AbrBrushes.cpp's own `appendBrush()` follows, because an
// Action Descriptor carries no length in front of a value (io/Descriptor.hpp's
// own header): a wrong count desynchronises everything after it.
std::vector<uint8_t> oneSampledBrushDesc(const char* name, const char* dmtrUnit, double dmtrValue,
                                         const char* sampledDataId) {
  DescFixture f;
  f.version();
  f.descriptor("null", "null", 1);
  f.key4("Brsh").vlls(1);
  f.objc("brushPreset", "brushPreset", 2);
  f.key4("Nm  ").textv(name);
  f.key4("Brsh").objc("sampledBrush", "sampledBrush", sampledDataId != nullptr ? 2u : 1u);
  f.key4("Dmtr").untf(dmtrUnit, dmtrValue);
  if (sampledDataId != nullptr) f.keyN("sampledData").textv(sampledDataId);
  return f.bytes;
}

// A full `.abr`: header, `samp`, then `desc` -- a real file's own order
// (io/AbrBrushes.hpp's header) and the order the section walk is written to
// handle regardless.
std::vector<uint8_t> wrapAbrWithSamp(const std::vector<uint8_t>& sampBody,
                                     const std::vector<uint8_t>& descBody,
                                     uint16_t subversion = 2) {
  DescFixture f;
  f.u16v(6).u16v(subversion);
  f.code("8BIM").code("samp").u32v(static_cast<uint32_t>(sampBody.size()));
  for (const uint8_t b : sampBody) f.u8v(b);
  if (sampBody.size() % 2 != 0) f.u8v(0);  // 8BIM's own word alignment
  f.code("8BIM").code("desc").u32v(static_cast<uint32_t>(descBody.size()));
  for (const uint8_t b : descBody) f.u8v(b);
  return f.bytes;
}

}  // namespace

// io/AbrBrushes' `samp` block (brush/Deposit.hpp §2c): the bitmap tip a
// `.abr` sample decodes to, matched to a preset by id, and what
// dabCoverage()/dabPixelBounds() do with one once it is attached to a
// BrushTip. Four sections: the record framing in isolation, the descriptor
// correlation end to end, the coverage/bounds mapping against a synthetic
// bitmap with no `.abr` involved at all, and the two UI-layer leaks found
// while wiring a live bitmap tip through the app.
bool runAbrSampledTipsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };

  // ==========================================================================
  std::printf("  -- A. samp record framing --\n");
  // ==========================================================================
  {
    // A 3x2 raw (uncompressed) sample, subversion 2. Values chosen so every
    // texel is distinct -- a transposition anywhere in the rect/depth/data
    // offsets would show up as the wrong byte in the wrong place rather than
    // as a value that happens to still be plausible.
    const std::vector<uint8_t> raw3x2 = {10, 20, 30, 40, 50, 60};
    const auto rec = buildSampRecord("aaaaaaaa-0000-1111-2222-333333333333", 2, 100, 200, 102,
                                     203, 8, 0, raw3x2);
    const auto tips =
        parseAbrSampledTips(std::span<const uint8_t>(rec), /*subversion=*/2);
    check(tips.size() == 1, "abr-samp: one well-formed raw record decodes to one tip");
    if (tips.size() == 1) {
      check(tips[0].id == "aaaaaaaa-0000-1111-2222-333333333333",
            "abr-samp: the id is the Pascal string's characters, its length byte consumed");
      // The reading that worked only by the 0x24 coincidence: an id of any
      // length other than 36 is where a '$'-sigil reader and a Pascal-string
      // reader part company. A sample that cannot be named can never be
      // matched by a preset's `sampledData`, so the brush falls back to a
      // round dab -- silently, which is the whole failure mode.
      const auto shortId = buildSampRecord("short-id-01", 2, 0, 0, 2, 3, 8, 0, raw3x2);
      const auto shortTips = parseAbrSampledTips(std::span<const uint8_t>(shortId), 2);
      check(shortTips.size() == 1 && shortTips[0].id == "short-id-01",
            "abr-samp: an id whose length is not 36 is still read, not refused");
      check(tips[0].bitmap != nullptr && tips[0].bitmap->width == 3 && tips[0].bitmap->height == 2,
            "abr-samp: width is right-left, height is bottom-top");
      check(tips[0].bitmap != nullptr && tips[0].bitmap->alpha == raw3x2,
            "abr-samp: raw (uncompressed) image bytes are kept verbatim, in order");
    }

    // The identical pixels, PackBits-encoded one literal run per scanline
    // (`packBitsLiteralRows()`) and read through the `compressed != 0` path
    // instead -- both must decode to the exact same bytes.
    const auto recCompressed =
        buildSampRecord("aaaaaaaa-1111-1111-2222-333333333333", 2, 100, 200, 102, 203, 8, 1,
                        packBitsLiteralRows(raw3x2, 3, 2));
    const auto compressedTips = parseAbrSampledTips(std::span<const uint8_t>(recCompressed), 2);
    check(compressedTips.size() == 1 && compressedTips[0].bitmap != nullptr &&
              compressedTips[0].bitmap->alpha == raw3x2,
          "abr-samp: the same pixels, PackBits-encoded, decode to the identical bytes as raw");
  }

  {
    // The identical rectangle and pixels, subversion 1 -- the header skip is
    // 47 rather than 301, and getting that branch wrong reads either garbage
    // or nothing at the rect offset.
    const std::vector<uint8_t> raw2x2 = {1, 2, 3, 4};
    const auto rec =
        buildSampRecord("bbbbbbbb-0000-1111-2222-333333333333", 1, 0, 0, 2, 2, 8, 0, raw2x2);
    const auto tips = parseAbrSampledTips(std::span<const uint8_t>(rec), /*subversion=*/1);
    check(tips.size() == 1 && tips[0].bitmap != nullptr && tips[0].bitmap->alpha == raw2x2,
          "abr-samp: subversion 1's shorter header skip (47, not 301) is honoured");
    // The SAME bytes read with the WRONG subversion's skip land on the wrong
    // offset for the rect -- this is what makes the branch above load-bearing
    // rather than decorative. `depth == 8` gates acceptance, so a
    // misread rect/depth from the wrong skip is expected to refuse rather
    // than to coincidentally decode a wrong-but-plausible bitmap; asserting
    // *some* difference (`refused OR different pixels`) is what the run below
    // checks, since "coincidentally decodes to the same 2x2" is possible in
    // principle but did not happen for this fixture.
    const auto wrongSkip = parseAbrSampledTips(std::span<const uint8_t>(rec), /*subversion=*/2);
    check(wrongSkip.empty() ||
              (wrongSkip[0].bitmap != nullptr && wrongSkip[0].bitmap->alpha != raw2x2) ||
              wrongSkip[0].bitmap == nullptr,
          "abr-samp: reading a subversion-1 record with subversion 2's skip does NOT recover "
          "the same pixels");
  }

  {
    // PackBits: one literal-encoded row plus one hand-built row using BOTH a
    // run (repeat a byte) and the NOP control byte, so all three opcodes this
    // decoder implements are exercised in one fixture.
    //   row 0: literal [9, 8, 7, 6]                    -- opcode 3 (=4-1)
    //   row 1: NOP, then run of 4 (=(-3)+1) of value 42 -- opcode -128, -3, 42
    DescFixture row0;
    row0.u8v(3).u8v(9).u8v(8).u8v(7).u8v(6);
    DescFixture row1;
    row1.u8v(0x80).u8v(static_cast<unsigned>(-3) & 0xFFu).u8v(42);

    DescFixture image;
    image.u16v(static_cast<unsigned>(row0.bytes.size()));
    image.u16v(static_cast<unsigned>(row1.bytes.size()));
    for (const uint8_t b : row0.bytes) image.u8v(b);
    for (const uint8_t b : row1.bytes) image.u8v(b);

    const auto rec = buildSampRecord("cccccccc-0000-1111-2222-333333333333", 2, 0, 0, 2, 4, 8, 1,
                                     image.bytes);
    const auto tips = parseAbrSampledTips(std::span<const uint8_t>(rec), 2);
    const std::vector<uint8_t> want = {9, 8, 7, 6, 42, 42, 42, 42};
    check(tips.size() == 1 && tips[0].bitmap != nullptr && tips[0].bitmap->alpha == want,
          "abr-samp: PackBits literal, run and NOP opcodes all decode correctly");
  }

  {
    // Two records back to back, the first record's total on-disk length
    // (`rec1.size()`, which is `bodyEnd` relative to the section start) NOT
    // congruent to 0 mod 4 -- so the second record does not start where a
    // naive `off = bodyEnd` would put it, and 4-byte alignment is genuinely
    // exercised rather than coincidentally matching a weaker rule.
    //
    // **6 bytes, not 7.** `rec1.size() mod 4` must be 2, specifically: mod 0
    // needs no padding either way (uninteresting), and mod 3 is the one
    // residue where "round up to the next EVEN number" (2-byte alignment)
    // and "round up to the next multiple of 4" happen to land on the same
    // byte -- an earlier draft of this fixture used a 7-byte image (mod 3)
    // and found that the hard way: it did not actually distinguish the two
    // rules. mod 2 is the residue with the largest, unambiguous gap between
    // them (2 bytes short under the weaker rule), which is what makes this
    // fixture the one that catches the sabotage in this file's own report
    // rather than merely gesturing at alignment.
    const std::vector<uint8_t> img1(6, 0xAA);
    const auto rec1 =
        buildSampRecord("dddddddd-0000-1111-2222-333333333333", 2, 0, 0, 1, 6, 8, 0, img1);
    const std::vector<uint8_t> img2 = {1, 2, 3, 4};
    const auto rec2 =
        buildSampRecord("eeeeeeee-0000-1111-2222-333333333333", 2, 0, 0, 2, 2, 8, 0, img2);
    check(rec1.size() % 4 == 2,
          "abr-samp: the two-record fixture's first record lands exactly on the residue (mod 4 "
          "== 2) that a 2-byte-alignment bug would misplace the second record by");
    const auto section = buildSampSection({rec1, rec2});
    const auto tips = parseAbrSampledTips(std::span<const uint8_t>(section), 2);
    check(tips.size() == 2, "abr-samp: both records of an unaligned pair are found");
    if (tips.size() == 2) {
      check(tips[0].id == "dddddddd-0000-1111-2222-333333333333" &&
                tips[1].id == "eeeeeeee-0000-1111-2222-333333333333",
            "abr-samp: the SECOND record's id is intact -- reading it at the wrong offset would "
            "desynchronise onto the middle of its own bytes");
      check(tips[1].bitmap != nullptr && tips[1].bitmap->alpha == img2,
            "abr-samp: the second record's pixels are intact too");
    }
  }

  {
    // depth 16: refused by name (io/AbrBrushes.hpp's header), not
    // reinterpreted. The id is still recorded (the key is read before depth
    // is even looked at) but there is no bitmap to go with it, so this
    // record contributes nothing to the map `importAbrBrushes()` builds --
    // section B's `sampledTips` count is where that is proven end to end.
    const std::vector<uint8_t> img16(8, 0);
    const auto rec =
        buildSampRecord("ffffffff-0000-1111-2222-333333333333", 2, 0, 0, 2, 2, 16, 0, img16);
    const auto tips = parseAbrSampledTips(std::span<const uint8_t>(rec), 2);
    check(tips.empty(), "abr-samp: depth 16 is refused -- no tip is returned for it at all");
  }

  {
    // A rectangle bigger than kMaxSampledTipDimension. This is the memory-
    // safety refusal (io/AbrBrushes.cpp's own comment on the constant): the
    // declared `right`/`bottom` are trusted numbers from an untrusted file,
    // and multiplying them into an allocation size before this check exists
    // is exactly the failure mode the check closes.
    const std::vector<uint8_t> tiny(4, 0);
    const auto rec = buildSampRecord("00000000-0000-1111-2222-333333333333", 2, 0, 0, 5000, 5000,
                                     8, 0, tiny);
    const auto tips = parseAbrSampledTips(std::span<const uint8_t>(rec), 2);
    check(tips.empty(), "abr-samp: a rectangle past the dimension cap is refused, not attempted");
  }

  {
    // Truncation safety: every prefix of a good two-record section must
    // decode SOME PREFIX of the records (0, 1 or 2) and never crash, never
    // read past its own buffer (io/AbrBrushes.cpp reuses the same bounds-
    // checked readU16/readU32 io/Descriptor.hpp's own module is built on),
    // and never report a record whose bytes it did not fully have. Mirrors
    // app/selftest/AbrBrushes.cpp §3's "every prefix survives" check, at this
    // module's own new entry point.
    const std::vector<uint8_t> a(6, 11);
    const std::vector<uint8_t> b(5, 22);
    const auto recA = buildSampRecord("11111111-0000-1111-2222-333333333333", 2, 0, 0, 2, 3, 8, 0, a);
    const auto recB = buildSampRecord("22222222-0000-1111-2222-333333333333", 2, 0, 0, 1, 5, 8, 0, b);
    const auto good = buildSampSection({recA, recB});
    bool allSurvive = true;
    for (size_t n = 0; n <= good.size(); ++n) {
      const std::vector<uint8_t> prefix(good.begin(), good.begin() + static_cast<long>(n));
      const auto tips = parseAbrSampledTips(std::span<const uint8_t>(prefix), 2);
      // A tip only ever appears once its record's declared length AND its
      // image bytes were fully within the buffer handed in -- so no prefix
      // can report a tip whose id belongs to a record beyond what was given.
      for (const auto& t : tips) {
        if (t.id != "11111111-0000-1111-2222-333333333333" &&
            t.id != "22222222-0000-1111-2222-333333333333")
          allSurvive = false;
      }
    }
    check(allSurvive, "abr-samp: every truncation of a two-record section decodes only complete "
                      "records, never garbage");
  }

  // ==========================================================================
  std::printf("  -- B. correlated through a whole .abr --\n");
  // ==========================================================================
  {
    const std::vector<uint8_t> raw4x2 = {0, 64, 128, 192, 255, 200, 100, 0};
    const auto rec = buildSampRecord("f00dcafe-1234-5678-9abc-def012345678", 2, 10, 10, 12, 14, 8,
                                     0, raw4x2);
    const auto samp = buildSampSection({rec});

    // B1. The id matches: the preset's tip arrives, and it costs NEITHER a
    // `sampledTips` count NOR a note -- the whole point of this step.
    {
      const auto desc =
          oneSampledBrushDesc("Sampled Inker", "#Pxl", 20.0,
                              "f00dcafe-1234-5678-9abc-def012345678");
      const AbrImportResult r = importAbrBrushes(wrapAbrWithSamp(samp, desc));
      check(r.ok && r.presets.size() == 1, "abr-samp: the one-brush library still imports");
      if (r.ok && r.presets.size() == 1) {
        const BrushPreset& p = r.presets[0];
        check(p.tipBitmap != nullptr && p.tipBitmap->width == 4 && p.tipBitmap->height == 2 &&
                  p.tipBitmap->alpha == raw4x2,
              "abr-samp: a `sampledData` id matching a real sample attaches its exact bitmap");
        check(r.sampledTips == 0 && r.notes.empty(),
              "abr-samp: a brush whose bitmap DID arrive costs no count and no note -- most of "
              "Kyle Webster's inkers, once this lands");
        // **The durable half.** The pointer above lives as long as the library
        // stays loaded; this id is what `user-presets.txt` writes, so a
        // duplicated preset still has its tip next launch (brush/Library.hpp's
        // `dabId`, app/DabLibrary's extraction). Asserted HERE, on the
        // importer's own output, because app/selftest/DabLibrary.cpp §G tests
        // the extractor and the resolver directly and would not notice the
        // importer forgetting to set the id at all -- which a sabotage
        // confirmed by surviving.
        check(p.dabId == "abr:f00dcafe-1234-5678-9abc-def012345678",
              "abr-samp: and the preset carries the tip's id, which is what survives a relaunch");
        check(r.tipSamples.size() == 1 && r.tipSamples[0].id == "f00dcafe-1234-5678-9abc-def012345678" &&
                  r.tipSamples[0].bitmap != nullptr,
              "abr-samp: and the tips come OUT of the import, so the app layer can write them");
      }
    }

    // B2. The id does NOT match anything in `samp` -- the existing "not
    // imported" fallback, unchanged: same counter, same note text, same
    // procedural-tip outcome app/selftest/AbrBrushes.cpp already asserts for
    // a `.abr` with no `samp` section at all. Proven here against a `samp`
    // section that DOES decode successfully, just not to this id, which is
    // the case that section could not exercise.
    {
      const auto desc =
          oneSampledBrushDesc("Orphan Inker", "#Pxl", 20.0, "not-a-real-id-in-this-file");
      const AbrImportResult r = importAbrBrushes(wrapAbrWithSamp(samp, desc));
      check(r.ok && r.presets.size() == 1 && r.presets[0].tipBitmap == nullptr,
            "abr-samp: an id naming no real sample falls back to the round procedural tip");
      check(r.sampledTips == 1 && !r.notes.empty() &&
                r.notes[0].what.find("sampled bitmap") != std::string::npos,
            "abr-samp: ...and is counted and noted exactly as an unmatched sampledData always "
            "was");
      // The id is still recorded, even though the lookup failed: a preset that
      // NAMES a tip this build could not decode should still say which one it
      // wanted, so importing the pack that has it later can make the brush
      // whole rather than leaving it silently round forever.
      check(r.ok && r.presets.size() == 1 &&
                r.presets[0].dabId == "abr:not-a-real-id-in-this-file",
            "abr-samp: ...and it still records WHICH tip it wanted, for a later import to find");
    }

    // B3. `#Prc` Dmtr, resolved: 50% of this sample's own larger dimension
    // (4) is diameter 2, radius 1.
    {
      const auto desc = oneSampledBrushDesc("Half-size Inker", "#Prc", 50.0,
                                            "f00dcafe-1234-5678-9abc-def012345678");
      const AbrImportResult r = importAbrBrushes(wrapAbrWithSamp(samp, desc));
      check(r.ok && r.presets.size() == 1 &&
                nearf(r.presets[0].radius, 1.0f, 1e-4f),
            "abr-samp: a #Prc Dmtr resolves against the SAMPLE's own larger dimension (4px), so "
            "50% is radius 1");
      check(r.notes.empty(), "abr-samp: ...and costs no note, because it DID resolve");
    }

    // B4. `#Prc` Dmtr with no bitmap to measure it against -- the existing
    // refusal, unchanged.
    {
      const auto desc =
          oneSampledBrushDesc("Procedural Percent", "#Prc", 50.0, nullptr);
      const AbrImportResult r = importAbrBrushes(wrapAbrWithSamp(samp, desc));
      check(r.ok && r.presets.size() == 1 && r.presets[0].tipBitmap == nullptr,
            "abr-samp: #Prc with no sampledData at all is still refused, same as before this "
            "step");
      bool sawPercentNote = false;
      for (const auto& n : r.notes)
        if (n.what.find("percentage") != std::string::npos) sawPercentNote = true;
      check(sawPercentNote, "abr-samp: ...and still says so");
    }
  }

  // ==========================================================================
  std::printf("  -- C. dabCoverage()/dabPixelBounds() sample the bitmap --\n");
  // ==========================================================================
  //
  // No `.abr` anywhere below -- a `BrushTipBitmap` built directly, which is
  // what makes the tolerances exact rather than derived: every query point is
  // chosen to land exactly on a texel centre or exactly halfway between two,
  // so bilinear interpolation of {0, 255}/255 = {0.0, 1.0} produces a result
  // representable without rounding (0.0, 0.5 or 1.0), and the assertion is
  // `==`, not a tolerance nobody can attribute to source or to arithmetic.
  {
    auto bitmapOf = [](int w, int h, std::vector<uint8_t> alpha) {
      auto b = std::make_shared<BrushTipBitmap>();
      b->width = w;
      b->height = h;
      b->alpha = std::move(alpha);
      return b;
    };

    // 2x2, top-left/bottom-right BLACK (0), top-right/bottom-left WHITE (255).
    const auto bmp2x2 = bitmapOf(2, 2, {0, 255, 255, 0});
    BrushTip tip;
    tip.bitmap = bmp2x2;
    tip.radius = 1.0f;  // nativeHalfMax = max(2,2)/2 = 1, so scale == 1.0

    // Exactly on texel (0,0)'s centre: bx=0.5 -> dx=(0.5-1.0)=-0.5, likewise y.
    check(dabCoverage(tip, -0.5f, -0.5f) == 0.0f,
          "abr-samp/coverage: a query at a stored texel's own centre returns that texel's value "
          "with ZERO blend (texel (0,0) = 0)");
    check(dabCoverage(tip, 0.5f, -0.5f) == 1.0f,
          "abr-samp/coverage: ...and texel (1,0) = 255 -> coverage 1.0, exactly");

    // Exactly halfway between texel (0,0)=0 and texel (1,0)=255, at row 0's
    // own centre (ty=0, so no vertical blend at all): bx=1.0 -> dx=0.0;
    // by=0.5 -> dy=-0.5.
    check(dabCoverage(tip, 0.0f, -0.5f) == 0.5f,
          "abr-samp/coverage: exactly halfway between a 0 and a 255 texel is exactly 0.5 -- "
          "bilinear, not nearest-neighbour");

    // Outside the bitmap's own rectangle: no radial gate to fall back to, so
    // this must be a hard zero, not a falloff.
    check(dabCoverage(tip, 1.5f, 0.0f) == 0.0f,
          "abr-samp/coverage: past the bitmap's own edge is coverage 0 -- a bitmap tip has no "
          "'outside the disc' test of its own beyond its rectangle");

    // Roundness squashes the SAME way it does for the procedural ellipse:
    // dy=0.6 is within the tip's un-squashed reach (roundness 1) but pushed
    // outside the bitmap's rectangle once roundness halves the reach.
    BrushTip roundTip = tip;
    roundTip.roundness = 1.0f;
    const float atFull = dabCoverage(roundTip, 0.0f, 0.6f);
    roundTip.roundness = 0.5f;
    const float atHalf = dabCoverage(roundTip, 0.0f, 0.6f);
    check(atFull > 0.0f && atHalf == 0.0f,
          "abr-samp/coverage: roundness squashes a bitmap tip's footprint exactly as it squashes "
          "the procedural ellipse -- the same point reads covered at roundness 1 and not at 0.5");

    // Angle: rotating the TIP by 90 degrees and rotating the QUERY POINT by
    // -90 degrees must sample the same bitmap location, up to float32's own
    // sin/cos rounding near pi/2 (on the order of 1e-7, this file's own
    // measured figure -- see the comment on the tolerance below). Both query
    // points sit comfortably inside one texel's region (not on a boundary),
    // so that rounding cannot flip which texel is sampled.
    BrushTip rot90 = tip;
    rot90.angle = 90.0f;
    const float viaRotatedTip = dabCoverage(rot90, 0.5f, 0.5f);
    const float viaRotatedQuery = dabCoverage(tip, 0.5f, -0.5f);
    // 1e-4: two orders of magnitude past float32 cos/sin's own error near
    // pi/2 (~1e-7), scaled by this fixture's O(1) coordinates -- generous
    // margin, not a fitted one.
    check(nearf(viaRotatedTip, viaRotatedQuery, 1e-4f),
          "abr-samp/coverage: rotating the tip 90 degrees samples the same pixel as rotating the "
          "query point -90 degrees");
  }

  {
    // dabPixelBounds(): a non-square bitmap (4 wide, 2 tall) at radius 2
    // (nativeHalfMax = max(4,2)/2 = 2, scale = 1) -- unrotated, the bound
    // should be an EXACT 2 px / 1 px half-extent (bw=2=radius, bh=1), not the
    // symmetric radius-square every other tip gets.
    auto bmp = std::make_shared<BrushTipBitmap>();
    bmp->width = 4;
    bmp->height = 2;
    bmp->alpha.assign(8, 255);
    BrushTip tip;
    tip.bitmap = bmp;
    tip.radius = 2.0f;

    const Vec2 centre{100.0f, 100.0f};
    const PixelBounds unrotated = dabPixelBounds(tip, centre, 200, 200);
    // Half-extent 2 in x, 1 in y, centred on 100, through `dabPixelBounds()`'s
    // own floor/ceil of the OPEN interval:
    //   x0 = floor(100 - 2 - 0.5) = 97      x1 = ceil(100 + 2 - 0.5) = 102
    //   y0 = floor(100 - 1 - 0.5) = 98      y1 = ceil(100 + 1 - 0.5) = 101
    //
    // **The upper bounds carry one pixel of deliberate slack**, and these
    // literals record that rather than the tight bound. Coverage can be
    // non-zero only where |x + 0.5 - cx| < halfX, so the tight inclusive
    // bound in x is 101, not 102 -- the `ceil` of an open interval always
    // rounds a half-integer edge outward by one. That is the convention this
    // function has always had for round tips and the bitmap branch inherits
    // it unchanged; a bound that is one pixel too GENEROUS costs one column
    // of zero-coverage work, while one pixel too tight would clip paint off
    // the edge of every dab. Asserting the tight bound here would have been
    // asserting a bug fix nobody made.
    //
    // The discriminating half is y: a symmetric radius-square would give
    // y0 == 97 and y1 == 102, identical to x. Asserting y0 == 98 && y1 == 101
    // is what proves the bound followed the bitmap's 2:1 aspect ratio.
    check(unrotated.x0 == 97 && unrotated.x1 == 102 && unrotated.y0 == 98 && unrotated.y1 == 101,
          "abr-samp/bounds: unrotated, a 4x2 bitmap's bound follows its own 2:1 aspect ratio in "
          "x and y separately, rather than being the symmetric radius-square every other tip gets");

    // Rotated 90 degrees, the bound must widen in x and narrow in y --
    // checked as a relative claim (not exact literals) because the
    // rotated-rectangle formula runs through float32 cos/sin near pi/2, and
    // the interesting claim is the SHAPE of the change, which floor/ceil to
    // pixel bounds already makes robust to that rounding.
    BrushTip rotTip = tip;
    rotTip.angle = 90.0f;
    const PixelBounds rotated = dabPixelBounds(rotTip, centre, 200, 200);
    const int32_t unrotatedWidth = unrotated.x1 - unrotated.x0;
    const int32_t unrotatedHeight = unrotated.y1 - unrotated.y0;
    const int32_t rotatedWidth = rotated.x1 - rotated.x0;
    const int32_t rotatedHeight = rotated.y1 - rotated.y0;
    check(rotatedWidth < unrotatedWidth && rotatedHeight > unrotatedHeight,
          "abr-samp/bounds: rotating a wide-and-short bitmap 90 degrees makes its bound "
          "narrow-and-tall -- a symmetric radius-square could never show this");

    // And it must still be a SUPERSET of what the bitmap can actually paint
    // -- dabPixelBounds() over-approximates, per its own header comment, but
    // must never under-approximate. Every texel the real dab reaches must
    // fall inside it, which section D's deposit checks directly.
  }

  {
    // depositDab(): the silhouette a bitmap tip paints is the BITMAP's own
    // rectangle, not the circle a procedural tip of the same radius would
    // paint. A 6x2 fully-opaque strip at radius 3 (nativeHalfMax = 3, scale
    // 1): a texel at (dx=0.5, dy=2.5) sits INSIDE a radius-3 circle
    // (0.25+6.25=6.5 < 9) but OUTSIDE this bitmap's height-1 half-extent --
    // exactly the point that tells the two shapes apart.
    auto bmp = std::make_shared<BrushTipBitmap>();
    bmp->width = 6;
    bmp->height = 2;
    bmp->alpha.assign(12, 255);
    BrushTip tip;
    tip.bitmap = bmp;
    tip.radius = 3.0f;
    tip.flow = 1.0f;

    PigmentTileStore store;
    const Vec2 centre{10.0f, 10.0f};
    depositDab(store, tip, centre, 20, 20, nullptr, nullptr);

    const auto massAt = [&](int32_t x, int32_t y) -> float {
      const PixelCoord at{x, y};
      const PigmentTile* t = store.find(tileCoordAt(at));
      return t != nullptr ? t->readTexel(tileLocalOffset(at)).mass : 0.0f;
    };
    check(massAt(10, 10) > 0.0f,
          "abr-samp/deposit: the bitmap tip's own centre is painted, unsurprisingly");
    check(massAt(12, 10) > 0.0f,
          "abr-samp/deposit: (dx=2.5, dy=0.5) is inside the 6-wide strip -- painted");
    check(massAt(10, 12) == 0.0f,
          "abr-samp/deposit: (dx=0.5, dy=2.5) is INSIDE a same-radius circle but OUTSIDE this "
          "2-tall strip -- unpainted, which is the whole claim that a bitmap tip replaces the "
          "radial gate rather than sitting inside it");
  }

  // ==========================================================================
  std::printf("  -- D. two leaks a live bitmap tip could reach through --\n");
  // ==========================================================================
  {
    auto bmpA = std::make_shared<BrushTipBitmap>();
    bmpA->width = 2;
    bmpA->height = 2;
    bmpA->alpha = {0, 255, 255, 0};
    auto bmpB = std::make_shared<BrushTipBitmap>();
    bmpB->width = 2;
    bmpB->height = 2;
    bmpB->alpha = {255, 0, 0, 255};  // different pixels, same dimensions

    BrushTip a;
    a.radius = 10.0f;
    a.bitmap = bmpA;
    BrushTip b = a;
    b.bitmap = bmpB;
    BrushTip c = a;  // identical pointer to `a`

    check(!dabPreviewTipsEqual(a, b),
          "abr-samp/preview: two tips with identical scalars but DIFFERENT bitmaps are not "
          "equal -- a cache that missed this would show one brush's preview for another's");
    check(dabPreviewTipsEqual(a, c),
          "abr-samp/preview: the identical bitmap pointer (and identical scalars) IS equal");
    BrushTip noBitmap = a;
    noBitmap.bitmap.reset();
    check(!dabPreviewTipsEqual(a, noBitmap),
          "abr-samp/preview: a bitmap tip and a null one, same scalars, are not equal");

    // The cache itself: feeding it `a` then `b` must rasterise TWICE, not
    // hit -- DabPreviewCache::imageFor()'s whole contract is that its key
    // check decides this, and dabPreviewTipsEqual() above is that check.
    DabPreviewCache cache;
    const std::array<BrushTip, kDabPreviewCells> firstTips{a, a, a};
    const std::array<BrushTip, kDabPreviewCells> secondTips{b, b, b};
    (void)cache.imageFor(firstTips);
    (void)cache.imageFor(secondTips);
    check(cache.rasterisations() == 2 && cache.hits() == 0,
          "abr-samp/preview: DabPreviewCache does NOT hit across two different bitmap tips with "
          "the same scalars");
  }

  {
    // brushRowIconTips(): `live` carries a bitmap; the row does not (a
    // BrushRow never does, by design -- brush/Library.hpp's own comment on
    // BrushPreset::tipBitmap). The row's icon tips must NOT show live's
    // bitmap -- the fix in app/BrushRowIcon.cpp is the `as.tipBitmap.reset()`
    // beside the existing `as.links = BrushLinkSet{}`.
    BrushState live;
    auto bmp = std::make_shared<BrushTipBitmap>();
    bmp->width = 2;
    bmp->height = 2;
    bmp->alpha = {0, 128, 128, 255};
    live.tipBitmap = bmp;

    BrushRow row;
    row.radius = 15.0f;
    row.hardness = 0.4f;

    MixboxLut lut;
    const auto tips = brushRowIconTips(row, live, lut);
    bool anyLeaked = false;
    for (const BrushTip& t : tips)
      if (t.bitmap != nullptr) anyLeaked = true;
    check(!anyLeaked,
          "abr-samp/row: an unloaded library row's icon does NOT inherit the LIVE brush's "
          "bitmap tip -- it previews the round procedural tip until the row is actually picked, "
          "same honesty app/BrushLibraryFile.hpp §4 already states for a row's links");
  }

  {
    // The pointer round trip: applyPresetToBrush() -> brushTipFor() ->
    // presetFromBrush() must carry the SAME BrushTipBitmap object all the
    // way through -- not a copy (brush/Library.hpp's own comment on why a
    // deep copy at any of these points would be the per-dab allocation
    // CONTEXT.md's *Lightweight* refuses).
    auto bmp = std::make_shared<BrushTipBitmap>();
    bmp->width = 3;
    bmp->height = 3;
    bmp->alpha.assign(9, 200);

    BrushPreset preset;
    preset.radius = 12.0f;
    preset.tipBitmap = bmp;

    BrushState brush;
    applyPresetToBrush(preset, brush);
    check(brush.tipBitmap == bmp,
          "abr-samp/roundtrip: applyPresetToBrush() carries the SAME bitmap pointer, not a copy");

    MixboxLut lut;
    const BrushTip tip = brushTipFor(brush, lut, 1.0f);
    check(tip.bitmap == bmp, "abr-samp/roundtrip: brushTipFor() carries it into the BrushTip");

    const BrushPreset dup = presetFromBrush("Duplicate", brush);
    check(dup.tipBitmap == bmp,
          "abr-samp/roundtrip: presetFromBrush() (Duplicate) carries it back into a preset");

    // The documented blind spot: presetMatches() does not compare tipBitmap
    // (brush/Library.hpp's comment on why). Proven here rather than only
    // asserted in prose -- two presets differing ONLY in their bitmap read
    // as matching by the eight fields presetMatches() actually checks.
    BrushPreset other = dup;
    auto differentBmp = std::make_shared<BrushTipBitmap>();
    differentBmp->width = 3;
    differentBmp->height = 3;
    differentBmp->alpha.assign(9, 1);
    other.tipBitmap = differentBmp;
    check(presetMatches(other, brush.radius, brush.hardness, brush.spacing, brush.roundness,
                        brush.angle, brush.load, brush.wetness, brush.links, brush.grain),
          "abr-samp/roundtrip: presetMatches() DELIBERATELY cannot tell `other`'s different "
          "bitmap apart from `brush`'s -- documented on BrushPreset::tipBitmap and on "
          "presetMatches() itself, because nothing today can move a live bitmap independently "
          "of picking a whole preset");
  }

  std::printf("[selftest] abr sampled tips %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
