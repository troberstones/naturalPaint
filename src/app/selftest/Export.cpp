#include "app/selftest/Support.hpp"

namespace np {

bool runExportTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  // --- Tolerances, derived rather than guessed ---------------------------
  //
  // The round trip under test is:
  //
  //   tile (premultiplied half) -> un-premultiply -> transfer function ->
  //   quantize to N bits -> PNG -> decodeImageLinear() (sample/max, then
  //   srgbDecode) -> compare against the tile's own stored linear value
  //
  // Comparing against the *tile's* stored value (read back through
  // Tile::readPixel, i.e. after half rounding) rather than against the float
  // literal that was written in is deliberate: half-precision storage is
  // io/ImageIO's boundary, already covered by runImageIOTest(), and leaving
  // it in would swamp the term this test is actually about. Every fixture
  // pixel used for a precision claim is fully opaque (alpha = 1.0, exact in
  // half), so the un-premultiply step is a division by exactly 1.0 and
  // contributes nothing either. What is left is exactly one lossy stage:
  //
  //   quantization of the *encoded* value to N bits, re-expanded back
  //   through the sRGB decode curve.
  //
  // Worst case is half a quantization step, amplified by the decode curve's
  // steepest slope. d(linear)/d(encoded) for sRGB peaks at encoded = 1:
  // 2.4/1.055 * ((1+0.055)/1.055)^1.4 = 2.2749. So:
  //
  //   16-bit: 0.5/65535 * 2.2749 = 1.74e-5
  //    8-bit: 0.5/255   * 2.2749 = 4.46e-3
  //
  // Both are measured for real below and printed at run time, so the numbers
  // in this comment are checkable rather than asserted: this fixture measures
  // 1.371e-5 and 3.542e-3 respectively, each comfortably under -- and of the
  // same order as -- its bound. The measurement lands below the bound because
  // 16 sampled pixels cannot be expected to hit the worst-case rounding phase
  // at the worst-case slope, which is exactly why the tolerance is set from
  // the bound rather than from the measurement: a tolerance fitted to the
  // measured number alone would be fragile against any other fixture. The
  // landed tolerances sit ~1.4x above the *derived bound* (2.5e-5 / 1.74e-5 =
  // 1.44; 6.5e-3 / 4.46e-3 = 1.46), the same headroom ratio
  // runLutBakeTest()'s kResidualTol = 2e-3 used over its own 1.46e-3.
  constexpr float kRoundTripTol16 = 2.5e-5f;
  constexpr float kRoundTripTol8 = 6.5e-3f;
  // Tolerance for values checked in the *encoded* domain (recovered as
  // srgbEncode(decoded), i.e. the literal 0..1 sample the file carries).
  // One 16-bit quantization step is 1/65535 = 1.53e-5; 1e-4 leaves ~6x
  // headroom for the srgbDecode/srgbEncode float round trip on top.
  constexpr float kEncodedDomainTol = 1e-4f;

  // Writes a *straight* (non-premultiplied) linear RGBA value into a
  // document's layer, premultiplying on the way in exactly the way
  // io/ImageIO.cpp's writeDecodedImageIntoLayer() does (rgb *= a, alpha
  // unchanged) -- so these fixtures hold what a real opened/painted document
  // holds, not a hand-tuned storage layout the export path never sees.
  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  // The raw premultiplied texel as the tile actually stores it -- the
  // reference every premultiply claim below is checked against, the same
  // discipline runProbeTest() used ("not just a plausible-looking number").
  auto storedPixel = [](const Document& doc, size_t layerIndex, int32_t x,
                        int32_t y) -> std::array<float, 4> {
    const PixelCoord p{x, y};
    const Tile* t = doc.layers[layerIndex].rgbTiles->find(tileCoordAt(p));
    return t ? t->readPixel(tileLocalOffset(p)) : std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f};
  };
  auto pixelOf = [](const DecodedImage& img, uint32_t x, uint32_t y) -> std::array<float, 4> {
    const float* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    return {p[0], p[1], p[2], p[3]};
  };
  // decodeImageLinear() always applies srgbDecode to RGB, so re-encoding a
  // decoded channel recovers the literal 0..1 sample the exported file
  // carries. That is what makes "which transfer function did the exporter
  // actually apply" directly observable, rather than inferred.
  auto sampleOf = [](float decodedLinear) { return srgbEncode(decodedLinear); };

  // --- fixture: a 4x4, fully opaque linear ramp --------------------------
  // 16 distinct values per channel spanning [0,1] including both endpoints,
  // and each channel offset from the others so a bug that swapped or copied
  // channels could not pass.
  Document ramp = Document::createBlank(4, 4, WorkingSpace{});
  for (int i = 0; i < 16; ++i) {
    const float r = static_cast<float>(i) / 15.0f;
    const float g = static_cast<float>((i * 7) % 16) / 15.0f;
    const float b = static_cast<float>((i * 11) % 16) / 15.0f;
    writeStraight(ramp, 0, i % 4, i / 4, r, g, b, 1.0f);
  }

  // --- 16-bit PNG round trip (PLAN.md Phase 4 step 1's core claim) -------
  {
    const ExportResult enc = exportDocument(ramp, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt16);
    check(enc.ok && enc.error.empty(),
          "exportDocument: 16-bit sRGB PNG of a 4x4 ramp encodes without error");
    if (!enc.ok) {
      std::printf("    (error was: %s)\n", enc.error.c_str());
    } else {
      check(enc.bytes.size() > 8 && enc.bytes[0] == 137 && enc.bytes[1] == 'P' &&
                enc.bytes[2] == 'N' && enc.bytes[3] == 'G',
            "exportDocument: the bytes really are a PNG (signature)");
      // IHDR's bit-depth field sits at a fixed offset in every PNG: 8-byte
      // signature + 4-byte length + 4-byte "IHDR" + 4-byte width + 4-byte
      // height = byte 24. Checked directly, so "16-bit" is a property of the
      // file rather than of this module's own bookkeeping.
      check(enc.bytes.size() > 25 && enc.bytes[24] == 16,
            "exportDocument: the file's own IHDR declares bit depth 16, not 8");

      const DecodedImage back = decodeImageLinear(enc.bytes.data(), enc.bytes.size());
      check(back.valid() && back.width == 4 && back.height == 4,
            "16-bit PNG export decodes back through decodeImageLinear() at the right size");
      if (back.valid()) {
        float maxResidual = 0.0f;
        for (int i = 0; i < 16; ++i) {
          const std::array<float, 4> stored = storedPixel(ramp, 0, i % 4, i / 4);
          const std::array<float, 4> got =
              pixelOf(back, static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
          for (int c = 0; c < 4; ++c)
            maxResidual = std::max(maxResidual, std::fabs(got[c] - stored[c]));
        }
        std::printf("    [measured] 16-bit round-trip max residual = %.3e (tol %.3e)\n",
                    static_cast<double>(maxResidual), static_cast<double>(kRoundTripTol16));
        check(maxResidual <= kRoundTripTol16,
              "16-bit PNG round trip: every channel of every pixel returns within the "
              "derived 16-bit quantization tolerance");
      }
    }
  }

  // --- 8-bit, same document: same code path, measurably coarser ----------
  float maxResidual8 = 0.0f;
  {
    const ExportResult enc = exportDocument(ramp, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt8);
    check(enc.ok, "exportDocument: 8-bit sRGB PNG of the same ramp encodes without error");
    if (enc.ok) {
      check(enc.bytes.size() > 25 && enc.bytes[24] == 8,
            "exportDocument: the 8-bit file's own IHDR declares bit depth 8");
      const DecodedImage back = decodeImageLinear(enc.bytes.data(), enc.bytes.size());
      if (back.valid()) {
        for (int i = 0; i < 16; ++i) {
          const std::array<float, 4> stored = storedPixel(ramp, 0, i % 4, i / 4);
          const std::array<float, 4> got =
              pixelOf(back, static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
          for (int c = 0; c < 4; ++c)
            maxResidual8 = std::max(maxResidual8, std::fabs(got[c] - stored[c]));
        }
        std::printf("    [measured]  8-bit round-trip max residual = %.3e (tol %.3e)\n",
                    static_cast<double>(maxResidual8), static_cast<double>(kRoundTripTol8));
        check(maxResidual8 <= kRoundTripTol8,
              "8-bit PNG round trip: within the derived 8-bit quantization tolerance");
      }
    }
  }

  // --- PRD B6, proven rather than assumed: a value that survives 16 bits
  // and is genuinely lost at 8 --------------------------------------------
  //
  // Two pixels whose sRGB-encoded values are 0.5010 and 0.5030. Both land
  // inside 8-bit code 128's bucket ([127.5/255, 128.5/255) = [0.50000,
  // 0.50392)) with margin on both sides, so an 8-bit export cannot tell them
  // apart at all; at 16 bits they are codes 32833 and 32964, 131 apart.
  {
    Document pair = Document::createBlank(2, 1, WorkingSpace{});
    const float lin0 = srgbDecode(0.5010f);
    const float lin1 = srgbDecode(0.5030f);
    writeStraight(pair, 0, 0, 0, lin0, lin0, lin0, 1.0f);
    writeStraight(pair, 0, 1, 0, lin1, lin1, lin1, 1.0f);
    check(!near(lin0, lin1, 1e-6f),
          "B6 fixture: the two source pixels genuinely differ in linear value");

    const ExportResult e8 = exportDocument(pair, ImageFormat::Png,
                                           ExportTargetSpace::Rec709Srgb,
                                           ExportBitDepth::UInt8);
    const ExportResult e16 = exportDocument(pair, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt16);
    check(e8.ok && e16.ok, "B6: both the 8-bit and 16-bit exports of the pair succeed");
    if (e8.ok && e16.ok) {
      const DecodedImage b8 = decodeImageLinear(e8.bytes.data(), e8.bytes.size());
      const DecodedImage b16 = decodeImageLinear(e16.bytes.data(), e16.bytes.size());
      if (b8.valid() && b16.valid()) {
        const auto p8a = pixelOf(b8, 0, 0), p8b = pixelOf(b8, 1, 0);
        const auto p16a = pixelOf(b16, 0, 0), p16b = pixelOf(b16, 1, 0);
        check(p8a[0] == p8b[0],
              "B6: at 8 bits the two distinct values collapse to the identical sample "
              "-- the precision really is gone, not merely nudged");
        check(p16a[0] != p16b[0],
              "B6: at 16 bits the same two values stay distinct");
        check(near(p16a[0], srgbDecode(0.5010f), kRoundTripTol16 * 2.0f) &&
                  near(p16b[0], srgbDecode(0.5030f), kRoundTripTol16 * 2.0f),
              "B6: and each 16-bit value comes back at its own correct level, not just "
              "'different from the other one'");
        check(std::fabs(p16a[0] - p16b[0]) > 1e-3f,
              "B6: the surviving 16-bit difference is far larger than the 16-bit "
              "quantization step, so it is signal and not noise");
      }
    }
  }

  // --- PRD B6's loud-failure half: an unsatisfiable depth request --------
  {
    const ExportResult jpeg16 = exportDocument(ramp, ImageFormat::Jpeg,
                                               ExportTargetSpace::Rec709Srgb,
                                               ExportBitDepth::UInt16);
    check(!jpeg16.ok && jpeg16.bytes.empty(),
          "B6: 16-bit into JPEG fails and writes no bytes, rather than degrading to 8");
    check(contains(jpeg16.error, "JPEG") && contains(jpeg16.error, "16-bit") &&
              contains(jpeg16.error, "8 bits per channel"),
          "B6: and the error names the format, the refused depth and the real limit "
          "-- not a bare 'export failed'");
    check(contains(jpeg16.error, "PNG"),
          "B6: the error also names the format that *can* carry the request");

    const ExportResult tga16 = exportDocument(ramp, ImageFormat::Tga,
                                              ExportTargetSpace::Rec709Srgb,
                                              ExportBitDepth::UInt16);
    check(!tga16.ok && contains(tga16.error, "TGA"), "B6: 16-bit into TGA fails by name");
    const ExportResult bmp16 = exportDocument(ramp, ImageFormat::Bmp,
                                              ExportTargetSpace::Rec709Srgb,
                                              ExportBitDepth::UInt16);
    check(!bmp16.ok && contains(bmp16.error, "BMP"), "B6: 16-bit into BMP fails by name");
    // Control: the refusals above are about the format's own limit, not a
    // blanket "16-bit never works".
    const ExportResult png16 = exportDocument(ramp, ImageFormat::Png,
                                              ExportTargetSpace::Rec709Srgb,
                                              ExportBitDepth::UInt16);
    check(png16.ok, "B6 control: the identical 16-bit request into PNG still succeeds");
  }

  // --- PRD I5, proven: the target space is really applied and really
  // selectable -------------------------------------------------------------
  {
    // One known linear value, 0.5 -- exactly representable in half, so the
    // expected encoded sample is exact arithmetic with nothing to round.
    Document mid = Document::createBlank(1, 1, WorkingSpace{});
    writeStraight(mid, 0, 0, 0, 0.5f, 0.5f, 0.5f, 1.0f);

    const ExportResult lin = exportDocument(mid, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Linear,
                                            ExportBitDepth::UInt16);
    const ExportResult srgb = exportDocument(mid, ImageFormat::Png,
                                             ExportTargetSpace::Rec709Srgb,
                                             ExportBitDepth::UInt16);
    const ExportResult r709 = exportDocument(mid, ImageFormat::Png,
                                             ExportTargetSpace::Rec709Bt709,
                                             ExportBitDepth::UInt16);
    check(lin.ok && srgb.ok && r709.ok,
          "I5: the same Document exports to all three target spaces");
    check(lin.bytes != srgb.bytes && srgb.bytes != r709.bytes && lin.bytes != r709.bytes,
          "I5: the three exports are pairwise different files -- the transfer function is "
          "genuinely selectable, not a parameter that gets ignored");

    const DecodedImage bl = decodeImageLinear(lin.bytes.data(), lin.bytes.size());
    const DecodedImage bs = decodeImageLinear(srgb.bytes.data(), srgb.bytes.size());
    const DecodedImage br = decodeImageLinear(r709.bytes.data(), r709.bytes.size());
    if (bl.valid() && bs.valid() && br.valid()) {
      const float linSample = sampleOf(pixelOf(bl, 0, 0)[0]);
      const float srgbSample = sampleOf(pixelOf(bs, 0, 0)[0]);
      const float r709Sample = sampleOf(pixelOf(br, 0, 0)[0]);
      check(near(linSample, 0.5f, kEncodedDomainTol),
            "I5: Rec709Linear writes the linear value 0.5 to the file verbatim -- no "
            "transfer function applied");
      check(!near(linSample, srgbEncode(0.5f), 0.01f),
            "I5: and that linear sample is emphatically NOT srgbEncode(0.5) ~ 0.735 -- the "
            "no-encode option is verifiably not silently sRGB-encoding");
      check(near(srgbSample, srgbEncode(0.5f), kEncodedDomainTol),
            "I5: Rec709Srgb writes srgbEncode(0.5) -- color/Space's own curve, not an "
            "approximation of it");
      check(near(r709Sample, rec709Encode(0.5f), kEncodedDomainTol),
            "I5: Rec709Bt709 writes rec709Encode(0.5)");
      check(!near(srgbSample, r709Sample, 1e-3f),
            "I5: sRGB and BT.709 land on genuinely different samples -- the two curves are "
            "not being conflated because their primaries match");
    }
  }

  // --- Premultiply: undone correctly, checked against the tile's own raw
  // premultiplied storage --------------------------------------------------
  {
    Document alpha = Document::createBlank(3, 1, WorkingSpace{});
    // (0,0): translucent. (1,0): deliberately never written -- fully
    // transparent, exercising the a <= 0 guard. (2,0): opaque control.
    writeStraight(alpha, 0, 0, 0, 0.8f, 0.4f, 0.2f, 0.5f);
    writeStraight(alpha, 0, 2, 0, 0.25f, 0.5f, 0.75f, 1.0f);

    const std::array<float, 4> raw = storedPixel(alpha, 0, 0, 0);
    check(raw[3] > 0.0f && raw[0] < 0.5f,
          "premultiply fixture: the tile really stores rgb*a (red 0.8*0.5 ~ 0.4), so "
          "un-premultiplying is something the export path has to actually do");

    // Rec709Linear so the check is pure premultiply arithmetic with no
    // transfer function standing between the tile and the file sample.
    const ExportResult enc = exportDocument(alpha, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Linear,
                                            ExportBitDepth::UInt16);
    check(enc.ok, "premultiply: a document with alpha < 1 exports to 16-bit linear PNG");
    if (enc.ok) {
      const DecodedImage back = decodeImageLinear(enc.bytes.data(), enc.bytes.size());
      if (back.valid()) {
        const auto got = pixelOf(back, 0, 0);
        // The expectation is derived from the tile's own stored values, not
        // from the 0.8/0.4/0.2 literals -- so this checks the export path,
        // not the fixture.
        const float expR = raw[0] / raw[3], expG = raw[1] / raw[3], expB = raw[2] / raw[3];
        check(near(sampleOf(got[0]), expR, kEncodedDomainTol) &&
                  near(sampleOf(got[1]), expG, kEncodedDomainTol) &&
                  near(sampleOf(got[2]), expB, kEncodedDomainTol),
              "premultiply: the exported RGB is exactly the tile's stored rgb divided by "
              "its stored alpha");
        check(near(got[3], raw[3], kRoundTripTol16),
              "premultiply: alpha itself is written straight through, un-curved and "
              "un-divided");
        check(!near(sampleOf(got[0]), raw[0], 0.05f),
              "premultiply: the exported red genuinely differs from the raw premultiplied "
              "value -- proves un-premultiply ran, not just alpha passthrough");

        const auto empty = pixelOf(back, 1, 0);
        check(near(empty[0], 0.0f, 1e-6f) && near(empty[1], 0.0f, 1e-6f) &&
                  near(empty[2], 0.0f, 1e-6f) && near(empty[3], 0.0f, 1e-6f),
              "premultiply: a never-painted texel exports as transparent black (the "
              "a <= 0 guard core/Probe.cpp uses), not as a divide-by-zero");

        const std::array<float, 4> rawOpaque = storedPixel(alpha, 0, 2, 0);
        const auto opaque = pixelOf(back, 2, 0);
        check(near(sampleOf(opaque[0]), rawOpaque[0], kEncodedDomainTol) &&
                  near(opaque[3], 1.0f, kRoundTripTol16),
              "premultiply: an alpha=1 texel is unchanged by the division (control)");
      }
    }
  }

  // --- The primaries scope decision, enforced rather than documented-only -
  {
    Document wide = Document::createBlank(1, 1, WorkingSpace{});
    writeStraight(wide, 0, 0, 0, 0.5f, 0.5f, 0.5f, 1.0f);
    // ACEScg's red primary (x = 0.713), i.e. a genuinely different gamut --
    // not a rounding-level perturbation.
    wide.workingSpace.primaries.redX = 0.713f;
    wide.workingSpace.primaries.redY = 0.293f;

    const ExportResult enc = exportDocument(wide, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt16);
    check(!enc.ok && enc.bytes.empty(),
          "primaries: a working space whose primaries differ from the target's is refused, "
          "not silently exported as if it matched");
    check(contains(enc.error, "primaries") && contains(enc.error, "0.7130"),
          "primaries: the error names the mismatch and quotes the offending coordinate");
    check(contains(enc.error, "transfer functions only"),
          "primaries: and says why -- this build converts transfer functions, not gamuts");

    // The check is a real comparison, not a blanket rejection: the same
    // document with matching primaries exports fine.
    Document matched = Document::createBlank(1, 1, WorkingSpace{});
    writeStraight(matched, 0, 0, 0, 0.5f, 0.5f, 0.5f, 1.0f);
    check(exportDocument(matched, ImageFormat::Png, ExportTargetSpace::Rec709Srgb,
                         ExportBitDepth::UInt16)
              .ok,
          "primaries control: the default Rec.709 working space exports without complaint");
  }

  // --- JPEG has no alpha channel: refused by name, not silently dropped ---
  {
    Document translucent = Document::createBlank(2, 1, WorkingSpace{});
    writeStraight(translucent, 0, 0, 0, 0.5f, 0.5f, 0.5f, 1.0f);
    writeStraight(translucent, 0, 1, 0, 0.5f, 0.5f, 0.5f, 0.25f);

    const ExportResult enc = exportDocument(translucent, ImageFormat::Jpeg,
                                            ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt8);
    check(!enc.ok, "alpha: a translucent document is refused for JPEG, which has no alpha");
    check(contains(enc.error, "no alpha channel") && contains(enc.error, "x=1, y=0"),
          "alpha: the error names the format's limitation and the first offending pixel");

    // The same document exports fine to the three formats that do carry
    // alpha -- so the refusal is about JPEG, not about alpha in general.
    check(exportDocument(translucent, ImageFormat::Png, ExportTargetSpace::Rec709Srgb,
                         ExportBitDepth::UInt8)
                  .ok &&
              exportDocument(translucent, ImageFormat::Tga, ExportTargetSpace::Rec709Srgb,
                             ExportBitDepth::UInt8)
                  .ok &&
              exportDocument(translucent, ImageFormat::Bmp, ExportTargetSpace::Rec709Srgb,
                             ExportBitDepth::UInt8)
                  .ok,
          "alpha control: PNG/TGA/BMP accept the same translucent document");
  }

  // --- PRD I1's write half: all four formats actually produce a file that
  // decodes back ------------------------------------------------------------
  {
    // Uniform, fully opaque mid-grey over 8x8 -- uniform specifically so
    // JPEG's block transform has nothing to ring on and its own lossiness
    // stays a quantization question rather than a spatial one.
    Document flat = Document::createBlank(8, 8, WorkingSpace{});
    for (int32_t y = 0; y < 8; ++y)
      for (int32_t x = 0; x < 8; ++x) writeStraight(flat, 0, x, y, 0.5f, 0.25f, 0.75f, 1.0f);
    const std::array<float, 4> stored = storedPixel(flat, 0, 4, 4);

    struct Case { ImageFormat format; const char* name; float tol; };
    // PNG/TGA/BMP are lossless containers at 8 bits, so they get the derived
    // 8-bit quantization tolerance. JPEG is lossy by construction and gets a
    // deliberately looser one -- calling that out rather than quietly using
    // one tolerance for all four.
    const Case cases[] = {
        {ImageFormat::Png, "PNG", kRoundTripTol8},
        {ImageFormat::Tga, "TGA", kRoundTripTol8},
        {ImageFormat::Bmp, "BMP", kRoundTripTol8},
        {ImageFormat::Jpeg, "JPEG", 0.02f},
    };
    for (const Case& c : cases) {
      const ExportResult enc =
          exportDocument(flat, c.format, ExportTargetSpace::Rec709Srgb, ExportBitDepth::UInt8);
      char label[96];
      std::snprintf(label, sizeof(label), "I1: 8-bit %s export round-trips through the decoder",
                    c.name);
      bool caseOk = enc.ok;
      if (enc.ok) {
        const DecodedImage back = decodeImageLinear(enc.bytes.data(), enc.bytes.size());
        caseOk = back.valid() && back.width == 8 && back.height == 8;
        if (caseOk) {
          const auto got = pixelOf(back, 4, 4);
          caseOk = near(got[0], stored[0], c.tol) && near(got[1], stored[1], c.tol) &&
                   near(got[2], stored[2], c.tol);
        }
      }
      check(caseOk, label);
    }
  }

  // --- flattenDocumentToLinear on its own ---------------------------------
  {
    // Content outside the canvas rectangle is clipped away: export writes
    // the document's canvas, not its content's bounding box.
    Document offCanvas = Document::createBlank(4, 4, WorkingSpace{});
    writeStraight(offCanvas, 0, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f);
    writeStraight(offCanvas, 0, 200, 200, 1.0f, 0.0f, 0.0f, 1.0f);  // a whole tile away
    const DecodedImage flat = flattenDocumentToLinear(offCanvas);
    check(flat.valid() && flat.width == 4 && flat.height == 4,
          "flattenDocumentToLinear: the result is exactly the canvas size");
    if (flat.valid()) {
      check(near(pixelOf(flat, 0, 0)[3], 1.0f, 1e-6f) &&
                near(pixelOf(flat, 3, 3)[3], 0.0f, 1e-6f),
            "flattenDocumentToLinear: in-canvas content survives, out-of-canvas content is "
            "clipped rather than wrapped or resized into view");
    }

    // Two layers, disjoint content -- the plain-sum path core/Probe.cpp's
    // sampleAllLayers already documents. Both layers' pixels must appear.
    Document twoLayers = Document::createBlank(2, 1, WorkingSpace{});
    writeStraight(twoLayers, 0, 0, 0, 1.0f, 0.0f, 0.0f, 1.0f);
    Layer second;
    second.kind = LayerKind::RGB;
    second.rgbTiles.emplace();
    twoLayers.layers.push_back(std::move(second));
    writeStraight(twoLayers, 1, 1, 0, 0.0f, 0.0f, 1.0f, 1.0f);
    const DecodedImage both = flattenDocumentToLinear(twoLayers);
    check(both.valid() && near(pixelOf(both, 0, 0)[0], 1.0f, 1e-3f) &&
              near(pixelOf(both, 1, 0)[2], 1.0f, 1e-3f),
          "flattenDocumentToLinear: every RGB layer contributes (the plain sum core/Probe "
          "already uses -- no compositing model exists to do better yet)");

    // A blank document is a legitimate thing to export: fully transparent,
    // not an error.
    const Document blank = Document::createBlank(2, 2, WorkingSpace{});
    const ExportResult blankEnc = exportDocument(blank, ImageFormat::Png,
                                                 ExportTargetSpace::Rec709Srgb,
                                                 ExportBitDepth::UInt16);
    check(blankEnc.ok, "export: a createBlank()'d document with zero painted tiles exports "
                       "successfully rather than erroring");
    if (blankEnc.ok) {
      const DecodedImage back = decodeImageLinear(blankEnc.bytes.data(), blankEnc.bytes.size());
      check(back.valid() && near(pixelOf(back, 0, 0)[3], 0.0f, 1e-6f),
            "export: and it comes back fully transparent");
    }

    // A zero-sized document has nothing to write, and says so.
    Document empty;
    const ExportResult emptyEnc = exportDocument(empty, ImageFormat::Png,
                                                 ExportTargetSpace::Rec709Srgb,
                                                 ExportBitDepth::UInt8);
    check(!emptyEnc.ok && contains(emptyEnc.error, "no pixels to export"),
          "export: a zero-sized document fails with a specific error, not a crash or an "
          "empty file");
  }

  // --- exportDocumentToFile: same bytes, and failures never touch disk ----
  {
    const char* path = "selftest_export.png";
    std::remove(path);
    std::string err;
    const bool wrote = exportDocumentToFile(ramp, path, ImageFormat::Png,
                                            ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt16, &err);
    check(wrote && err.empty(), "exportDocumentToFile: writes a 16-bit PNG to disk");
    if (wrote) {
      std::FILE* f = std::fopen(path, "rb");
      std::vector<uint8_t> fileBytes;
      if (f) {
        uint8_t buf[4096];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) fileBytes.insert(fileBytes.end(), buf, buf + n);
        std::fclose(f);
      }
      const ExportResult inMemory = exportDocument(ramp, ImageFormat::Png,
                                                   ExportTargetSpace::Rec709Srgb,
                                                   ExportBitDepth::UInt16);
      check(inMemory.ok && fileBytes == inMemory.bytes,
            "exportDocumentToFile: the file on disk is byte-identical to exportDocument()'s "
            "in-memory result -- one encode path, not two");
    }
    std::remove(path);

    const char* refusedPath = "selftest_export_refused.jpg";
    std::remove(refusedPath);
    std::string refusedErr;
    const bool refused = exportDocumentToFile(ramp, refusedPath, ImageFormat::Jpeg,
                                              ExportTargetSpace::Rec709Srgb,
                                              ExportBitDepth::UInt16, &refusedErr);
    check(!refused && contains(refusedErr, "JPEG"),
          "exportDocumentToFile: a refused request forwards the encode's own specific error");
    std::FILE* shouldNotExist = std::fopen(refusedPath, "rb");
    check(shouldNotExist == nullptr,
          "exportDocumentToFile: and leaves no file behind -- nothing is opened until the "
          "encode has fully succeeded");
    if (shouldNotExist) {
      std::fclose(shouldNotExist);
      std::remove(refusedPath);
    }
  }

  std::printf("[selftest] export %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
