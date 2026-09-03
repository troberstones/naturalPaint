#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "io/ExportAs.hpp"
#include "io/ExportStates.hpp"

// app/ExportDialog -- the decisions the two export dialogs make, lifted out of
// ui/MacPaintUI.cpp so `--selftest` can assert them without a window.
//
// The same split app/CurveEdit, app/FramePacing and app/ToolSurface already
// have, and here for the same reason those three record: the piece of a widget
// that can be wrong in a way a screenshot will not show is the piece that
// belongs in a testable module. What was left in ui/ after this module landed
// is genuinely only ImGui.
//
// ==========================================================================
// 0. What this module is NOT
// ==========================================================================
//
// **Not a third vocabulary.** io/ExportAs owns the words for formats, spaces,
// depths and resizes; io/Export owns every refusal string; io/ExportStates
// owns the batch's plan and its per-file report; ui/MenuModel owns the two
// menu item names. Every string this module returns is one of theirs verbatim,
// or -- for the two cases none of them has a sentence for -- the clause this
// build already refuses in elsewhere (see §3). Nothing here rewords an
// encoder's message, because a reworded refusal is a message that drifts.
//
// **Not a validator.** `validateExportRequest()` (io/ExportAs) and
// `planStateExport()` (io/ExportStates) already answer "is this legal". This
// module answers the question a *dialog* has that neither of them does:
// **which control is live, and when it is not, what sentence goes next to
// it.** That is a different question with a different answer -- a request can
// be perfectly legal and still have no output path typed, which is not a
// validation failure and is exactly why the Export button used to sit greyed
// with nothing on screen saying why.
//
// ==========================================================================
// 1. Why both dialogs call the same format-menu function
// ==========================================================================
//
// The two dialogs disagreed about this, and the disagreement is the clearest
// single piece of evidence for the report this module exists to answer ("the
// export dialog seems a little confusing"):
//
//   * File > Export As... built its Format combo from `allFormatCapabilities()`
//     and showed every format, greying the ones this build cannot write with
//     the capability query's own reason as the tooltip. Its own comment gives
//     the argument: "a menu that silently omits EXR cannot answer 'why can't I
//     export EXR?'".
//   * File > Export Comps / Layers To Files... built its Format combo from
//     `offerableExportFormats()`, which omits exactly those formats -- with no
//     tooltip, no reason, and no trace that they exist.
//
// So the same question, asked one dialog apart, got two answers, and the
// dialog that gave the *worse* one is the batch dialog, where a wrong format
// costs a whole folder of files rather than one. `exportFormatChoices()` is
// the single answer both now call, which makes agreement a property of the
// code rather than of two authors remembering the same rule.
//
// `offerableExportFormats()` is untouched and still correct for what it is
// documented as -- "the list a format combo box is built from" for a caller
// that wants only the writable ones; io/ExportStates' own `planStateExport()`
// still refuses an unwritable request through `exportRequestAvailability()`,
// which is the check that actually protects the batch. This module changes
// which of the two a *menu* is drawn from, not which one the engine trusts.
//
// ==========================================================================
// 2. Why the preset label needs its own equality, and why that equality is
//    mode-aware
// ==========================================================================
//
// Both dialogs' preset combos showed a fixed literal -- "Load a preset..." and
// "Load an Export As preset..." -- forever. Loading a preset changed the
// controls and left the combo saying "Load a preset...", so the one control
// whose job is to tell you which preset you are on never told you. That is
// `exportPresetMenuLabel()`'s whole reason to exist.
//
// The subtlety is the third case. Photoshop's idiom -- and the only honest one
// -- is that a preset whose controls have since been edited shows as modified
// rather than continuing to claim the preset's name. Deciding that needs an
// equality over `ExportRequest`, and a *memberwise* one would be wrong:
// `ExportResize`'s own header says "Only the fields its `mode` uses are read,
// so switching modes in a dialog never silently loses the other mode's
// numbers." A dialog that keeps `maxWidth` alive while the user is in Percent
// mode is doing exactly what that comment promises -- and a memberwise compare
// would then report the preset "modified" because of a number the request does
// not read and the export will never see.
//
// So `exportRequestsEqual()` compares the fields the mode actually uses, and
// nothing else. --selftest pins both halves: two requests differing only in an
// unread field are equal, and two differing in a read one are not.
//
// ==========================================================================
// 3. The two sentences this module contributes, and where they come from
// ==========================================================================
//
// `exportAsBlockedReason()` and `exportStatesBlockedReason()` answer "why is
// the Export button off". Three of their four cases quote an existing message
// verbatim (`ExportValidation::error`, `ExportStatesReport::error`, and the
// batch dialog's own empty-selection line). The two that had no owner are:
//
//   * **No document.** "Nothing to export: no document is open. File > New
//     Document makes one." -- the second clause is `ui/MacPaintUI.cpp:351`,
//     `:5507` and the title band's own clause verbatim, and the lead-in noun
//     varies exactly the way app/ToolSurface.hpp §"The voice" records those
//     sites already varying theirs ("layer command refused:", "Nothing to
//     sample:"). A third phrasing of one fact is a third thing to keep in step.
//   * **No output path.** Nothing else in this build asks for one, so this is
//     a genuinely new sentence; it names the two ways to supply the thing that
//     is missing rather than only that it is missing.
//
// A dialog that greys a button and says nothing is the worst of the three
// available behaviours -- worse than refusing on click, which at least
// produces a message. Both dialogs did it: Export As with an empty output
// path, and the batch dialog with an empty selection said its reason next to
// the *plan* rather than next to the button.
namespace np {

// Which numeric field a resize mode reads, and therefore which sub-control a
// dialog draws under the Resize combo. Derived from `ExportResizeMode` rather
// than from a parallel list, so a fourth mode is a compile error here instead
// of a mode with no editor.
enum class ExportResizeField {
  // `ExportResizeMode::None` -- the document's own size, nothing to edit.
  None,
  // `ExportResizeMode::Percent` -- one slider over `ExportResize::percent`.
  Percent,
  // `ExportResizeMode::FitWithin` -- two integers over `maxWidth`/`maxHeight`.
  FitBox,
};

ExportResizeField exportResizeField(ExportResizeMode mode);

// One row of a Format combo: the format, whether this build can write it, and
// -- when it cannot -- io/Export's own reason, ready to be a tooltip.
struct ExportFormatChoice {
  ImageFormat format = ImageFormat::Png;
  bool writable = false;
  // Empty exactly when `writable`. `exportRefusalReason()`'s string verbatim.
  std::string refusal;
};

// Every format, in `ImageFormat` declaration order, with the ones this build
// cannot write marked and explained rather than omitted. See §1.
//
// `space` and `depth` are the dialog's *current* settings and are passed
// through to `exportRefusalReason()` so the reason is the one that applies to
// what is on screen. The format-support check short-circuits ahead of the
// depth check inside that function, so an unwritable format's reason is always
// about the format -- but passing the live values costs nothing and keeps this
// from becoming a place where a default is quietly assumed.
std::vector<ExportFormatChoice> exportFormatChoices(ExportTargetSpace space, ExportBitDepth depth);

// `current` if `format` can be written at it in this build, otherwise the
// first depth it can. Falls back to `current` unchanged for a format with no
// writable depth at all, because there is no better answer and silently
// rewriting the control to an equally impossible value would only hide the
// refusal the dialog is about to print.
//
// Both dialogs already did this inline, in two copies, on every format change.
ExportBitDepth legaliseExportDepth(ImageFormat format, ExportBitDepth current);

// Whether two requests would produce the same file. Mode-aware over the resize
// half -- see §2 for why memberwise would be wrong.
bool exportRequestsEqual(const ExportRequest& a, const ExportRequest& b);

// The preview text a preset combo shows: "Custom" with nothing loaded,
// `loaded->name` while the controls still match it, and
// `loaded->name + " (modified)"` once they do not.
std::string exportPresetMenuLabel(const ExportPreset* loaded, const ExportRequest& current);

// Why File > Export As...'s Export button is off, or empty when it is live.
// §3 sources every sentence.
std::string exportAsBlockedReason(bool documentOpen, const ExportValidation& validation,
                                  std::string_view outputPath);

// The same for File > Export Comps / Layers To Files...'s Export button.
// `selectedCount` is how many checkboxes are ticked; `plan` is
// `planStateExport()`'s report, which is only meaningful when something is
// selected and is therefore consulted second.
std::string exportStatesBlockedReason(bool documentOpen, std::size_t selectedCount,
                                      const ExportStatesReport& plan);

}  // namespace np
