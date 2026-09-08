#include "app/selftest/Support.hpp"

#include "io/OiioBackend.hpp"

namespace np {

bool runFormatSupportTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  check(oiioBackendCompiledIn(),
        "the capability module reports the OIIO backend is compiled in");
  std::printf("    %s\n", imageBackendSummary().c_str());

  // --- Tolerances, derived rather than guessed ---------------------------
  //
  // Three genuinely different storage models are round-tripped below, and
  // they get three separately derived numbers rather than one number that
  // covers the worst of them.
  //
  // (1) **Half-float, encoded domain.** IEEE binary16 has 10 stored mantissa
  //     bits, so for a value v in [2^e, 2^(e+1)) one ulp is 2^(e-10) and the
  //     worst rounding error is half of that: at most v * 2^-11. For the
  //     [0,1] samples written here that bounds the absolute error at
  //     2^-11 = 4.883e-4. Landed 7.0e-4, ~1.43x the bound -- the same
  //     headroom ratio runLutBakeTest()'s kResidualTol = 2e-3 used over its
  //     own measured 1.46e-3, and step 1's kRoundTripTol16 over its 1.74e-5.
  constexpr float kHalfEncodedTol = 7.0e-4f;
  // (2) **16-bit integer, linear domain.** Identical in every term to
  //     runExportTest()'s own kRoundTripTol16 and re-derived here rather
  //     than shared, because it is being applied to different formats
  //     (TIFF, DPX) through a different encoder: half a quantization step,
  //     0.5/65535, amplified by the sRGB decode curve's steepest slope,
  //     2.4/1.055 * ((1+0.055)/1.055)^1.4 = 2.2749, giving 1.736e-5. Landed
  //     2.5e-5, 1.44x.
  constexpr float kInteger16Tol = 2.5e-5f;
  // (3) **Radiance RGBE (HDR), linear domain.** A fundamentally coarser
  //     storage than either: three 8-bit mantissas sharing one 8-bit
  //     exponent, so a channel's precision is set by the *largest* channel
  //     in its pixel. One mantissa step is 1/256 of the shared scale, and
  //     the shared scale is the smallest power of two above the largest
  //     channel -- so for a pixel whose largest channel is m, the absolute
  //     error on any channel is bounded by 2m/256 = m/128 (a full step, not
  //     half of one: RGBE encoders truncate rather than round to nearest).
  //     The fixture below peaks at m = 1.0, giving a bound of 7.8125e-3;
  //     measured 7.324e-3 -- close enough to the bound to confirm the model
  //     rather than merely not contradict it -- and landed 1.1e-2, 1.41x the
  //     bound and the same headroom ratio as the other two. Deliberately
  //     a separate, separately-labelled constant rather than folding HDR
  //     into the others: using one tolerance for RGBE and half would hide
  //     the fact that RGBE is ~16x coarser.
  constexpr float kRgbeTol = 1.1e-2f;

  // Same fixture helpers runExportTest() uses, and for the same reason:
  // writing *straight* linear RGBA through io/ImageIO.cpp's own `rgb *= a`
  // premultiply means the documents under test hold what a real
  // opened/painted document holds, and every precision claim is checked
  // against the tile's own post-half-rounding stored value rather than the
  // float literal that went in.
  auto writeStraight = [](Document& doc, size_t layerIndex, int32_t x, int32_t y, float r,
                          float g, float b, float a) {
    TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
    const PixelCoord p{x, y};
    tiles.getOrCreate(tileCoordAt(p)).writePixel(tileLocalOffset(p), {r * a, g * a, b * a, a});
  };
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

  // The shared 4x4 opaque ramp: 16 distinct values per channel spanning
  // [0,1] including both endpoints, each channel offset from the others so
  // a bug that swapped or copied channels could not pass.
  Document ramp = Document::createBlank(4, 4, WorkingSpace{});
  for (int i = 0; i < 16; ++i) {
    writeStraight(ramp, 0, i % 4, i / 4, static_cast<float>(i) / 15.0f,
                  static_cast<float>((i * 7) % 16) / 15.0f,
                  static_cast<float>((i * 11) % 16) / 15.0f, 1.0f);
  }

  // --- PRD I1's four: identical answers in both configurations ------------
  {
    struct Expect { ImageFormat format; bool alpha; bool uint8; bool uint16; };
    const Expect expects[] = {
        {ImageFormat::Png, true, true, true},
        {ImageFormat::Jpeg, false, true, false},
        {ImageFormat::Tga, true, true, false},
        {ImageFormat::Bmp, true, true, false},
    };
    bool available = true, stbBacked = true, depths = true, alpha = true, noFloat = true;
    for (const Expect& e : expects) {
      const FormatCapability& c = formatCapability(e.format);
      available = available && c.canRead && c.canWrite && c.unavailableReason.empty();
      stbBacked = stbBacked && c.backend == FormatBackend::Stb;
      depths = depths && c.canWriteDepth(ExportBitDepth::UInt8) == e.uint8 &&
               c.canWriteDepth(ExportBitDepth::UInt16) == e.uint16;
      alpha = alpha && c.hasAlpha == e.alpha;
      noFloat = noFloat && !c.canWriteDepth(ExportBitDepth::Half) &&
                !c.canWriteDepth(ExportBitDepth::Float32);
    }
    check(available, "I1: PNG/JPEG/TGA/BMP are readable and writable, with no caveat");
    check(stbBacked,
          "I1: and all four are stb-backed in BOTH configurations -- OpenImageIO never "
          "intercepts them, so it cannot regress them");
    check(depths, "I1: PNG carries 8 and 16-bit integer; JPEG/TGA/BMP carry 8 only");
    check(alpha, "I1: PNG/TGA/BMP carry alpha, JPEG does not");
    check(noFloat,
          "B6: none of the four integer formats claims a half or 32-bit-float depth");
  }

  // --- The OIIO-backed four: available exactly when the backend is --------
  {
    struct Expect {
      ImageFormat format;
      bool alpha;
      bool uint8, uint16, half, float32;
    };
    // Every `false` here for an otherwise-plausible depth is a case where
    // OpenImageIO accepts the request and writes something else: EXR+UINT8
    // and EXR+UINT16 -> half, TIFF+HALF and DPX+HALF -> float,
    // HDR+anything-but-FLOAT -> float. io/Capabilities probes for the
    // substitution and reports the depth unwritable, so these are `false`.
    //
    // Two of these rows were wrong when this test was first written -- I had
    // TIFF accepting half and DPX refusing 32-bit float, both from memory of
    // what those formats "usually" do. The runtime probe disagreed and the
    // probe was right (OpenImageIO's TIFF writer substitutes float for half;
    // its DPX writer genuinely writes 32-bit float, which the DPX spec's R32
    // element does allow). Left recorded here because it is the concrete
    // argument for PRD I3's wording: a hand-maintained table would have
    // shipped both mistakes, and the only reason this one is right is that
    // nobody is maintaining it -- the numbers below are transcribed from
    // what the linked library actually did.
    const Expect expects[] = {
        {ImageFormat::Exr, true, false, false, true, true},
        {ImageFormat::Tiff, true, true, true, false, true},
        {ImageFormat::Hdr, false, false, false, false, true},
        {ImageFormat::Dpx, true, true, true, false, true},
    };
    for (const Expect& e : expects) {
      const FormatCapability& c = formatCapability(e.format);
      char label[128];
      std::snprintf(label, sizeof(label),
                    "%s is readable+writable exactly when the OIIO backend is present",
                    imageFormatName(e.format));
      check(c.canRead && c.canWrite, label);
      std::snprintf(label, sizeof(label), "%s reports the right backend for this build",
                    imageFormatName(e.format));
      check(c.backend == FormatBackend::Oiio, label);

      std::snprintf(label, sizeof(label), "%s: the writable depth set matches this build",
                    imageFormatName(e.format));
      check(c.canWriteDepth(ExportBitDepth::UInt8) == e.uint8 &&
                c.canWriteDepth(ExportBitDepth::UInt16) == e.uint16 &&
                c.canWriteDepth(ExportBitDepth::Half) == e.half &&
                c.canWriteDepth(ExportBitDepth::Float32) == e.float32,
            label);
      std::snprintf(label, sizeof(label), "%s: alpha-channel support matches this build",
                    imageFormatName(e.format));
      check(c.hasAlpha == e.alpha, label);

      std::snprintf(label, sizeof(label), "%s: available, so it carries no failure reason",
                    imageFormatName(e.format));
      check(c.unavailableReason.empty(), label);
    }
    // HDR having no alpha is not written down anywhere -- it is discovered
    // by OpenImageIO's writer refusing to open a 4-channel image. Called
    // out separately because it is the clearest single case of a capability
    // that a hand-maintained table would have got wrong.
    check(formatCapability(ImageFormat::Hdr).hasAlpha == false,
          "HDR reports no alpha channel (Radiance RGBE is 3-channel by definition)");
  }

  // --- PSD: read-only where it exists at all ------------------------------
  {
    const FormatCapability& psd = formatCapability(ImageFormat::Psd);
    check(psd.canRead,
          "PSD is readable (flattened read is PLAN.md step 2's actual wording)");
    check(!psd.canWrite,
          "PSD is NOT writable -- PSD export is phase 15, and this OpenImageIO has no PSD "
          "writer at all");
    const ExportResult psdExport = exportDocument(ramp, ImageFormat::Psd,
                                                  ExportTargetSpace::Rec709Srgb,
                                                  ExportBitDepth::UInt8);
    check(!psdExport.ok && psdExport.bytes.empty(),
          "PSD export is refused and writes no bytes");
    check(contains(psdExport.error, "PSD") && contains(psdExport.error, "phase 15"),
          "PSD export's refusal names the format and why");
  }

  // --- Camera raw: whatever the LINKED OpenImageIO itself says. The I3 -----
  // assertion.
  //
  // This is not a per-platform `#ifdef`: `rawInLinkedOiio` is a runtime fact,
  // read from the same `oiioFormatPresent("raw")` query io/Capabilities.cpp
  // itself uses to decide `raw.canRead`/`raw.canWrite`. Two real answers
  // exist and both are asserted here, never guessed at from which OS this is:
  // the macOS build's own OpenImageIO is built without LibRaw (deliberately,
  // to keep LibRaw's transitive weight out), so `rawInLinkedOiio` is false
  // there and every line below prints byte-identical text to what this
  // section always printed. Ubuntu's `libopenimageio-dev` package, in
  // contrast, ships LibRaw in, so `rawInLinkedOiio` is true here and the
  // capability query's job is to say so truthfully -- "camera raw is
  // unsupported in every OIIO build" would itself be exactly the
  // hardcoded-table mistake PRD I3 exists to forbid, just written into the
  // test instead of into io/Capabilities.
  {
    const bool rawInLinkedOiio = oiioFormatPresent("raw");
    const FormatCapability& raw = formatCapability(ImageFormat::CameraRaw);
    if (!rawInLinkedOiio) {
      check(!raw.canRead && !raw.canWrite,
            "camera raw is unsupported in this build -- INCLUDING the OIIO build, which is "
            "the assertion a hardcoded 'NP_USE_OIIO implies step 2's list' table would fail");
      check(raw.backend == FormatBackend::None, "camera raw reports no backend at all");
      check(!raw.unavailableReason.empty(), "camera raw's refusal carries a reason");
      check(contains(raw.unavailableReason, "LibRaw"),
            "camera raw's reason names LibRaw's deliberate exclusion from this "
            "OpenImageIO build");
      check(contains(raw.unavailableReason, "run time") &&
                contains(raw.unavailableReason, "'raw'"),
            "camera raw's reason says the answer came from asking OpenImageIO at run "
            "time, and names the plugin it looked for");
    } else {
      // This build's linked OpenImageIO has LibRaw built in (its own
      // `format_list` names "raw"), so the honest answer is "supported", and
      // asserting anything else would just be a different hardcoded table.
      check(raw.canRead,
            "camera raw IS supported by this build's linked OpenImageIO -- its 'raw' "
            "plugin (LibRaw) is present, so the capability query says so rather than "
            "assuming the macOS build's answer");
      check(raw.backend == FormatBackend::Oiio,
            "camera raw reports the OIIO backend here, since that is the plugin actually "
            "answering the read");
      check(raw.unavailableReason.empty(),
            "camera raw carries no refusal reason in this build -- there is nothing to "
            "explain, it works");
    }
    char i3Label[200];
    std::snprintf(i3Label, sizeof(i3Label),
                  "I3: the query distinguishes two OIIO-listed formats from each other -- EXR "
                  "present, camera raw %s -- rather than answering per build option",
                  rawInLinkedOiio ? "present" : "absent");
    check(formatCapability(ImageFormat::Exr).canRead &&
              formatCapability(ImageFormat::CameraRaw).canRead == rawInLinkedOiio,
          i3Label);
  }

  // --- formatsThatCanWriteDepth(): the answer used to build refusals ------
  {
    const std::string u8 = formatsThatCanWriteDepth(ExportBitDepth::UInt8);
    const std::string u16 = formatsThatCanWriteDepth(ExportBitDepth::UInt16);
    const std::string h = formatsThatCanWriteDepth(ExportBitDepth::Half);
    const std::string f32 = formatsThatCanWriteDepth(ExportBitDepth::Float32);
    std::printf("    [query] 8-bit integer:  %s\n", u8.c_str());
    std::printf("    [query] 16-bit integer: %s\n", u16.c_str());
    std::printf("    [query] 16-bit half:    %s\n", h.empty() ? "(none)" : h.c_str());
    std::printf("    [query] 32-bit float:   %s\n", f32.empty() ? "(none)" : f32.c_str());
    check(contains(u8, "PNG") && contains(u8, "JPEG") && contains(u8, "TGA") &&
              contains(u8, "BMP"),
          "query: all four I1 formats are listed as 8-bit-writable");
    check(contains(u16, "PNG") && !contains(u16, "JPEG"),
          "query: PNG is listed as 16-bit-integer-writable and JPEG is not");
    check(!f32.empty(), "query: a 32-bit-float-capable format exists");
    check(h == "EXR",
          "query: EXR is the ONLY format here that writes half -- TIFF and DPX accept "
          "the request and write 32-bit float instead, so they are not listed");
    check(contains(f32, "EXR") && contains(f32, "TIFF") && contains(f32, "HDR") &&
              contains(f32, "DPX") && !contains(f32, "PNG"),
          "query: all four OIIO formats write 32-bit float, and none of the I1 four do");
  }

  // --- PRD B6 for the float depths: refused loudly by an integer format ---
  // Runs identically in both builds -- these four formats are stb-backed
  // either way, so this is not an OIIO-conditional claim.
  {
    const ImageFormat integerOnly[] = {ImageFormat::Png, ImageFormat::Jpeg, ImageFormat::Tga,
                                       ImageFormat::Bmp};
    bool allRefused = true, allNamed = true;
    for (ImageFormat f : integerOnly) {
      const ExportResult r = exportDocument(ramp, f, ExportTargetSpace::Rec709Linear,
                                            ExportBitDepth::Float32);
      allRefused = allRefused && !r.ok && r.bytes.empty();
      allNamed = allNamed && contains(r.error, imageFormatName(f)) &&
                 contains(r.error, "32-bit float") && contains(r.error, "PRD B6");
    }
    check(allRefused, "B6: a 32-bit-float request into PNG/JPEG/TGA/BMP writes no bytes");
    check(allNamed,
          "B6: and each error names the format, the refused depth and PRD B6 -- not a "
          "bare 'export failed'");

    const ExportResult halfPng = exportDocument(ramp, ImageFormat::Png,
                                                ExportTargetSpace::Rec709Linear,
                                                ExportBitDepth::Half);
    check(!halfPng.ok && contains(halfPng.error, "16-bit half float"),
          "B6: half into PNG is refused by its full name -- 'sixteen bits' is not "
          "ambiguous here between integer and half");
    // Control: the same PNG accepts the 16-bit *integer* request, so the
    // refusal above is about half specifically and not about 16 bits.
    check(exportDocument(ramp, ImageFormat::Png, ExportTargetSpace::Rec709Linear,
                         ExportBitDepth::UInt16)
              .ok,
          "B6 control: PNG still accepts 16-bit integer, so the half refusal is about "
          "the sample type and not the bit count");
  }

  // --- EXR export succeeds now that the OIIO backend is compiled in -------
  {
    const ExportResult exr = exportDocument(ramp, ImageFormat::Exr,
                                            ExportTargetSpace::Rec709Linear,
                                            ExportBitDepth::Half);
    check(exr.ok, "EXR export succeeds now that the OIIO backend is compiled in");
  }

  {
    // --- EXR round trip: exactly lossless, and why that is the right claim
    //
    // The chain is: tile (half) -> readPixel (exact) -> flatten's sum of a
    // single contribution (exact) -> un-premultiply by an alpha of exactly
    // 1.0 (exact) -> re-associate by the same 1.0 (exact) -> Rec709Linear,
    // i.e. no transfer function (exact) -> no clamp, because the depth is
    // float -> float-to-half, of a value that *is already a half* (exact).
    // Not one stage of that rounds, so the correct assertion is equality,
    // not a tolerance -- and it is the same claim docs/document-format.md
    // makes for the native container: "HALF channels -- byte-identical, no
    // conversion". A tolerance here would let a real regression through.
    {
      const ExportResult half = exportDocument(ramp, ImageFormat::Exr,
                                               ExportTargetSpace::Rec709Linear,
                                               ExportBitDepth::Half);
      check(half.ok && half.error.empty(), "EXR: a 4x4 ramp encodes to half without error");
      if (half.ok) {
        check(half.bytes.size() > 4 && half.bytes[0] == 0x76 && half.bytes[1] == 0x2f &&
                  half.bytes[2] == 0x31 && half.bytes[3] == 0x01,
              "EXR: the bytes really are an OpenEXR file (magic 0x76 0x2f 0x31 0x01)");
        // Decoded through the *production* entry point, which is what proves
        // io/ImageDecode's OpenImageIO fallback is wired up rather than just
        // written.
        const DecodedImage back = decodeImageLinear(half.bytes.data(), half.bytes.size());
        check(back.valid() && back.width == 4 && back.height == 4,
              "EXR: decodeImageLinear() reads it back at the right size -- the stb-first, "
              "OpenImageIO-fallback path works through the existing entry point");
        if (back.valid()) {
          float maxResidual = 0.0f;
          for (int i = 0; i < 16; ++i) {
            const std::array<float, 4> stored = storedPixel(ramp, 0, i % 4, i / 4);
            const std::array<float, 4> got =
                pixelOf(back, static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
            for (int c = 0; c < 4; ++c)
              maxResidual = std::max(maxResidual, std::fabs(got[c] - stored[c]));
          }
          std::printf("    [measured] EXR half   linear round-trip max residual = %.3e "
                      "(expected exactly 0)\n",
                      static_cast<double>(maxResidual));
          check(maxResidual == 0.0f,
                "EXR half: every channel of every pixel returns bit-exact -- the working "
                "space is already half, so a linear EXR loses nothing at all");
        }
      }

      const ExportResult f32 = exportDocument(ramp, ImageFormat::Exr,
                                              ExportTargetSpace::Rec709Linear,
                                              ExportBitDepth::Float32);
      check(f32.ok, "EXR: the same ramp encodes to 32-bit float");
      if (f32.ok) {
        check(f32.bytes.size() > half.bytes.size(),
              "EXR: the 32-bit-float file is larger than the half one -- the depth "
              "parameter reaches the actual writer");
        const DecodedImage back = decodeImageLinear(f32.bytes.data(), f32.bytes.size());
        if (back.valid()) {
          float maxResidual = 0.0f;
          for (int i = 0; i < 16; ++i) {
            const std::array<float, 4> stored = storedPixel(ramp, 0, i % 4, i / 4);
            const std::array<float, 4> got =
                pixelOf(back, static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
            for (int c = 0; c < 4; ++c)
              maxResidual = std::max(maxResidual, std::fabs(got[c] - stored[c]));
          }
          std::printf("    [measured] EXR float  linear round-trip max residual = %.3e "
                      "(expected exactly 0)\n",
                      static_cast<double>(maxResidual));
          check(maxResidual == 0.0f, "EXR 32-bit float: bit-exact round trip as well");
        }
      }
    }

    // --- The target space still reaches an EXR (PRD I5) -------------------
    //
    // Checked in the *encoded* domain: an sRGB-encoded EXR is a float file,
    // so the decoder correctly does not apply a curve to it, and what comes
    // back is the literal file sample. That makes "which transfer function
    // did the exporter apply" directly observable here, exactly as
    // runExportTest()'s own I5 case makes it observable for PNG.
    {
      const ExportResult lin = exportDocument(ramp, ImageFormat::Exr,
                                              ExportTargetSpace::Rec709Linear,
                                              ExportBitDepth::Half);
      const ExportResult srgb = exportDocument(ramp, ImageFormat::Exr,
                                               ExportTargetSpace::Rec709Srgb,
                                               ExportBitDepth::Half);
      check(lin.ok && srgb.ok && lin.bytes != srgb.bytes,
            "I5: EXR honours the target space -- linear and sRGB produce different files");
      const DecodedImage back = decodeImageLinear(srgb.bytes.data(), srgb.bytes.size());
      if (back.valid()) {
        float maxResidual = 0.0f;
        for (int i = 0; i < 16; ++i) {
          const std::array<float, 4> stored = storedPixel(ramp, 0, i % 4, i / 4);
          const std::array<float, 4> got =
              pixelOf(back, static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
          for (int c = 0; c < 3; ++c)
            maxResidual = std::max(maxResidual, std::fabs(got[c] - srgbEncode(stored[c])));
        }
        std::printf("    [measured] EXR half   sRGB sample residual         = %.3e "
                    "(tol %.3e)\n",
                    static_cast<double>(maxResidual), static_cast<double>(kHalfEncodedTol));
        check(maxResidual <= kHalfEncodedTol,
              "I5: each sRGB EXR sample is srgbEncode() of the stored value, within the "
              "derived half-float tolerance");
      }
    }

    // --- 32-bit float is genuinely deeper than half (PRD B6) --------------
    //
    // Driven through encodeLinearImage() with a hand-built DecodedImage
    // rather than a Document, deliberately: the working space's tiles are
    // themselves half, so a value that half cannot represent could not
    // survive long enough to reach the exporter from a Document. This is
    // the one place the 32-bit path's extra precision is actually
    // observable, so it is tested where it is observable.
    {
      DecodedImage img;
      img.width = 1;
      img.height = 1;
      img.pixels = {0.1f, 0.1f, 0.1f, 1.0f};  // 0.1 is not representable in half
      const ExportResult asHalf =
          encodeLinearImage(img, WorkingSpace{}, ImageFormat::Exr,
                            ExportTargetSpace::Rec709Linear, ExportBitDepth::Half);
      const ExportResult asFloat =
          encodeLinearImage(img, WorkingSpace{}, ImageFormat::Exr,
                            ExportTargetSpace::Rec709Linear, ExportBitDepth::Float32);
      check(asHalf.ok && asFloat.ok, "B6: 0.1 encodes to both half and 32-bit-float EXR");
      if (asHalf.ok && asFloat.ok) {
        const DecodedImage h = decodeImageLinear(asHalf.bytes.data(), asHalf.bytes.size());
        const DecodedImage f = decodeImageLinear(asFloat.bytes.data(), asFloat.bytes.size());
        if (h.valid() && f.valid()) {
          const float hv = pixelOf(h, 0, 0)[0], fv = pixelOf(f, 0, 0)[0];
          std::printf("    [measured] 0.1f -> half %.9f (err %.3e), float %.9f (err %.3e)\n",
                      static_cast<double>(hv), static_cast<double>(std::fabs(hv - 0.1f)),
                      static_cast<double>(fv), static_cast<double>(std::fabs(fv - 0.1f)));
          check(fv == 0.1f,
                "B6: the 32-bit-float file returns 0.1 bit-exact -- 32 bits really are "
                "32 bits, not 16 rounded up");
          check(hv != 0.1f && near(hv, 0.1f, 1e-4f),
                "B6: the half file returns a close but genuinely different value, so the "
                "two depths are not the same thing wearing different names");
        }
      }
    }

    // --- Values above 1.0: kept by a float depth, clipped by an integer ---
    {
      DecodedImage img;
      img.width = 1;
      img.height = 1;
      img.pixels = {4.0f, 12.5f, 1000.0f, 1.0f};  // all exact in half
      const ExportResult exr =
          encodeLinearImage(img, WorkingSpace{}, ImageFormat::Exr,
                            ExportTargetSpace::Rec709Linear, ExportBitDepth::Half);
      const ExportResult png =
          encodeLinearImage(img, WorkingSpace{}, ImageFormat::Png,
                            ExportTargetSpace::Rec709Linear, ExportBitDepth::UInt16);
      check(exr.ok && png.ok, "headroom: the same >1.0 pixel encodes to both EXR and PNG");
      if (exr.ok && png.ok) {
        const DecodedImage be = decodeImageLinear(exr.bytes.data(), exr.bytes.size());
        const DecodedImage bp = decodeImageLinear(png.bytes.data(), png.bytes.size());
        if (be.valid() && bp.valid()) {
          const auto e = pixelOf(be, 0, 0);
          const auto p = pixelOf(bp, 0, 0);
          check(e[0] == 4.0f && e[1] == 12.5f && e[2] == 1000.0f,
                "headroom: EXR keeps 4.0 / 12.5 / 1000.0 exactly -- the [0,1] clamp is "
                "keyed to the depth, not applied blindly");
          check(near(p[0], 1.0f, 1e-4f) && near(p[1], 1.0f, 1e-4f) &&
                    near(p[2], 1.0f, 1e-4f),
                "headroom: 16-bit-integer PNG clips all three to full scale, which is a "
                "property of asking for an integer file, not a depth truncation");
        }
      }
    }

    // --- Associated alpha: the two conversions agree ----------------------
    //
    // EXR is written with alpha associated (premultiplied) and read back
    // un-associated; TIFF is written and read straight. Exporting the same
    // translucent document to both and getting the same answer is what a
    // *paired* conversion looks like -- if either half were missing, the
    // EXR result would be off by a factor of alpha (2x here), which this
    // comparison would miss by three orders of magnitude. It does not prove
    // the file's samples are premultiplied (nothing reachable from here can
    // read the raw samples), and is not claimed to.
    {
      Document alpha = Document::createBlank(3, 1, WorkingSpace{});
      writeStraight(alpha, 0, 0, 0, 0.8f, 0.4f, 0.2f, 0.5f);
      // (1,0) deliberately never written -- fully transparent, exercising
      // the a <= 0 guard on both the associate and un-associate sides.
      writeStraight(alpha, 0, 2, 0, 0.25f, 0.5f, 0.75f, 1.0f);

      const ExportResult exr = exportDocument(alpha, ImageFormat::Exr,
                                              ExportTargetSpace::Rec709Linear,
                                              ExportBitDepth::Float32);
      const ExportResult tiff = exportDocument(alpha, ImageFormat::Tiff,
                                               ExportTargetSpace::Rec709Linear,
                                               ExportBitDepth::Float32);
      check(exr.ok && tiff.ok, "alpha: a translucent document exports to EXR and TIFF");
      if (exr.ok && tiff.ok) {
        const DecodedImage be = decodeImageLinear(exr.bytes.data(), exr.bytes.size());
        const DecodedImage bt = decodeImageLinear(tiff.bytes.data(), tiff.bytes.size());
        if (be.valid() && bt.valid()) {
          const auto e = pixelOf(be, 0, 0);
          const auto t = pixelOf(bt, 0, 0);
          const std::array<float, 4> raw = storedPixel(alpha, 0, 0, 0);
          check(near(e[0], t[0], 1e-6f) && near(e[1], t[1], 1e-6f) &&
                    near(e[2], t[2], 1e-6f) && near(e[3], t[3], 1e-6f),
                "alpha: EXR (associate on write, un-associate on read) and TIFF (neither) "
                "return the same straight colour -- the paired conversion cancels");
          check(near(e[0], raw[0] / raw[3], 1e-6f),
                "alpha: and that colour is the tile's own stored rgb divided by its own "
                "stored alpha, not a plausible-looking number");
          const auto empty = pixelOf(be, 1, 0);
          check(empty[0] == 0.0f && empty[1] == 0.0f && empty[2] == 0.0f && empty[3] == 0.0f,
                "alpha: a never-painted texel round-trips through EXR as transparent "
                "black, not a divide-by-zero NaN");
          check(near(pixelOf(be, 2, 0)[3], 1.0f, 1e-6f),
                "alpha: the opaque control keeps alpha 1 (association by 1.0 is identity)");
        }
      }
    }

    // --- TIFF and DPX at 16-bit integer -----------------------------------
    {
      const struct { ImageFormat format; const char* name; } cases[] = {
          {ImageFormat::Tiff, "TIFF"}, {ImageFormat::Dpx, "DPX"}};
      for (const auto& c : cases) {
        const ExportResult enc = exportDocument(ramp, c.format, ExportTargetSpace::Rec709Srgb,
                                                ExportBitDepth::UInt16);
        char label[96];
        std::snprintf(label, sizeof(label), "%s: 16-bit-integer sRGB export round-trips",
                      c.name);
        bool caseOk = enc.ok;
        float maxResidual = 0.0f;
        if (enc.ok) {
          const DecodedImage back = decodeImageLinear(enc.bytes.data(), enc.bytes.size());
          caseOk = back.valid() && back.width == 4 && back.height == 4;
          if (caseOk) {
            for (int i = 0; i < 16; ++i) {
              const std::array<float, 4> stored = storedPixel(ramp, 0, i % 4, i / 4);
              const std::array<float, 4> got =
                  pixelOf(back, static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
              for (int ch = 0; ch < 3; ++ch)
                maxResidual = std::max(maxResidual, std::fabs(got[ch] - stored[ch]));
            }
            std::printf("    [measured] %-4s 16-bit round-trip max residual   = %.3e "
                        "(tol %.3e)\n",
                        c.name, static_cast<double>(maxResidual),
                        static_cast<double>(kInteger16Tol));
            caseOk = maxResidual <= kInteger16Tol;
          }
        } else {
          std::printf("    (%s error was: %s)\n", c.name, enc.error.c_str());
        }
        check(caseOk, label);
      }
    }

    // --- HDR: no alpha, 32-bit float only, and RGBE's coarser precision ---
    {
      Document translucent = Document::createBlank(2, 1, WorkingSpace{});
      writeStraight(translucent, 0, 0, 0, 0.5f, 0.5f, 0.5f, 1.0f);
      writeStraight(translucent, 0, 1, 0, 0.5f, 0.5f, 0.5f, 0.25f);
      const ExportResult refused = exportDocument(translucent, ImageFormat::Hdr,
                                                  ExportTargetSpace::Rec709Linear,
                                                  ExportBitDepth::Float32);
      check(!refused.ok && contains(refused.error, "no alpha channel") &&
                contains(refused.error, "x=1, y=0"),
            "HDR: a translucent document is refused by name, the same check JPEG gets -- "
            "and nothing had to be told that HDR has no alpha");

      const ExportResult enc = exportDocument(ramp, ImageFormat::Hdr,
                                              ExportTargetSpace::Rec709Linear,
                                              ExportBitDepth::Float32);
      check(enc.ok, "HDR: the fully opaque ramp exports");
      if (enc.ok) {
        const DecodedImage back = decodeImageLinear(enc.bytes.data(), enc.bytes.size());
        check(back.valid() && back.width == 4 && back.height == 4,
              "HDR: decodeImageLinear() reads the Radiance file back at the right size");
        if (back.valid()) {
          float maxResidual = 0.0f;
          for (int i = 0; i < 16; ++i) {
            const std::array<float, 4> stored = storedPixel(ramp, 0, i % 4, i / 4);
            const std::array<float, 4> got =
                pixelOf(back, static_cast<uint32_t>(i % 4), static_cast<uint32_t>(i / 4));
            for (int ch = 0; ch < 3; ++ch)
              maxResidual = std::max(maxResidual, std::fabs(got[ch] - stored[ch]));
          }
          std::printf("    [measured] HDR (RGBE) round-trip max residual    = %.3e "
                      "(tol %.3e)\n",
                      static_cast<double>(maxResidual), static_cast<double>(kRgbeTol));
          check(maxResidual <= kRgbeTol,
                "HDR: within the separately derived RGBE tolerance -- shared-exponent "
                "storage, ~16x coarser than half, and labelled as such");
          check(near(pixelOf(back, 0, 0)[3], 1.0f, 1e-6f),
                "HDR: the synthesized alpha comes back fully opaque");
        }
      }
      const ExportResult wrongDepth = exportDocument(ramp, ImageFormat::Hdr,
                                                    ExportTargetSpace::Rec709Linear,
                                                    ExportBitDepth::UInt8);
      check(!wrongDepth.ok && contains(wrongDepth.error, "HDR") &&
                contains(wrongDepth.error, "8-bit integer"),
            "HDR: an 8-bit request is refused, even though OpenImageIO would have "
            "accepted it and written 32-bit float instead");
    }

    // --- Flattened PSD read, from a hand-built fixture ---------------------
    //
    // Built byte by byte here rather than checked in as a binary, for the
    // same reason io/Export's encodePng16() started life as a --selftest
    // fixture builder: a test whose input this file constructs from the
    // published file layout cannot pass by construction against a decoder
    // that shares its assumptions. Layout (Adobe Photoshop File Formats
    // spec): 26-byte header, then three length-prefixed sections left
    // empty, then a compression word and raw *planar* channel data.
    {
      std::vector<uint8_t> psd;
      auto u16 = [&](uint32_t v) {
        psd.push_back(static_cast<uint8_t>(v >> 8));
        psd.push_back(static_cast<uint8_t>(v));
      };
      auto u32 = [&](uint32_t v) {
        psd.push_back(static_cast<uint8_t>(v >> 24));
        psd.push_back(static_cast<uint8_t>(v >> 16));
        psd.push_back(static_cast<uint8_t>(v >> 8));
        psd.push_back(static_cast<uint8_t>(v));
      };
      const char signature[] = "8BPS";
      psd.insert(psd.end(), signature, signature + 4);
      u16(1);                                    // version 1 (PSD, not PSB)
      for (int i = 0; i < 6; ++i) psd.push_back(0);  // reserved, must be zero
      u16(3);                                    // channel count
      u32(2);                                    // height
      u32(2);                                    // width
      u16(8);                                    // bits per channel
      u16(3);                                    // colour mode: RGB
      u32(0);                                    // colour mode data: none
      u32(0);                                    // image resources: none
      u32(0);                                    // layer and mask info: none
      u16(0);                                    // compression: raw
      // Planar: the whole R plane, then G, then B. Four pixels: red, green,
      // blue, and one mixed value whose exact sRGB-decoded result is
      // hand-checkable below.
      const uint8_t planes[3][4] = {{255, 0, 0, 255}, {0, 255, 0, 128}, {0, 0, 255, 64}};
      for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 4; ++i) psd.push_back(planes[c][i]);

      std::string err;
      const DecodedImage back = decodeImageLinear(psd.data(), psd.size(), &err);
      check(back.valid() && back.width == 2 && back.height == 2,
            "PSD: a hand-built 52-byte flattened PSD decodes through decodeImageLinear()");
      if (back.valid()) {
        const auto red = pixelOf(back, 0, 0);
        const auto mixed = pixelOf(back, 1, 1);
        check(near(red[0], 1.0f, 1e-6f) && near(red[1], 0.0f, 1e-6f) &&
                  near(red[2], 0.0f, 1e-6f) && near(red[3], 1.0f, 1e-6f),
              "PSD: the planar channel layout is read in the right order (pixel 0 is red, "
              "not 'the first byte of each plane concatenated')");
        // 8-bit integer source, so io/OiioBackend applies the same sRGB
        // decode assumption io/ImageDecode.cpp already documents for
        // untagged integer files -- checked against color/Space's own curve
        // rather than a literal.
        check(near(mixed[0], srgbDecode(255.0f / 255.0f), 1e-6f) &&
                  near(mixed[1], srgbDecode(128.0f / 255.0f), 1e-6f) &&
                  near(mixed[2], srgbDecode(64.0f / 255.0f), 1e-6f),
              "PSD: an integer-typed source is sRGB-decoded to linear, matching "
              "io/ImageDecode's own documented assumption for untagged files");
        check(near(mixed[3], 1.0f, 1e-6f),
              "PSD: a 3-channel source decodes as fully opaque");
      }
    }
  }

  std::printf("[selftest] format support %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
