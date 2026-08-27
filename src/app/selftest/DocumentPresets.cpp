#include "app/selftest/Support.hpp"

#include "app/DocumentPresets.hpp"

namespace np {
namespace {

bool contains(const std::string& s, const char* needle) {
  return s.find(needle) != std::string::npos;
}

const DocumentPreset* findByName(const std::vector<DocumentPreset>& presets, const char* name) {
  for (const DocumentPreset& p : presets)
    if (p.name == name) return &p;
  return nullptr;
}

}  // namespace

// app/DocumentPresets: the sizes File > New offers (docs/testing-issues.md
// T9, piece 1). Everything under app/DocumentPresets.hpp's own header
// comment is argued there; this proves it.
bool runDocumentPresetsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  namespace fs = std::filesystem;
  std::error_code ec;

  // Everything below lives here; $NP_DOCUMENT_PRESETS points into it, so
  // nothing touches ~/Library/Application Support/naturalPaint. Same
  // mechanism, same reason, as app/selftest/UserBrushLibrary.cpp's own root.
  const std::string root = "selftest_documentpresets";
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  const std::string path = root + "/document-presets.txt";

  const char* prevEnv = std::getenv("NP_DOCUMENT_PRESETS");
  const std::string savedEnv = prevEnv ? prevEnv : "";
  setenv("NP_DOCUMENT_PRESETS", path.c_str(), 1);
  check(defaultDocumentPresetsFilePath() == path,
        "docpresets: $NP_DOCUMENT_PRESETS overrides the settings path, so this section cannot "
        "touch the real one");

  // ==========================================================================
  // 1. The built-in set is well-formed and actually consumable by
  //    Document::createBlank()
  // ==========================================================================
  {
    const std::vector<DocumentPreset>& builtins = builtinDocumentPresets();
    check(!builtins.empty(), "docpresets: there is at least one built-in preset");
    bool allBuiltin = true;
    bool allValidSize = true;
    for (const DocumentPreset& p : builtins) {
      if (!p.builtin) allBuiltin = false;
      if (!validateDocumentPresetSize(p.width, p.height).empty()) allValidSize = false;
    }
    check(allBuiltin, "docpresets: every entry in builtinDocumentPresets() is tagged builtin");
    check(allValidSize,
          "docpresets: every built-in's size passes validateDocumentPresetSize() -- the same "
          "check a hand-edited file's 'size' line is held to");
    bool noDuplicateNames = true;
    for (size_t i = 0; i < builtins.size(); ++i)
      for (size_t j = i + 1; j < builtins.size(); ++j)
        if (builtins[i].name == builtins[j].name) noDuplicateNames = false;
    check(noDuplicateNames, "docpresets: no two built-ins share a name");

    // The actual claim T9 is about: each built-in's width/height really is
    // what Document::createBlank() -- the entire new-document path this
    // header was written against -- produces a document at.
    bool everyBuiltinCreates = true;
    for (const DocumentPreset& p : builtins) {
      const Document doc = Document::createBlank(p.width, p.height, WorkingSpace{});
      if (doc.width != p.width || doc.height != p.height) everyBuiltinCreates = false;
    }
    check(everyBuiltinCreates,
          "docpresets: every built-in, fed straight to Document::createBlank(), produces a "
          "document at exactly that preset's width and height");
  }

  // ==========================================================================
  // 2. Empty store: allPresets() is just the built-ins, add/rename/remove
  //    round trip, persisted
  // ==========================================================================
  {
    DocumentPresetStore store;
    std::string err;
    check(store.loadFromFile(path, &err) && err.empty(),
          "docpresets: loading a file that has never been written is not an error -- the normal "
          "first-run case");
    check(store.userPresets().empty(),
          "docpresets: and it leaves zero user presets, not a crash and not a half-populated "
          "list");
    check(store.allPresets().size() == builtinDocumentPresets().size(),
          "docpresets: allPresets() on a fresh store is exactly the built-ins -- 'fall back to "
          "the built-ins' needs no separate code path from 'nothing has been saved yet'");

    check(store.add("My Poster", 3000, 4000, &err) && err.empty(),
          "docpresets: add() accepts a positive, in-range size under a fresh name");
    check(store.userPresets().size() == 1 && store.userPresets()[0].name == "My Poster" &&
              !store.userPresets()[0].builtin,
          "docpresets: the new preset is in userPresets(), tagged non-builtin");
    check(store.saveToFile(path, &err) && err.empty(), "docpresets: saveToFile() succeeds");

    DocumentPresetStore reloaded;
    check(reloaded.loadFromFile(path, &err) && err.empty() &&
              findByName(reloaded.userPresets(), "My Poster") != nullptr &&
              findByName(reloaded.userPresets(), "My Poster")->width == 3000 &&
              findByName(reloaded.userPresets(), "My Poster")->height == 4000,
          "docpresets: reloading a fresh store recovers the saved preset's exact size");

    check(store.rename("My Poster", "Gallery Wrap", &err) && err.empty(),
          "docpresets: rename() on a user preset succeeds");
    check(findByName(store.userPresets(), "My Poster") == nullptr &&
              findByName(store.userPresets(), "Gallery Wrap") != nullptr,
          "docpresets: the old name is gone and the new one is present");
    check(store.remove("Gallery Wrap", &err) && err.empty(),
          "docpresets: remove() on a user preset succeeds");
    check(store.userPresets().empty(), "docpresets: and the store is empty again");
    check(!store.remove("Gallery Wrap", &err) && !err.empty(),
          "docpresets: remove() on a name that is no longer there refuses, named");
  }

  // ==========================================================================
  // 3. Built-ins are not editable, not deletable, and cannot be duplicated
  //    into the user file
  // ==========================================================================
  {
    DocumentPresetStore store;
    std::string err;
    const std::string builtinName = builtinDocumentPresets().front().name;

    check(!store.add(builtinName, 100, 100, &err) && contains(err, "built-in"),
          "docpresets: add() refuses a name that exactly matches a built-in, named");
    check(store.userPresets().empty(),
          "docpresets: the refused add left no trace in userPresets()");

    check(!store.remove(builtinName, &err) && contains(err, "built-in"),
          "docpresets: remove() refuses a built-in's name, named");

    check(store.add("Renameable", 500, 500, &err),
          "docpresets: (setup) a real user preset to attempt renaming a built-in over");
    check(!store.rename(builtinName, "Doesn't Matter", &err) && contains(err, "built-in"),
          "docpresets: rename() refuses when the OLD name is a built-in -- built-ins cannot be "
          "renamed");
    check(!store.rename("Renameable", builtinName, &err) && contains(err, "built-in"),
          "docpresets: rename() also refuses when the NEW name would collide with a built-in");
    check(findByName(store.userPresets(), "Renameable") != nullptr &&
              findByName(store.userPresets(), "Renameable")->width == 500,
          "docpresets: the refused rename left 'Renameable' untouched");

    // "Must not be duplicated into the user file": the only way a built-in's
    // name could end up in document-presets.txt is via serialize() walking
    // userPresets(), and every path that could have put it there (add(),
    // rename()) was just proven to refuse. Checked directly here too.
    check(store.saveToFile(path, &err) && !contains(store.serialize(), builtinName.c_str()),
          "docpresets: the built-in's name never appears in serialize()'s own output");
  }

  // ==========================================================================
  // 4. Distinguishability: a hand-edited file colliding with a built-in is
  //    silently disambiguated at parse() (unlike the interactive add()/
  //    rename() above, which refuse outright)
  // ==========================================================================
  {
    const std::string builtinName = builtinDocumentPresets().front().name;
    DocumentPresetStore fromFile;
    fromFile.parse("naturalPaint-document-presets 1\n"
                   "preset " +
                   builtinName +
                   "\n"
                   "size 111 222\n");
    check(fromFile.userPresets().size() == 1 && fromFile.userPresets()[0].name != builtinName,
          "docpresets: a hand-edited user preset sharing a built-in's exact name comes back "
          "under a DIFFERENT name -- built-in and user preset can never carry the same name "
          "at the same time");
    check(contains(fromFile.userPresets()[0].name, builtinName.c_str()),
          "docpresets: the disambiguated name is still recognisably derived from the original "
          "(uniqueDocumentPresetName()'s ' 2', ' 3', ... suffix)");
  }

  // ==========================================================================
  // 5. Robustness: a malformed 'size' line drops only its own preset
  // ==========================================================================
  {
    DocumentPresetStore store;
    store.parse("naturalPaint-document-presets 1\n"
                "preset First\n"
                "size 800 600\n"
                "preset Sabotaged\n"
                "size not-a-number also-not\n"
                "preset Third\n"
                "size 1200 900\n");
    check(findByName(store.userPresets(), "First") != nullptr &&
              findByName(store.userPresets(), "Third") != nullptr,
          "docpresets: the presets before and after the sabotaged one both survive");
    check(findByName(store.userPresets(), "Sabotaged") == nullptr,
          "docpresets: the preset with the malformed 'size' line is dropped WHOLE -- not kept "
          "with a defaulted size nobody wrote");
    check(store.userPresets().size() == 2,
          "docpresets: exactly two presets survive a three-preset file with one sabotaged");
    bool sawMalformedProblem = false;
    for (const std::string& p : store.problems())
      if (contains(p, "Sabotaged")) sawMalformedProblem = true;
    check(sawMalformedProblem,
          "docpresets: the drop is reported in problems(), naming the preset it happened to");
  }

  // ==========================================================================
  // 6. Robustness: zero/negative/absurd sizes are rejected AT LOAD, never
  //    reaching a value Document::createBlank() would receive
  // ==========================================================================
  {
    DocumentPresetStore store;
    store.parse("naturalPaint-document-presets 1\n"
                "preset Zero\n"
                "size 0 600\n"
                "preset Negative\n"
                "size 800 -1\n"
                "preset Absurd\n"
                "size 999999999 900\n"
                "preset Fine\n"
                "size 800 600\n");
    check(findByName(store.userPresets(), "Zero") == nullptr &&
              findByName(store.userPresets(), "Negative") == nullptr &&
              findByName(store.userPresets(), "Absurd") == nullptr,
          "docpresets: a zero, a negative, and an absurdly large size are all rejected at "
          "load -- none of the three reaches userPresets()");
    check(findByName(store.userPresets(), "Fine") != nullptr,
          "docpresets: the well-formed preset after three rejected ones is unaffected");
    check(store.problems().size() == 3,
          "docpresets: each of the three rejections is reported, individually, in problems()");

    // The same rule from the interactive side: validateDocumentPresetSize()
    // is the ONE function both paths call (header §0), so this is really the
    // same claim proven twice, not a second implementation trusted on faith.
    std::string err;
    check(!store.add("Bad", 0, 100, &err) && !err.empty(),
          "docpresets: add() refuses a zero width the same way parse() does");
    check(!store.add("Bad", 100, -5, &err) && !err.empty(),
          "docpresets: add() refuses a negative height");
    check(!store.add("Bad", kMaxDocumentPresetDimension + 1, 100, &err) && !err.empty(),
          "docpresets: add() refuses a size one past the documented maximum");
    check(store.add("Ok At The Limit", kMaxDocumentPresetDimension, kMaxDocumentPresetDimension,
                    &err),
          "docpresets: add() accepts a size AT the documented maximum -- the limit is inclusive, "
          "not off-by-one");
  }

  // ==========================================================================
  // 7. Robustness: a truncated/garbage file falls back to the built-ins,
  //    never crashes, never half-applies
  // ==========================================================================
  {
    DocumentPresetStore store;
    store.parse("this is not the file format at all\n\xff\xfe\x00garbage\nsize\npreset\n");
    check(store.userPresets().empty(),
          "docpresets: parsing pure garbage yields zero user presets, not a crash");
    check(store.allPresets().size() == builtinDocumentPresets().size(),
          "docpresets: allPresets() on a garbage file is exactly the built-ins -- the same "
          "fallback a missing file gets");

    // Truncated mid-record: a real preset followed by a preset scope that
    // never gets to its 'size' line before EOF.
    DocumentPresetStore truncated;
    truncated.parse("naturalPaint-document-presets 1\n"
                    "preset Complete\n"
                    "size 400 400\n"
                    "preset Cut Off Before Its Size Line\n");
    check(findByName(truncated.userPresets(), "Complete") != nullptr &&
              truncated.userPresets().size() == 1,
          "docpresets: a file truncated mid-preset keeps every preset that finished before the "
          "cut, and drops only the incomplete tail -- not the whole file");
  }

  // ==========================================================================
  // 8. Durability: an abandoned '.tmp' cannot corrupt the real file
  // ==========================================================================
  //
  // Same proof app/selftest/UserBrushLibrary.cpp makes for its own atomic
  // writer, restated for this one: what is provable from inside this process
  // is that loadFromFile() only ever reads `path`, never `path + ".tmp"`, so
  // a `.tmp` a crashed process left behind cannot be mistaken for the real
  // file.
  {
    DocumentPresetStore store;
    std::string err;
    check(store.add("Durable", 640, 480, &err), "docpresets: (setup) one real user preset");

    {
      std::ofstream stale(path + ".tmp", std::ios::binary | std::ios::trunc);
      stale << "leftover from an unrelated earlier run";
    }
    check(fs::exists(path + ".tmp", ec), "docpresets: a stale '.tmp' is seeded first");

    check(store.saveToFile(path, &err),
          "docpresets: a real save completes, establishing a known-good file");
    check(!fs::exists(path + ".tmp", ec),
          "docpresets: **the save consumed the stale '.tmp'**, not merely avoided leaving a new "
          "one -- a save that instead wrote straight into the real path would leave this exact "
          "file sitting here untouched");
    const std::string goodBytes = [&] {
      std::ifstream f(path, std::ios::binary);
      std::ostringstream b;
      b << f.rdbuf();
      return b.str();
    }();

    // Simulate the crash: a process opened the `.tmp`, wrote a truncated,
    // unrelated payload, and died before fs::rename(). saveToFile() is never
    // called again in this block.
    {
      std::ofstream tmp(path + ".tmp", std::ios::binary | std::ios::trunc);
      tmp << "naturalPaint-document-presets 1\npreset Half-writ";
    }
    const std::string bytesAfterCrash = [&] {
      std::ifstream f(path, std::ios::binary);
      std::ostringstream b;
      b << f.rdbuf();
      return b.str();
    }();
    check(bytesAfterCrash == goodBytes,
          "docpresets: **the real file is byte-for-byte unchanged** by an abandoned '.tmp' "
          "sitting right beside it");

    DocumentPresetStore reread;
    check(reread.loadFromFile(path, &err) &&
              findByName(reread.userPresets(), "Durable") != nullptr &&
              findByName(reread.userPresets(), "Half-writ") == nullptr,
          "docpresets: loading after the 'crash' reads the last GOOD save -- loadFromFile() "
          "never reads a '.tmp' file at all");
    fs::remove(path + ".tmp", ec);
  }

  // Restore the environment for whatever runs after this section.
  if (savedEnv.empty()) unsetenv("NP_DOCUMENT_PRESETS");
  else setenv("NP_DOCUMENT_PRESETS", savedEnv.c_str(), 1);
  fs::remove_all(root, ec);
  check(!fs::exists(root, ec), "docpresets: every file this section wrote is removed");

  std::printf("[selftest] document presets %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
