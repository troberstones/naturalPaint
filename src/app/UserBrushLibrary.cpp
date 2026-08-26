#include "app/UserBrushLibrary.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace np {
namespace {

namespace fs = std::filesystem;

// A float written so that reading it back gives the identical float. Same
// width and the same reasoning as app/BrushLibraryFile.cpp's own `f9()`: nine
// significant digits is IEEE-754 binary32's documented round-trip width, and
// this module's whole reason to exist is a round trip that must be exact, not
// merely close.
std::string f9(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
  return buf;
}

// Control characters out of a name or a preserved line -- app/
// BrushLibraryFile.cpp's `sanitizeOneLine()`, copied rather than shared for
// the same reason `defaultUserPresetsFilePath()` re-derives its directory
// instead of calling into that module: a one-line function is not worth a
// dependency between two files whose lifetimes and durability contracts this
// module's own header (§0, §4) argues should stay separate.
std::string sanitizeOneLine(std::string s) {
  for (char& c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20u || u == 0x7fu) c = ' ';
  }
  return s;
}

std::string trimmedLine(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
    --e;
  return s.substr(b, e - b);
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

// Pull `count` floats off the front of `text`. Returns false if fewer than
// `count` parse -- app/BrushLibraryFile.cpp's `takeFloats()`, same contract:
// a record that does not fully parse is not half-populated.
bool takeFloats(const std::string& text, int count, float* out) {
  const char* p = text.c_str();
  for (int i = 0; i < count; ++i) {
    char* end = nullptr;
    // The C locale -- see app/BrushLibraryFile.cpp's own note at its
    // `takeFloats()`: nothing in this application calls `setlocale()`, so
    // '.' is the decimal point both here and in `f9()` above.
    const float v = std::strtof(p, &end);
    if (end == p) return false;
    out[i] = v;
    p = end;
  }
  return true;
}

// §4 of app/UserBrushLibrary.hpp: `fsync()`, not `fcntl(F_FULLFSYNC)`, for
// the measured trade-off app/Journal.cpp's own `syncPath()` documents at
// length (fsync covers a crashed process, a kill and a kernel panic --
// everything this application can actually produce -- at roughly a tenth the
// cost of also covering sudden power loss). Reimplemented rather than called:
// `Journal.cpp`'s version has internal linkage.
void syncPath(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return;
  ::fsync(fd);
  ::close(fd);
}

// Write-to-temp-then-rename, matching `app/Journal.cpp`'s
// `writeFileAtomically()` exactly in shape: a crash or a kill between the
// `ofstream` write and the `fs::rename()` leaves `path` untouched, because a
// rename is atomic on the same filesystem -- a reader only ever sees the
// whole old file or the whole new one.
bool writeFileAtomically(const std::string& path, const std::string& contents,
                         std::string* errorOut) {
  const std::string temp = path + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (errorOut) *errorOut = "user brushes: could not open '" + temp + "' for writing.";
      return false;
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
    if (!out) {
      if (errorOut) *errorOut = "user brushes: '" + temp + "' was opened but not fully written.";
      return false;
    }
  }
  syncPath(temp);
  std::error_code ec;
  fs::rename(temp, path, ec);
  if (ec) {
    if (errorOut)
      *errorOut =
          "user brushes: could not rename '" + temp + "' into place (" + ec.message() + ").";
    fs::remove(temp, ec);
    return false;
  }
  return true;
}

}  // namespace

std::string defaultUserPresetsFilePath() {
  // Same override-first shape as `defaultBrushLibraryFilePath()`, so a test
  // (or a second profile) never touches the real settings directory.
  if (const char* explicitPath = std::getenv("NP_USER_PRESETS")) {
    if (*explicitPath != '\0') return explicitPath;
  }
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  if (home && *home)
    return std::string(home) + "/Library/Application Support/naturalPaint/user-presets.txt";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
    if (*xdg != '\0') return std::string(xdg) + "/naturalPaint/user-presets.txt";
  }
  if (home && *home) return std::string(home) + "/.config/naturalPaint/user-presets.txt";
#endif
  return "user-presets.txt";
}

// --- Parsing ----------------------------------------------------------------

void UserBrushLibraryStore::parse(const std::string& text, BrushLibrary& lib) {
  unknownLines_.clear();
  presetUnknownLines_.clear();

  std::vector<BrushPreset> parsed;

  // State for the preset currently being read. `haveScalars` is what decides,
  // at the next `preset` line or at EOF, whether `pending` is kept or
  // dropped whole -- app/BrushLibraryFile.hpp §3's "not promoted... would
  // keep a corrupt line alive forever", applied to a whole preset rather
  // than one row: a `preset` block with no readable `scalars` line is not a
  // brush with defaulted numbers, it is a record this build could not read.
  BrushPreset pending;
  bool haveCurrent = false;
  bool haveScalars = false;
  std::vector<std::string> pendingUnknown;

  // Where a `point` line belongs, decided by the most recent `link` line:
  //   ActiveLink   -- append to `pending.links.back().curve`.
  //   PreserveBlock -- append verbatim to `pendingUnknown` (§2's
  //                    forward-compatible ordinal case).
  //   None         -- an orphaned `point` (hand edit, or after a malformed
  //                    `link`); dropped.
  enum class PointMode { None, ActiveLink, PreserveBlock };
  PointMode pointMode = PointMode::None;

  const auto flush = [&]() {
    if (haveCurrent && haveScalars) {
      parsed.push_back(pending);
      if (!pendingUnknown.empty()) presetUnknownLines_[pending.name] = pendingUnknown;
    }
    // A preset with no valid `scalars` line is dropped in full, including
    // whatever unknown lines it had collected -- they described a record
    // that no longer exists.
    pending = BrushPreset{};
    haveCurrent = false;
    haveScalars = false;
    pendingUnknown.clear();
    pointMode = PointMode::None;
  };

  std::istringstream in(text);
  std::string raw;
  bool firstLine = true;

  while (std::getline(in, raw)) {
    const std::string line = trimmedLine(raw);
    if (line.empty()) {
      firstLine = false;
      continue;
    }

    std::string key, rest;
    splitKey(line, key, rest);

    if (firstLine && key == kUserPresetsFileHeader) {
      firstLine = false;
      continue;
    }
    firstLine = false;

    if (key == "preset") {
      flush();
      if (rest.empty()) {
        // No name: cannot be found again by `serialize()`'s name-keyed
        // lookup and cannot be Saved or Deleted by name, so it is kept as a
        // stray line rather than as a preset nobody could ever address.
        unknownLines_.push_back(line);
        continue;
      }
      pending = BrushPreset{};
      pending.name = rest;
      haveCurrent = true;
      haveScalars = false;
      pointMode = PointMode::None;
      continue;
    }

    if (!haveCurrent) {
      // Nothing has opened a preset scope yet (or the last `preset` line was
      // rejected above) -- a file-level unknown line, same treatment as
      // app/BrushLibraryFile.hpp §3 gives a line before the first `library`.
      unknownLines_.push_back(line);
      continue;
    }

    if (key == "scalars") {
      float n[7];
      if (takeFloats(rest, 7, n)) {
        pending.radius = n[0];
        pending.hardness = n[1];
        pending.spacing = n[2];
        pending.roundness = n[3];
        pending.angle = n[4];
        pending.load = n[5];
        pending.wetness = n[6];
        haveScalars = true;
      }
      // A malformed `scalars` line is not promoted to unknown -- see
      // `flush()`: without it the whole preset is dropped, exactly like a
      // `row` with too few numbers is dropped rather than half-read.
      pointMode = PointMode::None;
      continue;
    }

    if (key == "grain") {
      // A SEPARATE keyword rather than an eighth `scalars` field --
      // `BrushPreset::grain`'s own comment gives the reason: growing
      // `scalars`' required count would make a FILE WRITTEN BEFORE this field
      // existed (seven floats, no eighth) fail `takeFloats(rest, 7, ...)`'s
      // exact-count parse and drop the whole preset. A new keyword an older
      // reader of THIS file does not know is instead caught by the "a key
      // this version does not know" branch at the bottom of this loop and
      // preserved verbatim -- no code is needed here to protect a newer
      // build's save against an older build's read.
      //
      // Malformed is treated like a malformed `link` line, not like a
      // malformed `scalars` one: `pending.grain` simply keeps its
      // default-constructed value (grain OFF), which is always a legal
      // brush, rather than the whole preset being dropped for one bad line.
      float n[5];
      if (takeFloats(rest, 5, n)) {
        pending.grain.enabled = n[0] != 0.0f;
        pending.grain.periodX = static_cast<int32_t>(n[1]);
        pending.grain.periodY = static_cast<int32_t>(n[2]);
        pending.grain.depth = n[3];
        pending.grain.strength = n[4];
      }
      pointMode = PointMode::None;
      continue;
    }

    if (key == "link") {
      float n[6];
      if (!takeFloats(rest, 6, n)) {
        // Genuinely unparsable. Dropped outright -- not even the forward-
        // compatible preserve, because this is not "a valid link this build
        // does not understand", it is corrupt.
        pointMode = PointMode::None;
        continue;
      }
      const int srcOrd = static_cast<int>(n[0]);
      const int tgtOrd = static_cast<int>(n[1]);
      const bool inRange = srcOrd >= 0 && srcOrd < static_cast<int>(kDynamicSourceCount) &&
                           tgtOrd >= 0 && tgtOrd < static_cast<int>(kDynamicTargetCount);
      if (!inRange) {
        // §2: a well-formed link this build's enum does not reach -- from a
        // build with more sources or targets than this one. Preserved
        // verbatim, along with the `point` lines that follow it, so this
        // build's own save does not erase what a newer one wrote.
        pendingUnknown.push_back(line);
        pointMode = PointMode::PreserveBlock;
        continue;
      }
      BrushLink link;
      link.source = static_cast<DynamicSource>(srcOrd);
      link.target = static_cast<DynamicTarget>(tgtOrd);
      link.rangeLo = n[2];
      link.rangeHi = n[3];
      link.invert = n[4] != 0.0f;
      link.enabled = n[5] != 0.0f;
      pending.links.links.push_back(link);
      pointMode = PointMode::ActiveLink;
      continue;
    }

    if (key == "point") {
      if (pointMode == PointMode::PreserveBlock) {
        pendingUnknown.push_back(line);
        continue;
      }
      if (pointMode != PointMode::ActiveLink) continue;  // orphaned; dropped
      float n[2];
      if (takeFloats(rest, 2, n)) {
        CurvePoint p;
        p.x = n[0];
        p.y = n[1];
        pending.links.links.back().curve.push_back(p);
      }
      // A malformed `point` line loses just itself; the link it belongs to
      // keeps every point that DID parse, same granularity as everywhere
      // else in this parser.
      continue;
    }

    if (key == "floor") {
      // `floor <targetOrdinal> <value>` -- one line per non-zero entry of
      // `pending.links.multiplyFloor` (brush/Dynamics.hpp), a per-TARGET
      // floor rather than a per-LINK field, so it is its own key rather than
      // a seventh number on `link`'s line -- exactly this file's own §1 rule
      // ("new scalar data must arrive as a new key, never an eighth field"),
      // restated for `link`'s six instead of `scalars`' seven.
      float n[2];
      if (!takeFloats(rest, 2, n)) {
        // Genuinely unparsable -- dropped outright, same treatment as an
        // unparsable `link` line two blocks up.
        pointMode = PointMode::None;
        continue;
      }
      const int tgtOrd = static_cast<int>(n[0]);
      if (tgtOrd < 0 || tgtOrd >= static_cast<int>(kDynamicTargetCount)) {
        // §2's forward-compatible case, restated for a per-target floor: a
        // future build's thirteenth `DynamicTarget` writing its own floor is
        // correct data this build cannot evaluate but has no reason to
        // destroy. No `point` lines can follow a `floor` (it names a target,
        // not a curve), so this needs no `PreserveBlock` -- the single line
        // is the whole record.
        pendingUnknown.push_back(line);
        pointMode = PointMode::None;
        continue;
      }
      pending.links.multiplyFloor[static_cast<size_t>(tgtOrd)] = n[1];
      pointMode = PointMode::None;
      continue;
    }

    // A key this version does not know, inside a preset's scope.
    pendingUnknown.push_back(line);
    pointMode = PointMode::None;
  }

  flush();

  // Append last: `uniquePresetName()` needs to see `lib`'s existing presets
  // (the four built-ins, and anything Duplicate already made this session)
  // to defend a hand-edited file whose preset happens to share a name with
  // one of them. Presets are matched back to `presetUnknownLines_` by the
  // name they were parsed with, before this possible rename -- a collision
  // is rare enough, and the consequence (one preserved-lines block attached
  // to the renamed copy instead of silently vanishing) mild enough, that a
  // second bookkeeping pass to track the rename is not worth adding.
  for (BrushPreset& p : parsed) {
    p.libraryId = 0;
    p.builtin = false;
    p.name = uniquePresetName(lib, p.name);
    lib.presets.push_back(std::move(p));
  }
}

bool UserBrushLibraryStore::loadFromFile(const std::string& path, BrushLibrary& lib,
                                         std::string* errorOut) {
  if (errorOut) errorOut->clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    // Not an error: no user preset has ever been saved.
    parse(std::string(), lib);
    return true;
  }
  std::ostringstream buf;
  buf << f.rdbuf();
  const bool readOk = !f.bad();
  parse(buf.str(), lib);
  if (!readOk && errorOut)
    *errorOut = "user brushes: '" + path +
                "' could not be read to the end; what was readable has been kept.";
  return readOk;
}

// --- Writing ------------------------------------------------------------

std::string UserBrushLibraryStore::serialize(const BrushLibrary& lib) const {
  std::string out;
  out += kUserPresetsFileHeader;
  out += " " + std::to_string(kUserPresetsFileVersion) + "\n";

  for (const std::string& line : unknownLines_) out += sanitizeOneLine(line) + "\n";

  // Every preset the user owns, in `lib.presets`' own order -- see the
  // header's §3 on `serialize()`: this walk IS the diff against the file on
  // disk, so Delete needs no code here and Save needs only to have already
  // mutated `lib.presets` before calling this.
  for (const BrushPreset& p : lib.presets) {
    if (p.libraryId != 0 || p.builtin) continue;
    out += "preset " + sanitizeOneLine(p.name) + "\n";
    out += "scalars " + f9(p.radius) + " " + f9(p.hardness) + " " + f9(p.spacing) + " " +
           f9(p.roundness) + " " + f9(p.angle) + " " + f9(p.load) + " " + f9(p.wetness) + "\n";
    // A preset with grain OFF still writes this line (with `enabled` 0) --
    // consistent with `scalars` above always being written regardless of
    // whether a value sits at its default, and simpler than a second code
    // path for "nothing to say here".
    out += "grain " + std::string(p.grain.enabled ? "1" : "0") + " " +
           std::to_string(p.grain.periodX) + " " + std::to_string(p.grain.periodY) + " " +
           f9(p.grain.depth) + " " + f9(p.grain.strength) + "\n";
    // One `floor <targetOrdinal> <value>` line per non-zero
    // `multiplyFloor` entry -- omitted entirely when zero (the default "no
    // floor" every preset with no Minimum Diameter has), so a preset that
    // predates this key, or simply never had one, round-trips through this
    // build byte-identical to before `multiplyFloor` existed.
    for (size_t t = 0; t < kDynamicTargetCount; ++t) {
      if (p.links.multiplyFloor[t] == 0.0f) continue;
      out += "floor " + std::to_string(static_cast<int>(t)) + " " +
             f9(p.links.multiplyFloor[t]) + "\n";
    }
    for (const BrushLink& link : p.links.links) {
      out += "link " + std::to_string(static_cast<int>(link.source)) + " " +
             std::to_string(static_cast<int>(link.target)) + " " + f9(link.rangeLo) + " " +
             f9(link.rangeHi) + " " + (link.invert ? "1" : "0") + " " +
             (link.enabled ? "1" : "0") + "\n";
      // A DISABLED link's curve is still written -- "a link the user has
      // switched off keeps its curve and range" (brush/Dynamics.hpp's own
      // comment on `BrushLink::enabled`), and a save that dropped a toggled-
      // off link's shape would make the toggle destructive.
      for (const CurvePoint& pt : link.curve) out += "point " + f9(pt.x) + " " + f9(pt.y) + "\n";
    }
    const auto it = presetUnknownLines_.find(p.name);
    if (it != presetUnknownLines_.end())
      for (const std::string& line : it->second) out += sanitizeOneLine(line) + "\n";
  }
  return out;
}

bool UserBrushLibraryStore::saveToFile(const std::string& path, const BrushLibrary& lib,
                                       std::string* errorOut) const {
  if (errorOut) errorOut->clear();
  std::error_code ec;
  const fs::path parent = fs::path(path).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);
  return writeFileAtomically(path, serialize(lib), errorOut);
}

}  // namespace np
