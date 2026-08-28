#include "app/AbrReport.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "brush/BrushModel.hpp"
#include "brush/Dynamics.hpp"
#include "io/AbrBrushes.hpp"
#include "io/Descriptor.hpp"
#include "io/PsPatterns.hpp"

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

namespace {

// `Brsh/12/dualBrush/Brsh/Dmtr` -> `Brsh[]/dualBrush/Brsh/Dmtr`.
//
// `DescriptorRef::path()` contributes a list element's INDEX, because a list
// element has no key of its own. That is the right answer for naming one node
// and exactly the wrong one for counting a population: without this collapse
// every preset's `useTexture` is its own row and the census says 101 keys
// occurring once each, which is the opposite of the thing being measured.
std::string collapseListIndices(const std::string& path) {
  std::string out;
  size_t at = 0;
  while (at <= path.size()) {
    const size_t slash = path.find('/', at);
    const size_t end = (slash == std::string::npos) ? path.size() : slash;
    const std::string segment = path.substr(at, end - at);
    const bool numeric =
        !segment.empty() &&
        std::all_of(segment.begin(), segment.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (!out.empty() && numeric) {
      out += "[]";  // attaches to the container's own segment, not its own row
    } else {
      if (!out.empty()) out += '/';
      out += segment;
    }
    if (slash == std::string::npos) break;
    at = slash + 1;
  }
  return out;
}


// One row of the census: everything that can be said about a key from the
// values alone, kept in the form that answers "is this worth implementing".
struct KeyStats {
  size_t count = 0;
  std::set<std::string> types;
  size_t trueCount = 0, falseCount = 0;       // bool
  std::map<std::string, size_t> enumValues;   // enum + TEXT, capped below
  bool haveNumber = false;
  double minValue = 0.0, maxValue = 0.0;      // doub / UntF / long
  std::set<std::string> units;                // UntF
};

void accumulate(KeyStats& st, const DescriptorRef& ref) {
  ++st.count;
  st.types.insert(descriptorTypeKey(ref.type()));  // io/Descriptor's own naming, not a second one

  if (const std::optional<bool> b = ref.asBoolean()) {
    if (*b) ++st.trueCount; else ++st.falseCount;
    return;
  }
  if (const std::optional<DescriptorEnumerated> e = ref.asEnumerated()) {
    ++st.enumValues[e->valueId];
    return;
  }
  // TEXT is histogrammed too, but capped: `Nm  ` is 101 distinct brush names
  // and listing them is what `--abr-report` already does. 12 is enough to show
  // "this is one of a few fixed values" and short enough not to bury the row.
  if (const std::optional<std::string_view> t = ref.asText()) {
    if (st.enumValues.size() < 12) ++st.enumValues[std::string(*t)];
    else ++st.enumValues["(more)"];
    return;
  }

  std::optional<double> number;
  if (const std::optional<DescriptorUnitFloat> u = ref.asUnitFloat()) {
    number = u->value;
    st.units.insert(u->unit);
  } else if (const std::optional<double> d = ref.asDouble()) {
    number = d;
  } else if (const std::optional<int32_t> i = ref.asInteger()) {
    number = static_cast<double>(*i);
  }
  if (number) {
    if (!st.haveNumber) {
      st.minValue = st.maxValue = *number;
      st.haveNumber = true;
    } else {
      st.minValue = std::min(st.minValue, *number);
      st.maxValue = std::max(st.maxValue, *number);
    }
  }
}

// Iterative, with an explicit stack, for the same reason io/Descriptor.hpp's
// own parser is: the tree comes from a file this build did not write, and a
// recursive walk over a hostile one is a stack overflow rather than a refusal.
// The parser's `maxDepth` already bounds this, so the stack is belt to that's
// braces -- but the parser's bound is a promise about the tree, and this is a
// walk that has to hold on its own.
void walkKeys(const DescriptorRef& root, std::map<std::string, KeyStats>& out) {
  std::vector<DescriptorRef> stack{root};
  while (!stack.empty()) {
    const DescriptorRef ref = stack.back();
    stack.pop_back();
    if (!ref.valid()) continue;

    // The root itself has no key and is not a row.
    if (!ref.key().empty() || ref.path() != "") {
      const std::string path = collapseListIndices(ref.path());
      if (!path.empty()) accumulate(out[path], ref);
    }
    for (size_t i = 0; i < ref.childCount(); ++i) stack.push_back(ref.child(i));
  }
}

}  // namespace

namespace {

// One row of the panel coverage table.
//
// **The point of this table is the gap between the two numbers.** Before it,
// an import that dropped Texture on 84 of 101 brushes reported nothing at all
// -- the summary counted sampled tips and Dual Brush and stopped there, so
// the five panels it never looked at were indistinguishable from five panels
// no file used. Two of this project's own lessons converge here: an absence
// claim rots, and a green suite once canonised a P0 gap. A number printed
// from the file every time it is read cannot do either.
struct PanelRow {
  const char* name;
  size_t requested = 0;  // presets whose file switches this panel ON
  size_t rendered = 0;   // presets where this build actually acts on it
  const char* note = "";
};

void printPanelCoverage(const AbrImportResult& r) {
  if (r.presets.empty()) return;

  PanelRow rows[] = {
      {"Brush Tip Shape", 0, 0, ""},
      {"Shape Dynamics", 0, 0, ""},
      {"Scattering", 0, 0, "Count is read but every dab still lands once"},
      {"  - Count > 1", 0, 0, "dabs per position; not yet stamped"},
      {"Texture", 0, 0, ""},
      {"Dual Brush", 0, 0, ""},
      {"Color Dynamics", 0, 0, "read; no engine target"},
      {"Transfer", 0, 0, "read; flow/opacity not yet applied"},
      {"Blend mode (Md )", 0, 0, "read; the stroke still composites Normal"},
      {"Noise", 0, 0, "refused: no published formula"},
      {"Wet Edges", 0, 0, "refused: not implemented"},
      {"Build-up (Rpt )", 0, 0, "refused: INFERRED meaning, and time-based"},
      {"Brush Pose", 0, 0, "refused: not implemented"},
  };
  enum { kTip, kShape, kScatter, kCount, kTexture, kDual, kColor, kTransfer,
         kBlend, kNoise, kWet, kAir, kPose };

  // Read off `presets[i].model` rather than a separate `AbrImportResult::models`
  // vector -- that vector used to exist, index-parallel to `presets`, and was
  // deleted in favour of the model living on the preset it describes
  // (`brush/Library.hpp`'s `BrushPreset::model` and this header's own comment
  // on `AbrImportResult::presets`). This loop reading the same table it always
  // did, from its new home, is the proof that move changed nothing this
  // report says.
  for (const BrushPreset& preset : r.presets) {
    const BrushModel& m = preset.model;
    if (!m.tip.dab.id.empty()) {
      ++rows[kTip].requested;
      if (m.tip.dab.bitmap != nullptr) ++rows[kTip].rendered;
    }
    if (m.shape.enabled) { ++rows[kShape].requested; ++rows[kShape].rendered; }
    if (m.scatter.enabled) { ++rows[kScatter].requested; ++rows[kScatter].rendered; }
    if (m.scatter.count > 1) ++rows[kCount].requested;
    if (m.texture.enabled) ++rows[kTexture].requested;
    if (m.dual.enabled) ++rows[kDual].requested;
    if (m.color.enabled) ++rows[kColor].requested;
    if (m.transfer.enabled) ++rows[kTransfer].requested;
    if (!m.options.blendMode.empty() && m.options.blendMode != "Nrml")
      ++rows[kBlend].requested;
    if (m.noise) ++rows[kNoise].requested;
    if (m.wetEdges) ++rows[kWet].requested;
    if (m.airbrush) ++rows[kAir].requested;
    if (m.brushPose) ++rows[kPose].requested;
  }
  // The Dual Brush is the one panel whose rendered count the import already
  // knows, because it has been counting its own losses all along.
  rows[kDual].rendered =
      rows[kDual].requested - r.dualBrushes - r.dualBrushUnsupportedBlend;
  // Texture's rendered count is the import's own, for the same reason the
  // Dual Brush's is: it is the only party that knows which paper resolved.
  rows[kTexture].rendered = r.texturesApplied;
  if (r.texturesNotApplied > 0) rows[kTexture].note = "see the notes for which papers went missing";
  else if (r.texturesApplied > 0) rows[kTexture].note = "";

  std::printf("\n-- panel coverage: what the file asks for, what this build does --\n");
  std::printf("%-20s %9s %9s  %s\n", "panel", "asked by", "rendered", "note");
  std::printf("%-20s %9s %9s  %s\n", "--------------------", "--------", "--------", "----");
  for (const PanelRow& row : rows) {
    if (row.requested == 0 && row.rendered == 0) {
      std::printf("%-20s %9s %9s  %s\n", row.name, "-", "-",
                  *row.note != '\0' ? row.note : "no preset in this file uses it");
      continue;
    }
    std::printf("%-20s %9zu %9zu  %s\n", row.name, row.requested, row.rendered, row.note);
  }
  std::printf("(of %zu presets; %zu pattern(s) decoded from `patt`", r.presets.size(),
              r.patternsDecoded);
  if (r.patternsSkipped > 0) std::printf(", %zu skipped", r.patternsSkipped);
  std::printf(")\n");
}

}  // namespace

int runAbrKeyCensus(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "abr-keys: cannot open %s\n", path);
    return 1;
  }
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());

  const AbrSectionTable table = readAbrSections(bytes);
  std::printf("abr-keys: %s (%zu bytes)\n", path, bytes.size());
  if (!table.ok) {
    std::fprintf(stderr, "abr-keys: %s\n", table.error.c_str());
    return 1;
  }
  std::printf("version %u.%u\n\n", static_cast<unsigned>(table.version),
              static_cast<unsigned>(table.subversion));

  std::printf("-- 8BIM sections --\n");
  size_t descAt = 0, descLen = 0;
  bool haveDesc = false;
  for (const AbrSection& s : table.sections) {
    // The share is worth printing rather than left to be divided by hand: it
    // is how `patt` was discovered to be 99% of a 36 MB pack while being read
    // by nothing at all.
    const double share = bytes.empty() ? 0.0
                                       : 100.0 * static_cast<double>(s.length) /
                                             static_cast<double>(bytes.size());
    std::printf("  %-6s at %10zu  length %12zu  (%5.1f%% of file)\n", s.key.c_str(), s.at,
                s.length, share);
    if (s.key == "desc" && !haveDesc) {
      descAt = s.at;
      descLen = s.length;
      haveDesc = true;
    }
  }
  // The pattern block, decoded rather than stepped over. Printed here and not
  // only in `runAbrReport()` because the census is where the question "is
  // there anything in that 36 MB" gets asked, and a count with names answers
  // it in a way a byte total does not.
  for (const AbrSection& s : table.sections) {
    if (s.key != "patt") continue;
    const PsPatternResult pat =
        parseAbrPatterns(std::span<const uint8_t>(bytes).subspan(s.at, s.length));
    std::printf("\n-- patt: %zu pattern(s) decoded, %zu skipped%s --\n", pat.patterns.size(),
                pat.skipped, pat.truncated ? ", WALK STOPPED EARLY" : "");
    for (const PsPattern& q : pat.patterns)
      std::printf("  %5dx%-5d %8zu texels  %.36s  %.60s\n", q.width, q.height, q.height8.size(),
                  q.id.c_str(), q.name.c_str());
  }

  if (!haveDesc) {
    std::printf("\nno `desc` block: nothing to census.\n");
    return 1;
  }

  const DescriptorParseResult parsed =
      parseVersionedActionDescriptor(std::span<const uint8_t>(bytes).subspan(descAt, descLen));
  if (!parsed.ok) {
    std::fprintf(stderr, "\nabr-keys: the `desc` block did not parse: %s\n", parsed.error.c_str());
    return 1;
  }

  std::map<std::string, KeyStats> keys;
  walkKeys(parsed.tree.root(), keys);

  std::printf("\n-- descriptor key census (%zu distinct paths, %zu nodes) --\n", keys.size(),
              parsed.tree.nodeCount());
  std::printf("%-46s %6s %-10s %s\n", "path", "count", "types", "values");
  std::printf("%-46s %6s %-10s %s\n", "----------------------------------------------", "-----",
              "----------", "------");
  for (const auto& [path, st] : keys) {
    std::string types;
    for (const std::string& t : st.types) {
      if (!types.empty()) types += ",";
      types += t;
    }

    std::string values;
    if (st.trueCount + st.falseCount > 0) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "true %zu / false %zu", st.trueCount, st.falseCount);
      values = buf;
    }
    for (const auto& [v, n] : st.enumValues) {
      if (!values.empty()) values += ", ";
      values += v + " x" + std::to_string(n);
    }
    if (st.haveNumber) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "%s%.4g..%.4g", values.empty() ? "" : ", ", st.minValue,
                    st.maxValue);
      values += buf;
    }
    for (const std::string& u : st.units) values += " [" + u + "]";

    std::printf("%-46.46s %6zu %-10.10s %.100s\n", path.c_str(), st.count, types.c_str(),
                values.c_str());
  }
  return 0;
}

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
  printPanelCoverage(r);

  std::printf(
      "\n%zu presets imported, %zu sampled tips NOT imported, %zu unmapped controls, "
      "%zu with no Dual Brush tip at all, %zu with an unsupported Dual Brush blend mode, "
      "%zu with a Dual Brush whose spacing/scatter/count are not honoured\n\n",
      r.presets.size(), r.sampledTips, r.unmappedControls, r.dualBrushes,
      r.dualBrushUnsupportedBlend, r.dualBrushCadenceNotHonoured);

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
    // `spacingPercent` is a percentage OF THE DIAMETER; this column has
    // always reported RADII (the old, now-deleted `BrushPreset::spacing`
    // scalar's own unit), so `/ 100 * 2` is the conversion that keeps this
    // report's numbers comparable across the migration, not a bare `/ 100`
    // (`app/StrokeSession::brushTipFor()`'s `tip.spacing` comment names the
    // same factor of two).
    std::printf("%-40.40s %7.1f %7.2f %7.2f %7.2f %7.1f  %s\n", p.name.c_str(),
                static_cast<double>(p.model.tip.diameterPx / 2.0f),
                static_cast<double>(p.model.tip.hardness),
                static_cast<double>(p.model.tip.spacingPercent / 100.0f * 2.0f),
                static_cast<double>(p.model.tip.roundness),
                static_cast<double>(p.model.tip.angleDeg), linkText.c_str());
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
  if (r.dualBrushes > 0) {
    std::printf(
        "\n**%zu of %zu brushes have Dual Brush switched ON with no second tip this build "
        "could bring across at all.**\n"
        "Photoshop stamps a SECOND tip through those, with its own blend mode,\n"
        "spacing, scatter and count. Neither the shape nor the blend mode arrived for these,\n"
        "so that second stamp is missing entirely -- which shows up as a mark that is\n"
        "smoother and more even than the original, since breaking up the first\n"
        "tip's edge is most of what the second one is there to do.\n"
        "\n"
        "Read this the same way as the sampled-tip line above: it is a reason\n"
        "the brush cannot look right YET, and it is not a dynamics problem.\n",
        r.dualBrushes, r.presets.size());
  }
  if (r.dualBrushUnsupportedBlend > 0) {
    std::printf(
        "\n**%zu of %zu brushes have a Dual Brush blend mode this build does not "
        "composite.**\n"
        "The second tip's SHAPE was read; its `BlnM` just names a mode other than\n"
        "Multiply, Overlay, Color Burn or Hard Mix, the four brush/Deposit.hpp's\n"
        "dabCoverage() implements.\n"
        "These paint with the primary tip alone, same as the line above, but the\n"
        "second tip itself is not the problem -- see the notes for which mode.\n",
        r.dualBrushUnsupportedBlend, r.presets.size());
  }
  if (r.dualBrushCadenceNotHonoured > 0) {
    std::printf(
        "\n**%zu of %zu brushes have a Dual Brush whose own spacing/scatter/count is not "
        "honoured.**\n"
        "Their second tip DOES paint, composited by Multiply, Overlay, Color Burn\n"
        "or Hard Mix -- but it is stamped once, centred on every dab of the first,\n"
        "rather than scattered its own number of times. These brushes will read\n"
        "less granular than Photoshop's even with the second tip's shape correct.\n",
        r.dualBrushCadenceNotHonoured, r.presets.size());
  }
  return 0;
}

}  // namespace np
