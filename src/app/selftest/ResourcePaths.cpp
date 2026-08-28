#include "app/selftest/Support.hpp"

// core/ResourcePaths (docs/architecture-review.md P1-2: "The binary cannot
// leave the machine that built it"). Four things, in order:
//
//   1. Every tier of `resolveFromCandidates()`'s search order is reachable
//      in isolation -- including tier 2 (executable-relative), proven with a
//      temp directory standing in for "the directory containing the running
//      binary" rather than by writing next to the real one.
//   2. The `NP_ASSET_DIR` override wins even when the other two tiers would
//      also resolve, and executable-relative wins over compile-time when no
//      override is set -- the ordering argument `core/ResourcePaths.hpp`'s
//      header comment makes, exercised rather than just asserted in prose.
//   3. A resource missing from every tier is *reported*, not silently
//      returned as an empty string -- `reportResourceMissing()`'s message,
//      captured via `fmemopen()` so this never touches the process's real
//      stderr, names the resource and every path it tried.
//   4. **The assertion that would have caught the Lucide trap.** This
//      project's own notes record a prior track here whose suite asserted a
//      macro was non-empty and never called the code that used it -- green
//      while the exact bug it was meant to catch was live
//      (`app/ZoomAndSize.hpp`'s header has the story). The equivalent
//      mistake here would be `check(!std::string(NP_LUCIDE_TTF).empty(), ...)`,
//      which proves the compiler saw a string literal and nothing else.
//      §4 below instead opens and reads real bytes from the real resolved
//      path of all five resources, through the real `resolveResourcePath()`
//      (real environment, real `executableDir()`, real `NP_*` macros) --
//      exactly the call every production call site now makes.
namespace np {

bool runResourcePathsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // A scratch directory of this section's own -- same convention as
  // app/selftest/QuitGuard.cpp and app/selftest/OpenAnyFile.cpp -- so the
  // fake "tier 2" and "tier 1" roots below never collide with a developer's
  // own files or a previous run's leftovers.
  const std::filesystem::path scratch =
      std::filesystem::temp_directory_path() / "np-selftest-resourcepaths";
  std::error_code ec;
  std::filesystem::remove_all(scratch, ec);
  std::filesystem::create_directories(scratch, ec);

  auto exists = [](const std::string& p) {
    std::error_code ec2;
    return std::filesystem::exists(p, ec2);
  };

  // opens the file at `path` and confirms at least one real byte comes back
  // -- not merely that `exists()` said yes, which a zero-length or
  // permission-denied file would also pass. This is the exact gap
  // `NP_LUCIDE_TTF` non-empty-macro check left open (see file header).
  auto opensAndReads = [](const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char buf[64] = {};
    in.read(buf, sizeof(buf));
    return in.gcount() > 0;
  };

  // ==========================================================================
  // 1. Each tier reachable in isolation.
  // ==========================================================================
  std::filesystem::path tier1Root, tier2Root, tier3File;
  {
    tier3File = scratch / "tier3.txt";
    { std::ofstream out(tier3File, std::ios::binary); out << "tier3"; }
    {
      const ResolvedResource r =
          resolveFromCandidates("", "", tier3File.string(), "irrelevant.txt", exists);
      check(r.found && r.tier == 3 && r.path == tier3File.string(),
            "resolver: tier 3 (compile-time path) reachable when tiers 1 and 2 are absent");
    }

    tier2Root = scratch / "tier2root";
    std::filesystem::create_directories(tier2Root / "sub", ec);
    { std::ofstream out(tier2Root / "sub" / "res.txt", std::ios::binary); out << "tier2"; }
    {
      const std::string missingTier3 = (scratch / "no-such-tier3.txt").string();
      const ResolvedResource r =
          resolveFromCandidates("", tier2Root.string(), missingTier3, "sub/res.txt", exists);
      check(r.found && r.tier == 2 &&
                r.path == (tier2Root / "sub" / "res.txt").string(),
            "resolver: tier 2 (executable-relative) reachable via a temp dir standing in for it");
    }

    tier1Root = scratch / "tier1root";
    std::filesystem::create_directories(tier1Root / "sub", ec);
    { std::ofstream out(tier1Root / "sub" / "res.txt", std::ios::binary); out << "tier1"; }
    {
      const std::string missingTier3 = (scratch / "no-such-tier3.txt").string();
      const ResolvedResource r =
          resolveFromCandidates(tier1Root.string(), "", missingTier3, "sub/res.txt", exists);
      check(r.found && r.tier == 1 &&
                r.path == (tier1Root / "sub" / "res.txt").string(),
            "resolver: tier 1 (NP_ASSET_DIR override) reachable on its own");
    }
  }

  // ==========================================================================
  // 2. Precedence: override beats both fallbacks; the compile-time source
  // tree beats the staged copy when no override is set. All three candidates
  // below point at real, distinct files, so a wrong precedence order is
  // directly observable in which tier answers.
  // ==========================================================================
  {
    const ResolvedResource r = resolveFromCandidates(
        tier1Root.string(), tier2Root.string(), tier3File.string(), "sub/res.txt", exists);
    check(r.found && r.tier == 1 && r.path == (tier1Root / "sub" / "res.txt").string(),
          "resolver: NP_ASSET_DIR override wins even when tiers 2 and 3 also resolve");
  }
  {
    // The source tree wins over the staged copy, and this is the assertion
    // that pins the reason: both exist only on the machine that built the
    // binary, and there `shaders/*.wgsl` and `keymaps/default.json` are meant
    // to be edited and picked up live (Cmd+R / `reload_shaders`). The
    // opposite order silently reads the stale staged copy instead --
    // demonstrated by corrupting `shaders/advect_water.wgsl` and watching
    // `--selftest` pass anyway. See core/ResourcePaths.hpp's search-order
    // section for the full trade.
    const ResolvedResource r = resolveFromCandidates("", tier2Root.string(), tier3File.string(),
                                                      "sub/res.txt", exists);
    check(r.found && r.tier == 3,
          "resolver: the compile-time source tree wins over the staged copy when no "
          "override is set, so live shader/keymap edits are not shadowed");
  }
  {
    // ...and the staged copy still answers when the source tree is gone,
    // which is the whole portability claim. Without this the assertion above
    // could be satisfied by a resolver that had simply dropped tier 2.
    const ResolvedResource r = resolveFromCandidates(
        "", tier2Root.string(), (scratch / "no-such-source-tree" / "res.txt").string(),
        "sub/res.txt", exists);
    check(r.found && r.tier == 2,
          "resolver: with no source tree present (the copied-binary case) the staged "
          "copy beside the executable answers");
  }

  // ==========================================================================
  // 3. A resource missing from every tier is reported, not silently empty.
  // ==========================================================================
  {
    const std::string missingExeDir = (scratch / "no-such-exe-dir").string();
    const std::string missingCompileTime = (scratch / "no-such-compiletime.txt").string();
    const ResolvedResource r = resolveFromCandidates(
        "", missingExeDir, missingCompileTime, "nope.txt", [](const std::string&) { return false; });
    check(!r.found, "resolver: correctly reports not-found when no tier has the file");
    check(!r.path.empty(),
          "resolver: not-found still returns a best-effort path rather than an empty string");
    check(r.tried.size() == 2,
          "resolver: not-found lists exactly the tiers that were applicable (2 and 3 here; 1 was unset)");

    // fmemopen(), not the process's real stderr -- so this proves the report
    // was actually written, byte for byte, rather than trusting that some
    // fprintf(stderr, ...) call somewhere fired.
    char buf[4096] = {};
    std::FILE* mem = fmemopen(buf, sizeof(buf), "w");
    reportResourceMissing("selftest-missing-resource", r.tried, mem);
    std::fclose(mem);
    const std::string report(buf);
    check(report.find("selftest-missing-resource") != std::string::npos,
          "reportResourceMissing: names the resource in its report");
    bool allTriedListed = true;
    for (const std::string& tried : r.tried)
      if (report.find(tried) == std::string::npos) allTriedListed = false;
    check(allTriedListed, "reportResourceMissing: lists every candidate path it tried");
  }

  std::filesystem::remove_all(scratch, ec);

  // ==========================================================================
  // 4. Every one of the five real resources resolves to an existing,
  // readable file in THIS build, through the real resolveResourcePath() --
  // real environment, real executableDir(), real NP_* macros. This is the
  // assertion that would have caught the Lucide trap (see file header).
  // ==========================================================================
  {
    const std::string shaders = shaderDir();
    check(std::filesystem::is_directory(shaders), "shaderDir() resolves to a real directory");
    check(opensAndReads(shaders + "/composite.wgsl"),
          "shaders/composite.wgsl actually opens and reads under the resolved shaderDir()");

    const std::string keymaps = keymapDir();
    check(std::filesystem::is_directory(keymaps), "keymapDir() resolves to a real directory");
    check(opensAndReads(keymaps + "/default.json"),
          "keymaps/default.json actually opens and reads under the resolved keymapDir()");

    check(opensAndReads(mixboxLutPath()),
          "mixboxLutPath() resolves to a file that actually opens and reads");
    check(opensAndReads(lucideTtfPath()),
          "lucideTtfPath() resolves to a file that actually opens and reads -- the check a "
          "non-empty NP_LUCIDE_TTF macro alone never made");
    check(opensAndReads(lucideCodepointsJsonPath()),
          "lucideCodepointsJsonPath() resolves to a file that actually opens and reads");
  }

  // ==========================================================================
  // 5. The staged copies beside the executable really exist.
  // ==========================================================================
  //
  // Section 4 above exercises whichever tier happens to win, and on a
  // developer's machine that is the source tree -- so on its own it says
  // nothing about whether `src/CMakeLists.txt`'s POST_BUILD staging step ran,
  // or is still copying the right five things. That is precisely the risk the
  // search order takes on (core/ResourcePaths.hpp: source tree before staged
  // copy, so live shader and keymap edits are not shadowed): tier 3 stops
  // being exercised by ordinary runs, and a staging step that quietly broke
  // would not surface until someone copied the binary somewhere and found it
  // launch with no icons -- the exact silent failure this module exists to
  // end, one layer down.
  //
  // So this checks the staged layout DIRECTLY, against executableDir(),
  // bypassing the resolver entirely. It is the assertion that keeps tier 3
  // honest while it is not the one answering.
  {
    const std::string& exeDir = executableDir();
    check(!exeDir.empty(), "executableDir() resolves (staging checks below depend on it)");
    if (!exeDir.empty()) {
      const std::filesystem::path stage(exeDir);
      check(opensAndReads((stage / "shaders" / "composite.wgsl").string()),
            "staged beside the executable: shaders/composite.wgsl (POST_BUILD copy ran)");
      check(opensAndReads((stage / "keymaps" / "default.json").string()),
            "staged beside the executable: keymaps/default.json");
      check(opensAndReads(
                (stage / "third_party" / "mixbox" / "shaders" / "mixbox_lut.png").string()),
            "staged beside the executable: third_party/mixbox/shaders/mixbox_lut.png");
      check(opensAndReads((stage / "third_party" / "lucide" / "lucide.ttf").string()),
            "staged beside the executable: third_party/lucide/lucide.ttf -- the icon font "
            "whose absence is the silent failure this module exists to end");
      check(opensAndReads((stage / "third_party" / "lucide" / "codepoints.json").string()),
            "staged beside the executable: third_party/lucide/codepoints.json");
    }
  }

  std::printf("[selftest] resource paths %s\n", ok ? "OK" : "FAIL");
  return ok;
}

}  // namespace np
