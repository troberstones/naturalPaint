#include "app/selftest/Support.hpp"

#include <map>
#include <set>
#include <string>

#include "brush/BrushModelIo.hpp"
#include "ui/BrushPanelLayout.hpp"

namespace np {

// ---------------------------------------------------------------------------
// ui/BrushPanelLayout: every `BrushModel` field has exactly one home.
//
// A field is edited by a row of a Brush Settings page, switched by a panel's
// checkbox in the list, or named in the omission table with a reason. A field in
// none of them is one the importer fills in and no painter can see; a field in
// two is two controls fighting over one value. Section C is the mirror image: a
// table naming a field that no longer exists.
// ---------------------------------------------------------------------------
bool runBrushPanelBindingTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const std::vector<std::string> fieldPaths = brushModelFieldPaths();
  const std::set<std::string> fieldPathSet(fieldPaths.begin(), fieldPaths.end());
  const std::vector<BrushFieldOmission>& omission = brushFieldOmissionTable();

  // Every leaf a control edits, with how many controls edit it.
  std::map<std::string, size_t> shown;
  for (const BrushRowSpec& row : brushPanelRows())
    for (const std::string& leaf : brushRowLeafPaths(row)) ++shown[leaf];
  for (size_t i = 0; i < kBrushPanelCount; ++i) {
    const BrushPanelSpec& spec = brushPanelSpec(static_cast<BrushPanel>(i));
    if (spec.enablePath[0] != '\0') ++shown[spec.enablePath];
  }
  std::set<std::string> omitted;
  for (const BrushFieldOmission& row : omission) omitted.insert(row.path);

  std::printf("  -- A. no field has two controls, and no omission is listed twice --\n");
  {
    size_t doubled = 0;
    std::string first;
    for (const auto& [path, count] : shown)
      if (count > 1 && doubled++ == 0) first = path;
    if (doubled > 0) std::printf("  [measured] %zu field(s) with two controls, first: %s\n", doubled, first.c_str());
    check(doubled == 0, "panels: every field is edited by at most one row or switch");
    check(omitted.size() == omission.size(), "omission table: no path listed twice");
  }

  std::printf("  -- B. every real field has a control or a reason --\n");
  {
    size_t missing = 0, both = 0;
    std::string firstMissing, firstBoth;
    for (const std::string& path : fieldPaths) {
      const bool hasControl = shown.count(path) != 0;
      const bool isOmitted = omitted.count(path) != 0;
      if (!hasControl && !isOmitted && missing++ == 0) firstMissing = path;
      if (hasControl && isOmitted && both++ == 0) firstBoth = path;
    }
    if (missing > 0)
      std::printf("  [measured] %zu field(s) with neither, first: %s\n", missing, firstMissing.c_str());
    check(missing == 0, "every path from brushModelFieldPaths() has a control or an omission reason");
    if (both > 0)
      std::printf("  [measured] %zu field(s) with both, first: %s\n", both, firstBoth.c_str());
    check(both == 0, "no field has both a control and an omission reason");
    check(shown.size() + omitted.size() == fieldPaths.size(),
          "controlled fields + omitted fields == brushModelFieldPaths().size()");
  }

  std::printf("  -- C. no table names a field that does not exist --\n");
  {
    size_t staleShown = 0, staleOmitted = 0;
    std::string firstShown, firstOmitted;
    for (const auto& [path, count] : shown)
      if (fieldPathSet.count(path) == 0 && staleShown++ == 0) firstShown = path;
    for (const std::string& path : omitted)
      if (fieldPathSet.count(path) == 0 && staleOmitted++ == 0) firstOmitted = path;
    if (staleShown > 0)
      std::printf("  [measured] %zu stale control path(s), first: %s\n", staleShown, firstShown.c_str());
    check(staleShown == 0, "every row and switch names a real BrushModel field");
    if (staleOmitted > 0)
      std::printf("  [measured] %zu stale omission path(s), first: %s\n", staleOmitted,
                  firstOmitted.c_str());
    check(staleOmitted == 0, "every omission-table path names a real BrushModel field");
  }

  std::printf("  -- D. every omission carries an actual reason --\n");
  {
    bool everyReason = true;
    for (const BrushFieldOmission& row : omission)
      if (row.reason == nullptr || row.reason[0] == '\0') everyReason = false;
    check(everyReason, "every omission-table row carries a non-empty reason");
  }

  std::printf("[selftest] brush panel binding %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
