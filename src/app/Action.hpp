#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "app/Command.hpp"
#include "core/Layer.hpp"
#include "core/OpStack.hpp"

// app/Action -- an action's *model*: a name and an ordered list of commands.
//
// **Under app/, not ops/, and the move was made at gather.** The plan called
// this `ops/Action`, and the track that built it recorded why that could not
// hold: a step IS an `app::Command`, so this header would have been the first
// under `ops/` to reach into `app/` -- every other one stops at `core/`. The
// alternative, a second structurally identical step type declared down here to
// preserve the layering, is the two-encoders drift trap one level up. So the
// file moved to the layer it already belonged to rather than the layering
// being bent around it.
// Nothing here knows what an action looks like on disk; io/ActionFile is that
// half (docs/automation-plan.md step 4).
//
// **The split is core/OpStack + io/OpSerial's, followed deliberately rather
// than by coincidence**, and this comment says so because the two halves are
// small enough that merging them would look like a simplification. It is not
// one. `core/OpStack` is a stack of graded operations that never mentions a
// byte, and `io/OpSerial` is the only place that knows `npops1:` exists; the
// consequence is that the format can be versioned, refused and replaced
// without the evaluator noticing, and the evaluator can gain a kind without
// the format silently changing meaning. An `Action` is in the same position:
// it is executed by `applyCommand()` and it is edited by a human in a text
// file, and those two audiences pull in different directions (PRD P5 wants
// readable and diffable; execution wants exact). Keeping them in separate
// translation units is what stops one being compromised for the other.
//
// ==========================================================================
// (1) The one layering inversion in this file, named rather than hidden
// ==========================================================================
//
// This header includes `app/Command.hpp`, and it is the first header under
// `ops/` to include anything from `app/` -- every other one reaches no higher
// than `core/`. That is a real inversion and it is worth stating what it is
// and what the fix is, because a reader who meets it later will otherwise
// assume it was an accident.
//
// A step *is* a `Command` (docs/automation-plan.md step 4 says so in as many
// words), and defining a second, structurally identical step type here so
// that `ops/` could stay below `app/` would be the drift trap the plan's §7
// warns about, one level up: two descriptions of one thing, and the tenth
// command id lands in one of them. So the type is shared, and the include
// goes where the type is.
//
// `Command` itself -- `{std::string id; JsonValue params;}` -- depends only on
// `io/Json`; it is `applyCommand()`, `CommandSpec` and the registry sharing
// its header that drag `app/DocumentLifecycle.hpp` in behind it. The fix, when
// somebody wants it, is to move those five lines down into their own header
// below `app/`, not to duplicate them. It is deliberately NOT done here:
// `src/app/Command.*` is being edited by several branches at once
// (docs/automation-plan.md step 1's remaining registrations), and a header
// split is the shape of change that merges clean and loses a row.
namespace np {

// One action: a name, and the ordered steps that make it up.
//
// Deliberately a plain aggregate with no invariants of its own. The interesting
// questions -- is every step's command one this build has, is a step's params
// object legal -- are asked at the two edges where the answer can be acted on:
// `io/ActionFile` asks them at load, because a file is where an unknown id
// arrives from, and `applyCommand()` asks them again at run. A model that
// enforced them would make it impossible to hold a half-edited action in the
// ACTIONS panel, which is exactly what that panel is for.
struct Action {
  std::string name;
  std::vector<Command> steps;
};

// The stable, lower_snake_case id for a point-op kind, as written into a
// `.npaint`-adjacent action file. Returns nullptr only for a value cast in
// from outside the enum.
//
// **This is not `pointOpKindName()`, and the difference is the house rule.**
// That function returns display text -- "Channel Mixer", with a space and a
// capital -- which is what a GRADE panel row and an undo label say, and which
// is UI copy: it can be reworded or localised tomorrow. A file keyed by it
// would stop opening when someone improved a label. These ids are keyed to
// the same enumerators and are never reworded; `--selftest` asserts that all
// nine are present and distinct, so a kind appended to `PointOpKind` cannot
// reach a file with no id or with someone else's.
const char* pointOpKindId(PointOpKind kind) noexcept;

// What `actionFromLayerOps()` produced, or why it would not.
struct LayerOpsToActionResult {
  bool ok = false;
  Action action;
  // The refusal sentence when `ok` is false. Names the layer, the entry and
  // the reason, in this codebase's refusal register.
  std::string error;
  // Non-fatal, and expected: see `actionFromLayerOps()` on why a converted
  // action can be structurally correct and not yet runnable.
  std::vector<std::string> warnings;
};

// PRD P6's converter: "recording an action is optional; any document's
// existing op stack is already one". Reads `layer.ops` and emits the steps
// that would rebuild it on another document.
//
// **Refuses, rather than skipping, an entry it cannot describe.** A
// `SpatialB`/`StrokeC`/`BakedD` entry has no implementation anywhere in this
// tree, and an `OpClass::Unknown` entry is one a newer build wrote. Emitting
// nothing for either would produce an action that runs to completion and
// grades the file differently from the document it was converted from --
// docs/automation-plan.md §7's "an `Unknown` op record refuses the run",
// applied one step earlier, at conversion.
//
// `actionName` is copied into `Action::name` as given; the caller owns naming.
LayerOpsToActionResult actionFromLayerOps(const Layer& layer, std::string actionName);

}  // namespace np
