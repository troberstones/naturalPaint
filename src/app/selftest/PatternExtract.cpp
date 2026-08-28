#include "app/selftest/Support.hpp"

#include <filesystem>

#include "app/DabLibrary.hpp"
#include "stb_image.h"

namespace np {

// ---------------------------------------------------------------------------
// app/DabLibrary's pattern extraction -- `extractAbrPatterns()` and
// `patternsImportedRootPath()`, the `patt`-block sibling of section G of
// app/selftest/DabLibrary.cpp's own `.abr` tip extraction.
//
// **The claim, restated from extractAbrPatterns()'s own header comment**: a
// `.abr`'s scanned paper used to live only inside `patternsById`, a map built
// fresh inside `importAbrBrushes()` and thrown away the moment that call
// returned -- feeding `BrushPreset::grain` for THAT import's own presets and
// nothing that survived a relaunch. This is what proves the fix: a decoded
// `PaperField` written to `patterns-imported/<uuid>.png` round-trips byte for
// byte, a second import of the same uuid leaves the file alone, and a
// malformed id is refused rather than trusted as a path component.
//
// **Single-channel, not alpha-over-black.** Section G's tip round-trip goes
// through `DabLibrary::resolve()` because a tip's mask has to survive §4's
// "real alpha, else `1 - luminance`" coverage rule; a pattern's height field
// answers to no such rule, so this reads the PNG back directly with
// `stbi_load(..., 1)` (the same "read the raw bytes back" move
// app/selftest/PresentTransfer.cpp already makes) rather than through
// DabLibrary at all -- a pattern is not a dab, and neither is this test.
//
// A synthetic fixture throughout, not a real `.abr` -- this build's own
// discipline (io/PsPatterns.hpp's header) keeps parsing correctness proven
// against real packs measured OUTSIDE `--selftest`, and this section only
// needs a `PaperField` with known bytes in it.
//
// Runs entirely against a scratch directory under the system temp path; the
// imported root is passed explicitly to `extractAbrPatterns()`; nothing here
// touches `~/Library/Application Support/naturalPaint`. Headless and
// GPU-free.
// ---------------------------------------------------------------------------
bool runPatternExtractTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  namespace fs = std::filesystem;
  const fs::path scratch = fs::temp_directory_path() / "np-selftest-patternextract";
  std::error_code ec;
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  const std::string importedRoot = (scratch / "patterns-imported").string();

  // A small synthetic PaperField -- not opaque everywhere and not zero
  // everywhere, so a byte-for-byte comparison actually exercises every value
  // rather than passing by coincidence on a flat field.
  const std::string uuid = "9f1c2e4a-0000-4000-8000-abc123def456";
  PaperField field;
  field.width = 5;
  field.height = 3;
  field.height8.resize(15);
  for (size_t i = 0; i < field.height8.size(); ++i)
    field.height8[i] = static_cast<uint8_t>(i * 17);

  auto readGrayPng = [&](const fs::path& p, int& w, int& h) -> std::vector<uint8_t> {
    int comp = 0;
    unsigned char* px = stbi_load(p.string().c_str(), &w, &h, &comp, 1);
    if (px == nullptr) return {};
    std::vector<uint8_t> out(px, px + static_cast<size_t>(w) * static_cast<size_t>(h));
    stbi_image_free(px);
    return out;
  };

  // ======================================================================
  std::printf("  -- A. a decoded pattern is written out under its own uuid --\n");
  // ======================================================================
  {
    std::vector<std::string> notes;
    const std::vector<std::string> ids =
        extractAbrPatterns(importedRoot, {{uuid, field}}, &notes);
    check(ids.size() == 1 && ids[0] == uuid && notes.empty(),
          "pattern/extract: a decoded pattern is written under its own `patt` uuid, bare");
    const fs::path target = fs::path(importedRoot) / (uuid + ".png");
    check(fs::exists(target), "pattern/extract: as a PNG in patterns-imported/, which the write DOES create");

    int w = 0, h = 0;
    const std::vector<uint8_t> back = readGrayPng(target, w, h);
    check(w == field.width && h == field.height,
          "pattern/extract: the PNG's own dimensions match the decoded pattern's");
    check(back.size() == field.height8.size() && back == field.height8,
          "pattern/extract: and every height byte reads back exactly -- single-channel, no alpha "
          "channel mixed in the way a tip's coverage mask deliberately is");
  }

  // ======================================================================
  std::printf("  -- B. a second import does not rewrite an existing file --\n");
  // ======================================================================
  {
    PaperField different = field;
    different.height8.assign(different.height8.size(), 200);
    const std::vector<std::string> again =
        extractAbrPatterns(importedRoot, {{uuid, different}});
    const fs::path target = fs::path(importedRoot) / (uuid + ".png");
    int w = 0, h = 0;
    const std::vector<uint8_t> back = readGrayPng(target, w, h);
    check(again.size() == 1 && again[0] == uuid,
          "pattern/reimport: the uuid already names this file -- reported, not re-decoded");
    check(back == field.height8,
          "pattern/reimport: and the file ON DISK is untouched -- a later touch-up survives a "
          "re-import of the same pack");
  }

  // ======================================================================
  std::printf("  -- C. a malformed uuid is refused, not asked to name a file --\n");
  // ======================================================================
  {
    // Removed first for the same reason app/selftest/DabLibrary.cpp's own
    // escape-guard section removes its target first: if the guard ever
    // fails, the file it writes must land outside `scratch`, which this test
    // is about to delete wholesale -- otherwise a later run finds a PREVIOUS
    // run's debris and fails for the wrong reason.
    const fs::path escapeTarget =
        fs::path(importedRoot).parent_path().parent_path() / "escaped-pattern.png";
    fs::remove(escapeTarget, ec);
    std::vector<std::string> evilNotes;
    const std::vector<std::string> refused =
        extractAbrPatterns(importedRoot, {{"../../escaped-pattern", field}}, &evilNotes);
    check(refused.empty() && evilNotes.size() == 1,
          "pattern/extract: an id that is not a plain uuid is refused, not asked to name a file");
    check(!fs::exists(escapeTarget),
          "pattern/extract: and nothing is written outside the imported root");
  }

  // ======================================================================
  std::printf("  -- D. a pattern over the threshold is downsampled before writing --\n");
  // ======================================================================
  {
    // Measured, not guessed: real `.abr` packs carry patterns up to
    // 2016x2016 (`kPatternDownsampleThreshold`'s own comment in
    // app/DabLibrary.cpp has the four-pack measurement), well past
    // io/PsPatterns.hpp's documented "128x128 to 900x900". 1031 is chosen
    // ODD and past the 1024 threshold on purpose: odd exercises
    // `boxFilterHalve()`'s edge-clamp column/row (the last output texel
    // averages a single input column/row against itself, not two), and past
    // the threshold exercises the halving at all.
    //
    // **A CONSTANT field, not a gradient**, is what makes the assertion exact
    // rather than approximate: a 2x2 box average of four (or, at a clamped
    // edge, one value doubled) identical bytes is that same byte, with no
    // rounding to reason about -- `(4*137 + 2) / 4` truncates to exactly 137
    // in integer division, for every V, not merely for 137. If this ever
    // came back off by one, it would be the rounding constant, not luck.
    const std::string bigUuid = "1031102e-0000-4000-8000-0000decade00";
    PaperField big;
    big.width = 1031;
    big.height = 1031;
    big.height8.assign(static_cast<size_t>(big.width) * big.height, 137);

    const std::vector<std::string> ids = extractAbrPatterns(importedRoot, {{bigUuid, big}});
    check(ids.size() == 1 && ids[0] == bigUuid,
          "pattern/downsample: a pattern over the threshold is still written under its uuid");

    int w = 0, h = 0;
    const std::vector<uint8_t> back =
        readGrayPng(fs::path(importedRoot) / (bigUuid + ".png"), w, h);
    // (1031 + 1) / 2 = 516 -- ceiling, so the odd last row/column is
    // represented rather than dropped by a floor division.
    check(w == 516 && h == 516,
          "pattern/downsample: 1031x1031 halves to EXACTLY 516x516 -- ceiling division, not floor");
    bool allExact = !back.empty();
    for (const uint8_t v : back)
      if (v != 137) allExact = false;
    check(allExact,
          "pattern/downsample: every texel of a constant field survives the box filter exactly, "
          "edge-clamped column/row included");

    // A pattern AT the threshold is written untouched -- the check is `>`,
    // not `>=`, so 1024 itself is not downsampled for no reason.
    const std::string atUuid = "10a4affa-0000-4000-8000-0000decade00";
    PaperField at;
    at.width = kPatternDownsampleThreshold;  // 1024, from app/DabLibrary.hpp's re-export... see below
    at.height = kPatternDownsampleThreshold;
    at.height8.assign(static_cast<size_t>(at.width) * at.height, 200);
    extractAbrPatterns(importedRoot, {{atUuid, at}});
    int aw = 0, ah = 0;
    readGrayPng(fs::path(importedRoot) / (atUuid + ".png"), aw, ah);
    check(aw == kPatternDownsampleThreshold && ah == kPatternDownsampleThreshold,
          "pattern/downsample: a pattern AT the threshold is written at full size -- '>', not '>='");
  }

  fs::remove_all(scratch, ec);
  return ok;
}

}  // namespace np
