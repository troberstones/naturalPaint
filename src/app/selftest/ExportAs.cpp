#include "app/selftest/Support.hpp"

namespace np {

bool runExportAsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto nearf = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  check(oiioBackendCompiledIn(),
        "the capability query reports the OIIO backend is compiled in");

  // --- Tolerances, derived rather than guessed ---------------------------
  //
  // (1) **The resampler.** ops/Resample accumulates in double (relative
  //     error ~1e-16 over the at most 64 terms any fixture below sums) and
  //     rounds exactly twice on the way out: once storing the horizontal
  //     pass's result as float, once storing the final un-premultiplied
  //     value. Each rounding is at most half a float ulp, i.e. 6e-8 relative,
  //     so for the [0,1] values used here the bound is 2 * 6e-8 = 1.2e-7.
  //     Landed 1.0e-6, 8.3x the bound -- deliberately looser than the ~1.4x
  //     the round-trip tolerances elsewhere in the suite use, because these
  //     comparisons are against hand-computed decimal references (0.3666667
  //     for (0.2 + 0.35)/1.5) whose own decimal-to-float conversion is worth
  //     more headroom than the arithmetic is.
  constexpr float kResampleTol = 1.0e-6f;
  // (2) **The linear-light proof's 8-bit round trip.** A 2x2 checker of
  //     linear 0 and 1 halved must land on linear 0.5, written through the
  //     sRGB curve at 8 bits: srgbEncode(0.5) = 0.73535, times 255 = 187.51,
  //     quantized to code 188. That rounding moves the encoded value by
  //     (188 - 187.51)/255 = 1.92e-3, and the sRGB *decode* slope at that
  //     point -- 2.4/1.055 * ((0.737255 + 0.055)/1.055)^1.4 = 1.5194 --
  //     turns it into a linear error of 2.92e-3. Landed 4.0e-3, 1.37x the
  //     bound, the same headroom ratio runExportTest()'s own tolerances use.
  //     Measured below and printed, so the derivation is checkable.
  constexpr float kLinearLightTol = 4.0e-3f;

  auto writeStraight = [](Document& doc, int32_t x, int32_t y, float r, float g, float b,
                          float a) {
    TileStore& tiles = *doc.layers[0].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
  auto makeImage = [](uint32_t w, uint32_t h) {
    DecodedImage img;
    img.width = w;
    img.height = h;
    img.pixels.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0.0f);
    return img;
  };
  auto setPixel = [](DecodedImage& img, uint32_t x, uint32_t y, float r, float g, float b,
                     float a) {
    float* p = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
  };
  auto sampleAt = [](const std::vector<float>& v, uint32_t w, uint32_t x, uint32_t y, int c) {
    return v[(static_cast<size_t>(y) * w + x) * 4 + static_cast<size_t>(c)];
  };

  // --- What the dialog is allowed to offer (PRD I3, I5) ------------------
  //
  // The dialog builds its combo boxes from exactly these two functions, so
  // "the dialog can never offer a combination io/Export will refuse" is
  // checkable here rather than by clicking.
  {
    const std::vector<ImageFormat> formats = offerableExportFormats();
    auto offered = [&](ImageFormat f) {
      return std::find(formats.begin(), formats.end(), f) != formats.end();
    };
    check(offered(ImageFormat::Png) && offered(ImageFormat::Jpeg) &&
              offered(ImageFormat::Tga) && offered(ImageFormat::Bmp),
          "PRD I1's four formats are offerable in BOTH build configurations");
    check(offered(ImageFormat::Exr) && offered(ImageFormat::Tiff) &&
              offered(ImageFormat::Hdr) && offered(ImageFormat::Dpx),
          "EXR/TIFF/HDR/DPX are offerable now that the OIIO backend is compiled in");
    check(!offered(ImageFormat::Psd) && !offered(ImageFormat::CameraRaw),
          "the read-only formats are never offered as export targets, in either build");

    const std::vector<ExportBitDepth> png = offerableExportDepths(ImageFormat::Png);
    check(png.size() == 2 && png[0] == ExportBitDepth::UInt8 && png[1] == ExportBitDepth::UInt16,
          "PNG offers exactly 8- and 16-bit integer -- no float depth it cannot write");
    const std::vector<ExportBitDepth> jpeg = offerableExportDepths(ImageFormat::Jpeg);
    check(jpeg.size() == 1 && jpeg[0] == ExportBitDepth::UInt8,
          "JPEG offers 8-bit only, so 16-bit-into-JPEG is unreachable from the dialog");
    const std::vector<ExportBitDepth> exr = offerableExportDepths(ImageFormat::Exr);
    check(exr.size() == 2u && exr[0] == ExportBitDepth::Half &&
              exr[1] == ExportBitDepth::Float32,
          "EXR offers half and 32-bit float and NOT 8-bit -- the depth probe's answer, not a "
          "guess about what EXR 'should' do");
    check(offerableExportDepths(ImageFormat::Psd).empty(),
          "a format this build cannot write offers no depths at all");
  }

  // --- resolveExportSize(): every mode, hand-computed ---------------------
  {
    uint32_t w = 0, h = 0;
    std::string err;

    ExportResize none;
    check(resolveExportSize(none, 1024, 768, &w, &h, &err) && w == 1024 && h == 768,
          "resize None resolves to the document's own size");

    ExportResize pct;
    pct.mode = ExportResizeMode::Percent;
    pct.percent = 50.0f;
    check(resolveExportSize(pct, 1024, 768, &w, &h, &err) && w == 512 && h == 384,
          "resize 50% of 1024x768 is 512x384");
    pct.percent = 33.0f;
    // 1024 * 0.33 = 337.92 -> 338; 768 * 0.33 = 253.44 -> 253. Each axis
    // rounds independently, which is what a percentage means.
    check(resolveExportSize(pct, 1024, 768, &w, &h, &err) && w == 338 && h == 253,
          "resize 33% rounds each axis half-away-from-zero: hand-computed 338x253");
    pct.percent = 100.0f;
    check(resolveExportSize(pct, 1024, 768, &w, &h, &err) && w == 1024 && h == 768,
          "resize 100% is exactly the source size, not source-minus-rounding");
    pct.percent = 0.01f;
    check(resolveExportSize(pct, 1024, 768, &w, &h, &err) && w == 1 && h == 1,
          "a percentage that would round an axis to zero clamps to 1 -- 1024x0 is not an image");

    pct.percent = 150.0f;
    check(!resolveExportSize(pct, 1024, 768, &w, &h, &err) && contains(err, "enlarge") &&
              contains(err, "Fit within") && w == 0 && h == 0,
          "resize above 100% is refused by name, and points at the mode that does clamp");
    pct.percent = 0.0f;
    check(!resolveExportSize(pct, 1024, 768, &w, &h, &err) && contains(err, "not a size"),
          "resize 0% is refused rather than producing a zero-sized file");
    pct.percent = -25.0f;
    check(!resolveExportSize(pct, 1024, 768, &w, &h, &err) && contains(err, "not a size"),
          "a negative percentage is refused with the same named reason");

    ExportResize fit;
    fit.mode = ExportResizeMode::FitWithin;
    fit.maxWidth = 2048;
    fit.maxHeight = 2048;
    // 4000x1000 into a 2048 box: the width binds (2048/4000 = 0.512), so
    // 4000*0.512 = 2048 and 1000*0.512 = 512.
    check(resolveExportSize(fit, 4000, 1000, &w, &h, &err) && w == 2048 && h == 512,
          "fit-within picks the binding axis and preserves aspect: hand-computed 2048x512");
    check(resolveExportSize(fit, 100, 50, &w, &h, &err) && w == 100 && h == 50,
          "fit-within NEVER enlarges: a document smaller than the box exports at 1:1");
    fit.maxWidth = 0;
    check(!resolveExportSize(fit, 100, 50, &w, &h, &err) && contains(err, "at least 1 pixel"),
          "a fit-within box with a zero side is refused by name");
    check(!resolveExportSize(none, 0, 100, &w, &h, &err) && contains(err, "0x100"),
          "a zero-sized source is refused, and the message quotes the size");
  }

  // --- resampleAreaAverage(): against hand-computed references ------------
  {
    std::string err;
    std::vector<float> out;

    // (a) 4x2 -> 2x1. Every destination texel is the mean of a 2x2 block,
    // hand-computed: (0.0+0.2+0.8+1.0)/4 = 0.5 and (0.4+0.6+0.1+0.3)/4 = 0.35.
    const float rows[2][4] = {{0.0f, 0.2f, 0.4f, 0.6f}, {0.8f, 1.0f, 0.1f, 0.3f}};
    DecodedImage block = makeImage(4, 2);
    for (uint32_t y = 0; y < 2; ++y)
      for (uint32_t x = 0; x < 4; ++x)
        setPixel(block, x, y, rows[y][x], rows[y][x], rows[y][x], 1.0f);
    check(resampleAreaAverage(block.pixels.data(), 4, 2, 2, 1, &out, &err) && out.size() == 8,
          "resample 4x2 -> 2x1 succeeds and produces exactly 2x1x4 samples");
    check(nearf(sampleAt(out, 2, 0, 0, 0), 0.5f, kResampleTol) &&
              nearf(sampleAt(out, 2, 1, 0, 0), 0.35f, kResampleTol),
          "resample 4x2 -> 2x1 matches the hand-computed 2x2 block means (0.5, 0.35)");
    check(sampleAt(out, 2, 0, 0, 3) == 1.0f && sampleAt(out, 2, 1, 0, 3) == 1.0f,
          "a fully opaque source stays EXACTLY opaque -- weights sum to 1 in double, so a "
          "downscale cannot make a document un-exportable to JPEG by rounding");

    // (b) 3x1 -> 2x1: a non-integer scale factor, where the fractional edge
    // weights are the whole point. Footprints are [0,1.5) and [1.5,3), so
    // dst0 = (s0 + 0.5*s1)/1.5 and dst1 = (0.5*s1 + s2)/1.5.
    const float s[3] = {0.2f, 0.7f, 0.1f};
    DecodedImage odd = makeImage(3, 1);
    for (uint32_t x = 0; x < 3; ++x) setPixel(odd, x, 0, s[x], s[x], s[x], 1.0f);
    const float expect0 = (0.2f + 0.5f * 0.7f) / 1.5f;  // 0.3666667
    const float expect1 = (0.5f * 0.7f + 0.1f) / 1.5f;  // 0.3
    check(resampleAreaAverage(odd.pixels.data(), 3, 1, 2, 1, &out, &err) &&
              nearf(sampleAt(out, 2, 0, 0, 0), expect0, kResampleTol) &&
              nearf(sampleAt(out, 2, 1, 0, 0), expect1, kResampleTol),
          "a non-integer 3->2 reduction matches the hand-computed fractional-weight "
          "reference (0.366667, 0.300000)");

    // (c) A constant image survives a lopsided reduction bit-exactly. This
    // is the strongest statement available about the weight normalisation:
    // no ulp of drift anywhere, at zero tolerance.
    DecodedImage flat = makeImage(500, 30);
    for (uint32_t y = 0; y < 30; ++y)
      for (uint32_t x = 0; x < 500; ++x) setPixel(flat, x, y, 0.3f, 0.6f, 0.9f, 1.0f);
    bool bitExact = true;
    if (resampleAreaAverage(flat.pixels.data(), 500, 30, 61, 7, &out, &err)) {
      for (size_t i = 0; i < out.size(); i += 4) {
        if (out[i] != 0.3f || out[i + 1] != 0.6f || out[i + 2] != 0.9f || out[i + 3] != 1.0f)
          bitExact = false;
      }
    } else {
      bitExact = false;
    }
    check(bitExact,
          "a constant 500x30 image reduced to 61x7 is bit-identical to its own constant at "
          "ZERO tolerance -- every one of 1708 samples");

    // (d) 1:1 is a verbatim copy, not a premultiply/divide round trip.
    check(resampleAreaAverage(block.pixels.data(), 4, 2, 4, 2, &out, &err) &&
              out == block.pixels,
          "a 1:1 'resize' returns the source bit-for-bit, short-circuiting the alpha round "
          "trip that could otherwise cost an ulp");

    // (e) Every refusal, by its error string.
    check(!resampleAreaAverage(block.pixels.data(), 4, 2, 8, 2, &out, &err) &&
              contains(err, "enlarge") && contains(err, "the width") && out.empty(),
          "an upscale is refused by name, says which axis grew, and leaves no partial buffer");
    check(!resampleAreaAverage(block.pixels.data(), 4, 2, 2, 4, &out, &err) &&
              contains(err, "the height"),
          "an upscale in the other axis names the height instead");
    check(!resampleAreaAverage(block.pixels.data(), 4, 2, 8, 4, &out, &err) &&
              contains(err, "both axes"),
          "an upscale in both axes names both");
    check(!resampleAreaAverage(nullptr, 4, 2, 2, 1, &out, &err) && contains(err, "source pixels"),
          "a null source is refused rather than dereferenced");
    check(!resampleAreaAverage(block.pixels.data(), 4, 2, 0, 1, &out, &err) &&
              contains(err, "at least 1 pixel"),
          "a zero destination dimension is refused by name");
  }

  // --- The phase 6 warning, measured rather than asserted -----------------
  //
  // PLAN.md phase 6: "**downscale must prefilter** (area average or descend
  // the mip pyramid); no reconstruction filter fixes aliasing after the
  // fact." Both patterns below have a known exact mean, so "how much of the
  // answer is alias" is a number, not an opinion. The naive path -- point
  // sampling the source at the destination grid -- is implemented here in
  // the test rather than shipped in ops/, because a wrong resampler has no
  // business being in the binary.
  {
    constexpr uint32_t kSrc = 512, kDst = 64;  // an exact factor of 8
    auto rmsAgainst = [](const std::vector<float>& v, uint32_t w, uint32_t h, float expected) {
      double acc = 0.0;
      for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
          const double d = static_cast<double>(v[(static_cast<size_t>(y) * w + x) * 4]) -
                           static_cast<double>(expected);
          acc += d * d;
        }
      return static_cast<float>(std::sqrt(acc / (static_cast<double>(w) * h)));
    };
    auto pointSample = [](const DecodedImage& img, uint32_t dstW, uint32_t dstH) {
      std::vector<float> out(static_cast<size_t>(dstW) * dstH * 4u, 0.0f);
      for (uint32_t y = 0; y < dstH; ++y)
        for (uint32_t x = 0; x < dstW; ++x) {
          const uint32_t sx = x * (img.width / dstW);
          const uint32_t sy = y * (img.height / dstH);
          const float* p = &img.pixels[(static_cast<size_t>(sy) * img.width + sx) * 4];
          float* d = &out[(static_cast<size_t>(y) * dstW + x) * 4];
          for (int c = 0; c < 4; ++c) d[c] = p[c];
        }
      return out;
    };

    // (a) A 1-pixel checkerboard: exactly at the source Nyquist limit, mean
    // 0.5 everywhere, and the classic case a naive resize destroys utterly.
    DecodedImage checker = makeImage(kSrc, kSrc);
    for (uint32_t y = 0; y < kSrc; ++y)
      for (uint32_t x = 0; x < kSrc; ++x) {
        const float v = ((x + y) & 1u) ? 1.0f : 0.0f;
        setPixel(checker, x, y, v, v, v, 1.0f);
      }
    std::vector<float> filtered;
    std::string err;
    check(resampleAreaAverage(checker.pixels.data(), kSrc, kSrc, kDst, kDst, &filtered, &err),
          "aliasing fixture: a 512x512 1-px checkerboard reduces to 64x64");
    const std::vector<float> naive = pointSample(checker, kDst, kDst);
    const float filteredRms = rmsAgainst(filtered, kDst, kDst, 0.5f);
    const float naiveRms = rmsAgainst(naive, kDst, kDst, 0.5f);
    std::printf("    [measured] 1-px checker 512->64: prefiltered RMS error vs the true mean "
                "0.5 = %.3e, naive point-sample = %.3e -- the naive result is a uniformly "
                "%s 64x64 image with no trace of the pattern left\n",
                static_cast<double>(filteredRms), static_cast<double>(naiveRms),
                naive[0] < 0.5f ? "black" : "white");
    check(filteredRms <= 1e-6f,
          "prefiltered: every destination texel of the checker lands on the true mean 0.5, "
          "within a float ulp");
    check(naiveRms >= 0.49f,
          "naive: the same 50%-grey pattern collapses to a FLAT image at full amplitude -- "
          "the aliased answer is not noisy, it is confidently wrong");

    // (b) A period-3 stripe pattern, whose period does not divide the scale
    // factor. This is where the box kernel's own limit shows: its sinc
    // frequency response leaves a residual ripple, and the honest thing is
    // to measure it rather than claim the filter is perfect.
    DecodedImage stripes = makeImage(kSrc, kSrc);
    for (uint32_t y = 0; y < kSrc; ++y)
      for (uint32_t x = 0; x < kSrc; ++x) {
        const float v = (x % 3u == 0u) ? 1.0f : 0.0f;
        setPixel(stripes, x, y, v, v, v, 1.0f);
      }
    std::vector<float> stripesFiltered;
    check(resampleAreaAverage(stripes.pixels.data(), kSrc, kSrc, kDst, kDst, &stripesFiltered,
                              &err),
          "aliasing fixture: a period-3 stripe pattern reduces to 64x64");
    const std::vector<float> stripesNaive = pointSample(stripes, kDst, kDst);
    const float mean3 = 1.0f / 3.0f;
    const float stripesFilteredRms = rmsAgainst(stripesFiltered, kDst, kDst, mean3);
    const float stripesNaiveRms = rmsAgainst(stripesNaive, kDst, kDst, mean3);
    std::printf("    [measured] period-3 stripes 512->64: prefiltered RMS = %.4f, naive = "
                "%.4f (%.1fx worse)\n",
                static_cast<double>(stripesFilteredRms), static_cast<double>(stripesNaiveRms),
                stripesFilteredRms > 0.0f
                    ? static_cast<double>(stripesNaiveRms / stripesFilteredRms)
                    : 0.0);
    check(stripesFilteredRms < stripesNaiveRms / 5.0f,
          "prefiltered beats naive by more than 5x on a pattern whose period does not divide "
          "the scale factor -- the case a box kernel is NOT perfect on");
    check(stripesFilteredRms > 0.0f && stripesFilteredRms < 0.10f,
          "and the box kernel's own residual ripple is real but bounded -- reported, not "
          "claimed away");
  }

  // --- Linear light, proven by the number the file carries ----------------
  {
    DecodedImage checker = makeImage(2, 2);
    setPixel(checker, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f);
    setPixel(checker, 1, 0, 0.0f, 0.0f, 0.0f, 1.0f);
    setPixel(checker, 0, 1, 0.0f, 0.0f, 0.0f, 1.0f);
    setPixel(checker, 1, 1, 1.0f, 1.0f, 1.0f, 1.0f);

    std::vector<float> half;
    std::string err;
    check(resampleAreaAverage(checker.pixels.data(), 2, 2, 1, 1, &half, &err) &&
              nearf(half[0], 0.5f, kResampleTol),
          "a 2x2 black/white checker halves to linear 0.5 -- the average of the LIGHT, not of "
          "the codes");

    DecodedImage one = makeImage(1, 1);
    one.pixels = half;
    const ExportResult enc = encodeLinearImage(one, WorkingSpace{}, ImageFormat::Png,
                                               ExportTargetSpace::Rec709Srgb,
                                               ExportBitDepth::UInt8);
    check(enc.ok, "the halved checker encodes to an 8-bit sRGB PNG");
    if (enc.ok) {
      const DecodedImage back = decodeImageLinear(enc.bytes.data(), enc.bytes.size());
      check(back.valid() && back.width == 1 && back.height == 1,
            "and decodes back as a 1x1 image");
      if (back.valid()) {
        const float gotLinear = back.pixels[0];
        const float gotCode = srgbEncode(gotLinear) * 255.0f;
        // The wrong pipeline: average the *encoded* values instead of the
        // linear ones. srgbEncode(0) = 0 and srgbEncode(1) = 1, so it lands
        // on encoded 0.5 -- 8-bit code 128 -- which decodes to linear 0.214.
        const float wrongEncoded = 0.5f * (srgbEncode(0.0f) + srgbEncode(1.0f));
        const float wrongLinear = srgbDecode(wrongEncoded);
        std::printf("    [measured] halved checker: correct path -> 8-bit code %.1f (linear "
                    "%.4f); averaging the encoded values instead -> encoded %.4f, code %.1f "
                    "before rounding (linear %.4f), %.3f too dark\n",
                    static_cast<double>(gotCode), static_cast<double>(gotLinear),
                    static_cast<double>(wrongEncoded),
                    static_cast<double>(wrongEncoded * 255.0f),
                    static_cast<double>(wrongLinear),
                    static_cast<double>(0.5f - wrongLinear));
        check(nearf(gotLinear, 0.5f, kLinearLightTol),
              "the exported file decodes to linear 0.5 within the derived 8-bit tolerance");
        check(nearf(gotCode, 188.0f, 0.51f),
              "the file's own 8-bit code is 188 = round(255 * srgbEncode(0.5)), the hand-"
              "computed answer");
        check(!nearf(wrongLinear, 0.5f, 0.2f) && wrongLinear < 0.25f,
              "and the encoded-domain average -- the bug this ordering prevents -- would have "
              "landed at linear 0.214, more than half a stop dark");
      }
    }
  }

  // --- Alpha: filtered premultiplied, not straight ------------------------
  {
    // A fully transparent texel whose straight RGB is arbitrary green sits
    // next to an opaque red one. Under premultiplied filtering the green
    // contributes nothing at all; under a straight average it contaminates
    // half the result.
    DecodedImage pair = makeImage(2, 1);
    setPixel(pair, 0, 0, 1.0f, 0.0f, 0.0f, 1.0f);
    setPixel(pair, 1, 0, 0.0f, 1.0f, 0.0f, 0.0f);

    std::vector<float> out;
    std::string err;
    check(resampleAreaAverage(pair.pixels.data(), 2, 1, 1, 1, &out, &err), "alpha fixture resamples");
    if (out.size() == 4) {
      const float straightAverageGreen = 0.5f * (0.0f + 1.0f);
      std::printf("    [measured] transparent-green + opaque-red -> green channel %.4f "
                  "(premultiplied filtering); a straight-alpha average gives %.4f\n",
                  static_cast<double>(out[1]), static_cast<double>(straightAverageGreen));
      check(out[1] == 0.0f,
            "the fully transparent texel's colour contributes EXACTLY nothing -- zero, not "
            "nearly zero");
      check(nearf(out[0], 1.0f, kResampleTol),
            "and the surviving colour is the opaque texel's own, un-diluted by the "
            "transparent one");
      check(nearf(out[3], 0.5f, kResampleTol),
            "while alpha itself averages normally, to 0.5");
      check(straightAverageGreen > 0.4f,
            "control: the straight-alpha average this avoids really would have been 0.5 green");
    }
  }

  // --- One set of refusal strings, not two --------------------------------
  //
  // The claim io/ExportAs.hpp makes about the dialog: every message it shows
  // is io/Export's own. Asserted by string equality against both the shared
  // helper and a real encode, rather than by reading the code.
  {
    ExportRequest req;
    req.format = ImageFormat::Jpeg;
    req.bitDepth = ExportBitDepth::UInt16;
    const ExportValidation v = validateExportRequest(req, 8, 8, nullptr, nullptr);
    const std::string direct = exportRefusalReason(ImageFormat::Jpeg,
                                                   ExportTargetSpace::Rec709Srgb,
                                                   ExportBitDepth::UInt16, nullptr, nullptr);
    check(!v.ok && !direct.empty() && v.error == direct,
          "validateExportRequest quotes exportRefusalReason() byte for byte");

    Document doc = Document::createBlank(2, 2, WorkingSpace{});
    for (int32_t y = 0; y < 2; ++y)
      for (int32_t x = 0; x < 2; ++x) writeStraight(doc, x, y, 0.5f, 0.25f, 0.75f, 1.0f);
    const ExportResult enc = exportDocument(doc, ImageFormat::Jpeg,
                                            ExportTargetSpace::Rec709Srgb,
                                            ExportBitDepth::UInt16);
    check(!enc.ok && enc.error == direct,
          "and a real encodeLinearImage() failure is that identical string -- there is one "
          "set of refusal strings in this binary, not a UI copy that can drift");
    check(contains(direct, "JPEG") && contains(direct, "16-bit integer"),
          "the shared string still names the format and the refused depth (PRD B6)");
  }

  // --- Every validation refusal, checked by its message -------------------
  {
    ExportRequest req;
    req.format = ImageFormat::Psd;
    std::string why = exportRequestAvailability(req);
    check(!why.empty() && contains(why, "PSD"),
          "a read-only format (PSD) is refused by name in both builds");
    req.format = ImageFormat::CameraRaw;
    why = exportRequestAvailability(req);
    check(!why.empty() && contains(why, "camera raw"),
          "camera raw is refused by name in both builds");

    req.format = ImageFormat::Exr;
    req.bitDepth = ExportBitDepth::Half;
    why = exportRequestAvailability(req);
    check(why.empty(), "an EXR half request is available");

    req.bitDepth = ExportBitDepth::UInt8;
    why = exportRequestAvailability(req);
    check(!why.empty() && contains(why, "EXR") && contains(why, "8-bit integer"),
          "an 8-bit EXR is still refused -- the depth probe's answer, which is the case "
          "OpenImageIO would have silently substituted half for");

    // The resize refusal reaches validation intact.
    ExportRequest big;
    big.resize.mode = ExportResizeMode::Percent;
    big.resize.percent = 200.0f;
    const ExportValidation up = validateExportRequest(big, 100, 100, nullptr, nullptr);
    check(!up.ok && contains(up.error, "enlarge"),
          "an upscaling request fails validation with ops/Resample's own wording");

    // Primaries, which only a real working space can trip.
    WorkingSpace odd;
    odd.primaries.redX = 0.7347f;  // ACES AP0-ish red, far outside the 1e-6 epsilon
    ExportRequest plain;
    const ExportValidation prim = validateExportRequest(plain, 4, 4, &odd, nullptr);
    check(!prim.ok && contains(prim.error, "primaries"),
          "a primaries mismatch is refused when a working space is supplied");
    check(validateExportRequest(plain, 4, 4, nullptr, nullptr).ok,
          "...and skipped, not silently passed, when no working space is supplied -- the same "
          "request with no document is a legal preset");

    // Translucency, which only real pixels can trip.
    DecodedImage soft = makeImage(2, 1);
    setPixel(soft, 0, 0, 1.0f, 1.0f, 1.0f, 1.0f);
    setPixel(soft, 1, 0, 1.0f, 1.0f, 1.0f, 0.5f);
    ExportRequest jpg;
    jpg.format = ImageFormat::Jpeg;
    const ExportValidation trans = validateExportRequest(jpg, 2, 1, nullptr, &soft);
    check(!trans.ok && contains(trans.error, "no alpha channel") && contains(trans.error, "x=1"),
          "a translucent document into JPEG is refused, naming the first offending pixel");
    check(validateExportRequest(jpg, 2, 1, nullptr, nullptr).ok,
          "...and the same request with no pixels to look at is a legal preset");
  }

  // --- PRD I11's warnings: legal, lossy, and named with a number ----------
  {
    ExportRequest req;
    req.resize.mode = ExportResizeMode::Percent;
    req.resize.percent = 50.0f;
    ExportValidation v = validateExportRequest(req, 1000, 1000, nullptr, nullptr);
    bool sawResize = false, sawDepth = false;
    for (const std::string& w : v.warnings) {
      if (contains(w, "1000x1000 -> 500x500") && contains(w, "75.0%")) sawResize = true;
      if (contains(w, "256 levels")) sawDepth = true;
    }
    check(v.ok && v.outWidth == 500 && v.outHeight == 500,
          "a 50% downscale of 1000x1000 validates, at 500x500");
    check(sawResize,
          "I11: the downscale warning names the exact sizes and that it discards 75.0% of the "
          "pixels -- a number, not 'some quality loss'");
    check(sawDepth, "I11: the 8-bit warning names 256 levels against the half-float working space");

    req.resize.mode = ExportResizeMode::None;
    req.targetSpace = ExportTargetSpace::Rec709Linear;
    v = validateExportRequest(req, 8, 8, nullptr, nullptr);
    bool sawBanding = false;
    for (const std::string& w : v.warnings)
      if (contains(w, "coarser near black") && contains(w, "12.9x")) sawBanding = true;
    check(sawBanding,
          "I11: 8-bit *linear* is warned about with its measured 12.9x shadow-step penalty, "
          "derived from color/Space's own curve at run time");

    DecodedImage hot = makeImage(2, 1);
    setPixel(hot, 0, 0, 0.5f, 0.5f, 0.5f, 1.0f);
    setPixel(hot, 1, 0, 2.5f, 1.0f, 1.0f, 1.0f);
    ExportRequest srgb8;
    v = validateExportRequest(srgb8, 2, 1, nullptr, &hot);
    bool sawClip = false;
    for (const std::string& w : v.warnings)
      if (contains(w, "2.5000") && contains(w, "clips to white") && contains(w, "x=1"))
        sawClip = true;
    check(v.ok && sawClip,
          "I11: an integer depth warns that the document's 2.5 highlight clips, and says where");

    ExportRequest jpg;
    jpg.format = ImageFormat::Jpeg;
    v = validateExportRequest(jpg, 8, 8, nullptr, nullptr);
    bool sawLossy = false, sawNoAlpha = false;
    for (const std::string& w : v.warnings) {
      if (contains(w, "JPEG is lossy")) sawLossy = true;
      if (contains(w, "no alpha channel")) sawNoAlpha = true;
    }
    check(v.ok && sawLossy && sawNoAlpha,
          "I11: JPEG warns that it is lossy at quality 95 and that it carries no alpha");

    ExportRequest clean;
    clean.bitDepth = ExportBitDepth::UInt16;
    v = validateExportRequest(clean, 8, 8, nullptr, nullptr);
    check(v.ok && v.warnings.empty(),
          "control: a 16-bit sRGB PNG at document size warns about nothing at all -- warnings "
          "are earned, not decorative");
  }

  // --- Presets: save, serialize, load, apply ------------------------------
  {
    ExportPresetStore store;
    std::string err;

    ExportPreset web;
    web.name = "  Web preview  ";  // deliberately padded; storage trims
    web.request.format = ImageFormat::Png;
    web.request.targetSpace = ExportTargetSpace::Rec709Srgb;
    web.request.bitDepth = ExportBitDepth::UInt8;
    web.request.resize.mode = ExportResizeMode::FitWithin;
    web.request.resize.maxWidth = 2048;
    web.request.resize.maxHeight = 1024;
    web.request.resize.percent = 42.5f;  // the *other* mode's number, non-default

    ExportPreset comp;
    comp.name = "Comp EXR";
    comp.request.format = ImageFormat::Exr;
    comp.request.targetSpace = ExportTargetSpace::Rec709Linear;
    comp.request.bitDepth = ExportBitDepth::Half;
    comp.request.resize.mode = ExportResizeMode::Percent;
    comp.request.resize.percent = 50.0f;

    ExportPreset print;
    print.name = "Print TIFF";
    print.request.format = ImageFormat::Tiff;
    print.request.targetSpace = ExportTargetSpace::Rec709Bt709;
    print.request.bitDepth = ExportBitDepth::UInt16;

    check(store.savePreset(web, &err) && store.savePreset(comp, &err) &&
              store.savePreset(print, &err),
          "three presets save, including two this build may have no writer for");
    check(store.presets().size() == 3 && store.presets()[0].name == "Web preview",
          "the stored name is trimmed of surrounding whitespace");

    const std::string text = store.serialize();
    ExportPresetStore reloaded;
    check(reloaded.loadFromString(text, "round trip") && reloaded.error().empty(),
          "the serialized preset file parses back");
    check(reloaded.problems().empty(),
          "and no preset is skipped in EITHER build -- an unwritable format is still a "
          "perfectly readable preset");
    bool allFieldsEqual = reloaded.presets().size() == store.presets().size();
    for (size_t i = 0; allFieldsEqual && i < store.presets().size(); ++i) {
      const ExportPreset& a = store.presets()[i];
      const ExportPreset& b = reloaded.presets()[i];
      allFieldsEqual = a.name == b.name && a.request.format == b.request.format &&
                       a.request.targetSpace == b.request.targetSpace &&
                       a.request.bitDepth == b.request.bitDepth &&
                       a.request.resize.mode == b.request.resize.mode &&
                       a.request.resize.percent == b.request.resize.percent &&
                       a.request.resize.maxWidth == b.request.resize.maxWidth &&
                       a.request.resize.maxHeight == b.request.resize.maxHeight;
    }
    check(allFieldsEqual,
          "every field of every preset survives save -> serialize -> load, including the "
          "numbers the active resize mode does not read");

    // The cross-build case, which is the reason any of this is interesting.
    const ExportPreset* loadedComp = reloaded.find("comp exr");
    check(loadedComp != nullptr, "preset lookup is case-insensitive");
    if (loadedComp) {
      check(loadedComp->request.format == ImageFormat::Exr &&
                loadedComp->request.bitDepth == ExportBitDepth::Half,
            "the EXR half preset still names EXR half after the round trip -- NEVER silently "
            "replaced by a format this build happens to be able to write");
      const std::string why = exportRequestAvailability(loadedComp->request);
      check(why.empty(), "and it is reported available");
      const ExportValidation v =
          validateExportRequest(loadedComp->request, 512, 512, nullptr, nullptr);
      check(v.ok, "applying it validates");
      check(!v.ok || (v.outWidth == 256 && v.outHeight == 256),
            "and where it does apply, its 50% resize resolves to 256x256");
    }

    // Replace by name, case-insensitively; delete; and the name rules.
    ExportPreset again;
    again.name = "WEB PREVIEW";
    again.request.bitDepth = ExportBitDepth::UInt16;
    check(store.savePreset(again, &err) && store.presets().size() == 3,
          "saving under an existing name (different case) replaces rather than duplicating");
    check(store.find("Web preview") != nullptr &&
              store.find("Web preview")->request.bitDepth == ExportBitDepth::UInt16 &&
              store.find("Web preview")->name == "WEB PREVIEW",
          "...the replacement's settings and capitalisation both win");
    check(store.removePreset("print tiff") && store.presets().size() == 2 &&
              !store.removePreset("print tiff"),
          "delete removes by name case-insensitively, and reports a second attempt as a miss");

    ExportPreset bad;
    bad.name = "   ";
    check(!store.savePreset(bad, &err) && contains(err, "empty or whitespace"),
          "a whitespace-only preset name is refused by name");
    bad.name = std::string(ExportPresetStore::kMaxPresetNameLength + 1, 'x');
    check(!store.savePreset(bad, &err) && contains(err, "the limit is 64"),
          "an over-long preset name is refused, naming the limit");
    bad.name = std::string("bell\x07here");
    check(!store.savePreset(bad, &err) && contains(err, "control character"),
          "a preset name with a control character is refused, naming the byte");
    check(store.presets().size() == 2, "and none of the three refusals modified the store");
  }

  // --- Preset file: the awkward inputs ------------------------------------
  {
    ExportPresetStore store;
    check(!store.loadFromString("this is not json", "fixture") && !store.error().empty(),
          "a structurally broken preset file fails to load, with a message that says where");
    check(!store.loadFromString("{ \"version\": 1 }", "fixture") &&
              contains(store.error(), "presets"),
          "a JSON file with no presets array is refused as 'not an export preset file'");
    check(store.loadFromString("{ \"version\": 1, \"presets\": [] }", "fixture") &&
              store.presets().empty(),
          "an empty preset list is a valid file, not an error");

    // A token from some future build: that one preset is skipped and
    // described; the rest load. See io/ExportAs.hpp on the cost.
    const char* mixed =
        "{ \"version\": 1, \"presets\": ["
        " {\"name\":\"future\",\"format\":\"jpegxl\",\"space\":\"rec709-srgb\","
        "  \"depth\":\"uint8\",\"resize\":\"none\",\"percent\":100,\"maxWidth\":1,"
        "  \"maxHeight\":1},"
        " {\"name\":\"present\",\"format\":\"png\",\"space\":\"rec709-srgb\","
        "  \"depth\":\"uint16\",\"resize\":\"none\",\"percent\":100,\"maxWidth\":1,"
        "  \"maxHeight\":1} ] }";
    check(store.loadFromString(mixed, "fixture") && store.presets().size() == 1 &&
              store.presets()[0].name == "present",
          "a preset naming an unrecognised format is skipped while the rest still load");
    check(store.problems().size() == 1 && contains(store.problems()[0], "jpegxl") &&
              contains(store.problems()[0], "future"),
          "...and the skip is reported, naming the preset and the token");

    // An unknown *field* is forward-compatible and must not break anything.
    const char* extraField =
        "{ \"version\": 2, \"presets\": ["
        " {\"name\":\"ok\",\"format\":\"png\",\"space\":\"rec709-srgb\",\"depth\":\"uint8\","
        "  \"resize\":\"none\",\"percent\":100,\"maxWidth\":1,\"maxHeight\":1,"
        "  \"futureField\":{\"nested\":[1,2,true,null]}} ], \"futureTop\": \"ignored\" }";
    check(store.loadFromString(extraField, "fixture") && store.presets().size() == 1 &&
              store.problems().empty(),
          "unrecognised fields (and a future version number) are skipped, not fatal");

    const char* dupes =
        "{ \"presets\": ["
        " {\"name\":\"A\",\"format\":\"png\",\"space\":\"rec709-srgb\",\"depth\":\"uint8\","
        "  \"resize\":\"none\",\"percent\":100,\"maxWidth\":1,\"maxHeight\":1},"
        " {\"name\":\"a\",\"format\":\"png\",\"space\":\"rec709-srgb\",\"depth\":\"uint8\","
        "  \"resize\":\"none\",\"percent\":100,\"maxWidth\":1,\"maxHeight\":1} ] }";
    check(!store.loadFromString(dupes, "fixture") &&
              contains(store.error(), "two presets are both named") && store.presets().empty(),
          "two presets whose names differ only in case are a load failure, not a menu with two "
          "identical rows");
  }

  // --- Preset file: a real file, and where it lives -----------------------
  {
    const char* path = "selftest_exportas_presets.json";
    std::remove(path);

    ExportPresetStore empty;
    check(empty.loadFromFile("selftest_exportas_does_not_exist.json") &&
              empty.presets().empty() && empty.error().empty(),
          "a preset file that does not exist is an empty store, not a failure -- every first "
          "run would otherwise look broken");

    ExportPresetStore store;
    ExportPreset p;
    p.name = "Round trip";
    p.request.format = ImageFormat::Bmp;
    p.request.targetSpace = ExportTargetSpace::Rec709Bt709;
    p.request.resize.mode = ExportResizeMode::FitWithin;
    p.request.resize.maxWidth = 777;
    p.request.resize.maxHeight = 555;
    std::string err;
    check(store.savePreset(p, &err) && store.saveToFile(path, &err),
          "a preset store writes to a real file");
    ExportPresetStore fromDisk;
    check(fromDisk.loadFromFile(path) && fromDisk.presets().size() == 1 &&
              fromDisk.presets()[0].request.resize.maxWidth == 777 &&
              fromDisk.presets()[0].request.resize.maxHeight == 555 &&
              fromDisk.presets()[0].request.format == ImageFormat::Bmp,
          "...and reads back identically from disk");
    std::remove(path);

    // The location override, which is what lets this run without touching
    // the developer's own settings.
    const char* previous = std::getenv("NP_EXPORT_PRESETS");
    const std::string saved = previous ? previous : "";
    setenv("NP_EXPORT_PRESETS", "/tmp/np-selftest-presets.json", 1);
    check(defaultExportPresetsPath() == "/tmp/np-selftest-presets.json",
          "$NP_EXPORT_PRESETS overrides the preset file location");
    unsetenv("NP_EXPORT_PRESETS");
    const std::string fallback = defaultExportPresetsPath();
    check(contains(fallback, "naturalPaint") && contains(fallback, "export-presets.json"),
          "and the default location is a per-user application-data path, never the source tree");
    if (previous) setenv("NP_EXPORT_PRESETS", saved.c_str(), 1);
  }

  // --- The whole operation, end to end ------------------------------------
  {
    // A 64x64 document, half black and half white in vertical stripes of 1
    // px, exported at 25% -- so the export path really does flatten, resize
    // and encode, and the answer is the known mean.
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    for (int32_t y = 0; y < 64; ++y)
      for (int32_t x = 0; x < 64; ++x) {
        const float v = (x & 1) ? 1.0f : 0.0f;
        writeStraight(doc, x, y, v, v, v, 1.0f);
      }

    ExportRequest req;
    req.format = ImageFormat::Png;
    req.targetSpace = ExportTargetSpace::Rec709Srgb;
    req.bitDepth = ExportBitDepth::UInt16;
    req.resize.mode = ExportResizeMode::Percent;
    req.resize.percent = 25.0f;

    const ExportResult out = exportDocumentWithRequest(doc, req);
    check(out.ok && out.error.empty(), "exportDocumentWithRequest: 64x64 at 25% encodes");
    if (out.ok) {
      const DecodedImage back = decodeImageLinear(out.bytes.data(), out.bytes.size());
      check(back.valid() && back.width == 16 && back.height == 16,
            "...and the file really is 16x16 -- the resize reached the encoder");
      if (back.valid()) {
        float maxDev = 0.0f;
        for (size_t i = 0; i < back.pixels.size(); i += 4)
          maxDev = std::max(maxDev, std::fabs(back.pixels[i] - 0.5f));
        std::printf("    [measured] 1-px stripes 64->16 through the full export path: max "
                    "deviation from the true linear mean 0.5 = %.3e\n",
                    static_cast<double>(maxDev));
        check(maxDev <= 1.0e-4f,
              "every texel of the exported file is the true linear mean 0.5, within the 16-bit "
              "quantization step");
        check(back.pixels[3] == 1.0f,
              "and the fully opaque document is still fully opaque after the resize");
      }
    }

    // A request the document itself makes illegal still refuses through the
    // composed path, with the same string.
    ExportRequest tooBig = req;
    tooBig.resize.percent = 400.0f;
    const ExportResult refused = exportDocumentWithRequest(doc, tooBig);
    check(!refused.ok && refused.bytes.empty() && contains(refused.error, "enlarge"),
          "an upscaling request refuses through the composed path and writes no bytes");

    const char* filePath = "selftest_exportas_out.png";
    std::string fileErr;
    check(exportDocumentWithRequestToFile(doc, filePath, req, &fileErr),
          "exportDocumentWithRequestToFile writes the resized file");
    std::FILE* f = std::fopen(filePath, "rb");
    check(f != nullptr, "...and the file is really there");
    if (f) std::fclose(f);
    std::remove(filePath);

    check(!exportDocumentWithRequestToFile(doc, "selftest_exportas_refused.png", tooBig,
                                           &fileErr) &&
              contains(fileErr, "enlarge"),
          "a refused request forwards the same error and never opens the file");
    std::FILE* shouldNotExist = std::fopen("selftest_exportas_refused.png", "rb");
    check(shouldNotExist == nullptr, "...leaving nothing behind on disk");
    if (shouldNotExist) {
      std::fclose(shouldNotExist);
      std::remove("selftest_exportas_refused.png");
    }
  }

  std::printf("[selftest] export as %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
