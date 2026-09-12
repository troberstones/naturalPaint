#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/Path.hpp"
#include "core/PathRaster.hpp"
#include "io/PsdVectorPath.hpp"
#include "io/PsdVectorStyle.hpp"

// The three PSD vector modules working TOGETHER on one real layer.
//
// io/PsdVectorPath, io/PsdVectorCompose and io/PsdVectorStyle were each built
// against a frozen header by a separate track, and each has its own section
// proving its own function. None of those sections can see a seam: a decoder
// whose subpath winding disagrees with what the composer expects, or a style
// reader keyed to a layer the geometry came from a different block of, fails
// only when the three are run in order on the same bytes.
//
// The layer is `Star 1` from `testNonSquareWithShapesOffPage.psd` -- ten
// corner knots, non-square canvas, and crossing the top edge, so the two
// coordinate facts the format hid until that file existed are both live here.
//
// **The expected values are psd-tools', not ours.** The probe points below
// were read off psd-tools' own render of this layer, placed into the canvas
// and clipped; the coverage total is that render's opaque pixel count. So a
// bug shared between our decoder and our composer cannot make this pass --
// which is the one thing a round trip through our own code could never say.
namespace np {
namespace {

std::vector<uint8_t> hexToBytes(const char* hex) {
  auto val = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
  };
  std::vector<uint8_t> out;
  const std::string s(hex);
  out.reserve(s.size() / 2);
  for (size_t i = 0; i + 1 < s.size(); i += 2)
    out.push_back(static_cast<uint8_t>((val(s[i]) << 4) | val(s[i + 1])));
  return out;
}

std::vector<float> rasterizeToImage(const Path& path, int32_t w, int32_t h) {
  std::vector<float> img(static_cast<size_t>(w) * static_cast<size_t>(h), 0.0f);
  PathRasterScratch scratch;
  const RasterClip clip{0, 0, w, h};
  rasterizePath(path, 0.05f, clip, scratch,
                [&](int32_t y, int32_t x0, int32_t x1, const float* cov) {
                  for (int32_t x = x0; x < x1; ++x)
                    img[static_cast<size_t>(y) * static_cast<size_t>(w) +
                        static_cast<size_t>(x)] = cov[x - x0];
                });
  return img;
}

// --- Real bytes: `Star 1`'s own tagged blocks ----------------------------
const char* const kStarVsmsHex =
    "000000030000000000060000000000000000000000000000000000000000000000000008"
    "0000000000000000000000000000000000000000000000000000000a0001000200000000"
    "00000000000000000000000000000002fff83b80005c2aabfff83b80005c2aabfff83b80"
    "005c2aab00020047065c006d1b780047065c006d1b780047065c006d1b7800020046d809"
    "00a44cb30046d80900a44cb30046d80900a44cb3000200775bfd007793c200775bfd0077"
    "93c200775bfd007793c2000200c60a370088bf4d00c60a370088bf4d00c60a370088bf4d"
    "000200953b4f005c2aab00953b4f005c2aab00953b4f005c2aab000200c60a37002f9608"
    "00c60a37002f960800c60a37002f9608000200775bfd0040c19300775bfd0040c1930077"
    "5bfd0040c19300020046d809001408a30046d809001408a30046d809001408a300020047"
    "065c004b39de0047065c004b39de0047065c004b39de0000";

const char* const kStarVscgHex =
    "536f436f00000010000000010000000000006e756c6c0000000100000000436c72204f62"
    "6a630000000100000000000052474243000000030000000052642020646f7562402ce51a"
    "e30000000000000047726e20646f75623fe2cd32cd20000000000000426c2020646f7562"
    "405a221de5c00000";

const char* const kStarVstkHex =
    "000000100000000100000000000b7374726f6b655374796c650000001000000012737472"
    "6f6b655374796c6556657273696f6e6c6f6e67000000020000000d7374726f6b65456e61"
    "626c6564626f6f6c010000000b66696c6c456e61626c6564626f6f6c0100000014737472"
    "6f6b655374796c654c696e655769647468556e74462350786c3ff0000000000000000000"
    "197374726f6b655374796c654c696e65446173684f6666736574556e744623506e740000"
    "000000000000000000157374726f6b655374796c654d697465724c696d6974646f756240"
    "59000000000000000000167374726f6b655374796c654c696e6543617054797065656e75"
    "6d000000167374726f6b655374796c654c696e6543617054797065000000127374726f6b"
    "655374796c6542757474436170000000177374726f6b655374796c654c696e654a6f696e"
    "54797065656e756d000000177374726f6b655374796c654c696e654a6f696e5479706500"
    "0000147374726f6b655374796c654d697465724a6f696e000000187374726f6b65537479"
    "6c654c696e65416c69676e6d656e74656e756d000000187374726f6b655374796c654c69"
    "6e65416c69676e6d656e74000000167374726f6b655374796c65416c69676e43656e7465"
    "72000000147374726f6b655374796c655363616c654c6f636b626f6f6c00000000177374"
    "726f6b655374796c655374726f6b6541646a757374626f6f6c00000000167374726f6b65"
    "5374796c654c696e6544617368536574566c4c7300000000000000147374726f6b655374"
    "796c65426c656e644d6f6465656e756d00000000426c6e4d000000004e726d6c00000012"
    "7374726f6b655374796c654f706163697479556e74462350726340590000000000000000"
    "00127374726f6b655374796c65436f6e74656e744f626a630000000100000000000f736f"
    "6c6964436f6c6f724c617965720000000100000000436c72204f626a6300000001000000"
    "00000052474243000000030000000052642020646f756200000000000000000000000047"
    "726e20646f7562000000000000000000000000426c2020646f7562000000000000000000"
    "0000157374726f6b655374796c655265736f6c7574696f6e646f75624062000000000000";

// Canvas of the file these came from.
constexpr int32_t kDocW = 768;
constexpr int32_t kDocH = 512;

// Read off psd-tools' render of this layer, clipped into the canvas. Chosen
// well away from any edge so antialiasing cannot decide the answer.
struct Probe {
  int32_t x, y;
  bool inside;
};
constexpr Probe kProbes[] = {
    {276, 120, true},  {276, 250, true},  {276, 60, true},   {180, 150, true},
    {380, 150, true},  {276, 10, true},   {150, 380, true},  {400, 380, true},
    {100, 120, false}, {460, 120, false}, {276, 380, false}, {60, 60, false},
    {490, 300, false},
};

// psd-tools' opaque pixel count for this layer inside the canvas. Its render
// includes the layer's 1 px centred stroke, which our fill-only rasterisation
// does not, so the comparison carries a tolerance rather than pretending to
// be exact -- the stroke's outer half-pixel is worth roughly the perimeter.
constexpr int kOracleOpaque = 57726;

}  // namespace

bool runPsdVectorChainTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const std::vector<uint8_t> vsms = hexToBytes(kStarVsmsHex);
  const std::vector<uint8_t> vscg = hexToBytes(kStarVscgHex);
  const std::vector<uint8_t> vstk = hexToBytes(kStarVstkHex);

  // Step 1.
  PsdPathStream stream;
  std::string error;
  const bool decoded = decodePsdPathRecords(vsms, kDocW, kDocH, stream, error);
  check(decoded && error.empty(), "chain: step 1 decodes Star 1's vsms block");
  if (!decoded) {
    std::printf("    (error was: %s)\n", error.c_str());
    std::printf("[selftest] psd vector chain FAIL\n");
    return false;
  }
  check(stream.subpaths.size() == 1 && stream.subpaths[0].sub.anchors.size() == 10,
        "chain: one subpath of ten knots, as the file's own record says");

  // Step 2.
  const PsdComposedPath composed = composePsdSubPaths(stream);
  check(composed.ok && composed.refusal.empty(),
        "chain: step 2 composes it -- a single all-Union subpath is expressible");
  check(pathIsFinite(composed.path), "chain: and the composed path is finite");

  // Step 3.
  PsdVectorStyleBlocks blocks;
  blocks.vscg = vscg;
  blocks.vstk = vstk;
  PsdVectorStyle style;
  std::string styleError;
  const bool styled = decodePsdVectorStyle(blocks, style, styleError);
  check(styled && styleError.empty(), "chain: step 3 reads the same layer's vscg and vstk");
  check(style.fill.on, "chain: the shape is filled (vscg carries the colour; there is no SoCo)");
  check(style.stroke.on && std::fabs(style.strokeStyle.width - 1.0f) < 1e-5f,
        "chain: and stroked at 1 px -- this file's shapes have strokeEnabled true, "
        "unlike Apple's");

  // The three together, against psd-tools' own render.
  const std::vector<float> img = rasterizeToImage(composed.path, kDocW, kDocH);
  int wrong = 0;
  for (const Probe& p : kProbes) {
    const float cov = img[static_cast<size_t>(p.y) * static_cast<size_t>(kDocW) +
                          static_cast<size_t>(p.x)];
    if ((cov > 0.9f) != p.inside) ++wrong;
  }
  check(wrong == 0,
        "chain: all 13 probe points agree with psd-tools' render -- inside and outside both");

  int covered = 0;
  for (const float c : img)
    if (c > 0.5f) ++covered;
  const double drift = std::fabs(covered - kOracleOpaque) / static_cast<double>(kOracleOpaque);
  std::printf("    [measured] fill coverage %d px against psd-tools' %d (%.2f%% apart; its "
              "render includes the 1 px stroke ours does not)\n",
              covered, kOracleOpaque, drift * 100.0);
  check(drift < 0.02,
        "chain: and the covered area matches that render to within 2% -- geometry, winding "
        "and scale all correct at once");

  std::printf("[selftest] psd vector chain %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
