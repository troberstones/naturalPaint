#include "app/selftest/Support.hpp"

#include <cstdio>

#include "app/selftest/DescFixture.hpp"
#include "io/GimpBrush.hpp"

namespace np {
namespace {

// Hand-built fixtures, for the reason io/GimpBrush.hpp's header states: no
// `.gbr` or `.gih` GIMP itself wrote was available. The framing below is the
// published standard's field order, so these prove the READER agrees with the
// specification -- they cannot prove the specification agrees with GIMP, and
// the header says which of the two this module is uncertain about.
std::vector<uint8_t> buildGbr(uint32_t version, const char* name, uint32_t width, uint32_t height,
                              uint32_t depth, uint32_t spacing,
                              const std::vector<uint8_t>& pixels, bool breakMagic = false) {
  size_t nameLength = 0;
  for (const char* p = name; *p != '\0'; ++p) ++nameLength;
  ++nameLength;  // the NUL the standard includes in header_size

  const uint32_t fixed = (version == 1) ? 20u : 28u;
  DescFixture f;
  f.u32v(fixed + static_cast<uint32_t>(nameLength));
  f.u32v(version);
  f.u32v(width);
  f.u32v(height);
  f.u32v(depth);
  if (version == 2) {
    f.u32v(breakMagic ? 0x47494D51u : 0x47494D50u);  // 'GIMP', or one bit off
    f.u32v(spacing);
  }
  for (const char* p = name; *p != '\0'; ++p) f.u8v(static_cast<unsigned char>(*p));
  f.u8v(0);
  for (const uint8_t b : pixels) f.u8v(b);
  return f.bytes;
}

std::vector<uint8_t> buildGih(const char* name, const char* params,
                              const std::vector<std::vector<uint8_t>>& cells) {
  DescFixture f;
  for (const char* p = name; *p != '\0'; ++p) f.u8v(static_cast<unsigned char>(*p));
  f.u8v('\n');
  for (const char* p = params; *p != '\0'; ++p) f.u8v(static_cast<unsigned char>(*p));
  f.u8v('\n');
  for (const auto& c : cells)
    for (const uint8_t b : c) f.u8v(b);
  return f.bytes;
}

}  // namespace

bool runGimpBrushTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const std::vector<uint8_t> grey3x2 = {10, 20, 30, 40, 50, 60};

  // ==========================================================================
  std::printf("  -- A. .gbr versions 1 and 2 --\n");
  // ==========================================================================
  {
    const GimpBrushResult v2 =
        readGimpBrush(buildGbr(2, "Chalk 01", 3, 2, 1, 25, grey3x2));
    check(v2.ok && v2.tips.size() == 1, "gbr: a well-formed version 2 brush reads");
    if (v2.ok && v2.tips.size() == 1) {
      check(v2.tips[0].name == "Chalk 01",
            "gbr: the name fills header_size past the fixed header, NUL dropped");
      check(v2.tips[0].width == 3 && v2.tips[0].height == 2 && v2.tips[0].alpha == grey3x2,
            "gbr: a greyscale brush's mask bytes are coverage, verbatim");
      check(v2.tips[0].haveSpacing && v2.tips[0].spacingPercent == 25,
            "gbr: version 2's spacing is read, as a percentage of the width");
    }

    // Version 1's fixed header is 20 bytes, not 28 -- it has neither the magic
    // nor the spacing. Reading it as version 2 takes eight bytes of the name
    // as those two fields and then reads the pixels eight bytes late, which
    // produces a shifted brush rather than a refusal.
    const GimpBrushResult v1 = readGimpBrush(buildGbr(1, "Old", 3, 2, 1, 0, grey3x2));
    check(v1.ok && v1.tips.size() == 1 && v1.tips[0].name == "Old" &&
              v1.tips[0].alpha == grey3x2,
          "gbr: version 1's 20-byte fixed header is not read as version 2's 28");
    check(v1.ok && !v1.tips[0].haveSpacing,
          "gbr: version 1 reports NO spacing rather than a default that looks like the file's");
  }

  // ==========================================================================
  std::printf("  -- B. RGBA, and what is dropped --\n");
  // ==========================================================================
  {
    // Distinct values per channel so a wrong stride shows as the wrong byte
    // rather than as a plausible one.
    const std::vector<uint8_t> rgba = {1, 2, 3, 200, 4, 5, 6, 100, 7, 8, 9, 50, 10, 11, 12, 25};
    const GimpBrushResult r = readGimpBrush(buildGbr(2, "Colour", 2, 2, 4, 10, rgba));
    check(r.ok && r.tips.size() == 1 &&
              r.tips[0].alpha == std::vector<uint8_t>({200, 100, 50, 25}),
          "gbr: an RGBA brush's ALPHA is the coverage, at stride 4, offset 3");
  }

  // ==========================================================================
  std::printf("  -- C. refusals, by name --\n");
  // ==========================================================================
  {
    const GimpBrushResult badMagic =
        readGimpBrush(buildGbr(2, "x", 3, 2, 1, 25, grey3x2, /*breakMagic=*/true));
    check(!badMagic.ok && badMagic.tips.empty(),
          "gbr: a version 2 file whose GIMP magic is absent is refused");

    const GimpBrushResult badVersion = readGimpBrush(buildGbr(3, "x", 3, 2, 1, 25, grey3x2));
    check(!badVersion.ok, "gbr: an unknown version is refused by name, not parsed hopefully");

    const GimpBrushResult badDepth = readGimpBrush(buildGbr(2, "x", 3, 2, 3, 25, grey3x2));
    check(!badDepth.ok, "gbr: a colour depth outside {1,4} is refused, not guessed");

    // Every truncation: none may read out of bounds and none may claim a tip.
    const auto whole = buildGbr(2, "Chalk 01", 3, 2, 1, 25, grey3x2);
    bool everyPrefixRefuses = true;
    for (size_t n = 0; n < whole.size(); ++n)
      if (readGimpBrush(std::span<const uint8_t>(whole.data(), n)).ok) everyPrefixRefuses = false;
    check(everyPrefixRefuses, "gbr: every truncation of a good file is refused, never half-read");
  }

  // ==========================================================================
  std::printf("  -- D. .gih, the image hose --\n");
  // ==========================================================================
  {
    const auto cell = buildGbr(2, "cell", 3, 2, 1, 25, grey3x2);
    const GimpBrushResult r =
        readGimpBrushPipe(buildGih("Fire", "3 ncells:3 step:20 dim:1 rank0:3 selection:random",
                                   {cell, cell, cell}));
    check(r.ok && r.tips.size() == 3,
          "gih: the cell count is the parameter line's leading integer");
    if (r.tips.size() == 3) {
      check(r.tips[0].name == "cell 01" && r.tips[2].name == "cell 03",
            "gih: cells are numbered, so the dab library gets distinct ids");
      check(r.tips[0].alpha == grey3x2 && r.tips[2].alpha == grey3x2,
            "gih: each cell decodes as a complete .gbr record, back to back");
    }

    // A hose whose cells stop arriving is a truncated file. Half a hose is not
    // a shorter hose, and returning one would present a bug as a brush set.
    const GimpBrushResult shortHose =
        readGimpBrushPipe(buildGih("Fire", "3 ncells:3", {cell, cell}));
    check(!shortHose.ok && shortHose.tips.empty(),
          "gih: a hose missing a declared cell is refused whole, not truncated");

    const GimpBrushResult noCount = readGimpBrushPipe(buildGih("Fire", "ncells:3", {cell}));
    check(!noCount.ok, "gih: a parameter line with no leading count is refused");
  }

  std::printf("[selftest] gimp brush %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
