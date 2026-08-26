#include "app/AbrReport.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "brush/Dynamics.hpp"
#include "io/AbrBrushes.hpp"

// app/AbrReport -- what actually survived importing an `.abr`.
//
// **Why this exists.** "These brushes do not feel right" has at least three
// causes that produce the same complaint and need completely different fixes:
//
//   1. the dynamics graph is imported but misread (a control mapped to the
//      wrong source, a range inverted, a curve flattened),
//   2. the graph is fine and the TIP never arrived -- Photoshop's expressive
//      brushes are overwhelmingly SAMPLED bitmap tips, and a sampled tip that
//      falls back to a round procedural dab cannot look like its preview no
//      matter how perfect the dynamics are,
//   3. the graph and tip are both fine and the STROKE ENGINE reads them
//      differently from Photoshop.
//
// Painting a stroke and squinting at it cannot separate these. This can: it
// prints the imported numbers next to the counts of what was dropped, so the
// question "is this a dynamics problem at all?" is answered before anyone
// starts tuning dynamics.
//
// Headless, GPU-free, and deliberately read-only -- it imports and prints,
// changes nothing, and writes no files.

namespace np {
namespace {

const char* curveShape(const Curve& c) {
  if (c.empty()) return "linear (none)";
  if (c.size() == 2) return "linear (2pt)";
  return "shaped";
}

}  // namespace

int runAbrReport(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "abr-report: cannot open %s\n", path);
    return 1;
  }
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  std::printf("abr-report: %s (%zu bytes)\n", path, bytes.size());

  const AbrImportResult r = importAbrBrushes(bytes);
  if (!r.ok) {
    std::fprintf(stderr, "abr-report: import failed: %s\n", r.error.c_str());
    return 1;
  }

  std::printf("\n%zu presets imported, %zu sampled tips NOT imported, %zu unmapped controls\n\n",
              r.presets.size(), r.sampledTips, r.unmappedControls);

  std::printf("%-40s %7s %7s %7s %7s %7s  %s\n", "name", "radius", "hard", "spacing", "round",
              "angle", "links");
  std::printf("%-40s %7s %7s %7s %7s %7s  %s\n", "----------------------------------------",
              "------", "----", "-------", "-----", "-----", "-----");

  // Counted so the summary can say whether the whole library shares one
  // configuration -- which is itself the finding, if it does.
  std::map<std::string, size_t> linkHistogram;
  size_t presetsWithNoLinks = 0;

  for (const BrushPreset& p : r.presets) {
    std::string linkText;
    if (p.links.links.empty()) {
      ++presetsWithNoLinks;
      linkText = "(none)";
    } else {
      for (const BrushLink& l : p.links.links) {
        if (!linkText.empty()) linkText += ", ";
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s->%s [%.2f..%.2f]%s%s %s", sourceName(l.source),
                      targetName(l.target), static_cast<double>(l.rangeLo),
                      static_cast<double>(l.rangeHi), l.invert ? " INV" : "",
                      l.enabled ? "" : " OFF", curveShape(l.curve));
        linkText += buf;
        char key[96];
        std::snprintf(key, sizeof(key), "%s -> %s", sourceName(l.source), targetName(l.target));
        ++linkHistogram[key];
      }
    }
    std::printf("%-40.40s %7.1f %7.2f %7.2f %7.2f %7.1f  %s\n", p.name.c_str(),
                static_cast<double>(p.radius), static_cast<double>(p.hardness),
                static_cast<double>(p.spacing), static_cast<double>(p.roundness),
                static_cast<double>(p.angle), linkText.c_str());
  }

  std::printf("\n-- link cells used across the library --\n");
  if (linkHistogram.empty()) {
    std::printf("  NONE. Every preset imported with an empty link set, so nothing in this\n");
    std::printf("  library varies with pressure, tilt, velocity or anything else.\n");
  } else {
    for (const auto& [cell, n] : linkHistogram)
      std::printf("  %-36s %zu preset(s)\n", cell.c_str(), n);
  }
  std::printf("  presets with NO links at all: %zu of %zu\n", presetsWithNoLinks,
              r.presets.size());

  // The notes are per-brush and repetitive by nature (one library tends to hit
  // the same limitation on every brush), so they are folded by text. The count
  // is what matters -- "47 brushes lost their tip" reads very differently from
  // one line saying a tip was lost.
  std::printf("\n-- import notes, folded by kind --\n");
  if (r.notes.empty()) {
    std::printf("  (none)\n");
  } else {
    std::map<std::string, size_t> byKind;
    for (const AbrImportNote& n : r.notes) ++byKind[n.what];
    for (const auto& [what, n] : byKind) std::printf("  %4zu x  %s\n", n, what.c_str());
  }

  if (r.sampledTips > 0) {
    std::printf(
        "\n**%zu of %zu brushes have a sampled bitmap tip that was not imported.**\n"
        "Those paint with the round procedural tip, so their shape is not this\n"
        "library's shape at all. No amount of dynamics tuning changes that:\n"
        "the mark being stamped is the wrong mark.\n",
        r.sampledTips, r.presets.size());
  }
  return 0;
}

}  // namespace np
