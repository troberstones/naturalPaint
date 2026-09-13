#include "app/selftest/Support.hpp"

#include "app/ChannelsPanel.hpp"
#include "core/SelectionMask.hpp"

namespace np {

// app/ChannelsPanel (PRD E13). Pure list mapping for the CHANNELS dock tab --
// see that header for what it deliberately does not decide. Headless,
// GPU-free.
bool runChannelsPanelTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("[selftest] channels panel: row mapping for the CHANNELS dock tab\n");

  {
    Document doc;
    check(channelsPanelRows(doc).empty(), "channels panel: no channels, no rows");
  }

  {
    Document doc;
    doc.channels.push_back(channelFromSelection(selectRectangle(0, 0, 8, 8), "Mask"));
    doc.channels.push_back(channelFromSelection(selectRectangle(0, 0, 4, 4), "Other"));
    doc.channels.push_back(channelFromSelection(selectRectangle(0, 0, 2, 2), ""));

    const std::vector<ChannelsPanelRow> rows = channelsPanelRows(doc);
    check(rows.size() == 3, "channels panel: one row per channel");
    check(rows[0].index == 0 && rows[0].name == "Mask" && rows[1].index == 1 &&
              rows[1].name == "Other" && rows[2].index == 2 && rows[2].name.empty(),
          "channels panel: model order, row 0 is channels[0] -- unlike LAYERS, like COMPS");

    check(channelRowText(rows[0]) == "Mask" && channelRowText(rows[1]) == "Other",
          "channels panel: a named row reads its name verbatim");
    check(channelRowText(rows[2]) == "(unnamed channel)",
          "channels panel: an empty name reads a placeholder, not nothing");
  }

  return ok;
}

}  // namespace np
