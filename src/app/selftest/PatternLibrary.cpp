#include "app/selftest/Support.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "app/DabLibrary.hpp"
#include "app/PatternLibrary.hpp"
#include "brush/BrushModel.hpp"
#include "brush/Library.hpp"
#include "io/Export.hpp"

namespace np {

// ---------------------------------------------------------------------------
// app/PatternLibrary -- the papers the Texture panel's picker offers.
//
// Real files in a temporary pair of folders: one paper written the way an .abr
// import writes it, one greyscale image of the user's, and two files that are
// not papers. The load-bearing sections are B (a paper reads back as exactly the
// heights that were written) and F (a saved preset, which stores only the id,
// gets its paper back).
// ---------------------------------------------------------------------------
bool runPatternLibraryTest() {
  namespace fs = std::filesystem;
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const fs::path base =
      fs::temp_directory_path() /
      ("np-pattern-library-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const fs::path userRoot = base / "patterns";
  const fs::path importedRoot = base / "patterns-imported";
  std::error_code ec;
  fs::create_directories(userRoot, ec);

  auto makeField = [](int w, int h, int seed) {
    PaperField f;
    f.width = w;
    f.height = h;
    f.height8.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    for (size_t i = 0; i < f.height8.size(); ++i)
      f.height8[i] = static_cast<uint8_t>((i * 37 + static_cast<size_t>(seed)) & 255);
    return f;
  };
  auto writeGreyPng = [](const fs::path& path, const PaperField& f) {
    const std::vector<uint8_t> png = encodePng8Gray(static_cast<uint32_t>(f.width),
                                                    static_cast<uint32_t>(f.height),
                                                    f.height8.data());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  };

  const std::string uuid = "63d61f21-0000-1111-2222-bc81e4dfd608";
  const PaperField paper = makeField(8, 6, 11);
  {
    std::vector<std::pair<std::string, PaperField>> extracted;
    extracted.emplace_back(uuid, paper);
    (void)extractAbrPatterns(importedRoot.string(), extracted);
  }
  const PaperField canvas = makeField(5, 7, 90);
  writeGreyPng(userRoot / "Canvas.png", canvas);
  std::ofstream(userRoot / "notes.txt") << "not a paper";
  std::ofstream(userRoot / ".DS_Store") << "platform metadata";

  PatternLibrary lib;
  lib.setRoots(userRoot.string(), importedRoot.string());

  std::printf("  -- A. a scan lists papers and decodes nothing --\n");
  const std::vector<std::string> notes = lib.rescan();
  {
    const PatternEntry* imported = lib.find(uuid);
    const PatternEntry* user = lib.find("file:Canvas.png");
    check(lib.entries().size() == 2, "scan: the extracted paper and the user's image, nothing else");
    check(imported != nullptr && imported->root == PatternRoot::Imported,
          "scan: an extracted paper keeps its bare uuid, the id a preset holds");
    check(user != nullptr && user->name == "Canvas",
          "scan: a user's image is file:<relpath>, named after its file");
    check(imported != nullptr && imported->width == 8 && imported->height == 6,
          "scan: a paper's size comes from its header");
    check(lib.decodeCount() == 0, "scan: no paper decoded");
    check(notes.size() == 1 && notes[0].find("notes.txt") != std::string::npos,
          "scan: the non-image is named in a note, the dot-file is skipped silently");
  }

  std::printf("  -- B. a paper reads back as the heights written --\n");
  const std::shared_ptr<const PaperField> a = lib.resolve(uuid);
  {
    check(a != nullptr && a->width == 8 && a->height == 6 && a->height8 == paper.height8,
          "resolve: an extracted paper, byte for byte");
    const std::shared_ptr<const PaperField> b = lib.resolve("file:Canvas.png");
    check(b != nullptr && b->height8 == canvas.height8,
          "resolve: a greyscale image's bytes are its heights");
    check(lib.decodeCount() == 2 && lib.resolve(uuid) == a && lib.decodeCount() == 2,
          "resolve: decoded once, then kept");
    check(lib.resolve("file:missing.png") == nullptr, "resolve: an unknown id is null");
  }

  std::printf("  -- C. a rescan re-reads only what changed --\n");
  {
    (void)lib.rescan();
    check(lib.resolve(uuid) == a && lib.decodeCount() == 2,
          "rescan: an unchanged file keeps its decoded paper");
    writeGreyPng(userRoot / "Canvas.png", makeField(3, 3, 200));
    const uint64_t before = lib.generation();
    (void)lib.rescan();
    const std::shared_ptr<const PaperField> changed = lib.resolve("file:Canvas.png");
    check(changed != nullptr && changed->width == 3 && lib.decodeCount() == 3,
          "rescan: a rewritten file is decoded again");
    check(lib.generation() != before, "rescan: a changed file moves the thumbnail generation");
  }

  std::printf("  -- D. an extracted paper takes its preset's name --\n");
  {
    lib.noteName(uuid, "Extra Heavy Canvas");
    check(lib.find(uuid) != nullptr && lib.find(uuid)->name == "Extra Heavy Canvas",
          "names: noteName renames the extracted paper");
    lib.setRoots(userRoot.string(), importedRoot.string());
    (void)lib.rescan();
    check(lib.find(uuid) != nullptr && lib.find(uuid)->name == "Extra Heavy Canvas",
          "names: remembered across setRoots and a fresh scan");
  }

  std::printf("  -- E. picking puts the paper on the Texture panel --\n");
  {
    PsTexture texture;
    check(selectPattern(texture, lib, uuid) && texture.pattern.id == uuid &&
              texture.pattern.name == "Extra Heavy Canvas" && texture.pattern.field != nullptr &&
              texture.pattern.field->height8 == paper.height8,
          "select: id, name and paper, together");
    check(!selectPattern(texture, lib, "file:missing.png") && texture.pattern.id == uuid,
          "select: an id that does not resolve leaves the panel as it was");
    check(selectPattern(texture, lib, "") && texture.pattern.empty() &&
              texture.pattern.field == nullptr,
          "select: None clears the pattern and its paper");
  }

  std::printf("  -- F. a saved preset gets its paper back --\n");
  {
    BrushLibrary brushes;
    BrushPreset saved;
    saved.name = "Saved";
    saved.model.texture.pattern.id = uuid;
    saved.model.texture.pattern.name = "Brayer 3";
    BrushPreset lost;
    lost.name = "Lost";
    lost.model.texture.pattern.id = "11111111-aaaa-bbbb";
    lost.model.texture.pattern.name = "Gone Paper";
    BrushPreset plain;
    plain.name = "Plain";
    brushes.presets = {saved, lost, plain};

    PatternLibrary fresh;
    fresh.setRoots(userRoot.string(), importedRoot.string());
    (void)fresh.rescan();
    std::vector<std::string> resolveNotes;
    const size_t resolved = resolvePatternIds(brushes, fresh, &resolveNotes);
    const auto& field = brushes.presets[0].model.texture.pattern.field;
    check(resolved == 1 && field != nullptr && field->height8 == paper.height8,
          "presets: the saved preset's paper is back, byte for byte");
    check(resolveNotes.size() == 1 && resolveNotes[0].find("Lost") != std::string::npos &&
              resolveNotes[0].find("Gone Paper") != std::string::npos,
          "presets: a paper neither folder holds is named, with its preset");
    check(fresh.find(uuid) != nullptr && fresh.find(uuid)->name == "Brayer 3",
          "presets: the preset's pattern name reaches the grid");
  }

  std::printf("  -- G. thumbnails --\n");
  {
    auto grey = [](const std::vector<uint8_t>& rgba, int x, int y, int side) {
      return rgba[(static_cast<size_t>(y) * side + x) * 4];
    };
    PaperField quad;
    quad.width = quad.height = 4;
    quad.height8.resize(16);
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x)
        quad.height8[y * 4 + x] = y < 2 ? (x < 2 ? 10 : 200) : (x < 2 ? 60 : 250);
    const std::vector<uint8_t> t = patternThumbnailRgba(quad, 8);
    check(grey(t, 0, 0, 8) == 10 && grey(t, 7, 0, 8) == 200 && grey(t, 0, 7, 8) == 60 &&
              grey(t, 7, 7, 8) == 250 && t[3] == 255 && t[1] == t[0],
          "thumbnail: opaque grey, the right way up and round");
    PaperField wide;
    wide.width = 8;
    wide.height = 4;
    wide.height8.resize(32);
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 8; ++x) wide.height8[y * 8 + x] = x < 4 ? 10 : 200;
    const std::vector<uint8_t> tw = patternThumbnailRgba(wide, 8);
    check(grey(tw, 7, 0, 8) == 10,
          "thumbnail: a wide paper is cropped to its top-left square, not squashed");
  }

  fs::remove_all(base, ec);
  std::printf("[selftest] pattern library %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
