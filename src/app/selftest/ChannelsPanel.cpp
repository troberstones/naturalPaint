#include "app/selftest/Support.hpp"

#include "app/ChannelsPanel.hpp"
#include "core/Half.hpp"
#include "core/SelectionMask.hpp"
#include "ui/MacPaintUI.hpp"

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

  // PRD E13's single-channel view: the solo-toggle decision, and the packer
  // that turns a viewed channel into a grayscale image.
  {
    check(toggleChannelView(std::nullopt, "Mask") == std::optional<std::string>("Mask"),
          "channel view: clicking View on an unviewed row starts viewing it");
    check(toggleChannelView(std::optional<std::string>("Mask"), "Mask") == std::nullopt,
          "channel view: clicking View again on the row already viewed turns it off");
    check(toggleChannelView(std::optional<std::string>("Mask"), "Other") ==
              std::optional<std::string>("Other"),
          "channel view: clicking a different row's View replaces, rather than adding to, "
          "the one already viewed");
  }

  {
    const AlphaChannel channel = channelFromSelection(selectRectangle(2, 2, 4, 4), "Mask");
    const std::vector<uint16_t> halves = packChannelViewHalf(channel, 8, 8);
    check(halves.size() == 8u * 8u * 4u, "channel view: one RGBA half-float quad per texel");

    const size_t insideIdx = (static_cast<size_t>(3) * 8 + 3) * 4;  // inside the 2..4 square
    check(halfToFloat(halves[insideIdx + 0]) == 1.0f &&
              halfToFloat(halves[insideIdx + 1]) == 1.0f &&
              halfToFloat(halves[insideIdx + 2]) == 1.0f && halfToFloat(halves[insideIdx + 3]) == 1.0f,
          "channel view: full coverage reads back as opaque white, not merely non-zero");

    const size_t outsideIdx = (static_cast<size_t>(0) * 8 + 0) * 4;  // outside the square
    check(halfToFloat(halves[outsideIdx + 0]) == 0.0f &&
              halfToFloat(halves[outsideIdx + 1]) == 0.0f &&
              halfToFloat(halves[outsideIdx + 2]) == 0.0f && halfToFloat(halves[outsideIdx + 3]) == 1.0f,
          "channel view: no coverage reads back as opaque black -- alpha stays 1.0 either way, "
          "since this replaces the canvas rather than tinting it");
  }

  std::printf("[selftest] channels panel %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
