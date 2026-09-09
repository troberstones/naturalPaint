#include "app/CommandSupport.hpp"

// app/CommandsPatterns -- the command rows for PLAN.md Phase 19 step 5's two
// parked P2 image ops: lens correction (PRD D22) and pattern define/fill
// (PRD D27).
//
// Its own file rather than a section of app/CommandsImage.cpp because those
// two ops are severable from everything else in phase 19 and are built
// separately; a shared file would be a shared merge conflict for no benefit.
//
// Empty until they land.
namespace np {

void registerPatternCommands(std::vector<CommandSpec>*) {}

}  // namespace np
