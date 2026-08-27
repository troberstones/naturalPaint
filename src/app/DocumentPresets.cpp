#include "app/DocumentPresets.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

// app/DocumentPresets -- implementation. Every design decision is argued in
// DocumentPresets.hpp; this file holds the mechanics.

namespace np {
namespace {

namespace fs = std::filesystem;

constexpr const char* kDocumentPresetsFileHeader = "naturalPaint-document-presets";
constexpr int kDocumentPresetsFileVersion = 1;

std::string trimmedLine(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
    --e;
  return s.substr(b, e - b);
}

std::string sanitizeOneLine(std::string s) {
  for (char& c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20u || u == 0x7fu) c = ' ';
  }
  return s;
}

void splitKey(const std::string& line, std::string& key, std::string& rest) {
  const size_t sp = line.find(' ');
  if (sp == std::string::npos) {
    key = line;
    rest.clear();
    return;
  }
  key = line.substr(0, sp);
  size_t at = sp;
  while (at < line.size() && line[at] == ' ') ++at;
  rest = line.substr(at);
}

// Pulls exactly two integers off the front of `text`. False if either fails
// to parse -- app/UserBrushLibrary.cpp's `takeFloats()` contract, restated
// for two ints: a record that does not fully parse is not half-populated.
bool takeTwoInts(const std::string& text, int32_t* a, int32_t* b) {
  const char* p = text.c_str();
  char* end = nullptr;
  const long v0 = std::strtol(p, &end, 10);
  if (end == p) return false;
  p = end;
  while (*p == ' ') ++p;
  const long v1 = std::strtol(p, &end, 10);
  if (end == p) return false;
  *a = static_cast<int32_t>(v0);
  *b = static_cast<int32_t>(v1);
  return true;
}

// §4 of app/UserBrushLibrary.hpp, reimplemented here for the identical
// reason that header gives: `fsync()`, not `fcntl(F_FULLFSYNC)`, covers a
// crashed process, a kill and a kernel panic -- everything this application
// can actually produce -- at a fraction of the cost of also covering sudden
// power loss.
void syncPath(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return;
  ::fsync(fd);
  ::close(fd);
}

// Write-to-temp-then-rename, matching app/UserBrushLibrary.cpp's
// `writeFileAtomically()` exactly in shape (itself matching
// app/Journal.cpp's `writeFileAtomically()`): a crash or a kill between the
// `ofstream` write and `fs::rename()` leaves `path` untouched.
bool writeFileAtomically(const std::string& path, const std::string& contents,
                         std::string* errorOut) {
  const std::string temp = path + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (errorOut) *errorOut = "document presets: could not open '" + temp + "' for writing.";
      return false;
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
    if (!out) {
      if (errorOut)
        *errorOut = "document presets: '" + temp + "' was opened but not fully written.";
      return false;
    }
  }
  syncPath(temp);
  std::error_code ec;
  fs::rename(temp, path, ec);
  if (ec) {
    if (errorOut)
      *errorOut =
          "document presets: could not rename '" + temp + "' into place (" + ec.message() + ").";
    fs::remove(temp, ec);
    return false;
  }
  return true;
}

}  // namespace

// --- Built-ins --------------------------------------------------------------

const std::vector<DocumentPreset>& builtinDocumentPresets() {
  static const std::vector<DocumentPreset> kBuiltins = {
      {"Web (1280 x 720)", 1280, 720, true},
      {"HD (1920 x 1080)", 1920, 1080, true},
      {"4K UHD (3840 x 2160)", 3840, 2160, true},
      {"Square (2048 x 2048)", 2048, 2048, true},
      {"US Letter @ 300dpi (2550 x 3300)", 2550, 3300, true},
      {"A4 @ 300dpi (2480 x 3508)", 2480, 3508, true},
  };
  return kBuiltins;
}

bool isBuiltinDocumentPresetName(const std::string& name) {
  for (const DocumentPreset& p : builtinDocumentPresets())
    if (p.name == name) return true;
  return false;
}

std::string uniqueDocumentPresetName(const std::string& candidate,
                                     const std::vector<DocumentPreset>& existingUser) {
  const auto taken = [&](const std::string& n) {
    if (isBuiltinDocumentPresetName(n)) return true;
    for (const DocumentPreset& p : existingUser)
      if (p.name == n) return true;
    return false;
  };
  if (!taken(candidate)) return candidate;
  for (int suffix = 2; suffix < 100000; ++suffix) {
    const std::string tryName = candidate + " " + std::to_string(suffix);
    if (!taken(tryName)) return tryName;
  }
  // Unreachable in practice (would need 100000 same-named presets); a
  // deterministic fallback rather than an infinite loop if it ever were.
  return candidate + " (copy)";
}

std::string validateDocumentPresetSize(int32_t width, int32_t height) {
  if (width <= 0 || height <= 0) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "a document preset's size must be positive on both axes (got %dx%d).", width,
                  height);
    return buf;
  }
  if (width > kMaxDocumentPresetDimension || height > kMaxDocumentPresetDimension) {
    char buf[224];
    std::snprintf(buf, sizeof(buf),
                  "a document preset's size cannot exceed %d px on either axis (got %dx%d).",
                  kMaxDocumentPresetDimension, width, height);
    return buf;
  }
  return {};
}

// --- Paths -------------------------------------------------------------------

std::string defaultDocumentPresetsFilePath() {
  if (const char* explicitPath = std::getenv("NP_DOCUMENT_PRESETS")) {
    if (*explicitPath != '\0') return explicitPath;
  }
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  if (home && *home)
    return std::string(home) + "/Library/Application Support/naturalPaint/document-presets.txt";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
    if (*xdg != '\0') return std::string(xdg) + "/naturalPaint/document-presets.txt";
  }
  if (home && *home) return std::string(home) + "/.config/naturalPaint/document-presets.txt";
#endif
  return "document-presets.txt";
}

// --- Parsing -----------------------------------------------------------------

void DocumentPresetStore::parse(const std::string& text) {
  presets_.clear();
  problems_.clear();
  unknownLines_.clear();

  std::vector<DocumentPreset> parsed;

  std::string pendingName;
  bool haveCurrent = false;
  bool haveSize = false;
  // Whether a `size` line was seen at all for the preset currently pending
  // (valid or not) -- distinguishes "never got a size line" (flush() reports
  // it below) from "got one and it was rejected" (already reported, with a
  // more specific reason, at the point the `size` line was read -- reporting
  // it again here would just be a vaguer duplicate of the same fact).
  bool sawSizeLine = false;
  int32_t pendingW = 0, pendingH = 0;
  size_t currentPresetLine = 0;

  const auto flush = [&]() {
    if (haveCurrent && haveSize) {
      DocumentPreset p;
      p.name = pendingName;
      p.width = pendingW;
      p.height = pendingH;
      p.builtin = false;
      parsed.push_back(std::move(p));
    } else if (haveCurrent && !sawSizeLine) {
      problems_.push_back("document-presets.txt:" + std::to_string(currentPresetLine) +
                          ": preset '" + pendingName +
                          "' has no 'size' line -- the preset was dropped.");
    }
    haveCurrent = false;
    haveSize = false;
    sawSizeLine = false;
    pendingName.clear();
    pendingW = pendingH = 0;
  };

  std::istringstream in(text);
  std::string raw;
  bool firstLine = true;
  size_t lineNumber = 0;

  while (std::getline(in, raw)) {
    ++lineNumber;
    const std::string line = trimmedLine(raw);
    if (line.empty()) {
      firstLine = false;
      continue;
    }

    std::string key, rest;
    splitKey(line, key, rest);

    if (firstLine && key == kDocumentPresetsFileHeader) {
      firstLine = false;
      continue;
    }
    firstLine = false;

    if (key == "preset") {
      flush();
      if (rest.empty()) {
        // No name: cannot be found again by name, so it is kept as a stray
        // line rather than as a preset nothing could ever address -- same
        // treatment app/UserBrushLibrary.cpp gives an unnamed `preset` line.
        unknownLines_.push_back(line);
        continue;
      }
      pendingName = rest;
      haveCurrent = true;
      haveSize = false;
      sawSizeLine = false;
      currentPresetLine = lineNumber;
      continue;
    }

    if (!haveCurrent) {
      // A line before any `preset` scope opened (or after one was rejected
      // above) -- preserved rather than dropped, same as app/
      // UserBrushLibrary.cpp's file-level unknown lines.
      unknownLines_.push_back(line);
      continue;
    }

    if (key == "size") {
      sawSizeLine = true;
      int32_t w = 0, h = 0;
      if (!takeTwoInts(rest, &w, &h)) {
        // Malformed -- not promoted to unknown. Re-emitting a `size` line
        // this build could not parse would keep a corrupt line alive in the
        // file forever (app/UserBrushLibrary.hpp §1's reasoning, restated).
        // `flush()` below drops the whole preset for lacking a valid size.
        problems_.push_back("document-presets.txt:" + std::to_string(lineNumber) +
                            ": preset '" + pendingName +
                            "' has a 'size' line that does not parse as two integers -- the "
                            "preset was dropped.");
        continue;
      }
      const std::string reason = validateDocumentPresetSize(w, h);
      if (!reason.empty()) {
        // Parses, but the numbers are not usable by Document::createBlank()
        // -- rejected here, at load, rather than reaching document creation
        // (this brief's own robustness rule). The preset is dropped exactly
        // as if the line had failed to parse at all.
        problems_.push_back("document-presets.txt:" + std::to_string(lineNumber) +
                            ": preset '" + pendingName + "' rejected: " + reason);
        continue;
      }
      pendingW = w;
      pendingH = h;
      haveSize = true;
      continue;
    }

    // A key this version does not know, inside a preset's scope. Preserved
    // at file level (§2 of the header: this format has no per-preset
    // sub-structure worth a second bookkeeping map yet).
    unknownLines_.push_back(line);
  }

  // flush() itself reports the "no size line at all" case (using
  // sawSizeLine, still holding whatever the last preset in the file left
  // it as); a `size` line that was seen but rejected already reported a
  // more specific reason at the point it was read.
  flush();

  // Appended last, and run through uniqueDocumentPresetName() against both
  // the built-ins and each other, in file order -- a hand-edited file that
  // collides with a built-in or repeats a name is disambiguated rather than
  // silently losing a preset or aliasing two of them (header §1).
  for (DocumentPreset& p : parsed) {
    p.name = uniqueDocumentPresetName(p.name, presets_);
    presets_.push_back(std::move(p));
  }
}

bool DocumentPresetStore::loadFromFile(const std::string& path, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    // Not an error: no document preset has ever been saved. The normal
    // first-run case, per this brief's robustness rules.
    parse(std::string());
    return true;
  }
  std::ostringstream buf;
  buf << f.rdbuf();
  const bool readOk = !f.bad();
  // A read that failed partway (`readOk == false`) still hands whatever was
  // readable to parse() -- a truncated file falls back to whichever presets
  // parsed cleanly out of the readable prefix rather than crashing or
  // half-applying anything past the truncation. If nothing readable made it
  // through, `presets_` ends up empty, which `allPresets()` already turns
  // into "just the built-ins."
  parse(buf.str());
  if (!readOk && errorOut)
    *errorOut = "document presets: '" + path +
                "' could not be read to the end; what was readable has been kept.";
  return readOk;
}

// --- Writing -----------------------------------------------------------------

std::string DocumentPresetStore::serialize() const {
  std::string out = kDocumentPresetsFileHeader;
  out += " " + std::to_string(kDocumentPresetsFileVersion) + "\n";
  for (const std::string& line : unknownLines_) out += sanitizeOneLine(line) + "\n";
  for (const DocumentPreset& p : presets_) {
    out += "preset " + sanitizeOneLine(p.name) + "\n";
    out += "size " + std::to_string(p.width) + " " + std::to_string(p.height) + "\n";
  }
  return out;
}

bool DocumentPresetStore::saveToFile(const std::string& path, std::string* errorOut) const {
  if (errorOut) errorOut->clear();
  std::error_code ec;
  const fs::path parent = fs::path(path).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);
  return writeFileAtomically(path, serialize(), errorOut);
}

std::vector<DocumentPreset> DocumentPresetStore::allPresets() const {
  std::vector<DocumentPreset> out = builtinDocumentPresets();
  out.insert(out.end(), presets_.begin(), presets_.end());
  return out;
}

bool DocumentPresetStore::add(const std::string& name, int32_t width, int32_t height,
                              std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (name.empty()) {
    if (errorOut) *errorOut = "a document preset needs a name.";
    return false;
  }
  const std::string sizeReason = validateDocumentPresetSize(width, height);
  if (!sizeReason.empty()) {
    if (errorOut) *errorOut = sizeReason;
    return false;
  }
  if (isBuiltinDocumentPresetName(name)) {
    if (errorOut)
      *errorOut = "'" + name +
                  "' is already the name of a built-in preset. Built-in presets cannot be "
                  "overwritten or shadowed -- choose a different name.";
    return false;
  }
  for (const DocumentPreset& p : presets_) {
    if (p.name == name) {
      if (errorOut)
        *errorOut = "'" + name + "' is already the name of one of your presets.";
      return false;
    }
  }
  DocumentPreset p;
  p.name = name;
  p.width = width;
  p.height = height;
  p.builtin = false;
  presets_.push_back(std::move(p));
  return true;
}

bool DocumentPresetStore::rename(const std::string& oldName, const std::string& newName,
                                 std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (newName.empty()) {
    if (errorOut) *errorOut = "a document preset needs a name.";
    return false;
  }
  if (isBuiltinDocumentPresetName(oldName)) {
    if (errorOut)
      *errorOut = "'" + oldName + "' is a built-in preset and cannot be renamed.";
    return false;
  }
  DocumentPreset* target = nullptr;
  for (DocumentPreset& p : presets_) {
    if (p.name == oldName) {
      target = &p;
      break;
    }
  }
  if (!target) {
    if (errorOut)
      *errorOut = "rename refused: there is no preset named '" + oldName + "'.";
    return false;
  }
  if (newName == oldName) return true;  // no-op, not a refusal
  if (isBuiltinDocumentPresetName(newName)) {
    if (errorOut)
      *errorOut = "'" + newName +
                  "' is already the name of a built-in preset -- choose a different name.";
    return false;
  }
  for (const DocumentPreset& p : presets_) {
    if (&p != target && p.name == newName) {
      if (errorOut)
        *errorOut = "'" + newName + "' is already the name of one of your presets.";
      return false;
    }
  }
  target->name = newName;
  return true;
}

bool DocumentPresetStore::remove(const std::string& name, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (isBuiltinDocumentPresetName(name)) {
    if (errorOut)
      *errorOut = "'" + name + "' is a built-in preset and cannot be deleted.";
    return false;
  }
  for (size_t i = 0; i < presets_.size(); ++i) {
    if (presets_[i].name == name) {
      presets_.erase(presets_.begin() + static_cast<std::ptrdiff_t>(i));
      return true;
    }
  }
  if (errorOut) *errorOut = "remove refused: there is no preset named '" + name + "'.";
  return false;
}

}  // namespace np
