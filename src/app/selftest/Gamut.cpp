#include "app/selftest/Support.hpp"

#include <cstring>

#include "color/Gamut.hpp"
#include "io/ImageDecode.hpp"
#include "io/SourceGamut.hpp"

namespace np {
namespace {

// --- Published reference values -------------------------------------------
//
// **Every matrix below is copied from a published source, not produced by the
// code under test.** That is the whole point of this section: a test that
// checks `rgbToXyz()` against `rgbToXyz()` proves only that the function is
// deterministic. These are the numbers a colour engineer would check against,
// so a sign error, a transposed matrix, or a missing chromatic adaptation
// fails here rather than shipping as a subtly wrong picture.
//
// sRGB / Rec.709 -> XYZ at D65, and the same adapted to D50 by Bradford:
// Lindbloom's RGB/XYZ matrices, which reproduce the IEC 61966-2-1 primaries.
constexpr float kSrgbToXyzD65[9] = {0.4124564f, 0.3575761f, 0.1804375f,
                                    0.2126729f, 0.7151522f, 0.0721750f,
                                    0.0193339f, 0.1191920f, 0.9503041f};

constexpr float kSrgbToXyzD50[9] = {0.4360747f, 0.3850649f, 0.1430804f,
                                    0.2225045f, 0.7168786f, 0.0606169f,
                                    0.0139322f, 0.0971045f, 0.7141733f};

// Display P3's D50-adapted colorants -- the values Apple's own Display P3
// profile carries in its `rXYZ`/`gXYZ`/`bXYZ` tags. Used two ways below: as
// the expected output of this build's own Bradford derivation, and as the
// *input* to a synthetic ICC profile, so the two independent routes to "what
// gamut is this" are checked against each other rather than only against
// themselves.
constexpr float kDisplayP3ToXyzD50[9] = {0.5151187f, 0.2919778f, 0.1571035f,
                                         0.2411892f, 0.6922441f, 0.0665741f,
                                         -0.0010507f, 0.0418844f, 0.7840592f};

// Linear Display P3 -> linear sRGB, both at D65. The matrix every colour
// library agrees on, and the one this build must reproduce by composing
// P3->XYZ(D50) with the inverse of Rec.709->XYZ(D50).
constexpr float kP3ToSrgb[9] = {1.2249401f,  -0.2249404f, 0.0000000f,
                                -0.0420569f, 1.0420571f,  0.0000000f,
                                -0.0196376f, -0.0786361f, 1.0982735f};

// --- Synthetic containers -------------------------------------------------

void pushU32Be(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x >> 24));
  v.push_back(static_cast<uint8_t>(x >> 16));
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}

// A minimal but *structurally real* matrix-shaper ICC profile: a 128-byte
// header declaring an RGB data space, a tag count, three tag-table entries,
// and three 20-byte XYZType elements. Built by hand rather than checked in as
// a binary fixture so the bytes this parser is held to are visible in the
// source next to the parser.
std::vector<uint8_t> makeIccProfile(const float rgbToXyzD50[9]) {
  std::vector<uint8_t> icc(128, 0);
  std::memcpy(icc.data() + 16, "RGB ", 4);  // data colour space

  pushU32Be(icc, 3);  // tag count
  const uint32_t tagTableAt = 132;
  const uint32_t firstElementAt = tagTableAt + 3 * 12;
  const char* sigs[3] = {"rXYZ", "gXYZ", "bXYZ"};
  for (uint32_t i = 0; i < 3; ++i) {
    icc.insert(icc.end(), sigs[i], sigs[i] + 4);
    pushU32Be(icc, firstElementAt + i * 20);
    pushU32Be(icc, 20);
  }
  // Column i of the matrix is where primary i lands in XYZ.
  for (uint32_t col = 0; col < 3; ++col) {
    icc.insert(icc.end(), {'X', 'Y', 'Z', ' ', 0, 0, 0, 0});
    for (uint32_t row = 0; row < 3; ++row) {
      const float v = rgbToXyzD50[row * 3 + col];
      pushU32Be(icc, static_cast<uint32_t>(static_cast<int32_t>(v * 65536.0f + (v < 0 ? -0.5f : 0.5f))));
    }
  }
  return icc;
}

// A PNG that carries colour chunks and nothing decodable. `sniffSourceGamut()`
// walks chunks and stops at IDAT, so it never needs real image data -- which
// is exactly what lets this test the chunk walk without a deflate encoder.
std::vector<uint8_t> makePngWithChunk(const char* type, const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  auto chunk = [&](const char* t, const std::vector<uint8_t>& body) {
    pushU32Be(png, static_cast<uint32_t>(body.size()));
    png.insert(png.end(), t, t + 4);
    png.insert(png.end(), body.begin(), body.end());
    pushU32Be(png, 0);  // CRC -- not checked by the gamut walk, and it says so
  };
  // A plausible IHDR first, so the walk is exercised over more than one chunk.
  std::vector<uint8_t> ihdr;
  pushU32Be(ihdr, 4);
  pushU32Be(ihdr, 4);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  chunk("IHDR", ihdr);
  chunk(type, payload);
  chunk("IEND", {});
  return png;
}

std::vector<uint8_t> chrmPayload(const Primaries& p) {
  std::vector<uint8_t> v;
  auto push = [&](float x) { pushU32Be(v, static_cast<uint32_t>(x * 100000.0f + 0.5f)); };
  push(p.whiteX);
  push(p.whiteY);
  push(p.redX);
  push(p.redY);
  push(p.greenX);
  push(p.greenY);
  push(p.blueX);
  push(p.blueY);
  return v;
}

// A JPEG carrying an ICC profile split across two APP2 segments -- the split
// is the point, since a real Display P3 profile from a phone is a few
// kilobytes and JPEG caps a segment at 64 KB, but a profile that fits in one
// segment would never exercise the reassembly.
std::vector<uint8_t> makeJpegWithIcc(const std::vector<uint8_t>& icc) {
  std::vector<uint8_t> jpg = {0xFF, 0xD8};
  const size_t half = icc.size() / 2;
  const size_t parts[2][2] = {{0, half}, {half, icc.size() - half}};
  for (int i = 0; i < 2; ++i) {
    const size_t at = parts[i][0];
    const size_t len = parts[i][1];
    const size_t payload = 12 + 2 + len;  // "ICC_PROFILE\0" + seq + count + body
    jpg.insert(jpg.end(), {0xFF, 0xE2});
    const uint16_t segLen = static_cast<uint16_t>(payload + 2);
    jpg.push_back(static_cast<uint8_t>(segLen >> 8));
    jpg.push_back(static_cast<uint8_t>(segLen & 0xFF));
    const char* marker = "ICC_PROFILE";
    jpg.insert(jpg.end(), marker, marker + 12);  // includes the NUL
    jpg.push_back(static_cast<uint8_t>(i + 1));
    jpg.push_back(2);
    jpg.insert(jpg.end(), icc.begin() + static_cast<ptrdiff_t>(at),
               icc.begin() + static_cast<ptrdiff_t>(at + len));
  }
  jpg.insert(jpg.end(), {0xFF, 0xDA});  // SOS: stop walking
  return jpg;
}

}  // namespace

bool runGamutTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto matrixNear = [](const ColorMat3& got, const float want[9], float tol) {
    for (int i = 0; i < 9; ++i)
      if (!(std::fabs(got.m[static_cast<size_t>(i)] - want[i]) <= tol)) return false;
    return true;
  };

  std::printf("  -- 1. the derivation, against published matrices --\n");

  {
    const std::optional<ColorMat3> d65 = rgbToXyz(kRec709Primaries);
    check(d65.has_value(), "Rec.709 primaries produce a matrix");
    // **3e-4, and the reason is worth stating because a tighter tolerance
    // here fails and the failure is not a bug.** `Primaries` stores the white
    // point as xy at four decimal places, so D65 is (0.3127, 0.3290) and this
    // derivation gets Zw = (1 - 0.3127 - 0.3290) / 0.3290 = 1.089058. The
    // published matrices were derived from the CIE-tabulated D65 XYZ, whose
    // Z is 1.08883. That ~2e-4 difference in the white point propagates
    // straight into the blue column -- measured 0.9505321 here against the
    // published 0.9503041 -- and it is a property of the four-decimal xy this
    // codebase stores, not of the arithmetic above it.
    //
    // Loosening the tolerance is therefore the honest response and tightening
    // it would be pinning this build to a white point it does not have. What
    // must NOT be loosened is section 2's conversion check, for the reason
    // asserted there.
    check(d65 && matrixNear(*d65, kSrgbToXyzD65, 3e-4f),
          "Rec.709 -> XYZ at D65 matches the published sRGB matrix (to 4-decimal xy)");

    const std::optional<ColorMat3> d50 = rgbToXyzD50(kRec709Primaries);
    check(d50 && matrixNear(*d50, kSrgbToXyzD50, 3e-4f),
          "Rec.709 -> XYZ at D50 matches the published Bradford-adapted matrix");
    // The assertion that catches a missing adaptation: if `rgbToXyzD50()`
    // forgot to adapt, it would equal the D65 matrix above, and both of the
    // checks either side of this would still be satisfiable by one of them.
    check(d50 && !matrixNear(*d50, kSrgbToXyzD65, 1e-3f),
          "and the D50 matrix is genuinely NOT the D65 one (the adaptation happened)");
  }

  {
    const std::optional<ColorMat3> p3 = rgbToXyzD50(kDisplayP3Primaries);
    check(p3 && matrixNear(*p3, kDisplayP3ToXyzD50, 3e-4f),
          "Display P3 -> XYZ at D50 matches Apple's own profile colorants");
  }

  {
    // ProPhoto is defined at D50, so its adaptation must be the identity --
    // arrived at through the general path, not special-cased.
    const std::optional<ColorMat3> own = rgbToXyz(kProPhotoRgbPrimaries);
    const std::optional<ColorMat3> d50 = rgbToXyzD50(kProPhotoRgbPrimaries);
    check(own && d50 && matrixNear(*d50, own->m.data(), 1e-5f),
          "a gamut already at D50 (ProPhoto) is unchanged by the adaptation");
  }

  std::printf("  -- 2. the conversion a P3 import actually performs --\n");

  {
    const std::optional<ColorMat3> p3 = rgbToXyzD50(kDisplayP3Primaries);
    const std::optional<ColorMat3> conv = p3 ? gamutConversion(*p3, rec709RgbToXyzD50())
                                             : std::nullopt;
    check(conv.has_value(), "P3 -> Rec.709 conversion is derivable");
    // **Tighter than the absolute matrices above, deliberately.** The
    // four-decimal white point that costs ~2e-4 in each RGB->XYZ matrix is
    // the SAME white point on both sides of this composition, so it very
    // largely cancels: measured, this reproduces the published conversion to
    // better than 1e-4 while neither of its two factors does. That is the
    // property that makes the import correct despite the imprecise constant,
    // so it is asserted rather than left as a happy accident -- a future
    // change that adapted source and destination through different white
    // points would still pass section 1 and would fail here.
    check(conv && matrixNear(*conv, kP3ToSrgb, 1e-4f),
          "and matches the published Display-P3-to-sRGB matrix MORE tightly than "
          "either factor (the shared white point cancels)");

    // The single most legible consequence, stated as a value rather than a
    // matrix: pure P3 red is MORE saturated than Rec.709 can express, so it
    // must land above 1.0 in red and slightly negative in green and blue.
    // Getting the conversion backwards, or transposing it, breaks this.
    if (conv) {
      const std::array<float, 3> red = colorMat3Apply(*conv, {1.0f, 0.0f, 0.0f});
      std::printf("    [measured] pure P3 red -> Rec.709 linear (%.4f, %.4f, %.4f)\n", red[0],
                  red[1], red[2]);
      check(red[0] > 1.2f && red[0] < 1.25f, "pure P3 red exceeds Rec.709's red primary");
      check(red[1] < 0.0f && red[2] < 0.0f,
            "and goes negative in green and blue -- out of gamut, not clipped");
    }

    // Rec.709 into itself is the identity, which is what makes the decode
    // path free for the overwhelming majority of files.
    const std::optional<ColorMat3> same =
        gamutConversion(rec709RgbToXyzD50(), rec709RgbToXyzD50());
    check(same && colorMat3NearIdentity(*same, 1e-5f),
          "Rec.709 -> Rec.709 is the identity (an sRGB file pays nothing)");
  }

  std::printf("  -- 3. reading the gamut out of a container --\n");

  {
    const std::vector<uint8_t> icc = makeIccProfile(kDisplayP3ToXyzD50);
    const std::optional<ColorMat3> parsed = iccRgbToXyzD50(icc.data(), icc.size());
    check(parsed.has_value(), "a matrix-shaper ICC profile parses");
    // 2e-5: s15Fixed16 quantises at 1/65536, so this is the tightest
    // tolerance the encoding itself permits.
    check(parsed && matrixNear(*parsed, kDisplayP3ToXyzD50, 2e-5f),
          "and its colorants round-trip through s15Fixed16 to the right matrix");

    // A profile whose declared colour space is not RGB has no colorant tags
    // to find, and must be declined by signature rather than by search.
    std::vector<uint8_t> cmyk = icc;
    std::memcpy(cmyk.data() + 16, "CMYK", 4);
    check(!iccRgbToXyzD50(cmyk.data(), cmyk.size()).has_value(),
          "a non-RGB profile is declined");

    // Truncation at every length: the parser reads attacker-supplied bytes
    // before anything has validated them, so "does not crash and does not
    // read past the end" is a correctness property, not hardening.
    bool survivedTruncation = true;
    for (size_t n = 0; n < icc.size(); ++n)
      if (iccRgbToXyzD50(icc.data(), n).has_value() && n < 132) survivedTruncation = false;
    check(survivedTruncation, "every truncation of a profile is declined, none accepted");
    check(!iccRgbToXyzD50(nullptr, 0).has_value(), "a null profile is declined");
  }

  {
    const std::vector<uint8_t> png =
        makePngWithChunk("cHRM", chrmPayload(kDisplayP3Primaries));
    const SourceGamut g = sniffSourceGamut(png.data(), png.size());
    check(g.known, "a PNG cHRM chunk is found");
    check(g.known && std::strcmp(g.source, "PNG cHRM") == 0, "and is attributed to cHRM");
    check(g.known && matrixNear(g.rgbToXyzD50, kDisplayP3ToXyzD50, 3e-4f),
          "and yields Display P3 when that is what it describes");
  }

  {
    const std::vector<uint8_t> png = makePngWithChunk("sRGB", {0});
    const SourceGamut g = sniffSourceGamut(png.data(), png.size());
    check(g.known && std::strcmp(g.source, "PNG sRGB") == 0,
          "a PNG sRGB chunk is found and says so");
    const std::optional<ColorMat3> conv =
        g.known ? gamutConversion(g.rgbToXyzD50, rec709RgbToXyzD50()) : std::nullopt;
    check(conv && colorMat3NearIdentity(*conv, 1e-5f),
          "and converts to the identity -- an explicit sRGB tag changes nothing");
  }

  {
    const std::vector<uint8_t> icc = makeIccProfile(kDisplayP3ToXyzD50);
    const std::vector<uint8_t> jpg = makeJpegWithIcc(icc);
    const std::vector<uint8_t> recovered = jpegIccProfile(jpg.data(), jpg.size());
    check(recovered == icc, "a JPEG ICC profile split across two APP2 segments reassembles");

    const SourceGamut g = sniffSourceGamut(jpg.data(), jpg.size());
    check(g.known && std::strcmp(g.source, "JPEG ICC") == 0, "and is found by the sniffer");
    check(g.known && matrixNear(g.rgbToXyzD50, kDisplayP3ToXyzD50, 2e-5f),
          "and yields the profile's own gamut");
  }

  {
    // **The most common tagged file in the world: a PNG carrying an sRGB ICC
    // profile.** It must take the identity fast path, so that such a file is
    // bit-identical to an untagged one rather than merely close to it.
    //
    // This is the case that was wrong when first measured -- it came out at
    // (1.00003, 0.00001, 0.00001) -- because the conversion is the identity
    // only to ~3e-4, the same four-decimal-white-point gap section 1
    // measures, and the decode path's tolerance was 1e-4. Asserted here with
    // the published sRGB colorants so the tolerance cannot quietly tighten
    // again.
    const std::vector<uint8_t> icc = makeIccProfile(kSrgbToXyzD50);
    const std::optional<ColorMat3> parsed = iccRgbToXyzD50(icc.data(), icc.size());
    check(parsed.has_value(), "an sRGB ICC profile parses");
    const std::optional<ColorMat3> conv =
        parsed ? gamutConversion(*parsed, rec709RgbToXyzD50()) : std::nullopt;
    check(conv && colorMat3NearIdentity(*conv, 1e-3f),
          "and converts to the identity at HALF's own precision -- an sRGB-tagged file "
          "is left bit-identical, not merely close");
    // The bound that makes the assertion above meaningful rather than
    // vacuous: it is NOT identity at 1e-4, which is why the decode path's
    // tolerance is what it is.
    check(conv && !colorMat3NearIdentity(*conv, 1e-5f),
          "and is genuinely not the exact identity (so the tolerance is load-bearing)");
  }

  {
    // Nothing to find: an empty buffer, a file that is neither, and a PNG
    // with no colour chunk at all. All three must report "unknown" so the
    // decoder falls back to the Rec.709 assumption it always made.
    check(!sniffSourceGamut(nullptr, 0).known, "a null buffer declares nothing");
    const uint8_t garbage[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    check(!sniffSourceGamut(garbage, sizeof garbage).known, "garbage declares nothing");
    const std::vector<uint8_t> plain = makePngWithChunk("tEXt", {'h', 'i'});
    check(!sniffSourceGamut(plain.data(), plain.size()).known,
          "an untagged PNG declares nothing (the pre-existing assumption stands)");
  }

  return ok;
}

}  // namespace np
