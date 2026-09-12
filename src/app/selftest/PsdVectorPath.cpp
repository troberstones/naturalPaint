#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "io/PsdVectorPath.hpp"

// io/PsdVectorPath -- decodePsdPathRecords(), step 1 of docs/psd-vector-
// shapes.md. Headless, GPU-free, no filesystem.
//
//   A. Five known-answer fixtures: real `vsms`/`vmsk` payloads dumped byte
//      for byte from two Photoshop files (Apple's `App Icon Template.psd`
//      and `testNonSquareWithShapesOffPage.psd`), decoded here and checked
//      against numbers independently derived -- geometrically, from a
//      second file field (`vogk`'s own doubles), and against a third-party
//      render's bounding box. A pass proves this reader agrees with those
//      three, not merely with itself.
//   B. Hand-built fixtures for the framing this module must survive without
//      reading a byte outside the block: the pad-remainder rule, a
//      truncated block, a block shorter than its own header, a garbage
//      selector, an unknown path-operation value, an open subpath, a
//      zero-knot subpath, and a subpath whose declared knot count outruns
//      the records actually present.
namespace np {
namespace {

bool nearf(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

bool contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}
bool anyContains(const std::vector<std::string>& hay, const std::string& needle) {
  for (const std::string& s : hay)
    if (contains(s, needle)) return true;
  return false;
}

// --- hex -> bytes, for the real fixtures dumped as hex text -------------
std::vector<uint8_t> hexToBytes(const std::string& hex) {
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  std::vector<uint8_t> out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<uint8_t>((val(hex[i]) << 4) | val(hex[i + 1])));
  }
  return out;
}

// --- a tiny big-endian byte writer, for the hand-built fixtures in B, in
// PsdImport.cpp's ByteWriter mould but scoped to this file only (Support.hpp
// -- "internal linkage cannot cross a TU boundary"). ----------------------
struct ByteWriter {
  std::vector<uint8_t> b;
  void u8(uint32_t v) { b.push_back(static_cast<uint8_t>(v & 0xFFu)); }
  void u16(uint32_t v) { u8(v >> 8); u8(v); }
  void u32(uint32_t v) { u8(v >> 24); u8(v >> 16); u8(v >> 8); u8(v); }
  void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
  void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
};

std::vector<uint8_t> pathBlockHeader(uint32_t version = 3, uint32_t flags = 0) {
  ByteWriter w;
  w.u32(version);
  w.u32(flags);
  return w.b;
}

// A type-6/8 record: selector then 24 zero bytes -- 26 total, matching
// every fill-rule record this module has seen (io/PsdVectorPath.hpp).
std::vector<uint8_t> otherRecord(uint16_t selector) {
  ByteWriter w;
  w.u16(selector);
  for (int i = 0; i < 12; ++i) w.u16(0);
  return w.b;
}

// A subpath-length record (selector 0 closed, 3 open).
std::vector<uint8_t> lengthRecord(uint16_t selector, uint16_t knotCount, int16_t op) {
  ByteWriter w;
  w.u16(selector);
  w.u16(knotCount);
  w.i16(op);
  w.u16(1);   // offset 6: varies in real files, keyed off nothing here
  w.u32(0);
  w.u32(0);   // origination index
  for (int i = 0; i < 5; ++i) w.u16(0);  // 10 bytes zero
  return w.b;
}

int32_t fixedFrac(double frac) {
  return static_cast<int32_t>(std::llround(frac * static_cast<double>(1u << 24)));
}

// A knot record built from ABSOLUTE document-space coordinates, converted
// to signed 8.24 fixed point internally -- the same scale the production
// decoder inverts.
std::vector<uint8_t> knotRecordAbs(uint16_t selector, double inX, double inY, double ptX, double ptY,
                                    double outX, double outY, double docW, double docH) {
  ByteWriter w;
  w.u16(selector);
  w.i32(fixedFrac(inY / docH));
  w.i32(fixedFrac(inX / docW));
  w.i32(fixedFrac(ptY / docH));
  w.i32(fixedFrac(ptX / docW));
  w.i32(fixedFrac(outY / docH));
  w.i32(fixedFrac(outX / docW));
  return w.b;
}

}  // namespace

bool runPsdVectorPathTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-64s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  constexpr float kTol = 5e-3f;  // 8.24 fixed point's own rounding

  std::printf(
      "  -- A. Five known-answer fixtures, real vsms/vmsk payloads from two "
      "Photoshop files --\n");
  // ==========================================================================
  {
    // ns:Ellipse 1, doc 768x512 -- proves BOTH open questions at once: it
    // sits entirely off the top of the canvas (negative y, so the field
    // must be read SIGNED) and the document is non-square (so x must divide
    // by width and y by height, not the same divisor for both). Getting
    // either wrong moves this shape to a different place, not merely a
    // slightly-off one -- docs/psd-vector-shapes.md's own worked check:
    // swapping the divisors gives x 328..444, y -283..-24, nowhere near
    // psd-tools' render bbox (491,-191,668,-14).
    const std::vector<uint8_t> bytes = hexToBytes(
        "000000030000000000060000000000000000000000000000000000000000000000000008"
        "000000000000000000000000000000000000000000000000000000040001000100000000"
        "00000000000000000000000000000001ffa1800000b13e12ffa1800000c12aabffa18000"
        "00d117430001ffb4dd1b00de0000ffccc00000de0000ffe4a2e500de00000001fff80000"
        "00d11743fff8000000c12aabfff8000000b13e120001ffe4a2e500a45555ffccc00000a4"
        "5555ffb4dd1b00a455550000");
    PsdPathStream out;
    std::string error;
    const bool decoded =
        decodePsdPathRecords(std::span<const uint8_t>(bytes.data(), bytes.size()), 768, 512, out, error);
    check(decoded, "A1: ns:Ellipse 1 (768x512) decodes");
    check(out.subpaths.size() == 1, "A1: exactly one subpath");
    if (out.subpaths.size() == 1) {
      const SubPath& s = out.subpaths[0].sub;
      check(s.closed, "A1: the subpath is closed");
      check(out.subpaths[0].op == PsdPathOp::Union && out.subpaths[0].opKnown,
            "A1: its path operation is the known value Union (raw 1)");
      check(s.anchors.size() == 4, "A1: it has exactly 4 anchors");
      if (s.anchors.size() == 4) {
        // Bounding box, both axes, against psd-tools' render bbox
        // (491,-191,668,-14) -- the 2 px gap is stroke+AA the decoder has
        // no part in.
        float minX = s.anchors[0].pt.x, maxX = minX, minY = s.anchors[0].pt.y, maxY = minY;
        for (const Anchor& a : s.anchors) {
          minX = std::min(minX, a.pt.x); maxX = std::max(maxX, a.pt.x);
          minY = std::min(minY, a.pt.y); maxY = std::max(maxY, a.pt.y);
        }
        check(nearf(minX, 493.0f, kTol) && nearf(maxX, 666.0f, kTol),
              "A1: x spans 493..666 -- horizontal divided by document WIDTH (768)");
        check(nearf(minY, -189.0f, kTol) && nearf(maxY, -16.0f, kTol),
              "A1: y spans -189..-16, SIGNED and above the canvas -- vertical divided by "
              "document HEIGHT (512)");
        check(s.anchors[0].smooth, "A1: knot 0 is selector 1 (linked) -> Anchor::smooth true");
      }
    }
    check(!out.sawOpenSubPath, "A1: no open subpath was seen");
    check(out.warnings.empty(), "A1: a well-formed block produces no warnings");
  }
  {
    // apple:PNG/1 - Layer.png, doc 1024x1024 -- a circle, centre (512,357)
    // r 256. Independent cross-check that does not reuse this decoder's own
    // arithmetic: a circle's cubic handle offset is r * 0.5523 (the
    // standard 4-cubic circle-approximation constant), so knot 0's `out.x`
    // minus its `pt.x` must be close to 256 * 0.5523 = 141.3849 -- a fact
    // about circles, not about this code.
    const std::vector<uint8_t> bytes = hexToBytes(
        "000000030000000000060000000000000000000000000000000000000000000000000008"
        "000000000000000000000000000000000000000000000000000000040001000100000000"
        "0000000000000000000000000000000100194000005ca75e001940000080000000194000"
        "00a358a200010035e75e00c000000059400000c00000007c98a200c00000000100994000"
        "00a358a2009940000080000000994000005ca75e0001007c98a200400000005940000040"
        "00000035e75e004000000000");
    PsdPathStream out;
    std::string error;
    const bool decoded = decodePsdPathRecords(std::span<const uint8_t>(bytes.data(), bytes.size()),
                                               1024, 1024, out, error);
    check(decoded, "A2: apple:PNG/1 (1024x1024) decodes");
    check(out.subpaths.size() == 1 && out.subpaths[0].sub.anchors.size() == 4,
          "A2: one subpath, 4 anchors");
    if (out.subpaths.size() == 1 && out.subpaths[0].sub.anchors.size() == 4) {
      const Anchor& k0 = out.subpaths[0].sub.anchors[0];
      check(nearf(k0.pt.x, 512.0f, kTol) && nearf(k0.pt.y, 101.0f, kTol),
            "A2: knot 0's anchor is (512,101), the top of the circle (357-256)");
      check(nearf(k0.out.x - k0.pt.x, 256.0f * 0.5523f, 0.05f),
            "A2: knot 0's handle offset is r*0.5523 (256*0.5523=141.3849) -- the circle "
            "constant, not this decoder's own arithmetic");
      check(k0.smooth, "A2: knot 0 is selector 1 (linked) -> Anchor::smooth true");
    }
  }
  {
    // apple:App Icon Shape, doc 1024x1024 -- 52 records, two subpaths with
    // different operations: op 1 (Union) on the exact document rectangle,
    // op 2 (Subtract) on a 44-knot squircle. Proves multi-subpath handling
    // and that each subpath keeps its OWN operation rather than sharing one.
    const std::vector<uint8_t> bytes = hexToBytes(
        "000000030000000000060000000000000000000000000000000000000000000000000008"
        "000000000000000000000000000000000000000000000000000000040001000100000000"
        "000000000000000000000000000000010000000000000000000000000000000000000000"
        "000000000001000000000100000000000000010000000000000001000000000101000000"
        "010000000100000001000000010000000100000000010100000000000000010000000000"
        "000001000000000000000000002c00020001000000000000000100000000000000000000"
        "000200a67fe400ffffa100a67fe400ffffa100a9eac300ffffa1000100ad557c01000000"
        "00b0c05b00fffa7900b3a13e00fff63c000100b681f700ffedbc00b962a000ffd9c600bf"
        "a7e700ffae43000100c5fb1c00ff4fc700cc2e8f00fe326b00d278f800fd10ee000100d8"
        "53c600fb388000de0af500f84fb900e3a90f00f573b4000100e8cd1700f1b73100ed4224"
        "00ed422400f1b73100e8cd17000100f573b400e3a90f00f84fb900de0af500fb388000d8"
        "53c6000100fd10ee00d278f800fe326b00cc2e8f00ff4fc700c5fb1c000100ffae4300bf"
        "a7e700ffd9c600b962a000ffedbc00b681f7000100fff63c00b3a13e00fffa7900b0c05b"
        "0100000000ad557c000100ffffa100a9eac300ffffa100a67fe400ffffa100a67fe40002"
        "00ffffa10059801c00ffffa10059801c00ffffa10056153d0001010000000052aa8400ff"
        "fa79004f3fa500fff63c004c5ec2000100ffedbc00497e0900ffd9c600469d5f00ffae43"
        "00405819000100ff4fc7003a04e400fe326b0033d17000fd10ee002d8708000100fb3880"
        "0027ac3a00f84fb90021f50b00f573b4001c56f0000100f1b731001732e900ed42240012"
        "bddc00e8cd17000e48cf000100e3a90f000a8c4c00de0af50007b04700d853c60004c780"
        "000100d278f80002ef1200cc2e8f0001cd9400c5fb1c0000b038000100bfa7e7000051bd"
        "00b962a00000263a00b681f700001243000100b3a13e000009c300b0c05b0000058700ad"
        "557c00000000000100a9eac30000005e00a67fe40000005e00a67fe40000005e00020059"
        "801c0000005e0059801c0000005e0056153d0000005e00010052aa8400000000004f3fa5"
        "00000587004c5ec2000009c3000100497e090000124300469d5f0000263a004058190000"
        "51bd0001003a04e40000b0380033d1700001cd94002d87080002ef1200010027ac3a0004"
        "c7800021f50b0007b047001c56f0000a8c4c0001001732e9000e48cf0012bddc0012bddc"
        "000e48cf001732e90001000a8c4c001c56f00007b0470021f50b0004c7800027ac3a0001"
        "0002ef12002d87080001cd940033d1700000b038003a04e40001000051bd004058190000"
        "263a00469d5f0000124300497e090001000009c3004c5ec200000587004f3fa500000000"
        "0052aa8400010000005e0056153d0000005e0059801c0000005e0059801c00020000005e"
        "00a67fe40000005e00a67fe40000005e00a9eac300010000000000ad557c0000058700b0"
        "c05b000009c300b3a13e00010000124300b681f70000263a00b962a0000051bd00bfa7e7"
        "00010000b03800c5fb1c0001cd9400cc2e8f0002ef1200d278f800010004c78000d853c6"
        "0007b04700de0af5000a8c4c00e3a90f0001000e48cf00e8cd170012bddc00ed42240017"
        "32e900f1b7310001001c56f000f573b40021f50b00f84fb90027ac3a00fb38800001002d"
        "870800fd10ee0033d17000fe326b003a04e400ff4fc700010040581900ffae4300469d5f"
        "00ffd9c600497e0900ffedbc0001004c5ec200fff63c004f3fa500fffa790052aa840100"
        "000000010056153d00ffffa10059801c00ffffa10059801c00ffffa1");
    PsdPathStream out;
    std::string error;
    const bool decoded = decodePsdPathRecords(std::span<const uint8_t>(bytes.data(), bytes.size()),
                                               1024, 1024, out, error);
    check(decoded, "A3: apple:App Icon Shape (1024x1024, 52 records) decodes");
    check(out.subpaths.size() == 2, "A3: two subpaths");
    if (out.subpaths.size() == 2) {
      const PsdSubPath& first = out.subpaths[0];
      const PsdSubPath& second = out.subpaths[1];
      check(first.sub.closed && first.sub.anchors.size() == 4,
            "A3: subpath 1 is closed with 4 anchors");
      check(first.opKnown && first.op == PsdPathOp::Union,
            "A3: subpath 1's operation is the known value Union (raw 1)");
      if (first.sub.anchors.size() == 4) {
        check(nearf(first.sub.anchors[0].pt.x, 0.0f, kTol) && nearf(first.sub.anchors[0].pt.y, 0.0f, kTol) &&
                  nearf(first.sub.anchors[1].pt.x, 1024.0f, kTol) && nearf(first.sub.anchors[1].pt.y, 0.0f, kTol) &&
                  nearf(first.sub.anchors[2].pt.x, 1024.0f, kTol) && nearf(first.sub.anchors[2].pt.y, 1024.0f, kTol) &&
                  nearf(first.sub.anchors[3].pt.x, 0.0f, kTol) && nearf(first.sub.anchors[3].pt.y, 1024.0f, kTol),
              "A3: subpath 1 is exactly the document rectangle (0,0)-(1024,1024)");
      }
      check(second.sub.closed && second.sub.anchors.size() == 44,
            "A3: subpath 2 is closed with 44 anchors (the squircle)");
      check(second.opKnown && second.op == PsdPathOp::Subtract,
            "A3: subpath 2's operation is the known value Subtract (raw 2)");
    }
    check(!out.sawOpenSubPath, "A3: no open subpath was seen");
    check(out.warnings.empty(), "A3: a well-formed 52-record block produces no warnings");
  }
  {
    // ns:Star 1, doc 768x512 -- 13 records (2 fill-rule + 1 length + 10
    // knots), all knots selector 2 (unlinked corner points, in == pt ==
    // out). Independent cross-check: the file's own `vogk` descriptor says,
    // in doubles, one corner is Hrzn=60.09375 Vrtc=-15.5351621...; this
    // decoder's extreme knot must land on the same point via completely
    // different arithmetic (int32 fixed point, not a descriptor double).
    const std::vector<uint8_t> bytes = hexToBytes(
        "000000030000000000060000000000000000000000000000000000000000000000000008"
        "0000000000000000000000000000000000000000000000000000000a0001000200000000"
        "00000000000000000000000000000002fff83b80005c2aabfff83b80005c2aabfff83b80"
        "005c2aab00020047065c006d1b780047065c006d1b780047065c006d1b7800020046d809"
        "00a44cb30046d80900a44cb30046d80900a44cb3000200775bfd007793c200775bfd0077"
        "93c200775bfd007793c2000200c60a370088bf4d00c60a370088bf4d00c60a370088bf4d"
        "000200953b4f005c2aab00953b4f005c2aab00953b4f005c2aab000200c60a37002f9608"
        "00c60a37002f960800c60a37002f9608000200775bfd0040c19300775bfd0040c1930077"
        "5bfd0040c19300020046d809001408a30046d809001408a30046d809001408a300020047"
        "065c004b39de0047065c004b39de0047065c004b39de0000");
    PsdPathStream out;
    std::string error;
    const bool decoded =
        decodePsdPathRecords(std::span<const uint8_t>(bytes.data(), bytes.size()), 768, 512, out, error);
    check(decoded, "A4: ns:Star 1 (768x512, 13 records, pad=2) decodes");
    check(out.subpaths.size() == 1 && out.subpaths[0].sub.anchors.size() == 10,
          "A4: one closed subpath with 10 anchors");
    if (out.subpaths.size() == 1 && out.subpaths[0].sub.anchors.size() == 10) {
      const SubPath& s = out.subpaths[0].sub;
      check(s.closed, "A4: the subpath is closed");
      bool allCorners = true;
      bool anySmooth = false;
      for (const Anchor& a : s.anchors) {
        if (!nearf(a.in.x, a.pt.x, kTol) || !nearf(a.in.y, a.pt.y, kTol) ||
            !nearf(a.out.x, a.pt.x, kTol) || !nearf(a.out.y, a.pt.y, kTol)) {
          allCorners = false;
        }
        if (a.smooth) anySmooth = true;
      }
      check(allCorners, "A4: every knot's in/pt/out coincide -- corner points, no curvature");
      check(!anySmooth, "A4: every knot is selector 2 (unlinked) -> Anchor::smooth false");
      // Two DIFFERENT knots (a 10-point star, not one corner): the
      // leftmost knot's x and the topmost knot's y, each matching vogk's
      // own doubles (Hrzn=60.09375, Vrtc=-15.5351621) to within
      // fixed-point rounding -- independent arithmetic, not this decoder's.
      bool sawLeftmostX = false;
      bool sawTopmostY = false;
      for (const Anchor& a : s.anchors) {
        if (nearf(a.pt.x, 60.0977f, 0.02f)) sawLeftmostX = true;
        if (nearf(a.pt.y, -15.5352f, 0.02f)) sawTopmostY = true;
      }
      check(sawLeftmostX,
            "A4: the leftmost knot's x is 60.0977, matching vogk's own Hrzn=60.09375 "
            "from independent arithmetic");
      check(sawTopmostY,
            "A4: the topmost knot's y is -15.5352, matching vogk's own Vrtc=-15.5351621 "
            "from independent arithmetic");
    }
  }
  {
    // ns:Shape 1, doc 768x512, 8 records (2 fill-rule + 1 length + 5 knots)
    // -- a mixed subpath: knot 0 has distinct in/pt/out (a real curve), the
    // rest are corner points. Rounds out A with a subpath that is not all
    // corners and not all curves.
    const std::vector<uint8_t> bytes = hexToBytes(
        "000000030000000000060000000000000000000000000000000000000000000000000008"
        "000000000000000000000000000000000000000000000000000000050001000100000000"
        "0000000000000000000000000000000100544700008b7155006e000000acaaab0087b900"
        "00cde400000100b8800000d3555500b8800000d3555500b8800000d35555000100d50000"
        "00b5555500d5000000b5555500d5000000b55555000100b98000008c555500b98000008c"
        "555500b98000008c55550001008b000000925555008b000000925555008b000000925555");
    PsdPathStream out;
    std::string error;
    const bool decoded =
        decodePsdPathRecords(std::span<const uint8_t>(bytes.data(), bytes.size()), 768, 512, out, error);
    check(decoded, "A5: ns:Shape 1 (768x512, 8 records) decodes");
    check(out.subpaths.size() == 1 && out.subpaths[0].sub.anchors.size() == 5,
          "A5: one closed subpath with 5 anchors");
    if (out.subpaths.size() == 1 && out.subpaths[0].sub.anchors.size() == 5) {
      const Anchor& k0 = out.subpaths[0].sub.anchors[0];
      check(nearf(k0.pt.x, 518.0f, kTol) && nearf(k0.pt.y, 220.0f, kTol),
            "A5: knot 0's anchor is (518,220)");
      check(!nearf(k0.in.x, k0.pt.x, kTol) || !nearf(k0.in.y, k0.pt.y, kTol),
            "A5: knot 0's handles are NOT collinear with its anchor -- a real curve, "
            "unlike Star 1's corners");
    }
  }

  std::printf(
      "  -- B. Framing this module must survive without reading a byte outside "
      "the block --\n");
  // ==========================================================================
  {
    // B1: a block shorter than its own 8-byte header. Nothing to decode --
    // this is the one case that returns false with a named error, per
    // io/PsdVectorPath.hpp's contract.
    const std::vector<uint8_t> bytes = {1, 2, 3, 4, 5};
    PsdPathStream out;
    std::string error;
    const bool decoded =
        decodePsdPathRecords(std::span<const uint8_t>(bytes.data(), bytes.size()), 100, 100, out, error);
    check(!decoded, "B1: a 5-byte block (shorter than the 8-byte header) refuses");
    check(!error.empty(), "B1: the refusal names an error");
  }
  {
    // B2: the pad-remainder rule. 8 + 7*26 + 2 = 192 bytes, the exact
    // number docs/psd-vector-shapes.md derives -- 7 records, the trailing 2
    // bytes are pad, NOT a corrupt eighth record.
    std::vector<uint8_t> bytes = pathBlockHeader();
    for (int i = 0; i < 7; ++i) {
      const std::vector<uint8_t> rec = otherRecord(6);
      bytes.insert(bytes.end(), rec.begin(), rec.end());
    }
    bytes.push_back(0);
    bytes.push_back(0);  // 2 bytes of pad
    check(bytes.size() == 192, "B2: the built fixture is exactly 192 bytes, matching the doc");
    PsdPathStream out;
    std::string error;
    const bool decoded =
        decodePsdPathRecords(std::span<const uint8_t>(bytes.data(), bytes.size()), 100, 100, out, error);
    check(decoded, "B2: a 192-byte block (7 records + 2 pad bytes) decodes");
    check(out.subpaths.empty() && out.warnings.empty(),
          "B2: 7 type-6 records produce no subpaths and no warnings; the pad bytes are never "
          "read as an 8th record");
  }
  {
    // B3: a truncated block -- one full knot record's worth of bytes
    // is simply missing off the end, leaving a remainder the module must
    // discard rather than read past. The one knot record that IS present
    // must still decode correctly.
    std::vector<uint8_t> bytes = pathBlockHeader();
    const std::vector<uint8_t> len = lengthRecord(0, 1, 1);
    const std::vector<uint8_t> knot = knotRecordAbs(1, 10, 10, 20, 30, 40, 40, 100, 100);
    bytes.insert(bytes.end(), len.begin(), len.end());
    bytes.insert(bytes.end(), knot.begin(), knot.end());
    bytes.push_back(0xAA);
    bytes.push_back(0xBB);
    bytes.push_back(0xCC);  // 3 stray bytes: not a full record, must be discarded
    PsdPathStream out;
    std::string error;
    const bool decoded =
        decodePsdPathRecords(std::span<const uint8_t>(bytes.data(), bytes.size()), 100, 100, out, error);
    check(decoded, "B3: a block truncated mid-record (3 stray trailing bytes) still decodes");
    check(out.subpaths.size() == 1 && out.subpaths[0].sub.anchors.size() == 1,
          "B3: the one complete knot record present was decoded; the stray bytes were not "
          "read as a record");
    if (out.subpaths.size() == 1 && out.subpaths[0].sub.anchors.size() == 1) {
      check(nearf(out.subpaths[0].sub.anchors[0].pt.x, 20.0f, kTol) &&
                nearf(out.subpaths[0].sub.anchors[0].pt.y, 30.0f, kTol),
            "B3: that knot's anchor is exactly the (20,30) this fixture wrote");
    }
  }
  {
    // B4: a garbage selector -- a value none of {0..8} names. Skipped, and
    // named in a warning rather than silently dropped or treated as
    // framing failure.
    std::vector<uint8_t> bytes = pathBlockHeader();
    const std::vector<uint8_t> rec = otherRecord(42);
    bytes.insert(bytes.end(), rec.begin(), rec.end());
    PsdPathStream out;
    std::string error;
    const bool decoded =
        decodePsdPathRecords(std::span<const uint8_t>(bytes.data(), bytes.size()), 100, 100, out, error);
    check(decoded, "B4: a block with one unrecognised selector (42) still decodes (ok)");
    check(out.subpaths.empty(), "B4: it produced no subpath");
    check(anyContains(out.warnings, "42"), "B4: a warning names the unrecognised selector 42");
  }
  {
    // B5: an unknown path-operation int16 (99, not one of {0,1,2,3,-1}).
    // The geometry is still kept; only the operation is flagged unknown.
    std::vector<uint8_t> bytes = pathBlockHeader();
    const std::vector<uint8_t> len = lengthRecord(0, 1, 99);
    const std::vector<uint8_t> knot = knotRecordAbs(1, 5, 5, 5, 5, 5, 5, 100, 100);
    bytes.insert(bytes.end(), len.begin(), len.end());
    bytes.insert(bytes.end(), knot.begin(), knot.end());
    PsdPathStream out;
    std::string error;
    const bool decoded =
        decodePsdPathRecords(std::span<const uint8_t>(bytes.data(), bytes.size()), 100, 100, out, error);
    check(decoded, "B5: a subpath whose operation int16 is 99 (unknown) still decodes (ok)");
    check(out.subpaths.size() == 1 && out.subpaths[0].sub.anchors.size() == 1,
          "B5: its one knot's geometry was still decoded");
    check(!out.subpaths.empty() && !out.subpaths[0].opKnown && out.subpaths[0].rawOp == 99,
          "B5: opKnown is false and rawOp records the actual value 99");
    check(anyContains(out.warnings, "99"), "B5: a warning names the unrecognised operation 99");
  }
  {
    // B6: an open subpath (selector 3, knots 4/5). sawOpenSubPath must be
    // set, closed must be false, and linked/unlinked still maps to smooth.
    std::vector<uint8_t> bytes = pathBlockHeader();
    const std::vector<uint8_t> len = lengthRecord(3, 2, 1);
    const std::vector<uint8_t> k0 = knotRecordAbs(4, 1, 1, 2, 2, 3, 3, 100, 100);   // linked
    const std::vector<uint8_t> k1 = knotRecordAbs(5, 4, 4, 5, 5, 6, 6, 100, 100);   // unlinked
    bytes.insert(bytes.end(), len.begin(), len.end());
    bytes.insert(bytes.end(), k0.begin(), k0.end());
    bytes.insert(bytes.end(), k1.begin(), k1.end());
    PsdPathStream out;
    std::string error;
    const bool decoded =
        decodePsdPathRecords(std::span<const uint8_t>(bytes.data(), bytes.size()), 100, 100, out, error);
    check(decoded, "B6: an open subpath (selector 3) decodes");
    check(out.sawOpenSubPath, "B6: sawOpenSubPath is set");
    check(out.subpaths.size() == 1 && !out.subpaths[0].sub.closed && out.subpaths[0].sub.anchors.size() == 2,
          "B6: one open subpath (closed=false) with 2 anchors");
    if (out.subpaths.size() == 1 && out.subpaths[0].sub.anchors.size() == 2) {
      check(out.subpaths[0].sub.anchors[0].smooth, "B6: knot 0 (selector 4, linked) is smooth");
      check(!out.subpaths[0].sub.anchors[1].smooth, "B6: knot 1 (selector 5, unlinked) is not smooth");
    }
  }
  {
    // B7: a zero-knot subpath -- a length record declaring 0 knots,
    // nothing following it. Decodes to one subpath with no anchors, not a
    // refusal and not a dropped subpath.
    std::vector<uint8_t> bytes = pathBlockHeader();
    const std::vector<uint8_t> len = lengthRecord(0, 0, 1);
    bytes.insert(bytes.end(), len.begin(), len.end());
    PsdPathStream out;
    std::string error;
    const bool decoded =
        decodePsdPathRecords(std::span<const uint8_t>(bytes.data(), bytes.size()), 100, 100, out, error);
    check(decoded, "B7: a subpath declaring 0 knots decodes");
    check(out.subpaths.size() == 1 && out.subpaths[0].sub.anchors.empty(),
          "B7: it produced one subpath with zero anchors");
    check(out.warnings.empty(), "B7: 0 declared and 0 present is not a mismatch, so no warning");
  }
  {
    // B8: a subpath's declared knot count (9000) vastly outruns the
    // records actually present (2, then the block ends). The module must
    // never allocate from the claimed count -- only from what is there --
    // and must decode exactly the 2 real knots without reading past them.
    std::vector<uint8_t> bytes = pathBlockHeader();
    const std::vector<uint8_t> len = lengthRecord(0, 9000, 1);
    const std::vector<uint8_t> k0 = knotRecordAbs(2, 1, 1, 1, 1, 1, 1, 100, 100);
    const std::vector<uint8_t> k1 = knotRecordAbs(2, 2, 2, 2, 2, 2, 2, 100, 100);
    bytes.insert(bytes.end(), len.begin(), len.end());
    bytes.insert(bytes.end(), k0.begin(), k0.end());
    bytes.insert(bytes.end(), k1.begin(), k1.end());
    PsdPathStream out;
    std::string error;
    const bool decoded =
        decodePsdPathRecords(std::span<const uint8_t>(bytes.data(), bytes.size()), 100, 100, out, error);
    check(decoded, "B8: a subpath claiming 9000 knots with only 2 present decodes (ok), "
                   "never reading out of bounds");
    check(out.subpaths.size() == 1 && out.subpaths[0].sub.anchors.size() == 2,
          "B8: exactly the 2 knot records actually present were decoded -- not 9000, "
          "not 0");
    check(anyContains(out.warnings, "9000") && anyContains(out.warnings, "2"),
          "B8: a warning names both the declared count 9000 and the 2 actually present");
  }

  std::printf("[selftest] psd vector path %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
