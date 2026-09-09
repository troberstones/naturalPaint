#include "app/CommandSupport.hpp"

// app/CommandsOpStack -- the command rows for a layer's own non-destructive op
// stack, and for the selection.
//
// **The two belong together, and the reason is the action file.** An op-stack
// step carries an op, and an op's text encoding is io/ActionFile's problem;
// a selection step is what makes every *destructive* step in the same file
// mean what it meant when it was recorded (docs/automation-plan.md §7 -- a
// selection is session state and is never in a document file, so an action
// that does not record it silently applies to the whole canvas).
//
// Empty until docs/automation-plan.md step 1's remaining registrations land.
namespace np {

void registerOpStackCommands(std::vector<CommandSpec>*) {}

}  // namespace np
