#include "ui/AtelierChrome.hpp"

#include <cmath>
#include <cstdio>
#include <type_traits>
#include <utility>
#include <vector>

#include "app/DocumentLifecycle.hpp"
#include "app/Memory.hpp"
#include "core/TileStore.hpp"
#include "ui/AtelierTheme.hpp"

#include "imgui.h"

namespace np {
namespace {

// The flags every band shares: fixed furniture, never moved, never focused
// ahead of the canvas.
constexpr ImGuiWindowFlags kBandFlags =
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBringToFrontOnFocus |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
    ImGuiWindowFlags_NoSavedSettings;

void beginBand(const char* id, const AtelierRect& r, uint32_t bg) {
  ImGui::SetNextWindowPos(ImVec2(r.x, r.y));
  ImGui::SetNextWindowSize(ImVec2(r.w, r.h));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(atelierToken(bg)));
  ImGui::Begin(id, nullptr, kBandFlags);
}

void endBand() {
  ImGui::End();
  ImGui::PopStyleColor();
}

// Vertically centre the next line of text/widgets in a band of height `h`.
// The bands are 46 px and 26 px against a frame height near 23, so without
// this every one of them sits its content hard against the top edge.
void centreInBand(float h, float contentH) {
  const float pad = (h - contentH) * 0.5f;
  if (pad > 0.0f) ImGui::SetCursorPosY(pad);
}

// A caps label in the secondary colour -- docs/ui.md section 1's "800-weight
// caps" role, standing in until the type ramp lands (this build still draws
// the whole UI in one 13 px face; see ui/Fonts.hpp).
void capsLabel(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(atelierToken(kTextSecondary)));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
}

void bandSeparator() {
  ImGui::SameLine(0.0f, 12.0f);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = ImGui::GetFrameHeight();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x, p.y + h), atelierToken(kDivider),
                                      kDividerThickness);
  ImGui::SameLine(0.0f, 12.0f);
}

std::string formatMiB(size_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  return buf;
}

}  // namespace

ImU32 atelierToken(uint32_t rgb) noexcept {
  float c[3];
  unpackRgb(rgb, c);
  return IM_COL32(static_cast<int>(c[0] * 255.0f + 0.5f), static_cast<int>(c[1] * 255.0f + 0.5f),
                  static_cast<int>(c[2] * 255.0f + 0.5f), 255);
}

const char* workingSpaceLabel(const Document& doc) noexcept {
  (void)doc;
  // core/TileStore holds `uint16_t` half-floats -- 16 bits per channel, linear
  // (core/Half.hpp converts at the read/write boundary). Widening the store is
  // what makes `LIN32` reachable, and this stops compiling at that moment.
  static_assert(sizeof(std::remove_pointer_t<decltype(std::declval<const Tile&>().data())>) == 2,
                "PRD L1: the label is derived from the tile's storage, so a wider tile "
                "has to add the LIN32 branch here rather than silently mislabel itself");
  return "LIN16";
}

ResidentReading atelierResident() noexcept {
  return ResidentReading{currentResidentBytes(), kResidentBudgetBytes};
}

std::string atelierViewStateMarkers(const CanvasView& view) {
  std::string out;
  const auto add = [&out](const char* s) {
    if (!out.empty()) out += "  ";
    out += s;
  };
  // Named by axis, not by a single "MIRROR": docs/ui.md section 5 -- "with two
  // mirror axes this matters more, not less: both on looks like a deliberate
  // composition, not like two toggles nobody cleared."
  if (view.mirrorX) add("MIRROR L/R");
  if (view.mirrorY) add("MIRROR U/D");
  if (view.grayscale) add("GRAYSCALE");
  if (std::fabs(view.rotation) > 1e-4f) add("ROTATED");
  return out;
}

const char* toolName(Tool t) {
  switch (t) {
    case Tool::Brush:      return "Brush";
    case Tool::Water:      return "Water";
    case Tool::DryBrush:   return "Dry Brush";
    case Tool::Eyedropper: return "Eyedropper";
    case Tool::Hand:       return "Hand";
    case Tool::Zoom:       return "Zoom";
    case Tool::Count:      break;
  }
  return "?";
}

void drawAtelierRules(const AtelierBands& bands) {
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  for (size_t i = 0; i < bands.ruleCount; ++i) {
    const AtelierRect& r = bands.rules[i];
    dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), atelierToken(kRule));
  }
}

bool drawAtelierTabStrip(AppState& st, const AtelierBands& bands, std::string* statusOut) {
  if (bands.tabStrip.empty()) return false;
  beginBand("##atelierTabs", bands.tabStrip, kChromeMid);

  bool newDocument = false;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float h = bands.tabStrip.h;
  float x = bands.tabStrip.x;
  const float top = bands.tabStrip.y;

  // Hand-drawn rather than ImGui's own tab bar. Three reasons, in the order
  // they bite: ImGui tabs size themselves to their labels and reorder on drag,
  // which a fixed 34 px band cannot express; the active tab has to be chrome
  // *deep* cut into a chrome-mid strip, which is the inverse of ImGui's raised
  // selected tab; and the dirty marker is a filled accent dot, not the `*`
  // ImGui appends to the label.
  for (size_t i = 0; i < st.documents.count(); ++i) {
    const OpenDocument* doc = st.documents.at(i);
    if (doc == nullptr) continue;
    const bool active = i == st.documents.activeIndex();
    const std::string name = documentDisplayName(*doc);

    const float labelW = ImGui::CalcTextSize(name.c_str()).x;
    const float tabW = labelW + 54.0f;  // dirty dot, close box, padding
    if (x + tabW > bands.tabStrip.right()) break;  // no scroll yet; see below

    ImGui::SetCursorScreenPos(ImVec2(x, top));
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::InvisibleButton("##tab", ImVec2(tabW, h))) st.documents.setActive(i);
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetTooltip("%s", name.c_str());

    dl->AddRectFilled(ImVec2(x, top), ImVec2(x + tabW, top + h),
                      active         ? atelierToken(kChromeDeep)
                      : hovered      ? atelierToken(kChromeBase)
                                     : atelierToken(kChromeMid));
    // A 2 px accent underline on the active tab. The design marks the active
    // tab by its fill; the underline is what keeps it legible at a glance in a
    // strip where the fills are two greys four steps apart.
    if (active)
      dl->AddRectFilled(ImVec2(x, top + h - kRuleThickness), ImVec2(x + tabW, top + h),
                        atelierToken(kAccent));
    dl->AddText(ImVec2(x + 12.0f, top + (h - ImGui::GetTextLineHeight()) * 0.5f),
                atelierToken(active ? kTextPrimary : kTextSecondary), name.c_str());

    if (doc->isDirty())
      dl->AddCircleFilled(ImVec2(x + tabW - 26.0f, top + h * 0.5f), 4.0f, atelierToken(kAccent),
                          12);

    // Close. Refuses a dirty document by name, which is the File menu's own
    // rule and the same call -- a tab that discarded work on one click would
    // be the one place in the application where PRD I11 did not hold.
    ImGui::SetCursorScreenPos(ImVec2(x + tabW - 18.0f, top + (h - 14.0f) * 0.5f));
    if (ImGui::InvisibleButton("##close", ImVec2(14.0f, 14.0f))) {
      std::string err;
      if (!st.documents.close(i, false, &err) && statusOut != nullptr) *statusOut = err;
      ImGui::PopID();
      break;  // the list changed under the loop
    }
    const ImU32 xCol = ImGui::IsItemHovered() ? atelierToken(kAccent)
                                              : atelierToken(kTextSecondary);
    const ImVec2 c(x + tabW - 11.0f, top + h * 0.5f);
    dl->AddLine(ImVec2(c.x - 4, c.y - 4), ImVec2(c.x + 4, c.y + 4), xCol, 1.5f);
    dl->AddLine(ImVec2(c.x - 4, c.y + 4), ImVec2(c.x + 4, c.y - 4), xCol, 1.5f);

    ImGui::PopID();
    // 1 px internal divider between tabs (docs/ui.md section 1).
    dl->AddRectFilled(ImVec2(x + tabW, top), ImVec2(x + tabW + kDividerThickness, top + h),
                      atelierToken(kDivider));
    x += tabW + kDividerThickness;
  }

  // `+` -- a new blank document. The caller makes it: only main.cpp's canvas
  // dimensions decide how big a blank one is, and this module does not know
  // them and should not learn them for one button.
  ImGui::SetCursorScreenPos(ImVec2(x, top));
  if (ImGui::InvisibleButton("##newdoc", ImVec2(h, h))) newDocument = true;
  const ImU32 plusCol =
      ImGui::IsItemHovered() ? atelierToken(kAccent) : atelierToken(kTextSecondary);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("New document");
  const ImVec2 pc(x + h * 0.5f, top + h * 0.5f);
  dl->AddLine(ImVec2(pc.x - 6, pc.y), ImVec2(pc.x + 6, pc.y), plusCol, 1.5f);
  dl->AddLine(ImVec2(pc.x, pc.y - 6), ImVec2(pc.x, pc.y + 6), plusCol, 1.5f);

  // Overflow is dropped, and said so out loud rather than left as a silently
  // short strip: with no scroll and no overflow menu, a document past the
  // right edge is reachable only from the Window menu. Wiring a scroll here
  // before the split exists would be building the wrong half of step 14.
  if (x + h > bands.tabStrip.right()) {
    ImGui::SetCursorScreenPos(ImVec2(bands.tabStrip.right() - 30.0f, top));
    capsLabel("...");
  }

  endBand();
  return newDocument;
}

void drawAtelierOptionsBar(AppState& st, const AtelierBands& bands) {
  beginBand("##atelierOptions", bands.optionsBar, kChromeDeep);
  centreInBand(bands.optionsBar.h, ImGui::GetFrameHeight());

  // The accent block that leads the band in the design (`[]BRUSH`): the active
  // tool, marked in the one colour reserved for "this is on".
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = ImGui::GetFrameHeight();
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + 6.0f, p.y + h),
                                            atelierToken(kAccent));
  ImGui::Dummy(ImVec2(6.0f, h));
  ImGui::SameLine(0.0f, 8.0f);
  ImGui::TextUnformatted(toolName(st.brush.tool));

  bandSeparator();
  capsLabel("SIZE");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(140.0f);
  ImGui::SliderFloat("##size", &st.brush.radius, 2.0f, 90.0f, "%.0f px");

  bandSeparator();
  capsLabel("HARD");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110.0f);
  ImGui::SliderFloat("##hard", &st.brush.hardness, 0.0f, 1.0f, "%.2f");

  bandSeparator();
  capsLabel("LOAD");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110.0f);
  ImGui::SliderFloat("##load", &st.brush.load, 0.0f, 1.0f, "%.2f");

  bandSeparator();
  capsLabel("WET");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110.0f);
  ImGui::SliderFloat("##wet", &st.brush.wetness, 0.0f, 3.0f, "%.2f");

  endBand();
}

void drawAtelierStatusBar(AppState& st, const AtelierBands& bands, uint32_t canvasW,
                          uint32_t canvasH) {
  beginBand("##atelierStatus", bands.statusBar, kChromeBase);
  centreInBand(bands.statusBar.h, ImGui::GetTextLineHeight());

  ImGui::Text("%.0f%%", st.view.zoom * 100.0f);

  ImGui::SameLine(0.0f, 16.0f);
  // Dimensions come from the open document when there is one; `canvasW/H` are
  // the solver canvas, which is what exists before a document does. The label
  // is the *working space*, never a bit depth (PRD L1).
  const OpenDocument* od = st.documents.active();
  if (od != nullptr)
    ImGui::Text("%d x %d  -  %s", od->document.width, od->document.height,
                workingSpaceLabel(od->document));
  else
    ImGui::Text("%u x %u  -  canvas", canvasW, canvasH);

  ImGui::SameLine(0.0f, 16.0f);
  const ResidentReading mem = atelierResident();
  // Over budget is the one status-bar value that changes colour: it is a
  // number the user is meant to act on, and PRD L7 exists because the
  // lightweight claim should be continuously visible rather than something
  // only `--selftest` knows.
  const bool over = mem.bytes > mem.budget;
  if (over) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(atelierToken(kAccent)));
  ImGui::Text("%s / %s", formatMiB(mem.bytes).c_str(), formatMiB(mem.budget).c_str());
  if (over) ImGui::PopStyleColor();

  const std::string markers = atelierViewStateMarkers(st.view);
  if (!markers.empty()) {
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(atelierToken(kAccent)));
    ImGui::TextUnformatted(markers.c_str());
    ImGui::PopStyleColor();
  }

  endBand();
}

}  // namespace np
