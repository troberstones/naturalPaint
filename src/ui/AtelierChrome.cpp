#include "ui/AtelierChrome.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "app/PenTool.hpp"
#include "app/TextTool.hpp"    // toolEditsText()
#include "app/CloseDecision.hpp"
#include "app/CropTool.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/GradientTool.hpp"
#include "app/Memory.hpp"
#include "app/MoveTool.hpp"
#include "app/StrokeSession.hpp"
#include "app/ZoomAndSize.hpp"
#include "color/Space.hpp"
#include "core/TileStore.hpp"
#include "ui/AtelierTheme.hpp"
#include "ui/Fonts.hpp"
#include "ui/MacPaintUI.hpp"

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
// A small on/off chip: a `SmallButton` whose accented state says it is on.
// `Checkbox` is what the flood-fill row uses and is right there -- a labelled
// tick for a setting with a name. These four are DIFFERENT: B / I / L / C / R /
// J are a mutually-legible row of single letters where the letter IS the label,
// and six ticks in a row read as a list of unrelated options rather than as one
// choice. Returns true on the frame it was pressed; the caller owns the state,
// so a radio group and a pair of independent toggles both fall out of it.
bool atelierToggleChip(const char* id, bool on) {
  if (on) {
    ImGui::PushStyleColor(ImGuiCol_Button, atelierToken(kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, atelierToken(kAccent));
    ImGui::PushStyleColor(ImGuiCol_Text, atelierToken(kOnAccent));
  }
  const bool pressed = ImGui::SmallButton(id);
  if (on) ImGui::PopStyleColor(3);
  return pressed;
}

// ASCII case-insensitive substring test, for the font filter box. ASCII-only
// on purpose and not a locale-aware fold: a font family name that needs one
// (a Japanese family typed in kana) will not match a Latin filter string
// either way, and pulling in a locale fold to almost-handle that would be a
// behaviour that varies by machine -- which is the property
// `availableFontFamilies()`'s own byte-wise sort was chosen to avoid.
bool containsNoCase(const std::string& haystack, const char* needle) {
  const size_t n = std::strlen(needle);
  if (n == 0) return true;
  if (haystack.size() < n) return false;
  auto lower = [](unsigned char c) {
    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c);
  };
  for (size_t i = 0; i + n <= haystack.size(); ++i) {
    size_t k = 0;
    while (k < n && lower(static_cast<unsigned char>(haystack[i + k])) ==
                        lower(static_cast<unsigned char>(needle[k])))
      ++k;
    if (k == n) return true;
  }
  return false;
}

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
    // **Built**, as of app/MoveTool: a pen-down begins an app/TransformSession
    // on the active layer (or on the selection's pixels), the drag accumulates
    // a pure translation and pen-up commits it, and the arrow keys are the same
    // gesture from the keyboard. Same reason the eraser's row below sits in
    // this half of the table: the rows are in `Tool`'s declaration order and
    // the static_assert rests on that, so the divider marks where the enum's
    // not-built run began, not a second list to keep in step.
    {"Move", "move", 57633u, "V", true},
    {"Lasso", "lasso", 57806u, "L", true},
    {"Polygon Lasso", "pentagon", 58667u, "Shift+L", true},
    {"Magic Wand", "wand-sparkles", 58199u, "W", true},
    // **Built**: app/CropTool, gated by `toolCropsCanvas()` -- the eighth
    // canvas gate, and a new predicate rather than a name added to an existing
    // one for `toolMeasuresCanvas()`'s reason, which is concrete here:
    // `toolDrawsSelection()` is the gate on the selection tools' canvas block,
    // so a Crop widened into it would have every crop drag handed to
    // `commitDrawnSelection()`. Two modes, chosen in this band's own row
    // below -- a rectangle through `cropDocument()` and a four-corner
    // perspective through `transformFromQuad()` + `transformDocument()`.
    // `Tool::Slice`, which shares its palette group and its cursor, is still
    // one of the not-built cells and stays false.
    {"Crop", "crop", 57515u, "C", true},
    // **Built**: app/MeasureLine, gated by `toolMeasuresCanvas()` -- the one
    // tool in this palette whose gesture writes no texel at all. Same
    // arrangement as the eraser row below: the rows are in `Tool` declaration
    // order, so a built tool stays where the enum puts it and the divider
    // above marks the enum's not-built run, not a second sorted half.
    {"Measure", "ruler", 57675u, "", true},
    {"Frame", "frame", 58001u, "", false},
    // **Built**, as of the clone route: brush/CloneStamp, and
    // app/StrokeSession §1b for the table it routes through. It stays in this
    // half of the table for the same reason the Eraser row just below does --
    // the rows are in `Tool`'s declaration order and the static_assert rests on
    // that, so the divider above marks where the enum's not-built run began,
    // not a second list to keep in step. This tool needs the flag twice over:
    // it makes the palette cell clickable at all, and its Option+click source
    // gesture only exists while the cell is selected.
    {"Clone Stamp", "stamp", 58299u, "S", true},
    // **Built**, as of the RGB erase route: PRD F9/F10 (P0), ADR-0007,
    // brush/RgbErase. It stays in this half of the table because the rows are in
    // `Tool`'s declaration order and the static_assert below rests on that --
    // the divider above marks where the enum's not-built run began, not a second
    // list to keep in step. Flipping this flag is what makes the palette cell
    // clickable at all; a route that works behind a disabled cell is a feature
    // no user can reach.
    {"Eraser", "eraser", 57999u, "E", true},
    {"Paint Bucket", "paint-bucket", 58086u, "Shift+G", true},
    {"Gradient", "blend", 58780u, "G", true},
    // **Built**, as of the aliased-mark route: brush/PencilDeposit,
    // `StrokeRoute::PencilDeposit`, app/StrokeSession §1's Pencil rows. Same
    // note as the Eraser above about why it stays in this half of the table:
    // the rows are in `Tool`'s declaration order and the static_assert below
    // rests on that, so the divider marks where the enum's not-built run began
    // rather than a second list to keep in step.
    {"Pencil", "pencil", 57849u, "", true},
    // **Built**, as of the tonal route: `strokeRouteFor()` sends both to
    // `StrokeRoute::TonalBrush` on a writable RGB layer (brush/TonalBrush;
    // app/StrokeSession.hpp §1's Dodge/Burn rows). Two rows for one engine and
    // one route -- the palette is a list of tools a user picks, and the
    // direction is what the pick means. Same placement argument as the Eraser
    // row above: the rows are in `Tool`'s declaration order and the
    // static_assert below rests on that.
    {"Smudge", "droplets", 57525u, "N", true},
    {"Dodge", "sun", 57720u, "O", true},
    {"Burn", "moon", 57630u, "Shift+O", true},
    // **Built**, as of the smudge route: brush/Smudge, StrokeRoute::Smudge.
    // Same rule as the Eraser row above -- the flag flips in the commit that
    // wires the drag, not in the one that writes the arithmetic, and until it
    // flips the palette cell is disabled and the route is unreachable however
    // complete the engine is.
    // Pen and Curve flip **here**, in the commit that adds their canvas block
    // to `drawUI()` and `toolEditsPath()` to `toolHasCanvasHandler()` below.
    // That pairing is not a style preference: `app/selftest/Eyedropper.cpp`
    // asserts `toolImplemented(t) == toolHasCanvasHandler(t)` for every tool
    // and separately asserts `toolNoHandlerException()` is empty, so flipping
    // either half alone turns the suite red -- and the tempting repair is a
    // row in the table that is asserted to have none.
    {"Pen", "pen-tool", 57649u, "P", true},
    {"Curve", "spline", 58251u, "Shift+P", true},
    {"Text", "type", 57752u, "T", true},
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

bool toolHasCanvasHandler(Tool t) noexcept {
  // Ten gates, each of them the expression the corresponding block in
  // `ui/MacPaintUI.cpp`'s canvas is actually written with. Nothing here is a
  // restatement of "which tools work" -- see the header.
  //
  // **`toolMeasuresCanvas()` is a NEW term, not a name added to
  // `toolSamplesCanvas()`.** `Tool::Measure` sits in the eyedropper's palette
  // group and answers the same `ToolCursor::Sample`, so widening that
  // predicate is the one-line way to make this function go true for it. It is
  // also precisely the failure the Zoom assertion in
  // `app/selftest/Eyedropper.cpp` exists to catch -- "wired by widening an
  // existing predicate rather than adding the next one" -- and here the
  // consequence is concrete rather than stylistic: `toolSamplesCanvas()` is
  // the gate on the eyedropper's own canvas block, so a Measure that
  // satisfied it would have every ruler drag handed to
  // `applyEyedropperPick()`. See `app/StrokeSession.hpp` §6b.
  //
  // `toolBeginsStroke()` is not `noexcept` (it builds two probe Layers), and
  // this function is, so the call is guarded by short-circuit ordering alone
  // -- which is not enough on its own. It is the last term deliberately: the
  // cheap `noexcept` predicates are tested first, so for every tool that
  // has any other kind of handler the allocating one is never reached. For the
  // rest, an allocation failure here would `std::terminate` rather than
  // propagate, which is the correct outcome for a chrome predicate that runs
  // every frame -- there is no sensible answer to "is this tool handled" under
  // bad_alloc, and returning `false` would silently un-implement every brush.
  //
  // `toolMovesPixels()` is the seventh, and app/MoveTool.hpp section 5 is the
  // argument for it being its own gate rather than a term folded into
  // `toolWritesRgbPixels()` (the bucket/gradient family, whose refusal ladder
  // and cursor Move does not share) or `toolPansView()` (which moves the VIEW,
  // not the content). Placed before the allocating `toolBeginsStroke()` for
  // the ordering reason stated above: it is cheap and `noexcept`.
  // `toolCropsCanvas()` is the eighth, and it lives in `app/CropTool` rather
  // than in `app/StrokeSession` beside the other predicates for the reason
  // that header's section 6 states: it is one tool's own module's answer about
  // its own tool, and widening `toolDrawsSelection()` instead -- the one-line
  // way to make this go true for Crop -- would hand every crop drag to
  // `commitDrawnSelection()`. Placed before the allocating `toolBeginsStroke()`
  // for the ordering reason stated above: it is cheap and `noexcept`.
  return toolWritesRgbPixels(t) || toolDrawsSelection(t) || toolSamplesCanvas(t) ||
         toolMeasuresCanvas(t) || toolPansView(t) || toolMovesPixels(t) ||
         toolCropsCanvas(t) || toolBeginsStroke(t) || toolZoomsView(t) ||
         toolEditsPath(t) || toolEditsText(t);
}

const char* toolNoHandlerException(Tool) noexcept {
  // **Empty, and that is the point: the one row it held has been paid off.**
  //
  // It recorded Tool::Zoom -- implemented=true, a bespoke ToolCursor::Zoom,
  // and no canvas handler -- and said "delete this row the day it lands."
  // Scrubby zoom landed, `toolZoomsView()` is the sixth gate above, and the
  // row is gone. The forcing function worked exactly as designed: the
  // assertion in app/selftest/Eyedropper.cpp fails the moment a recorded
  // exception acquires a handler, so this could not be left behind to rot
  // into a lie about what the tool does.
  //
  // Kept rather than deleted outright because the mechanism is the valuable
  // part, not the row: a tool marked implemented with no handler is the
  // silent no-op the reachability audit was written about, and this is where
  // a future one has to be argued for in prose before it can ship.
  return nullptr;
}

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
  // **Content only -- no window of its own**, unlike every earlier revision
  // of this function. See this function's declaration comment
  // (ui/AtelierChrome.hpp) for why: `bands.tabStrip` now overlaps
  // `BeginMainMenuBar()`'s own window in screen space, and a `NoBringToFront
  // OnFocus` band window (this one's old `beginBand()`) is inserted at the
  // *front* of Dear ImGui's window list on creation (`CreateNewWindow()`,
  // imgui.cpp) -- the opposite end from a plain window like the menu bar's,
  // which always ends up on top of it. Two overlapping windows would not
  // merely look wrong: the tab strip's own window would draw and immediately
  // be painted over by the menu bar's background, which is exactly what the
  // first attempt at this merge did, silently. Drawing straight into the
  // caller's already-open window sidesteps the question entirely -- there is
  // only one window in this row now, and it owns one draw list.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // The kChromeMid fill `beginBand()` used to give this band as a WHOLE
  // window background, painted by hand instead: the two-shade look (this
  // region a step lighter than the title row's own kChromeBase) is still the
  // design's, it is just one rect on the caller's draw list now rather than
  // a second window's background.
  dl->AddRectFilled(ImVec2(bands.tabStrip.x, bands.tabStrip.y),
                    ImVec2(bands.tabStrip.right(), bands.tabStrip.bottom()),
                    atelierToken(kChromeMid));

  bool newDocument = false;
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
    // The delayed tooltip check is deliberately separate from `hovered`
    // above: that bool also drives the tab's hover fill a few lines down,
    // and gating the fill on the same stationary+delay timer would make the
    // tab itself feel laggy to hover, not just its tooltip.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
      ImGui::SetTooltip("%s", name.c_str());

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
  ImGui::SetItemTooltip("New document");
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
    // Separate from `hovered`, which also colours the icon itself a few
    // lines down -- delaying that shared bool would delay the hover tint,
    // not just the tooltip.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
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

  return newDocument;
}

void drawAtelierOptionsBarContent(AppState& st, float bandH, const std::string& refusal) {
  // **Content only -- no window of its own.** The options bar used to be a
  // band welded under the tab strip, so it opened its own `beginBand()` window
  // and the only question was which rect. It is a dockable panel now
  // (app/ControlsLayout.hpp's `ControlsSection::Options`), so it can be a slot
  // in any of the four docks or the body of a flyout, and every one of those
  // is a caller that has ALREADY made a window and wants this drawn into it.
  //
  // The version in between took a rect and still made its own window, and it
  // did not work: a `Begin()` inside a dock's `BeginChild()` is a second
  // top-level window floating at the same coordinates, so the panel drew
  // either behind its own dock or over its neighbours depending on focus
  // order. Drawing into the caller's window is the fix, and it also deletes
  // the special case the dock had to carry for this one panel.
  centreInBand(bandH, ImGui::GetFrameHeight());

  // The accent block that leads the band in the design (`[]BRUSH`): the active
  // tool, marked in the one colour reserved for "this is on".
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float h = ImGui::GetFrameHeight();
  ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, p.y), ImVec2(p.x + 6.0f, p.y + h),
                                            atelierToken(kAccent));
  ImGui::Dummy(ImVec2(6.0f, h));
  ImGui::SameLine(0.0f, 8.0f);
  ImGui::TextUnformatted(toolName(st.brush.tool));

  // --- the eyedropper's own two options (PRD Q10, P0) ---------------------
  //
  // **The first tool in this band to have options of its own.** Everything
  // below is the brush's -- SIZE, HARD, LOAD, WET -- and it used to be drawn
  // for every tool, so selecting the eyedropper showed four brush sliders and
  // none of the two settings PRD Q10 actually names ("with sample size,
  // sample-all-layers"). The band's job per docs/ui.md section 2 is "the active
  // tool and its options", so the active tool's options are what it shows.
  //
  // Both controls are combos rather than sliders, and both for the same
  // reason: neither field is continuous. Sample size is Photoshop's fixed
  // ladder (`core::kProbeSampleSizes`) and a slider would offer the 46x46
  // average nobody wants; sample source is three named states.
  //
  // **One field, one widget, one range.** This band's LOAD slider states the
  // rule -- "one field behind two widgets with two ranges is two clamps, and
  // the narrower one silently truncates what the other set" -- and SIZE, four
  // lines down, already breaks it (2..90 here against the BRUSH panel's
  // 1..200). These two are surfaced here and nowhere else, so there is nothing
  // for a second range to disagree with, and if either ever appears in a second
  // panel it must offer `kProbeSampleSizes` and `ProbeSource` entire.
  if (st.brush.tool == Tool::Eyedropper) {
    bandSeparator();
    capsLabel("SAMPLE");
    ImGui::SameLine();
    // Wide enough for "101 by 101 Average", the longest label in the table --
    // measured from the string rather than guessed, so a label added to
    // `kProbeSampleSizeLabels` cannot silently start clipping.
    ImGui::SetNextItemWidth(
        ImGui::CalcTextSize("101 by 101 Average").x + ImGui::GetFrameHeight() + 16.0f);
    pushAtelierMono();
    int sizeIndex = 0;
    for (int i = 0; i < kProbeSampleSizeCount; ++i)
      if (kProbeSampleSizes[i] == st.eyedropper.sampleSize) sizeIndex = i;
    if (ImGui::BeginCombo("##sampleSize", kProbeSampleSizeLabels[sizeIndex])) {
      for (int i = 0; i < kProbeSampleSizeCount; ++i) {
        if (ImGui::Selectable(kProbeSampleSizeLabels[i], i == sizeIndex))
          st.eyedropper.sampleSize = kProbeSampleSizes[i];
      }
      ImGui::EndCombo();
    }
    popAtelierMono();

    bandSeparator();
    capsLabel("SOURCE");
    ImGui::SameLine();
    // The three ProbeSource values with the labels Photoshop uses for them,
    // in stack order (bottom of the stack question to top) rather than enum
    // order, so the menu reads as a widening scope.
    struct SourceRow {
      ProbeSource source;
      const char* label;
      const char* tip;
    };
    static constexpr SourceRow kSources[] = {
        {ProbeSource::CurrentLayer, "Current Layer",
         "The active layer's own stored colour, ignoring its visibility, its opacity and its "
         "mask -- what is ON the layer, so a hidden layer is still sampleable."},
        {ProbeSource::ActiveAndBelow, "Current & Below",
         "The stack composited from the bottom up to and including the active layer, ignoring "
         "everything above it. Honours visibility and opacity, because it asks what a stack "
         "SHOWS."},
        {ProbeSource::AllLayers, "All Layers",
         "The whole document as it is composited on screen -- the same colour an export "
         "produces at this pixel."},
    };
    int sourceIndex = 0;
    for (int i = 0; i < 3; ++i)
      if (kSources[i].source == st.eyedropper.source) sourceIndex = i;
    ImGui::SetNextItemWidth(
        ImGui::CalcTextSize("Current & Below").x + ImGui::GetFrameHeight() + 16.0f);
    pushAtelierMono();
    if (ImGui::BeginCombo("##sampleSource", kSources[sourceIndex].label)) {
      for (int i = 0; i < 3; ++i) {
        if (ImGui::Selectable(kSources[i].label, i == sourceIndex))
          st.eyedropper.source = kSources[i].source;
        // The asymmetry between the three modes is a real rule with a real
        // reason and is invisible from three short labels, so each row carries
        // it. A user who never opens the tooltip still gets the right answer;
        // one who wonders why a hidden layer sampled differently in two modes
        // finds out here instead of by experiment.
        ImGui::SetItemTooltip("%s", kSources[i].tip);
      }
      ImGui::EndCombo();
    }
    popAtelierMono();

    // What the last pick did. It is here rather than in the COLOR panel
    // because the one thing that needs saying -- that a pick in PIGMENT mode
    // moved the panel to RGB mode -- is a consequence of *this* tool, and this
    // band is where this build already puts "what your last gesture with this
    // tool actually did" (the stroke and fill refusals, below).
    if (!st.lastPickReport.empty()) {
      bandSeparator();
      ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::ColorConvertU32ToFloat4(atelierToken(kTextSecondary)));
      ImGui::TextUnformatted(st.lastPickReport.c_str());
      ImGui::PopStyleColor();
    }

    return;
  }

  // --- the measure tool's readout (app/MeasureLine) -----------------------
  //
  // **A readout, not a control**, and it is the second tool after the
  // eyedropper to take this band's early return rather than falling through
  // to SIZE/HARD/LOAD/WET. The four brush sliders are meaningless here for a
  // stronger reason than they are for the eyedropper: Measure has no tip at
  // all, so a SIZE slider would be a live control over something the tool
  // provably never reads.
  //
  // Photoshop puts these numbers in the Info panel. This build has no Info
  // panel, and inventing one for four floats would be a dock slot and a
  // layout decision for a tool that has nothing else to say -- while
  // `docs/ui.md` section 2 already gives this band the job of showing "the
  // active tool and its options". So they go here, where the eyedropper's
  // "what your last gesture did" line already set the precedent.
  //
  // Monospace, because all four are live numerics that change every frame of
  // a drag -- the same reason the SIZE slider's own value is mono, stated
  // where that widget is drawn.
  if (st.brush.tool == Tool::Measure) {
    bandSeparator();
    const OpenDocument* od = st.documents.active();
    if (!measureLineAppliesTo(st.measure, od != nullptr ? od->id : 0u)) {
      ImGui::PushStyleColor(ImGuiCol_Text,
                            ImGui::ColorConvertU32ToFloat4(atelierToken(kTextSecondary)));
      // The empty state says what to DO, not merely that there is nothing --
      // this tool leaves no other trace in the chrome, so a blank band is
      // indistinguishable from a broken one.
      ImGui::TextUnformatted("Drag a line on the canvas to measure it.");
      ImGui::PopStyleColor();
      return;
    }
    const MeasureReadout r = measureReadout(st.measure);
    pushAtelierMono();
    // W and H before L and A, in the order Photoshop's Info panel lists them.
    // `%+.1f` on the two components: their SIGN is the whole content of "which
    // way did I drag", and an unsigned run and rise would make a measurement
    // up-and-left indistinguishable from one down-and-right while the angle
    // beside it says they differ.
    //
    // "px" is spelled once, on L, and W/H are bare: all three are document
    // texels (app/MeasureLine.hpp §2) and repeating the unit three times in a
    // band this dense buys nothing. `deg` rather than a degree glyph -- the
    // options bar's mono face is loaded from the Latin block only.
    capsLabel("W");
    ImGui::SameLine();
    ImGui::Text("%+.1f", static_cast<double>(r.dx));
    ImGui::SameLine();
    capsLabel("H");
    ImGui::SameLine();
    ImGui::Text("%+.1f", static_cast<double>(r.dy));
    ImGui::SameLine();
    capsLabel("L");
    ImGui::SameLine();
    ImGui::Text("%.2f px", static_cast<double>(r.lengthPx));
    ImGui::SameLine();
    capsLabel("A");
    ImGui::SameLine();
    ImGui::Text("%.1f deg", static_cast<double>(r.angleDeg));
    popAtelierMono();
    return;
  }

  // --- the gradient tool's ramp and its spread mode -----------------------
  //
  // **A swatch, not a colour well.** The foreground well in the COLOR panel
  // already answers "what colour", and a second copy of it here would answer
  // the question the user is not asking. What a gradient tool has to show is
  // the *ramp* -- the colour AND the fade together, because the fade is half
  // of what this tool lays down and is the half a solid swatch cannot state.
  //
  // Drawn over a checkerboard, for the reason the layer panel's alpha-lock
  // chip is one (`ui/MacPaintUI.cpp`): a checkerboard is the transparency
  // mark every image editor already uses, so the transparent end of the ramp
  // reads as transparent rather than as "whatever colour the options bar
  // happens to be". Over a flat band fill, a foreground-to-transparent ramp
  // and a foreground-to-band-colour ramp are the same picture, and this is a
  // tool whose whole default is the difference between them.
  //
  // The third early return in this band, and the strongest case of the three:
  // Measure has no tip, and the gradient does not even have a stroke -- SIZE,
  // HARD, LOAD and WET are read by no code path this tool can reach.
  if (st.brush.tool == Tool::Gradient) {
    bandSeparator();
    capsLabel("RAMP");
    ImGui::SameLine();

    const float rampH = ImGui::GetFrameHeight() - 4.0f;
    const float rampW = 160.0f;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const ImVec2 r0(o.x, o.y + 2.0f);
    const ImVec2 r1(o.x + rampW, o.y + 2.0f + rampH);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // The checkerboard. Two neutrals rather than the theme's own tokens: this
    // is a transparency mark, not a surface, and it has to read as "nothing
    // here" against BOTH themes and against whatever colour the ramp lays
    // over it. Clipped to the swatch so the squares cannot spill into the
    // band when `rampW` is not a multiple of the cell.
    constexpr float kCell = 5.0f;
    constexpr ImU32 kCheckA = IM_COL32(0x9a, 0x9a, 0x9a, 0xff);
    constexpr ImU32 kCheckB = IM_COL32(0x6e, 0x6e, 0x6e, 0xff);
    dl->PushClipRect(r0, r1, true);
    dl->AddRectFilled(r0, r1, kCheckA);
    for (int row = 0; row * kCell < rampH; ++row) {
      for (int col = 0; col * kCell < rampW; ++col) {
        if (((row + col) & 1) == 0) continue;
        const ImVec2 c0(r0.x + col * kCell, r0.y + row * kCell);
        dl->AddRectFilled(c0, ImVec2(c0.x + kCell, c0.y + kCell), kCheckB);
      }
    }

    // The ramp itself, sampled from THE stop list -- the same one
    // `renderGradient()` is handed at pen-up (`app/GradientTool.hpp` § 1), so
    // this cannot become a picture of a gradient the tool no longer draws.
    //
    // One column per device pixel, which is what makes the fade smooth rather
    // than banded, and cheap: `gradientSampleStraight()` is a walk of four
    // stops and this runs only on the frames the gradient tool is selected.
    //
    // **`srgbEncode` on the way out, and only on the colour.** The stops are
    // scene-linear (`color/Space.hpp`), the options bar is display-referred,
    // and omitting the encode is the failure `foregroundLinearRgba()`'s own
    // header describes from the other direction -- the swatch would come out
    // far darker than the paint it is promising. Alpha is a coverage fraction
    // and is not a colour, so it is passed through untouched; encoding it
    // would make the fade reach halfway across the swatch instead of a third.
    const GradientStops stops = currentGradientStops(st.brush);
    const int columns = static_cast<int>(rampW);
    for (int i = 0; i < columns; ++i) {
      // Sampled at the column's CENTRE. Sampling at its left edge puts the
      // t=1 end one column short of the swatch's right edge, which is
      // invisible on a fade and obvious on a ramp with a hard last stop.
      const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(columns);
      const std::array<float, 4> c = gradientSampleStraight(stops, t);
      // **`clampToDisplayRange()` after the encode, one of the named set in
      // `color/Space.hpp`.** The stops come from the foreground, which is
      // scene-referred since T25a (app/AppState.hpp's `BrushState::rgb`), so
      // an encoded channel here can exceed 1.0 -- and `IM_COL32` shifts each
      // int into its own byte lane, which turns 334 into 0x14E and spills the
      // 0x1 into the neighbouring channel. Unclamped this ramp would not read
      // "too bright to draw", it would read as the wrong hue.
      const std::array<float, 3> enc =
          clampToDisplayRange({srgbEncode(c[0]), srgbEncode(c[1]), srgbEncode(c[2])});
      const ImU32 col = IM_COL32(static_cast<int>(enc[0] * 255.0f + 0.5f),
                                 static_cast<int>(enc[1] * 255.0f + 0.5f),
                                 static_cast<int>(enc[2] * 255.0f + 0.5f),
                                 static_cast<int>(c[3] * 255.0f + 0.5f));
      dl->AddRectFilled(ImVec2(r0.x + static_cast<float>(i), r0.y),
                        ImVec2(r0.x + static_cast<float>(i) + 1.0f, r1.y), col);
    }
    dl->PopClipRect();
    dl->AddRect(r0, r1, atelierToken(kDivider));

    // `Dummy` rather than nothing: the drawing above is on the draw list and
    // ImGui knows nothing about it, so without a laid-out item of the same
    // size the SPREAD combo would be positioned on top of the swatch.
    ImGui::Dummy(ImVec2(rampW, ImGui::GetFrameHeight()));
    ImGui::SetItemTooltip(
        "Foreground to transparent -- the one ramp this build offers, because there is no "
        "background colour for a second one to end at. Drag on the canvas from the ramp's "
        "start to its end.");

    bandSeparator();
    capsLabel("KIND");
    ImGui::SameLine();
    int kindIndex = 0;
    for (size_t i = 0; i < kGradientKindCount; ++i)
      if (kGradientKinds[i].kind == st.gradient.kind) kindIndex = static_cast<int>(i);
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Angular").x + ImGui::GetFrameHeight() + 16.0f);
    pushAtelierMono();
    if (ImGui::BeginCombo("##gradientKind", kGradientKinds[kindIndex].label)) {
      for (size_t i = 0; i < kGradientKindCount; ++i) {
        if (ImGui::Selectable(kGradientKinds[i].label, static_cast<int>(i) == kindIndex))
          st.gradient.kind = kGradientKinds[i].kind;
        // What the two handles MEAN changes per kind -- start-and-end,
        // centre-and-rim, centre-and-zero-angle -- and that is not guessable
        // from one word. A user who drags a Radial expecting a span and gets
        // a radius has been misled by the label, not by the tool.
        ImGui::SetItemTooltip("%s", kGradientKinds[i].tip);
      }
      ImGui::EndCombo();
    }
    popAtelierMono();

    bandSeparator();
    // **SPREAD goes dead for Angular, visibly and with the reason.** A sweep
    // wraps into [0, 1) by construction, and on that range all three spread
    // modes are the identity -- so the control would be live over something
    // that provably changes no texel, which is the same defect as a palette
    // cell for a tool that does not exist. `docs/ui.md` §4a settles it: no
    // dead button looks live. `--selftest` proves the inertness by rendering
    // the same sweep under all three modes.
    //
    // Disabled rather than hidden: a control that vanishes makes the band
    // reflow and leaves the user hunting for a setting they used a moment
    // ago. Greyed with a tooltip says where it went and why.
    //
    // The predicate is `gradientKindUsesSpread()`, not `kind == Angular`
    // spelled again here (`app/GradientTool.hpp` § 4).
    const bool spreadLive = gradientKindUsesSpread(st.gradient.kind);
    capsLabel("SPREAD");
    ImGui::SameLine();
    int spreadIndex = 0;
    for (size_t i = 0; i < kGradientSpreadCount; ++i)
      if (kGradientSpreads[i].spread == st.gradient.spread) spreadIndex = static_cast<int>(i);
    // Width measured from the longest label rather than guessed, the same way
    // the eyedropper's two combos are, so a label added to `kGradientSpreads`
    // cannot silently start clipping.
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Reflect").x + ImGui::GetFrameHeight() + 16.0f);
    pushAtelierMono();
    ImGui::BeginDisabled(!spreadLive);
    if (ImGui::BeginCombo("##gradientSpread", kGradientSpreads[spreadIndex].label)) {
      for (size_t i = 0; i < kGradientSpreadCount; ++i) {
        if (ImGui::Selectable(kGradientSpreads[i].label, static_cast<int>(i) == spreadIndex))
          st.gradient.spread = kGradientSpreads[i].spread;
        // What each mode does OUTSIDE the drag is the whole content of this
        // control and is unguessable from three one-word labels -- the same
        // argument the eyedropper's SOURCE rows make for carrying theirs.
        ImGui::SetItemTooltip("%s", kGradientSpreads[i].tip);
      }
      ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    // Outside the BeginDisabled pair on purpose: a disabled item does not
    // report hover, so a tooltip set inside it is exactly the tooltip nobody
    // can ever read -- which would leave the greyed control saying nothing at
    // all, the failure this whole branch exists to avoid.
    if (!spreadLive)
      ImGui::SetItemTooltip(
          "%s sweeps once around and wraps, so there is no outside for a spread mode to fill. "
          "Your choice is remembered and applies again on Linear and Radial.",
          gradientKindLabel(st.gradient.kind));
    popAtelierMono();
    return;
  }

  // --- the crop tool's MODE, its extent readout and its two verbs ---------
  //
  // **The fourth early return in this band, and `docs/ui.md` §4b's test settles
  // it without argument.** That test is not "does this tool have settings" but
  // *would the four brush sliders be live controls over something this tool
  // provably never reads*, and the crop reads none of them: nothing in
  // `app/CropTool` or in the two engines behind it (`cropDocument()`,
  // `transformDocument()`) touches a `BrushTip`. It is not the smudge's case --
  // the smudge keeps the sliders because `smudgeDab()` genuinely reads three of
  // the four -- and it is not a marginal call the way the smudge's was: the
  // crop has no tip, no stroke and no deposit at all. Six rows in §4b now.
  //
  // The user asked for the mode HERE specifically, rather than as a second
  // palette cell beside `Tool::Crop`, and that is also the right answer by this
  // build's own rules: `Tool::Marquee` and `Tool::EllipseMarquee` are two cells
  // because `docs/shortcuts.md` reserves a key for each, while Rectangle and
  // Perspective crop are one gesture with one commit and one refusal ladder --
  // two cells would be two `kToolMeta` rows, two cursors and two canvas blocks
  // for a difference of four draggable corners.
  if (st.brush.tool == Tool::Crop) {
    CropSession& crop = st.crop;
    bandSeparator();
    capsLabel("MODE");
    ImGui::SameLine();
    int modeIndex = 0;
    for (size_t i = 0; i < kCropModeCount; ++i)
      if (kCropModes[i].mode == crop.mode) modeIndex = static_cast<int>(i);
    // Measured INSIDE the mono push, unlike the gradient's combos above. Those
    // measure with the default font and get away with it because "Angular" is
    // short; "Perspective" is not, and the mono face is wider -- measuring
    // outside clipped the label to "Perspectiv" with the arrow over the "e".
    pushAtelierMono();
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Perspective").x + ImGui::GetFrameHeight() +
                            16.0f);
    if (ImGui::BeginCombo("##cropMode", kCropModes[modeIndex].label)) {
      for (size_t i = 0; i < kCropModeCount; ++i) {
        if (ImGui::Selectable(kCropModes[i].label, static_cast<int>(i) == modeIndex)) {
          // `cropSetMode()`, never a bare assignment. Switching to Rectangle
          // has to snap four dragged corners to their bounding box, and a
          // combo that only wrote the enum would leave a perspective quad on
          // screen being committed as its own bounding box -- a shape that is
          // not the shape the user is looking at.
          cropSetMode(crop, kCropModes[i].mode);
        }
        // What the mode changes is four handles versus eight and one resample
        // versus none, which is not guessable from one word -- the same
        // argument the gradient's KIND rows make for carrying theirs. The
        // Perspective row also carries the aspect-ratio honesty
        // (`app/CropTool.hpp` §3): the tool does not claim to recover the
        // scene's true proportions, and the only place a user can learn that
        // is here.
        ImGui::SetItemTooltip("%s", kCropModes[i].tip);
      }
      ImGui::EndCombo();
    }
    popAtelierMono();

    bandSeparator();
    // **The extent that will result, not the extent of the gesture**, and for
    // Perspective those are different numbers -- `perspectiveCropExtent()`'s
    // longer-of-opposite-edges rule (`app/CropTool.hpp` §3) is the whole
    // decision this feature turns on, and a user who cannot see the answer
    // before committing has to commit to find out. Drawn from the same function
    // the commit calls, so it cannot become a picture of a rule the tool no
    // longer follows.
    capsLabel("SIZE");
    ImGui::SameLine();
    pushAtelierMono();
    if (crop.active) {
      const DocumentRegion extent = crop.mode == CropMode::Rectangle
                                        ? cropRegionOf(crop)
                                        : perspectiveCropExtent(crop.quad);
      ImGui::Text("%u x %u", extent.width, extent.height);
    } else {
      // Not "0 x 0": there is no crop, and a pair of zeroes reads as a crop
      // that has collapsed rather than as one that has not been drawn.
      ImGui::TextUnformatted("--");
    }
    popAtelierMono();

    bandSeparator();
    // **Both verbs are visible, and that is the point.** Enter and Escape are
    // the shortcuts a user who knows them reaches for; a tool whose entire
    // interaction is dragging cannot have its only commit on a key, or half the
    // users never find it. These two set request flags read by the canvas
    // block, so the button and the key are one commit path
    // (`app/CropTool.hpp` §5).
    const std::string quadWhy =
        crop.mode == CropMode::Perspective ? cropQuadRefusal(crop.quad) : std::string{};
    const DocumentRegion rectRegion = cropRegionOf(crop);
    const bool rectUsable = crop.mode != CropMode::Rectangle || !rectRegion.empty();
    const bool committable = crop.active && quadWhy.empty() && rectUsable;
    ImGui::BeginDisabled(!committable);
    if (ImGui::Button("Crop")) crop.commitRequested = true;
    ImGui::EndDisabled();
    // Outside the disabled pair, for the SPREAD combo's stated reason: a
    // disabled item reports no hover, so a tooltip set inside one is the
    // tooltip nobody can read -- and this is precisely the case where the user
    // most needs to be told why the button is grey.
    if (!crop.active) {
      ImGui::SetItemTooltip("Drag a rectangle on the canvas first.");
    } else if (!quadWhy.empty()) {
      ImGui::SetItemTooltip("%s", quadWhy.c_str());
    } else if (!rectUsable) {
      ImGui::SetItemTooltip("The rectangle is less than one texel wide or tall.");
    } else {
      ImGui::SetItemTooltip("Crop the document to this rectangle. Enter does the same. Undo "
                            "gives back everything it hid -- the crop moves the pixels, it "
                            "does not delete them.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!crop.active);
    if (ImGui::Button("Cancel")) crop.cancelRequested = true;
    ImGui::EndDisabled();
    if (crop.active)
      ImGui::SetItemTooltip("Discard the crop rectangle. Escape does the same, and so does "
                            "choosing another tool.");

    // The refusal, or the steep-quad warning, in the band itself rather than
    // only in a tooltip -- a bow-tie is one careless drag away and the reason
    // has to be readable without hunting for the control that is greyed.
    if (!quadWhy.empty()) {
      bandSeparator();
      ImGui::PushStyleColor(ImGuiCol_Text, atelierToken(kAccent));
      ImGui::TextUnformatted(quadWhy.c_str());
      ImGui::PopStyleColor();
    } else if (crop.active && crop.mode == CropMode::Perspective &&
               cropQuadIsSteep(crop.quad)) {
      // **Not a refusal.** `ops/Transform.hpp` §3 says the resampler takes its
      // filter factor from the Jacobian at the destination CENTRE, so a strong
      // perspective is correctly filtered in the middle and progressively less
      // so at the far edge. That limitation is named there rather than fixed,
      // and this band is where it stops being silent -- the crop is still
      // correct, it is just soft where it is most magnified.
      bandSeparator();
      ImGui::PushStyleColor(ImGuiCol_Text, atelierToken(kTextSecondary));
      ImGui::TextUnformatted(
          "Steep quad: the far edge will be soft. The resampler picks its filter from the "
          "middle of the result.");
      ImGui::PopStyleColor();
    }
    return;
  }


  // --- Tool::Text: FONT, SIZE, style, ALIGN and COLOUR --------------------
  //
  // **The fifth early return in this band, and docs/ui.md section 4b's test
  // settles it without argument.** That test is not "does this tool have
  // settings" but *would the four brush sliders be live controls over
  // something this tool provably never reads*, and the Text tool reads none
  // of them: nothing in `core/TextContent`, `text/Shaper` or `app/TextTool`
  // touches a `BrushTip`. It has no tip, no stroke and no deposit -- the
  // crop's case exactly, not the smudge's marginal one.
  //
  // ==========================================================================
  // What these five controls edit, which is the only real decision here
  // ==========================================================================
  //
  // **They edit `st.textStyle` / `st.textAlign` -- the TOOL's own defaults --
  // and write through to the active Text layer when one is being edited.**
  // `app/AppState.hpp`'s own comment on those two members is the argument;
  // the short version is that a per-tool row cannot go dead for the entire
  // time before the first click, which is exactly when a user is choosing a
  // font.
  //
  // COLOR is the one exception and is NOT a tool default: a new text block
  // takes the FOREGROUND colour, like every other tool in this build, so a
  // third colour store would be a colour the user set that nothing painted
  // with. The swatch therefore edits the active layer's own `fill` and goes
  // dead -- visibly, with the reason -- when there is no Text layer, which is
  // SPREAD-on-Angular's precedent rather than a new idea.
  if (st.brush.tool == Tool::Text) {
    OpenDocument* textOd = st.documents.active();
    Layer* textLayer = textOd != nullptr ? activeLayerOf(*textOd) : nullptr;
    if (textLayer != nullptr && textLayer->kind != LayerKind::Text) textLayer = nullptr;

    // Every control below funnels its change through this, so "write through
    // to the layer, and mark the document dirty so the composite actually
    // updates" exists once rather than five times. The revision bump is not
    // optional: `core/DirtyTiles` compares two documents, and without a
    // recorded edit there is no *before* for it to compare against.
    auto pushStyleToLayer = [&](const char* what) {
      if (textLayer == nullptr || textOd == nullptr) return;
      textLayer->text.style = st.textStyle;
      textLayer->text.align = st.textAlign;
      textOd->recordEdit(what, EditKind::Content);
    };

    bandSeparator();
    capsLabel("FONT");
    ImGui::SameLine();

    // **The list is built once per process, not per frame.**
    // `availableFontFamilies()` walks the platform's whole font catalogue
    // (321 families on the machine this was written on), and this row draws
    // every frame the Text tool is selected. The invalidation story is stated
    // rather than hidden: a font installed while the app is running does not
    // appear until a restart. That is the wrong trade for a font manager and
    // the right one for a paint program, and it is one line to change if it
    // ever bites.
    static std::vector<std::string> families;
    static bool familiesLoaded = false;
    if (!familiesLoaded) {
      families = availableFontFamilies();
      familiesLoaded = true;
    }

    // The combo's label is `style.fontFamily` itself and NOT anything read
    // back from the shaper. `ShapedText::fontsUsed` reports POSTSCRIPT names
    // ("Times New Roman" comes back as "TimesNewRomanPSMT"), measured while
    // building this -- so a label sourced from there would show the user a
    // name they never picked and could not find in this list.
    const float fontComboW = 180.0f;
    ImGui::SetNextItemWidth(fontComboW);
    pushAtelierMono();
    if (ImGui::BeginCombo("##textFont", st.textStyle.fontFamily.c_str())) {
      // A filter box, because 321 rows is a scroll and not a choice. Kept in a
      // static for the same reason the list is: it is transient UI state with
      // no meaning to the document, and putting it on `AppState` would imply
      // it were worth persisting.
      static char fontFilter[64] = {};
      ImGui::SetNextItemWidth(fontComboW - 8.0f);
      ImGui::InputTextWithHint("##fontFilter", "filter", fontFilter, sizeof(fontFilter));
      ImGui::Separator();
      if (families.empty()) {
        // Only reachable on a build with no shaper (text/StubShaper.cpp), and
        // it says which rather than drawing an empty popup that looks broken.
        ImGui::TextDisabled("%s", shaperUnavailableReason()[0] != '\0'
                                      ? shaperUnavailableReason()
                                      : "no font families reported");
      }
      for (const std::string& f : families) {
        if (fontFilter[0] != '\0' && !containsNoCase(f, fontFilter)) continue;
        if (ImGui::Selectable(f.c_str(), f == st.textStyle.fontFamily)) {
          st.textStyle.fontFamily = f;
          pushStyleToLayer("text font");
        }
      }
      ImGui::EndCombo();
    }
    popAtelierMono();
    ImGui::SetItemTooltip(
        "The font family, from the platform's own catalogue (%zu here). A family this "
        "build cannot find is not an error: CoreText substitutes its own default and the "
        "text still renders, so an imported document with a typo in its font name still "
        "opens.",
        families.size());

    bandSeparator();
    capsLabel("SIZE");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(72.0f);
    pushAtelierMono();
    float sizePx = st.textStyle.sizePx;
    // Drag rather than a slider: type size has no natural maximum, and a
    // slider would have to invent one. `%.1f` because half-point sizes are
    // real and 0.1 px is the finest anyone sets by hand.
    if (ImGui::DragFloat("##textSize", &sizePx, 0.5f, 1.0f, 2000.0f, "%.1f px")) {
      st.textStyle.sizePx = std::max(1.0f, sizePx);
      pushStyleToLayer("text size");
    }
    popAtelierMono();
    ImGui::SetItemTooltip("Type size in document pixels at 1:1 zoom. Drag, or double-click "
                          "to type a value.");

    bandSeparator();
    // **B and I are toggles rather than a family-variant list**, and that is a
    // deliberate simplification with a named cost: CoreText resolves them
    // through its own font-descriptor traits, so a family with no italic gets
    // a synthesised slant rather than a refusal. Offering the real variant
    // list means enumerating each family's faces, which is a second platform
    // call and a second control; the day someone needs Semibold, that is the
    // change. Until then these two cover what a paint program's captions need.
    pushAtelierMono();
    bool bold = st.textStyle.bold;
    if (atelierToggleChip("B##textBold", bold)) {
      st.textStyle.bold = !bold;
      pushStyleToLayer("text weight");
    }
    ImGui::SameLine();
    bool italic = st.textStyle.italic;
    if (atelierToggleChip("I##textItalic", italic)) {
      st.textStyle.italic = !italic;
      pushStyleToLayer("text slant");
    }
    popAtelierMono();

    bandSeparator();
    // **ALIGN goes dead for POINT text, greyed with the reason in a tooltip.**
    // `text/Shaper.hpp` is explicit that a point-text block is one line with
    // nothing to align against, so a live control here would sit over
    // something that provably changes no texel -- the identical situation
    // SPREAD-on-Angular is in, decided the identical way (docs/ui.md section
    // 4a's "no dead button looks live"). Disabled rather than hidden so the
    // band does not reflow and the user's choice is still visible and still
    // remembered for the next paragraph block.
    const bool paragraph = textLayer != nullptr ? textLayer->text.frame.width > 0.0f
                                                : st.textEdit.frameDragActive;
    capsLabel("ALIGN");
    ImGui::SameLine();
    pushAtelierMono();
    ImGui::BeginDisabled(!paragraph);
    struct AlignChip {
      const char* label;
      TextAlign align;
      const char* tip;
    };
    static const AlignChip kAlignChips[] = {
        {"L##textAlignL", TextAlign::Left, "Left"},
        {"C##textAlignC", TextAlign::Center, "Centred"},
        {"R##textAlignR", TextAlign::Right, "Right"},
        {"J##textAlignJ", TextAlign::Justified,
         "Justified -- the last line stays left-aligned, which is CoreText's own rule"},
    };
    for (const AlignChip& chip : kAlignChips) {
      bool on = st.textAlign == chip.align;
      if (atelierToggleChip(chip.label, on)) {
        st.textAlign = chip.align;
        pushStyleToLayer("text alignment");
      }
      ImGui::SetItemTooltip("%s", chip.tip);
      ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::EndDisabled();
    popAtelierMono();
    // Outside the BeginDisabled pair on purpose: a disabled item reports no
    // hover, so a tooltip set inside it is the one tooltip nobody can read --
    // the gradient's SPREAD row learned this the same way.
    if (!paragraph)
      ImGui::SetItemTooltip(
          "This is point text -- one line, no wrapping, so there is nothing for an "
          "alignment to align against. Drag with the Text tool instead of clicking to make "
          "a paragraph box, and these come alive. Your choice is remembered.");

    bandSeparator();
    // **COLOR edits the LAYER, not a tool default** -- see this block's own
    // header. Dead with no Text layer, and the tooltip says where the colour
    // of the next block comes from instead, because "this control is off" and
    // "your text will be black" are different facts and the user needs both.
    capsLabel("COLOR");
    ImGui::SameLine();
    ImGui::BeginDisabled(textLayer == nullptr);
    // sRGB-encoded on the way in and decoded on the way out. `Paint::rgba` is
    // linear-light straight alpha (core/VectorShape.hpp), ImGui's picker is
    // display-referred, and skipping the conversion is the failure the
    // gradient ramp's own comment describes from the other side: the swatch
    // would read far darker than the text it is promising.
    std::array<float, 4> lin = textLayer != nullptr ? textLayer->text.fill.rgba
                                                    : std::array<float, 4>{0, 0, 0, 1};
    float enc[4] = {srgbEncode(lin[0]), srgbEncode(lin[1]), srgbEncode(lin[2]), lin[3]};
    if (ImGui::ColorEdit4("##textColor", enc,
                          ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar |
                              ImGuiColorEditFlags_AlphaPreviewHalf) &&
        textLayer != nullptr && textOd != nullptr) {
      textLayer->text.fill.on = true;
      textLayer->text.fill.rgba = {srgbDecode(enc[0]), srgbDecode(enc[1]), srgbDecode(enc[2]),
                                   enc[3]};
      textOd->recordEdit("text colour", EditKind::Content);
    }
    ImGui::EndDisabled();
    if (textLayer == nullptr)
      ImGui::SetItemTooltip(
          "No Text layer selected, so there is no text whose colour this could be. Click on "
          "the canvas to make one -- it takes the FOREGROUND colour, like every other tool "
          "here -- and then this edits it.");
    return;
  }

  // --- the magic wand's and the paint bucket's flood-fill options ---------
  //
  // **Six engine parameters across two tools, with zero visible controls until
  // this row existed.** `ops/FloodFill.hpp` has shipped `tolerance`, `edgeBand`
  // and `reach` fully implemented since it was written; both call sites in
  // `ui/MacPaintUI.cpp` default-constructed a `FloodFillParams` and set at most
  // one of them, from an undocumented Option-click. That is the same
  // engine-capability-with-no-control gap the KIND combo three blocks up closed
  // for the gradient, and this closes it for the other two tools.
  //
  // **Not an early return, unlike the three blocks above.** Eyedropper, Measure
  // and Gradient each `return` before the band's trailing "what will this tool
  // do to this layer" line; these two must not. That line is the one place in
  // the chrome that answers the bucket's refusals (a Pigment layer, a locked
  // layer), and its own comment records that the bucket used to be missing from
  // it and read a grey "-> none" about a tool that was about to fill. So the
  // row is drawn here, the four brush sliders below are skipped instead, and
  // the trailing block is reached exactly as before.
  //
  // The gate is `floodToolParamsFor()` -- `app/AppState.hpp`'s single mapping
  // from a tool to the block its click actually reads -- and not a
  // `tool == MagicWand || tool == PaintBucket` spelled again here. Those two
  // spellings agree until the day they do not, and the way they stop agreeing
  // is a row of live controls bound to a struct the canvas never looks at: a
  // toolbar that responds perfectly and changes nothing.
  FloodFillParams* flood = floodToolParamsFor(st, st.brush.tool);
  if (flood != nullptr) {
    bandSeparator();
    // Photoshop's units, 0..255, converted at the widget rather than stored --
    // `floodToleranceToUi()` carries the argument for why the number a painter
    // reads is not the number the engine holds. A local int, the same shape as
    // the SIZE slider's local half-diameter view of `diameterPx` below:
    // `SliderInt()` needs an `int*` it can write through every frame.
    capsLabel("TOLERANCE");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    pushAtelierMono();
    {
      int toleranceUi = floodToleranceToUi(flood->tolerance);
      // Written back only on an actual edit. An unconditional write-back would
      // push the field through the integer grid on every frame this tool is
      // selected, quantising a tolerance some other writer had set -- a widget
      // that silently rounds a value it is not being used to change.
      if (ImGui::SliderInt("##floodTolerance", &toleranceUi, 0, kFloodToleranceUiMax))
        flood->tolerance = floodToleranceFromUi(toleranceUi);
    }
    popAtelierMono();
    ImGui::SetItemTooltip(
        "How far from the texel you click a texel may be and still be taken: the largest "
        "per-channel difference, on the colours as DISPLAYED (0-255, Photoshop's units and "
        "Photoshop's default of 32). Measured on screen values rather than on stored light, so "
        "one number means the same thing in a sky and in a shadow.");

    bandSeparator();
    capsLabel("REACH");
    ImGui::SameLine();
    int reachIndex = 0;
    for (size_t i = 0; i < kFloodReachCount; ++i)
      if (kFloodReaches[i].reach == flood->reach) reachIndex = static_cast<int>(i);
    // Width measured from the longest label rather than guessed, the same rule
    // the eyedropper's two combos and the gradient's two state at their own
    // call sites: a label added to `kFloodReaches` cannot silently start
    // clipping.
    //
    // **Measured over the table, and INSIDE the mono push** -- on both counts
    // unlike those four. Each of them measures one hand-copied string with the
    // band's proportional face active and then draws it in the mono one, which
    // is a different metric; that over- or under-reserves by a little and has
    // been harmless only because their labels are short. This row's are not:
    // measured the old way, "Contiguous" reserved less than it draws and the
    // combo clipped it under its own arrow, which the first capture of the
    // `wand_options` golden view showed directly. Walking the table also means
    // the widest label wins whether or not whoever adds one notices that "All
    // Similar" is a character longer than "Contiguous".
    pushAtelierMono();
    float widestReachLabel = 0.0f;
    for (size_t i = 0; i < kFloodReachCount; ++i)
      widestReachLabel = std::max(widestReachLabel, ImGui::CalcTextSize(kFloodReaches[i].label).x);
    ImGui::SetNextItemWidth(widestReachLabel + ImGui::GetFrameHeight() + 16.0f);
    if (ImGui::BeginCombo("##floodReach", kFloodReaches[reachIndex].label)) {
      for (size_t i = 0; i < kFloodReachCount; ++i) {
        if (ImGui::Selectable(kFloodReaches[i].label, static_cast<int>(i) == reachIndex))
          flood->reach = kFloodReaches[i].reach;
        // Two words cannot carry "4-connected, so a diagonal hairline holds it
        // in" or "costs a pass over the whole document" -- the same argument
        // the eyedropper's SOURCE rows make for carrying theirs.
        ImGui::SetItemTooltip("%s", kFloodReaches[i].tip);
      }
      ImGui::EndCombo();
    }
    popAtelierMono();

    bandSeparator();
    // **A checkbox and not a slider, and the engine's own derivation is what
    // settles that.** `edgeBand` is a float, so a slider is the obvious
    // binding -- and it would be a track almost none of whose interior is
    // defensible. `ops/FloodFill.hpp` § 2 derives `kFloodEdgeBandFloor` as the
    // NARROWEST ramp that is not a lie (anything narrower cannot reach all 256
    // coverage levels the store holds, whatever the picture contains), and
    // `FloodFillParams` clamps the band to `tolerance` internally. So below the
    // floor the control claims precision the pipeline provably cannot deliver,
    // and above the tolerance it is clamped and moves no texel: at the shipped
    // default that is a slider whose left half lies and whose right half is
    // dead, which is the control `docs/ui.md` §4a forbids.
    //
    // What is left is two defensible values -- 0 and the floor -- and a
    // two-value field deserves a two-state control. Photoshop ships this same
    // field as an "Anti-alias" checkbox; that is corroboration here rather than
    // the reason, because the reason is above and is this build's own.
    //
    // The LABEL is Photoshop's word rather than the implementation's. "Edge
    // band" is what the code calls the ramp; ANTI-ALIAS is what the setting
    // does, and `ops/FloodFill.hpp` § 2 already equates the two in its own
    // prose ("`edgeBand == 0` ... is Photoshop's Anti-alias checkbox,
    // unticked").
    //
    // The field stays a float and the checkbox is a VIEW of it -- read as
    // `> 0`, written as one of the two values -- rather than `AppState` holding
    // a bool the call site expands. A bool there would be a second spelling of
    // the engine's own field, which is exactly the drift `app/AppState.hpp`
    // holds `FloodFillParams` itself in order to avoid.
    capsLabel("ANTI-ALIAS");
    ImGui::SameLine();
    {
      bool antiAlias = flood->edgeBand > 0.0f;
      if (ImGui::Checkbox("##floodAntiAlias", &antiAlias))
        flood->edgeBand = antiAlias ? kFloodDefaultEdgeBand : 0.0f;
    }
    ImGui::SetItemTooltip(
        "Soft edges. On, the outermost sliver of the accepted band is partially selected "
        "instead of in-or-out; off, every texel is fully in or fully out. It never changes "
        "WHICH texels are taken, only how much of the boundary ones -- so turning it off "
        "cannot move the region you are about to select or fill.");
  }

  // The brush's own four parameters, for the tools that actually carry a tip.
  // Skipped for the two flood tools above, for the reason the eyedropper's and
  // the gradient's early returns give: SIZE, HARD, LOAD and WET are read by no
  // code path a click on the wand or the bucket can reach, and a live control
  // over something the tool provably never reads is the same defect as a
  // palette cell for a tool that does not exist.
  if (flood == nullptr) {
    bandSeparator();
    capsLabel("SIZE");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    // The value inside the slider is a live numeric, so it is monospace: it
    // changes every frame of a drag, and a proportional face makes the track's
    // text jump about as the digits change width.
    pushAtelierMono();
    // kBrushRadiusMin/Max (app/AppState.hpp) -- the one range for this field,
    // also read by the BRUSH panel's Radius slider. See that constant's own
    // comment: this bar used to hardcode 2..90, a narrower range than the
    // panel's 1..200, so a value the panel set above 90 clamped back down the
    // moment this widget was touched (reachability audit B3).
    // `st.brush.radius` is gone (Part 5) -- read/written through
    // `model.tip.diameterPx` now, via a local half-diameter view of it since
    // `SliderFloat()` needs a `float*` it can write through directly every
    // frame.
    {
      float sizeRadius = st.brush.model.tip.diameterPx / 2.0f;
      ImGui::SliderFloat("##size", &sizeRadius, kBrushRadiusMin, kBrushRadiusMax, "%.0f px");
      st.brush.model.tip.diameterPx = sizeRadius * 2.0f;
    }
    popAtelierMono();

    bandSeparator();
    capsLabel("HARD");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    pushAtelierMono();
    // kBrushHardnessMin/Max (app/AppState.hpp) -- the one range for this
    // field, also read by the BRUSH panel's Hardness slider.
    ImGui::SliderFloat("##hard", &st.brush.model.tip.hardness, kBrushHardnessMin, kBrushHardnessMax,
                       "%.2f");
    popAtelierMono();

    bandSeparator();
    // kBrushLoadMin/Max (app/AppState.hpp) -- the same range drawBrushSection()
    // uses, not a second one invented here: one field behind two widgets with
    // two ranges is two clamps, and the narrower one silently truncates what
    // the other set.
    capsLabel("LOAD");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    pushAtelierMono();
    ImGui::SliderFloat("##load", &st.brush.load, kBrushLoadMin, kBrushLoadMax, "%.2f");
    popAtelierMono();

    bandSeparator();
    capsLabel("WET");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    pushAtelierMono();
    // **Same honest-refusal treatment MacPaintUI.cpp's drawBrushSection() gives
    // this same field** (see that comment for the full argument):
    // `st.brush.wetness` reaches `sim::PaintSim`'s `brushWater` only, through
    // `applyToolToBrush()`, and that is called only when `strokeRouteFor()`
    // answers `StrokeRoute::PaintSim` -- Water always, or Brush/DryBrush with
    // no document layer to aim at. A locally-scoped `route`, not the band's own
    // one three separators down: that one is computed after this slider and
    // reusing it here would mean drawing WET's disabled state off a value this
    // control has not reached yet on the very frame the tool or layer changes.
    {
      const OpenDocument* wetOd = st.documents.active();
      const Layer* wetTarget = wetOd != nullptr ? activeLayerOf(*wetOd) : nullptr;
      const bool wetHonoured = wetnessReachesSolver(strokeRouteFor(st.brush.tool, wetTarget));
      ImGui::BeginDisabled(!wetHonoured);
      // kBrushWetnessMin/Max (app/AppState.hpp) -- the one range for this
      // field, also read by the BRUSH panel's Water slider.
      ImGui::SliderFloat("##wet", &st.brush.wetness, kBrushWetnessMin, kBrushWetnessMax, "%.2f");
      ImGui::EndDisabled();
    }
    popAtelierMono();
  }

  // --- the smudge's own controls -------------------------------------------
  //
  // **In ADDITION to the four brush sliders above, not instead of them, and
  // that is a deliberate departure from every other per-tool block in this
  // function.** `docs/ui.md` §4b's test for taking an early return is not "does
  // this tool have settings" but *would the four brush sliders be live controls
  // over something this tool provably never reads*, and the smudge fails that
  // test three ways: `smudgeDab()` calls `dabCoverage()` (SIZE, HARD, and the
  // roundness/angle behind them) and multiplies every texel's weight by
  // `tip.flow` (LOAD). Only WET is dead here, and WET is already disabled by
  // `wetnessReachesSolver()` on its own. So this is the first tool in the band
  // to bring a row and keep the sliders -- the eyedropper, the measure and the
  // gradient have no tip at all, and the two flood tools walk a predicate
  // rather than a shape.
  //
  // The gate is `smudgeToolParamsFor()` -- `app/AppState.hpp`'s single mapping
  // from a tool to the block a stroke actually reads -- and not a
  // `tool == Smudge` spelled again here, for `floodToolParamsFor()`'s stated
  // reason one block up.
  if (SmudgeParams* smudge = smudgeToolParamsFor(st.brush, st.brush.tool)) {
    bandSeparator();
    // **STRENGTH, and it is a new field rather than the OPACITY slider it used
    // to be** -- `brush/Smudge.hpp` §3b. That sharing is what shipped the tool
    // at strength 1, the one value at which a smear provably never fades, with
    // its only control in the BRUSH panel under a name that means something
    // else on five other routes.
    capsLabel("STRENGTH");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    pushAtelierMono();
    ImGui::SliderFloat("##smudgeStrength", &smudge->strength, 0.0f, 1.0f, "%.2f");
    popAtelierMono();
    ImGui::SetItemTooltip(
        "How far the colour you picked up is carried before the canvas takes it back. Lower "
        "picks up more of what it crosses, so the smear fades sooner; 1 drags the colour "
        "under the pen at pen-down to the far end of the stroke with no fade at all, and 0 "
        "leaves the layer untouched byte for byte.");

    bandSeparator();
    // **TIP: the smudge's own dab, or the brush's.** The report's second half
    // ("picking a dab shape as well ... for smudge is likely ideal"), scoped to
    // the half that reuses machinery that exists: this offers `app/DabLibrary`'s
    // own entries, resolved through the same `resolve()` the BRUSH panel's
    // picker grid calls, and stores the same `(id, bitmap)` pair
    // `BrushState::dabId`/`tipBitmap` stores. **A full second brush -- its own
    // size, spacing, dynamics and preset -- is deliberately not here**; see
    // `brush/Smudge.hpp` §3b's last paragraph for why every field but the
    // bitmap would be a copy of a field the smudge already reads off the brush.
    capsLabel("TIP");
    ImGui::SameLine();
    // The label a cell shows, capped so the preview can never be clipped under
    // its own arrow. Dab names come from FILENAMES, so unlike every other combo
    // in this band their length is unbounded and measuring the true widest one
    // would let a folder full of long names push the trailing route indicator
    // out of the band. Truncating and then measuring over the TRUNCATED forms
    // is what keeps the measurement and the drawing the same string -- the
    // REACH combo's own history two blocks up is what happens when they are
    // not. The back-off loop is because a name is UTF-8 and a byte cut through
    // a continuation byte draws as a replacement glyph.
    constexpr size_t kTipLabelMax = 18;
    auto shortLabel = [](const std::string& s) -> std::string {
      if (s.size() <= kTipLabelMax) return s;
      size_t cut = kTipLabelMax - 1;
      while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
      return s.substr(0, cut) + "\xE2\x80\xA6";  // U+2026 HORIZONTAL ELLIPSIS
    };
    // "Brush's tip" rather than "None" or "Default": the empty state is not the
    // absence of a tip, it is the tip the brush is on, which is what this tool
    // has used for its whole history and what a user who never opens this combo
    // keeps getting.
    static const char* kBrushTipLabel = "Brush's tip";
    const DabEntry* chosen =
        smudge->dabId.empty() ? nullptr : st.dabLibrary.find(smudge->dabId);
    // A set id the library cannot resolve falls back to the brush's tip in
    // `brushTipFor()`, so it says so here too rather than showing a name for a
    // dab that is not going to paint.
    const std::string preview =
        chosen != nullptr ? shortLabel(chosen->name) : std::string(kBrushTipLabel);
    pushAtelierMono();
    float widest = ImGui::CalcTextSize(kBrushTipLabel).x;
    for (const DabEntry& e : st.dabLibrary.entries())
      widest = std::max(widest, ImGui::CalcTextSize(shortLabel(e.name).c_str()).x);
    ImGui::SetNextItemWidth(widest + ImGui::GetFrameHeight() + 16.0f);
    if (ImGui::BeginCombo("##smudgeTip", preview.c_str())) {
      if (ImGui::Selectable(kBrushTipLabel, smudge->dabId.empty())) {
        // Both halves cleared together, always -- `SmudgeParams`' own contract
        // and `BrushState::dabId`'s before it. An id cleared without its bitmap
        // leaves a tip that paints from a dab the UI says is not selected.
        smudge->dabId.clear();
        smudge->tipBitmap.reset();
      }
      for (const DabEntry& e : st.dabLibrary.entries()) {
        if (ImGui::Selectable(shortLabel(e.name).c_str(), e.id == smudge->dabId)) {
          smudge->dabId = e.id;
          // `resolve()` decodes on first use and caches -- the same call the
          // BRUSH panel's picker makes, so a dab chosen in either place costs
          // one decode between them rather than one each.
          smudge->tipBitmap = st.dabLibrary.resolve(e.id);
        }
        // The full name, since the row above may be the truncated one. The
        // tooltip is the only place the whole name is readable at all.
        ImGui::SetItemTooltip("%s", e.name.c_str());
      }
      ImGui::EndCombo();
    }
    popAtelierMono();
    ImGui::SetItemTooltip(
        "Which dab this tool drags. \"%s\" follows whatever tip the brush is on, which is what "
        "the smudge has always used; anything else overrides it for smudge strokes and for "
        "nothing else, so picking a smear shape does not repaint your next brush stroke with "
        "it. SIZE, HARD and LOAD above are still the brush's and are still live here.",
        kBrushTipLabel);
  }

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
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
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
    ImGui::SetItemTooltip("The active layer is \"%s\".\nA %s stroke on it goes to: %s",
                          target->name.c_str(), toolName(st.brush.tool), strokeRouteName(route));
  }
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
  // docs/testing-issues.md T6, and the one place a user will actually look
  // for the answer. This readout is honest -- it is the real `resident_size`
  // against the design's 512 MB -- but Activity Monitor's "Memory" column is
  // `phys_footprint`, a different quantity, and on this application it reads
  // about four times higher. A user who sees "145 MB / 512 MB" here and
  // "551 MB" there has no way to reconcile them, and re-reports it; that is
  // literally how T6 was filed.
  //
  // A tooltip rather than a longer label, deliberately: the band is dense,
  // the visible glyphs stay exactly as `docs/ui.md` section 2 draws them, and
  // both numbers are computed here rather than one of them being narrated
  // from a comment that could go stale.
  ImGui::SetItemTooltip(
      "Resident memory: %s of a %s budget.\n\n"
      "Activity Monitor will show a larger number (%s right now). It reports "
      "phys_footprint, which also charges this process for memory the graphics "
      "driver maps in on its behalf -- around 400 MB of it, allocated once when "
      "the GPU starts work and independent of window size, document size and "
      "whether the fluid solver exists.\n\n"
      "This meter tracks the memory naturalPaint itself holds, which is the part "
      "the budget is about.",
      formatMiB(mem.bytes).c_str(), formatMiB(mem.budget).c_str(),
      formatMiB(currentFootprintBytes()).c_str());

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
