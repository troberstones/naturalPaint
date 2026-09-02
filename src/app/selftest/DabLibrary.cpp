#include "app/selftest/Support.hpp"

#include <filesystem>
#include <fstream>

#include "app/DabLibrary.hpp"
#include "color/Space.hpp"

namespace np {

// ---------------------------------------------------------------------------
// app/DabLibrary -- the watched folder, and the two claims it stands on.
//
// **Claim one: an unchanged rescan decodes nothing.** That is what makes a
// scan cheap enough to run on a window-focus event, and it is the difference
// between a folder that can hold five hundred tips and one that can hold
// twenty. It is asserted here against `decodeCount()` rather than described,
// because a rescan that quietly re-decoded everything would pass every
// correctness test in this file and still make the feature unusable.
//
// **Claim two: a rename does not orphan a preset.** A `file:` id names a path
// (DabLibrary.hpp §3), so renaming in Finder would break every brush pointing
// at it. The index carries a fingerprint of the decoded coverage so a rescan
// can recognise the file under its new name and keep the old id. §3 is honest
// that this is a mitigation with edges; section E walks up to one of them
// deliberately.
//
// Everything here runs against a scratch directory under the system temp
// path. **Nothing touches `~/Library/Application Support/naturalPaint`** --
// the roots are injected through `setRoots()`, which is what that parameter
// is for.
// ---------------------------------------------------------------------------
bool runDabLibraryTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  namespace fs = std::filesystem;
  const fs::path scratch = fs::temp_directory_path() / "np-selftest-dablibrary";
  std::error_code ec;
  fs::remove_all(scratch, ec);
  fs::create_directories(scratch, ec);
  const std::string userRoot = (scratch / "dabs").string();
  const std::string importedRoot = (scratch / "dabs-imported").string();
  const std::string indexPath = (scratch / "dab-index.txt").string();

  // --- fixtures ----------------------------------------------------------
  auto writeFile = [](const fs::path& p, const std::vector<uint8_t>& bytes) {
    std::error_code e;
    fs::create_directories(p.parent_path(), e);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  };
  // An RGBA PNG whose alpha is a horizontal ramp -- the "has a real alpha
  // channel" branch.
  auto rgbaRampPng = [](int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        const size_t i = (static_cast<size_t>(y) * w + x) * 4;
        px[i] = px[i + 1] = px[i + 2] = 255;
        px[i + 3] = static_cast<uint8_t>(x * 255 / std::max(1, w - 1));
      }
    std::vector<uint8_t> out;
    stbi_write_png_to_func(&appendToVector, &out, w, h, 4, px.data(), w * 4);
    return out;
  };
  // A fully OPAQUE greyscale PNG -- the "black on white, use 1 - luminance"
  // branch, which is how most scanned tips are drawn.
  auto greyPng = [](int w, int h, uint8_t value) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
      px[i] = px[i + 1] = px[i + 2] = value;
      px[i + 3] = 255;
    }
    std::vector<uint8_t> out;
    stbi_write_png_to_func(&appendToVector, &out, w, h, 4, px.data(), w * 4);
    return out;
  };
  auto gbrV2 = [](int w, int h, const char* name, uint32_t spacing, uint8_t seed) {
    std::vector<uint8_t> out;
    auto be32 = [&out](uint32_t v) {
      out.push_back(static_cast<uint8_t>(v >> 24));
      out.push_back(static_cast<uint8_t>(v >> 16));
      out.push_back(static_cast<uint8_t>(v >> 8));
      out.push_back(static_cast<uint8_t>(v));
    };
    const size_t nameLen = std::strlen(name) + 1;
    be32(static_cast<uint32_t>(28 + nameLen));
    be32(2);
    be32(static_cast<uint32_t>(w));
    be32(static_cast<uint32_t>(h));
    be32(1);
    be32(0x47494D50u);
    be32(spacing);
    for (size_t i = 0; i < nameLen; ++i) out.push_back(static_cast<uint8_t>(name[i]));
    for (int i = 0; i < w * h; ++i) out.push_back(static_cast<uint8_t>((i * 7 + seed) % 256));
    return out;
  };

  // ======================================================================
  std::printf("  -- A. one rule turns a picture into coverage --\n");
  // ======================================================================
  {
    // The alpha branch. A ramp so the mapping is checkable at more than its
    // endpoints, and 255 = full coverage the way brush/Deposit §2c wants.
    std::vector<float> px(5 * 1 * 4, 1.0f);
    for (int x = 0; x < 5; ++x) px[static_cast<size_t>(x) * 4 + 3] = static_cast<float>(x) / 4.0f;
    const BrushTipBitmap fromAlpha = coverageFromDecodedImage(5, 1, px);
    check(fromAlpha.width == 5 && fromAlpha.height == 1 && fromAlpha.alpha.size() == 5,
          "dab/coverage: dimensions and buffer length come straight from the image");
    check(fromAlpha.alpha[0] == 0 && fromAlpha.alpha[4] == 255 && fromAlpha.alpha[2] == 128,
          "dab/coverage: a real alpha channel IS the coverage, 255 = full");

    // The luminance branch. Every alpha is 1, which is both "no alpha
    // channel" and "an opaque one" -- the header's argument that those two
    // are indistinguishable in the buffer and want the same answer.
    const float midLinear = srgbDecode(0.5f);
    std::vector<float> opaque(3 * 1 * 4, 1.0f);
    opaque[0] = opaque[1] = opaque[2] = 0.0f;              // black
    opaque[4] = opaque[5] = opaque[6] = midLinear;         // mid grey
    opaque[8] = opaque[9] = opaque[10] = 1.0f;             // white
    const BrushTipBitmap fromLuma = coverageFromDecodedImage(3, 1, opaque);
    check(fromLuma.alpha[0] == 255 && fromLuma.alpha[2] == 0,
          "dab/coverage: with no usable alpha, BLACK paints and WHITE does not");
    // **The display-encoded luminance, not the linear one.** An sRGB mid-grey
    // is 0.216 in linear; `1 - 0.216` would make what the artist saw as half
    // dark come out 78% opaque. Bracketed rather than pinned to one byte
    // because the round trip through the encode is not exact.
    std::printf("    [measured] an sRGB mid-grey becomes coverage %u/255 "
                "(linear luminance would give %u)\n",
                static_cast<unsigned>(fromLuma.alpha[1]),
                static_cast<unsigned>(static_cast<uint8_t>((1.0f - midLinear) * 255.0f + 0.5f)));
    check(fromLuma.alpha[1] >= 126 && fromLuma.alpha[1] <= 130,
          "dab/coverage: a mid-grey is half coverage -- display-encoded luma, not linear");

    // Fingerprints: dimensions first, so a transposed tip is a different one.
    BrushTipBitmap a;
    a.width = 4; a.height = 9; a.alpha.assign(36, 7);
    BrushTipBitmap b = a;
    b.width = 9; b.height = 4;
    check(dabFingerprint(a) != dabFingerprint(b),
          "dab/fingerprint: 4x9 and 9x4 carrying identical bytes are two fingerprints");
    BrushTipBitmap c = a;
    c.alpha[17] = 8;
    check(dabFingerprint(a) != dabFingerprint(c) && dabFingerprint(a) == dabFingerprint(BrushTipBitmap(a)),
          "dab/fingerprint: one changed byte changes it, and it is a pure function");
  }

  // ======================================================================
  std::printf("  -- B. the index round-trips, and a foreign version is discarded --\n");
  // ======================================================================
  {
    DabLibrary lib;
    lib.setRoots(userRoot, importedRoot, indexPath);
    // A path with a tab and a backslash in it. Both are legal in a filename
    // on every platform this builds for, and the index is tab-separated.
    lib.parseIndex(
        "version 1\n"
        "file:odd\\tname\\\\x.png\todd\\tname\\\\x.png\tuser\t0\t0\t16\t16\t123\t99\t7\t0\t0\todd\n");
    check(lib.entries().size() == 1 &&
              lib.entries()[0].relPath == std::string("odd\tname\\x.png"),
          "dab/index: a tab and a backslash in a path survive the round trip");
    const std::string text = lib.indexText();
    DabLibrary again;
    again.setRoots(userRoot, importedRoot, indexPath);
    again.parseIndex(text);
    check(again.entries().size() == 1 && again.entries()[0].id == lib.entries()[0].id &&
              again.entries()[0].relPath == lib.entries()[0].relPath &&
              again.entries()[0].fingerprint == 123 && again.entries()[0].width == 16,
          "dab/index: re-serialised and re-parsed, every field is what it was");

    // A cache from a build whose columns this one does not know. Discarded
    // whole rather than half-read -- the cost is one slower scan.
    DabLibrary future;
    future.setRoots(userRoot, importedRoot, indexPath);
    future.parseIndex("version 99\nfile:x.png\tx.png\tuser\t0\t0\t1\t1\t1\t1\t1\t0\t0\tx\n");
    check(future.entries().empty(),
          "dab/index: an index from a newer build is dropped, not partly believed");
  }

  // ======================================================================
  std::printf("  -- C. a scan that finds nothing leaves nothing behind --\n");
  // ======================================================================
  {
    // Neither root exists yet. Opening a picker must not create folders in
    // the user's Application Support, and must not leave a stub index there
    // either -- the same impoliteness by a side door.
    DabLibrary lib;
    lib.setRoots(userRoot, importedRoot, indexPath);
    const DabScanResult r = lib.rescan();
    check(r.added == 0 && r.rejected == 0 && lib.entries().empty(),
          "dab/empty: a library with no folders scans to nothing, and is not an error");
    check(!fs::exists(userRoot) && !fs::exists(importedRoot),
          "dab/empty: and the scan does NOT create either root");
    check(!fs::exists(indexPath),
          "dab/empty: nor a stub index -- an empty library writes no file at all");
  }

  // ======================================================================
  std::printf("  -- D. an unchanged rescan decodes NOTHING --\n");
  // ======================================================================
  {
    writeFile(fs::path(userRoot) / "ramp.png", rgbaRampPng(8, 4));
    writeFile(fs::path(userRoot) / "sub" / "grey.png", greyPng(6, 6, 64));
    writeFile(fs::path(userRoot) / "chalk.gbr", gbrV2(5, 3, "Chalk", 37, 11));
    // Platform metadata, and a file that is not a picture.
    writeFile(fs::path(userRoot) / ".DS_Store", {1, 2, 3});
    writeFile(fs::path(userRoot) / "notes.txt", {'h', 'i', '\n'});

    DabLibrary lib;
    lib.setRoots(userRoot, importedRoot, indexPath);
    const DabScanResult first = lib.rescan();
    std::printf("    [measured] first scan: %d added, %d refused, %zu decoded\n", first.added,
                first.rejected, lib.decodeCount());
    check(first.added == 3 && lib.entries().size() == 3,
          "dab/scan: three tips found, including one in a subfolder");
    check(first.rejected == 1 && first.notes.size() == 1,
          "dab/scan: the non-picture is refused ONCE, with a note naming it");
    check(first.notes[0].rfind("notes.txt", 0) == 0,
          "dab/scan: and the note names the file, so a folder can answer 'why not mine'");
    // A dot-file is not refused, it is not a candidate. Counting it as a
    // refusal would put `.DS_Store` in front of the user on every scan.
    check(first.rejected == 1, "dab/scan: a dot-file is skipped silently, not reported");

    check(lib.find("file:ramp.png") != nullptr && lib.find("file:sub/grey.png") != nullptr &&
              lib.find("gbr:chalk.gbr") != nullptr,
          "dab/ids: an image is `file:`, a GIMP brush is `gbr:`, both relative to the root");
    const DabEntry* chalk = lib.find("gbr:chalk.gbr");
    check(chalk != nullptr && chalk->haveSpacing && chalk->spacingPercent == 37.0f,
          "dab/gbr: the file's own spacing is carried, offered rather than applied");

    // **The claim.**
    const size_t decodedAfterFirst = lib.decodeCount();
    const DabScanResult second = lib.rescan();
    std::printf("    [measured] second scan: %d unchanged, %d added, %zu decoded (was %zu)\n",
                second.unchanged, second.added, lib.decodeCount() - decodedAfterFirst,
                decodedAfterFirst);
    check(second.unchanged == 3 && second.added == 0,
          "dab/cache: a rescan with nothing changed carries all three across");
    check(lib.decodeCount() == decodedAfterFirst,
          "dab/cache: and opens NOT ONE file -- the property that makes a focus-event scan free");

    // A cold library reading the index off disk must be just as cheap, which
    // is the case that actually happens: the application relaunches.
    DabLibrary cold;
    cold.setRoots(userRoot, importedRoot, indexPath);
    const DabScanResult coldScan = cold.rescan();
    check(coldScan.unchanged == 3 && cold.decodeCount() == 0,
          "dab/cache: a fresh library reading the index off disk decodes nothing either");

    // **A refusal is cached too, and that is not a detail.** Refusals were the
    // one path through a scan that was not indexed, so a folder holding fifty
    // holiday photos beside three brushes cost fifty decode ATTEMPTS on every
    // focus event -- the exact cost the index exists to avoid, arriving by the
    // door nobody had shut. The two assertions above are what caught it:
    // `decodeCount()` counts attempts, not successes.
    check(coldScan.rejected == 1 && coldScan.notes.size() == 1 &&
              coldScan.notes[0].rfind("notes.txt", 0) == 0,
          "dab/cache: the refusal is replayed from the index, note and all");
    check(cold.refusals().size() == 1 && cold.refusals()[0].relPath == "notes.txt",
          "dab/cache: and carried forward, so the next scan does not open it either");

    // ...and `resolve()` is what pays for the bitmap, once, on demand.
    const auto bitmap = cold.resolve("file:ramp.png");
    check(bitmap != nullptr && bitmap->width == 8 && bitmap->height == 4,
          "dab/resolve: a carried row decodes lazily, when something actually asks");
    check(cold.decodeCount() == 1 && cold.resolve("file:ramp.png") == bitmap,
          "dab/resolve: and once resolved it is held, not decoded again");
    check(cold.resolve("file:nothing-like-this.png") == nullptr,
          "dab/resolve: an unknown id is null rather than a fabricated blank tip");

    // Touching a file's CONTENT must reopen it. Both size and mtime move
    // here, which is the ordinary case; the header is clear that the pair is
    // a heuristic and not a hash.
    writeFile(fs::path(userRoot) / "ramp.png", rgbaRampPng(12, 4));
    DabLibrary changed;
    changed.setRoots(userRoot, importedRoot, indexPath);
    const DabScanResult third = changed.rescan();
    const DabEntry* wider = changed.find("file:ramp.png");
    check(changed.decodeCount() == 1 && third.unchanged == 2,
          "dab/cache: an edited file is reopened and the other two still are not");
    check(wider != nullptr && wider->width == 12,
          "dab/cache: and the entry picks up its new size");

    // The other half of a cached refusal: editing the file gets it a fresh
    // hearing. A permanent verdict on a path would mean fixing a truncated
    // download could never be noticed.
    writeFile(fs::path(userRoot) / "notes.txt", greyPng(4, 4, 32));
    DabLibrary fixed;
    fixed.setRoots(userRoot, importedRoot, indexPath);
    const DabScanResult fourth = fixed.rescan();
    check(fourth.rejected == 0 && fixed.find("file:notes.txt") != nullptr,
          "dab/cache: a refused file that CHANGES is reconsidered, not condemned to its path");
    // Put it back, and settle the index, so section E's counts are about
    // section E's rename and nothing else.
    std::error_code rm;
    fs::remove(fs::path(userRoot) / "notes.txt", rm);
    DabLibrary settle;
    settle.setRoots(userRoot, importedRoot, indexPath);
    settle.rescan();
  }

  // ======================================================================
  std::printf("  -- E. a rename keeps the id, so a preset is not orphaned --\n");
  // ======================================================================
  {
    fs::rename(fs::path(userRoot) / "chalk.gbr", fs::path(userRoot) / "renamed chalk.gbr", ec);
    DabLibrary lib;
    lib.setRoots(userRoot, importedRoot, indexPath);
    const DabScanResult r = lib.rescan();
    const DabEntry* kept = lib.find("gbr:chalk.gbr");
    std::printf("    [measured] after a rename: %d added, %d renamed, %d gone\n", r.added,
                r.repaired, r.removed);
    check(r.repaired == 1 && r.removed == 0 && r.added == 0,
          "dab/rename: recognised by fingerprint as a rename, not as a delete plus an add");
    check(kept != nullptr && kept->relPath == "renamed chalk.gbr",
          "dab/rename: the id survives -- a preset pointing at it still resolves");
    check(kept != nullptr && lib.resolve("gbr:chalk.gbr") != nullptr,
          "dab/rename: and resolving that id opens the file under its NEW path");

    // The edge §3 admits to, walked up to on purpose so it is documented by a
    // passing assertion rather than only by a comment: two identical files
    // are one fingerprint, and deleting one while adding another is
    // indistinguishable from moving it.
    writeFile(fs::path(userRoot) / "twin.gbr", gbrV2(5, 3, "Chalk", 37, 11));
    DabLibrary twins;
    twins.setRoots(userRoot, importedRoot, indexPath);
    twins.rescan();
    check(twins.find("gbr:chalk.gbr") != nullptr && twins.find("gbr:twin.gbr") != nullptr,
          "dab/rename: a duplicate that appears while the original STAYS gets its own id");
  }

  // ======================================================================
  std::printf("  -- F. deletions, and the two roots stay apart --\n");
  // ======================================================================
  {
    fs::remove(fs::path(userRoot) / "twin.gbr", ec);
    // The same bytes under the imported root: a different root is a different
    // entry, never a rename of the user's copy. The application writes to one
    // of these folders and not the other, and confusing them is how it would
    // delete somebody's own file.
    writeFile(fs::path(importedRoot) / "twin.gbr", gbrV2(5, 3, "Chalk", 37, 11));

    DabLibrary lib;
    lib.setRoots(userRoot, importedRoot, indexPath);
    const DabScanResult r = lib.rescan();
    const DabEntry* imported = lib.find("gbr:twin.gbr");
    check(imported != nullptr && imported->root == DabRoot::Imported,
          "dab/roots: the imported copy is found and is marked as the imported one");
    check(r.repaired <= 1, "dab/roots: at most the one move was treated as a rename");

    // And a real deletion, with nothing to match it, counts as gone.
    fs::remove(fs::path(importedRoot) / "twin.gbr", ec);
    fs::remove(fs::path(userRoot) / "sub" / "grey.png", ec);
    DabLibrary after;
    after.setRoots(userRoot, importedRoot, indexPath);
    const DabScanResult gone = after.rescan();
    check(gone.removed == 2 && after.find("file:sub/grey.png") == nullptr,
          "dab/delete: a removed file leaves the library, and is counted as gone");
    check(fs::exists(fs::path(userRoot) / "ramp.png"),
          "dab/delete: and the scan itself deletes nothing -- it only ever reads `dabs/`");
  }

  // ======================================================================
  std::printf("  -- G. an extracted .abr tip outlives the pack it came from --\n");
  // ======================================================================
  {
    // The defect brush/Library.hpp names in its own words: Duplicate on a
    // sampled-tip preset copies a pointer, Save writes seven scalars and no
    // bitmap, "so a saved duplicate of a sampled-tip brush reloads next launch
    // as the round procedural tip". This is that sequence, run end to end.
    const std::string uuid = "63d61f21-0000-4000-8000-bc81e4dfd608";
    BrushTipBitmap tip;
    tip.width = 6;
    tip.height = 4;
    tip.alpha.resize(24);
    for (size_t i = 0; i < tip.alpha.size(); ++i) tip.alpha[i] = static_cast<uint8_t>(i * 10);
    // One texel at full coverage, so the "opaque everywhere" branch of §4 is
    // not what this is measuring, and one at zero.
    tip.alpha[0] = 0;
    tip.alpha[23] = 255;

    std::vector<std::string> notes;
    const std::vector<std::string> ids =
        extractAbrTips(importedRoot, {{uuid, tip}}, &notes);
    check(ids.size() == 1 && ids[0] == "abr:" + uuid && notes.empty(),
          "dab/extract: a sampled tip is written out under its own `samp` uuid");
    check(fs::exists(fs::path(importedRoot) / (uuid + ".png")),
          "dab/extract: as a PNG in the imported root, which the write DOES create");

    // **The round trip, which is the whole point.** The mask goes out in the
    // alpha channel over black; §4's rule must read it back as the same
    // coverage, byte for byte. A greyscale PNG would come back inverted here
    // and this assertion is what says so.
    DabLibrary lib;
    lib.setRoots(userRoot, importedRoot, indexPath);
    lib.rescan();
    const auto back = lib.resolve("abr:" + uuid);
    bool identical = back != nullptr && back->width == tip.width && back->height == tip.height &&
                     back->alpha.size() == tip.alpha.size();
    if (identical)
      for (size_t i = 0; i < tip.alpha.size(); ++i)
        if (back->alpha[i] != tip.alpha[i]) identical = false;
    check(identical,
          "dab/extract: and reads back BYTE-IDENTICAL -- alpha over black, not greyscale");

    const DabEntry* e = lib.find("abr:" + uuid);
    check(e != nullptr && e->source == DabSource::Abr && e->root == DabRoot::Imported,
          "dab/extract: recognised as an extracted tip by where it sits and what it is called");

    // Re-importing the same pack must not rewrite it -- the uuid names the
    // tip, so a file already at that name IS this tip, and rewriting would
    // discard a touch-up the user made in an image editor.
    BrushTipBitmap different = tip;
    different.alpha.assign(different.alpha.size(), 200);
    const std::vector<std::string> again = extractAbrTips(importedRoot, {{uuid, different}});
    // Read through a FRESH library, not `lib`. `lib` is holding the bitmap it
    // resolved a moment ago and would hand back the cached copy whatever is on
    // disk -- so asking it proves nothing about the file, which is the thing
    // under test. (A sabotage that removed the exists-check survived the
    // version of this assertion that used `lib`, which is how that was found.)
    DabLibrary reread;
    reread.setRoots(userRoot, importedRoot, indexPath);
    reread.rescan();
    const auto stillOriginal = reread.resolve("abr:" + uuid);
    check(again.size() == 1 && stillOriginal != nullptr && stillOriginal->alpha == tip.alpha,
          "dab/extract: a second import reports the id and leaves the file ON DISK alone");

    // A uuid from a file lands in a PATH, so it is checked and not trusted.
    //
    // The escape target is removed FIRST. If the guard ever fails, the file it
    // writes lands outside the scratch directory this section cleans up, so a
    // later run would find that debris and fail for the previous run's reason
    // rather than its own -- which is exactly what happened while sabotaging
    // this guard. Clearing it makes each run's verdict its own.
    const fs::path escapeTarget = fs::path(importedRoot).parent_path().parent_path() / "escaped.png";
    fs::remove(escapeTarget, ec);
    std::vector<std::string> evilNotes;
    const std::vector<std::string> refused =
        extractAbrTips(importedRoot, {{"../../escaped", tip}}, &evilNotes);
    check(refused.empty() && evilNotes.size() == 1,
          "dab/extract: an id that is not a plain uuid is refused, not asked to name a file");
    check(!fs::exists(escapeTarget),
          "dab/extract: and nothing is written outside the imported root");

    // The load half: a preset carrying only the id gets its bitmap back.
    BrushLibrary presets;
    BrushPreset saved;
    saved.name = "duplicated sampled brush";
    saved.dabId = "abr:" + uuid;
    presets.presets.push_back(saved);
    BrushPreset orphan;
    orphan.name = "points at a deleted tip";
    orphan.dabId = "file:gone.png";
    presets.presets.push_back(orphan);

    std::vector<std::string> resolveNotes;
    const size_t resolved = resolveDabIds(presets, lib, &resolveNotes);
    check(resolved == 1 && presets.presets[0].tipBitmap != nullptr &&
              presets.presets[0].tipBitmap->alpha == tip.alpha,
          "dab/persist: a preset carrying only an id gets its exact tip back");
    check(presets.presets[1].tipBitmap == nullptr && resolveNotes.size() == 1 &&
              resolveNotes[0].find("points at a deleted tip") != std::string::npos,
          "dab/persist: an id that no longer resolves is NAMED, and paints procedurally");
  }

  fs::remove_all(scratch, ec);
  return ok;
}

}  // namespace np
