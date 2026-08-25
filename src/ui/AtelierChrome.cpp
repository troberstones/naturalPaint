#include "ui/AtelierChrome.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <type_traits>
#include <utility>
#include <vector>

#include "app/CloseDecision.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/Memory.hpp"
#include "app/StrokeSession.hpp"
#include "core/TileStore.hpp"
#include "ui/AtelierTheme.hpp"
#include "ui/Fonts.hpp"

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
  pushAtelierMono();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(atelierToken(kTextSecondary)));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  popAtelierMono();
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

void pushAtelierMono() {
  if (uiFonts().mono != nullptr) ImGui::PushFont(uiFonts().mono, 0.0f);
}
void popAtelierMono() {
  if (uiFonts().mono != nullptr) ImGui::PopFont();
}

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

namespace {

// One row per Tool value, **in the enum's declaration order** -- the
// static_assert right after the table is what makes that a fact rather than
// a convention: add a value to `enum class Tool` without a row here and the
// build stops, instead of every one of toolName()/toolImplemented()/
// toolIconCodepoint()/toolShortcutLabel() quietly returning the same wrong
// answer for it the way four separate switches each missing a case would.
//
// `shortcut` is docs/shortcuts.md section 1's reserved letter, spelled out
// as "Shift+X" rather than the design's own "⇧X" glyph: this build's merged
// font holds only the Lucide codepoints below plus whatever
// requiredUiCodepoints() lists (ui/Fonts.cpp), and adding one more codepoint
// for a single tooltip character is not worth the merge-range entry when
// plain ASCII already says the same thing unambiguously.
struct ToolMeta {
  const char* name;
  const char* lucideName;
  uint32_t codepoint;
  const char* shortcut;  // "" when docs/shortcuts.md reserves none yet
  bool implemented;
};

constexpr ToolMeta kToolMeta[] = {
    // --- the tools with real behaviour --------------------------------
    {"Brush", "brush", 57811u, "B", true},
    {"Water", "droplet", 57524u, "", true},
    {"Dry Brush", "paintbrush-2", 58088u, "", true},
    {"Eyedropper", "pipette", 57659u, "I", true},
    {"Rectangle Marquee", "square-dashed", 57803u, "M", true},
    {"Elliptical Marquee", "circle-dashed", 58544u, "Shift+M", true},
    {"Hand", "hand", 57815u, "H", true},
    {"Zoom", "zoom-in", 57782u, "Z", true},
    // --- the name/icon/slot-only cells (app/AppState.hpp) -------------
    {"Move", "move", 57633u, "V", false},
    {"Lasso", "lasso", 57806u, "L", true},
    {"Polygon Lasso", "pentagon", 58667u, "Shift+L", true},
    {"Magic Wand", "wand-sparkles", 58199u, "W", true},
    {"Crop", "crop", 57515u, "C", false},
    {"Measure", "ruler", 57675u, "", false},
    {"Frame", "frame", 58001u, "", false},
    {"Clone Stamp", "stamp", 58299u, "S", false},
    {"Eraser", "eraser", 57999u, "E", false},
    {"Paint Bucket", "paint-bucket", 58086u, "Shift+G", true},
    {"Gradient", "blend", 58780u, "G", true},
    {"Pencil", "pencil", 57849u, "", false},
    {"Smudge", "droplets", 57525u, "N", false},
    {"Dodge", "sun", 57720u, "O", false},
    {"Burn", "moon", 57630u, "Shift+O", false},
    {"Pen", "pen-tool", 57649u, "P", false},
    {"Curve", "spline", 58251u, "Shift+P", false},
    {"Text", "type", 57752u, "T", false},
    {"Shape", "shapes", 58547u, "", false},
    {"Slice", "slice", 58096u, "", false},
};
static_assert(std::size(kToolMeta) == static_cast<size_t>(Tool::Count),
              "one ToolMeta row per Tool value, in app/AppState.hpp's declaration order");

constexpr ToolMeta kUnknownTool{"?", "", 0u, "", false};

// `t` past the table (Tool::Count, or any other stray cast) reads as the
// unknown row -- the same "?" contract the old per-field switches gave
// `toolName()`, and what app/selftest/AtelierChrome.cpp's tripwire checks.
const ToolMeta& metaFor(Tool t) noexcept {
  const size_t i = static_cast<size_t>(t);
  return i < std::size(kToolMeta) ? kToolMeta[i] : kUnknownTool;
}

}  // namespace

const char* toolName(Tool t) { return metaFor(t).name; }
bool toolImplemented(Tool t) noexcept { return metaFor(t).implemented; }
const char* toolIconName(Tool t) noexcept { return metaFor(t).lucideName; }
uint32_t toolIconCodepoint(Tool t) noexcept { return metaFor(t).codepoint; }

const std::vector<uint32_t>& toolIconCodepoints() {
  static const std::vector<uint32_t> kPoints = [] {
    std::vector<uint32_t> points;
    for (const ToolMeta& m : kToolMeta)
      if (m.codepoint != 0u &&
          std::find(points.begin(), points.end(), m.codepoint) == points.end())
        points.push_back(m.codepoint);
    points.push_back(kMoreIconCodepoint);
    std::sort(points.begin(), points.end());
    return points;
  }();
  return kPoints;
}

std::string toolShortcutLabel(Tool t) { return metaFor(t).shortcut; }

std::string toolTooltip(Tool t) {
  std::string s = toolName(t);
  s += " Tool";
  const std::string shortcut = toolShortcutLabel(t);
  if (!shortcut.empty()) {
    s += "  ";
    s += shortcut;
  }
  if (!toolImplemented(t)) s += "\nNot built yet.";
  return s;
}

int toolGroupIndex(Tool t) noexcept {
  for (int g = 0; g < kToolGroupCount; ++g)
    for (int m = 0; m < kToolGroups[g].memberCount; ++m)
      if (kToolGroups[g].members[m] == t) return g;
  return -1;
}

Tool toolGroupDefaultMember(int groupIndex) noexcept {
  const ToolGroup& group = kToolGroups[groupIndex];
  for (int m = 0; m < group.memberCount; ++m)
    if (toolImplemented(group.members[m])) return group.members[m];
  return group.members[0];
}

void drawAtelierRules(const AtelierBands& bands) {
  // **Background list, not foreground, and the difference is visible.**
  //
  // These were on the foreground list to stop a neighbouring band's window
  // overdrawing a 2 px rule down to 1 px. But ImGui renders the foreground
  // list after EVERY window -- popups included -- so each rule painted a pale
  // line straight across any menu or tool flyout that crossed it. The tool
  // palette's own rule runs the full height of the mid band, so the flyout
  // (which opens directly beside the palette) got two of them through it every
  // time.
  //
  // The overdraw the foreground list was defending against cannot happen:
  // ui/AtelierLayout gives every rule its OWN gap rather than laying it over a
  // band. `hRule()` consumes the cursor as it emits, and the mid band's canvas
  // starts at `leftRule.right()` and stops at `rightRule.x` -- so no window is
  // ever positioned over a rule's rect, and a list drawn behind the windows
  // shows through the gaps exactly as intended. The rules keep their full
  // thickness and popups now draw over them, which is the correct order.
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  for (size_t i = 0; i < bands.ruleCount; ++i) {
    const AtelierRect& r = bands.rules[i];
    dl->AddRectFilled(ImVec2(r.x, r.y), ImVec2(r.right(), r.bottom()), atelierToken(kRule));
  }
}

AtelierPaneDocuments atelierPaneDocuments(DocumentSession& session,
                                          AtelierSplitState& state) {
  AtelierPaneDocuments out;
  if (state.focusedPane != 0 && state.focusedPane != 1) state.focusedPane = 0;

  OpenDocument* active = session.active();
  if (active == nullptr) {
    state.companion = 0;
    state.focusedPane = 0;
    return out;  // one empty pane: the canvas still draws paper with no document
  }

  out.pane[0] = active;
  if (state.mode == AtelierSplit::Single || session.count() < 2) {
    // Nothing to put in a second pane. The companion is dropped rather than
    // remembered: re-opening the split re-derives it from the tab order, which
    // is one rule instead of a remembered one that can go stale.
    state.companion = 0;
    state.focusedPane = 0;
    return out;
  }

  OpenDocument* companion = state.companion != 0 ? session.find(state.companion) : nullptr;
  if (companion == nullptr || companion->id == active->id) {
    const size_t activeIndex = session.activeIndex();
    companion = session.at(activeIndex > 0 ? activeIndex - 1 : activeIndex + 1);
  }
  if (companion == nullptr || companion->id == active->id) {
    state.companion = 0;
    state.focusedPane = 0;
    return out;
  }

  state.companion = companion->id;
  out.count = 2;
  out.focusedPane = state.focusedPane;
  out.pane[state.focusedPane] = active;
  out.pane[1 - state.focusedPane] = companion;
  return out;
}

bool drawAtelierTabStrip(AppState& st, const AtelierBands& bands,
                         AtelierSplitState& split, std::string* statusOut) {
  if (bands.tabStrip.empty()) return false;
  beginBand("##atelierTabs", bands.tabStrip, kChromeMid);

  bool newDocument = false;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float h = bands.tabStrip.h;
  float x = bands.tabStrip.x;
  const float top = bands.tabStrip.y;

  // The two split icons sit hard against the right edge, so the tabs and the
  // `+` stop short of them. Reserved *before* the loop rather than checked
  // inside it: a tab that had already been drawn could not be un-drawn.
  const float splitIconsX = bands.tabStrip.right() - 2.0f * h;

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
    if (x + tabW > splitIconsX) break;  // no scroll yet; see below

    ImGui::SetCursorScreenPos(ImVec2(x, top));
    ImGui::PushID(static_cast<int>(i));
    // **The close box sits inside this button's rect, so this one has to let
    // it through.** Without this line the tab claims the hover for its whole
    // width, `ItemHoverable()` refuses every later item overlapping it
    // (`g.HoveredId != id && !g.HoveredIdAllowOverlap`), and the `##close`
    // button submitted below is never hovered, never clicked and never even
    // drawn in its hover colour. That is not a subtlety -- it is why clicking
    // the `x` did nothing at all: the click landed on the tab, which merely
    // made it active, and on the tab that was already active it did literally
    // nothing. No amount of work behind the close box could have been reached.
    //
    // `SetNextItemAllowOverlap()` is ImGui's front-to-back hit test: the item
    // submitted *later* wins the overlap, which is exactly the reading a user
    // has of a small control drawn on top of a large one. The price is that
    // the tab's own hover is gated on the previous frame's hovered id, so its
    // fill lights one frame after the pointer arrives -- invisible at any
    // frame rate this application runs at, and the correct trade for a close
    // box that works.
    ImGui::SetNextItemAllowOverlap();
    if (ImGui::InvisibleButton("##tab", ImVec2(tabW, h))) st.documents.setActive(i);
    // False while the pointer is over the close box, which is what makes the
    // tab stop painting its hover fill the moment the `x` takes the hit.
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

    // Close. A clean document goes on this click; a dirty one raises the
    // Save / Don't Save / Cancel question (app/CloseDecision.hpp), which the
    // File menu's own Close Document raises too -- one decision point, so the
    // two paths cannot give a user different answers about the same document.
    //
    // This used to call `st.documents.close(i, false, ...)` and drop the
    // refusal into the status line. PRD I11 was satisfied on paper and the
    // control read as broken in the hand.
    ImGui::SetCursorScreenPos(ImVec2(x + tabW - 18.0f, top + (h - 14.0f) * 0.5f));
    bool closedHere = false;
    if (ImGui::InvisibleButton("##close", ImVec2(14.0f, 14.0f))) {
      const CloseOutcome outcome = requestDocumentClose(st.documents, i, st.pendingClose);
      if (statusOut != nullptr && !outcome.status.empty()) *statusOut = outcome.status;
      closedHere = outcome.closed;
    }
    // **Only a close leaves the loop.** A raised question changed nothing --
    // the document is still open and still in the list at the same index -- so
    // the strip finishes drawing normally and the tab the user clicked stays
    // on screen behind the modal, which is where they need to see it. Breaking
    // for a question would blank every tab to the right of it for a frame and
    // make the strip appear to lose documents at the moment it asks about one.
    if (closedHere) {
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
  if (x + h > splitIconsX) {
    ImGui::SetCursorScreenPos(ImVec2(splitIconsX - 30.0f, top));
    capsLabel("...");
  }

  // --- the two split icons (docs/ui.md section 5) ------------------------
  //
  // Hand-drawn from two rectangles each, for drawAtelierTabStrip()'s own
  // reason: this build loads one 13 px text face and no icon font, so a
  // `columns-2` glyph does not exist to draw. Two panes of a rounded outline
  // is unambiguous at 34 px and needs no atlas.
  //
  // Disabled -- drawn in the divider grey and inert -- with fewer than two
  // documents open. A split control that produces one pane is a control that
  // appears broken, and the tooltip says which of the two it is.
  const bool canSplit = st.documents.count() >= 2;
  const AtelierSplit modes[2] = {AtelierSplit::Columns, AtelierSplit::Rows};
  const char* tips[2] = {"Split side by side", "Split top and bottom"};
  for (int i = 0; i < 2; ++i) {
    const float ix = splitIconsX + static_cast<float>(i) * h;
    ImGui::SetCursorScreenPos(ImVec2(ix, top));
    ImGui::PushID(1000 + i);
    const bool pressed = ImGui::InvisibleButton("##split", ImVec2(h, h));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    const bool on = canSplit && split.mode == modes[i];
    if (pressed && canSplit)
      // The way out is the way in: pressing the arrangement that is already on
      // returns to a single pane, so two icons cover three states.
      split.mode = on ? AtelierSplit::Single : modes[i];
    if (hovered)
      ImGui::SetTooltip("%s", canSplit ? tips[i]
                                       : "Split needs a second open document");

    if (on)
      dl->AddRectFilled(ImVec2(ix, top), ImVec2(ix + h, top + h), atelierToken(kChromeDeep));
    // Disabled is `kChromeBase`, and **not** `kDivider`: ui/AtelierTheme.hpp
    // asserts `kDivider == kChromeMid`, which is this band's own background,
    // so a disabled icon in the divider grey is an icon nobody can see. That
    // is not a hypothetical -- it is what the first screenshot of this control
    // showed, an empty right-hand end of a strip the design puts two icons in.
    // Chrome base is the palette's one value darker than the band that is not
    // the near-black, so it reads as present and recessive rather than absent.
    const ImU32 col = !canSplit  ? atelierToken(kChromeBase)
                      : on       ? atelierToken(kAccent)
                      : hovered  ? atelierToken(kTextPrimary)
                                 : atelierToken(kTextSecondary);
    // A 16x14 outline with one internal rule, cut the way the pane it stands
    // for is cut.
    const float bx = ix + (h - 16.0f) * 0.5f;
    const float by = top + (h - 14.0f) * 0.5f;
    dl->AddRect(ImVec2(bx, by), ImVec2(bx + 16.0f, by + 14.0f), col, 2.0f, 0, 1.5f);
    if (modes[i] == AtelierSplit::Columns)
      dl->AddLine(ImVec2(bx + 8.0f, by), ImVec2(bx + 8.0f, by + 14.0f), col, 1.5f);
    else
      dl->AddLine(ImVec2(bx, by + 7.0f), ImVec2(bx + 16.0f, by + 7.0f), col, 1.5f);
  }

  endBand();
  return newDocument;
}

void drawAtelierOptionsBar(AppState& st, const AtelierBands& bands,
                           const std::string& refusal) {
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
  // The value inside the slider is a live numeric, so it is monospace: it
  // changes every frame of a drag, and a proportional face makes the track's
  // text jump about as the digits change width.
  pushAtelierMono();
  ImGui::SliderFloat("##size", &st.brush.radius, 2.0f, 90.0f, "%.0f px");
  popAtelierMono();

  bandSeparator();
  capsLabel("HARD");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110.0f);
  pushAtelierMono();
  ImGui::SliderFloat("##hard", &st.brush.hardness, 0.0f, 1.0f, "%.2f");
  popAtelierMono();

  bandSeparator();
  // The same range drawBrushSection() uses (0..2.5), not a second one invented
  // here: one field behind two widgets with two ranges is two clamps, and the
  // narrower one silently truncates what the other set.
  capsLabel("LOAD");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110.0f);
  pushAtelierMono();
  ImGui::SliderFloat("##load", &st.brush.load, 0.0f, 2.5f, "%.2f");
  popAtelierMono();

  bandSeparator();
  capsLabel("WET");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110.0f);
  pushAtelierMono();
  ImGui::SliderFloat("##wet", &st.brush.wetness, 0.0f, 3.0f, "%.2f");
  popAtelierMono();

  // --- what the next gesture will actually hit -----------------------------
  //
  // The layers panel says which layer is selected; this says what *using this
  // tool* on it does, which is a different question and one no other part of
  // the chrome answers. A brush that routes to the solver when the user thinks
  // they are painting a layer, a brush that refuses because the layer is
  // locked, and a bucket whose click is refused all look identical on the
  // canvas: nothing happens where you expected paint.
  //
  // Two tables feed it, because there are two kinds of gesture and one enum
  // cannot honestly cover both: `strokeRouteFor()` (app/StrokeSession §1) for
  // the tools that make a stroke, and `pixelOpRefusalFor()` (§6) for the two
  // that fill. Both branches use their table's own "does this reach the user's
  // layer" predicate for the accent, so the colour means the same thing in
  // either.
  bandSeparator();
  const OpenDocument* od = st.documents.active();
  const Layer* target = od != nullptr ? activeLayerOf(*od) : nullptr;
  const StrokeRoute route = strokeRouteFor(st.brush.tool, target);
  if (!refusal.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(atelierToken(kAccent)));
    ImGui::TextUnformatted(refusal.c_str());
    ImGui::PopStyleColor();
  } else if (target == nullptr) {
    capsLabel("NO LAYER");
  } else if (toolWritesRgbPixels(st.brush.tool)) {
    // **The paint bucket and the gradient are not strokes, and this band used
    // to lie about both of them.** `strokeRouteFor()` answers `None` for every
    // tool that cannot begin a stroke -- correct for its own table, and wrong
    // here: with the bucket selected over a perfectly writable RGB layer this
    // read a grey "-> none", so the one place in the chrome that answers "what
    // will this tool do to this layer" said "nothing" about a tool that was
    // about to fill it.
    //
    // They get their own answer, off `pixelOpWritesLayer()` -- the same
    // predicate the canvas block gates the click with and the same one its
    // refusal sentence comes from (app/StrokeSession §6), so this band and that
    // refusal cannot disagree about whether the fill will land.
    const bool writes = pixelOpWritesLayer(target);
    pushAtelierMono();
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        ImGui::ColorConvertU32ToFloat4(atelierToken(writes ? kAccent : kTextSecondary)));
    ImGui::Text("-> %s", writes ? "rgb-fill" : "none");
    ImGui::PopStyleColor();
    popAtelierMono();
    if (ImGui::IsItemHovered()) {
      // The refusal's own sentence in the tooltip rather than a second wording
      // of it, and shown whether or not the user has clicked yet -- so the
      // answer is available *before* the wasted gesture as well as after it.
      const std::string why =
          pixelOpRefusalMessage(pixelOpRefusalFor(target), target, toolName(st.brush.tool));
      if (why.empty())
        ImGui::SetTooltip("The active layer is \"%s\".\nThe %s fills its RGB pixels.",
                          target->name.c_str(), toolName(st.brush.tool));
      else
        ImGui::SetTooltip("%s", why.c_str());
    }
  } else {
    pushAtelierMono();
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        ImGui::ColorConvertU32ToFloat4(atelierToken(
            strokeRouteWritesLayer(route) ? kAccent : kTextSecondary)));
    ImGui::Text("-> %s", strokeRouteName(route));
    ImGui::PopStyleColor();
    popAtelierMono();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The active layer is \"%s\".\nA %s stroke on it goes to: %s",
                        target->name.c_str(), toolName(st.brush.tool), strokeRouteName(route));
  }

  endBand();
}

void drawAtelierStatusBar(AppState& st, const AtelierBands& bands, uint32_t canvasW,
                          uint32_t canvasH) {
  beginBand("##atelierStatus", bands.statusBar, kChromeBase);
  // The whole band is numerics and caps markers -- there is no prose in it --
  // so the face is pushed once around the lot rather than at five sites.
  pushAtelierMono();
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

  popAtelierMono();
  endBand();
}

}  // namespace np
