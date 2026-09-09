#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "ops/Action.hpp"

// io/ActionFile -- `.npaction`, the text form of an `ops/Action`, and the
// library directory it lives in. docs/automation-plan.md §5 and step 4;
// PRD P5 ("human-readable, and diffable"), P6.
//
// The serialisation half of the split `ops/Action.hpp` describes: that header
// is the model and never mentions a byte, this one is the only place that
// knows the word `npaction` exists. The precedent, followed on purpose, is
// `core/OpStack` + `io/OpSerial`.
//
// ==========================================================================
// (1) JSON, and one JSON reader
// ==========================================================================
//
// `export-presets.json` established the pattern, and `io/Json` -- extracted at
// step 0 precisely because this file was going to be its third consumer --
// is the reader. Nothing here parses a character or writes a brace by hand.
// That is not tidiness: `io/ExportAs.cpp` used to escape `"` and `\` and
// nothing else, so a newline in a preset name wrote a file that would not read
// back, and the only reason that class of bug is now fixed everywhere at once
// is that there is one escaper.
//
// ==========================================================================
// (2) The version key is checked before anything else is interpreted
// ==========================================================================
//
// `io/OpSerial` puts its version in the *prefix* -- ahead of the payload's
// first byte -- and argues why: a reader must decide whether it understands
// the encoding before it decodes anything, so that a build meeting `npops2:`
// says so by name instead of misreading a payload whose framing changed.
//
// JSON cannot have a literal byte prefix, and the difference is worth being
// precise about rather than claiming a property this format does not have.
// A JSON file is tokenised in full before any key is looked at -- but
// tokenising is version-independent (JSON is JSON in every version of this
// format), and *interpretation* is not. So the rule this file actually
// enforces, and the one that matters, is:
//
//     nothing but the `"npaction"` key is interpreted until `"npaction"` has
//     been read and found to be a version this build knows.
//
// `"name"` and `"steps"` are not looked at, not defaulted, and not partially
// applied to the caller's `Action` before that check passes. A version-2 file
// whose `"steps"` mean something else therefore cannot be half-read as a
// version-1 one. The check is by key and not by position, so a hand-edited
// file that ends up with `"npaction"` last still reads -- `--selftest`'s
// fixture is deliberately written that way.
//
// ==========================================================================
// (3) Parameters sit FLAT beside "cmd", and the alternative was real
// ==========================================================================
//
// A step is `{"cmd": "filter_gaussian_blur", "sigma": 4.0}` and not
// `{"cmd": "filter_gaussian_blur", "params": {"sigma": 4.0}}`.
//
// The nested form has one genuine advantage, and it is not hypothetical: with
// params in their own object, no parameter name can ever collide with the
// step's own keys, so the format needs no reserved words and can grow a second
// step-level key (a comment, a disabled flag, a unit) at any time without
// breaking a file that happens to use that word as a parameter.
//
// Flat wins anyway, on the audience. PRD P5's whole claim for this format is
// that a person reads and edits these files, and the plan's §5 example is
// written flat for a reason a diff makes obvious: nesting costs every
// parameter an indent level and every step two extra lines of punctuation, on
// a structure whose steps are typically one or two parameters long. A file a
// person will not read is not human-readable however well-formed it is.
//
// The cost is paid explicitly rather than silently, and this is the part that
// makes the choice safe: **`"cmd"` is a reserved key.** `writeAction()`
// REFUSES a step whose parameters contain one, by name. Without that refusal
// the flat form has a genuine silent-loss path -- the writer would emit `cmd`
// twice, `io/Json` keeps the first duplicate, and the parameter would
// disappear on the round trip with nothing said. Should this format ever need
// a second step-level key, it is reserved the same way, in the same list, with
// the same refusal.
//
// ==========================================================================
// (4) Key order is stable across a rewrite, which is a format guarantee
// ==========================================================================
//
// `JsonValue`'s object is an ordered vector for exactly this reason (see
// `io/Json.hpp` §2). Reading a file preserves the order its keys were typed
// in; writing preserves the order they are held in. So re-saving a file
// somebody hand-edited diffs as the lines they changed, not as the whole file
// -- which is the difference between an action being reviewable in a pull
// request and not.
//
// The writer pretty-prints one key per line rather than packing a step onto
// one line as the plan's example sketch does. Same reason, taken one step
// further: a changed sigma is then a one-line diff instead of a rewritten
// step.
namespace np {

// The version this build writes, and the only one it reads. Exposed so
// `--selftest` and any future migration can name it rather than spelling the
// literal a second time.
inline constexpr int kActionFileVersion = 1;

// The key that carries it. See this header's §2.
inline constexpr const char* kActionFileVersionKey = "npaction";

// Including the dot, as `std::filesystem::path::extension()` reports it.
inline constexpr const char* kActionFileExtension = ".npaction";

// The step key a parameter may not be called. See §3.
inline constexpr const char* kActionStepCommandKey = "cmd";

// --- text form -----------------------------------------------------------

// Serialises `action` into `*out`.
//
// Fails only for the reserved-key case of §3: a step whose parameters contain
// a key called `"cmd"` cannot be written flat without losing it, so it is
// refused by name instead. Every other action this codebase can construct
// writes successfully.
//
// **Deliberately asymmetric with `readAction()`, which also refuses a step
// naming a command this build does not have.** The writer does not check
// that, and the reason is which direction each refusal protects. Loading
// precedes *executing*: a step that cannot be evaluated must stop the file
// before it writes thirty wrong outputs. Writing does not precede anything --
// an action holding a step from a plugin, a newer registration or a
// half-finished edit is a legitimate thing to save, and a writer that refused
// it would make the ACTIONS panel unable to save its own working state.
bool writeAction(const Action& action, std::string* out, std::string* errorOut = nullptr);

// Parses `text`, which `label` names in every refusal.
//
// Returns false and leaves `*out` **untouched** on any refusal -- a partially
// filled action is how a version this build does not know gets half-executed.
// Refuses, each naming `label` and the reason:
//
//   * no `"npaction"` key, or one that is not a number;
//   * a version this build does not read;
//   * `"steps"` present and not an array (an action that silently becomes
//     empty reports success on every file in a batch);
//   * a step that is not an object, or has no string `"cmd"`;
//   * a step naming a command this build does not have -- refused HERE, at
//     load, rather than at run. docs/automation-plan.md §7: `Op::unrecognised`
//     preserves what a *document* round trip cannot interpret, but an action
//     is executed, and executing a step you cannot evaluate writes a wrong
//     file that looks fine.
bool readAction(std::string_view text, std::string_view label, Action* out,
                std::string* errorOut = nullptr);

// --- files ---------------------------------------------------------------

bool saveActionToFile(const std::string& path, const Action& action,
                      std::string* errorOut = nullptr);

// A missing file is a refusal here, unlike `ExportPresetStore::loadFromFile()`
// where "the user has never saved a preset" is the ordinary first run. A
// caller naming one action file has already been told it exists -- by a
// listing, a panel row or a command line -- so its absence is a fact worth a
// sentence.
bool loadActionFromFile(const std::string& path, Action* out, std::string* errorOut = nullptr);

// `~/Library/Application Support/naturalPaint/actions/`, or the XDG and
// no-HOME cases, following `defaultExportPresetsPath()`'s resolver exactly --
// including its `NP_*` override, which is what lets `--selftest` and a second
// profile stay off the real library. No trailing slash.
std::string actionsDirectoryPath();

// The file name for an action called `name`, extension included, or an empty
// string when `name` has nothing usable in it.
//
// **A name is user text and a file name is not**, and this is the seam
// between them. An action called `../../etc/passwd` or `Height prep 512` must
// not decide where bytes land, so every byte outside `[A-Za-z0-9 ._-]` becomes
// `_`, leading dots go (so a name cannot produce a hidden file, or `.` or
// `..`), and the result is capped well inside every filesystem's limit. The
// action's real name is the one inside the file; this is only how it is
// filed.
std::string actionFileNameFor(std::string_view name);

// Every `.npaction` file directly in `dir`, full paths, **sorted by file
// name**. Sorted because `std::filesystem::directory_iterator` yields entries
// in whatever order the filesystem holds them, which differs between machines
// and between two runs on one machine after an edit -- a panel that lists
// actions in a different order each launch, and a test that passes here and
// fails on the next volume.
//
// A directory that does not exist is an empty list and not a refusal: a user
// who has never saved an action has no library yet.
std::vector<std::string> listActionFiles(const std::string& dir);

}  // namespace np
