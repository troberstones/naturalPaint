#include "app/BrushLibraryFile.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include "app/DabLibrary.hpp"
#include "io/AbrBrushes.hpp"

namespace np {
namespace {

namespace fs = std::filesystem;

// A float written so that reading it back gives the identical float.
//
// `%.9g` rather than `%f` or `%g`: nine significant decimal digits is the
// documented round-trip width for IEEE-754 binary32, and the row cache is
// compared with `brushRowsEqual()` at BIT equality. `%g`'s default six digits
// would turn 0.35 into 0.35 but 0.123456789 into 0.123457, and a cached row
// that differs from the preset it was made from by one ulp is a cache that
// reports itself invalid on every launch -- which looks exactly like a
// working cache that is never hit.
std::string f9(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
  return buf;
}

// Control characters out of anything that is going into a one-record-per-line
// file. A brush name arrives from a `.abr`'s UTF-16 string and can hold
// anything at all; one newline inside one brush name would split its row line
// in two and every line after it would be read as a different record.
//
// Replaced with a space rather than dropped, so a name does not silently lose
// the word boundary a stray character was standing in for.
std::string sanitizeOneLine(std::string s) {
  for (char& c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20u || u == 0x7fu) c = ' ';
  }
  return s;
}

bool hasControlCharacter(const std::string& s) {
  for (const char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20u || u == 0x7fu) return true;
  }
  return false;
}

std::string trimmedLine(const std::string& s) {
  size_t b = 0, e = s.size();
  // `\r` as well as space, so a preferences file that has been through a
  // Windows editor -- or a Windows checkout of a dotfiles repository -- parses
  // rather than producing a path whose last character is invisible.
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
    --e;
  return s.substr(b, e - b);
}

// Split `line` into its first whitespace-delimited word and the untrimmed
// remainder. The remainder keeps its interior spacing, because `library` and
// `row` both end in a field that is "the rest of the line".
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

// Pull `count` floats off the front of `text`, leaving the untouched remainder
// in `rest`. Returns false if fewer than `count` parse -- which is what makes a
// garbage `row` line drop exactly that row rather than half-populating one.
bool takeFloats(const std::string& text, int count, float* out, std::string& rest) {
  const char* p = text.c_str();
  for (int i = 0; i < count; ++i) {
    char* end = nullptr;
    // The C locale, because nothing in this application calls `setlocale()` --
    // so the decimal point is '.' both here and in the `%.9g` above. A build
    // that ever does call it would have to write and read through the same
    // locale or every cached row would come back as its integer part.
    const float v = std::strtof(p, &end);
    if (end == p) return false;
    out[i] = v;
    p = end;
  }
  while (*p == ' ') ++p;
  rest = p;
  return true;
}

struct StatResult {
  bool present = false;
  uint64_t size = 0;
  int64_t mtime = 0;
};

// `stat()` rather than `std::filesystem::last_write_time()`, which returns a
// `file_time_type` whose conversion to a wall clock is not portable in C++20
// (`clock_cast` for `file_clock` is not available on every standard library
// this builds against). Two integers is all the format stores and all the
// staleness check compares, and `stat()` gives both in one call.
StatResult statFile(const std::string& path) {
  StatResult r;
  struct ::stat st {};
  if (::stat(path.c_str(), &st) != 0) return r;
  // A directory is not a brush library. Reported as absent rather than as a
  // zero-byte file, because "it is a directory now" and "it is gone" send a
  // user looking in two different places -- app/DocumentLifecycle's
  // `recentDocumentMissing()` draws the same distinction.
  if ((st.st_mode & S_IFMT) == S_IFDIR) return r;
  r.present = true;
  r.size = static_cast<uint64_t>(st.st_size);
  r.mtime = static_cast<int64_t>(st.st_mtime);
  return r;
}

// Point `active` at the first preset no library owns -- `Round Bristle 03` on
// a stock install. The one place a dangling active index is repaired, so the
// three callers (a stale reload, an unload, an unresolvable `active` line)
// cannot disagree about where a lost selection lands.
//
// Never at a preset belonging to a library: falling back to *another* imported
// brush would mean removing one pack silently selects a brush from a different
// one, which is a stranger event than landing on the brush a fresh install
// starts on.
void clampActiveToBuiltIn(BrushLibrary& lib) {
  for (size_t i = 0; i < lib.presets.size(); ++i) {
    if (lib.presets[i].libraryId == 0) {
      lib.active = i;
      return;
    }
  }
  // No built-in at all is not reachable through this application --
  // `defaultBrushLibrary()` ships four and the pane's Delete refuses to remove
  // the last row -- but an index into an empty vector is worth not producing.
  lib.active = 0;
}

}  // namespace

// --- Rows -----------------------------------------------------------------

BrushRow brushRowFor(const BrushPreset& preset) {
  BrushRow r;
  r.name = preset.name;
  // Radius/hardness/roundness/angle/spacing now live on `preset.model`
  // (brush/Library.hpp's own comment on the five deleted `BrushPreset`
  // scalars) -- projected into the same units this row cache has always
  // held, so a row drawn from this cache looks exactly as it did before.
  r.radius = preset.model.tip.diameterPx / 2.0f;
  r.hardness = preset.model.tip.hardness;
  r.roundness = preset.model.tip.roundness;
  r.angle = preset.model.tip.angleDeg;
  // `BrushRow::spacing` is RADII (`ui/MacPaintUI.cpp`'s "%.2f r" row caption,
  // and the old, now-deleted `BrushPreset::spacing` scalar's own unit) --
  // `spacingPercent` is a percentage OF THE DIAMETER, so `/ 100 * 2` is the
  // conversion, not a bare `/ 100` (`app/StrokeSession::brushTipFor()`'s
  // `tip.spacing` comment names the same factor of two).
  r.spacing = preset.model.tip.spacingPercent / 100.0f * 2.0f;
  r.load = preset.load;
  r.linkCount = static_cast<uint32_t>(preset.links.links.size());
  // `wetness` is deliberately absent: nothing in a one-dab preview depends on
  // it (app/DabPreview's own §5 -- the deposit simulates no wetness at all),
  // and caching a value the row cannot draw would be caching a *parameter* of
  // the brush rather than the description of a row. A row that carried enough
  // to half-apply itself to the live brush is the state §5 of the header
  // rejects.
  return r;
}

bool brushRowsEqual(const BrushRow& a, const BrushRow& b) noexcept {
  return a.name == b.name && a.radius == b.radius && a.hardness == b.hardness &&
         a.roundness == b.roundness && a.angle == b.angle && a.spacing == b.spacing &&
         a.load == b.load && a.linkCount == b.linkCount;
}

// --- Status ---------------------------------------------------------------

const char* brushLibraryStatusName(BrushLibraryStatus status) noexcept {
  switch (status) {
    case BrushLibraryStatus::Remembered: return "not loaded";
    case BrushLibraryStatus::Loaded: return "loaded";
    case BrushLibraryStatus::Stale: return "changed on disk";
    case BrushLibraryStatus::Missing: return "missing";
    case BrushLibraryStatus::Failed: return "failed";
  }
  return "unknown";
}

std::string RememberedLibrary::displayName() const {
  const std::string name = fs::path(path).filename().string();
  return name.empty() ? path : name;
}

std::string defaultBrushLibraryFilePath() {
  // An explicit override first, so a test (or a second profile) never has to
  // touch the real one -- exactly `$NP_EXPORT_PRESETS` and
  // `$NP_RECENT_DOCUMENTS`, and the reason --selftest can run this section
  // without creating `~/Library/Application Support/naturalPaint`.
  if (const char* explicitPath = std::getenv("NP_BRUSH_LIBRARIES")) {
    if (*explicitPath != '\0') return explicitPath;
  }
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  if (home && *home)
    return std::string(home) + "/Library/Application Support/naturalPaint/brush-libraries.txt";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
    if (*xdg != '\0') return std::string(xdg) + "/naturalPaint/brush-libraries.txt";
  }
  if (home && *home) return std::string(home) + "/.config/naturalPaint/brush-libraries.txt";
#endif
  // No HOME at all (a stripped environment). The working directory is a poor
  // place for user settings, but it is a real, writable one, and silently
  // returning an empty path would turn "import a brush library" into something
  // that forgets every time.
  return "brush-libraries.txt";
}

// --- Parsing --------------------------------------------------------------

void BrushLibraryStore::parse(const std::string& text) {
  libraries_.clear();
  unknownLines_.clear();
  fileVersion_ = kBrushLibraryFileVersion;
  activeOrdinal_ = 0;
  activeIndex_ = 0;
  clearPendingActive();
  // Ids are not reused within a session even across a re-parse: a preset in
  // `BrushLibrary::presets` may still be carrying an id from before, and
  // handing that id to a different library is how unload deletes the wrong
  // brushes. `nextId_` therefore only ever goes up.

  std::istringstream in(text);
  std::string raw;
  bool sawHeader = false;
  bool firstLine = true;
  RememberedLibrary* current = nullptr;

  while (std::getline(in, raw)) {
    const std::string line = trimmedLine(raw);
    // A blank line is not data and is not an unknown line -- preserving them
    // would grow the file by one line per launch on any editor that leaves a
    // trailing newline.
    if (line.empty()) {
      firstLine = false;
      continue;
    }

    std::string key, rest;
    splitKey(line, key, rest);

    if (firstLine && key == kBrushLibraryFileHeader) {
      firstLine = false;
      sawHeader = true;
      const int v = std::atoi(rest.c_str());
      // A header with no number, or a nonsense one, leaves the version at ours
      // rather than at 0 -- the version is only ever used to *describe* the
      // file, never to gate reading it (§1), so a bad one must not read as
      // "written by version 0" in a diagnostic.
      if (v > 0) fileVersion_ = v;
      continue;
    }
    firstLine = false;

    if (key == "library") {
      // The path is the whole rest of the line: no quoting, no escaping, and
      // therefore no escaping bug. An empty one is dropped rather than
      // remembered, because a library with no path can never be loaded, shown
      // usefully, or unloaded by name.
      if (rest.empty()) {
        unknownLines_.push_back(line);
        continue;
      }
      RememberedLibrary entry;
      entry.id = nextId_++;
      entry.path = rest;
      libraries_.push_back(std::move(entry));
      current = &libraries_.back();
      continue;
    }

    if (key == "active") {
      float nums[2] = {0.0f, 0.0f};
      std::string tail;
      if (takeFloats(rest, 2, nums, tail) && nums[0] >= 0.0f && nums[1] >= 0.0f) {
        activeOrdinal_ = static_cast<int>(nums[0]);
        activeIndex_ = static_cast<size_t>(nums[1]);
      } else {
        unknownLines_.push_back(line);
      }
      continue;
    }

    if (current != nullptr) {
      if (key == "size") {
        current->size = static_cast<uint64_t>(std::strtoull(rest.c_str(), nullptr, 10));
        continue;
      }
      if (key == "mtime") {
        current->mtime = static_cast<int64_t>(std::strtoll(rest.c_str(), nullptr, 10));
        continue;
      }
      if (key == "row") {
        // Seven numbers then the name. **Frozen positionally** -- see the
        // header's §3 on why an eighth field is not how this format grows.
        float n[7] = {};
        std::string name;
        if (!takeFloats(rest, 7, n, name) || name.empty()) {
          // A malformed row loses that row and nothing else. It is not
          // promoted to an unknown line: an unknown line is re-emitted
          // verbatim, and re-emitting a row this build could not read would
          // keep a corrupt line alive in the file forever.
          continue;
        }
        BrushRow row;
        row.radius = n[0];
        row.hardness = n[1];
        row.roundness = n[2];
        row.angle = n[3];
        row.spacing = n[4];
        row.load = n[5];
        row.linkCount = n[6] > 0.0f ? static_cast<uint32_t>(n[6]) : 0u;
        row.name = name;
        current->rows.push_back(std::move(row));
        continue;
      }
      // §3: a key this version does not know, inside a library. Kept with that
      // library so it travels with it -- including out of the file when the
      // library is unloaded, which is right: it was that library's data.
      current->unknownLines.push_back(line);
      continue;
    }

    unknownLines_.push_back(line);
  }

  // A file with no header at all is still read -- it is what a hand-written
  // one looks like -- and is described as this version's, since there is
  // nothing else it could be.
  if (!sawHeader) fileVersion_ = kBrushLibraryFileVersion;
}

void BrushLibraryStore::refreshStatuses() {
  for (RememberedLibrary& entry : libraries_) {
    // A library read this session is not re-checked. Its presets are already
    // in the `BrushLibrary` and marking it stale would say something about
    // brushes the user can see are there.
    if (entry.status == BrushLibraryStatus::Loaded) continue;
    ++statCalls_;
    const StatResult st = statFile(entry.path);
    if (!st.present) {
      entry.status = BrushLibraryStatus::Missing;
      entry.failure = "'" + entry.path +
                      "' is not there -- it has been moved, renamed, deleted, or is on a "
                      "volume that is not mounted. Its brushes are listed from the last "
                      "time it was read.";
      continue;
    }
    // 0/0 is "the file recorded nothing", which a hand-edited preferences file
    // produces. Treated as stale rather than as a match: believing an
    // unrecorded size is how a cache stops being checkable at all.
    const bool sameFile = entry.size != 0 && st.size == entry.size && st.mtime == entry.mtime;
    entry.status = sameFile ? BrushLibraryStatus::Remembered : BrushLibraryStatus::Stale;
    entry.failure.clear();
  }
}

void BrushLibraryStore::resolveActive(BrushLibrary& lib) {
  const auto fallBackToBuiltIn = [&lib]() {
    clampActiveToBuiltIn(lib);
  };

  if (activeOrdinal_ <= 0) {
    // The Nth preset whose `libraryId` is 0 -- counted rather than taken as a
    // raw index into `presets`, so that loading or unloading a library, which
    // changes how many presets sit after the built-ins, cannot move it.
    size_t seen = 0;
    for (size_t i = 0; i < lib.presets.size(); ++i) {
      if (lib.presets[i].libraryId != 0) continue;
      if (seen == activeIndex_) {
        lib.active = i;
        return;
      }
      ++seen;
    }
    fallBackToBuiltIn();
    return;
  }

  const size_t ordinal = static_cast<size_t>(activeOrdinal_);
  if (ordinal > libraries_.size()) {
    // The file named a library that is no longer in it -- a hand edit, or a
    // file written by a version that ordered them differently. Nothing to
    // point at.
    fallBackToBuiltIn();
    return;
  }
  const RememberedLibrary& entry = libraries_[ordinal - 1];
  if (activeIndex_ >= entry.rows.size()) {
    fallBackToBuiltIn();
    return;
  }
  // §5: recorded, not restored. Restoring it would read this `.abr` during
  // startup, which is the single thing this module exists to prevent.
  pendingActiveLibrary_ = entry.id;
  pendingActiveRow_ = activeIndex_;
  fallBackToBuiltIn();
}

bool BrushLibraryStore::loadFromFile(const std::string& path, BrushLibrary& lib,
                                     std::string* errorOut) {
  if (errorOut) errorOut->clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    // Not an error: a fresh install has no preferences file, and reporting one
    // every launch would train the user to ignore the line that will one day
    // say something real.
    parse(std::string());
    resolveActive(lib);
    return true;
  }
  std::ostringstream buf;
  buf << f.rdbuf();
  const bool readOk = !f.bad();
  parse(buf.str());
  refreshStatuses();
  resolveActive(lib);
  if (!readOk && errorOut)
    *errorOut = "brush libraries: '" + path +
                "' could not be read to the end; what was readable has been kept.";
  return readOk;
}

// --- Writing --------------------------------------------------------------

std::string BrushLibraryStore::serialize(const BrushLibrary& lib) const {
  std::string out;
  out += kBrushLibraryFileHeader;
  out += " " + std::to_string(kBrushLibraryFileVersion) + "\n";

  // §3's preserved file-level lines, before the libraries, which is where they
  // were found.
  for (const std::string& line : unknownLines_) out += sanitizeOneLine(line) + "\n";

  // Which library owns the active preset, and its index within that library --
  // computed here, from the live `BrushLibrary`, rather than carried as state,
  // so it cannot describe a selection that has since moved.
  int activeOrdinal = 0;
  size_t activeIndex = 0;
  if (lib.active < lib.presets.size()) {
    const uint32_t owner = lib.presets[lib.active].libraryId;
    size_t within = 0;
    for (size_t i = 0; i < lib.active; ++i)
      if (lib.presets[i].libraryId == owner) ++within;
    activeIndex = within;
    if (owner != 0) {
      for (size_t i = 0; i < libraries_.size(); ++i) {
        if (libraries_[i].id == owner) {
          activeOrdinal = static_cast<int>(i) + 1;
          break;
        }
      }
      // An owner id with no library behind it cannot happen through this
      // module's own operations, but a caller that appended a preset with an
      // invented id would produce one. Written as built-in 0 rather than as a
      // dangling ordinal.
      if (activeOrdinal == 0) activeIndex = 0;
    }
  }

  for (const RememberedLibrary& entry : libraries_) {
    out += "library " + sanitizeOneLine(entry.path) + "\n";
    out += "size " + std::to_string(entry.size) + "\n";
    out += "mtime " + std::to_string(entry.mtime) + "\n";
    for (const BrushRow& row : entry.rows) {
      out += "row " + f9(row.radius) + " " + f9(row.hardness) + " " + f9(row.roundness) + " " +
             f9(row.angle) + " " + f9(row.spacing) + " " + f9(row.load) + " " +
             std::to_string(row.linkCount) + " " + sanitizeOneLine(row.name) + "\n";
    }
    for (const std::string& line : entry.unknownLines) out += sanitizeOneLine(line) + "\n";
  }

  out += "active " + std::to_string(activeOrdinal) + " " + std::to_string(activeIndex) + "\n";
  return out;
}

bool BrushLibraryStore::saveToFile(const std::string& path, const BrushLibrary& lib,
                                   std::string* errorOut) const {
  if (errorOut) errorOut->clear();
  std::error_code ec;
  const fs::path parent = fs::path(path).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);

  const std::string text = serialize(lib);
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    if (errorOut)
      *errorOut = "brush libraries: could not open '" + path +
                  "' for writing; the list of loaded libraries will not survive this session.";
    return false;
  }
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  f.close();
  if (!f) {
    if (errorOut)
      *errorOut = "brush libraries: '" + path + "' was opened but not fully written.";
    return false;
  }
  return true;
}

// --- Contents -------------------------------------------------------------

const RememberedLibrary* BrushLibraryStore::find(uint32_t id) const {
  for (const RememberedLibrary& entry : libraries_)
    if (entry.id == id) return &entry;
  return nullptr;
}

RememberedLibrary* BrushLibraryStore::findMutable(uint32_t id) {
  for (RememberedLibrary& entry : libraries_)
    if (entry.id == id) return &entry;
  return nullptr;
}

std::vector<BrushPaneRow> BrushLibraryStore::paneRows(const BrushLibrary& lib) const {
  std::vector<BrushPaneRow> out;
  out.reserve(lib.presets.size() + 8);

  // Everything that really exists first, in `presets` order: the four
  // built-ins, anything Duplicate made, then each loaded library's brushes in
  // the order they were appended.
  for (size_t i = 0; i < lib.presets.size(); ++i) {
    const BrushPreset& p = lib.presets[i];
    BrushPaneRow r;
    r.row = brushRowFor(p);
    r.libraryId = p.libraryId;
    r.presetIndex = i;
    size_t within = 0;
    for (size_t j = 0; j < i; ++j)
      if (lib.presets[j].libraryId == p.libraryId) ++within;
    r.rowIndex = within;
    if (p.libraryId != 0) {
      const RememberedLibrary* entry = find(p.libraryId);
      r.status = entry ? entry->status : BrushLibraryStatus::Loaded;
    }
    out.push_back(std::move(r));
  }

  // Then the cache: every library whose `.abr` has not been read. **This is
  // the whole of the lazy feature as the pane sees it** -- a row with no
  // preset index, drawn from seven numbers and a name.
  for (const RememberedLibrary& entry : libraries_) {
    if (entry.status == BrushLibraryStatus::Loaded) continue;
    for (size_t i = 0; i < entry.rows.size(); ++i) {
      BrushPaneRow r;
      r.row = entry.rows[i];
      r.libraryId = entry.id;
      r.rowIndex = i;
      r.presetIndex = kNoPresetIndex;
      r.status = entry.status;
      out.push_back(std::move(r));
    }
  }
  return out;
}

// --- Loading --------------------------------------------------------------

BrushLibraryLoadResult BrushLibraryStore::readInto(RememberedLibrary& entry, BrushLibrary& lib) {
  BrushLibraryLoadResult result;
  result.libraryId = entry.id;

  const StatResult st = statFile(entry.path);
  ++statCalls_;
  if (!st.present) {
    entry.status = BrushLibraryStatus::Missing;
    entry.failure = "'" + entry.path +
                    "' is not there -- it has been moved, renamed, deleted, or is on a volume "
                    "that is not mounted.";
    result.status = entry.failure;
    return result;
  }

  std::ifstream f(entry.path, std::ios::binary);
  if (!f) {
    entry.status = BrushLibraryStatus::Failed;
    entry.failure = "'" + entry.path + "' could not be opened for reading.";
    result.status = entry.failure;
    return result;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
  // Counted here, at the one place a `.abr` is actually opened and parsed, and
  // counted even when the parse then refuses -- a refused read still cost the
  // launch it happened during, which is what the counter is measuring.
  ++abrReads_;

  const AbrImportResult imported = importAbrBrushes(bytes);
  if (!imported.ok) {
    entry.status = BrushLibraryStatus::Failed;
    entry.failure = "'" + entry.path + "': " + imported.error;
    result.status = entry.failure;
    return result;
  }
  if (imported.presets.empty()) {
    // A `.abr` that parsed and holds nothing. Refused rather than recorded as
    // an empty library, which would draw as a header with no rows under it and
    // no way to tell from a library whose rows failed to cache.
    entry.status = BrushLibraryStatus::Failed;
    entry.failure = "'" + entry.path + "' parsed but contains no brushes.";
    result.status = entry.failure;
    return result;
  }

  // A reload of a library whose file changed replaces its presets wholesale.
  // Removing first, by id, means a pack that lost a brush does not leave the
  // twelfth one behind for the rest of the session.
  //
  // The active index is carried across the same way `unload()` does it, and
  // for the same reason: it is only counted survivors, and an index left alone
  // would point at whatever moved into the slot. The caller (the pane) sets
  // `active` to the row that was clicked immediately after this returns, so
  // this repair is what keeps the index legal in between rather than the final
  // word on it.
  size_t survivingBefore = 0;
  const bool activeWasHere =
      lib.active < lib.presets.size() && lib.presets[lib.active].libraryId == entry.id;
  if (!activeWasHere && lib.active < lib.presets.size()) {
    for (size_t i = 0; i < lib.active; ++i)
      if (lib.presets[i].libraryId != entry.id) ++survivingBefore;
  }
  lib.presets.erase(std::remove_if(lib.presets.begin(), lib.presets.end(),
                                   [&entry](const BrushPreset& p) {
                                     return p.libraryId == entry.id;
                                   }),
                    lib.presets.end());
  if (activeWasHere || survivingBefore >= lib.presets.size())
    clampActiveToBuiltIn(lib);
  else
    lib.active = survivingBefore;

  // Cleared HERE rather than beside `entry.status` below, because the tip
  // extraction just under this appends to it: a reload that cleared afterwards
  // would drop exactly the notes this reload produced, and one that never
  // cleared would accumulate them across every reload of the session.
  entry.notes.clear();

  // **The tips are written out here, once, at the one place a `.abr` is
  // actually parsed.** Until this, a sampled tip lived exactly as long as the
  // library stayed loaded (brush/Library.hpp's `tipBitmap`), so a duplicated
  // preset reloaded next launch as a round procedural one. Each tip becomes
  // `dabs-imported/<uuid>.png` and the preset's `dabId` names it, so the
  // bitmap stops depending on the pack: unload it, move it, delete it, the
  // brush still has its tip.
  //
  // Existing files are never rewritten, so re-importing the same pack -- or
  // importing two packs that share a tip -- costs nothing and cannot undo a
  // touch-up the user made in an image editor.
  if (!imported.tipSamples.empty()) {
    std::vector<std::pair<std::string, BrushTipBitmap>> tips;
    tips.reserve(imported.tipSamples.size());
    for (const AbrSampledTip& tip : imported.tipSamples)
      if (tip.bitmap != nullptr) tips.emplace_back(tip.id, *tip.bitmap);
    std::vector<std::string> extractNotes;
    extractAbrTips(dabImportedRootPath(), tips, &extractNotes);
    // A tip that could not be written is a note and not a failure: the
    // library still loaded, the brushes still paint this session, and only
    // the survives-a-relaunch half is lost. Refusing the whole import over it
    // would be a worse trade than saying so.
    for (const std::string& note : extractNotes) entry.notes.push_back(note);
  }

  entry.rows.clear();
  for (const BrushPreset& p : imported.presets) {
    BrushPreset added = p;
    added.libraryId = entry.id;
    // Two packs can both hold "Round 5", and the pane lists by name: two
    // identical rows cannot be told apart. `uniquePresetName()` is the
    // existing answer and is used rather than a second one.
    added.name = uniquePresetName(lib, added.name);
    entry.rows.push_back(brushRowFor(added));
    lib.presets.push_back(std::move(added));
  }

  entry.status = BrushLibraryStatus::Loaded;
  entry.failure.clear();
  entry.size = st.size;
  entry.mtime = st.mtime;
  for (const AbrImportNote& n : imported.notes)
    entry.notes.push_back(n.brushName + ": " + n.what);

  result.ok = true;
  result.presetsAdded = imported.presets.size();
  result.notes = entry.notes;
  result.status = entry.displayName() + ": " + std::to_string(imported.presets.size()) +
                  " brush" + (imported.presets.size() == 1 ? "" : "es") + " loaded";
  if (imported.sampledTips > 0) {
    // PRD G9. `io/AbrBrushes.cpp` now imports most sampled bitmap tips
    // (brush/Deposit.hpp §2c) -- this counter is the brushes it still could
    // NOT bring across (io/AbrBrushes.hpp's header names the three ways),
    // which is why the line only appears at all when that count is nonzero.
    // Said in the one line the pane shows by default, not only in the notes
    // nobody expands.
    result.status += " (" + std::to_string(imported.sampledTips) +
                     " with a bitmap tip that will paint round)";
  }
  return result;
}

BrushLibraryLoadResult BrushLibraryStore::importFile(const std::string& path,
                                                     BrushLibrary& lib) {
  BrushLibraryLoadResult result;
  const std::string trimmed = trimmedLine(path);
  if (trimmed.empty()) {
    result.status = "import refused: no path was given.";
    return result;
  }
  if (hasControlCharacter(trimmed)) {
    // The preferences file is one record per line and the path is the rest of
    // its line, so a path with a newline in it would split the record and make
    // every line after it read as something else. Refused at the point of
    // entry rather than sanitised, because a sanitised path names a different
    // file.
    result.status = "import refused: that path contains a control character, and the brush "
                    "library list is one path per line.";
    return result;
  }

  // Already imported? Re-importing the same path would produce two libraries
  // with the same brushes and two rows for every one of them, and unloading
  // "the" library would only take half of them away.
  for (RememberedLibrary& entry : libraries_) {
    if (entry.path != trimmed) continue;
    if (entry.status == BrushLibraryStatus::Loaded) {
      result.ok = true;
      result.libraryId = entry.id;
      result.status = entry.displayName() + " is already loaded (" +
                      std::to_string(entry.rows.size()) + " brushes).";
      return result;
    }
    // Remembered but unread: the import is exactly its first use.
    return readInto(entry, lib);
  }

  RememberedLibrary entry;
  entry.id = nextId_++;
  entry.path = trimmed;
  libraries_.push_back(std::move(entry));
  BrushLibraryLoadResult r = readInto(libraries_.back(), lib);
  if (!r.ok) {
    // A failed *import* leaves nothing behind. That is the difference between
    // an import and a first use: a use is retrying something the user already
    // chose to keep, and a failed import is a path they typed wrong -- keeping
    // it would put a permanently broken row in the pane and in the preferences
    // file for a typo.
    libraries_.pop_back();
  }
  return r;
}

BrushLibraryLoadResult BrushLibraryStore::useLibrary(uint32_t id, BrushLibrary& lib) {
  BrushLibraryLoadResult result;
  result.libraryId = id;
  if (id == 0) {
    // The built-ins. Nothing to load, and saying so is better than a silent
    // success that hides a caller which lost track of which row it is on.
    result.ok = true;
    result.status = "built-in brushes need no library.";
    return result;
  }
  RememberedLibrary* entry = findMutable(id);
  if (entry == nullptr) {
    result.status = "that brush library is no longer in the list.";
    return result;
  }
  if (entry->status == BrushLibraryStatus::Loaded) {
    // **The assertion that makes lazy loading testable**: a second use reads
    // nothing, so `abrReads()` does not move. A cache that quietly re-reads
    // passes every test that only checks the answer.
    result.ok = true;
    result.libraryId = id;
    result.status = entry->displayName() + " is loaded.";
    return result;
  }
  return readInto(*entry, lib);
}

// --- Unload ---------------------------------------------------------------

bool BrushLibraryStore::unload(uint32_t id, BrushLibrary& lib, std::string* messageOut) {
  const auto say = [messageOut](std::string s) {
    if (messageOut) *messageOut = std::move(s);
  };
  if (id == 0) {
    // Not a guard that could be forgotten: there is no library with id 0, so
    // this branch is unreachable through the pane, and the built-ins are safe
    // because of the id rather than because of the check.
    say("the built-in brushes are not a library and cannot be removed.");
    return false;
  }
  const size_t at = [&]() -> size_t {
    for (size_t i = 0; i < libraries_.size(); ++i)
      if (libraries_[i].id == id) return i;
    return static_cast<size_t>(-1);
  }();
  if (at == static_cast<size_t>(-1)) {
    say("that brush library is no longer in the list.");
    return false;
  }

  const std::string name = libraries_[at].displayName();
  const bool activeWasHere =
      lib.active < lib.presets.size() && lib.presets[lib.active].libraryId == id;

  // **Where the surviving active preset will land, computed BEFORE the erase.**
  //
  // Erasing shifts every later index down. An active index left as it was
  // would afterwards be pointing at whichever brush moved into that slot --
  // the user unloads a pack they were not using and the pane quietly claims
  // they are on a different brush, which is the exact silent change §6 exists
  // to prevent. Counting survivors ahead of it is the shift, exactly; matching
  // the preset back up by name afterwards would get two brushes called
  // "Round 5" wrong.
  size_t survivingBefore = 0;
  if (!activeWasHere && lib.active < lib.presets.size()) {
    for (size_t i = 0; i < lib.active; ++i)
      if (lib.presets[i].libraryId != id) ++survivingBefore;
  }

  const size_t before = lib.presets.size();
  lib.presets.erase(std::remove_if(lib.presets.begin(), lib.presets.end(),
                                   [id](const BrushPreset& p) { return p.libraryId == id; }),
                    lib.presets.end());
  const size_t removed = before - lib.presets.size();

  // Header §6. `lib.active` has to end up somewhere real, because the EDITED
  // badge is defined against `presets[active]`; the live brush is deliberately
  // NOT touched here, and this function could not touch it if it wanted to --
  // it takes no `BrushState`. The visible result is EDITED on a built-in,
  // which is exactly true of a brush that came from a pack that is gone.
  if (activeWasHere || survivingBefore >= lib.presets.size())
    clampActiveToBuiltIn(lib);
  else
    lib.active = survivingBefore;

  libraries_.erase(libraries_.begin() + static_cast<std::ptrdiff_t>(at));
  if (pendingActiveLibrary_ == id) clearPendingActive();

  say(name + ": " + std::to_string(removed) + " brush" + (removed == 1 ? "" : "es") +
      " removed. The library will not come back next launch.");
  return true;
}

}  // namespace np
