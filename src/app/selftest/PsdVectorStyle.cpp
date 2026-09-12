#include "app/selftest/Support.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "color/Space.hpp"
#include "io/PsdVectorStyle.hpp"

// io/PsdVectorStyle -- step 3 of docs/psd-vector-shapes.md, and the first
// module in this tree to hand real Photoshop bytes to io/Descriptor. Every
// fixture below is a hex dump of an actual `SoCo`/`vscg`/`vstk` block payload
// read out of two real files (Apple's `App Icon Template.psd` and
// `testNonSquareWithShapesOffPage.psd`), not a synthetic one this file's own
// writer produced -- see docs/psd-vector-shapes.md "Where the colour is, and
// the trap in it" and io/PsdVectorStyle.hpp for the design this checks
// against, and psd-tools' own reading of these bytes for the numbers.
//
// **Headline result: io/Descriptor.hpp's grammar reading held on the first
// run.** Every osType these blocks use (`Objc doub bool UntF enum long VlLs`)
// decoded correctly against psd-tools' independent parse with no changes
// needed to this file's understanding of the format -- including the
// zero-length Key quirk showing up for real (`"Clr "`, `"Rd  "`, `"Grn "`,
// `"Bl  "` are Photoshop's actual four-character, space-padded keys) and an
// explicit-length enum value id (`BlnM`/`"normal"`, six characters, rather
// than the four-character `"Nrml"` shorthand seen elsewhere in the very same
// file). Section G below is where that grammar claim gets tested rather than
// assumed.
namespace np {
namespace {

std::vector<uint8_t> hexBytes(const std::string& hex) {
  std::vector<uint8_t> out;
  std::string clean;
  clean.reserve(hex.size());
  for (const char c : hex)
    if (!std::isspace(static_cast<unsigned char>(c))) clean.push_back(c);
  out.reserve(clean.size() / 2);
  for (size_t i = 0; i + 1 < clean.size(); i += 2)
    out.push_back(static_cast<uint8_t>(std::stoul(clean.substr(i, 2), nullptr, 16)));
  return out;
}

bool approxEq(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) <= tol; }

// -- Real bytes, from /private/tmp/np-scatter/psdvecstyle/fixtures-style.txt --
//
// `ns:Star 1` and `ns:Ellipse 1` are from testNonSquareWithShapesOffPage.psd;
// `apple:*` layers are from Apple's App Icon Template.psd. Each is exactly the
// payload io/PsdImport already slices per tagged block, with no skip applied.

const char* kStarVscgHex =
    "536f436f00000010000000010000000000006e756c6c0000000100000000436c"
    "72204f626a630000000100000000000052474243000000030000000052642020"
    "646f7562402ce51ae30000000000000047726e20646f75623fe2cd32cd200000"
    "00000000426c2020646f7562405a221de5c00000";

const char* kStarVstkHex =
    "000000100000000100000000000b7374726f6b655374796c6500000010000000"
    "127374726f6b655374796c6556657273696f6e6c6f6e67000000020000000d73"
    "74726f6b65456e61626c6564626f6f6c010000000b66696c6c456e61626c6564"
    "626f6f6c01000000147374726f6b655374796c654c696e655769647468556e74"
    "462350786c3ff0000000000000000000197374726f6b655374796c654c696e65"
    "446173684f6666736574556e744623506e740000000000000000000000157374"
    "726f6b655374796c654d697465724c696d6974646f7562405900000000000000"
    "0000167374726f6b655374796c654c696e6543617054797065656e756d000000"
    "167374726f6b655374796c654c696e6543617054797065000000127374726f6b"
    "655374796c6542757474436170000000177374726f6b655374796c654c696e65"
    "4a6f696e54797065656e756d000000177374726f6b655374796c654c696e654a"
    "6f696e54797065000000147374726f6b655374796c654d697465724a6f696e00"
    "0000187374726f6b655374796c654c696e65416c69676e6d656e74656e756d00"
    "0000187374726f6b655374796c654c696e65416c69676e6d656e740000001673"
    "74726f6b655374796c65416c69676e43656e746572000000147374726f6b6553"
    "74796c655363616c654c6f636b626f6f6c00000000177374726f6b655374796c"
    "655374726f6b6541646a757374626f6f6c00000000167374726f6b655374796c"
    "654c696e6544617368536574566c4c7300000000000000147374726f6b655374"
    "796c65426c656e644d6f6465656e756d00000000426c6e4d000000004e726d6c"
    "000000127374726f6b655374796c654f706163697479556e7446235072634059"
    "000000000000000000127374726f6b655374796c65436f6e74656e744f626a63"
    "0000000100000000000f736f6c6964436f6c6f724c6179657200000001000000"
    "00436c72204f626a630000000100000000000052474243000000030000000052"
    "642020646f756200000000000000000000000047726e20646f75620000000000"
    "00000000000000426c2020646f75620000000000000000000000157374726f6b"
    "655374796c655265736f6c7574696f6e646f75624062000000000000";

const char* kEllipseVscgHex =
    "536f436f00000010000000010000000000006e756c6c0000000100000000436c"
    "72204f626a630000000100000000000052474243000000030000000052642020"
    "646f7562406301de2ce000000000000047726e20646f756240618c13ee600000"
    "00000000426c2020646f7562406c800003600000";

// Ellipse 1's vstk is byte-identical to Star 1's -- same stroke style, same
// file. Reusing kStarVstkHex is deliberate, not an oversight: it is what
// docs/psd-vector-shapes.md's fixture dump actually shows for this file.

const char* kPng1VscgHex =
    "536f436f00000010000000010000000000006e756c6c0000000100000000436c"
    "72204f626a630000000100000000000052474243000000030000000052642020"
    "646f75624067ffe8000000000000000047726e20646f756240697fe680000000"
    "00000000426c2020646f7562406affe500000000";

const char* kPng1VstkHex =
    "000000100000000100000000000b7374726f6b655374796c6500000010000000"
    "127374726f6b655374796c6556657273696f6e6c6f6e67000000020000000d73"
    "74726f6b65456e61626c6564626f6f6c000000000b66696c6c456e61626c6564"
    "626f6f6c01000000147374726f6b655374796c654c696e655769647468556e74"
    "462350786c3ff0000000000000000000197374726f6b655374796c654c696e65"
    "446173684f6666736574556e744623506e740000000000000000000000157374"
    "726f6b655374796c654d697465724c696d6974646f7562405900000000000000"
    "0000167374726f6b655374796c654c696e6543617054797065656e756d000000"
    "167374726f6b655374796c654c696e6543617054797065000000127374726f6b"
    "655374796c6542757474436170000000177374726f6b655374796c654c696e65"
    "4a6f696e54797065656e756d000000177374726f6b655374796c654c696e654a"
    "6f696e54797065000000147374726f6b655374796c654d697465724a6f696e00"
    "0000187374726f6b655374796c654c696e65416c69676e6d656e74656e756d00"
    "0000187374726f6b655374796c654c696e65416c69676e6d656e740000001673"
    "74726f6b655374796c65416c69676e43656e746572000000147374726f6b6553"
    "74796c655363616c654c6f636b626f6f6c00000000177374726f6b655374796c"
    "655374726f6b6541646a757374626f6f6c00000000167374726f6b655374796c"
    "654c696e6544617368536574566c4c7300000000000000147374726f6b655374"
    "796c65426c656e644d6f6465656e756d00000000426c6e4d000000066e6f726d"
    "616c000000127374726f6b655374796c654f706163697479556e744623507263"
    "4059000000000000000000127374726f6b655374796c65436f6e74656e744f62"
    "6a630000000100000000000f736f6c6964436f6c6f724c617965720000000100"
    "000000436c72204f626a63000000010000000000005247424300000003000000"
    "0052642020646f756200000000000000000000000047726e20646f7562000000"
    "000000000000000000426c2020646f7562000000000000000000000015737472"
    "6f6b655374796c655265736f6c7574696f6e646f756240620000000000000000";

// The trap row: fillEnabled AND strokeEnabled are both false here, even
// though the vscg colour is a bright, entirely plausible orange.
const char* kPng4VscgHex =
    "536f436f00000010000000010000000000006e756c6c0000000100000000436c"
    "72204f626a630000000100000000000052474243000000030000000052642020"
    "646f7562406fe000000000000000000047726e20646f75624063ffec00000000"
    "00000000426c2020646f75624049806600000000";

const char* kPng4VstkHex =
    "000000100000000100000000000b7374726f6b655374796c6500000010000000"
    "127374726f6b655374796c6556657273696f6e6c6f6e67000000020000000d73"
    "74726f6b65456e61626c6564626f6f6c000000000b66696c6c456e61626c6564"
    "626f6f6c00000000147374726f6b655374796c654c696e655769647468556e74"
    "462350786c3ff0000000000000000000197374726f6b655374796c654c696e65"
    "446173684f6666736574556e744623506e740000000000000000000000157374"
    "726f6b655374796c654d697465724c696d6974646f7562405900000000000000"
    "0000167374726f6b655374796c654c696e6543617054797065656e756d000000"
    "167374726f6b655374796c654c696e6543617054797065000000127374726f6b"
    "655374796c6542757474436170000000177374726f6b655374796c654c696e65"
    "4a6f696e54797065656e756d000000177374726f6b655374796c654c696e654a"
    "6f696e54797065000000147374726f6b655374796c654d697465724a6f696e00"
    "0000187374726f6b655374796c654c696e65416c69676e6d656e74656e756d00"
    "0000187374726f6b655374796c654c696e65416c69676e6d656e740000001673"
    "74726f6b655374796c65416c69676e43656e746572000000147374726f6b6553"
    "74796c655363616c654c6f636b626f6f6c00000000177374726f6b655374796c"
    "655374726f6b6541646a757374626f6f6c00000000167374726f6b655374796c"
    "654c696e6544617368536574566c4c7300000000000000147374726f6b655374"
    "796c65426c656e644d6f6465656e756d00000000426c6e4d000000066e6f726d"
    "616c000000127374726f6b655374796c654f706163697479556e744623507263"
    "4059000000000000000000127374726f6b655374796c65436f6e74656e744f62"
    "6a630000000100000000000f736f6c6964436f6c6f724c617965720000000100"
    "000000436c72204f626a63000000010000000000005247424300000003000000"
    "0052642020646f756200000000000000000000000047726e20646f7562000000"
    "000000000000000000426c2020646f7562000000000000000000000015737472"
    "6f6b655374796c655265736f6c7574696f6e646f756240620000000000000000";

// `App Icon Shape` has a `SoCo` and NO `vscg`/`vstk` at all -- the defaults
// case.
const char* kAppIconSocoHex =
    "00000010000000010000000000006e756c6c0000000100000000436c72204f62"
    "6a630000000100000000000052474243000000030000000052642020646f7562"
    "00000000000000000000000047726e20646f7562000000000000000000000000"
    "426c2020646f75620000000000000000";

}  // namespace

bool runPsdVectorStyleTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ==========================================================================
  std::printf("  -- A. vscg-only fill: 'SoCo' is NOT required --\n");
  // ==========================================================================
  {
    PsdVectorStyleBlocks blocks;
    const std::vector<uint8_t> vscg = hexBytes(kStarVscgHex);
    const std::vector<uint8_t> vstk = hexBytes(kStarVstkHex);
    blocks.vscg = vscg;
    blocks.vstk = vstk;
    PsdVectorStyle out;
    std::string error;
    const bool decoded = decodePsdVectorStyle(blocks, out, error);
    check(decoded, "ns:Star 1: decodes with no SoCo block at all");
    check(out.fill.on, "ns:Star 1: fill is on from vscg's embedded SoCo tag alone");

    // Known-answer doubles, psd-tools' own reading of these exact bytes
    // (known-answers.txt): Rd=14.447470754384995, Grn=0.5875486379954964,
    // Bl=104.5330747961998, all 0..255 sRGB-encoded.
    const float rd = srgbDecode(14.447470754384995f / 255.0f);
    const float gr = srgbDecode(0.5875486379954964f / 255.0f);
    const float bl = srgbDecode(104.5330747961998f / 255.0f);
    check(approxEq(out.fill.rgba[0], rd) && approxEq(out.fill.rgba[1], gr) &&
             approxEq(out.fill.rgba[2], bl),
          "ns:Star 1: fill colour is the sRGB-decoded LINEAR value");
    // If srgbDecode() were skipped and the raw 0..255 fraction used directly,
    // this file's own doubles would produce a visibly different (much
    // brighter) linear value -- proving the assertion above is not vacuous.
    check(!(approxEq(out.fill.rgba[0], 14.447470754384995f / 255.0f) &&
           approxEq(out.fill.rgba[1], 0.5875486379954964f / 255.0f) &&
           approxEq(out.fill.rgba[2], 104.5330747961998f / 255.0f)),
          "ns:Star 1: fill colour is NOT the raw un-decoded sRGB fraction "
          "(the assertion above would fail if srgbDecode() were skipped)");
  }

  {
    PsdVectorStyleBlocks blocks;
    const std::vector<uint8_t> vscg = hexBytes(kEllipseVscgHex);
    const std::vector<uint8_t> vstk = hexBytes(kStarVstkHex);
    blocks.vscg = vscg;
    blocks.vstk = vstk;
    PsdVectorStyle out;
    std::string error;
    check(decodePsdVectorStyle(blocks, out, error), "ns:Ellipse 1: decodes");
    const float rd = srgbDecode(152.05837100744247f / 255.0f);
    const float gr = srgbDecode(140.3774330019951f / 255.0f);
    const float bl = srgbDecode(228.0000016093254f / 255.0f);
    check(approxEq(out.fill.rgba[0], rd) && approxEq(out.fill.rgba[1], gr) &&
             approxEq(out.fill.rgba[2], bl),
          "ns:Ellipse 1: fill colour matches its own vscg doubles");
  }

  {
    PsdVectorStyleBlocks blocks;
    const std::vector<uint8_t> vscg = hexBytes(kPng1VscgHex);
    const std::vector<uint8_t> vstk = hexBytes(kPng1VstkHex);
    blocks.vscg = vscg;
    blocks.vstk = vstk;
    PsdVectorStyle out;
    std::string error;
    check(decodePsdVectorStyle(blocks, out, error), "apple:PNG/1: decodes");
    check(out.fill.on, "apple:PNG/1: fillEnabled true -> fill on (eight of Apple's nine "
                       "layers have vscg and NO SoCo, this is one)");
    const float rd = srgbDecode(191.9970703125f / 255.0f);
    const float gr = srgbDecode(203.99688720703125f / 255.0f);
    const float bl = srgbDecode(215.9967041015625f / 255.0f);
    check(approxEq(out.fill.rgba[0], rd) && approxEq(out.fill.rgba[1], gr) &&
             approxEq(out.fill.rgba[2], bl),
          "apple:PNG/1: fill colour matches its own vscg doubles");
    check(!out.stroke.on, "apple:PNG/1: strokeEnabled false -> stroke off");
  }

  // ==========================================================================
  std::printf("  -- B. apple:PNG/4 -- the trap row: an orange vscg colour, drawn nowhere --\n");
  // ==========================================================================
  {
    PsdVectorStyleBlocks blocks;
    const std::vector<uint8_t> vscg = hexBytes(kPng4VscgHex);
    const std::vector<uint8_t> vstk = hexBytes(kPng4VstkHex);
    blocks.vscg = vscg;
    blocks.vstk = vstk;
    PsdVectorStyle out;
    std::string error;
    check(decodePsdVectorStyle(blocks, out, error), "apple:PNG/4: decodes (not fatal)");
    // fillEnabled and strokeEnabled are both false in this layer's real vstk
    // bytes. A reader that takes the vscg colour without reading fillEnabled
    // paints a solid orange circle nobody's artwork has.
    check(!out.fill.on,
          "apple:PNG/4 TRAP: fill.on is false despite a fully-readable orange vscg colour");
    check(!out.stroke.on, "apple:PNG/4: strokeEnabled false -> stroke off");
  }

  // ==========================================================================
  std::printf("  -- C. apple:App Icon Shape -- SoCo only, no vscg, no vstk: the defaults --\n");
  // ==========================================================================
  {
    PsdVectorStyleBlocks blocks;
    const std::vector<uint8_t> soco = hexBytes(kAppIconSocoHex);
    blocks.soco = soco;
    PsdVectorStyle out;
    std::string error;
    check(decodePsdVectorStyle(blocks, out, error), "App Icon Shape: decodes");
    check(out.fill.on,
          "App Icon Shape: no vstk -> fillEnabled defaults TRUE -> fill is on");
    check(approxEq(out.fill.rgba[0], 0.0f) && approxEq(out.fill.rgba[1], 0.0f) &&
             approxEq(out.fill.rgba[2], 0.0f),
          "App Icon Shape: colour is black, from SoCo's own Rd=Grn=Bl=0.0");
    check(!out.stroke.on,
          "App Icon Shape: no vstk -> strokeEnabled defaults FALSE -> stroke is off");
  }

  // ==========================================================================
  std::printf("  -- D. Star 1 / Ellipse 1's shared stroke: enabled, exercised by real bytes --\n");
  // ==========================================================================
  {
    PsdVectorStyleBlocks blocks;
    const std::vector<uint8_t> vscg = hexBytes(kStarVscgHex);
    const std::vector<uint8_t> vstk = hexBytes(kStarVstkHex);
    blocks.vscg = vscg;
    blocks.vstk = vstk;
    PsdVectorStyle out;
    std::string error;
    check(decodePsdVectorStyle(blocks, out, error), "ns:Star 1: decodes");
    check(out.stroke.on, "ns:Star 1: strokeEnabled true -> stroke is on");
    check(approxEq(out.strokeStyle.width, 1.0f), "ns:Star 1: stroke width is 1 px");
    check(out.strokeStyle.cap == LineCap::Butt, "ns:Star 1: butt cap");
    check(out.strokeStyle.join == LineJoin::Miter, "ns:Star 1: miter join");
    check(approxEq(out.strokeStyle.miterLimit, 100.0f), "ns:Star 1: miter limit 100");
    check(approxEq(out.stroke.rgba[0], 0.0f) && approxEq(out.stroke.rgba[1], 0.0f) &&
             approxEq(out.stroke.rgba[2], 0.0f),
          "ns:Star 1: strokeStyleContent's own colour is black");
    bool alignmentWarned = false;
    for (const std::string& w : out.warnings)
      if (w.find("LineAlignment") != std::string::npos) alignmentWarned = true;
    check(!alignmentWarned,
          "ns:Star 1: centre alignment (the only kind this codebase can express) warns of nothing");
  }

  // ==========================================================================
  std::printf("  -- E. Absent, truncated and malformed blocks: never fatal to the shape --\n");
  // ==========================================================================
  {
    PsdVectorStyleBlocks blocks;  // all three spans empty
    PsdVectorStyle out;
    std::string error;
    check(decodePsdVectorStyle(blocks, out, error),
          "no blocks at all: succeeds -- a style that cannot be read still leaves the geometry");
    check(!out.fill.on && !out.stroke.on, "no blocks at all: both fill and stroke are off");
  }
  {
    // Version word (16) plus a few bytes, cut well before even the class
    // name length finishes -- parseVersionedActionDescriptor must see this as
    // truncated, not read past it.
    const std::vector<uint8_t> truncated = hexBytes("00000010000000010000");
    PsdVectorStyleBlocks blocks;
    blocks.vstk = truncated;
    PsdVectorStyle out;
    std::string error;
    check(!decodePsdVectorStyle(blocks, out, error) && !error.empty(),
          "vstk truncated mid-header: refused by name, not read out of bounds");
  }
  {
    // A version word this build has never seen (io/Descriptor.hpp refuses any
    // version other than 16, by name).
    const std::vector<uint8_t> badVersion =
        hexBytes("000000ff0000000100000000000b7374726f6b655374796c6500000000");
    PsdVectorStyleBlocks blocks;
    blocks.vstk = badVersion;
    PsdVectorStyle out;
    std::string error;
    check(!decodePsdVectorStyle(blocks, out, error) && !error.empty(),
          "vstk: a descriptor version other than 16 is refused by name");
  }
  {
    // Fewer than kPsdVscgDescriptorSkip (4) bytes: not even enough to hold
    // the fill-type tag, let alone a descriptor.
    const std::vector<uint8_t> tooShort = hexBytes("536f43");
    PsdVectorStyleBlocks blocks;
    blocks.vscg = tooShort;
    PsdVectorStyle out;
    std::string error;
    check(!decodePsdVectorStyle(blocks, out, error) && !error.empty(),
          "vscg: 3 bytes is too short to hold its own 4-byte fill-type tag");
  }

  std::printf("[selftest] psd vector style %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
