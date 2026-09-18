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

#include "core/Document.hpp"
#include "io/NpaintFile.hpp"

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

  // ==========================================================================
  // (d) The thumbnail cache. Real .npaint files and real cache files: a hit
  // is proved by overwriting a source with garbage (same size, same mtime)
  // and still getting its picture back, so it can only have come from the
  // cache; a miss is proved by changing what the key covers.
  // ==========================================================================
  {
    const fs::path root = fs::temp_directory_path() / "np-gallery-cache-selftest";
    const fs::path docs = root / "docs";
    const fs::path cache = root / "cache";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(docs, ec);

    auto makeDoc = [&](const fs::path& p, float r, float g, float b) {
      Document doc = Document::createBlank(256, 256, WorkingSpace{});
      for (int32_t y = 0; y < 256; ++y) {
        for (int32_t x = 0; x < 256; ++x) {
          const PixelCoord at{x, y};
          doc.layers[0].rgbTiles->getOrCreate(tileCoordAt(at))
              .writePixel(tileLocalOffset(at), {r, g, b, 1.0f});
        }
      }
      return saveNpaint(doc, p.string()).ok;
    };
    auto same = [](const GalleryThumbnail& a, const GalleryThumbnail& b) {
      return !a.rgba.empty() && a.rgba == b.rgba && a.x == b.x && a.y == b.y && a.w == b.w &&
             a.h == b.h;
    };
    auto cacheFiles = [&]() {
      std::vector<fs::path> v;
      std::error_code e2;
      for (fs::directory_iterator it(cache, e2); !e2 && it != fs::directory_iterator();
           it.increment(e2)) {
        v.push_back(it->path());
      }
      return v;
    };
    auto byName = [](const std::vector<GalleryEntry>& es, const char* name) -> const GalleryEntry* {
      for (const GalleryEntry& e : es) {
        if (e.displayName == name) return &e;
      }
      return nullptr;
    };

    // Same name length on purpose, for the collision probe further down.
    const fs::path a = docs / "AAAA.npaint";
    const fs::path b = docs / "BBBB.npaint";
    const bool made = makeDoc(a, 0.9f, 0.1f, 0.1f) && makeDoc(b, 0.1f, 0.1f, 0.9f);
    check(made, "cache: two real .npaint files were written");

    GalleryScanStats st1;
    const auto cold = scanDocumentGallery(docs.string(), cache.string(), &st1);
    check(cold.size() == 2 && st1.decoded == 2 && st1.cacheHits == 0,
          "cache: a cold scan decodes every document and hits nothing");
    check(byName(cold, "AAAA") && !byName(cold, "AAAA")->thumb.rgba.empty() && byName(cold, "BBBB") &&
              !byName(cold, "BBBB")->thumb.rgba.empty(),
          "cache: the cold scan's thumbnails are real pictures");
    check(!same(byName(cold, "AAAA")->thumb, byName(cold, "BBBB")->thumb),
          "cache: the two documents' thumbnails differ (so 'same' below means something)");
    check(cacheFiles().size() == 2, "cache: the cold scan wrote one cache file per document");

    GalleryScanStats st2;
    const auto warm = scanDocumentGallery(docs.string(), cache.string(), &st2);
    check(st2.cacheHits == 2 && st2.decoded == 0, "cache: a warm scan decodes nothing");
    check(warm.size() == 2 && byName(warm, "AAAA") && byName(warm, "BBBB") &&
              same(byName(warm, "AAAA")->thumb, byName(cold, "AAAA")->thumb) &&
              same(byName(warm, "BBBB")->thumb, byName(cold, "BBBB")->thumb),
          "cache: the warm scan's thumbnails are byte-identical to the cold scan's");

    // The proof a hit came from the cache: the source is garbage now, with
    // its size and mtime restored, and the picture still comes back.
    {
      const auto mtime = fs::last_write_time(a, ec);
      const auto size = fs::file_size(a, ec);
      {
        std::ofstream f(a, std::ios::binary | std::ios::trunc);
        f << std::string(static_cast<size_t>(size), 'X');
      }
      fs::last_write_time(a, mtime, ec);
      GalleryScanStats st;
      const auto es = scanDocumentGallery(docs.string(), cache.string(), &st);
      check(st.cacheHits == 2 && st.decoded == 0 && byName(es, "AAAA") &&
                same(byName(es, "AAAA")->thumb, byName(cold, "AAAA")->thumb),
            "cache: a hit never opens the source (it is garbage, and the picture still comes back)");
    }

    // Changing what the key covers is a miss -- and the rebuilt picture is
    // the NEW document, not the cached one.
    {
      const bool remade = makeDoc(a, 0.1f, 0.9f, 0.1f);
      fs::last_write_time(a, fs::last_write_time(a, ec) + std::chrono::hours(1), ec);
      GalleryScanStats st;
      const auto es = scanDocumentGallery(docs.string(), cache.string(), &st);
      check(remade && st.decoded == 1 && st.cacheHits == 1, "cache: an edited document is a miss, the other still a hit");
      check(byName(es, "AAAA") && !byName(es, "AAAA")->thumb.rgba.empty() &&
                !same(byName(es, "AAAA")->thumb, byName(cold, "AAAA")->thumb),
            "cache: the edited document's tile is its new picture, not the stale one");
      GalleryScanStats st3;
      scanDocumentGallery(docs.string(), cache.string(), &st3);
      check(st3.cacheHits == 2 && st3.decoded == 0, "cache: and the rebuilt tile is itself cached");
    }

    // Same mtime, different size: a miss on size alone.
    {
      const auto mtime = fs::last_write_time(b, ec);
      {
        std::ofstream f(b, std::ios::binary | std::ios::app);
        f << "trailing";
      }
      fs::last_write_time(b, mtime, ec);
      GalleryScanStats st;
      scanDocumentGallery(docs.string(), cache.string(), &st);
      check(st.decoded == 1 && st.cacheHits == 1, "cache: a changed file size alone is a miss");
    }

    // Same size, new mtime: a miss on mtime alone (a re-save often keeps the
    // size, so the edit above proves nothing about this field).
    {
      fs::last_write_time(b, fs::last_write_time(b, ec) + std::chrono::minutes(1), ec);
      GalleryScanStats st;
      scanDocumentGallery(docs.string(), cache.string(), &st);
      check(st.decoded == 1 && st.cacheHits == 1, "cache: a changed mtime alone is a miss");
    }

    // Two documents with identical bytes AND identical mtime: every field of
    // the key matches except the path, so only the stored path can refuse the
    // other's cache file. The pictures are identical too, so the scan stats
    // are the only way to see it.
    {
      const fs::path c = docs / "CCCC.npaint";
      const fs::path d = docs / "DDDD.npaint";
      fs::copy_file(a, c, ec);
      fs::copy_file(a, d, ec);
      const auto t = fs::last_write_time(a, ec);
      fs::last_write_time(c, t, ec);
      fs::last_write_time(d, t, ec);
      scanDocumentGallery(docs.string(), cache.string());  // warm both
      fs::path cFile, dFile;
      for (const fs::path& f : cacheFiles()) {
        std::ifstream in(f, std::ios::binary);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (all.find(c.string()) != std::string::npos) cFile = f;
        if (all.find(d.string()) != std::string::npos) dFile = f;
      }
      if (!cFile.empty() && !dFile.empty()) {
        fs::copy_file(cFile, dFile, fs::copy_options::overwrite_existing, ec);
        GalleryScanStats st;
        scanDocumentGallery(docs.string(), cache.string(), &st);
        check(st.decoded == 1,
              "cache: a cache file differing only in its stored path is refused (hash collision)");
      } else {
        check(false, "cache: CCCC and DDDD each have a cache file");
      }
      fs::remove(c, ec);
      fs::remove(d, ec);
      scanDocumentGallery(docs.string(), cache.string());  // prune theirs
    }

    // A cache file for the wrong document: AAAA's file dropped under BBBB's
    // name. Same length path, so only the stored-path check can refuse it.
    {
      const auto files = cacheFiles();
      fs::path aFile, bFile;
      for (const fs::path& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (all.find(a.string()) != std::string::npos) aFile = f;
        if (all.find(b.string()) != std::string::npos) bFile = f;
      }
      check(!aFile.empty() && !bFile.empty() && aFile != bFile,
            "cache: each cache file records the path it was built for");
      if (!aFile.empty() && !bFile.empty()) {
        fs::copy_file(aFile, bFile, fs::copy_options::overwrite_existing, ec);
        GalleryScanStats st;
        const auto es = scanDocumentGallery(docs.string(), cache.string(), &st);
        check(st.decoded >= 1 && byName(es, "BBBB") &&
                  !same(byName(es, "BBBB")->thumb, byName(es, "AAAA")->thumb),
              "cache: another document's cache file under this name is refused, not shown");
      }
    }

    // Damage: truncated, and a flipped version byte. Each is a miss that
    // rewrites a good file.
    {
      for (const bool truncate : {true, false}) {
        const auto files = cacheFiles();
        check(!files.empty(), "cache: there is a cache file to damage");
        if (files.empty()) break;
        if (truncate) {
          fs::resize_file(files[0], fs::file_size(files[0], ec) / 2, ec);
        } else {
          std::fstream f(files[0], std::ios::binary | std::ios::in | std::ios::out);
          f.seekp(4);
          f.put('\x7f');
        }
        GalleryScanStats st;
        scanDocumentGallery(docs.string(), cache.string(), &st);
        check(st.decoded == 1 && st.cacheHits == 1,
              truncate ? "cache: a truncated cache file is a miss, not a crash or a bad picture"
                       : "cache: a wrong-version cache file is a miss");
        GalleryScanStats healed;
        scanDocumentGallery(docs.string(), cache.string(), &healed);
        check(healed.cacheHits == 2 && healed.decoded == 0,
              "cache: and the miss rewrote a good file");
      }
    }

    // Pruning: a deleted document's cache file goes, a stray .tmp goes, and a
    // file that is not ours stays.
    {
      {
        std::ofstream f(cache / "deadbeef00000000.npthumb.tmp", std::ios::binary);
        f << "x";
        std::ofstream g(cache / "notes.txt", std::ios::binary);
        g << "x";
      }
      fs::remove(b, ec);
      scanDocumentGallery(docs.string(), cache.string());
      size_t npthumbs = 0;
      bool tmpLeft = false, notesLeft = false;
      for (const fs::path& f : cacheFiles()) {
        const std::string n = f.filename().string();
        if (n == "notes.txt") notesLeft = true;
        else if (n.find(".tmp") != std::string::npos) tmpLeft = true;
        else if (n.size() > 8 && n.compare(n.size() - 8, 8, ".npthumb") == 0) ++npthumbs;
      }
      check(npthumbs == 1, "cache: a deleted document's cache file is pruned");
      check(!tmpLeft, "cache: a stray .tmp from an interrupted write is pruned");
      check(notesLeft, "cache: a file that is not a cache file is left alone");
    }

    // No cache directory: nothing is read or written.
    {
      const fs::path noCache = root / "never-made";
      GalleryScanStats st;
      const auto es = scanDocumentGallery(docs.string(), "", &st);
      check(es.size() == 1 && st.decoded == 1 && st.cacheHits == 0 && !fs::exists(noCache),
            "cache: an empty cache directory means every scan decodes and nothing is written");
    }

    // A missing source directory must not prune the cache: a transient
    // failure to list Documents must not throw away every tile.
    {
      const size_t before = cacheFiles().size();
      const auto es = scanDocumentGallery((root / "no-such-dir").string(), cache.string());
      check(es.empty() && cacheFiles().size() == before,
            "cache: an unreadable documents directory leaves the cache untouched");
    }

    fs::remove_all(root, ec);
  }

  return ok;
}

}  // namespace np
