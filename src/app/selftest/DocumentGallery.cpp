// --selftest section: the gallery's long-press actions (ui/DocumentGallery.hpp).
//
// `drawDocumentGallery()` itself is unreachable from here -- no ImGui frame,
// no GPU (docs/reachability-audit.md F4) -- so what this section can reach is
// the half of Rename / Duplicate / Delete that decides WHICH PATH is written:
// `galleryRenameRefusal()`, `galleryRenamedPath()` and
// `galleryDuplicatePath()`. That is deliberately where the damage lives. A
// rename is `fs::rename()`; the only way it can hurt is by being handed a
// destination outside the gallery folder, one that already exists, or one the
// scan will never list again. A duplicate is `fs::copy_file()`; the only way
// it can hurt is by being handed a destination that is already someone's
// document.
//
// The last section runs against a real temporary directory rather than
// against string arithmetic alone: `galleryDuplicatePath()`'s whole job is
// answering "is this taken?", and a test that never creates a file cannot
// tell a working existence check from one that always says no.

#include "app/selftest/Support.hpp"

#include "ui/DocumentGallery.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace np {

bool runDocumentGalleryTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ==========================================================================
  // (a) What a name may not be. Every one of these is a refusal because it
  // would either escape the gallery directory or produce a file the scan
  // cannot list.
  // ==========================================================================
  {
    check(galleryRenameRefusal("Sketch").empty(), "rename: an ordinary name is accepted");
    check(galleryRenameRefusal("  Sketch  ").empty(),
          "rename: surrounding blanks are trimmed, not refused");
    check(galleryRenameRefusal("Studies 2026-09-17").empty(),
          "rename: spaces and digits inside a name are accepted");

    check(!galleryRenameRefusal("").empty(), "rename: an empty name is refused");
    check(!galleryRenameRefusal("   ").empty(), "rename: an all-blank name is refused");
    // An INTERIOR separator, in a name no other rule would catch. The first
    // draft of these two used "../escape", which a sabotage of the separator
    // check still refused -- by the leading-dot rule, one line further down.
    // A probe that passes for the wrong reason is not a probe.
    check(!galleryRenameRefusal("sub/escape").empty(),
          "rename: a forward slash is refused (would write outside the gallery)");
    check(!galleryRenameRefusal("sub\\escape").empty(), "rename: a backslash is refused");
    check(!galleryRenameRefusal("../escape").empty(),
          "rename: a relative path is refused (leading dot AND separator)");
    check(!galleryRenameRefusal(".").empty(), "rename: \".\" is refused");
    check(!galleryRenameRefusal("..").empty(), "rename: \"..\" is refused");
    check(!galleryRenameRefusal(".hidden").empty(),
          "rename: a leading dot is refused (invisible to the Files app)");
    check(!galleryRenameRefusal(std::string(300, 'x')).empty(),
          "rename: a 300-character name is refused (255-byte component limit)");

    // The boundary itself, both sides. 248 is 255 less the seven bytes
    // ".npaint" costs -- a name that fits exactly must not be refused, and one
    // byte more must be.
    check(galleryRenameRefusal(std::string(248, 'x')).empty(),
          "rename: 248 characters -- exactly 255 with .npaint -- is accepted");
    check(!galleryRenameRefusal(std::string(249, 'x')).empty(),
          "rename: 249 characters is refused by one byte");
  }

  // ==========================================================================
  // (b) Where a rename lands. The parent directory is never the user's to
  // choose here, and the extension is exactly one `.npaint` however they
  // spelled it.
  // ==========================================================================
  {
    const std::string src = "/docs/Sketch.npaint";
    check(galleryRenamedPath(src, "Study") == "/docs/Study.npaint",
          "renamed path: same directory, new stem, .npaint appended");
    check(galleryRenamedPath(src, "  Study  ") == "/docs/Study.npaint",
          "renamed path: the name is trimmed before it is written");
    check(galleryRenamedPath(src, "Study.npaint") == "/docs/Study.npaint",
          "renamed path: a typed .npaint is not doubled");
    // A dot inside a name is a legitimate part of it ("v1.2 study"), and must
    // not be mistaken for an extension to strip.
    check(galleryRenamedPath(src, "v1.2 study") == "/docs/v1.2 study.npaint",
          "renamed path: an interior dot is kept, not read as an extension");
  }

  // ==========================================================================
  // (c) Duplicate, against a real directory -- the only way to test an
  // existence check.
  // ==========================================================================
  {
    const fs::path dir = fs::temp_directory_path() / "np-gallery-selftest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    auto touch = [](const fs::path& p) {
      std::ofstream f(p, std::ios::binary);
      f << "x";
    };

    const fs::path src = dir / "Sketch.npaint";
    touch(src);

    const std::string first = galleryDuplicatePath(src.string());
    check(first == (dir / "Sketch copy.npaint").string(),
          "duplicate: the first copy is \"<name> copy\"");

    touch(fs::path(first));
    const std::string second = galleryDuplicatePath(src.string());
    check(second == (dir / "Sketch copy 2.npaint").string(),
          "duplicate: with \"copy\" taken, the next is \"copy 2\" (not an overwrite)");

    touch(fs::path(second));
    check(galleryDuplicatePath(src.string()) == (dir / "Sketch copy 3.npaint").string(),
          "duplicate: numbering continues past 2");

    // Duplicating a duplicate: the stem is whatever the file is called, so
    // "Sketch copy" duplicates to "Sketch copy copy". Asserted because it is
    // the behaviour, not because it is elegant -- the alternative (parsing
    // the trailing " copy N" back off) is what would need a reason.
    check(galleryDuplicatePath(first) == (dir / "Sketch copy copy.npaint").string(),
          "duplicate: a duplicate's own duplicate appends a second \"copy\"");

    // Every candidate is a sibling of the source, never anywhere else.
    check(fs::path(first).parent_path() == dir && fs::path(second).parent_path() == dir,
          "duplicate: every candidate stays in the source's own directory");

    fs::remove_all(dir, ec);
  }

  return ok;
}

}  // namespace np
