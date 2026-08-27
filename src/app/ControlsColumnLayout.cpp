#include "app/ControlsColumnLayout.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace np {
namespace {

namespace fs = std::filesystem;

// The stable key table, one row per `ControlsSection` enumerator, in the
// enum's own declared order (not `controlsSections()`'s draw order -- the two
// are deliberately independent; see the header's serialization section).
// `--selftest` asserts this table is total, the same way app/ControlsLayout's
// own suite asserts `controlsSections()` has one spec per enumerator.
struct KeyRow {
  ControlsSection section;
  const char* key;
};
constexpr KeyRow kKeyTable[] = {
    {ControlsSection::Color, "color"},
    {ControlsSection::Layers, "layers"},
    {ControlsSection::History, "history"},
    {ControlsSection::Comps, "comps"},
    {ControlsSection::Grade, "grade"},
    {ControlsSection::BrushLibrary, "brush_library"},
    {ControlsSection::Brush, "brush"},
    {ControlsSection::Pigment, "pigment"},
    {ControlsSection::Medium, "medium"},
    {ControlsSection::BoardTilt, "board_tilt"},
    {ControlsSection::Grid, "grid"},
    {ControlsSection::Solver, "solver"},
};

std::string trimmedLine(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
    --e;
  return s.substr(b, e - b);
}

// app/UserBrushLibrary.cpp's own `syncPath()`, reimplemented rather than
// shared -- see this module's header §4 for why. `fsync()`, not
// `fcntl(F_FULLFSYNC)`, for the same measured trade-off app/Journal.cpp
// documents at length: it covers a crashed process, a kill and a kernel
// panic (everything this application can actually produce) at roughly a
// tenth the cost of also covering sudden power loss.
void syncPath(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return;
  ::fsync(fd);
  ::close(fd);
}

// app/UserBrushLibrary.cpp's own `writeFileAtomically()`, reimplemented in
// the identical shape: write to `<path>.tmp`, fsync it, then `fs::rename()`
// it over `path`. A rename is atomic on the same filesystem, so a reader (or
// a crash) only ever sees the whole old file or the whole new one, never a
// mixture.
bool writeFileAtomically(const std::string& path, const std::string& contents,
                          std::string* errorOut) {
  const std::string temp = path + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (errorOut) *errorOut = "panel layout: could not open '" + temp + "' for writing.";
      return false;
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
    if (!out) {
      if (errorOut) *errorOut = "panel layout: '" + temp + "' was opened but not fully written.";
      return false;
    }
  }
  syncPath(temp);
  std::error_code ec;
  fs::rename(temp, path, ec);
  if (ec) {
    if (errorOut)
      *errorOut =
          "panel layout: could not rename '" + temp + "' into place (" + ec.message() + ").";
    fs::remove(temp, ec);
    return false;
  }
  return true;
}

}  // namespace

const char* controlsSectionKey(ControlsSection section) {
  for (const KeyRow& row : kKeyTable)
    if (row.section == section) return row.key;
  // Unreachable while the table is total, which --selftest asserts. Not a
  // null so a caller that concatenates it into a line does not crash; an
  // empty key can never collide with a real one, so it always round-trips as
  // an "unknown section" if it is ever somehow written.
  return "";
}

bool controlsSectionFromKey(const std::string& key, ControlsSection* out) {
  for (const KeyRow& row : kKeyTable)
    if (key == row.key) {
      if (out) *out = row.section;
      return true;
    }
  return false;
}

std::string defaultControlsColumnLayoutFilePath() {
  // Same override-first shape as `defaultUserPresetsFilePath()`, so
  // `--selftest` (or a second profile) never touches the real settings
  // directory.
  if (const char* explicitPath = std::getenv("NP_PANEL_LAYOUT")) {
    if (*explicitPath != '\0') return explicitPath;
  }
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  if (home && *home)
    return std::string(home) + "/Library/Application Support/naturalPaint/panel-layout.txt";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
    if (*xdg != '\0') return std::string(xdg) + "/naturalPaint/panel-layout.txt";
  }
  if (home && *home) return std::string(home) + "/.config/naturalPaint/panel-layout.txt";
#endif
  return "panel-layout.txt";
}

ControlsColumnLayout::ControlsColumnLayout() { resetToDefault(); }

void ControlsColumnLayout::resetToDefault() {
  entries_.clear();
  for (const ControlsSectionSpec& spec : controlsSections())
    entries_.push_back(ControlsColumnEntry{spec.section, true});
}

std::vector<ControlsSection> ControlsColumnLayout::visibleSections() const {
  std::vector<ControlsSection> out;
  for (const ControlsColumnEntry& e : entries_)
    if (e.visible) out.push_back(e.section);
  return out;
}

size_t ControlsColumnLayout::indexOf(ControlsSection section) const noexcept {
  for (size_t i = 0; i < entries_.size(); ++i)
    if (entries_[i].section == section) return i;
  return entries_.size();
}

bool ControlsColumnLayout::isVisible(ControlsSection section) const noexcept {
  const size_t i = indexOf(section);
  return i < entries_.size() && entries_[i].visible;
}

void ControlsColumnLayout::setVisible(ControlsSection section, bool visible) {
  const size_t i = indexOf(section);
  if (i < entries_.size()) entries_[i].visible = visible;
}

void ControlsColumnLayout::moveTo(ControlsSection section, size_t newIndex) {
  const size_t from = indexOf(section);
  if (from >= entries_.size()) return;  // not present; invariant violated elsewhere
  if (entries_.size() <= 1) return;
  const size_t clamped = std::min(newIndex, entries_.size() - 1);
  if (clamped == from) return;
  const ControlsColumnEntry entry = entries_[from];
  entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(from));
  entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(clamped), entry);
}

void ControlsColumnLayout::moveUp(ControlsSection section) {
  const size_t i = indexOf(section);
  if (i == 0 || i >= entries_.size()) return;
  moveTo(section, i - 1);
}

void ControlsColumnLayout::moveDown(ControlsSection section) {
  const size_t i = indexOf(section);
  if (i + 1 >= entries_.size()) return;
  moveTo(section, i + 1);
}

void ControlsColumnLayout::parse(const std::string& text) {
  // Every non-empty line must be either the header (only recognised as the
  // very first non-empty line) or exactly `section <key> <0|1>`. A line that
  // is not becomes the FOURTH per-line repair rather than a verdict on the
  // whole file -- see the header's repair section for why skipping degrades
  // better than discarding, and why it still lands a genuinely foreign file
  // on the default.
  std::vector<ControlsColumnEntry> parsed;
  std::vector<bool> seen(std::size(kKeyTable), false);
  bool sawHeader = false;

  std::istringstream in(text);
  std::string raw;
  while (std::getline(in, raw)) {
    const std::string line = trimmedLine(raw);
    if (line.empty()) continue;

    std::istringstream ls(line);
    std::string tok1, tok2, tok3, extra;
    ls >> tok1;

    if (!sawHeader && tok1 == kControlsColumnLayoutFileHeader) {
      sawHeader = true;
      // The rest of the line (a version number) is read but not enforced --
      // this header's own note on why. Any content, or none, is accepted.
      continue;
    }
    sawHeader = true;  // no header line is also accepted; do not re-check this branch

    // Skipped, not fatal. The section this line failed to name is then
    // simply one this pass did not see, so the missing-section rule below
    // appends it -- the result is always a complete, valid layout.
    if (tok1 != "section" || !(ls >> tok2) || !(ls >> tok3) || (ls >> extra)) continue;
    if (tok3 != "0" && tok3 != "1") continue;

    ControlsSection section;
    if (!controlsSectionFromKey(tok2, &section)) {
      // Unknown section name: grammatically fine, ignored.
      continue;
    }
    size_t row = std::size(kKeyTable);
    for (size_t i = 0; i < std::size(kKeyTable); ++i)
      if (kKeyTable[i].section == section) { row = i; break; }
    if (row < seen.size() && seen[row]) continue;  // duplicate: first wins
    if (row < seen.size()) seen[row] = true;
    parsed.push_back(ControlsColumnEntry{section, tok3 == "1"});
  }

  // Append every section this pass did not see, in controlsSections()'s own
  // default order relative to the other missing ones -- the "missing
  // section" repair rule.
  for (const ControlsSectionSpec& spec : controlsSections()) {
    size_t row = std::size(kKeyTable);
    for (size_t i = 0; i < std::size(kKeyTable); ++i)
      if (kKeyTable[i].section == spec.section) { row = i; break; }
    if (row < seen.size() && !seen[row]) parsed.push_back(ControlsColumnEntry{spec.section, true});
  }

  entries_ = std::move(parsed);
}

std::string ControlsColumnLayout::serialize() const {
  std::string out;
  out += kControlsColumnLayoutFileHeader;
  out += " " + std::to_string(kControlsColumnLayoutFileVersion) + "\n";
  for (const ControlsColumnEntry& e : entries_) {
    out += "section ";
    out += controlsSectionKey(e.section);
    out += e.visible ? " 1\n" : " 0\n";
  }
  return out;
}

bool ControlsColumnLayout::loadFromFile(const std::string& path, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    // Not an error: no layout has ever been saved. Treated exactly like an
    // empty file, which parse()'s own "every section missing" rule resolves
    // to the default layout.
    parse(std::string());
    return true;
  }
  std::ostringstream buf;
  buf << f.rdbuf();
  const bool readOk = !f.bad();
  parse(buf.str());
  if (!readOk && errorOut)
    *errorOut = "panel layout: '" + path +
                "' could not be read to the end; the default layout has been kept.";
  return readOk;
}

bool ControlsColumnLayout::saveToFile(const std::string& path, std::string* errorOut) const {
  if (errorOut) errorOut->clear();
  std::error_code ec;
  const fs::path parent = fs::path(path).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);
  return writeFileAtomically(path, serialize(), errorOut);
}

}  // namespace np
