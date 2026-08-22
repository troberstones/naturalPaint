// tools/GoldenTool -- the comparison half of the golden-image harness.
//
// app/Screenshot makes the app photograph its own swapchain (see that
// header's comment for why: every macOS route to another process's window
// pixels is gated behind a permission that fails silently). What was
// missing was anything that *diffed* the result. This is that: a small,
// separate CLI tool, built from one translation unit, with no dependency on
// the app's GPU/ImGui/SDL machinery -- it only ever touches PNG bytes
// already sitting on disk, so it links against stb_image/stb_image_write
// alone and stays fast to build and trivial to reason about.
//
// Two subcommands:
//
//   goldentool crop <in.png> <out.png> <x> <y> <w> <h>
//     Crops a full-window --screenshot capture down to one tightly-scoped
//     region (a toolbar strip, a layer row, a canvas patch). Reference
//     images are kept small by cropping BEFORE anything is committed, not
//     by shrinking a full-window PNG after the fact.
//
//   goldentool diff <actual.png> <reference.png> <diff-out.png> <threshold>
//                   [max-changed-px]
//     Channel-aware comparison: for every pixel, the reported difference is
//     max(|dR|, |dG|, |dB|) -- not a raw byte/memcmp, and not blended across
//     channels the way a summed or averaged diff would let a red shift and
//     a blue shift cancel out. Alpha is read but not scored: app/Screenshot
//     forces alpha to 255 on every capture (the surface is opaque), so an
//     alpha channel carries no information here and scoring it would just
//     add a constant to every pixel's distance.
//
//     threshold is an integer 0-255, the maximum per-pixel channel-aware
//     difference allowed before the view is considered changed. It is a
//     command-line argument rather than a constant in this file because the
//     harness derives it empirically per run (see tools/golden/run_golden.sh)
//     from a measured run-to-run noise floor -- this tool has no opinion on
//     what that floor is, only on how to apply it.
//
//     max-changed-px (optional, default 0) is the second, independent
//     criterion: how many pixels may differ AT ALL, whatever their
//     magnitude. Both must hold to pass. A magnitude threshold alone is
//     blind to a diffuse shift -- see the comment at the pass check in
//     runDiff() for the measurement that proved it, in which 76% of a crop
//     changed and the view still passed.
//
//     On a dimension mismatch the tool refuses by name (exit 2) rather than
//     guessing how to align differently-sized images -- the same "refuse
//     rather than guess" stance app/Screenshot takes on an unrecognised
//     surface format.
//
//     On a mismatch (exit 1), it writes <diff-out.png>: a black canvas with
//     each differing pixel painted red at an intensity proportional to its
//     score (amplified x8 and clamped, so a small but real difference is
//     still visible rather than reading as near-black). It does not
//     overwrite the reference or the actual capture -- the caller is
//     expected to have already placed the actual capture on disk (crop's
//     <out.png>) and passes that same path in as <actual.png>, so nothing
//     needs copying here.
//
// Exit codes: 0 pass, 1 mismatch (diff written), 2 usage/IO/dimension error.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

int usage(const char* argv0) {
  std::fprintf(stderr,
               "usage:\n"
               "  %s crop <in.png> <out.png> <x> <y> <w> <h>\n"
               "  %s diff <actual.png> <reference.png> <diff-out.png> <threshold> "
               "[max-changed-px]\n",
               argv0, argv0);
  return 2;
}

// Loads `path` as 8-bit RGBA, refusing (rather than guessing a channel
// count) on any format stb_image cannot decode. Returns false and leaves
// *w/*h unspecified on failure.
bool loadRgba(const char* path, std::vector<unsigned char>& out, int* w, int* h) {
  int channelsInFile = 0;
  unsigned char* px = stbi_load(path, w, h, &channelsInFile, 4);
  if (px == nullptr) {
    std::fprintf(stderr, "goldentool: could not read %s: %s\n", path, stbi_failure_reason());
    return false;
  }
  out.assign(px, px + static_cast<size_t>(*w) * static_cast<size_t>(*h) * 4);
  stbi_image_free(px);
  return true;
}

int runCrop(int argc, char** argv) {
  if (argc != 8) return usage(argv[0]);
  const char* inPath = argv[2];
  const char* outPath = argv[3];
  const int x = std::atoi(argv[4]);
  const int y = std::atoi(argv[5]);
  const int w = std::atoi(argv[6]);
  const int h = std::atoi(argv[7]);
  if (w <= 0 || h <= 0 || x < 0 || y < 0) {
    std::fprintf(stderr, "goldentool crop: x/y/w/h must be non-negative and w/h positive\n");
    return 2;
  }

  std::vector<unsigned char> src;
  int sw = 0, sh = 0;
  if (!loadRgba(inPath, src, &sw, &sh)) return 2;

  if (x + w > sw || y + h > sh) {
    std::fprintf(stderr,
                 "goldentool crop: requested region (%d,%d,%d,%d) does not fit inside %s "
                 "(%dx%d) -- refusing rather than clamping silently\n",
                 x, y, w, h, inPath, sw, sh);
    return 2;
  }

  std::vector<unsigned char> dst(static_cast<size_t>(w) * h * 4);
  for (int row = 0; row < h; ++row) {
    const unsigned char* srcRow = src.data() + (static_cast<size_t>(y + row) * sw + x) * 4;
    unsigned char* dstRow = dst.data() + static_cast<size_t>(row) * w * 4;
    std::memcpy(dstRow, srcRow, static_cast<size_t>(w) * 4);
  }

  if (stbi_write_png(outPath, w, h, 4, dst.data(), w * 4) == 0) {
    std::fprintf(stderr, "goldentool crop: could not write %s\n", outPath);
    return 2;
  }
  std::printf("goldentool crop: %s (%dx%d) -> %s (%dx%d at +%d+%d)\n", inPath, sw, sh, outPath, w,
              h, x, y);
  return 0;
}

int runDiff(int argc, char** argv) {
  if (argc != 6 && argc != 7) return usage(argv[0]);
  const char* actualPath = argv[2];
  const char* refPath = argv[3];
  const char* diffOutPath = argv[4];
  const int threshold = std::atoi(argv[5]);
  // Absent, this is 0: any changed pixel at all fails. That is the strict
  // reading, and it is the right default for a view whose magnitude
  // threshold is also 0 -- there, the two criteria agree and the argument
  // changes nothing. It only matters for a view that had to accept a
  // non-zero magnitude floor.
  const size_t maxChangedPx = argc == 7 ? static_cast<size_t>(std::atol(argv[6])) : 0;

  std::vector<unsigned char> a, b;
  int aw = 0, ah = 0, bw = 0, bh = 0;
  if (!loadRgba(actualPath, a, &aw, &ah)) return 2;
  if (!loadRgba(refPath, b, &bw, &bh)) return 2;

  if (aw != bw || ah != bh) {
    std::fprintf(stderr,
                 "goldentool diff: dimension mismatch -- %s is %dx%d, %s is %dx%d. Refusing to "
                 "guess an alignment; a size change is itself a regression.\n",
                 actualPath, aw, ah, refPath, bw, bh);
    return 2;
  }

  const size_t pixelCount = static_cast<size_t>(aw) * ah;
  size_t mismatched = 0;
  int maxDiff = 0;
  double sumDiff = 0.0;
  std::vector<unsigned char> diffImg;  // built lazily, only if something differs

  for (size_t i = 0; i < pixelCount; ++i) {
    const unsigned char* pa = &a[i * 4];
    const unsigned char* pb = &b[i * 4];
    const int dr = std::abs(static_cast<int>(pa[0]) - static_cast<int>(pb[0]));
    const int dg = std::abs(static_cast<int>(pa[1]) - static_cast<int>(pb[1]));
    const int db = std::abs(static_cast<int>(pa[2]) - static_cast<int>(pb[2]));
    // Channel-aware: the worst single channel, not a sum/average across
    // three that would let independent shifts cancel or blend.
    const int d = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
    sumDiff += d;
    if (d > maxDiff) maxDiff = d;
    if (d > 0) ++mismatched;
  }

  const double meanDiff = pixelCount > 0 ? sumDiff / static_cast<double>(pixelCount) : 0.0;
  const double mismatchPct =
      pixelCount > 0 ? 100.0 * static_cast<double>(mismatched) / static_cast<double>(pixelCount)
                     : 0.0;
  std::printf(
      "goldentool diff: %dx%d, %zu px, %zu mismatched (%.4f%%), max channel diff %d, mean %.4f "
      "(thresholds: magnitude %d, changed px %zu)\n",
      aw, ah, pixelCount, mismatched, mismatchPct, maxDiff, meanDiff, threshold, maxChangedPx);

  // Two independent criteria, because run-to-run noise and a real regression
  // differ along two different axes and a single magnitude threshold cannot
  // separate them.
  //
  // The measured noise here (run_golden.sh's header records the trials) is
  // A FEW pixels at a MODERATE magnitude: at worst 14 px of 121 600, none
  // above channel diff 25, all of it an ImGui hover-tint lerp that has not
  // quite settled. Tolerating that costs a magnitude floor.
  //
  // But a magnitude floor alone is blind in the other direction, and not
  // hypothetically -- measured, on this harness, before this criterion
  // existed: lightening `kChromeBase` by 40 per channel changed **76% of
  // the `layers` crop (92 516 of 121 600 px)** and the view still reported
  // PASS, because 40 <= its magnitude threshold of 96. Every pixel of a
  // panel background moved and the guard said nothing.
  //
  // So: noise is few-and-moderate, and a regression is either MANY pixels
  // or ANY pixel moved FAR. Fail on either axis independently. A view whose
  // magnitude floor is 0 gets a changed-px budget of 0 too, and the second
  // criterion is then a no-op -- it exists for the views that had to accept
  // a floor, which are exactly the views the floor could hide something in.
  if (maxDiff <= threshold && mismatched <= maxChangedPx) {
    std::printf("goldentool diff: PASS\n");
    return 0;
  }
  if (maxDiff <= threshold) {
    std::printf(
        "goldentool diff: every pixel is within the magnitude threshold (%d), but %zu px changed "
        "against a budget of %zu -- a diffuse shift, not run-to-run noise.\n",
        threshold, mismatched, maxChangedPx);
  }

  // A handful of mismatched pixels is small enough to name individually --
  // useful when tracking down exactly what moved, rather than only knowing
  // that something did.
  if (mismatched <= 20) {
    for (size_t i = 0; i < pixelCount; ++i) {
      const unsigned char* pa = &a[i * 4];
      const unsigned char* pb = &b[i * 4];
      const int dr = std::abs(static_cast<int>(pa[0]) - static_cast<int>(pb[0]));
      const int dg = std::abs(static_cast<int>(pa[1]) - static_cast<int>(pb[1]));
      const int db = std::abs(static_cast<int>(pa[2]) - static_cast<int>(pb[2]));
      const int d = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
      if (d <= 0) continue;
      const int px = static_cast<int>(i % static_cast<size_t>(aw));
      const int py = static_cast<int>(i / static_cast<size_t>(aw));
      std::printf("  (%d,%d): actual rgb(%d,%d,%d) vs reference rgb(%d,%d,%d), diff %d\n", px, py,
                  pa[0], pa[1], pa[2], pb[0], pb[1], pb[2], d);
    }
  }

  // Diagnosable failure (house requirement): write a difference
  // visualisation next to the reference. Black background, differing
  // pixels painted red at an amplified (x8, clamped) intensity so a small
  // real difference does not read as indistinguishable from noise.
  diffImg.assign(pixelCount * 4, 0);
  for (size_t i = 0; i < pixelCount; ++i) {
    const unsigned char* pa = &a[i * 4];
    const unsigned char* pb = &b[i * 4];
    const int dr = std::abs(static_cast<int>(pa[0]) - static_cast<int>(pb[0]));
    const int dg = std::abs(static_cast<int>(pa[1]) - static_cast<int>(pb[1]));
    const int db = std::abs(static_cast<int>(pa[2]) - static_cast<int>(pb[2]));
    const int d = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
    if (d <= 0) continue;
    int amplified = d * 8;
    if (amplified > 255) amplified = 255;
    diffImg[i * 4 + 0] = static_cast<unsigned char>(amplified);
    diffImg[i * 4 + 3] = 255;
  }
  if (stbi_write_png(diffOutPath, aw, ah, 4, diffImg.data(), aw * 4) == 0) {
    std::fprintf(stderr, "goldentool diff: could not write %s\n", diffOutPath);
    return 2;
  }
  std::printf("goldentool diff: FAIL -- actual %s, reference %s, diff visualisation %s\n",
              actualPath, refPath, diffOutPath);
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage(argv[0]);
  const std::string cmd = argv[1];
  if (cmd == "crop") return runCrop(argc, argv);
  if (cmd == "diff") return runDiff(argc, argv);
  return usage(argv[0]);
}
