#include "app/CompPanel.hpp"

#include <algorithm>

namespace np {

std::vector<CompPanelRow> compPanelRows(const Document& doc) {
  std::vector<CompPanelRow> rows;
  rows.reserve(doc.comps.size());
  for (size_t i = 0; i < doc.comps.size(); ++i) {
    const LayerComp& comp = doc.comps[i];
    CompPanelRow row;
    row.index = i;
    row.name = comp.name;
    row.known = comp.known;
    row.captured = comp.layers.size();
    // Against the live document, every frame, uncached -- see the header's
    // section (c) on why there is deliberately no panel-side cache here.
    for (const LayerCompEntry& entry : comp.layers) {
      if (entry.layerId == 0) continue;
      const auto it = std::find_if(doc.layers.begin(), doc.layers.end(),
                                   [&](const Layer& l) { return l.id == entry.layerId; });
      if (it != doc.layers.end()) ++row.stillHere;
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string compRowText(const CompPanelRow& row) {
  std::string s = row.name.empty() ? "(unnamed comp)" : row.name;
  if (!row.known) return s + " \xC2\xB7 UNREADABLE (kept)";
  s += " \xC2\xB7 " + std::to_string(row.captured) + " layer";
  if (row.captured != 1) s += "s";
  // Quiet in the common case: a comp that still matches its document says
  // nothing extra, so the marker below means "this restore will be partial"
  // rather than being decoration on every row.
  if (row.stillHere != row.captured)
    s += " \xC2\xB7 " + std::to_string(row.stillHere) + " still here";
  return s;
}

bool compRowIsPartial(const CompPanelRow& row) {
  return row.known && row.stillHere != row.captured;
}

size_t compRowMoveUpTarget(size_t row, size_t rowCount) noexcept {
  // Up the panel is toward row 0, and row 0 IS index 0 here -- no reversal.
  if (row == 0 || row >= rowCount) return kNoCompRow;
  return row - 1;
}

size_t compRowMoveDownTarget(size_t row, size_t rowCount) noexcept {
  if (row + 1 >= rowCount) return kNoCompRow;
  return row + 1;
}

}  // namespace np
