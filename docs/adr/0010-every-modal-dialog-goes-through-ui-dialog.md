# ADR-0010 — Every modal dialog goes through `ui/Dialog`

**Status:** accepted · **Date:** 2026-09-10

## Context

[docs/modal-screenshots/README.md](../modal-screenshots/README.md) photographed every
modal dialog in the application (thirty-four at the time) and measured what building them
one at a time, each against no shared rule, had produced: nine different commit-button
labels; labels trailing their controls in 33 of 34; no Escape key anywhere, because this
build deliberately leaves `ImGuiConfigFlags_NavEnableKeyboard` off and ImGui's Escape
handling is gated on it; one dialog 1766 px wide because a single unwrapped sentence
citing a header file set its width; eight dialogs pinned to the top edge while twenty-six
centred; nineteen call sites each inventing their own red and amber; one "dialog" that
was a non-modal `BeginPopup` and dismissed itself when anything took focus.

None of that was any one dialog's fault, and none of it was caught by anything. Each
dialog compiled, opened, drew, and passed every runtime assertion the suite has -- the
suite measures what a dialog *does*, and a dialog with no Escape key does everything it
is asked. The golden harness photographs three of them. The other thirty-one had never
been looked at side by side until the survey put them on one page.

The survey proposed a nine-item standard (S1–S9). Commit `30b7525` built it as one
module, [`src/ui/Dialog.hpp`](../../src/ui/Dialog.hpp), and moved all thirty-five dialogs
onto it. This ADR is what keeps the thirty-sixth from being written the old way.

## Decision

**A modal dialog in this application is `beginDialog()` … `dialogFooter()` …
`endDialog()`, and nothing else.** Concretely:

1. **`ImGui::BeginPopupModal()` is called in exactly one place in `src/ui`:
   `ui/Dialog.cpp`.** A dialog does not call it, wrap it, or copy it. The module owns the
   window flags, the width, the centring, the height cap, the padding and the scrim.
2. **The rule is asserted, not described.** `app/selftest/DialogModule.cpp` scans the
   `src/ui` this binary was built from (`NP_UI_SOURCE_DIR`), strips comments, and fails
   the suite on any `BeginPopupModal(` outside `ui/Dialog.cpp`, and on either of the two
   colour literals the module retired (`0.95f, 0.45f, 0.40f`, `0.92f, 0.78f, 0.35f`).
   It also asserts it scanned a plausible tree and found the module's own one call, so a
   wrong directory is a FAIL, not a clean bill.
3. **Exceptions are a table with a sentence each, asserted empty.** `kAllowedBareModals`
   in that file is where a dialog that genuinely cannot use the module goes, with the
   reason beside it; the suite prints the table's size, so adding a row is a visible act
   in a diff and a test line, never a quiet omission from the scan. There is no known
   candidate for the table.
4. **What the module gives, a dialog does not re-decide:** fixed width (460 pt, or 640 for
   a list or a path field); a right-aligned 116 pt label column via the `dialog*()`
   controls; prose wrapped at the dialog's width through `dialogText()` / `dialogHint()`;
   one right-aligned footer, Cancel then the commit button drawn as the default in the
   accent; `Apply` when the dialog previews and a verb when it does not, never the
   dialog's own name; Escape cancels and Return commits, read once by the footer; a
   destructive third choice alone at the left where no key reaches it; error and warning
   text through `dialogStatusLine()` in the theme's `kError` / `kWarning`. A dialog that
   needs something the module lacks extends the module.
5. **A dialog's own strings do not cite source files.** `ops/…hpp` and `core/…hpp` belong
   in the comment the sentence came from. This applies to engine strings that reach a
   dialog (the export validators) as much as to the dialog's own text.

## What this does not decide

- **`BeginPopup()` is not guarded.** Context menus, the tool flyout, combo popups and
  colour-picker popovers are legitimately non-modal, and there is no textual test that
  tells a menu from a dialog written as a menu (S9's Add Guide was the latter). That
  distinction is left to review, with the contact sheet as the evidence.
- **Using the module well.** A dialog can call `beginDialog()` and then lay out trailing
  labels by hand. `tools/modal-shots/capture_modals.sh` is where a human sees that; the
  suite only closes the mechanical loophole.
- **Whether keyboard navigation should be turned on globally.** Leaving
  `NavEnableKeyboard` off is a tool-key decision (Tab, Space and the arrows are canvas
  keys), and the footer reads its two keys itself for that reason. If that decision is
  ever reversed, `dialogFooter()` is the one place that changes.

## Consequences

- A new dialog is shorter to write than it was: there is nothing to decide about
  placement, width, footer, keys or colour, and the reviewer has nothing to check on
  those axes either.
- A dialog on a branch that predates this ADR fails the suite at merge, by name. The
  first known case is the `automation` branch's BATCH dialog, written against bare
  `BeginPopupModal()` before the module existed; the merge of `main` into that branch is
  where it moves onto the module.
- The suite now reads the source tree. `NP_UI_SOURCE_DIR` is a compile-time path into the
  checkout that built the binary, the same convention as `NP_SVG_TEST_DIR`; a binary
  copied elsewhere fails this section loudly (the directory check is the first line),
  which is the intended reading -- this is a developer/CI-time assertion, not a shipped
  one.
- `docs/ui.md` §5a is the design-side statement of the same rule and links here.
