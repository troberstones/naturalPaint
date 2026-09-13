#include "app/ChannelsPanel.hpp"

namespace np {

std::vector<ChannelsPanelRow> channelsPanelRows(const Document& doc) {
  std::vector<ChannelsPanelRow> rows;
  rows.reserve(doc.channels.size());
  for (size_t i = 0; i < doc.channels.size(); ++i) {
    ChannelsPanelRow row;
    row.index = i;
    row.name = doc.channels[i].name;
    rows.push_back(std::move(row));
  }
  return rows;
}

std::string channelRowText(const ChannelsPanelRow& row) {
  return row.name.empty() ? "(unnamed channel)" : row.name;
}

}  // namespace np
