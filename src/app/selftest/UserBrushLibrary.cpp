#include "app/selftest/Support.hpp"

#include "brush/BrushModelDiff.hpp"
#include "brush/BrushModelIo.hpp"

#include "app/BrushLibraryFile.hpp"
#include "app/BrushRowIcon.hpp"
#include "app/DabPreview.hpp"
#include "app/StrokeSession.hpp"
#include "app/UserBrushLibrary.hpp"

namespace np {
namespace {

bool contains(const std::string& s, const char* needle) {
  return s.find(needle) != std::string::npos;
}

size_t countBuiltins(const BrushLibrary& lib) {
  size_t n = 0;
  for (const BrushPreset& p : lib.presets)
    if (p.builtin) ++n;
  return n;
}

const BrushPreset* findByName(const BrushLibrary& lib, const char* name) {
  for (const BrushPreset& p : lib.presets)
    if (p.name == name) return &p;
  return nullptr;
}

// A preset with two links, chosen so neither is a preset trivially: an
// S-curve with an interior point that needs all nine of `%.9g`'s digits to
// come back exactly, and a disabled link whose curve and range must survive
// being switched off (brush/Dynamics.hpp's own comment on `BrushLink::
// enabled`: "a link the user has switched off keeps its curve and range").
//
// **The links no longer round-trip as LIVE data.** Part 4 changed
// `UserBrushLibrary.cpp`'s parser so every well-formed `link`/`floor` line
// is preserved verbatim rather than built into a live `BrushLinkSet` --
// `app/selftest/ShelvedLinks.cpp` is the dedicated test for that byte-exact
// text preservation. What THIS file's own links are still good for: proving
// a preset that carries them serialises and reloads its scalars/model
// correctly regardless, and (§1 below) that the text itself still survives.
BrushPreset makeAuthoredPreset(BrushLibrary& lib, const char* wantedName) {
  BrushPreset p;
  p.name = uniquePresetName(lib, wantedName);
  p.model.tip.diameterPx = 67.0f;      // radius 33.5
  p.model.tip.hardness = 0.618034f;
  p.model.tip.spacingPercent = 18.3f;  // spacing (radii) 0.366 -- /100*2
  p.model.tip.roundness = 0.729f;
  p.model.tip.angleDeg = 47.25f;
  p.load = 1.14159265f;
  p.wetness = 0.874321f;

  BrushLink sizeLink;
  sizeLink.source = DynamicSource::Pressure;
  sizeLink.target = DynamicTarget::Size;
  sizeLink.rangeLo = 0.123456789f;  // exercises every one of f9()'s 9 digits
  sizeLink.rangeHi = 1.0f;
  sizeLink.invert = false;
  sizeLink.enabled = true;
  sizeLink.curve = {{0.0f, 0.0f}, {0.400000006f, 0.142857143f}, {1.0f, 1.0f}};
  addLink(p.links, sizeLink);

  BrushLink offLink;
  offLink.source = DynamicSource::Tilt;
  offLink.target = DynamicTarget::Angle;
  offLink.rangeLo = -30.0f;
  offLink.rangeHi = 30.0f;
  offLink.invert = true;
  offLink.enabled = false;  // must still round-trip its curve and range
  offLink.curve = {{0.0f, 0.2f}, {1.0f, 0.9f}};
  addLink(p.links, offLink);

  return p;
}

}  // namespace

// app/UserBrushLibrary: the presets a user made, saved so they are still
// there tomorrow (PRD G6, reachability-audit.md's A7). app/BrushLibraryFile's
// own suite is the model for shape and is not restated here; read that file
// first for the line-based-file reasoning this one shares.
//
// The one thing that suite could not prove and this one exists to: a full
// `BrushLinkSet`, with real `Curve` control points, surviving a save and a
// reload with nothing lost -- and doing so in a way that would actually
// redden if the links silently stopped making the round trip, rather than
// only checking that a struct still has the right shape.
bool runUserBrushLibraryTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  namespace fs = std::filesystem;
  std::error_code ec;

  // Everything below lives here; $NP_USER_PRESETS and $NP_BRUSH_LIBRARIES
  // both point into it, so nothing touches
  // ~/Library/Application Support/naturalPaint. Same mechanism, same reason,
  // as app/selftest/BrushLibraryFile.cpp's own root.
  const std::string root = "selftest_userbrushlib";
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  const std::string userPath = root + "/user-presets.txt";
  const std::string abrPrefsPath = root + "/brush-libraries.txt";

  const char* prevUser = std::getenv("NP_USER_PRESETS");
  const std::string savedUser = prevUser ? prevUser : "";
  setenv("NP_USER_PRESETS", userPath.c_str(), 1);
  const char* prevAbr = std::getenv("NP_BRUSH_LIBRARIES");
  const std::string savedAbr = prevAbr ? prevAbr : "";
  setenv("NP_BRUSH_LIBRARIES", abrPrefsPath.c_str(), 1);

  check(defaultUserPresetsFilePath() == userPath,
        "userbrushlib: $NP_USER_PRESETS overrides the settings path, so this section cannot "
        "touch the real one");

  const MixboxLut lut;  // invalid on purpose -- see app/selftest/BrushLibraryFile.cpp's own
                        // note; this section is about presets and links, not pigment.

  // ======================================================================
  // 1. THE assertion, rewritten for Part 4: the scalars/model round-trip at
  //    zero tolerance, and the link TEXT survives even though the live
  //    struct no longer does
  // ======================================================================
  //
  // Save then reload, and check every field -- not via `presetMatches()`
  // (that comparison is what the EDITED badge uses and deliberately ignores
  // `libraryId`/`builtin`; here those two matter as much as the rest) but
  // field by field. The links are no longer checked via `linkSetsEqual()`
  // against a live reloaded struct -- Part 4 made `link`/`floor` lines
  // preserve-verbatim unconditionally, so `back->links` is the DEFAULT
  // `BrushLinkSet`, not a reconstruction of `authored.links`. What DOES
  // still need proving here is that the two links' TEXT, curve points
  // included, comes back byte-for-byte -- `app/selftest/ShelvedLinks.cpp`
  // is the dedicated, focused version of this same claim; this section
  // keeps one instance of it so a regression here (this file's own long-
  // standing round-trip fixture) still reddens even if that newer file did
  // not exist.
  {
    BrushState brush;
    BrushLibrary& lib = brush.brushLibrary;
    const BrushPreset authored = makeAuthoredPreset(lib, "Round Trip Test");
    lib.presets.push_back(authored);
    lib.active = lib.presets.size() - 1;

    UserBrushLibraryStore store;
    std::string err;
    check(store.saveToFile(userPath, lib, &err) && err.empty(),
          "userbrushlib: a preset with two links, one disabled, saves without error");

    BrushState fresh;
    BrushLibrary& freshLib = fresh.brushLibrary;
    UserBrushLibraryStore reader;
    check(reader.loadFromFile(userPath, freshLib, &err) && err.empty(),
          "userbrushlib: and reads back into a brand-new BrushLibrary");

    const BrushPreset* back = findByName(freshLib, "Round Trip Test");
    check(back != nullptr, "userbrushlib: the preset comes back under its own name");
    if (back != nullptr) {
      check(back->libraryId == 0 && !back->builtin,
            "userbrushlib: reloaded as a user preset -- not a built-in, not owned by any "
            "imported library");
      check(back->model.tip.diameterPx == authored.model.tip.diameterPx &&
                back->model.tip.hardness == authored.model.tip.hardness &&
                back->model.tip.spacingPercent == authored.model.tip.spacingPercent &&
                back->model.tip.roundness == authored.model.tip.roundness &&
                back->model.tip.angleDeg == authored.model.tip.angleDeg &&
                back->load == authored.load && back->wetness == authored.wetness,
            "userbrushlib: all seven scalars (five of them projections of `model.tip` now) come "
            "back bit-identical -- `%.9g` is what buys that for values that are not round "
            "numbers");
      check(back->links.links.empty() &&
                back->links.multiplyFloor[static_cast<size_t>(DynamicTarget::Size)] == 0.0f,
            "userbrushlib: `back->links` is the DEFAULT BrushLinkSet -- Part 4's parser no "
            "longer builds a live one from `link`/`floor` lines, so this is empty even though "
            "`authored.links` had two links in it");
    }
    // Read the file back as raw text -- what actually proves the two links
    // (enabled and disabled, curve points included) survived is that their
    // exact lines are still there, not that a live struct rebuilt them.
    {
      std::ifstream f(userPath, std::ios::binary);
      std::ostringstream b;
      b << f.rdbuf();
      const std::string fileText = b.str();
      // `0.123456789f`/`0.142857143f` are not exact in binary32 -- the
      // nearest float to each, printed at `f9()`'s own nine significant
      // digits, is "0.123456791"/"0.142857149", not the literal this
      // fixture typed. The point of authoring a value that needs every
      // digit is that IT round-trips exactly; the digits themselves are
      // whatever binary32 actually holds.
      check(contains(fileText, "link 0 0 0.123456791 1 0 1") &&
                contains(fileText, "point 0.400000006 0.142857149"),
            "userbrushlib: the enabled link's line and its S-curve interior point (the value "
            "chosen to need all nine of `%.9g`'s digits) are both in the saved file, verbatim");
      check(contains(fileText, "link 1 1 -30 30 1 0"),
            "userbrushlib: and the DISABLED link's line is too -- 'a link the user has switched "
            "off keeps its curve and range' (brush/Dynamics.hpp) now means its TEXT survives, "
            "the same as an enabled one's");
    }

    // ====================================================================
    // 2. Not vacuous, restated for the model: the reloaded MODEL actually
    //    changes what the brush does, and the round trip preserved that
    //    behaviour exactly. (Links no longer drive appearance at all --
    //    `brushPresetIconTips()` -> `applyPresetToBrush()` copies
    //    `preset.model` onto the live brush and `brushTipFor()` reads only
    //    that, so a fixture built on clearing `links` would no longer prove
    //    anything.)
    // ====================================================================
    if (back != nullptr) {
      BrushPreset defaultModel = *back;
      defaultModel.model = BrushModel{};  // same preset, PLAIN model

      const DabPreviewImage original =
          rasteriseDabPreview(brushPresetIconTips(authored, brush, lut));
      const DabPreviewImage reloaded =
          rasteriseDabPreview(brushPresetIconTips(*back, brush, lut));
      const DabPreviewImage plain =
          rasteriseDabPreview(brushPresetIconTips(defaultModel, brush, lut));

      check(reloaded.rgba == original.rgba && !reloaded.rgba.empty(),
            "userbrushlib: the RELOADED preset previews byte-identically to the one that was "
            "saved -- the round trip preserved behaviour, not merely field values");
      check(reloaded.rgba != plain.rgba,
            "userbrushlib: and previews DIFFERENTLY from the same preset with a default model -- "
            "proof the model that came back is the one doing something, so the identity above "
            "could have failed");
    }
  }

  // ======================================================================
  // 3. Built-ins survive Save (fork), Delete, and Save again
  // ======================================================================
  //
  // Mirrors what `ui/MacPaintUI.cpp`'s `drawBrushSection()` Save button does
  // on a built-in -- forks a new preset via `presetFromBrush()` +
  // `uniquePresetName()`, exactly the sequence Duplicate already uses -- and
  // what its Delete button does, without going through ImGui: this suite is
  // headless, so the model-layer calls are made directly, the same way
  // app/selftest/BrushLibraryFile.cpp's own suite never touches
  // `ui/MacPaintUI.cpp` either.
  {
    BrushState brush;
    BrushLibrary& lib = brush.brushLibrary;
    check(countBuiltins(lib) == 4,
          "userbrushlib: a fresh library starts with the four built-ins");
    std::vector<BrushPreset> builtinsBefore;
    for (const BrushPreset& p : lib.presets)
      if (p.builtin) builtinsBefore.push_back(p);

    UserBrushLibraryStore store;
    std::string err;

    // Save on a built-in forks. Edit the live brush first, or the fork would
    // be an exact, unedited copy -- which is legal but would not exercise
    // "what got saved is what was on screen".
    lib.active = 0;  // "Round Bristle 03"
    applyPresetToBrush(lib.presets[0], brush);
    // `brush.radius` (BrushState's own remaining shadow scalar, pending its
    // own deletion) is no longer what `presetFromBrush()` reads -- it copies
    // `brush.model` wholesale now (`app/StrokeSession.cpp`'s own comment on
    // `applyPresetToBrush()`/`presetFromBrush()`), so the edit has to land
    // on `brush.model.tip.diameterPx` to actually reach the fork.
    brush.model.tip.diameterPx = 122.0f;  // radius 61
    BrushPreset forked = presetFromBrush(uniquePresetName(lib, lib.presets[0].name), brush);
    lib.presets.push_back(forked);
    lib.active = lib.presets.size() - 1;
    check(store.saveToFile(userPath, lib, &err) && err.empty(),
          "userbrushlib: Save-as-new on a built-in saves without error");

    check(countBuiltins(lib) == 4,
          "userbrushlib: the fork left the built-in COUNT unchanged");
    bool builtinsIntact = true;
    for (const BrushPreset& before : builtinsBefore)
      if (findByName(lib, before.name.c_str()) == nullptr ||
          findByName(lib, before.name.c_str())->model.tip.diameterPx != before.model.tip.diameterPx)
        builtinsIntact = false;
    check(builtinsIntact && lib.presets[0].model.tip.diameterPx == 40.0f,
          "userbrushlib: and every built-in's OWN preset is byte-for-byte what "
          "defaultBrushLibrary() shipped -- 'Round Bristle 03' itself is still radius 20, "
          "not the edited 61 that went into the fork");
    check(lib.presets.back().model.tip.diameterPx == 122.0f &&
              lib.presets.back().name != "Round Bristle 03",
          "userbrushlib: the fork carries the edit, under a name that is not the built-in's");

    UserBrushLibraryStore reload1;
    BrushState afterSave;
    check(reload1.loadFromFile(userPath, afterSave.brushLibrary, &err) &&
              findByName(afterSave.brushLibrary, forked.name.c_str()) != nullptr,
          "userbrushlib: the fork is on disk and comes back by its own name");

    // Delete the fork.
    const size_t forkIndex = lib.presets.size() - 1;
    lib.presets.erase(lib.presets.begin() + static_cast<std::ptrdiff_t>(forkIndex));
    lib.active = 0;
    check(store.saveToFile(userPath, lib, &err) && err.empty(),
          "userbrushlib: deleting the fork and saving again succeeds");
    check(countBuiltins(lib) == 4,
          "userbrushlib: the built-ins are still exactly four after the delete");

    UserBrushLibraryStore reload2;
    BrushState afterDelete;
    check(reload2.loadFromFile(userPath, afterDelete.brushLibrary, &err) &&
              findByName(afterDelete.brushLibrary, forked.name.c_str()) == nullptr &&
              countBuiltins(afterDelete.brushLibrary) == 4,
          "userbrushlib: reloading after the delete shows no trace of the removed preset, and "
          "still exactly the four built-ins -- Delete actually removes the file entry, it does "
          "not merely hide it in memory");

    // Save once more, a second fork.
    lib.active = 1;  // "Flat Wash"
    applyPresetToBrush(lib.presets[1], brush);
    brush.model.tip.hardness = 0.05f;  // see the comment above on why this
                                       // has to land on `.model` now
    BrushPreset secondFork = presetFromBrush(uniquePresetName(lib, lib.presets[1].name), brush);
    lib.presets.push_back(secondFork);
    lib.active = lib.presets.size() - 1;
    check(store.saveToFile(userPath, lib, &err) && err.empty(),
          "userbrushlib: saving a SECOND fork after the first was deleted succeeds");
    UserBrushLibraryStore reload3;
    BrushState afterSecond;
    check(reload3.loadFromFile(userPath, afterSecond.brushLibrary, &err) &&
              findByName(afterSecond.brushLibrary, secondFork.name.c_str()) != nullptr &&
              findByName(afterSecond.brushLibrary, forked.name.c_str()) == nullptr &&
              countBuiltins(afterSecond.brushLibrary) == 4,
          "userbrushlib: and the file now holds exactly the second fork -- not the first "
          "(deleted), not a duplicate, and still four untouched built-ins");
  }

  // ======================================================================
  // 4. Durability: an abandoned write cannot corrupt the real file, and a
  //    completed save actually goes through the temp file
  // ======================================================================
  //
  // **What this does and does not prove, stated plainly**: this process
  // cannot interrupt its own `fs::rename()` mid-syscall, so atomicity of the
  // rename itself is an OS/filesystem guarantee this test takes on faith,
  // exactly as `app/Journal.cpp`'s own comment does for the same call. What
  // IS provable, and is the property that actually matters: `loadFromFile()`
  // only ever reads `path`, never `path + ".tmp"` -- so a `.tmp` left behind
  // by a process that opened it, wrote to it, and died before the rename
  // (precisely what a crash mid-write leaves, per app/UserBrushLibrary.hpp
  // §4) cannot be mistaken for the real file, and the real file is left
  // exactly as the last COMPLETED save left it.
  //
  // **The stale `.tmp` seeded below, BEFORE the first save, is the other
  // half of that and is not optional.** A save into a settings directory
  // that has never held a `.tmp` before leaves none behind whether or not
  // `saveToFile()` goes through one on its way -- "no `.tmp` after a save
  // that never created one" is true of a plain `std::ofstream(path, trunc)`
  // too, so checking only that would pass a `saveToFile()` that skipped the
  // temp file entirely and wrote straight into the real path. Seeding one
  // first and checking it is GONE afterward is what actually exercises the
  // temp-then-rename path: the real implementation opens that same `.tmp`
  // name, overwrites it, and consumes it in the rename; an implementation
  // that writes straight into `path` never touches it and leaves it sitting
  // there.
  {
    BrushState brush;
    BrushLibrary& lib = brush.brushLibrary;
    lib.presets.push_back(makeAuthoredPreset(lib, "Durable Brush"));
    lib.active = lib.presets.size() - 1;

    {
      std::ofstream stale(userPath + ".tmp", std::ios::binary | std::ios::trunc);
      stale << "leftover from an unrelated earlier run";
    }
    check(fs::exists(userPath + ".tmp", ec), "userbrushlib: a stale `.tmp` is seeded first");

    UserBrushLibraryStore store;
    std::string err;
    check(store.saveToFile(userPath, lib, &err),
          "userbrushlib: a real save completes, establishing a known-good file");
    check(!fs::exists(userPath + ".tmp", ec),
          "userbrushlib: **and the save consumed the stale `.tmp`, not merely avoided leaving "
          "a NEW one** -- a save that instead writes straight into the real path would leave "
          "this exact file sitting here untouched");
    const std::string goodBytes = [&] {
      std::ifstream f(userPath, std::ios::binary);
      std::ostringstream b;
      b << f.rdbuf();
      return b.str();
    }();

    // Simulate the crash: a process opened the `.tmp`, wrote a truncated,
    // unrelated payload, and died before `fs::rename()`. Never call
    // saveToFile() again in this block -- that is the one call that would
    // legitimately replace the real file, and this test is about what
    // happens when it does NOT get to run to completion.
    {
      std::ofstream tmp(userPath + ".tmp", std::ios::binary | std::ios::trunc);
      tmp << "naturalPaint-user-presets 1\npreset Half-writ";
      // No trailing newline, no `scalars` line, no rename: exactly a write
      // that stopped partway.
    }
    check(fs::exists(userPath + ".tmp", ec), "userbrushlib: the abandoned `.tmp` exists");

    const std::string bytesAfterCrash = [&] {
      std::ifstream f(userPath, std::ios::binary);
      std::ostringstream b;
      b << f.rdbuf();
      return b.str();
    }();
    check(bytesAfterCrash == goodBytes,
          "userbrushlib: **the real file is byte-for-byte unchanged** by an abandoned `.tmp` "
          "sitting right beside it");

    BrushState reread;
    check(store.loadFromFile(userPath, reread.brushLibrary, &err) &&
              findByName(reread.brushLibrary, "Durable Brush") != nullptr &&
              findByName(reread.brushLibrary, "Half-writ") == nullptr,
          "userbrushlib: loading after the 'crash' reads the last GOOD save -- the half-written "
          "preset from the abandoned `.tmp` is not there, because loadFromFile() never reads "
          "a `.tmp` file at all");

    fs::remove(userPath + ".tmp", ec);
  }

  // ======================================================================
  // 5. A newer build's unknown lines survive a save by this one
  // ======================================================================
  {
    BrushState brush;
    BrushLibrary& lib = brush.brushLibrary;
    BrushPreset p = makeAuthoredPreset(lib, "Forward Compat");
    lib.presets.push_back(p);
    UserBrushLibraryStore store;
    const std::string base = store.serialize(lib);

    // A file-level unknown line, a header from a build numbered ahead of
    // this one, an unrecognised key inside the preset's scope, AND a `link`
    // whose ordinals this build's enums do not reach -- the forward-
    // compatible case app/UserBrushLibrary.hpp §2 exists to spell out,
    // distinct from a `link` that is simply corrupt (tested in section 6).
    std::string newer = base;
    newer.replace(0, newer.find('\n'), "naturalPaint-user-presets 99");
    const size_t presetAt = newer.find("preset Forward Compat");
    newer.insert(presetAt, "future-global-setting 1\n");
    const size_t scalarsEnd = newer.find('\n', newer.find("scalars", presetAt)) + 1;
    newer.insert(scalarsEnd, "future-preset-key abc\nlink 99 4 0.1 0.9 0 1\npoint 0 0\npoint 1 1\n");

    // Parsed into a SEPARATE, fresh library -- `lib` above already has a
    // preset named "Forward Compat" in it (pushed a few lines up to build
    // `base`), and parsing into the same one would run this fixture's
    // preset through `uniquePresetName()` and rename it, which would only
    // be testing that renaming works, not the forward-compat claim this
    // section is about.
    BrushState parsedInto;
    UserBrushLibraryStore fromNewer;
    fromNewer.parse(newer, parsedInto.brushLibrary);
    check(contains(newer, "naturalPaint-user-presets 99"),
          "userbrushlib: the hand-edited fixture really does carry a newer version number");
    const BrushPreset* readBack = findByName(parsedInto.brushLibrary, p.name.c_str());
    // `readBack->links` is the DEFAULT `BrushLinkSet` now, same as section
    // 1 above -- Part 4 made EVERY well-formed `link`/`floor` line preserve-
    // verbatim, not only the out-of-range one this fixture injects, so `p`'s
    // own two (in-range) links no longer come back live either.
    check(readBack != nullptr && readBack->links.links.empty(),
          "userbrushlib: a file from a newer build is READ, not refused -- and its own links, "
          "in-range or not, all take the preserve-verbatim path now, so `readBack->links` is "
          "empty rather than a reconstruction of `p.links`");
    check(fromNewer.unknownLines().size() == 1 &&
              contains(fromNewer.unknownLines()[0], "future-global-setting"),
          "userbrushlib: the file-level unknown line is preserved");
    // **Eleven lines, not four.** Before Part 4, only the out-of-range
    // `link 99 ...` block (this fixture's own injected 4 lines) took the
    // preserve-verbatim path -- `p`'s own two in-range links parsed live and
    // were invisible to `presetUnknownLines()`. Now every `link`/`point`
    // line in the preset's scope does, in file order: the injected block
    // first (`future-preset-key`, `link 99 ...`, its 2 points), then `p`'s
    // own `sizeLink` (1 link line + 3 points) and `offLink` (1 line + 2
    // points) -- 4 + 3 + 4 = 11.
    const auto it = fromNewer.presetUnknownLines().find(p.name);
    const std::vector<std::string> expectedLines = {
        "future-preset-key abc",
        "link 99 4 0.1 0.9 0 1",
        "point 0 0",
        "point 1 1",
        "link 0 0 0.123456791 1 0 1",
        "point 0 0",
        "point 0.400000006 0.142857149",
        "point 1 1",
        "link 1 1 -30 30 1 0",
        "point 0 0.200000003",  // 0.2f is not exact in binary32 either
        "point 1 0.899999976",  // -- same rounding as the rangeLo above
    };
    check(it != fromNewer.presetUnknownLines().end() && it->second == expectedLines,
          "userbrushlib: **the preset-scoped unknown key, the whole out-of-range `link` block "
          "(points included), AND `p`'s own two now-verbatim links are ALL preserved, in the "
          "file's own order** -- a future source or target ordinal this build cannot evaluate "
          "is not the same as a corrupt line, and must not be treated as one, and neither is "
          "an ordinary in-range link now that nothing reads it live");

    const std::string rewritten = fromNewer.serialize(parsedInto.brushLibrary);
    check(contains(rewritten, "future-global-setting 1") && contains(rewritten, "future-preset-key abc") &&
              contains(rewritten, "link 99 4 0.1 0.9 0 1") &&
              contains(rewritten, "link 0 0 0.123456791 1 0 1") &&
              contains(rewritten, "link 1 1 -30 30 1 0"),
          "userbrushlib: a save BY THIS BUILD still contains every one of those lines, `p`'s "
          "own two included -- the newer build's data is not erased by an older build "
          "round-tripping the file, and neither is this build's own preset's links, which now "
          "take the identical preserve-verbatim path");

    // **`fromNewer`, the SAME store that parsed `newer`, not a fresh one.**
    // `presetUnknownLines_` lives on the store instance
    // (`app/UserBrushLibrary.hpp`'s own class layout), not on the
    // `BrushLibrary` -- `MultiplyFloor.cpp`'s own §6 rewrite hit this same
    // trap first. A second cycle through a FRESH store, parsing
    // `rewritten`, is exactly what the check below already does correctly.
    UserBrushLibraryStore twice;
    BrushLibrary lib2;
    twice.parse(rewritten, lib2);
    check(twice.presetUnknownLines().find(p.name) != twice.presetUnknownLines().end() &&
              twice.presetUnknownLines().at(p.name).size() == 11,
          "userbrushlib: a second cycle is stable -- the preserved lines do not multiply");
  }

  // ======================================================================
  // 6. Malformed records are dropped, not half-read, and not preserved
  // ======================================================================
  {
    BrushLibrary lib;
    UserBrushLibraryStore genuinelyCorrupt;
    // `link` whose ordinals do not even parse -- corrupt, not "from the
    // future". Must be dropped outright, unlike section 5's out-of-range
    // (but well-formed) ordinal.
    genuinelyCorrupt.parse(
        "naturalPaint-user-presets 1\n"
        "preset Bad Link\n"
        "scalars 20 0.5 0.25 1 0 0.9 1.3\n"
        "link oops not numbers 0 1\n"
        "point 0 0\n"
        "preset Bad Scalars\n"
        "scalars 20 not-a-number\n"
        "preset Fine\n"
        "scalars 15 0.4 0.2 1 5 0.8 1.0\n",
        lib);
    const BrushPreset* badLink = findByName(lib, "Bad Link");
    const BrushPreset* badScalars = findByName(lib, "Bad Scalars");
    const BrushPreset* fine = findByName(lib, "Fine");
    check(badLink != nullptr && badLink->links.links.empty(),
          "userbrushlib: a preset with a `scalars` line that DID parse keeps its scalars even "
          "when a `link` line inside it did not -- the malformed link is dropped, not the "
          "whole preset");
    check(badScalars == nullptr,
          "userbrushlib: a preset whose `scalars` line does NOT parse is dropped WHOLE -- a "
          "half-populated preset (defaulted numbers nobody wrote) is worse than none");
    check(fine != nullptr && fine->model.tip.angleDeg == 5.0f,
          "userbrushlib: and the well-formed preset after two bad ones is unaffected -- one "
          "corrupt record costs only itself");
  }

  // ======================================================================
  // 7. The interaction with the `.abr` registry (app/BrushLibraryFile)
  // ======================================================================
  //
  // That module's own suite covers everything about ITS file; this asserts
  // only where the two touch: `BrushLibraryStore::resolveActive()` restores
  // the active preset by counting every preset with `libraryId == 0`, which
  // is true of a user preset as much as a built-in -- so loading order
  // matters, and this is the test that would redden if it stopped mattering.
  {
    // -- 7a. Correct order: user presets loaded before the `.abr` registry
    //    resolves `active` -- what ui/MacPaintUI.cpp's
    //    ensureUserBrushLibraryLoaded() is placed first to guarantee.
    BrushState session1;
    BrushLibrary& lib1 = session1.brushLibrary;
    BrushPreset mine = makeAuthoredPreset(lib1, "My Wash");
    lib1.presets.push_back(mine);
    lib1.active = lib1.presets.size() - 1;  // the user preset is what was active at quit

    UserBrushLibraryStore userStore;
    std::string err;
    check(userStore.saveToFile(userPath, lib1, &err), "userbrushlib: session 1 saves its brush");
    BrushLibraryStore abrStore;
    check(abrStore.saveToFile(abrPrefsPath, lib1, &err),
          "userbrushlib: session 1 also writes the (empty) `.abr` registry -- `active` is "
          "computed from the SAME `lib.presets`, so it names the user preset too");
    check(contains(abrStore.serialize(lib1), "active 0 4"),
          "userbrushlib: specifically as the 5th libraryId==0 preset (index 4) -- the "
          "registry's own counting scheme, unmodified, already reaches a user preset");

    BrushState session2;
    BrushLibrary& lib2 = session2.brushLibrary;
    UserBrushLibraryStore userStore2;
    BrushLibraryStore abrStore2;
    check(userStore2.loadFromFile(userPath, lib2, &err), "userbrushlib: relaunch: user presets load first");
    check(abrStore2.loadFromFile(abrPrefsPath, lib2, &err), "userbrushlib: then the `.abr` registry");
    check(lib2.active < lib2.presets.size() && lib2.presets[lib2.active].name == "My Wash" &&
              !lib2.presets[lib2.active].builtin,
          "userbrushlib: **loaded in the documented order, `active` correctly lands back on "
          "the user's own preset** -- restoring PRD G6's promise across a relaunch, not just "
          "across a single save/load pair");
    check(countBuiltins(lib2) == 4, "userbrushlib: and the built-ins are still exactly four");

    // -- 7b. The rejected order, run beside the chosen one (PLAN.md's own
    //    "run a rejected alternative and print both" pattern): loading the
    //    `.abr` registry FIRST sees only 4 built-ins when it counts, so
    //    `active` falls back to a built-in instead of the user's brush --
    //    which is exactly the bug ensureUserBrushLibraryLoaded()'s ordering
    //    comment exists to prevent, made visible here rather than argued in
    //    prose alone.
    BrushState session3;
    BrushLibrary& lib3 = session3.brushLibrary;
    BrushLibraryStore abrFirst;
    UserBrushLibraryStore userSecond;
    check(abrFirst.loadFromFile(abrPrefsPath, lib3, &err), "userbrushlib: wrong order: registry first");
    const size_t activeRightAfterAbr = lib3.active;
    check(userSecond.loadFromFile(userPath, lib3, &err), "userbrushlib: then user presets");
    check(lib3.active == activeRightAfterAbr,
          "userbrushlib: loading user presets AFTER never moves `active` on its own -- this "
          "module never writes `lib.active`, which is exactly why the order above is load-"
          "bearing rather than incidental");
    check(lib3.presets[lib3.active].builtin,
          "userbrushlib: **in the wrong order, `active` lands on a built-in, not 'My Wash'** -- "
          "the registry counted only 4 candidates when it resolved, so this comparison is "
          "capable of failing and only fails to fail because the real code path uses the "
          "other order");

    // -- 7c. No cross-contamination: each file only ever contains its own
    //    vocabulary.
    const std::string userBytes = userStore2.serialize(lib2);
    const std::string abrBytes = abrStore2.serialize(lib2);
    check(!contains(userBytes, "library ") && !contains(userBytes, "\nrow "),
          "userbrushlib: user-presets.txt never contains an `.abr` registry line");
    check(!contains(abrBytes, "\npreset ") && !contains(abrBytes, "\nscalars "),
          "userbrushlib: and brush-libraries.txt never contains a user-preset line -- saving "
          "one file never leaks the other's vocabulary into it");
  }

  // -- 8. The `dab` line: a sampled tip survives Duplicate -> Save -> relaunch
  //
  // brush/Library.hpp used to say, in its own words, that a saved duplicate of
  // a sampled-tip brush "reloads next launch as the round procedural tip",
  // because the format had no slot for a bitmap. It still has none -- what it
  // has now is a slot for an ID, pointing at a PNG app/DabLibrary wrote. This
  // is the format half of that; app/selftest/DabLibrary.cpp §G is the folder
  // half.
  {
    BrushLibrary lib;
    BrushPreset sampled;
    sampled.name = "Kyle's Marker";
    sampled.dabId = "abr:63d61f21-0000-4000-8000-bc81e4dfd608";
    lib.presets.push_back(sampled);
    BrushPreset plain;
    plain.name = "Plain Round";
    lib.presets.push_back(plain);

    UserBrushLibraryStore store;
    const std::string text = store.serialize(lib);
    check(contains(text, "dab abr:63d61f21-0000-4000-8000-bc81e4dfd608"),
          "userbrushlib: a preset with a sampled tip writes a `dab` line naming it");
    // Written only when there IS one. A preset with no sampled tip must come
    // out byte-identical to a file written before this key existed, or every
    // upgrade rewrites every preset for nothing.
    check(!contains(text, "dab \n") && !contains(text, "dab\n"),
          "userbrushlib: and a preset without one writes no empty `dab` line");

    BrushLibrary back;
    UserBrushLibraryStore reload;
    reload.parse(text, back);
    const BrushPreset* marker = nullptr;
    const BrushPreset* round = nullptr;
    for (const BrushPreset& p : back.presets) {
      if (p.name == "Kyle's Marker") marker = &p;
      if (p.name == "Plain Round") round = &p;
    }
    check(marker != nullptr && marker->dabId == sampled.dabId,
          "userbrushlib: the id round-trips through parse, which is the whole persistence");
    check(round != nullptr && round->dabId.empty(),
          "userbrushlib: and a preset that never had one still has none");

    // A file written by an older build has no `dab` line at all and must load
    // unchanged -- the reason this is a separate keyword and not an eighth
    // `scalars` field, exactly as `grain` is.
    BrushLibrary old;
    UserBrushLibraryStore oldStore;
    oldStore.parse(
        std::string(kUserPresetsFileHeader) + " 1\npreset Legacy\nscalars 20 0.5 0.25 1 0 1 0\n",
        old);
    check(old.presets.size() == 1 && old.presets[0].name == "Legacy" &&
              old.presets[0].dabId.empty() && old.presets[0].model.tip.diameterPx == 40.0f,
          "userbrushlib: a file written before this key existed loads with everything else intact");
  }

  // Restore the environment for whatever runs after this section.
  // =====================================================================
  std::printf("  -- the BrushModel survives the file --\n");
  // =====================================================================
  //
  // **This is the assertion Phase A exists for.** Before it, a `BrushModel`
  // was built by the importer, handed back on a vector parallel to the
  // presets, and destroyed when the import call returned -- so Duplicate on
  // an imported brush kept fourteen scalars and silently discarded the other
  // 137 leaves: the texture pattern, the dual tip's own scatter, the transfer
  // curves, the blend mode. The copy painted differently from the original
  // and nothing said so.
  //
  // The round trip is asserted with `brushModelDiff()` rather than a
  // hand-written field comparison, and that is not a convenience: a
  // comparison written here would be a SECOND enumeration of the 151 fields,
  // free to omit exactly the field the writer forgot -- the fork
  // brush/BrushModelFields.hpp exists to prevent. It also means a failure
  // names the field instead of saying "the model did not survive", which is
  // the difference between an actionable red and a puzzle.
  {
    UserBrushLibraryStore store;
    BrushLibrary lib;
    BrushPreset p;
    p.name = "model round trip";
    p.libraryId = 0;
    p.builtin = false;

    // Every leaf set to a DISTINCT non-default value, walked through the one
    // field list. Distinct matters: a fixture that set every float to 1.0f
    // would pass just as happily against a writer that swapped two
    // same-typed fields, which is the "what other implementation would also
    // pass this?" question every green here has to survive.
    int tick = 0;
    visitBrushModelFields(p.model, [&](const std::string&, auto& ref) {
      using T = std::decay_t<decltype(ref)>;
      ++tick;
      if constexpr (std::is_same_v<T, float>) ref = 0.5f + static_cast<float>(tick);
      else if constexpr (std::is_same_v<T, bool>) ref = true;
      else if constexpr (std::is_same_v<T, int32_t>) ref = tick;
      else if constexpr (std::is_same_v<T, std::string>) ref = "v" + std::to_string(tick);
      else if constexpr (std::is_same_v<T, VarianceControl>)
        ref = static_cast<VarianceControl>(tick % 8);
      else if constexpr (std::is_same_v<T, CoverageBlend>)
        ref = static_cast<CoverageBlend>(tick % 10);
    });
    check(tick == 151, "userbrushlib/model: the fixture touched all 151 leaves");
    check(!brushModelToLines(p.model).empty(),
          "userbrushlib/model: a fully-populated model writes lines at all");

    lib.presets.push_back(p);
    std::string err;
    check(store.saveToFile(userPath, lib, &err), "userbrushlib/model: saves");

    // **Read back through a FRESH store and a FRESH library**, never through
    // the objects that still hold the originals in memory -- an assertion
    // that reads back through the writer proves the writer can remember, not
    // that the file can carry.
    UserBrushLibraryStore reread;
    BrushLibrary lib2;
    check(reread.loadFromFile(userPath, lib2, &err), "userbrushlib/model: loads");

    const BrushPreset* got = nullptr;
    for (const BrushPreset& q : lib2.presets)
      if (q.name == "model round trip") got = &q;
    check(got != nullptr, "userbrushlib/model: the preset came back");

    if (got != nullptr) {
      const std::vector<std::string> differing = brushModelDiff(p.model, got->model);
      if (!differing.empty()) {
        std::printf("  [measured] %zu leaf/leaves did not survive, first few:\n",
                    differing.size());
        for (size_t i = 0; i < differing.size() && i < 6; ++i)
          std::printf("      %s\n", differing[i].c_str());
      }
      check(differing.empty(),
            "userbrushlib/model: all 151 leaves survive save -> load at zero tolerance");
      check(brushModelEqual(p.model, got->model),
            "userbrushlib/model: and brushModelEqual() agrees with the diff");
    }
  }

  // =====================================================================
  std::printf("  -- a model field this build does not know is KEPT --\n");
  // =====================================================================
  //
  // The forward-compatible case, which is the one a user meets by opening a
  // file a newer build wrote. An unknown path is preserved verbatim and
  // written back on save, so an older build cannot silently strip settings it
  // did not understand -- the same promise the `floor` branch already makes
  // for an out-of-range target ordinal.
  {
    const std::string fromTheFuture =
        std::string(kUserPresetsFileHeader) + " 1\n"
        "preset future brush\n"
        "scalars 21 0.3 0.25 1 0 0.9 1.3\n"
        "model tip.diameterPx 44\n"
        "model tip.thisFieldDoesNotExistYet 7\n";
    { std::ofstream f(userPath, std::ios::binary); f << fromTheFuture; }

    UserBrushLibraryStore store;
    BrushLibrary lib;
    std::string err;
    check(store.loadFromFile(userPath, lib, &err),
          "userbrushlib/future: a file with an unknown model path still loads");
    const BrushPreset* got = nullptr;
    for (const BrushPreset& q : lib.presets)
      if (q.name == "future brush") got = &q;
    check(got != nullptr && got->model.tip.diameterPx == 44.0f,
          "userbrushlib/future: the KNOWN field beside it was still read");

    check(store.saveToFile(userPath, lib, &err), "userbrushlib/future: saves again");
    std::string back;
    { std::ifstream f(userPath, std::ios::binary);
      back.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
    check(back.find("tip.thisFieldDoesNotExistYet 7") != std::string::npos,
          "userbrushlib/future: the unknown field is still in the file after a save");
  }

  if (savedUser.empty()) unsetenv("NP_USER_PRESETS");
  else setenv("NP_USER_PRESETS", savedUser.c_str(), 1);
  if (savedAbr.empty()) unsetenv("NP_BRUSH_LIBRARIES");
  else setenv("NP_BRUSH_LIBRARIES", savedAbr.c_str(), 1);
  fs::remove_all(root, ec);
  check(!fs::exists(root, ec), "userbrushlib: every file this section wrote is removed");

  std::printf("[selftest] user brush library %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
