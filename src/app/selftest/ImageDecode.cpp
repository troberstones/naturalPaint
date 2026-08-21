#include "app/selftest/Support.hpp"

namespace np {

// io/ImageDecode (PLAN.md Phase 2 step 6, decode half). PLAN.md's own verify
// criterion for this phase is explicit: open both an 8-bit and a 16-bit PNG
// and check known pixel values, so those two cases are not optional. BMP,
// TGA and JPEG fixtures round out coverage of the other three formats
// STBI_ONLY_x now admits (Palette.cpp) -- these are the format the actual
// STBI_ONLY_x wiring could silently break (a wrong macro leaves stb_image's
// decoder for that format compiled out and decodeImageLinear() failing at
// runtime instead of at compile time).
bool runImageDecodeTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  auto near = [](float a, float b, float tol) { return std::fabs(a - b) <= tol; };
  // 8-bit source: byte quantization plus the transfer-function curve itself
  // can shift the decoded value by a bit more than 1/255 in the steep part
  // of the curve, hence a tolerance a little looser than "1 ULP at 8 bits".
  constexpr float kTol8 = 0.01f;
  // 16-bit source is precise enough for a tight check.
  constexpr float kTol16 = 0.001f;
  // JPEG is lossy (8x8 block DCT + chroma subsampling) -- even a flat block
  // isn't guaranteed byte-exact after quantization, so this stays generous
  // and is not meant to catch small regressions, only "did JPEG decode at
  // all and land in roughly the right place."
  constexpr float kTolJpeg = 0.06f;

  // --- 8-bit PNG: 2x2 fixture with known corners --------------------------
  {
    const uint8_t px[2 * 2 * 4] = {
        255, 255, 255, 255,  0,   0,   0,   255,
        128, 128, 128, 255,  200, 40,  40,  128,
    };
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 2, 2, 4, px, 2 * 4);

    std::string err;
    const DecodedImage img = decodeImageLinear(png.data(), png.size(), &err);
    check(img.valid(), "8-bit PNG fixture decodes");
    if (img.valid()) {
      check(img.width == 2 && img.height == 2, "8-bit PNG: dimensions match");
      auto at = [&](int x, int y) { return &img.pixels[(static_cast<size_t>(y) * 2 + x) * 4]; };
      const float* tl = at(0, 0);
      const float* tr = at(1, 0);
      const float* bl = at(0, 1);
      const float* br = at(1, 1);
      check(near(tl[0], 1.0f, kTol8) && near(tl[1], 1.0f, kTol8) && near(tl[2], 1.0f, kTol8) &&
                near(tl[3], 1.0f, kTol8),
            "8-bit PNG: white corner (255) decodes to linear (1,1,1,1)");
      check(near(tr[0], 0.0f, kTol8) && near(tr[1], 0.0f, kTol8) && near(tr[2], 0.0f, kTol8) &&
                near(tr[3], 1.0f, kTol8),
            "8-bit PNG: black corner (0) decodes to linear (0,0,0,1)");
      check(near(bl[0], srgbDecode(128 / 255.0f), kTol8) &&
                near(bl[1], srgbDecode(128 / 255.0f), kTol8),
            "8-bit PNG: mid-grey (128) matches srgbDecode(128/255)");
      check(near(br[0], srgbDecode(200 / 255.0f), kTol8) &&
                !near(br[0], 200 / 255.0f, 0.02f),
            "8-bit PNG: colour channel is sRGB-decoded, not left encoded");
      check(near(br[3], 128 / 255.0f, 1e-4f),
            "8-bit PNG: alpha (128) passes through linearly, not sRGB-decoded");
    }
  }

  // --- 8-bit PNG, no alpha channel: decodes fully opaque -------------------
  {
    const uint8_t px[3] = {100, 100, 100};
    std::vector<uint8_t> png;
    stbi_write_png_to_func(&appendToVector, &png, 1, 1, 3, px, 3);

    const DecodedImage img = decodeImageLinear(png.data(), png.size());
    check(img.valid() && img.width == 1 && img.height == 1, "3-channel PNG fixture decodes");
    if (img.valid()) {
      check(near(img.pixels[3], 1.0f, 1e-4f),
            "PNG with no alpha channel decodes fully opaque (alpha = 1.0)");
      check(near(img.pixels[0], srgbDecode(100 / 255.0f), kTol8),
            "3-channel PNG: colour channel still sRGB-decoded");
    }
  }

  // --- 16-bit PNG: 2x2 fixture, built by io/Export's encodePng16() ---------
  // stb_image_write's PNG writer (used for every other fixture here) is
  // 8-bit-per-channel only, so this fixture goes through the hand-rolled
  // 16-bit writer instead. That writer used to live in this file as a
  // test-only helper; PRD B6 made 16-bit export a P0 production
  // requirement, so it is now io/Export.hpp's encodePng16() and this
  // fixture calls the same one production export does -- there is exactly
  // one 16-bit PNG writer in the binary, not a test copy that can drift.
  {
    const uint16_t px[2 * 2 * 4] = {
        65535, 65535, 65535, 65535,  0,     0,     0,     65535,
        32768, 32768, 32768, 65535,  0,     0,     65535, 32768,
    };
    const std::vector<uint8_t> png = encodePng16(2, 2, px);

    std::string err;
    const DecodedImage img = decodeImageLinear(png.data(), png.size(), &err);
    check(img.valid(), "16-bit PNG fixture decodes");
    if (!img.valid() && !err.empty()) std::printf("    (%s)\n", err.c_str());
    if (img.valid()) {
      check(img.width == 2 && img.height == 2, "16-bit PNG: dimensions match");
      auto at = [&](int x, int y) { return &img.pixels[(static_cast<size_t>(y) * 2 + x) * 4]; };
      const float* tl = at(0, 0);
      const float* tr = at(1, 0);
      const float* bl = at(0, 1);
      const float* br = at(1, 1);
      check(near(tl[0], 1.0f, kTol16) && near(tl[3], 1.0f, kTol16),
            "16-bit PNG: white corner (65535) ~ linear (1,1,1,1)");
      check(near(tr[0], 0.0f, kTol16) && near(tr[3], 1.0f, kTol16),
            "16-bit PNG: black corner (0) ~ linear (0,0,0,1)");
      check(near(bl[0], srgbDecode(32768 / 65535.0f), kTol16),
            "16-bit PNG: mid-grey (32768) matches srgbDecode(32768/65535)");
      check(near(br[2], srgbDecode(65535 / 65535.0f), kTol16) && near(br[0], 0.0f, kTol16) &&
                near(br[1], 0.0f, kTol16),
            "16-bit PNG: pure-blue pixel isolated to the right channel");
      check(near(br[3], 32768 / 65535.0f, 1e-4f),
            "16-bit PNG: alpha (32768) passes through linearly at 16-bit precision");
    }
  }

  // --- BMP: lossless container, exact-value check --------------------------
  {
    const uint8_t px[2 * 2 * 4] = {
        255, 255, 255, 255,  0,   0,   0,   255,
        255, 0,   0,   255,  0,   255, 0,   255,
    };
    std::vector<uint8_t> bmp;
    stbi_write_bmp_to_func(&appendToVector, &bmp, 2, 2, 4, px);

    const DecodedImage img = decodeImageLinear(bmp.data(), bmp.size());
    check(img.valid() && img.width == 2 && img.height == 2, "BMP fixture decodes");
    if (img.valid()) {
      // BMP is lossless but stb_image normalizes row order to top-to-bottom
      // regardless of source format (BMP's own on-disk convention is
      // bottom-up), so corner checks here are the same shape as PNG's.
      auto at = [&](int x, int y) { return &img.pixels[(static_cast<size_t>(y) * 2 + x) * 4]; };
      check(near(at(0, 0)[0], 1.0f, kTol8), "BMP: white corner round-trips");
      check(near(at(1, 0)[0], 0.0f, kTol8), "BMP: black corner round-trips");
      check(near(at(0, 1)[0], srgbDecode(1.0f), kTol8) && near(at(0, 1)[1], 0.0f, kTol8),
            "BMP: pure-red pixel isolated to the right channel");
    }
  }

  // --- TGA: lossless container, exact-value check ---------------------------
  {
    const uint8_t px[2 * 2 * 4] = {
        255, 255, 255, 255,  0,   0,   0,   255,
        255, 0,   0,   255,  0,   255, 0,   255,
    };
    std::vector<uint8_t> tga;
    stbi_write_tga_to_func(&appendToVector, &tga, 2, 2, 4, px);

    const DecodedImage img = decodeImageLinear(tga.data(), tga.size());
    check(img.valid() && img.width == 2 && img.height == 2, "TGA fixture decodes");
    if (img.valid()) {
      auto at = [&](int x, int y) { return &img.pixels[(static_cast<size_t>(y) * 2 + x) * 4]; };
      check(near(at(0, 0)[0], 1.0f, kTol8), "TGA: white corner round-trips");
      check(near(at(1, 0)[0], 0.0f, kTol8), "TGA: black corner round-trips");
      check(near(at(0, 1)[0], srgbDecode(1.0f), kTol8) && near(at(0, 1)[1], 0.0f, kTol8),
            "TGA: pure-red pixel isolated to the right channel");
    }
  }

  // --- JPEG: lossy, generous tolerance, no exact-value assertions ----------
  {
    // 16x16, aligned to JPEG's 8x8 DCT block size: left half solid white,
    // right half solid black, sampled well away from the block boundary in
    // the middle to dodge quantization ringing at the edge.
    constexpr int kJpegSize = 16;
    std::vector<uint8_t> px(static_cast<size_t>(kJpegSize) * kJpegSize * 3);
    for (int y = 0; y < kJpegSize; ++y)
      for (int x = 0; x < kJpegSize; ++x) {
        const uint8_t v = (x < kJpegSize / 2) ? 255 : 0;
        uint8_t* p = &px[(static_cast<size_t>(y) * kJpegSize + x) * 3];
        p[0] = p[1] = p[2] = v;
      }
    std::vector<uint8_t> jpg;
    stbi_write_jpg_to_func(&appendToVector, &jpg, kJpegSize, kJpegSize, 3, px.data(), 90);

    const DecodedImage img = decodeImageLinear(jpg.data(), jpg.size());
    check(img.valid() && img.width == kJpegSize && img.height == kJpegSize,
          "JPEG fixture decodes");
    if (img.valid()) {
      auto at = [&](int x, int y) {
        return &img.pixels[(static_cast<size_t>(y) * kJpegSize + x) * 4];
      };
      const float* white = at(3, 8);   // interior of the white block
      const float* black = at(12, 8);  // interior of the black block
      check(near(white[0], 1.0f, kTolJpeg), "JPEG: white block decodes near linear 1.0");
      check(near(black[0], 0.0f, kTolJpeg), "JPEG: black block decodes near linear 0.0");
      check(near(white[3], 1.0f, 1e-4f),
            "JPEG (no alpha channel) decodes fully opaque (alpha = 1.0)");
    }
  }

  std::printf("[selftest] image decode %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
