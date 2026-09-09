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
};

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
