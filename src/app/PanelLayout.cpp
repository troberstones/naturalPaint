#include "app/PanelLayout.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "ui/AtelierLayout.hpp"  // kDefaultDockExtents

namespace np {
namespace {

namespace fs = std::filesystem;

// The stable key table, one row per `ControlsSection` enumerator, in the
// enum's own declared order (not `controlsSections()`'s draw order -- the two
// are deliberately independent; see the header's serialization section).
// `--selftest` asserts this table is total, the same way `controlsSectionSpec()`
// asserts every enumerator has a spec.
struct KeyRow {
  ControlsSection section;
  const char* key;
};
constexpr KeyRow kKeyTable[] = {
    {ControlsSection::Tools, "tools"},
    {ControlsSection::Options, "options"},
    {ControlsSection::Color, "color"},
    {ControlsSection::Layers, "layers"},
    {ControlsSection::History, "history"},
    {ControlsSection::Comps, "comps"},
    {ControlsSection::Grade, "grade"},
    {ControlsSection::Histogram, "histogram"},
    {ControlsSection::BrushLibrary, "brush_library"},
    {ControlsSection::Brush, "brush"},
    {ControlsSection::Pigment, "pigment"},
    {ControlsSection::Medium, "medium"},
    {ControlsSection::BoardTilt, "board_tilt"},
    {ControlsSection::Grid, "grid"},
    {ControlsSection::Solver, "solver"},
};

struct PlacementRow {
  PanelPlacement placement;
  const char* key;
};
constexpr PlacementRow kPlacementTable[] = {
    {PanelPlacement::Left, "left"},     {PanelPlacement::Right, "right"},
    {PanelPlacement::Top, "top"},       {PanelPlacement::Bottom, "bottom"},
    {PanelPlacement::Flyout, "flyout"}, {PanelPlacement::Hidden, "hidden"},
};

// Where each section starts life. Everything not named here goes to the right
// dock, which is where the outgoing single column was -- so this table is
// exactly "the panels that are not in the column", and it is two entries long
// because that is how many bands the chrome used to have welded to it.
PanelPlacement defaultPlacementFor(ControlsSection section) {
  switch (section) {
    case ControlsSection::Tools:   return PanelPlacement::Left;
    case ControlsSection::Options: return PanelPlacement::Top;
    default:                       return PanelPlacement::Right;
  }
}

// One panel's default state.
//
// **The default COLLAPSE set is `controlsSections()`'s own `defaultOpen`
// flags, inverted** -- and that is not a tidy coincidence, it is the thing
// that makes the default arrangement usable at all.
//
// A dock does not scroll, so its panels divide it (ui/DockLayout). Thirteen
// expanded panels in the right dock need 13 x 72 px of floor plus twelve
// splitters -- over a thousand pixels before a single control is drawn -- so
// on any ordinary window the dock lands in `DockTiling::overflowed`, every
// panel is squeezed to its floor, and the dock falls back to scrolling. That
// is the exact behaviour the revamp exists to remove, arrived at from the
// other direction.
//
// `app/ControlsLayout` already carries the answer, and has since long before
// this feature: `defaultOpen` names the four sections worth a column of screen
// space in a fresh session (COLOR and the three document sections), and its
// header argues the case at length -- "a simulation section is a *tuning*
// surface: a handful of numbers that are set once for a medium and then left".
// A section not worth being open in a scrolling column is not worth a slot in
// a dock either. So the default is: those four expanded, the rest collapsed to
// their headers, and TOOLS and OPTIONS expanded because a collapsed tool
// palette is an empty left edge.
//
// The result is that the default arrangement shows exactly what the outgoing
// column showed on its first screen -- and shows it without scrolling, which
// the outgoing column could not.
PanelEntry defaultEntryFor(ControlsSection section) {
  PanelEntry e;
  e.section = section;
  e.placement = defaultPlacementFor(section);
  e.weight = kPanelDefaultWeight;
  e.collapsed = !controlsSectionSpec(section).defaultOpen;
  return e;
}

std::string trimmedLine(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
    --e;
  return s.substr(b, e - b);
}

// app/UserBrushLibrary.cpp's own `syncPath()`, reimplemented rather than
// shared -- see that module's header §4 for why. `fsync()`, not
// `fcntl(F_FULLFSYNC)`, for the same measured trade-off app/Journal.cpp
// documents at length: it covers a crashed process, a kill and a kernel panic
// (everything this application can actually produce) at roughly a tenth the
// cost of also covering sudden power loss.
void syncPath(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return;
  ::fsync(fd);
  ::close(fd);
}

// app/UserBrushLibrary.cpp's own `writeFileAtomically()`, reimplemented in the
// identical shape: write to `<path>.tmp`, fsync it, then `fs::rename()` it
// over `path`. A rename is atomic on the same filesystem, so a reader (or a
// crash) only ever sees the whole old file or the whole new one, never a
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

// Parse a float strictly: the whole token, nothing trailing, and finite. A
// `std::stof` that stops early would accept "1.0abc" and a `strtof` that
// overflows would hand back an infinity -- both of which reach ui/DockLayout's
// arithmetic as a weight and produce a NaN rect rather than a parse error.
bool parseFinite(const std::string& tok, float* out) {
  if (tok.empty()) return false;
  char* end = nullptr;
  const double v = std::strtod(tok.c_str(), &end);
  if (end != tok.c_str() + tok.size()) return false;
  if (!std::isfinite(v)) return false;
  *out = static_cast<float>(v);
  return true;
}

}  // namespace

bool panelPlacementIsDock(PanelPlacement p) noexcept {
  return p == PanelPlacement::Left || p == PanelPlacement::Right || p == PanelPlacement::Top ||
         p == PanelPlacement::Bottom;
}

const char* controlsSectionKey(ControlsSection section) {
  for (const KeyRow& row : kKeyTable)
    if (row.section == section) return row.key;
  // Unreachable while the table is total, which --selftest asserts. Not a
  // null so a caller that concatenates it into a line does not crash; an empty
  // key can never collide with a real one, so it always round-trips as an
  // "unknown section" if it is ever somehow written.
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

const char* panelPlacementKey(PanelPlacement placement) {
  for (const PlacementRow& row : kPlacementTable)
    if (row.placement == placement) return row.key;
  return "";
}

bool panelPlacementFromKey(const std::string& key, PanelPlacement* out) {
  for (const PlacementRow& row : kPlacementTable)
    if (key == row.key) {
      if (out) *out = row.placement;
      return true;
    }
  return false;
}

std::string defaultPanelLayoutFilePath() {
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

PanelLayout::PanelLayout() { resetToDefault(); }

void PanelLayout::resetToDefault() {
  entries_.clear();
  for (const ControlsSectionSpec& spec : controlsSections())
    entries_.push_back(defaultEntryFor(spec.section));
  docks_.left = kDefaultDockExtents.left;
  docks_.right = kDefaultDockExtents.right;
  docks_.top = kDefaultDockExtents.top;
  docks_.bottom = kDefaultDockExtents.bottom;
}

std::vector<ControlsSection> PanelLayout::sectionsIn(PanelPlacement placement) const {
  std::vector<ControlsSection> out;
  for (const PanelEntry& e : entries_)
    if (e.placement == placement) out.push_back(e.section);
  return out;
}

size_t PanelLayout::indexOf(ControlsSection section) const noexcept {
  for (size_t i = 0; i < entries_.size(); ++i)
    if (entries_[i].section == section) return i;
  return entries_.size();
}

PanelPlacement PanelLayout::placementOf(ControlsSection section) const noexcept {
  const size_t i = indexOf(section);
  return i < entries_.size() ? entries_[i].placement : PanelPlacement::Hidden;
}

float PanelLayout::weightOf(ControlsSection section) const noexcept {
  const size_t i = indexOf(section);
  return i < entries_.size() ? entries_[i].weight : kPanelDefaultWeight;
}

bool PanelLayout::isCollapsed(ControlsSection section) const noexcept {
  const size_t i = indexOf(section);
  return i < entries_.size() && entries_[i].collapsed;
}

void PanelLayout::setPlacement(ControlsSection section, PanelPlacement placement) {
  // Appending to the end of the target placement is what "index one past the
  // last" means, and clamping in `setPlacementAt()` turns that into the last
  // position -- so this is genuinely the same operation and not a second
  // implementation of it.
  size_t countThere = 0;
  for (const PanelEntry& e : entries_)
    if (e.placement == placement && e.section != section) ++countThere;
  setPlacementAt(section, placement, countThere);
}

void PanelLayout::setPlacementAt(ControlsSection section, PanelPlacement placement,
                                 size_t indexInPlacement) {
  const size_t from = indexOf(section);
  if (from >= entries_.size()) return;  // not present; invariant violated elsewhere

  PanelEntry moving = entries_[from];
  moving.placement = placement;
  entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(from));

  // Walk what is left, counting only the entries already in the target
  // placement, and splice in once that count reaches `indexInPlacement`. This
  // preserves every other entry's relative order -- including the interleaving
  // between placements, which is not meaningful to the user but is what makes
  // `entries()` a stable list for the PANELS menu to draw.
  size_t seen = 0;
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].placement != placement) continue;
    if (seen == indexInPlacement) {
      entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(i), moving);
      return;
    }
    ++seen;
  }
  entries_.push_back(moving);
}

void PanelLayout::setWeight(ControlsSection section, float weight) {
  const size_t i = indexOf(section);
  if (i >= entries_.size()) return;
  // Clamped rather than refused: a weight arrives here from a splitter drag,
  // and a drag that ran past a bound should stop at the bound rather than
  // silently do nothing.
  entries_[i].weight = std::isfinite(weight) ? std::max(kPanelMinWeight, weight)
                                             : kPanelDefaultWeight;
}

void PanelLayout::setCollapsed(ControlsSection section, bool collapsed) {
  const size_t i = indexOf(section);
  if (i < entries_.size()) entries_[i].collapsed = collapsed;
}

void PanelLayout::moveUp(ControlsSection section) {
  const size_t i = indexOf(section);
  if (i >= entries_.size()) return;
  const PanelPlacement p = entries_[i].placement;
  // The previous entry IN THE SAME PLACEMENT -- see the header for why this is
  // not `i - 1`.
  for (size_t j = i; j-- > 0;) {
    if (entries_[j].placement != p) continue;
    std::swap(entries_[i], entries_[j]);
    return;
  }
}

void PanelLayout::moveDown(ControlsSection section) {
  const size_t i = indexOf(section);
  if (i >= entries_.size()) return;
  const PanelPlacement p = entries_[i].placement;
  for (size_t j = i + 1; j < entries_.size(); ++j) {
    if (entries_[j].placement != p) continue;
    std::swap(entries_[i], entries_[j]);
    return;
  }
}

void PanelLayout::setDockExtent(PanelPlacement dock, float extent) {
  if (!panelPlacementIsDock(dock)) return;
  if (!std::isfinite(extent) || extent < 0.0f) return;
  const bool horizontal = (dock == PanelPlacement::Top || dock == PanelPlacement::Bottom);
  // Zero passes through unclamped: it is how a dock is switched off, and
  // clamping it up to the floor would make an empty dock impossible to hide.
  const float v = (extent == 0.0f)
                      ? 0.0f
                      : std::max(extent, horizontal ? kDockMinHeight : kDockMinWidth);
  switch (dock) {
    case PanelPlacement::Left:   docks_.left = v; break;
    case PanelPlacement::Right:  docks_.right = v; break;
    case PanelPlacement::Top:    docks_.top = v; break;
    case PanelPlacement::Bottom: docks_.bottom = v; break;
    default: break;
  }
}

PanelDockExtents PanelLayout::effectiveDockExtents() const {
  PanelDockExtents out;
  const auto occupied = [this](PanelPlacement p) {
    for (const PanelEntry& e : entries_)
      if (e.placement == p) return true;
    return false;
  };
  if (occupied(PanelPlacement::Left)) out.left = docks_.left;
  if (occupied(PanelPlacement::Right)) out.right = docks_.right;
  if (occupied(PanelPlacement::Top)) out.top = docks_.top;
  if (occupied(PanelPlacement::Bottom)) out.bottom = docks_.bottom;
  return out;
}

void PanelLayout::parse(const std::string& text) {
  std::vector<PanelEntry> parsed;
  std::vector<bool> seen(std::size(kKeyTable), false);
  bool sawHeader = false;

  // Start from the defaults, so a file that mentions no `dock` lines at all --
  // every version 1 file, for instance -- lands on the default extents rather
  // than on four zeroes, which would switch every dock off and present an
  // empty window.
  PanelDockExtents docks;
  docks.left = kDefaultDockExtents.left;
  docks.right = kDefaultDockExtents.right;
  docks.top = kDefaultDockExtents.top;
  docks.bottom = kDefaultDockExtents.bottom;

  const auto rowOf = [](ControlsSection s) {
    for (size_t i = 0; i < std::size(kKeyTable); ++i)
      if (kKeyTable[i].section == s) return i;
    return std::size(kKeyTable);
  };

  std::istringstream in(text);
  std::string raw;
  while (std::getline(in, raw)) {
    const std::string line = trimmedLine(raw);
    if (line.empty()) continue;

    std::istringstream ls(line);
    std::string tok1;
    ls >> tok1;

    if (!sawHeader && tok1 == kPanelLayoutFileHeader) {
      sawHeader = true;
      // The rest of the line (a version number) is read but not enforced --
      // app/BrushLibraryFile.hpp §1's and app/UserBrushLibrary.hpp §1's same
      // rule: a version mismatch is a diagnostic opportunity a later build
      // could use, never a refusal to read an otherwise-fine file. Which
      // grammar a line uses is decided by the line itself, below, not by this
      // number -- so a file whose header says 1 but whose body is version 2
      // still reads correctly.
      continue;
    }
    sawHeader = true;  // no header line is also accepted

    // --- dock <side> <extent> ---------------------------------------------
    if (tok1 == "dock") {
      std::string sideTok, extentTok, extra;
      if (!(ls >> sideTok) || !(ls >> extentTok) || (ls >> extra)) continue;
      PanelPlacement side;
      if (!panelPlacementFromKey(sideTok, &side) || !panelPlacementIsDock(side)) continue;
      float extent = 0.0f;
      if (!parseFinite(extentTok, &extent) || extent < 0.0f) continue;
      switch (side) {
        case PanelPlacement::Left:   docks.left = extent; break;
        case PanelPlacement::Right:  docks.right = extent; break;
        case PanelPlacement::Top:    docks.top = extent; break;
        case PanelPlacement::Bottom: docks.bottom = extent; break;
        default: break;
      }
      continue;
    }

    // --- version 1: section <key> <0|1> -----------------------------------
    //
    // Defined in terms of the version 2 grammar rather than handled apart from
    // it -- see the header. `1` meant "in the right-hand column"; `0` meant
    // "not shown", which is `hidden`.
    if (tok1 == "section") {
      std::string keyTok, visTok, extra;
      if (!(ls >> keyTok) || !(ls >> visTok) || (ls >> extra)) continue;
      if (visTok != "0" && visTok != "1") continue;
      ControlsSection section;
      if (!controlsSectionFromKey(keyTok, &section)) continue;
      const size_t row = rowOf(section);
      if (row < seen.size() && seen[row]) continue;  // duplicate: first wins
      if (row < seen.size()) seen[row] = true;
      // Everything except the placement comes from the default, because
      // version 1 had nothing else to say: it recorded order and a shown/hidden
      // bool, and a section's open/closed state lived in ImGui's per-session
      // header state where no file could see it. Defaulting the collapse is
      // what the old build actually showed a user on a fresh session, so it is
      // the honest reading of a file that does not mention it.
      PanelEntry e = defaultEntryFor(section);
      e.placement = (visTok == "1") ? PanelPlacement::Right : PanelPlacement::Hidden;
      parsed.push_back(e);
      continue;
    }

    // --- version 2: panel <key> <placement> <weight> <collapsed> ----------
    if (tok1 != "panel") continue;
    std::string keyTok, placeTok, weightTok, collapsedTok, extra;
    if (!(ls >> keyTok) || !(ls >> placeTok) || !(ls >> weightTok) || !(ls >> collapsedTok) ||
        (ls >> extra))
      continue;
    if (collapsedTok != "0" && collapsedTok != "1") continue;
    ControlsSection section;
    if (!controlsSectionFromKey(keyTok, &section)) continue;
    PanelPlacement placement;
    if (!panelPlacementFromKey(placeTok, &placement)) continue;
    float weight = kPanelDefaultWeight;
    // A non-positive or non-finite weight is a malformed line, not a value to
    // clamp: see the header's fourth repair rule.
    if (!parseFinite(weightTok, &weight) || !(weight > 0.0f)) continue;
    const size_t row = rowOf(section);
    if (row < seen.size() && seen[row]) continue;
    if (row < seen.size()) seen[row] = true;
    PanelEntry e;
    e.section = section;
    e.placement = placement;
    e.weight = std::max(kPanelMinWeight, weight);
    e.collapsed = (collapsedTok == "1");
    parsed.push_back(e);
  }

  // Append every section this pass did not see, at its DEFAULT placement, in
  // controlsSections()'s own order relative to the other missing ones -- the
  // "missing section" repair rule. See the header for why the default
  // placement rather than a fixed one.
  for (const ControlsSectionSpec& spec : controlsSections()) {
    const size_t row = rowOf(spec.section);
    if (row >= seen.size() || seen[row]) continue;
    parsed.push_back(defaultEntryFor(spec.section));
  }

  entries_ = std::move(parsed);
  docks_ = docks;
}

std::string PanelLayout::serialize() const {
  std::string out;
  out += kPanelLayoutFileHeader;
  out += " " + std::to_string(kPanelLayoutFileVersion) + "\n";

  // `%g`-style trimming would be shorter, but a fixed three decimals keeps the
  // file diffable and keeps a round-trip exact to well inside the precision a
  // splitter drag can produce.
  const auto num = [](float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
    return std::string(buf);
  };

  out += "dock left " + num(docks_.left) + "\n";
  out += "dock right " + num(docks_.right) + "\n";
  out += "dock top " + num(docks_.top) + "\n";
  out += "dock bottom " + num(docks_.bottom) + "\n";

  for (const PanelEntry& e : entries_) {
    out += "panel ";
    out += controlsSectionKey(e.section);
    out += " ";
    out += panelPlacementKey(e.placement);
    out += " " + num(e.weight);
    out += e.collapsed ? " 1\n" : " 0\n";
  }
  return out;
}

bool PanelLayout::loadFromFile(const std::string& path, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    // Not an error: no layout has ever been saved. Treated exactly like an
    // empty file, which parse()'s own "every section missing" rule resolves to
    // the default layout.
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

bool PanelLayout::saveToFile(const std::string& path, std::string* errorOut) const {
  if (errorOut) errorOut->clear();
  std::error_code ec;
  const fs::path parent = fs::path(path).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);
  return writeFileAtomically(path, serialize(), errorOut);
}

}  // namespace np
