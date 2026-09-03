#include "app/selftest/Support.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "app/ExportDialog.hpp"

namespace np {

// app/ExportDialog -- the decisions File > Export As... and File > Export Comps
// / Layers To Files... used to make inline, in two copies, with no coverage of
// any kind: neither dialog had a `--selftest` assertion or a golden view, so
// every one of these answers was checkable only by opening a modal by hand.
//
// **What would make this section worthless, stated first.** Three of these
// functions are thin, and a thin function asserted against a restatement of
// itself is a tautology. So each one is asserted against the *other* module's
// answer wherever one exists -- `exportFormatChoices()` against
// `offerableExportFormats()` and `allFormatCapabilities()`, `legaliseExportDepth()`
// against `offerableExportDepths()`, the blocked reasons against
// `ExportValidation::error` and `ExportStatesReport::error` -- rather than
// against a literal repeated from the implementation. The one place a literal
// is unavoidable (the two sentences this module contributes, io/Export having
// none for those cases) is asserted for the property that matters instead:
// that it ends in the same clause the rest of the build refuses in.
//
// Headless and GPU-free: no device, no window, no ImGui frame, no filesystem.
// The widgets these answers drive are photographed by
// `tools/golden/run_golden.sh`'s `export_as`, `export_as_blocked` and
// `export_states` views, which are the only place that drawing can be checked
// at all -- and which did not exist before this step for the plain reason that
// no launch argument could open either dialog.
bool runExportDialogTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] export dialog: which control is live, and what it says when it is "
              "not\n");

  // ---------------------------------------------------------------- resize
  //
  // The mapping the dialog draws its sub-control from. Asserted per enumerator
  // rather than as a count so a fourth mode cannot pass by arithmetic.
  check(exportResizeField(ExportResizeMode::None) == ExportResizeField::None &&
            exportResizeField(ExportResizeMode::Percent) == ExportResizeField::Percent &&
            exportResizeField(ExportResizeMode::FitWithin) == ExportResizeField::FitBox,
        "resize: each mode names the one numeric field it reads");
  {
    // Every mode is covered, counted off kExportResizeModeCount rather than
    // off the three cases above -- which is what makes adding a mode without
    // an editor a red line here.
    bool everyModeMapped = true;
    for (std::size_t i = 0; i < kExportResizeModeCount; ++i) {
      const auto m = static_cast<ExportResizeMode>(i);
      const ExportResizeField f = exportResizeField(m);
      const bool needsField = m != ExportResizeMode::None;
      if (needsField != (f != ExportResizeField::None)) everyModeMapped = false;
    }
    check(everyModeMapped,
          "resize: every mode but None asks for an editor, and None asks for one");
  }

  // ---------------------------------------------------------- format choices
  //
  // The fix for the two dialogs disagreeing. Asserted against BOTH of the
  // lists the two dialogs used to build their menus from, so the claim "these
  // are now the same menu" is measured against the originals rather than
  // asserted about itself.
  {
    const std::vector<ExportFormatChoice> choices =
        exportFormatChoices(ExportTargetSpace::Rec709Srgb, ExportBitDepth::UInt8);
    check(choices.size() == kImageFormatCount,
          "format menu: every format has a row -- an unwritable one is greyed, never "
          "omitted (the batch dialog used to omit them)");

    bool orderMatchesEnum = true;
    for (std::size_t i = 0; i < choices.size(); ++i)
      if (choices[i].format != static_cast<ImageFormat>(i)) orderMatchesEnum = false;
    check(orderMatchesEnum, "format menu: rows are in ImageFormat declaration order");

    const std::vector<ImageFormat> offerable = offerableExportFormats();
    std::size_t writableRows = 0;
    bool writableSetMatches = true;
    bool reasonExactlyWhenUnwritable = true;
    for (const ExportFormatChoice& c : choices) {
      const bool inOfferable =
          std::find(offerable.begin(), offerable.end(), c.format) != offerable.end();
      if (inOfferable != c.writable) writableSetMatches = false;
      if (c.writable) ++writableRows;
      if (c.writable != c.refusal.empty()) reasonExactlyWhenUnwritable = false;
    }
    check(writableSetMatches && writableRows == offerable.size(),
          "format menu: the writable rows are exactly offerableExportFormats() -- the "
          "greying adds rows, it never changes which ones work");
    check(reasonExactlyWhenUnwritable,
          "format menu: a row carries a reason exactly when it is not writable");

    // The reason must be io/Export's own, verbatim -- this module is not
    // allowed a second vocabulary. Compared against the function the dialog
    // used to call directly.
    bool reasonsAreExportsOwn = true;
    for (const ExportFormatChoice& c : choices) {
      if (c.writable) continue;
      if (c.refusal != exportRefusalReason(c.format, ExportTargetSpace::Rec709Srgb,
                                           ExportBitDepth::UInt8, nullptr, nullptr))
        reasonsAreExportsOwn = false;
    }
    check(reasonsAreExportsOwn,
          "format menu: every greyed row's tooltip is exportRefusalReason()'s string "
          "verbatim, not a reworded one");

    // PSD and camera raw are read-only in EVERY configuration -- the one part
    // of this list that does not depend on the build -- so they are the rows
    // that pin "unwritable formats are still shown" without depending on
    // whether this binary has OpenImageIO.
    bool psdShownAndRefused = false;
    bool rawShownAndRefused = false;
    for (const ExportFormatChoice& c : choices) {
      if (c.format == ImageFormat::Psd) psdShownAndRefused = !c.writable && !c.refusal.empty();
      if (c.format == ImageFormat::CameraRaw)
        rawShownAndRefused = !c.writable && !c.refusal.empty();
    }
    check(psdShownAndRefused && rawShownAndRefused,
          "format menu: PSD and camera raw appear, greyed, with a reason -- read-only in "
          "every build, so this holds in both configurations");
  }

  // --------------------------------------------------------- depth legalising
  //
  // Asserted against `offerableExportDepths()` for every (format, depth) pair
  // in the build rather than for a hand-picked one, so this cannot pass by
  // agreeing with itself about PNG.
  {
    bool alwaysLegal = true;
    bool keepsLegalUnchanged = true;
    bool picksTheFirst = true;
    for (std::size_t f = 0; f < kImageFormatCount; ++f) {
      const auto format = static_cast<ImageFormat>(f);
      const std::vector<ExportBitDepth> depths = offerableExportDepths(format);
      for (std::size_t d = 0; d < kExportBitDepthCount; ++d) {
        const auto depth = static_cast<ExportBitDepth>(d);
        const ExportBitDepth got = legaliseExportDepth(format, depth);
        const bool wasLegal = std::find(depths.begin(), depths.end(), depth) != depths.end();
        if (depths.empty()) {
          // Nothing better to answer; leaving it alone is what lets the
          // refusal below the combo be about the format rather than about a
          // depth the dialog silently rewrote.
          if (got != depth) alwaysLegal = false;
          continue;
        }
        if (std::find(depths.begin(), depths.end(), got) == depths.end()) alwaysLegal = false;
        if (wasLegal && got != depth) keepsLegalUnchanged = false;
        if (!wasLegal && got != depths.front()) picksTheFirst = false;
      }
    }
    check(alwaysLegal,
          "depth: legalise() answers a depth the format can be written at, for every "
          "format-depth pair this build has");
    check(keepsLegalUnchanged,
          "depth: a depth the new format already allows is left exactly alone");
    check(picksTheFirst, "depth: an illegal one falls to the format's first writable depth");
  }

  // ------------------------------------------------------- request equality
  //
  // The mode-aware half is the whole point -- see app/ExportDialog.hpp §2.
  {
    ExportRequest a;
    ExportRequest b;
    check(exportRequestsEqual(a, b), "equality: two default requests are equal");

    b = a;
    b.format = ImageFormat::Jpeg;
    check(!exportRequestsEqual(a, b), "equality: a different format is a different request");
    b = a;
    b.targetSpace = ExportTargetSpace::Rec709Linear;
    check(!exportRequestsEqual(a, b), "equality: a different colour space is different");
    b = a;
    b.bitDepth = ExportBitDepth::UInt16;
    check(!exportRequestsEqual(a, b), "equality: a different bit depth is different");
    b = a;
    b.resize.mode = ExportResizeMode::Percent;
    check(!exportRequestsEqual(a, b), "equality: a different resize mode is different");

    // The two that a memberwise compare would get wrong, and the two it would
    // get right -- asserted as a pair so neither half can be dropped.
    a.resize.mode = ExportResizeMode::Percent;
    a.resize.percent = 50.0f;
    a.resize.maxWidth = 2048;
    b = a;
    b.resize.maxWidth = 512;
    b.resize.maxHeight = 77;
    check(exportRequestsEqual(a, b),
          "equality: in Percent mode the FitWithin box is not read, so changing it does "
          "NOT make a preset 'modified'");
    b = a;
    b.resize.percent = 25.0f;
    check(!exportRequestsEqual(a, b), "equality: in Percent mode the percentage IS read");

    a.resize.mode = ExportResizeMode::FitWithin;
    b = a;
    b.resize.percent = 3.0f;
    check(exportRequestsEqual(a, b),
          "equality: in FitWithin mode the percentage is not read, and does not either");
    b = a;
    b.resize.maxHeight = 512;
    check(!exportRequestsEqual(a, b), "equality: in FitWithin mode the box IS read");

    a.resize.mode = ExportResizeMode::None;
    b = a;
    b.resize.percent = 12.5f;
    b.resize.maxWidth = 9;
    check(exportRequestsEqual(a, b),
          "equality: at document size neither resize number is read");
  }

  // ------------------------------------------------------------ preset label
  //
  // The control that used to read "Load a preset..." forever, including
  // immediately after loading one.
  {
    ExportPreset preset;
    preset.name = "Web 2048";
    preset.request.format = ImageFormat::Jpeg;
    preset.request.resize.mode = ExportResizeMode::FitWithin;
    preset.request.resize.maxWidth = 2048;
    preset.request.resize.maxHeight = 2048;

    ExportRequest current = preset.request;
    check(exportPresetMenuLabel(nullptr, current) == "Custom",
          "preset label: nothing loaded reads 'Custom'");
    check(exportPresetMenuLabel(&preset, current) == "Web 2048",
          "preset label: a loaded, untouched preset reads its own name");
    current.resize.maxWidth = 1024;
    check(exportPresetMenuLabel(&preset, current) == "Web 2048 (modified)",
          "preset label: editing a control the request reads marks it modified");
    current = preset.request;
    current.resize.percent = 42.0f;
    check(exportPresetMenuLabel(&preset, current) == "Web 2048",
          "preset label: editing a control the request does NOT read leaves it unmodified");
  }

  // ------------------------------------------------------ Export As blocking
  //
  // Every branch, in order, plus the ordering itself: a dialog with two things
  // wrong must name the one the user hits first.
  {
    ExportValidation good;
    good.ok = true;
    good.outWidth = 800;
    good.outHeight = 600;
    ExportValidation bad;
    bad.ok = false;
    bad.error = "export refused: this build cannot write EXR. <capability reason>";

    check(exportAsBlockedReason(true, good, "/tmp/a.png").empty(),
          "export as: a document, a valid request and a path leaves the button live");
    check(!exportAsBlockedReason(false, good, "/tmp/a.png").empty(),
          "export as: no document blocks it");
    check(exportAsBlockedReason(true, bad, "/tmp/a.png") == bad.error,
          "export as: an invalid request reports ExportValidation's own error verbatim");
    check(!exportAsBlockedReason(true, good, "").empty(),
          "export as: an empty output path blocks it -- and now SAYS so, which is the "
          "case that used to grey the button silently");
    check(exportAsBlockedReason(false, bad, "") ==
              exportAsBlockedReason(false, good, "/tmp/a.png"),
          "export as: with three things wrong the no-document reason wins -- there is "
          "nothing to export before there is a way to export it");
    check(exportAsBlockedReason(true, bad, "") == bad.error,
          "export as: with a bad request and no path, the request's refusal wins");

    // The one sentence with no upstream owner: it must end in the clause the
    // rest of this build already refuses in, or it is a third phrasing of one
    // fact. `ui/MacPaintUI.cpp:351` and `:5507` and the title band.
    const std::string kClause = "no document is open. File > New Document makes one.";
    const std::string noDoc = exportAsBlockedReason(false, good, "/tmp/a.png");
    check(noDoc.size() > kClause.size() &&
              noDoc.compare(noDoc.size() - kClause.size(), kClause.size(), kClause) == 0,
          "export as: the no-document sentence ends in the build's own clause, with only "
          "the lead-in noun differing");
  }

  // -------------------------------------------------- Export states blocking
  {
    ExportStatesReport clean;
    clean.ok = true;
    ExportStatesReport refused;
    refused.error = "export refused: the output directory '/nope' does not exist.";

    check(exportStatesBlockedReason(true, 3, clean).empty(),
          "export states: a document, a selection and a clean plan leaves it live");
    check(!exportStatesBlockedReason(false, 3, clean).empty(),
          "export states: no document blocks it");
    check(!exportStatesBlockedReason(true, 0, clean).empty(),
          "export states: an empty selection blocks it");
    check(exportStatesBlockedReason(true, 3, refused) == refused.error,
          "export states: a refused plan reports ExportStatesReport's own error verbatim");
    check(exportStatesBlockedReason(true, 0, refused) ==
              exportStatesBlockedReason(true, 0, clean),
          "export states: an empty selection is reported ahead of the plan -- an empty "
          "`selection` means 'all of them' to io/ExportStates, so its plan is about a "
          "different question");
    check(exportStatesBlockedReason(false, 0, refused) ==
              exportAsBlockedReason(false, ExportValidation{}, "/tmp/a.png"),
          "export states: both dialogs give the SAME no-document sentence -- one clause, "
          "one place, two callers");
  }

  std::printf("[selftest] export dialog %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
