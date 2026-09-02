#include "app/selftest/Support.hpp"

#include <set>
#include <string>

#include "brush/BrushModelIo.hpp"
#include "ui/BrushFieldPresentation.hpp"

namespace np {

// ---------------------------------------------------------------------------
// ui/BrushFieldPresentation: the exhaustiveness guarantee this task exists
// to add.
//
// Before this file, a `BrushModel` leaf (brush/BrushModelFields.hpp's own
// 151, walked by `visitBrushModelFields()`) could go from "the importer
// fills it in" to "no control anywhere ever draws it" with no warning at
// all -- `BrushModelFields.hpp`'s own header names this as the third thing
// its one visitor was always going to need to support, and this is that
// support arriving.
//
// The load-bearing claim is section B: every path `brushModelFieldPaths()`
// produces is in EXACTLY ONE of `brushFieldPresentationTable()` (gets a
// live control somewhere) or `brushFieldOmissionTable()` (deliberately does
// not, with a reason). A path in neither is a field silently missing a
// control; a path in both is two tables disagreeing about whether one
// exists. Section C is the mirror-image failure: a stale row in either
// table naming a path that is no longer a real field at all (a rename or a
// removal left behind a dangling entry).
// ---------------------------------------------------------------------------
bool runBrushPanelBindingTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const std::vector<std::string> fieldPaths = brushModelFieldPaths();
  const std::set<std::string> fieldPathSet(fieldPaths.begin(), fieldPaths.end());
  const std::vector<BrushFieldSpec>& presentation = brushFieldPresentationTable();
  const std::vector<BrushFieldOmission>& omission = brushFieldOmissionTable();

  // ==========================================================================
  std::printf("  -- A. no duplicate rows in either table --\n");
  // ==========================================================================
  {
    std::set<std::string> presPaths, omitPaths;
    for (const BrushFieldSpec& row : presentation) presPaths.insert(row.path);
    for (const BrushFieldOmission& row : omission) omitPaths.insert(row.path);
    check(presPaths.size() == presentation.size(),
          "presentation table: no path listed twice");
    check(omitPaths.size() == omission.size(), "omission table: no path listed twice");
  }

  // ==========================================================================
  std::printf("  -- B. every real field is in exactly one table --\n");
  // ==========================================================================
  {
    size_t missing = 0, doubled = 0;
    std::string firstMissing, firstDoubled;
    for (const std::string& path : fieldPaths) {
      const bool inPresentation = findBrushFieldSpec(path) != nullptr;
      const bool inOmission = findBrushFieldOmissionReason(path) != nullptr;
      if (!inPresentation && !inOmission) {
        if (missing == 0) firstMissing = path;
        ++missing;
      } else if (inPresentation && inOmission) {
        if (doubled == 0) firstDoubled = path;
        ++doubled;
      }
    }
    if (missing > 0)
      std::printf("  [measured] %zu path(s) in neither table, first: %s\n", missing,
                  firstMissing.c_str());
    check(missing == 0,
          "every path from brushModelFieldPaths() is in the presentation table or the "
          "omission table");
    if (doubled > 0)
      std::printf("  [measured] %zu path(s) in BOTH tables, first: %s\n", doubled,
                  firstDoubled.c_str());
    check(doubled == 0, "no path is in both the presentation table and the omission table");

    // Belt and braces on the same claim, arithmetically: if every path is in
    // exactly one table and neither table has a stray entry (section C), the
    // two sizes must sum to the field count.
    check(presentation.size() + omission.size() == fieldPaths.size(),
          "presentation table size + omission table size == brushModelFieldPaths().size()");
  }

  // ==========================================================================
  std::printf("  -- C. neither table names a field that does not exist --\n");
  // ==========================================================================
  //
  // The mirror-image failure from section B: a path renamed or removed in
  // `BrushModelFields.hpp` that leaves a now-dangling row behind in either
  // table here. Section B alone would not catch this -- it only walks
  // `brushModelFieldPaths()` forward, never asks whether a table row was
  // reachable from there.
  {
    size_t stalePresentation = 0, staleOmission = 0;
    std::string firstStalePresentation, firstStaleOmission;
    for (const BrushFieldSpec& row : presentation) {
      if (fieldPathSet.count(row.path) == 0) {
        if (stalePresentation == 0) firstStalePresentation = row.path;
        ++stalePresentation;
      }
    }
    for (const BrushFieldOmission& row : omission) {
      if (fieldPathSet.count(row.path) == 0) {
        if (staleOmission == 0) firstStaleOmission = row.path;
        ++staleOmission;
      }
    }
    if (stalePresentation > 0)
      std::printf("  [measured] %zu stale presentation-table path(s), first: %s\n",
                  stalePresentation, firstStalePresentation.c_str());
    check(stalePresentation == 0,
          "every presentation-table path names a real BrushModel field");
    if (staleOmission > 0)
      std::printf("  [measured] %zu stale omission-table path(s), first: %s\n", staleOmission,
                  firstStaleOmission.c_str());
    check(staleOmission == 0, "every omission-table path names a real BrushModel field");
  }

  // ==========================================================================
  std::printf("  -- D. every omission carries an actual reason --\n");
  // ==========================================================================
  //
  // An empty reason string would still make sections A-C pass -- this is the
  // one property only readable from the table itself, so it gets its own
  // check rather than riding along with something else.
  {
    bool everyReasonNonEmpty = true;
    for (const BrushFieldOmission& row : omission) {
      if (row.reason == nullptr || row.reason[0] == '\0') everyReasonNonEmpty = false;
    }
    check(everyReasonNonEmpty, "every omission-table row carries a non-empty reason");
  }

  // ==========================================================================
  std::printf("  -- E. every presentation row carries an actual label --\n");
  // ==========================================================================
  {
    bool everyLabelNonEmpty = true;
    for (const BrushFieldSpec& row : presentation) {
      if (row.label == nullptr || row.label[0] == '\0') everyLabelNonEmpty = false;
    }
    check(everyLabelNonEmpty, "every presentation-table row carries a non-empty label");
  }

  std::printf("[selftest] brush panel binding %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
