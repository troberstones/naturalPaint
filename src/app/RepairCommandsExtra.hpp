#pragma once

#include "app/Command.hpp"
#include "app/FilterOps.hpp"  // ContentAwareFillRequest, SeamHealRequest

// app/RepairCommandsExtra -- encoders for the track `repair` dialogs
// (Content-Aware Fill, Seam Heal), following app/FilterCommandsExtra.hpp's
// own precedent: one small header for a dialog's encoder, implemented beside
// the reader that validates its keys (app/CommandsImage.cpp), so this header
// carries no logic of its own.
namespace np {

Command contentAwareFillCommand(const ContentAwareFillRequest& p);
Command seamHealCommand(const SeamHealRequest& p);

}  // namespace np
