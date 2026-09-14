#pragma once

#include "app/Command.hpp"
#include "ops/Filters.hpp"  // DustScratchesParams, ShadowsHighlightsParams

// app/FilterCommandsFilters -- encoders for two commands (Filter > Dust & Scratches, Image > Adjustments > Shadows/
// Highlights), following app/FilterCommandsExtra.hpp's own precedent: a
// small header of its own rather than widening app/CommandsImage.hpp's
// declaration surface, which every other track also edits. Each function is
// still implemented beside the reader that validates its keys
// (app/CommandsImage.cpp), so this header carries no logic of its own.
namespace np {

Command dustScratchesCommand(const DustScratchesParams& p);
Command shadowsHighlightsCommand(const ShadowsHighlightsParams& p);

}  // namespace np
