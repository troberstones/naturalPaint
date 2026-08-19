#include "ui/MacPaintUI.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "app/Snapping.hpp"
#include "app/ViewTransform.hpp"
#include "imgui.h"

namespace np {
namespace {

constexpr float kToolCol = 2.0f;
constexpr float kToolSize = 30.0f;
constexpr float kPaletteW = kToolCol * kToolSize + 18.0f;
constexpr float kControlsW = 268.0f;
constexpr float kSwatchStripH = 62.0f;
// Peak gravity, in the same cells-per-step units as the rest of the velocity field.
constexpr float kMaxTilt = 0.50f;
// PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD Q5-Q7).
// Screen-space px, not document-space -- the ruler strip's on-screen size is
// a layout constant like kSwatchStripH above, and the snap radius is
// defined to *feel* constant on screen regardless of zoom (converted to a
// document-space threshold per-frame; see the canvas block below).
constexpr float kRulerThickness = 20.0f;
constexpr float kSnapThresholdPx = 8.0f;

const char* toolName(Tool t) {
  switch (t) {
    case Tool::Brush:      return "Brush";
    case Tool::Water:      return "Water";
    case Tool::DryBrush:   return "Dry Brush";
    case Tool::Eyedropper: return "Eyedropper";
    case Tool::Hand:       return "Hand";
    case Tool::Zoom:       return "Zoom";
    default:               return "?";
  }
}

// Icons are drawn rather than loaded so there is no asset to ship and they stay
// crisp at any DPI. Chunky strokes, no anti-aliased flourishes — the originals
// were 1-bit and read better for it.
void drawToolIcon(ImDrawList* dl, Tool t, ImVec2 c, float s, ImU32 col) {
  const float th = std::max(1.5f, s * 0.11f);
  switch (t) {
    case Tool::Brush: {
      dl->AddLine(ImVec2(c.x + s * 0.30f, c.y - s * 0.42f),
                  ImVec2(c.x - s * 0.06f, c.y + s * 0.06f), col, th);
      dl->AddCircleFilled(ImVec2(c.x - s * 0.20f, c.y + s * 0.22f), s * 0.24f, col, 16);
      break;
    }
    case Tool::Water: {
      // Teardrop: apex up, round belly.
      ImVec2 pts[3] = {ImVec2(c.x, c.y - s * 0.46f),
                       ImVec2(c.x + s * 0.34f, c.y + s * 0.12f),
                       ImVec2(c.x - s * 0.34f, c.y + s * 0.12f)};
      dl->AddTriangleFilled(pts[0], pts[1], pts[2], col);
      dl->AddCircleFilled(ImVec2(c.x, c.y + s * 0.12f), s * 0.34f, col, 20);
      break;
    }
    case Tool::DryBrush: {
      for (int i = -2; i <= 2; ++i) {
        const float x = c.x + i * s * 0.16f;
        dl->AddLine(ImVec2(x, c.y - s * 0.34f),
                    ImVec2(x + s * 0.06f, c.y + s * 0.38f), col, th * 0.8f);
      }
      break;
    }
    case Tool::Eyedropper: {
      dl->AddLine(ImVec2(c.x + s * 0.32f, c.y - s * 0.36f),
                  ImVec2(c.x - s * 0.12f, c.y + s * 0.10f), col, th * 1.6f);
      ImVec2 tip[3] = {ImVec2(c.x - s * 0.34f, c.y + s * 0.40f),
                       ImVec2(c.x - s * 0.24f, c.y - s * 0.02f),
                       ImVec2(c.x + s * 0.02f, c.y + s * 0.22f)};
      dl->AddTriangleFilled(tip[0], tip[1], tip[2], col);
      break;
    }
    case Tool::Hand: {
      dl->AddRectFilled(ImVec2(c.x - s * 0.26f, c.y - s * 0.06f),
                        ImVec2(c.x + s * 0.26f, c.y + s * 0.40f), col);
      for (int i = 0; i < 3; ++i) {
        const float x = c.x - s * 0.24f + i * s * 0.20f;
        dl->AddRectFilled(ImVec2(x, c.y - s * 0.40f),
                          ImVec2(x + s * 0.13f, c.y), col);
      }
      break;
    }
    case Tool::Zoom: {
      dl->AddCircle(ImVec2(c.x - s * 0.08f, c.y - s * 0.10f), s * 0.28f, col, 20, th);
      dl->AddLine(ImVec2(c.x + s * 0.10f, c.y + s * 0.10f),
                  ImVec2(c.x + s * 0.38f, c.y + s * 0.38f), col, th * 1.4f);
      break;
    }
    default: break;
  }
}

bool toolButton(AppState& st, Tool t) {
  ImGui::PushID(static_cast<int>(t));
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 size(kToolSize, kToolSize);
  const bool clicked = ImGui::InvisibleButton("##tool", size);
  const bool selected = st.brush.tool == t;
  const bool hovered = ImGui::IsItemHovered();

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 bg = selected ? ImGui::GetColorU32(ImGuiCol_ButtonActive)
                            : (hovered ? ImGui::GetColorU32(ImGuiCol_ButtonHovered)
                                       : ImGui::GetColorU32(ImGuiCol_Button));
  dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), bg);
  dl->AddRect(p, ImVec2(p.x + size.x, p.y + size.y),
              ImGui::GetColorU32(ImGuiCol_Border));

  // Selected tools invert, the way MacPaint's did.
  const ImU32 fg = selected ? IM_COL32(20, 22, 24, 255)
                            : ImGui::GetColorU32(ImGuiCol_Text);
  drawToolIcon(dl, t, ImVec2(p.x + size.x * 0.5f, p.y + size.y * 0.5f),
               size.x * 0.62f, fg);

  if (hovered) ImGui::SetTooltip("%s", toolName(t));
  if (clicked) st.brush.tool = t;
  ImGui::PopID();
  return clicked;
}

// Tilt is a direction and a steepness, so it gets a pad you drag rather than two
// numbers. The dot is where the low corner of the board is; distance from centre
// is how steep. Matches screen orientation: drag down, washes run down.
bool tiltPad(AppState& st, float size) {
  ImGui::PushID("tilt");
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
  const float r = size * 0.5f - 4.0f;

  ImGui::InvisibleButton("##pad", ImVec2(size, size));
  const bool active = ImGui::IsItemActive();
  bool changed = false;

  if (active) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    float dx = (m.x - c.x) / r;
    float dy = (m.y - c.y) / r;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len > 1.0f) { dx /= len; dy /= len; }
    st.sim.tiltX = dx * kMaxTilt;
    st.sim.tiltY = dy * kMaxTilt;
    changed = true;
  }
  // Double-click lays the board flat again.
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    st.sim.tiltX = 0.0f;
    st.sim.tiltY = 0.0f;
    changed = true;
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size),
                    ImGui::GetColorU32(ImGuiCol_ChildBg));
  dl->AddRect(p, ImVec2(p.x + size, p.y + size),
              ImGui::GetColorU32(ImGuiCol_Border));
  const ImU32 grid = ImGui::GetColorU32(ImGuiCol_Border);
  dl->AddLine(ImVec2(c.x, p.y + 3), ImVec2(c.x, p.y + size - 3), grid);
  dl->AddLine(ImVec2(p.x + 3, c.y), ImVec2(p.x + size - 3, c.y), grid);
  dl->AddCircle(c, r, grid, 32);

  const float tx = st.sim.tiltX / kMaxTilt;
  const float ty = st.sim.tiltY / kMaxTilt;
  const ImVec2 knob(c.x + tx * r, c.y + ty * r);
  if (tx != 0.0f || ty != 0.0f) {
    dl->AddLine(c, knob, IM_COL32(80, 165, 185, 255), 2.0f);
  }
  dl->AddCircleFilled(knob, 5.0f, IM_COL32(235, 235, 225, 255), 16);

  ImGui::PopID();
  return changed;
}

void applyToolToBrush(AppState& st) {
  switch (st.brush.tool) {
    case Tool::Water:
      st.sim.brushPigment = 0.0f;
      st.sim.brushWater = st.brush.wetness * 1.4f;
      st.sim.brushHardness = 0.15f;
      break;
    case Tool::DryBrush:
      st.sim.brushPigment = st.brush.load * 1.3f;
      st.sim.brushWater = st.brush.wetness * 0.15f;
      st.sim.brushHardness = 0.9f;
      break;
    case Tool::Brush:
    default:
      st.sim.brushPigment = st.brush.load;
      st.sim.brushWater = st.brush.wetness;
      st.sim.brushHardness = st.brush.hardness;
      break;
  }
}

// PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD
// Q5-Q7). ------------------------------------------------------------

// A "nice" ruler step (the 1-2-5-10-20-50-100... progression every ruler --
// Photoshop, Illustrator, browser devtools -- uses) such that consecutive
// major ticks land at least ~50 screen px apart, so labels stay readable at
// any zoom instead of crowding together.
float rulerStep(float zoom) {
  static constexpr float kSteps[] = {1,    2,    5,    10,   20,    25,   50,    100,
                                     200,  250,  500,  1000, 2000,  5000, 10000, 20000};
  for (float s : kSteps)
    if (s * zoom >= 50.0f) return s;
  return kSteps[(sizeof(kSteps) / sizeof(kSteps[0])) - 1];
}

// Thin strips along the top and left of the canvas window with document-
// space tick marks and coordinate labels, reusing the exact same
// ViewTransform every other document-space thing on the canvas goes
// through -- not a second, independently derived screen<->document mapping.
//
// Only meaningful at view.rotation == 0: a ruler measures a single
// screen-aligned document axis, which stops being a coherent idea once that
// axis isn't screen-aligned any more. PLAN.md step 12 explicitly allows a
// simple fallback here rather than solving rotated-ruler UX -- this draws a
// plain, dimmed, tickless band at any non-zero rotation instead of ticks
// that would no longer correspond to anything.
void drawRulers(ImDrawList* dl, const ViewTransform& xform, const CanvasView& view,
                ImVec2 canvasPos, ImVec2 paintOrigin, ImVec2 avail, float thickness) {
  const ImU32 bg = ImGui::GetColorU32(ImGuiCol_ChildBg);

  const ImVec2 topMin(paintOrigin.x, canvasPos.y);
  const ImVec2 topMax(paintOrigin.x + avail.x, canvasPos.y + thickness);
  const ImVec2 leftMin(canvasPos.x, paintOrigin.y);
  const ImVec2 leftMax(canvasPos.x + thickness, paintOrigin.y + avail.y);

  dl->AddRectFilled(canvasPos, topMax, bg);  // corner + top, one rect
  dl->AddRectFilled(leftMin, leftMax, bg);

  constexpr float kRotationEps = 1e-4f;
  if (std::fabs(view.rotation) > kRotationEps) {
    const ImU32 dim = IM_COL32(0, 0, 0, 70);
    dl->AddRectFilled(topMin, topMax, dim);
    dl->AddRectFilled(leftMin, leftMax, dim);
    return;
  }

  const ImU32 tick = ImGui::GetColorU32(ImGuiCol_Border);
  const ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);

  // Document-space extent currently visible along each ruler, found by
  // inverting the strip's own screen-space endpoints through toCanvas() --
  // min/max rather than a direct left->right read, since a mirrored view
  // can reverse which screen edge maps to the smaller document coordinate.
  // At rotation == 0 the transform is diagonal (see ViewTransform.cpp's
  // algebra), so the y/x coordinate fed in below for the "other" axis is
  // arbitrary -- it cancels out.
  const float cx0 = xform.toCanvas(Vec2{topMin.x, topMin.y}).x;
  const float cx1 = xform.toCanvas(Vec2{topMax.x, topMin.y}).x;
  const float cy0 = xform.toCanvas(Vec2{leftMin.x, leftMin.y}).y;
  const float cy1 = xform.toCanvas(Vec2{leftMin.x, leftMax.y}).y;
  const float xMin = std::min(cx0, cx1), xMax = std::max(cx0, cx1);
  const float yMin = std::min(cy0, cy1), yMax = std::max(cy0, cy1);

  const float step = rulerStep(view.zoom);
  char label[32];

  for (float x = std::floor(xMin / step) * step; x <= xMax + step; x += step) {
    const Vec2 s = xform.toScreen(Vec2{x, 0.0f});
    if (s.x < topMin.x - 1.0f || s.x > topMax.x + 1.0f) continue;
    dl->AddLine(ImVec2(s.x, topMax.y - thickness * 0.6f), ImVec2(s.x, topMax.y), tick, 1.0f);
    std::snprintf(label, sizeof(label), "%.0f", x);
    dl->AddText(ImVec2(s.x + 2.0f, topMin.y + 2.0f), text, label);
  }
  for (float y = std::floor(yMin / step) * step; y <= yMax + step; y += step) {
    const Vec2 s = xform.toScreen(Vec2{0.0f, y});
    if (s.y < leftMin.y - 1.0f || s.y > leftMax.y + 1.0f) continue;
    dl->AddLine(ImVec2(leftMax.x - thickness * 0.6f, s.y), ImVec2(leftMax.x, s.y), tick, 1.0f);
    std::snprintf(label, sizeof(label), "%.0f", y);
    dl->AddText(ImVec2(leftMin.x + 2.0f, s.y + 2.0f), text, label);
  }
}

// PRD Q7: grid overlay, spacing/subdivisions from AppState's own sliders
// (this file's ##controls panel). Lines are found the same way rulers place
// ticks -- document-space positions from gridLinePositions(), mapped to
// screen through the same ViewTransform -- so the overlay tracks pan, zoom,
// mirror and rotation exactly like the canvas image itself.
void drawGridOverlay(ImDrawList* dl, const ViewTransform& xform, float texW, float texH,
                     float spacing, int subdivisions, float zoom) {
  if (!(spacing > 0.0f) || subdivisions < 1) return;
  const float minor = spacing / static_cast<float>(std::max(1, subdivisions));
  if (!(minor > 0.0f)) return;
  // Too dense to read (or draw efficiently) at the current zoom -- fall
  // back to major-only lines rather than flooding the canvas with
  // sub-pixel-spaced minor lines.
  const bool minorVisible = minor * zoom >= 4.0f;
  const auto xs = gridLinePositions(spacing, minorVisible ? subdivisions : 1, 0.0f, texW);
  const auto ys = gridLinePositions(spacing, minorVisible ? subdivisions : 1, 0.0f, texH);
  const ImU32 minorCol = IM_COL32(255, 255, 255, 22);
  const ImU32 majorCol = IM_COL32(255, 255, 255, 55);

  for (float x : xs) {
    const ImU32 col = isMajorGridLine(x, spacing) ? majorCol : minorCol;
    const Vec2 a = xform.toScreen(Vec2{x, 0.0f});
    const Vec2 b = xform.toScreen(Vec2{x, texH});
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), col, 1.0f);
  }
  for (float y : ys) {
    const ImU32 col = isMajorGridLine(y, spacing) ? majorCol : minorCol;
    const Vec2 a = xform.toScreen(Vec2{0.0f, y});
    const Vec2 b = xform.toScreen(Vec2{texW, y});
    dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), col, 1.0f);
  }
}

// One guide, drawn across the full canvas extent (0..texW or 0..texH) --
// not an unbounded "infinite" line -- through the same ViewTransform as
// everything else. Bounding it to the canvas extent is a deliberate
// simplification: this app has no pasteboard-beyond-the-canvas concept for
// a line to usefully extend into.
void drawGuideLine(ImDrawList* dl, const ViewTransform& xform, GuideOrientation orientation,
                   float position, float texW, float texH, ImU32 col) {
  Vec2 a, b;
  if (orientation == GuideOrientation::Horizontal) {
    a = xform.toScreen(Vec2{0.0f, position});
    b = xform.toScreen(Vec2{texW, position});
  } else {
    a = xform.toScreen(Vec2{position, 0.0f});
    b = xform.toScreen(Vec2{position, texH});
  }
  dl->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), col, 1.0f);
}

// Screen-space point-to-segment distance, for right-click-to-delete guide
// picking below -- a guide can be dragged/rotated on screen (mirror,
// rotation), so "distance to the line's on-screen endpoints" is the only
// correct hit test, not a document-space one.
float distancePointToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
  const ImVec2 ab(b.x - a.x, b.y - a.y);
  const float lenSq = ab.x * ab.x + ab.y * ab.y;
  float t = lenSq > 1e-6f ? ((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / lenSq : 0.0f;
  t = std::clamp(t, 0.0f, 1.0f);
  const ImVec2 proj(a.x + ab.x * t, a.y + ab.y * t);
  const float dx = p.x - proj.x, dy = p.y - proj.y;
  return std::sqrt(dx * dx + dy * dy);
}

}  // namespace

void drawUI(AppState& st, std::unique_ptr<PaintSim>& sim, GpuContext& gpu,
           const MixboxLut& lut, uint32_t canvasW, uint32_t canvasH) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();

  // ------------------------------------------------------------ menu bar
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("New", "Cmd+N")) st.requestClear = true;
      ImGui::Separator();
      if (ImGui::MenuItem("Quit", "Cmd+Q")) st.quit = true;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
      if (ImGui::MenuItem("Clear Canvas", "Cmd+K")) st.requestClear = true;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Medium")) {
      for (int i = 0; i < static_cast<int>(PaintMode::Count); ++i) {
        const PaintMode m = static_cast<PaintMode>(i);
        if (ImGui::MenuItem(paintModeName(m), nullptr, st.mode == m) && st.mode != m) {
          st.mode = m;
          st.requestMode = true;
        }
      }
      ImGui::Separator();
      ImGui::TextDisabled("switching clears the canvas");
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Goodies")) {
      for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
        const Tool t = static_cast<Tool>(i);
        if (ImGui::MenuItem(toolName(t), nullptr, st.brush.tool == t))
          st.brush.tool = t;
      }
      ImGui::Separator();
      ImGui::MenuItem("Pause solver", "Space", &st.paused);
      if (ImGui::MenuItem("Reload shaders", "Cmd+R")) st.requestReload = true;
      ImGui::EndMenu();
    }
    // PLAN.md Phase 2 step 11 ("View controls", PRD Q1-Q4): menu-item mirror
    // of the same keymap actions main.cpp's SDL_EVENT_KEY_DOWN dispatch
    // resolves (keymaps/default.json) -- both paths end up setting the same
    // AppState fields, so there is exactly one place ("view" state consumed
    // in this file's canvas block below) that actually acts on any of them.
    if (ImGui::BeginMenu("View")) {
      if (ImGui::MenuItem("Fit to Window", "Cmd+0")) st.requestFitWindow = true;
      if (ImGui::MenuItem("100%", "Cmd+1")) st.requestZoom100 = true;
      if (ImGui::MenuItem("Zoom In", "Cmd+=")) st.requestZoomIn = true;
      if (ImGui::MenuItem("Zoom Out", "Cmd+-")) st.requestZoomOut = true;
      ImGui::Separator();
      ImGui::MenuItem("Mirror Left/Right", "F", &st.view.mirrorX);
      ImGui::MenuItem("Mirror Up/Down", "Shift+F", &st.view.mirrorY);
      if (ImGui::MenuItem("Reset Rotation", "Shift+R")) st.view.rotation = 0.0f;
      ImGui::Separator();
      ImGui::MenuItem("Grayscale Preview", "Cmd+Y", &st.view.grayscale);
      // PLAN.md Phase 3 step 6 debug scaffolding -- temporary, explicitly
      // NOT step 8's real op-authoring UI. See main.cpp's AppState-
      // construction comment for the full rationale: this is the only
      // way to exercise the Apply pass in the running app before a real
      // reorder/toggle/delete/curve widget exists. Flips both of the two
      // fixed ops main.cpp seeds into st.opStack (indices 0 and 1)
      // together, matching however this checkbox itself just toggled.
      if (ImGui::MenuItem("Test Grade (debug)", nullptr, &st.view.grade)) {
        st.opStack.setEnabled(0, st.view.grade);
        st.opStack.setEnabled(1, st.view.grade);
      }
      ImGui::Separator();
      // PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD
      // Q5-Q7). Rulers is deliberately the one item here with no shortcut
      // string -- docs/shortcuts.md assigns it Cmd+R, but that chord is
      // already bound to reload_shaders (see main.cpp's dispatch comment
      // for the full reasoning). Menu-only until that spec conflict is
      // resolved by a product decision, not by this step picking a
      // different key on its own initiative.
      ImGui::MenuItem("Rulers", nullptr, &st.showRulers);
      ImGui::MenuItem("Guides", "Cmd+;", &st.showGuides);
      if (ImGui::MenuItem("Add Guide...")) ImGui::OpenPopup("AddGuidePopup");
      if (ImGui::MenuItem("Clear Guides", nullptr, false, !st.guides.empty()))
        st.guides.clear();
      ImGui::MenuItem("Grid", "Cmd+'", &st.showGrid);
      ImGui::MenuItem("Snap", "Cmd+Shift+;", &st.snappingEnabled);
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Window")) {
      ImGui::MenuItem("ImGui demo", nullptr, &st.showDemo);
      ImGui::EndMenu();
    }

    // Right-aligned status, the way the classic apps put the zoom box.
    char status[128];
    std::snprintf(status, sizeof(status), "%s   %.1f fps   %ux%u   %.0f%%",
                  paintModeName(st.mode),
                  st.frameMs > 0.0f ? 1000.0f / st.frameMs : 0.0f, canvasW,
                  canvasH, st.view.zoom * 100.0f);
    const float w = ImGui::CalcTextSize(status).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - w - 12.0f);
    ImGui::TextDisabled("%s", status);
    ImGui::EndMainMenuBar();
  }

  // PLAN.md Phase 2 step 12, PRD Q5: "guide at a numeric or percentage
  // position" via a small popup opened from the View menu's "Add Guide..."
  // item above (ImGui::OpenPopup("AddGuidePopup")) -- doesn't need to be
  // elaborate, just a position field and an orientation choice. Defined
  // here, outside BeginMainMenuBar/EndMainMenuBar (popups are identified by
  // a global string ID, not nested inside the ID stack of whatever widget
  // opened them), so it renders regardless of which menu is currently open.
  if (ImGui::BeginPopup("AddGuidePopup")) {
    static int orientationIdx = 0;  // 0 = Horizontal, 1 = Vertical
    static char posBuf[32] = "50%";
    ImGui::TextUnformatted("Add Guide");
    ImGui::Separator();
    ImGui::RadioButton("Horizontal", &orientationIdx, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Vertical", &orientationIdx, 1);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText("Position", posBuf, sizeof(posBuf));
    ImGui::TextDisabled("e.g. 512 or 50%%");
    const bool horizontal = orientationIdx == 0;
    // A Horizontal guide's position is along canvasH (it sits at a fixed Y);
    // a Vertical guide's is along canvasW (a fixed X) -- same axis pairing
    // app/Snapping.hpp's parseGuidePosition() doc comment spells out.
    const float axisExtent = horizontal ? static_cast<float>(canvasH) : static_cast<float>(canvasW);
    const auto parsed = parseGuidePosition(posBuf, axisExtent);
    if (ImGui::Button("Add") && parsed) {
      st.guides.push_back(
          Guide{horizontal ? GuideOrientation::Horizontal : GuideOrientation::Vertical, *parsed});
      ImGui::CloseCurrentPopup();
    }
    if (!parsed) {
      ImGui::SameLine();
      ImGui::TextDisabled("(enter a number or a percentage)");
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  const ImVec2 work = vp->WorkPos;
  const ImVec2 size = vp->WorkSize;

  const ImGuiWindowFlags fixedFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;

  // ------------------------------------------------------------ tool palette
  ImGui::SetNextWindowPos(work);
  ImGui::SetNextWindowSize(ImVec2(kPaletteW, size.y - kSwatchStripH));
  if (ImGui::Begin("##tools", nullptr, fixedFlags)) {
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      if (i % 2 != 0) ImGui::SameLine();
      toolButton(st, static_cast<Tool>(i));
    }
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 6));

    // Brush size preview, MacPaint's line-width box reinterpreted.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float boxW = kToolCol * kToolSize + 4.0f;
    const float boxH = 54.0f;
    dl->AddRectFilled(p, ImVec2(p.x + boxW, p.y + boxH),
                      ImGui::GetColorU32(ImGuiCol_ChildBg));
    dl->AddRect(p, ImVec2(p.x + boxW, p.y + boxH),
                ImGui::GetColorU32(ImGuiCol_Border));
    const auto& pig = defaultPalette()[st.brush.pigment];
    dl->AddCircleFilled(ImVec2(p.x + boxW * 0.5f, p.y + boxH * 0.5f),
                        std::min(st.brush.radius * 0.5f, boxH * 0.4f),
                        IM_COL32((int)(pig.rgb[0] * 255), (int)(pig.rgb[1] * 255),
                                 (int)(pig.rgb[2] * 255), 255),
                        24);
    ImGui::Dummy(ImVec2(boxW, boxH));
    ImGui::SetNextItemWidth(boxW);
    ImGui::SliderFloat("##size", &st.brush.radius, 2.0f, 90.0f, "%.0f px");
  }
  ImGui::End();

  // ------------------------------------------------------------ pigment strip
  ImGui::SetNextWindowPos(ImVec2(work.x, work.y + size.y - kSwatchStripH));
  ImGui::SetNextWindowSize(ImVec2(size.x, kSwatchStripH));
  if (ImGui::Begin("##pigments", nullptr, fixedFlags)) {
    const auto& palette = defaultPalette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float sw = 30.0f;
    for (size_t i = 0; i < palette.size(); ++i) {
      if (i) ImGui::SameLine(0.0f, 3.0f);
      ImGui::PushID(static_cast<int>(i));
      const ImVec2 p = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton("##sw", ImVec2(sw, sw)))
        st.brush.pigment = static_cast<int>(i);

      const auto& pg = palette[i];
      dl->AddRectFilled(p, ImVec2(p.x + sw, p.y + sw),
                        IM_COL32((int)(pg.rgb[0] * 255), (int)(pg.rgb[1] * 255),
                                 (int)(pg.rgb[2] * 255), 255));
      const bool sel = st.brush.pigment == static_cast<int>(i);
      dl->AddRect(p, ImVec2(p.x + sw, p.y + sw),
                  sel ? IM_COL32(235, 235, 225, 255)
                      : ImGui::GetColorU32(ImGuiCol_Border),
                  0.0f, 0, sel ? 2.0f : 1.0f);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", pg.name);
      ImGui::PopID();
    }
    ImGui::TextDisabled("%s", palette[st.brush.pigment].name);
  }
  ImGui::End();

  // ------------------------------------------------------------ solver panel
  ImGui::SetNextWindowPos(ImVec2(work.x + size.x - kControlsW, work.y));
  ImGui::SetNextWindowSize(ImVec2(kControlsW, size.y - kSwatchStripH));
  if (ImGui::Begin("##controls", nullptr,
                   fixedFlags & ~ImGuiWindowFlags_NoScrollbar)) {
    ImGui::TextUnformatted("BRUSH");
    ImGui::Separator();
    ImGui::SliderFloat("Load", &st.brush.load, 0.0f, 2.5f);
    ImGui::SliderFloat("Water", &st.brush.wetness, 0.0f, 3.0f);
    ImGui::SliderFloat("Hardness", &st.brush.hardness, 0.0f, 1.0f);
    ImGui::Checkbox("Pressure -> size", &st.brush.pressureSize);
    ImGui::Checkbox("Pressure -> flow", &st.brush.pressureFlow);
    if (!st.penSeen) ImGui::TextDisabled("(no tablet detected)");

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextUnformatted("PIGMENT");
    ImGui::Separator();
    ImGui::SliderFloat("Density", &st.sim.density, 0.0f, 1.0f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("How fast pigment drops out of suspension.");
    ImGui::SliderFloat("Staining", &st.sim.staining, 0.02f, 1.0f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Resistance to being lifted back into the water.");
    ImGui::SliderFloat("Granulation", &st.sim.granulation, 0.0f, 1.0f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Affinity for the paper's valleys.");
    ImGui::SliderFloat("Diffusion", &st.sim.pigmentDiffuse, 0.0f, 1.0f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Pigment spreading through the wet film.\n"
                        "At zero the water outruns the pigment and the\n"
                        "leading edge of a wash runs clear.");

    ImGui::Dummy(ImVec2(0, 8));
    if (st.mode == PaintMode::Oil) {
      ImGui::TextUnformatted("OIL");
      ImGui::Separator();
      ImGui::SliderFloat("Brush load", &st.sim.brushLoad, 0.0f, 3.0f);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Paint the brush picks up when a stroke starts.\n"
                          "It runs out as you paint, like a real one.");
      ImGui::SliderFloat("Pressure", &st.sim.penetration, 0.05f, 2.0f);
      ImGui::SliderFloat("Squish", &st.sim.oilPressure, 0.0f, 4.0f);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("vp = -c * grad(penetration): paint pushed out\n"
                          "sideways from under the bristles.");
      ImGui::SliderFloat("Transfer", &st.sim.xferFraction, 0.0f, 0.5f);
      ImGui::SliderFloat("Max transfer", &st.sim.maxXfer, 0.0f, 0.1f, "%.4f");
      ImGui::SliderFloat("Levelling", &st.sim.viscosity, 0.0f, 0.25f);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Wet paint relaxing under surface tension.\n"
                          "At zero the brush stamps leave periodic ridges.");
      ImGui::SliderFloat("Impasto light", &st.sim.impastoLight, 0.0f, 1.5f);
      ImGui::SliderFloat("Adhesion", &st.sim.adhesion, 0.0f, 0.4f);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Paint never fully leaves a cell. At zero the\n"
                          "canvas feels like Teflon.");
    } else if (st.mode == PaintMode::Ink) {
      ImGui::TextUnformatted("INK");
      ImGui::Separator();
      ImGui::SliderFloat("Relaxation", &st.sim.omega, 0.5f, 1.95f);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("LBE omega. Viscosity = (1/omega - 1/2)/3.");
      ImGui::SliderFloat("Blocking", &st.sim.blocking, 0.0f, 0.9f);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Base permeability of the paper. Higher blocks\n"
                          "more flow and pins the mark's edge.");
      ImGui::SliderFloat("Grain block", &st.sim.grainBlock, 0.0f, 0.9f);
      ImGui::SliderFloat("Glue", &st.sim.glue, 0.0f, 0.6f);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Artists add glue to limit spread.");
      ImGui::SliderFloat("Receptivity", &st.sim.receptivity, 0.1f, 2.5f);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Wet paper takes less ink. Lower this and a second\n"
                          "stroke over a damp mark barely registers.");
      ImGui::SliderFloat("Settle rate", &st.sim.settleScale, 0.0f, 0.05f, "%.4f");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How fast ink fixes to the fibres. Too high and it\n"
                          "deposits before it can travel, so nothing bleeds.");
      if (sim) ImGui::SliderInt("Lattice steps", &sim->inkSubsteps, 1, 20);
      ImGui::SliderFloat("Evaporation", &st.sim.evaporation, 0.0f, 0.03f, "%.4f");
    } else {
    ImGui::TextUnformatted("WATER");
    ImGui::Separator();
    ImGui::SliderFloat("Viscosity", &st.sim.viscosity, 0.0f, 0.5f);
    ImGui::SliderFloat("Drag", &st.sim.drag, 0.0f, 1.5f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Above ~0.5 the velocity field dies before water moves.");
    ImGui::SliderFloat("Edge darkening", &st.sim.edgeDarkening, 0.0f, 2.0f, "%.3f");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Curtis FlowOutward: pulls pigment to the stroke rim.\n"
                        "Zero this and washes go flat.");
    ImGui::SliderFloat("Paper slope", &st.sim.paperSlope, 0.0f, 4.0f);

    // One control for the wet lifetime. Evaporation and absorption are derived
    // from it rather than exposed separately: letting the two drift out of step
    // only makes the timing unpredictable, and neither means much alone.
    if (ImGui::SliderFloat("Working time", &st.workingTime, 1.0f, 20.0f, "%.1f s"))
      setWorkingTime(st.sim, st.workingTime);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("How long a wash keeps bleeding before it sets.\n"
                        "Good to about 15%% across this range. Past ~20 s the\n"
                        "wash spreads thin enough that capillary dilution ends\n"
                        "it regardless of how slowly it dries.");

    ImGui::SliderFloat("Max film", &st.sim.maxFilm, 0.2f, 8.0f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Deepest water the paper holds before it runs.\n"
                        "Raise it far and a wash empties into its own rim.");
    ImGui::SliderFloat("Capillary diffuse", &st.sim.diffuseRate, 0.0f, 1.0f);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("How fast water wicks through the fibres.\n"
                        "Sets how far a wash reaches, not how long it lasts.");

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextUnformatted("BOARD TILT");
    ImGui::Separator();
    tiltPad(st, 96.0f);
    ImGui::SameLine();
    ImGui::BeginGroup();
    const float steep = std::sqrt(st.sim.tiltX * st.sim.tiltX +
                                  st.sim.tiltY * st.sim.tiltY) / kMaxTilt;
    ImGui::TextDisabled("%.0f%%", steep * 100.0f);
    ImGui::TextDisabled("drag to");
    ImGui::TextDisabled("tilt");
    ImGui::Dummy(ImVec2(0, 4));
    if (ImGui::SmallButton("Level")) { st.sim.tiltX = 0.0f; st.sim.tiltY = 0.0f; }
    ImGui::EndGroup();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Only wet paint runs — the force scales with film\n"
                        "depth, so a puddle streaks and damp paper does not.\n"
                        "Double-click the pad to level.");
    }

    // PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD Q7):
    // "a simple settings surface... doesn't need its own dedicated window" --
    // a couple of sliders here, alongside the rest of the view/sim controls,
    // is sufficient. Mirrors the View menu's Grid/Snap checkboxes (same
    // AppState fields; either path sets the one place that's actually read,
    // this file's canvas block below).
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextUnformatted("GRID");
    ImGui::Separator();
    ImGui::Checkbox("Show grid", &st.showGrid);
    ImGui::SliderFloat("Spacing", &st.gridSpacing, 4.0f, 512.0f, "%.0f px");
    ImGui::SliderInt("Subdivisions", &st.gridSubdivisions, 1, 10);
    ImGui::Checkbox("Snap", &st.snappingEnabled);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Snaps guide creation/dragging to guides, the grid\n"
                        "and canvas edges. Never affects freehand painting.");

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextUnformatted("SOLVER");
    ImGui::Separator();
    if (sim) {
      if (st.mode == PaintMode::Watercolor)
        ImGui::SliderInt("Jacobi iters", &sim->jacobiIterations, 1, 60);
      ImGui::SliderInt("Substeps", &sim->substeps, 1, 6);
    }
    ImGui::Checkbox("Paused", &st.paused);
    if (ImGui::Button("Clear canvas")) st.requestClear = true;
    ImGui::SameLine();
    if (ImGui::Button("Reload shaders")) st.requestReload = true;
  }
  ImGui::End();

  // ------------------------------------------------------------ canvas
  const ImVec2 canvasPos(work.x + kPaletteW, work.y);
  const ImVec2 canvasSize(size.x - kPaletteW - kControlsW, size.y - kSwatchStripH);
  ImGui::SetNextWindowPos(canvasPos);
  ImGui::SetNextWindowSize(canvasSize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  if (ImGui::Begin("##canvas", nullptr, fixedFlags)) {
    const ImVec2 fullAvail = ImGui::GetContentRegionAvail();
    const float texW = static_cast<float>(canvasW);
    const float texH = static_cast<float>(canvasH);

    // PLAN.md Phase 2 step 12: rulers reserve a thin strip along the top and
    // left of this window when shown, so the paintable area shrinks by
    // exactly that much. `avail`/`paintOrigin` below replace the old
    // `avail`/`canvasPos` everywhere else in this block that computes
    // paint-area geometry (fit-to-window, drawSize/origin, the zoom anchor,
    // the paint hit-test rect) -- at st.showRulers == false (the default),
    // rulerThickness == 0 so paintOrigin == canvasPos and avail ==
    // fullAvail == canvasSize exactly, meaning this step changes nothing
    // about the existing pan/zoom/paint arithmetic unless rulers are on.
    const float rulerThickness = st.showRulers ? kRulerThickness : 0.0f;
    const ImVec2 avail(fullAvail.x - rulerThickness, fullAvail.y - rulerThickness);
    const ImVec2 paintOrigin(canvasPos.x + rulerThickness, canvasPos.y + rulerThickness);

    // --- fit to window / 100% (PRD Q1) -- consumed here, not where the
    // menu item or key fired, because both need `avail`: the canvas
    // window's actual on-screen size, which only exists inside this
    // Begin()/End() block. Same request-then-consume shape as
    // requestClear/requestReload below. Both re-centre (panX/panY = 0) --
    // "fit" and "100%" both mean "show me the whole thing squarely," and
    // rotation/mirror are untouched: those are independent view toggles,
    // not reset by a zoom command.
    if (st.requestFitWindow) {
      st.view.zoom = std::clamp(std::min(avail.x / texW, avail.y / texH), 0.1f, 8.0f);
      st.view.panX = 0.0f;
      st.view.panY = 0.0f;
      st.requestFitWindow = false;
    }
    if (st.requestZoom100) {
      st.view.zoom = 1.0f;
      st.view.panX = 0.0f;
      st.view.panY = 0.0f;
      st.requestZoom100 = false;
    }

    // `drawSize`/`origin` are exactly the pre-step-11 computation --
    // unrotated, unmirrored, zoom/pan only. Kept as-is (rather than folded
    // into the view matrix) specifically so wheel-zoom's cursor-anchoring
    // below, which reads `origin` and `canvasPos`, keeps behaving exactly
    // as it did before this step -- the regression bar PLAN.md step 11
    // asks for. Mirror and rotation are layered on top of this quad's own
    // *centre* (`pivotScreen` below), not baked into `origin` itself.
    const ImVec2 drawSize(texW * st.view.zoom, texH * st.view.zoom);
    const ImVec2 origin(
        paintOrigin.x + std::max(0.0f, (avail.x - drawSize.x) * 0.5f) + st.view.panX,
        paintOrigin.y + std::max(0.0f, (avail.y - drawSize.y) * 0.5f) + st.view.panY);

    // One view matrix (PLAN.md Phase 2 step 11): mirror and rotation pivot
    // around the canvas's own centre -- the point zoom/pan alone would
    // already draw the unrotated, unmirrored quad's centre at. See
    // app/ViewTransform.hpp/.cpp for why toScreen()/toCanvas() are
    // guaranteed to be exact inverses of each other rather than two
    // independently hand-derived formulas, and its .cpp's comment for the
    // algebra proving this reduces to today's plain `origin + p*zoom` when
    // mirror/rotation are at identity.
    const ImVec2 pivotScreen(origin.x + drawSize.x * 0.5f, origin.y + drawSize.y * 0.5f);
    const ViewTransform xform(st.view, Vec2{texW * 0.5f, texH * 0.5f},
                              Vec2{pivotScreen.x, pivotScreen.y});
    const Vec2 xc00 = xform.toScreen(Vec2{0.0f, 0.0f});
    const Vec2 xc10 = xform.toScreen(Vec2{texW, 0.0f});
    const Vec2 xc11 = xform.toScreen(Vec2{texW, texH});
    const Vec2 xc01 = xform.toScreen(Vec2{0.0f, texH});
    const ImVec2 q00(xc00.x, xc00.y), q10(xc10.x, xc10.y), q11(xc11.x, xc11.y),
        q01(xc01.x, xc01.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Drop shadow so the sheet reads as paper lying on a dark desk. A fixed
    // screen-space offset applied to the already-transformed quad, not
    // mapped through the transform itself -- a shadow shouldn't mirror or
    // rotate along with the paper; real light doesn't.
    dl->AddQuadFilled(ImVec2(q00.x + 6, q00.y + 6), ImVec2(q10.x + 6, q10.y + 6),
                      ImVec2(q11.x + 6, q11.y + 6), ImVec2(q01.x + 6, q01.y + 6),
                      IM_COL32(0, 0, 0, 110));
    // AddImageQuad, not AddImage: AddImage can only place an axis-aligned
    // rect, which has no way to express a flipped or rotated quad. This is
    // the one drawing change mirror/rotation actually needed -- everything
    // else is the transform feeding it different corner points.
    // Precedence chain, most-specific-preview-wins: grade, then
    // grayscale, then the plain canvas -- a deliberate narrow-scope
    // choice (PLAN.md Phase 3 step 6). Composing grade and grayscale
    // together isn't a normal use case and isn't required by PLAN.md's
    // Verify criterion for either step, so they stay mutually exclusive
    // by this precedence rather than layered.
    const bool gradeActive = st.view.grade && sim;
    const bool grayscaleActive = !gradeActive && st.view.grayscale && sim;
    if (sim) {
      const WGPUTextureView tv = gradeActive     ? sim->gradedView()
                                 : grayscaleActive ? sim->grayscaleView()
                                                    : sim->canvasView();
      dl->AddImageQuad((ImTextureID)(intptr_t)tv, q00, q10, q11, q01);
    } else {
      // 1.4 / ADR-0001: no PaintSim exists yet (nothing painted this
      // session), so there is no composite to show. A flat blank-paper
      // quad reads as "ready to paint" rather than a rendering glitch --
      // the first stroke below constructs the sim and this becomes the
      // real canvasView() from the very next frame.
      dl->AddQuadFilled(q00, q10, q11, q01, IM_COL32(250, 250, 247, 255));
    }
    dl->AddQuad(q00, q10, q11, q01, ImGui::GetColorU32(ImGuiCol_Border));

    ImGui::SetCursorScreenPos(paintOrigin);
    ImGui::InvisibleButton("##canvasHit", avail,
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    // Pen input maps back through the transform's actual inverse (docs/
    // shortcuts.md section 3) -- not a second, independently re-derived
    // "un-zoom, un-rotate, un-mirror" formula. This is what keeps painting
    // landing in the right place under a mirror or a rotation: `tx`/`ty`
    // feed the exact same stroke/paint-tool logic below that they always
    // did, in canvas texel space, regardless of what the view is doing.
    const Vec2 canvasMouse = xform.toCanvas(Vec2{mouse.x, mouse.y});
    const float tx = canvasMouse.x;
    const float ty = canvasMouse.y;

    // --- rulers + drag-to-create guides (PRD Q5) -- ruler hit regions live
    // in the band `avail`/`paintOrigin` carved out above, so they never
    // overlap ##canvasHit's paint/pan/rotate rect; only exist at all when
    // st.showRulers is on ("drag-to-create... only meaningful once rulers
    // exist and are visible", per this step's own scope note).
    if (rulerThickness > 0.0f) {
      drawRulers(dl, xform, st.view, canvasPos, paintOrigin, avail, rulerThickness);

      ImGui::SetCursorScreenPos(ImVec2(paintOrigin.x, canvasPos.y));
      ImGui::InvisibleButton("##rulerTop", ImVec2(avail.x, rulerThickness));
      if (ImGui::IsItemActivated())
        st.pendingGuide = Guide{GuideOrientation::Horizontal, canvasMouse.y};

      ImGui::SetCursorScreenPos(ImVec2(canvasPos.x, paintOrigin.y));
      ImGui::InvisibleButton("##rulerLeft", ImVec2(rulerThickness, avail.y));
      if (ImGui::IsItemActivated())
        st.pendingGuide = Guide{GuideOrientation::Vertical, canvasMouse.x};
    }

    // While a guide drag is in progress, update its (possibly snapped)
    // position every frame the mouse stays down; on release, commit it into
    // st.guides -- always, even if it landed outside [0,texW]/[0,texH] or
    // was barely dragged off the ruler (the numeric/percentage popup allows
    // any value too; "Clear Guides" in the View menu is the way back).
    // view.zoom is the transform's uniform length scale (rotation and
    // mirror both preserve length), so dividing the fixed screen-space
    // snap radius by it converts to document-space exactly, not
    // approximately -- the snap feels the same size on screen at any zoom.
    if (st.pendingGuide) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float snapThresholdDoc =
            st.snappingEnabled ? (kSnapThresholdPx / std::max(st.view.zoom, 0.01f)) : 0.0f;
        const SnapResult snap = resolveSnap(canvasMouse, st.guides, st.gridSpacing,
                                            st.gridSubdivisions, texW, texH, snapThresholdDoc);
        st.pendingGuide->position = st.pendingGuide->orientation == GuideOrientation::Horizontal
                                        ? snap.point.y
                                        : snap.point.x;
      } else {
        st.guides.push_back(*st.pendingGuide);
        st.pendingGuide.reset();
      }
    }

    // --- rotate view on drag (PRD Q4: "R" held + drag) -- resolved before
    // the pan/zoom blocks below since, like Hand-tool panning, it claims
    // the left-mouse-drag gesture; the two must agree on who wins. Not
    // routed through app/Keymap's resolve(): that dispatcher fires once per
    // discrete SDL_EVENT_KEY_DOWN, which has no way to express "held," so
    // this reads live key state directly the same way this file's own
    // tiltPad()/panning already read live mouse state instead of going
    // through a discrete action. `⇧R` (reset) *is* a discrete action --
    // see main.cpp's "reset_rotation" dispatch arm -- because resetting is
    // a one-shot command, not a hold.
    const bool rotateHeld = ImGui::IsKeyDown(ImGuiKey_R);
    // !st.pendingGuide: a guide drag-to-create claims the left-mouse-drag
    // gesture too (PRD Q5), the same way Hand-tool panning already does
    // below -- these must not fire simultaneously with dragging a new guide
    // off a ruler.
    const bool rotating = rotateHeld && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
                          !st.pendingGuide.has_value();
    if (rotating) {
      // Angular delta from a linear mouse delta around `pivotScreen`:
      // d(theta) ~= cross(radiusVector, mouseDelta) / |radiusVector|^2.
      // Exact in the limit and a good approximation at frame-to-frame
      // scale, same spirit as tiltPad's direct-manipulation mapping above.
      const ImVec2 v(mouse.x - pivotScreen.x, mouse.y - pivotScreen.y);
      const float r2 = v.x * v.x + v.y * v.y;
      // Too close to the pivot and the angle becomes ill-conditioned --
      // skip this frame's update rather than let a stray pixel of jitter
      // spin the view.
      if (r2 > 400.0f) {
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        st.view.rotation += (v.x * d.y - v.y * d.x) / r2;
      }
    }

    // --- zoom, anchored the same way whether the wheel or a keyboard
    // command triggered it (PRD Q1) -- one formula, reused, rather than the
    // keyboard commands inventing a second anchoring rule.
    auto applyZoomFactor = [&](float factor) {
      const float oldZoom = st.view.zoom;
      st.view.zoom = std::clamp(st.view.zoom * factor, 0.1f, 8.0f);
      const float k = st.view.zoom / oldZoom;
      st.view.panX = (st.view.panX + (origin.x - paintOrigin.x)) * k - (origin.x - paintOrigin.x);
      st.view.panY = (st.view.panY + (origin.y - paintOrigin.y)) * k - (origin.y - paintOrigin.y);
    };
    // --- zoom on wheel, anchored under the cursor ---
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f)
      applyZoomFactor(1.0f + ImGui::GetIO().MouseWheel * 0.12f);
    if (st.requestZoomIn) { applyZoomFactor(1.2f); st.requestZoomIn = false; }
    if (st.requestZoomOut) { applyZoomFactor(1.0f / 1.2f); st.requestZoomOut = false; }

    const bool panning =
        !rotateHeld && !st.pendingGuide.has_value() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
         (st.brush.tool == Tool::Hand && ImGui::IsMouseDragging(ImGuiMouseButton_Left)));
    if (panning) {
      const ImVec2 d = ImGui::GetIO().MouseDelta;
      st.view.panX += d.x;
      st.view.panY += d.y;
    }

    // --- stroke ---
    const bool paintTool = st.brush.tool == Tool::Brush ||
                           st.brush.tool == Tool::Water ||
                           st.brush.tool == Tool::DryBrush;
    const bool inside = tx >= 0 && ty >= 0 && tx < texW && ty < texH;
    const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);

    st.sim.brushActive = 0;
    st.paintingThisFrame = false;
    st.pendingDabs.clear();

    // Oil's contact -> velocity -> transfer pipeline (PaintSim::frame(),
    // shaders/oil_*.wgsl) still wants a genuine segment, not a point: its
    // tangential brush-velocity term (oil_velocity.wgsl's `vb`) and the
    // swept brush-grid footprint (include/donor.wgsl's brushGridCoord) both
    // need real motion between two positions. Feeding it
    // (last dab before now) -> (newest dab), both arc-length-quantised,
    // keeps that motion meaningful while making "holding still produces no
    // motion" true by construction -- jitter too small to cross a spacing
    // threshold never produces a dab, so it never advances this segment
    // either. That is what makes deleting oil_transfer.wgsl's
    // velocityCutoff safe (ADR-0003 bullet 4): a truly held brush now
    // reaches this function with pendingDabs empty, every frame.
    auto applyDabsToOilSegment = [&]() {
      if (st.pendingDabs.empty()) return;
      st.sim.brushAx = st.lastDabX;
      st.sim.brushAy = st.lastDabY;
      st.sim.brushBx = st.pendingDabs.back().x;
      st.sim.brushBy = st.pendingDabs.back().y;
      st.sim.brushActive = 1;
      st.lastDabX = st.pendingDabs.back().x;
      st.lastDabY = st.pendingDabs.back().y;
    };

    // !st.pendingGuide: same reasoning as `rotating`/`panning` above --
    // freehand painting must never fire while a guide drag is in progress.
    // This is also the boundary that keeps snapping (app/Snapping.hpp)
    // entirely out of the brush path: resolveSnap() is only ever called
    // from the pendingGuide block above, never from here -- a stroke is
    // exactly as continuous and unsnapped after this step as before it.
    if (paintTool && down && hovered && inside && !panning && !rotating &&
        !st.pendingGuide.has_value()) {
      // 1.4 / ADR-0001: this is the first moment a paint tool actually
      // deposits, so it's where PaintSim gets constructed if it doesn't
      // exist yet -- everything above (hover, tool selection, sliders,
      // even the tilt pad) never needed a live sim. A fresh construction
      // starts in PaintSim's default Watercolour mode; if the user already
      // picked a different medium from the Medium menu before ever
      // painting, honour that choice now instead of silently starting
      // Watercolour and waiting for a mode switch that already happened.
      const bool wasNull = !sim;
      PaintSim* s = ensurePaintSim(sim, gpu, canvasW, canvasH, lut);
      if (s && wasNull && st.mode != PaintMode::Watercolor) s->setMode(gpu, st.mode);

      if (s) {
        st.paintingThisFrame = true;
        if (!st.strokeActive) {
          st.strokeActive = true;
          st.lastX = tx;
          st.lastY = ty;
          st.lastDabX = tx;
          st.lastDabY = ty;
          // A fresh stroke recharges the oil brush with the selected paint,
          // and resets the arc-length emitter -- leftover distance and point
          // history from whatever stroke happened before must not bleed
          // into this one.
          st.sim.brushReload = 1;
          st.strokePath.reset();
        } else {
          st.sim.brushReload = 0;
        }
        const float pressure = st.penSeen ? st.penPressure : 1.0f;
        const float sizeMul = st.brush.pressureSize ? (0.25f + 0.75f * pressure) : 1.0f;
        const float flowMul = st.brush.pressureFlow ? (0.15f + 0.85f * pressure) : 1.0f;

        applyToolToBrush(st);
        st.sim.brushRadius = st.brush.radius * sizeMul;
        st.sim.brushPigment *= flowMul;
        st.sim.brushWater *= flowMul;

        // Arc-length dab emission (1.3 / ADR-0003): feed this frame's
        // sampled position through the centripetal Catmull-Rom emitter. It
        // returns 0..N dab positions spaced `spacing * radius` px apart
        // along the smoothed path, carrying any leftover sub-spacing
        // distance across frames so spacing stays continuous rather than
        // resetting on every render frame.
        const float spacingPx = std::max(st.brush.spacing * st.sim.brushRadius, 0.1f);
        st.strokePath.addPoint(tx, ty, spacingPx, st.pendingDabs);
        applyDabsToOilSegment();

        st.lastX = tx;
        st.lastY = ty;
      }
    } else if (!down) {
      if (st.strokeActive) {
        // Pen-up: flush whatever tail segment the emitter was still holding
        // back for lack of a real "next" sample to confirm its shape --
        // otherwise the last stretch of a stroke (up to one spacing
        // interval) silently deposits nothing.
        const float spacingPx = std::max(st.brush.spacing * st.sim.brushRadius, 0.1f);
        st.strokePath.flush(spacingPx, st.pendingDabs);
        applyDabsToOilSegment();
      }
      st.strokeActive = false;
    }

    // --- grid + guides overlay (PRD Q7, Q5) -- drawn once, after every
    // input-driven update above has settled for this frame (guide commit,
    // pan/zoom/rotate), so a guide committed this same frame draws
    // immediately rather than lagging a frame behind. ---
    if (st.showGrid) drawGridOverlay(dl, xform, texW, texH, st.gridSpacing, st.gridSubdivisions,
                                     st.view.zoom);

    if (st.showGuides) {
      constexpr ImU32 kGuideCol = IM_COL32(70, 190, 230, 200);
      constexpr ImU32 kPendingGuideCol = IM_COL32(140, 230, 255, 230);
      // Right-click near a guide removes it -- the only per-guide management
      // this step provides beyond bulk "Clear Guides" (View menu); repositioning
      // an existing guide isn't implemented (PRD Q5 only asks for the two
      // creation paths: drag-to-create and the numeric/percentage popup).
      // Only live while hovering the canvas and not mid-drag, so a stray
      // right-click elsewhere (or during guide creation) can't delete one.
      int guideToDelete = -1;
      if (hovered && !st.pendingGuide && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        constexpr float kPickPx = 5.0f;
        float bestDist = kPickPx;
        for (size_t i = 0; i < st.guides.size(); ++i) {
          const auto& g = st.guides[i];
          Vec2 a, b;
          if (g.orientation == GuideOrientation::Horizontal) {
            a = xform.toScreen(Vec2{0.0f, g.position});
            b = xform.toScreen(Vec2{texW, g.position});
          } else {
            a = xform.toScreen(Vec2{g.position, 0.0f});
            b = xform.toScreen(Vec2{g.position, texH});
          }
          const float d = distancePointToSegment(mouse, ImVec2(a.x, a.y), ImVec2(b.x, b.y));
          if (d < bestDist) {
            bestDist = d;
            guideToDelete = static_cast<int>(i);
          }
        }
      }
      if (guideToDelete >= 0)
        st.guides.erase(st.guides.begin() + guideToDelete);

      for (const auto& g : st.guides)
        drawGuideLine(dl, xform, g.orientation, g.position, texW, texH, kGuideCol);
      if (st.pendingGuide)
        drawGuideLine(dl, xform, st.pendingGuide->orientation, st.pendingGuide->position, texW,
                      texH, kPendingGuideCol);
    }

    // --- brush cursor ring ---
    if (hovered && paintTool && inside) {
      dl->AddCircle(mouse, st.brush.radius * st.view.zoom,
                    IM_COL32(235, 235, 225, 170), 32, 1.0f);
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();

  if (st.showDemo) ImGui::ShowDemoWindow(&st.showDemo);

  // Nothing to switch, clear or reload if no PaintSim exists yet -- st.mode
  // itself already carries the user's choice (see the Medium menu above)
  // and gets applied the moment painting actually constructs the sim.
  if (st.requestMode) {
    if (sim) sim->setMode(gpu, st.mode);
    st.requestMode = false;
  }
  if (st.requestClear) {
    if (sim) sim->clearCanvas(gpu);
    st.requestClear = false;
  }
  if (st.requestReload) {
    if (sim && sim->reloadShaders(gpu)) std::printf("[shader] reloaded\n");
    st.requestReload = false;
  }
}

}  // namespace np
