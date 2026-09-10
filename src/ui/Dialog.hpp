#pragma once

#include <cstddef>
#include <string>

#include "imgui.h"

// ui/Dialog -- the one shape every modal dialog in this application takes.
//
// docs/modal-screenshots/README.md photographed thirty-four dialogs and found
// nine different commit-button labels, labels trailing their controls in 33 of
// 34, no Escape key anywhere, a 1766 px Image Size dialog whose width was set
// by one unwrapped sentence, and eight dialogs pinned to the top edge while
// twenty-six centred. None of that was any one dialog's fault: each was written
// against no shared rule, so each made its own. This module is the rule.
//
// What a dialog built from it gets, without doing anything:
//
//   * **A fixed width** (`DialogWidth`), so prose wraps and the window's size
//     is a design decision rather than the length of its longest line. This is
//     also what fixes placement: ImGui centres a modal once, on the frame it
//     first sizes it, and a `TextWrapped()` with no wrap width is estimated
//     tall on that frame, clamped to the top edge, and never re-centred. A
//     known width makes the first estimate the right one.
//   * **A label column** to the left of every control, right-aligned, the way
//     macOS and Windows both lay a form out. Dear ImGui's default is a trailing
//     label; `ui/LabelledControl` already argued that down for the docked
//     column, and this is the same argument for dialogs. It has its own column
//     rather than borrowing that module's because that column is measured
//     across the docked panels each frame and reported as a diagnostic -- a
//     dialog wants a constant, not a moving target.
//   * **A slider that is a slider and a field.** `dialogSlider()` draws the
//     track with no value text and a numeric field beside it, so the value is
//     readable under a grab that used to overlap it, and typeable exactly. The
//     unit lives after the field, not inside it.
//   * **One footer**, right-aligned: Cancel, then the commit button drawn as
//     the default in the accent. Return commits, Escape cancels, both once per
//     press. A third, destructive choice ("Don't Save") sits alone at the
//     left, and no key reaches it -- app/CloseDecision's rule, now every
//     dialog's.
//   * **Error and warning colour from the theme** (`kError`, `kWarning`,
//     ui/AtelierTheme.hpp), not a literal per call site.
//
// Keyboard, and the one thing to know about it: this build does not set
// `ImGuiConfigFlags_NavEnableKeyboard`, on purpose -- Tab, Space and the arrows
// are tool keys on a canvas -- so ImGui itself will neither close a modal on
// Escape nor press a default button on Return. The footer reads both keys
// itself, once, and records the frame it did so in
// `dialogHandledKeyThisFrame()`; anything else in the application that reads
// those same bare keys (the transform gizmo's own Return/Escape, for one)
// checks that predicate and stands down. Without it a Return in the Numeric
// Transform dialog committed the session twice: once here, then once more in
// the canvas block, which by then found no session and reported an error.

namespace np {

// Window widths in ImGui points. `Standard` is every parameter dialog;
// `Wide` is one that carries a list or a form with a path field (New Document,
// the two Export dialogs).
enum class DialogWidth { Standard, Wide };

constexpr float kDialogStandardWidth = 460.0f;
constexpr float kDialogWideWidth = 640.0f;

// The right-aligned label column, in points. Wide enough for the longest label
// any dialog here carries ("Preserve luminosity" is a checkbox and sits in the
// control column instead) -- a label that does not fit is a label to shorten,
// not a column to widen; the unit goes after the field, the qualifier in a
// hint below.
constexpr float kDialogLabelColumn = 116.0f;

// Wraps `ImGui::BeginPopupModal()`: centres the window on the frame it
// appears, pins its width, and caps its height at 85% of the viewport so a long
// dialog scrolls inside itself rather than running off the screen (Export Comps
// was 85% of the window before this and had nowhere to grow). Returns false
// when the popup is not open, exactly as BeginPopupModal does; the caller
// still owns `OpenPopup()`.
bool beginDialog(const char* title, DialogWidth width = DialogWidth::Standard);
void endDialog();

// --- Content -----------------------------------------------------------------

// What a control reports. `changed` is true on every frame the value moved
// (a slider mid-drag, a field mid-typing); `settled` is true once, when the
// user let go or pressed Return in the field. Cheap point ops preview on
// `changed`; a filter whose recompute costs hundreds of milliseconds previews
// on `settled` -- ui/MacPaintUI.cpp's Gaussian Blur dialog carries the
// measurement that decided which is which.
struct DialogEdit {
  bool changed = false;
  bool settled = false;
  explicit operator bool() const noexcept { return changed; }
  DialogEdit& operator|=(const DialogEdit& o) noexcept {
    changed |= o.changed;
    settled |= o.settled;
    return *this;
  }
};

// A slider plus a numeric field, sharing one value. `fmt` formats the field;
// `unit`, when given, is drawn after it in the secondary colour.
DialogEdit dialogSlider(const char* label, float* v, float lo, float hi,
                        const char* fmt = "%.2f", const char* unit = nullptr);
DialogEdit dialogSliderInt(const char* label, int* v, int lo, int hi, const char* unit = nullptr);

// A drag-field alone (no track), for values with no natural range.
DialogEdit dialogDrag(const char* label, float* v, float speed, float lo, float hi,
                      const char* fmt = "%.1f", const char* unit = nullptr);

DialogEdit dialogInputInt(const char* label, int* v, const char* unit = nullptr);
DialogEdit dialogInputText(const char* label, char* buf, size_t cap, ImGuiInputTextFlags flags = 0);

// BeginCombo laid out in the column; the caller ends it with EndCombo().
bool dialogBeginCombo(const char* label, const char* preview);
bool dialogCombo(const char* label, int* idx, const char* const items[], int count);

// The box sits at the control column with its label to the right -- a
// checkbox's label is part of the control, not a field label.
bool dialogCheckbox(const char* label, bool* v);

// A row of radio buttons at the control column, `label` in the label column.
bool dialogRadioRow(const char* label, int* idx, const char* const items[], int count);

// RGB colour edit in the column (float, linear -- the caller says what space).
DialogEdit dialogColor(const char* label, float rgb[3], ImGuiColorEditFlags flags = 0);

// Draws `label` in the column and leaves the cursor at the control column,
// with the remaining width in `*availOut`, for content none of the helpers
// above fit (a histogram, a curve editor, a preset list).
void dialogLabelRow(const char* label, float* availOut = nullptr);

// Spacing, then a caps heading in the mono face with a rule -- the section
// divider the docked column's headers use, at dialog scale.
void dialogSection(const char* title);

// Prose. `dialogText` is primary; `dialogHint` is the secondary explanation
// under a control. Both wrap at the dialog's width, which is the whole point.
void dialogText(const char* fmt, ...) IM_FMTARGS(1);
void dialogHint(const char* fmt, ...) IM_FMTARGS(1);

// A status line in the theme's colour for its kind, wrapped. Draws nothing for
// an empty string, so a dialog's `status` can be passed unconditionally.
enum class DialogStatus { Info, Warning, Error };
void dialogStatusLine(DialogStatus kind, const std::string& text);

// The colour a status kind is drawn in -- the theme's `kError` / `kWarning`
// tokens, or the secondary text colour -- for the few places that colour a
// widget other than a status line (a report row, a swatch) and used to spell
// the literal out instead.
ImVec4 dialogStatusColor(DialogStatus kind);

// --- Footer ------------------------------------------------------------------

enum class DialogAction { None, Commit, Cancel, Alternate };

struct DialogFooter {
  // The default button. `nullptr` draws no commit button (a dialog that only
  // offers to be dismissed), and Return then does nothing.
  const char* commit = "OK";
  bool commitEnabled = true;
  // Drawn in the error colour instead of the accent: for the one dialog
  // whose default action destroys work ("Discard and Revert"), so the colour
  // says what the label says.
  bool commitDestructive = false;
  // `nullptr` draws no Cancel; Escape then maps to Commit if that is the only
  // button (an "OK"-only dialog), else to nothing.
  const char* cancel = "Cancel";
  // A third choice, alone at the left edge, never reachable by a key. Drawn
  // in the error colour when `alternateDestructive`.
  const char* alternate = nullptr;
  bool alternateDestructive = false;
  // A short note at the left of the footer ("Changes apply as you make them."),
  // for the dialogs whose commit model is not the default one.
  const char* note = nullptr;
};

// Draws the separator and the button row, reads Return and Escape, and says
// what happened. Does NOT close the popup -- the caller decides, because some
// commits refuse and keep the dialog up with the reason.
DialogAction dialogFooter(const DialogFooter& f);

// True during the frame in which a footer acted on Return or Escape.
bool dialogHandledKeyThisFrame();

}  // namespace np
