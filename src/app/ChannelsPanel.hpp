#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/Channels.hpp"
#include "core/Document.hpp"

// app/ChannelsPanel (PRD E13). Pure list mapping for the CHANNELS dock tab,
// same split as app/CompPanel.hpp/app/PathsPanel.hpp: rows and row text here
// so `--selftest` can check them with no window; the chrome (buttons, rename
// field, the four commands in app/CommandsOpStack.cpp they call) is
// ui/MacPaintUI.cpp's `drawChannelsSection()`.
//
// Row order is model order (`channels[0]` is row 0) -- a channel has no
// compositing direction the way a layer does, so nothing here reverses
// anything, matching app/CompPanel and unlike app/LayerPanel. A row's
// `index` is valid for one frame only: the only writers of
// `Document::channels` are recorded, panel-redrawing commands
// (save/rename/delete), so it goes stale only between a mutation and the end
// of that same frame, same as every other list in this column.
//
// No enablement predicate here, unlike app/PathsPanel's `PathOpAvailability`:
// rename/delete are always available on a valid row, and Save Selection as
// Channel's precondition is session state (`OpenDocument::selection`) this
// header does not reach -- the chrome reads that greying off
// `applyCommand()` directly, the way every other button already does.
namespace np {

struct ChannelsPanelRow {
  size_t index = 0;   // into Document::channels; see header note on lifetime
  std::string name;   // AlphaChannel::name, verbatim
};

std::vector<ChannelsPanelRow> channelsPanelRows(const Document& doc);

// The name, or `(unnamed channel)` for the empty one -- this build never
// creates one, but a hand-edited or legacy `.npaint` might.
std::string channelRowText(const ChannelsPanelRow& row);

}  // namespace np
