#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "app/DocumentLifecycle.hpp"
#include "io/Json.hpp"

// app/Command -- one door every recordable document edit goes through, and
// the table of what those edits are.
//
// docs/automation-plan.md §1 and step 1. This is the seam a recorder taps and
// a replayer drives, and building it is most of what phase 19 costs; the
// vocabulary it indexes already existed, spread across four places that had
// no reason to know about each other.
//
// **Adding a command, rather than reading how the table came to exist?**
// docs/automation.md is written for that: the four edits a new recordable
// feature needs (a row here, an encoder beside its reader, a UI boundary, a
// `coverageFor()` classification), the parameter rules and which assertion
// enforces each, and the two places where nothing does.
//
// ==========================================================================
// (1) The rule that decides what is in this table, and it is not a taste call
// ==========================================================================
//
// **A command is recordable iff it can be expressed as a function of an
// `OpenDocument` alone.**
//
// That one line sorts `ui/MenuModel.hpp`'s ninety-four `MenuAction`s without
// anyone arguing about any of them. `Zoom In`, `Fit Window`, `Toggle
// Grayscale Preview`, tool selection and panel layout are functions of
// `AppState` -- session state, not the document -- so they are not here, and
// their absence is not a gap to be filled later. `Flatten Image`, `Gaussian
// Blur`, `Set Blend Mode` and `Image Size` are functions of the document, so
// they are.
//
// The rule is visible in each existing function's own signature, which is why
// `--selftest`'s exhaustiveness section can check the table against the four
// vocabularies rather than against a list someone maintains by hand.
//
// ==========================================================================
// (2) Why this is not `performMenuAction()`
// ==========================================================================
//
// `ui/MenuModel.hpp:730`'s `performMenuAction(AppState&, ...)` is the *UI*
// door: both menu backends route through it, which is what makes them
// incapable of disagreeing. It takes an `AppState&`, so it cannot run in
// `--batch`, where there is no window, no GPU and no session -- and a command
// that cannot run headlessly is a command whose batch behaviour would differ
// from its interactive behaviour, silently.
//
// So the UI door calls this one, never the reverse. `applyCommand()` knows
// about an `OpenDocument` and nothing above it.
//
// ==========================================================================
// (3) A command's identity is a string, and never an ordinal
// ==========================================================================
//
// `io/OpSerial` already refuses to key its format by an enum ordinal, for the
// reason `core/OpStack.hpp` gives: the enum is appended to. The same applies
// twice over here, because a `.npaction` file is read by builds that have
// more commands than the one that wrote it.
//
// It is not a *label* either. `layerCommandLabel()` returns menu text ("New
// Pigment Layer"); menu text is UI copy and gets reworded, and a file keyed by
// it would break when someone improved a menu. Ids are lower_snake_case, are
// never reused for a different meaning, and are what a human reads in the
// file (PRD P5).
namespace np {

// One recorded or replayed edit. `params` is always an object -- possibly
// empty, never null -- so a reader never has to distinguish "no parameters"
// from "not an object".
struct Command {
  std::string id;
  JsonValue params = JsonValue::object();
};

// What one command did.
//
// **`texelsChanged == 0` with `ok == true` is a real and reportable outcome,
// not a contradiction.** `applyImageSize()` states the rule this shape exists
// to carry -- "a no-op the user asked for is not an edit" -- and in the UI
// that is a nothing-happens the user can see. In a batch it is thirty files
// written unmodified and reported as successes, so the replayer turns a zero
// here into a warning rather than a silent pass (docs/automation-plan.md §7).
// `changesPixels` says whether the command is one where that number means
// anything at all: `select_layer` changes no texels by construction.
struct CommandResult {
  bool ok = false;
  // One sentence, in this codebase's refusal style: name the thing, name the
  // reason. **Never empty**, in either direction.
  std::string status;
  std::vector<std::string> warnings;
  size_t texelsChanged = 0;
  bool changesPixels = false;
};

// One row of the table.
struct CommandSpec {
  // Stable, lower_snake_case, written into `.npaction` files. See §3.
  const char* id;
  // Human text for the ACTIONS panel's step list. Free to be reworded; the id
  // is not.
  const char* label;
  // The parameter keys this command reads, in the order it writes them. Used
  // by the panel, and by `--selftest` to assert that every key an applier
  // reads is one the table advertises.
  std::vector<std::string> paramNames;
  // Whether this command could run on this document right now, and why not.
  // The replayer consults it before applying, which is how a step that the UI
  // would have greyed out becomes a named refusal rather than a silent no-op.
  // Returns an empty string when the command is available.
  std::string (*unavailableReason)(const OpenDocument& doc, const JsonValue& params);
  // Does the thing. Only called when `unavailableReason()` returned empty.
  CommandResult (*apply)(OpenDocument& doc, const JsonValue& params);

  // **True when an ABSENT selection silently means "the whole canvas" for this
  // command.** That is the whole definition, and it is narrower than "reads
  // the selection" on purpose.
  //
  // app/Recorder §4 is the consumer: a step taken under a live marquee that no
  // saved channel matches is refused at record time, because a selection is
  // session state and never reaches a file (docs/automation-plan.md §7), so
  // the step would replay with nothing selected -- and nothing selected means
  // no restriction, so it would cover the whole canvas and report success.
  // Nothing in the file would be wrong; the file would be missing the half of
  // the state that made the step mean what it meant.
  //
  // The recorder used to decide this from `CommandResult::changesPixels`, and
  // that proxy is wrong in both directions:
  //
  //  * `image_size`, `canvas_size` and `trim_to_content` all report changing
  //    pixels and none of them is bounded by the selection -- each acts on the
  //    whole document by construction. Recording a resize under a live marquee
  //    was refused for a reason that does not apply to it, with a fix that
  //    would not have changed anything.
  //  * `define_pattern` changes no pixel at all and IS bounded: its source
  //    rectangle is the selection's bounds, and absent means the whole canvas
  //    (app/CommandsPatterns.cpp says so at the fallback). A recorded
  //    define_pattern used to sail through and replay as a pattern the size of
  //    the document.
  //
  // **`changesPixels` was also not being set consistently, which is worth
  // knowing before anyone reaches for it again.** Only `fromFilterResult()`,
  // `fromDocumentOutcome()` and `fromDocumentTransform()` set it
  // (app/CommandSupport.hpp); `fromLayerEdit()` and `fromDocumentOpResult()`
  // do not. So every `LayerCommand`, every layer setter and every op-stack row
  // reports `false` -- `flatten_image` and `merge_down` included, which plainly
  // do change pixels. That did no harm to the marquee rule, because none of
  // them is selection-bounded either, but it means the old proxy was not even
  // measuring what its name says. Step 5 wants this field for the "a step that
  // changed zero texels is a warning" rule, and will have to fix it first.
  //
  // The commands that *operate on* the selection -- `select_grow`,
  // `invert_selection`, `save_selection_as_channel` -- are **not** bounded.
  // The selection is their operand rather than a mask on their reach, and an
  // absent one is a named refusal from their own precondition rather than a
  // silent "everything". `save_selection_as_channel` in particular has to stay
  // recordable: it is the fix the recorder's refusal tells the user to apply,
  // and a recorder that refused to record the escape hatch would be a closed
  // loop.
  //
  // **The default is false, and `--selftest` is what makes that safe.** Fifty
  // of the sixty-odd rows are not bounded, so writing `false` on each would be
  // noise a reader learns to skip. What stops a new bounded row from being
  // forgotten is not this default but an assertion: app/selftest/Command.cpp
  // section H walks the whole table and requires every row whose precondition
  // is `pixelOpUnavailable` -- which is every command that reaches the
  // selection-aware `applyPixelFilter()` bridge -- to carry this flag, and
  // requires the rows that carry it *without* that precondition to be exactly
  // the one named exception. A filter registered next month gets the rule for
  // free, and one registered with a hand-written precondition fails the suite
  // by name.
  bool selectionBounded = false;
};

// --- where the rows come from -------------------------------------------
//
// Each family of commands lives in its own translation unit and appends its
// own rows. **Separate files, deliberately, and the reason is a merge one:**
// the remaining registrations (docs/automation-plan.md step 1) are built in
// parallel, and one shared table literal would be one shared conflict --
// docs/... the same trap `run_golden.sh`'s nine parallel view arrays already
// sprang once, where two branches each appended a view, merged clean, and left
// the arrays one element short of each other.
//
// The order of these calls is the order commands appear in the ACTIONS panel,
// and nothing else depends on it: every lookup is by id.
void registerImageCommands(std::vector<CommandSpec>* out);
void registerLayerCommandRows(std::vector<CommandSpec>* out);
void registerOpStackCommands(std::vector<CommandSpec>* out);
void registerPatternCommands(std::vector<CommandSpec>* out);
// core/Region: add/delete/rename/move/resize (PLAN.md gap-closing wave,
// track `region`). See app/CommandsRegions.cpp's own header for why the
// interactive gesture does not call these directly (app/CropTool's own
// precedent) while the edits themselves are registered anyway.
void registerRegionCommands(std::vector<CommandSpec>* out);

// Every recordable command, in a stable order. Walked by the panel, by the
// recorder and by `--selftest`, so a command added to one vocabulary without a
// row here fails the suite rather than being quietly unrecordable -- the
// discipline `allLayerCommands()` established for the two menus.
const std::vector<CommandSpec>& allCommands();

// The row with this id, or nullptr. Linear over a table of tens; a map would
// be a second thing to keep in step with the vector.
const CommandSpec* findCommand(std::string_view id);

// Runs one command. Refuses, by name, an id this build does not know -- which
// is the correct answer for an action written by a newer build, and is
// deliberately NOT the round-trip rule `Op::unrecognised` follows: a document
// with an entry we cannot evaluate still opens, but an action *executes*, and
// executing a step you cannot evaluate writes a wrong file that looks fine.
CommandResult applyCommand(OpenDocument& doc, const Command& command);

// Resolves a layer by name, then by kind if `kind` is given. Returns the
// document's size when nothing matches, which every caller tests for -- an
// index-shaped "not found" that cannot be mistaken for index 0.
//
// **By name, never by index** (docs/automation-plan.md §5): an action recorded
// on one document is replayed on another whose layers differ in order and
// count, and an index would silently address a different layer.
size_t layerIndexNamed(const Document& doc, std::string_view name);

}  // namespace np
