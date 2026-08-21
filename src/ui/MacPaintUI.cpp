#include "ui/MacPaintUI.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <string>
#include <vector>

#include "app/CurveEdit.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/HistoryPanel.hpp"
#include "app/Journal.hpp"
#include "app/LayerPanel.hpp"
#include "app/Snapping.hpp"
#include "app/ViewTransform.hpp"
#include "color/LutBake.hpp"  // kMaxCurvePointsPerChannel
#include "imgui.h"
#include "io/ExportAs.hpp"
#include "ui/DocumentTexture.hpp"

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

// ui/DocumentTexture: the active document, composited and uploaded once per
// revision, drawn over the paper by the canvas block and reported on by the
// layers panel. File-scope for exactly that reason -- two places in this file
// need the same instance, and it is neither app state (app/AppState.hpp's own
// rule: transient UI-owned state stays in ui/) nor something a caller of
// drawUI() has any use for.
//
// It owns GPU objects and is deliberately never released: gfx/Wgpu.hpp's
// convention is that every GPU object here lives for the process, and a
// destructor running after the device is gone would be worse than the leak it
// prevents. See DocumentTexture::release().
DocumentTexture g_documentTexture;

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

// PLAN.md Phase 3 step 8 ("Op-stack UI -- reorder, toggle, delete, and a
// curve widget operating in the shaper domain"). ---------------------------

const char* pointOpKindName(PointOpKind k) {
  switch (k) {
    case PointOpKind::Levels:       return "Levels";
    case PointOpKind::Curves:       return "Curves";
    case PointOpKind::Exposure:     return "Exposure";
    case PointOpKind::Saturation:   return "Saturation";
    case PointOpKind::Grayscale:    return "Grayscale";
    case PointOpKind::ChannelMixer: return "Channel Mixer";
    default:                       return "?";
  }
}

// The one PLAN.md step 8 names explicitly ("a curve widget operating in the
// shaper domain"). ImGui glue around app/CurveEdit.hpp's pure geometry/
// list-mutation helpers -- that header's own doc comment: "the plot, the
// click/drag/right-click handling and the spline draw itself are UI...
// everything here is what that UI calls into." Edits `curve` in place;
// returns true on any frame it actually changed (a point added, moved or
// removed), which the caller uses to decide whether to write the containing
// Op back through OpStack::setOp() -- this function never touches OpStack
// itself, matching every other per-kind editor in drawGradeSection() below.
//
// Axes are plain [0,1]x[0,1] shaper-domain space (ADR-0004) -- Curve control
// points are already shaper-domain coordinates by contract, so nothing here
// calls color::shaperEncode/Decode; see app/CurveEdit.hpp's own header
// comment for why plotting needs no colour-domain conversion at all.
bool drawCurveWidget(Curve& curve) {
  constexpr float kPlotSize = 200.0f;
  constexpr float kHitRadiusPx = 8.0f;
  constexpr int kSamples = 64;

  bool changed = false;
  ImGuiStorage* storage = ImGui::GetStateStorage();
  // Scoped by the caller's own PushID(opIndex) further up the ID stack, so
  // each Curves op in the stack keeps its own drag state independently.
  const ImGuiID dragKey = ImGui::GetID("curveDragIdx");
  int dragIdx = storage->GetInt(dragKey, -1);

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 plotMax(origin.x + kPlotSize, origin.y + kPlotSize);
  dl->AddRectFilled(origin, plotMax, IM_COL32(20, 20, 22, 255));

  // The spline itself, sampled across shaper-domain x via the same
  // evalCurve() the grading pipeline (ops/PointOps.cpp, color/LutBake) runs
  // -- the plotted line is never a separate approximation of what grading
  // will actually do.
  ImVec2 prevPt{};
  for (int s = 0; s <= kSamples; ++s) {
    const float cx = static_cast<float>(s) / static_cast<float>(kSamples);
    const float cy = evalCurve(curve, cx);
    float px = 0.0f, py = 0.0f;
    curveToPlot(cx, cy, kPlotSize, px, py);
    const ImVec2 pt(origin.x + px, origin.y + py);
    if (s > 0) dl->AddLine(prevPt, pt, IM_COL32(255, 200, 90, 255), 1.5f);
    prevPt = pt;
  }
  for (size_t i = 0; i < curve.size(); ++i) {
    float px = 0.0f, py = 0.0f;
    curveToPlot(curve[i].x, curve[i].y, kPlotSize, px, py);
    dl->AddCircleFilled(ImVec2(origin.x + px, origin.y + py), 4.0f,
                        IM_COL32(240, 240, 235, 255));
  }
  dl->AddRect(origin, plotMax, ImGui::GetColorU32(ImGuiCol_Border));

  ImGui::InvisibleButton("##curvePlot", ImVec2(kPlotSize, kPlotSize));
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 mouse = ImGui::GetIO().MousePos;
  const float mx = mouse.x - origin.x;
  const float my = mouse.y - origin.y;

  // Click-down: grab an existing point within radius, or plant a new one at
  // the click location and grab that -- mirrors this file's own
  // st.pendingGuide "click-down starts a drag, IsMouseDown continues it"
  // idiom (see the canvas block's ruler drag-to-create above) rather than a
  // from-scratch input pattern. Capped at kMaxCurvePointsPerChannel
  // (color/LutBake.hpp) -- color::LutBake's GPU kernel truncates any longer
  // curve to its first 16 points when baking (with a stderr warning), so
  // letting the widget accept more here would let the plotted spline
  // silently diverge from what grading actually bakes; existing points
  // beyond the cap (none can exist, since insertion is capped) would still
  // be movable/deletable if they somehow did.
  if (ImGui::IsItemActivated()) {
    const auto hit = hitTestPoint(curve, mx, my, kPlotSize, kHitRadiusPx);
    if (hit) {
      dragIdx = static_cast<int>(*hit);
    } else if (curve.size() < static_cast<size_t>(kMaxCurvePointsPerChannel)) {
      float cx = 0.0f, cy = 0.0f;
      plotToCurve(mx, my, kPlotSize, cx, cy);
      dragIdx = static_cast<int>(insertPoint(curve, cx, cy));
      changed = true;
    }
    storage->SetInt(dragKey, dragIdx);
  }
  if (dragIdx >= 0 && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    float cx = 0.0f, cy = 0.0f;
    plotToCurve(mx, my, kPlotSize, cx, cy);
    dragIdx = static_cast<int>(movePoint(curve, static_cast<size_t>(dragIdx), cx, cy));
    storage->SetInt(dragKey, dragIdx);
    changed = true;
  } else if (dragIdx >= 0) {
    dragIdx = -1;
    storage->SetInt(dragKey, dragIdx);
  }

  // Right-click deletes the point under the cursor, if any -- PLAN.md step
  // 8's own "double-click (or right-click, your call)" wording, decided in
  // favour of right-click: a double-click's second click also fires as an
  // ordinary left click one event earlier (Dear ImGui's own documented
  // behaviour -- "note that a double-click will also report
  // IsMouseClicked() == true"), which would insert-then-immediately-delete
  // a point when double-clicking empty plot area under the click-to-add
  // handler just above. Right-click has no such overlap with the left-click
  // gestures, so it needs no special-casing around it.
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    const auto hit = hitTestPoint(curve, mx, my, kPlotSize, kHitRadiusPx);
    if (hit) {
      removePoint(curve, *hit);
      changed = true;
      // Indices may have shifted under the removal -- abandon any
      // in-progress drag rather than risk moving the wrong point next frame.
      dragIdx = -1;
      storage->SetInt(dragKey, dragIdx);
    }
  }

  if (hovered)
    ImGui::SetTooltip("Click empty area: add point\nDrag a point: move it\n"
                      "Right-click a point: delete it");

  ImGui::TextDisabled("%d / %d points", static_cast<int>(curve.size()),
                      kMaxCurvePointsPerChannel);
  return changed;
}

// The rest of PLAN.md step 8: add/list/toggle/reorder/delete, plus the
// per-kind inline editors -- deliberately minimal outside Curves (see this
// step's own scope notes below at each kind). Called once from the
// ##controls panel's GRADE section, mirroring that panel's existing
// TextUnformatted/Separator/Dummy idiom for every other section (BRUSH,
// PIGMENT, OIL/INK/WATER, GRID, SOLVER above it).
void drawGradeSection(AppState& st) {
  ImGui::Checkbox("Preview Graded Output", &st.view.grade);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Shows the canvas through the op stack below\n"
                      "(PLAN.md Phase 3's grading pipeline). Off by\n"
                      "default, the same as Grayscale Preview above --\n"
                      "grading never turns on just because the stack\n"
                      "below is non-empty.");

  // 1. Add -- PLAN.md step 8 item 1. A new op always starts disabled: this
  // codebase already has one established "build then reveal" pattern for
  // brand-new content (a freshly dragged guide, a newly placed layer) never
  // exists, so "add a not-yet-visible op, then opt it in" is the safer
  // default over "adding an op instantly changes what you see" -- the same
  // reasoning CanvasView::grade itself follows for the whole preview.
  static int newOpKindIdx = 0;
  const char* kKindNames[] = {"Levels", "Curves", "Exposure",
                              "Saturation", "Grayscale", "Channel Mixer"};
  ImGui::SetNextItemWidth(150.0f);
  ImGui::Combo("##newOpKind", &newOpKindIdx, kKindNames, IM_ARRAYSIZE(kKindNames));
  ImGui::SameLine();
  if (ImGui::Button("+ Add")) {
    Op op;
    op.opClass = OpClass::PointA;
    op.enabled = false;
    op.pointKind = static_cast<PointOpKind>(newOpKindIdx);
    st.opStack.add(op);
  }

  // 2/3. List, one row per op in stack order, each with an enable checkbox,
  // a kind label, up/down/delete, and (for Curves) the widget above.
  //
  // `op` is a *copy* of st.opStack.at(i), not a reference -- reorder()/
  // remove() erase-and-insert into OpStack's internal std::vector<Op>,
  // which can invalidate references to later elements; holding a reference
  // across one of those calls and then reading it again in the same
  // iteration (e.g. the per-kind editor below) would be exactly that bug.
  // `structureChanged` stops the loop the same frame a reorder/remove
  // fires, rather than continuing to render rows against indices that no
  // longer mean what they did a moment ago -- the list simply reflects the
  // new state from the next frame on, standard immediate-mode practice.
  bool structureChanged = false;
  for (size_t i = 0; i < st.opStack.size() && !structureChanged; ++i) {
    ImGui::PushID(static_cast<int>(i));
    Op op = st.opStack.at(i);

    bool enabled = op.enabled;
    // Must go through OpStack::setEnabled(), never `op.enabled = ...`
    // directly: `op` here is a local copy, so a direct mutation wouldn't
    // even reach the live stack, and OpStack doesn't expose a non-const
    // reference to mutate in place either way -- setEnabled() is the only
    // path that bumps version(), which updateGradePreview()'s rebake gate
    // depends on entirely.
    if (ImGui::Checkbox("##en", &enabled)) st.opStack.setEnabled(i, enabled);
    ImGui::SameLine();
    ImGui::TextUnformatted(pointOpKindName(op.pointKind));

    ImGui::BeginDisabled(i == 0);
    if (ImGui::SmallButton("Up")) {
      st.opStack.reorder(i, i - 1);
      structureChanged = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(i + 1 >= st.opStack.size());
    if (ImGui::SmallButton("Down")) {
      st.opStack.reorder(i, i + 1);
      structureChanged = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Delete")) {
      st.opStack.remove(i);
      structureChanged = true;
    }

    // 3. Per-kind inline editor -- deliberately minimal, not exhaustive
    // polish (see each case's own comment for the specific scope cut).
    // Skipped once structureChanged is true: `op` may already be stale
    // (see the comment above the loop), and the row is about to disappear
    // from the very next frame regardless.
    if (!structureChanged) {
      ImGui::Indent();
      bool changed = false;
      switch (op.pointKind) {
        case PointOpKind::Exposure:
          // Linear-light stops (ops/PointOps.hpp's ExposureParams -- the
          // one op deliberately NOT in the shaper domain). +-5 stops is a
          // generously wide but ordinary editing range.
          changed = ImGui::SliderFloat("Stops", &op.exposure.stops, -5.0f, 5.0f);
          break;
        case PointOpKind::Saturation:
          // lumaWeights stays at kRec709LumaWeights -- not exposed in this
          // narrow scope, matching Grayscale's own cut below for the same
          // shared weight.
          changed = ImGui::SliderFloat("Scale", &op.saturation.scale, 0.0f, 2.0f);
          break;
        case PointOpKind::Levels: {
          // ONE shared LevelsParams editor applied identically to all
          // three levels[0..2] entries on any change -- PLAN.md step 2's
          // own wording: "a composite levels adjustment is just the caller
          // passing the same LevelsParams for all three channels, not a
          // separate code path." Per-channel authoring is out of this
          // narrow scope.
          LevelsParams p = op.levels[0];
          bool ch = false;
          ch |= ImGui::SliderFloat("Black in", &p.blackIn, 0.0f, 1.0f);
          ch |= ImGui::SliderFloat("White in", &p.whiteIn, 0.0f, 1.0f);
          ch |= ImGui::SliderFloat("Gamma", &p.gamma, 0.1f, 4.0f);
          ch |= ImGui::SliderFloat("Black out", &p.blackOut, 0.0f, 1.0f);
          ch |= ImGui::SliderFloat("White out", &p.whiteOut, 0.0f, 1.0f);
          if (ch) {
            op.levels[0] = op.levels[1] = op.levels[2] = p;
            changed = true;
          }
          break;
        }
        case PointOpKind::Grayscale:
          // No weight editor in this narrow scope -- default
          // kRec709LumaWeights only. Fully functional either way: add/
          // toggle/reorder/delete all work, and it bakes and grades
          // correctly at the default weights. A per-channel weight editor
          // is real, separate scope PLAN.md step 8's literal wording (it
          // names only "a curve widget," singular) doesn't call for.
          ImGui::TextDisabled("(default Rec.709 weights -- no editor in this scope)");
          break;
        case PointOpKind::ChannelMixer:
          // Same treatment as Grayscale immediately above, for the same
          // reason -- a 12-value 3x4 matrix editor is real, separate scope.
          ImGui::TextDisabled("(identity matrix -- no matrix editor in this scope)");
          break;
        case PointOpKind::Curves: {
          // The one PLAN.md step 8 explicitly calls out. Channel tabs
          // (R/G/B) pick which of curves[0..2] the widget above shows/
          // edits; selection persists per-op-row via ImGui's own ID-scoped
          // state storage (GetStateStorage()), not a new AppState field --
          // this is UI-only state, exactly like drawCurveWidget()'s own
          // drag-index storage right above it.
          ImGuiStorage* storage = ImGui::GetStateStorage();
          const ImGuiID chKey = ImGui::GetID("curveChannel");
          int channel = storage->GetInt(chKey, 0);
          const char* chNames[3] = {"R", "G", "B"};
          for (int c = 0; c < 3; ++c) {
            if (c > 0) ImGui::SameLine();
            const bool sel = channel == c;
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(chNames[c])) {
              channel = c;
              storage->SetInt(chKey, c);
            }
            if (sel) ImGui::PopStyleColor();
          }
          if (drawCurveWidget(op.curves[static_cast<size_t>(channel)])) changed = true;
          break;
        }
      }
      if (changed) st.opStack.setOp(i, op);
      ImGui::Unindent();
    }
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PopID();
  }
}

// ------------------------------------------------------------------ Layers
//
// PLAN.md Phase 5 step 1's panel, following docs/ui.md §3.2's layer row: a
// kind glyph, the name, and a monospace sub-line reading `RGB · NORMAL · 100%`.
//
// Same split as drawCurveWidget()/app/CurveEdit and drawExportAsDialog()/
// io/ExportAs: everything that can be wrong here without a screenshot showing
// it lives outside this function and is exercised headlessly by `--selftest`.
// Specifically:
//
//  * **Which layer a row is** -- app/LayerPanel's `layerIndexForPanelRow()`.
//    The panel lists top-first while `Document::layers` is bottom-first, and
//    that reversal happens in exactly one place, in a pure function with a
//    test, because a second reversal in this draw loop is precisely how "Up"
//    ends up moving a layer down.
//  * **What a row says** -- app/LayerPanel's `layerRowTitle()` /
//    `layerRowSubLine()` / `layerKindGlyph()`.
//  * **What a button does** -- core/LayerOps' add/remove/move/duplicate and
//    the property setters, which are also where `locked` is enforced. This
//    function never touches `doc.layers` directly, so the lock cannot be
//    bypassed by a widget; a refusal comes back as the operation's own
//    sentence and is shown verbatim rather than reworded.
//  * **Dirty and journal tracking** -- app/DocumentLifecycle's
//    `recordLayerEdit()`, which is what makes every change here a structural
//    edit (PRD O5) rather than something the journal finds out about on its
//    next timer tick.
//
// --- Which half is visible, stated plainly --------------------------------
//
// **These layers are on the canvas**, as of the UI detour's step 2.
// ui/DocumentTexture composites this document and the canvas block draws it
// over the paper, so toggling a visibility box, dragging an opacity, changing
// a blend, adding a mask, grading through an adjustment layer or clipping one
// layer to another changes what is on screen while you watch. Every sentence
// this comment used to carry about a panel that "never shows what is on
// screen" is deleted rather than softened, because it is no longer true.
//
// **The paint is still not one of these layers.** `sim::PaintSim` owns a
// single dense GPU texture with no layer or document awareness; a stroke
// writes that texture and touches no `Layer::rgbTiles` anywhere. So the two
// pictures are *stacked* -- document over paper -- and not merged: a document
// that has been painted on still holds none of the paint. Closing that gap is
// the stroke bridge, a later step of this detour, and the panel says which
// half is which on screen rather than leaving a user to infer it.
//
// There are still no thumbnails. The reason used to be that a thumbnail of a
// document the canvas could not reach would imply a connection that did not
// exist; now it is only that the canvas shows the composite itself, at full
// size, which is a better thumbnail than a thumbnail.
//
// **The blend dropdown arrived with PLAN.md Phase 5 step 2** and it holds no
// list of its own. Which modes exist, which of them may be offered on *this*
// layer (PRD L5) and what each entry reads (PRD B7's display-referred label)
// all come from app/LayerPanel's `blendMenuForLayer()` / `blendMenuEntryText()`
// / `blendMenuSelection()`, which are pure and tested headlessly; this function
// only draws them and hands the chosen mode to `core::setLayerBlend()`. There
// is deliberately no string literal naming a blend mode anywhere in this file.
// The row also *displays* the blend it carries, marked `(!)` when this build
// cannot composite it.
void drawLayersSection(AppState& st) {
  OpenDocument* od = st.documents.active();
  if (od == nullptr) {
    ImGui::TextDisabled("No document open.");
    ImGui::TextWrapped("A session starts with one, so this means every document was closed. "
                       "File > New Document or File > Open... makes another, and its layers "
                       "are drawn on the canvas over the paper. Painting is separate: a "
                       "stroke writes sim::PaintSim's texture, not a layer, so paint never "
                       "appears in this list.");
    return;
  }

  Document& doc = od->document;
  ImGui::TextDisabled("%s -- %d x %d, %zu layer(s)", documentDisplayName(*od).c_str(), doc.width,
                      doc.height, doc.layers.size());
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("The layers of the open document, composited and drawn\n"
                      "on the canvas over the paper.\n"
                      "Painting is still separate: sim::PaintSim owns one dense\n"
                      "texture with no layer awareness, so a stroke reaches no\n"
                      "layer and nothing painted appears here.");
  // The revision cache, measured rather than believed (ui/DocumentTexture.hpp
  // owns the argument). `uploads` counts recomposites; `cached` counts frames
  // that cost two integer comparisons. In a still window the second number
  // climbs at the frame rate and the first does not move at all.
  ImGui::TextDisabled("composite: %llu upload(s), %llu cached, last %.2f ms",
                      static_cast<unsigned long long>(g_documentTexture.uploads()),
                      static_cast<unsigned long long>(g_documentTexture.cacheHits()),
                      g_documentTexture.lastUploadMs());

  // Session-local, exactly like drawGradeSection()'s newOpKindIdx and the
  // Export As dialog's fields -- app/AppState.hpp's own rule is that transient
  // widget state stays in ui/. The selection is a *model* index (bottom-first),
  // not a panel row, so it survives a reorder meaning the same layer.
  static size_t selected = 0;
  static std::string lastError;
  static char renameBuf[128] = "";

  if (selected >= doc.layers.size()) selected = doc.layers.empty() ? 0 : doc.layers.size() - 1;

  auto run = [&](LayerOpResult r) {
    const DocumentOpResult out = recordLayerEdit(*od, std::move(r));
    lastError = out.ok ? std::string() : out.error;
    return out.ok;
  };

  if (ImGui::SmallButton("+ Add")) {
    // Above the selection, which is where every editor puts a new layer.
    const size_t at = doc.layers.empty() ? 0 : selected + 1;
    if (run(addLayer(doc, at, makeRgbLayer(defaultNewLayerName(doc))))) selected = at;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(doc.layers.empty());
  if (ImGui::SmallButton("Duplicate")) {
    if (run(duplicateLayer(doc, selected))) selected += 1;
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Delete")) {
    if (run(removeLayer(doc, selected)) && selected > 0) selected -= 1;
  }
  ImGui::EndDisabled();

  // Rows, top of the stack first. `structureChanged` stops the loop the same
  // frame a reorder/add/remove fires rather than continuing to render rows
  // against indices that no longer mean what they did -- the identical
  // precaution drawGradeSection() takes over core::OpStack, and for the
  // identical reason (core/LayerOps' move/remove shift the vector).
  bool structureChanged = false;
  const size_t count = doc.layers.size();
  for (size_t row = 0; row < count && !structureChanged; ++row) {
    const size_t i = layerIndexForPanelRow(row, count);
    const Layer& layer = doc.layers[i];
    ImGui::PushID(static_cast<int>(i));

    bool visible = layer.visible;
    if (ImGui::Checkbox("##vis", &visible)) {
      run(setLayerVisible(doc, i, visible));
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Visibility. Allowed even on a locked layer --\n"
                        "hiding a layer changes nothing about it.");
    ImGui::SameLine();
    ImGui::TextUnformatted(layerKindGlyph(layer.kind));
    ImGui::SameLine();
    if (ImGui::Selectable(layerRowTitle(layer, i).c_str(), selected == i)) selected = i;
    ImGui::Indent();
    ImGui::TextDisabled("%s", layerRowSubLine(layer).c_str());

    ImGui::BeginDisabled(i + 1 >= count);
    if (ImGui::SmallButton("Up")) {  // up the panel == up the stack == +1
      if (run(moveLayer(doc, i, i + 1))) selected = i + 1;
      structureChanged = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(i == 0);
    if (ImGui::SmallButton("Down")) {
      if (run(moveLayer(doc, i, i - 1))) selected = i - 1;
      structureChanged = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    bool locked = layer.locked;
    if (ImGui::Checkbox("Lock", &locked)) run(setLayerLocked(doc, i, locked));
    ImGui::SameLine();
    // PLAN.md Phase 5 step 9 / PRD C9. Disabled on the bottom row rather than
    // offered-and-refused, because "there is nothing below this layer" is a
    // property of the row itself and the Up/Down buttons on the same line
    // already use exactly that idiom for the ends of the stack. Every *other*
    // reason a clip cannot be taken -- a layer that holds no pixels beneath,
    // an unbroken clipped run below, a live `Mix` pair -- depends on the whole
    // stack and is surfaced as core/LayerOps' own refusal sentence below,
    // which is the same split drawExportAsDialog() uses for io/Export's.
    ImGui::BeginDisabled(i == 0);
    bool clipped = layer.clipped;
    if (ImGui::Checkbox("Clip", &clipped)) run(setLayerClipped(doc, i, clipped));
    ImGui::EndDisabled();
    // `AllowWhenDisabled` on purpose: the bottom row is exactly the row whose
    // user most needs the last sentence of this tooltip, and a disabled item
    // reports no hover without it.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Clip to the layer below (PRD C9): this layer shows only\n"
                        "where the layer beneath it has alpha. A run of clipped\n"
                        "layers all clips to ONE base -- the nearest unclipped\n"
                        "layer below -- and the group meets the document through\n"
                        "that base's blend mode and opacity, so hiding the base\n"
                        "hides the whole group. The bottom layer cannot be\n"
                        "clipped: there is nothing below it.");

    if (selected == i && !structureChanged) {
      float opacity = layer.opacity;
      // SliderFloat reports a change every frame of a drag; each one goes
      // through setLayerOpacity() so a locked layer refuses every one of them
      // rather than the first only. The revision counter therefore rises once
      // per frame of a drag -- accepted here because history (Phase 5 step 7)
      // is what owns coalescing an interaction into one entry, and inventing a
      // second, weaker coalescing rule in the panel would be in its way.
      if (ImGui::SliderFloat("Opacity", &opacity, 0.0f, 1.0f, "%.2f"))
        run(setLayerOpacity(doc, i, opacity));

      // The blend dropdown. The preview string is the *selected entry's* text
      // when the layer carries a mode in the menu, and the layer's raw stored
      // string when it does not -- a value from a newer build (PRD I10), or
      // `Mix` on a layer a reorder has just taken out of L5's reach. Showing
      // the first entry instead would quietly claim the layer says "Normal"
      // when it does not.
      const std::vector<BlendMode> menu = blendMenuForLayer(doc, i);
      const size_t sel = blendMenuSelection(doc, i, menu);
      const std::string preview =
          sel < menu.size() ? blendMenuEntryText(menu[sel]) : layer.blend + "  (this build "
                                                                           "cannot set this)";
      if (ImGui::BeginCombo("Blend", preview.c_str())) {
        for (size_t m = 0; m < menu.size(); ++m) {
          const bool isSelected = (m == sel);
          if (ImGui::Selectable(blendMenuEntryText(menu[m]).c_str(), isSelected))
            run(setLayerBlend(doc, i, menu[m]));
          if (isSelected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Blend modes are chosen for the linear working space.\n"
                          "One of them -- Screen -- only behaves above 1.0 if 1.0\n"
                          "is white, so it is labelled display-referred (PRD B7).\n"
                          "Mix is offered only on a Pigment layer sitting on\n"
                          "another Pigment layer (PRD L5), and does not composite\n"
                          "yet: no layer stores a pigment latent until Pigment\n"
                          "tiles land.");

      std::snprintf(renameBuf, sizeof(renameBuf), "%s", layer.name.c_str());
      if (ImGui::InputText("Name", renameBuf, sizeof(renameBuf),
                           ImGuiInputTextFlags_EnterReturnsTrue))
        run(setLayerName(doc, i, renameBuf));
    }
    ImGui::Unindent();
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PopID();
  }

  // The refusal, verbatim. core/LayerOps' sentences already name the layer and
  // say what to do about it, so there is no second vocabulary here to drift
  // from the model's -- the same rule drawExportAsDialog() follows for
  // io/Export's messages.
  if (!lastError.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 120, 110, 255));
    ImGui::TextWrapped("%s", lastError.c_str());
    ImGui::PopStyleColor();
    if (ImGui::SmallButton("Dismiss")) lastError.clear();
  }
}

// ----------------------------------------------------------------- History
//
// PLAN.md Phase 5 step 8 ("History panel listing entries by originating tool
// or op; clicking one moves the cursor there in a single replay, not N"; PRD
// O2, O3, with O1's redo and O4's snapshots made visible). docs/ui.md §5 puts
// it in this right-hand docked column.
//
// Same split as drawLayersSection()/app/LayerPanel: everything that can be
// wrong here without a screenshot showing it lives in app/HistoryPanel.hpp and
// is exercised headlessly by `--selftest`. Specifically:
//
//  * **Which state a row is** -- `HistoryPanelRow::serial`, and NOT its index.
//    Eviction shifts every index down by one, so an index-keyed row would
//    silently repoint at a different state; this loop never passes a row
//    number to anything.
//  * **What a row says** -- `historyRowText()`, including the PAST / CURRENT /
//    REDOABLE word, so the redo tail a new edit would destroy is legible in
//    text and not only in a colour.
//  * **What a click does** -- `historyPanelClick()`, which performs exactly one
//    `History::jumpTo()` at any distance (PRD O3) and refuses, with numbers,
//    rather than redirecting when the row's state is gone.
//  * **What the notes say** -- `historyDroppedNote()` and
//    `historyRedoTailNote()`.
//
// Two deliberate choices in this function itself:
//
//  1. **The click is applied after the row loop, never inside it.** A jump
//     changes the cursor, which changes every row's state; acting mid-loop
//     would render the rest of the list against a cursor that has already
//     moved. The identical precaution drawLayersSection() takes with
//     `structureChanged`, for the identical reason.
//  2. **`History::budgetPressure()` and `overBudget()` are not called here.**
//     Both run `bytes()`, an O(slots) scan over every tile of every entry, and
//     this function runs every frame. The part of that a user can act on --
//     that undo has stopped going back, and why -- is in
//     `historyDroppedNote()`, which reads two O(1) counters.
//
// **What this panel does NOT show, stated plainly**: a painted stroke. It
// lists the open document's history, and `sim::PaintSim` owns a dense GPU
// texture no stroke escapes -- core/History.hpp says so at length. So the rows
// are layer operations, placing an image, duplicating and reverting, and a
// session spent painting produces exactly one row.
void drawHistorySection(AppState& st) {
  OpenDocument* od = st.documents.active();
  if (od == nullptr) {
    ImGui::TextDisabled("No document open.");
    return;
  }

  History& h = od->history;
  static std::string lastError;

  // A cursor move is not an edit: it must NOT go through recordEdit(), which
  // would append a history entry for the act of moving through history. It
  // does change the document on screen, so the dirty flag and the journal's
  // structural revision both move -- an undone "add layer" changes the layer
  // stack, and a journal that kept the pre-jump structural state would be
  // holding a document that is no longer open.
  auto install = [&](const HistoryPanelClick& r) {
    lastError = r.ok ? std::string() : r.refusal;
    if (!r.ok || r.document == nullptr) return;
    od->document = *r.document;
    ++od->revision;
    ++od->structuralRevision;
  };

  const std::vector<HistoryPanelRow> rows = historyPanelRows(h);
  ImGui::TextDisabled("%zu state(s), cursor on %zu", rows.size(),
                      rows.empty() ? 0 : h.cursor() + 1);

  // Undo and Redo go through the panel's own click rather than
  // `History::undo()` / `redo()`, so there is exactly one path from a widget to
  // a cursor move and exactly one place a refusal can come from. The two guards
  // are `canUndo()` / `canRedo()`, so the neighbouring row always exists and
  // `cursor() - 1` cannot wrap.
  ImGui::BeginDisabled(!h.canUndo());
  if (ImGui::SmallButton("Undo"))
    install(historyPanelClick(h, historySerialForRow(h, h.cursor() - 1)));
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!h.canRedo());
  if (ImGui::SmallButton("Redo"))
    install(historyPanelClick(h, historySerialForRow(h, h.cursor() + 1)));
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::SmallButton("Snapshot"))
    h.takeSnapshot("snapshot " + std::to_string(h.snapshots().size() + 1), od->document);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("A named state exempt from the byte budget's eviction\n"
                      "until you dismiss it (PRD O4). Kept in its own list, not\n"
                      "in the undo chain, so undoing past it cannot lose it.");

  const std::string dropped = historyDroppedNote(h);
  if (!dropped.empty()) ImGui::TextWrapped("%s", dropped.c_str());

  // Rows, oldest at the top -- the OPPOSITE of drawLayersSection()'s order,
  // and app/HistoryPanel.hpp says why the two panels differ. There is no
  // reversal in this loop and there must not be one.
  uint64_t clickedEntry = 0;
  for (const HistoryPanelRow& row : rows) {
    ImGui::PushID(static_cast<int>(row.serial));
    const bool redoable = row.state == HistoryRowState::Redoable;
    if (redoable) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 150, 150, 255));
    if (ImGui::Selectable(historyRowText(row).c_str(), row.state == HistoryRowState::Current))
      clickedEntry = row.serial;
    if (redoable) ImGui::PopStyleColor();
    ImGui::PopID();
  }

  const std::string tail = historyRedoTailNote(h);
  if (!tail.empty()) ImGui::TextWrapped("%s", tail.c_str());

  // Snapshots, as their own group below the list and never interleaved into
  // it: they are not on the linear chain and have no cursor position, which is
  // core/History.hpp's reason for their being a second list in the first
  // place.
  const std::vector<HistorySnapshotRow> snaps = historySnapshotRows(h);
  if (!snaps.empty()) {
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::TextDisabled("SNAPSHOTS (exempt from eviction)");
    uint64_t restore = 0;
    size_t dismiss = kNoHistoryRow;
    for (const HistorySnapshotRow& row : snaps) {
      ImGui::PushID(static_cast<int>(row.serial));
      ImGui::TextUnformatted(historySnapshotRowText(row).c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Restore")) restore = row.serial;
      ImGui::SameLine();
      if (ImGui::SmallButton("Dismiss")) dismiss = row.index;
      ImGui::PopID();
    }
    if (restore != 0) install(historyPanelRestoreSnapshot(h, restore));
    if (dismiss != kNoHistoryRow) h.dismissSnapshot(dismiss);
  }

  if (clickedEntry != 0) install(historyPanelClick(h, clickedEntry));

  // The refusal, verbatim, for the same reason drawLayersSection() shows
  // core/LayerOps' sentence verbatim: app/HistoryPanel's refusals already name
  // the counts and say what did not happen, so there is no second vocabulary
  // here to drift from theirs.
  if (!lastError.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 120, 110, 255));
    ImGui::TextWrapped("%s", lastError.c_str());
    ImGui::PopStyleColor();
    if (ImGui::SmallButton("Dismiss##historyerror")) lastError.clear();
  }
}

// ---------------------------------------------------------------- Export As
//
// PLAN.md Phase 4 step 7 / PRD I15 ("Export As: target format, colour space,
// bit depth **and resize**, with saveable presets"). Everything that can be
// wrong here without a screenshot showing it -- what may be offered, what a
// combination costs, how a preset is stored and what happens to one this
// build cannot honour -- lives in io/ExportAs.hpp and is exercised headlessly
// by --selftest. This function is the widgets and nothing else, the same
// split app/CurveEdit + drawCurveWidget() already has.
//
// Two properties are structural rather than careful:
//
//  1. **It cannot offer a combination io/Export would refuse.** The format
//     list is offerableExportFormats() and the depth list is
//     offerableExportDepths(format), both computed from io/Capabilities'
//     runtime query -- so in an NP_USE_OIIO=OFF build EXR is not in the menu
//     at all, and in an ON build EXR offers half and 32-bit float but not
//     8-bit, because that is what this OpenImageIO actually writes. The
//     formats this build cannot write are still *shown*, greyed, with the
//     capability query's own reason as their tooltip: a menu that silently
//     omits EXR cannot answer "why can't I export EXR?".
//  2. **Every message it shows is io/Export's own.** The red line under the
//     controls is exportRefusalReason()'s string verbatim, not a reworded
//     one, so there is no second vocabulary here to drift from the encoder's.
//
// **Updated by PLAN.md Phase 4 step 8.** When step 7 landed, the Export
// button was disabled and said so, because there was no `core::Document`
// anywhere in the running application to export from. There is now:
// `AppState::documents` holds them, and File > New Document / Open... puts
// one there. So the button is live whenever a document is open, exporting the
// *active document* -- and still disabled, with the accurate reason, when
// none is. What has NOT changed is the honest half: the painting canvas is
// still not a document (sim::PaintSim owns one dense texture with no layer
// awareness), so this exports what was opened, never what is on screen. The
// dialog says which of the two it is looking at rather than leaving it to be
// inferred.
bool g_exportAsRequested = false;

void drawExportAsDialog(AppState& st, uint32_t canvasW, uint32_t canvasH) {
  // Session state. Function-local statics, exactly like drawGradeSection()'s
  // newOpKindIdx and the Add Guide popup's fields -- this is UI state, not
  // app state, and app/AppState's ownership is PLAN.md Phase 4 step 8's
  // decision to make rather than this step's to pre-empt.
  static ExportRequest request;
  static ExportPresetStore presets;
  static bool presetsLoaded = false;
  static char presetNameBuf[96] = "";
  static char exportPathBuf[512] = "";
  static std::string status;

  if (g_exportAsRequested) {
    g_exportAsRequested = false;
    ImGui::OpenPopup("Export As");
  }
  if (!ImGui::BeginPopupModal("Export As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  // Loaded on first open, never at startup: a preset file nobody asked for
  // costs nothing (PRD A2, ADR-0001), and --selftest's idle-RSS measurement
  // would notice if that stopped being true.
  if (!presetsLoaded) {
    presetsLoaded = true;
    if (!presets.loadFromFile(defaultExportPresetsPath()))
      status = presets.error();
    else if (!presets.problems().empty())
      status = presets.problems().front();
  }

  const std::string presetsPath = defaultExportPresetsPath();
  const ImVec4 kError(0.95f, 0.45f, 0.40f, 1.0f);
  const ImVec4 kWarn(0.92f, 0.78f, 0.35f, 1.0f);

  // What is being exported, named rather than assumed. These are two
  // genuinely different things in this build and conflating them on screen
  // would be the dishonest option.
  const OpenDocument* activeDoc = st.documents.active();
  const uint32_t srcW = activeDoc ? static_cast<uint32_t>(activeDoc->document.width) : canvasW;
  const uint32_t srcH = activeDoc ? static_cast<uint32_t>(activeDoc->document.height) : canvasH;
  if (activeDoc)
    ImGui::TextDisabled("Source: %ux%u (document '%s')", srcW, srcH,
                        documentDisplayName(*activeDoc).c_str());
  else
    ImGui::TextDisabled("Source: %ux%u (the live canvas -- not a document; nothing to export)",
                        srcW, srcH);
  ImGui::Separator();

  // --- Format -------------------------------------------------------------
  if (ImGui::BeginCombo("Format", imageFormatName(request.format))) {
    for (const FormatCapability& caps : allFormatCapabilities()) {
      const bool writable = caps.canWrite;
      if (!writable) ImGui::BeginDisabled();
      if (ImGui::Selectable(imageFormatName(caps.format), caps.format == request.format) &&
          writable) {
        request.format = caps.format;
        // Keep the depth legal for the new format rather than leaving an
        // impossible pair on screen and refusing it a line later.
        const std::vector<ExportBitDepth> depths = offerableExportDepths(request.format);
        if (!depths.empty() &&
            std::find(depths.begin(), depths.end(), request.bitDepth) == depths.end())
          request.bitDepth = depths.front();
      }
      if (!writable) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
          const std::string why =
              exportRefusalReason(caps.format, request.targetSpace, request.bitDepth, nullptr,
                                  nullptr);
          ImGui::SetTooltip("%s", why.c_str());
        }
      }
    }
    ImGui::EndCombo();
  }

  // --- Target colour space (PRD I5) ---------------------------------------
  if (ImGui::BeginCombo("Colour space", exportTargetSpaceName(request.targetSpace))) {
    for (int i = 0; i < 3; ++i) {
      const auto s = static_cast<ExportTargetSpace>(i);
      if (ImGui::Selectable(exportTargetSpaceName(s), s == request.targetSpace))
        request.targetSpace = s;
    }
    ImGui::EndCombo();
  }

  // --- Bit depth (PRD B6, I5) ---------------------------------------------
  const std::vector<ExportBitDepth> depths = offerableExportDepths(request.format);
  if (ImGui::BeginCombo("Bit depth", exportBitDepthName(request.bitDepth))) {
    for (ExportBitDepth d : depths) {
      if (ImGui::Selectable(exportBitDepthName(d), d == request.bitDepth)) request.bitDepth = d;
    }
    ImGui::EndCombo();
  }

  // --- Resize (PRD I15's "and resize") ------------------------------------
  ImGui::Separator();
  if (ImGui::BeginCombo("Resize", exportResizeModeName(request.resize.mode))) {
    for (std::size_t i = 0; i < kExportResizeModeCount; ++i) {
      const auto m = static_cast<ExportResizeMode>(i);
      if (ImGui::Selectable(exportResizeModeName(m), m == request.resize.mode))
        request.resize.mode = m;
    }
    ImGui::EndCombo();
  }
  if (request.resize.mode == ExportResizeMode::Percent) {
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Percent", &request.resize.percent, 1.0f, 100.0f, "%.1f%%");
  } else if (request.resize.mode == ExportResizeMode::FitWithin) {
    int box[2] = {static_cast<int>(request.resize.maxWidth),
                  static_cast<int>(request.resize.maxHeight)};
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::InputInt2("Fit within (px)", box)) {
      request.resize.maxWidth = static_cast<uint32_t>(box[0] > 0 ? box[0] : 0);
      request.resize.maxHeight = static_cast<uint32_t>(box[1] > 0 ? box[1] : 0);
    }
    ImGui::TextDisabled("Aspect preserved; a smaller document is never enlarged.");
  }

  // --- Validation, in io/Export's own words -------------------------------
  const ExportValidation validation = validateExportRequest(
      request, srcW, srcH, activeDoc ? &activeDoc->document.workingSpace : nullptr, nullptr);
  ImGui::Separator();
  if (validation.ok) {
    ImGui::Text("Output: %ux%u", validation.outWidth, validation.outHeight);
    for (const std::string& w : validation.warnings) {
      ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
      ImGui::TextWrapped("! %s", w.c_str());
      ImGui::PopStyleColor();
    }
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, kError);
    ImGui::TextWrapped("%s", validation.error.c_str());
    ImGui::PopStyleColor();
  }

  // --- Presets (PRD I15's "with saveable presets") ------------------------
  ImGui::Separator();
  ImGui::TextUnformatted("Presets");
  if (ImGui::BeginCombo("##presets", "Load a preset...")) {
    if (presets.presets().empty()) ImGui::TextDisabled("(none saved yet)");
    for (const ExportPreset& p : presets.presets()) {
      // A preset this build cannot honour is shown, greyed, with the reason
      // -- never silently dropped and never silently substituted. See
      // io/ExportAs.hpp: this is exactly what an NP_USE_OIIO=ON preset looks
      // like in an OFF build.
      const std::string why = exportRequestAvailability(p.request);
      if (!why.empty()) ImGui::BeginDisabled();
      if (ImGui::Selectable(p.name.c_str()) && why.empty()) {
        request = p.request;
        std::snprintf(presetNameBuf, sizeof(presetNameBuf), "%s", p.name.c_str());
        status = "Loaded preset '" + p.name + "'.";
      }
      if (!why.empty()) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("%s", why.c_str());
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SetNextItemWidth(220.0f);
  ImGui::InputText("##presetName", presetNameBuf, sizeof(presetNameBuf));
  ImGui::SameLine();
  if (ImGui::Button("Save preset")) {
    ExportPreset p;
    p.name = presetNameBuf;
    p.request = request;
    std::string err;
    if (!presets.savePreset(p, &err)) {
      status = err;
    } else if (!presets.saveToFile(presetsPath, &err)) {
      status = err;
    } else {
      status = "Saved preset '" + p.name + "' to " + presetsPath;
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Delete preset")) {
    std::string err;
    if (!presets.removePreset(presetNameBuf)) {
      status = std::string("No preset named '") + presetNameBuf + "' to delete.";
    } else if (!presets.saveToFile(presetsPath, &err)) {
      status = err;
    } else {
      status = std::string("Deleted preset '") + presetNameBuf + "'.";
    }
  }
  if (!status.empty()) ImGui::TextWrapped("%s", status.c_str());
  ImGui::TextDisabled("Presets file: %s", presetsPath.c_str());

  // --- Export, and the honest part ----------------------------------------
  ImGui::Separator();
  ImGui::SetNextItemWidth(360.0f);
  ImGui::InputText("Output file", exportPathBuf, sizeof(exportPathBuf));
  const bool canExport = activeDoc != nullptr && validation.ok && exportPathBuf[0] != '\0';
  if (!canExport) ImGui::BeginDisabled();
  if (ImGui::Button("Export...")) {
    std::string err;
    if (exportDocumentWithRequestToFile(activeDoc->document, exportPathBuf, request, &err))
      status = std::string("Exported ") + exportPathBuf;
    else
      status = err;
  }
  if (!canExport) ImGui::EndDisabled();
  ImGui::SameLine();
  if (!activeDoc) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("No document is open, so there is nothing to export. Use File > New "
                       "Document or File > Open... -- the painting canvas is a solver texture, "
                       "not a document, and still has no bridge into one.");
    ImGui::PopStyleColor();
  } else {
    ImGui::TextDisabled("Exports the open document, not the painting canvas.");
  }
  if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

// ------------------------------------------------------- Document lifecycle
//
// PLAN.md Phase 4 step 8 / PRD I18 ("Revert, duplicate document, save a copy,
// save incremental, open recent"). Everything with a decision in it lives in
// app/DocumentLifecycle.hpp and is exercised headlessly by --selftest; this is
// the widgets, the same split step 7 and app/CurveEdit already use.
//
// **There is no native file picker in this codebase**, and adding one is a
// platform-integration job (NSOpenPanel behind an interface) rather than part
// of a lifecycle step. So Open, Save As and Save a Copy take a typed path in a
// small modal. That is deliberately spartan rather than pretending: the
// operations underneath are the real ones, and swapping the text field for a
// panel later changes this function and nothing else.
//
// Which state lives where follows app/AppState.hpp's rule: the session and
// the recent list are on AppState; the text buffer, the pending action and
// the last status line are function-local, because they are widget state.
enum class DocPathAction { None, Open, SaveAs, SaveCopy };

bool g_docPathRequested = false;
DocPathAction g_docPathAction = DocPathAction::None;
bool g_revertConfirmRequested = false;
std::string g_docStatus;

void applyDocumentPathAction(AppState& st, DocPathAction action, const std::string& path) {
  OpenDocument* doc = st.documents.active();
  DocumentOpResult r;
  switch (action) {
    case DocPathAction::Open: {
      OpenDocument opened;
      r = openNpaintDocument(path, &opened, &st.recentDocuments);
      if (r.ok) st.documents.add(std::move(opened));
      break;
    }
    case DocPathAction::SaveAs:
      if (!doc) return;
      r = saveDocumentAs(*doc, path, {}, &st.recentDocuments);
      break;
    case DocPathAction::SaveCopy:
      if (!doc) return;
      // No RecentDocuments argument exists on this one -- see
      // app/DocumentLifecycle.hpp on why a copy is not an open document.
      r = saveDocumentCopy(*doc, path);
      break;
    case DocPathAction::None:
      return;
  }
  if (r.ok) {
    std::string saveErr;
    st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
    g_docStatus = "OK: " + r.path;
    for (const std::string& w : r.warnings) g_docStatus += "\n! " + w;
  } else {
    g_docStatus = r.error;
  }
}

void drawDocumentDialogs(AppState& st) {
  static char pathBuf[512] = "";

  if (g_docPathRequested) {
    g_docPathRequested = false;
    g_docStatus.clear();
    // Pre-fill with the active document's own path, so Save As on an already
    // saved document starts from its name rather than from nothing.
    if (const OpenDocument* d = st.documents.active())
      std::snprintf(pathBuf, sizeof(pathBuf), "%s", d->path.c_str());
    ImGui::OpenPopup("Document path");
  }
  if (ImGui::BeginPopupModal("Document path", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    const char* verb = g_docPathAction == DocPathAction::Open      ? "Open"
                       : g_docPathAction == DocPathAction::SaveAs  ? "Save As"
                                                                   : "Save a Copy";
    ImGui::Text("%s", verb);
    if (g_docPathAction == DocPathAction::SaveCopy)
      ImGui::TextDisabled("Writes elsewhere; this document stays bound to its own file.");
    ImGui::SetNextItemWidth(480.0f);
    ImGui::InputText("Path", pathBuf, sizeof(pathBuf));
    if (ImGui::Button(verb)) {
      applyDocumentPathAction(st, g_docPathAction, pathBuf);
      if (g_docStatus.rfind("OK: ", 0) == 0) ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    if (!g_docStatus.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
      ImGui::TextWrapped("%s", g_docStatus.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::EndPopup();
  }

  // Revert's confirmation is the refusal itself: revertDocument() is called
  // without the discard flag, and its own PRD I11 message -- which names the
  // edits -- is what the dialog shows. There is no second sentence here to
  // drift from it.
  if (g_revertConfirmRequested) {
    g_revertConfirmRequested = false;
    g_docStatus.clear();
    ImGui::OpenPopup("Revert");
  }
  if (ImGui::BeginPopupModal("Revert", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    OpenDocument* doc = st.documents.active();
    if (!doc) {
      ImGui::CloseCurrentPopup();
    } else {
      // Attempted, not probed: a clean document has nothing to confirm, so
      // the unconfirmed call simply succeeds and the popup closes. A dirty
      // one refuses *before* touching anything (see revertDocument()'s order
      // of checks), so calling it every frame the popup is open is free.
      const DocumentOpResult attempt = revertDocument(*doc, {});
      if (attempt.ok) {
        g_docStatus = "Reverted " + attempt.path;
        ImGui::CloseCurrentPopup();
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.78f, 0.35f, 1.0f));
        ImGui::TextWrapped("%s", attempt.error.c_str());
        ImGui::PopStyleColor();
        if (ImGui::Button("Discard and revert")) {
          const DocumentOpResult done = revertDocument(*doc, {true});
          g_docStatus = done.ok ? "Reverted " + done.path : done.error;
          ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
}

// ------------------------------------------------------------ Recovery offer
//
// PLAN.md Phase 4 step 9 / PRD O8: "Unclean scratch directories are offered
// for recovery on launch, named and dated; never opened silently, never
// auto-deleted." The list is discovered by main() before the journal's own
// session begins and lives on AppState; this is the offer.
//
// Three things the widgets here must not get wrong, each of which is a
// property of app/Journal rather than of this dialog, and none of which this
// dialog is allowed to work around:
//
//  * **Nothing opens by itself.** The modal appears; a document arrives in
//    the session only when its own Recover button is pressed.
//  * **Declining deletes nothing.** "Later" closes the dialog and leaves
//    every byte where it was, so the same offer is made next launch. Only
//    the explicitly labelled Discard button removes anything, and it says
//    what it is about to remove.
//  * **A damaged entry is shown, not hidden.** An entry whose model file is
//    truncated is listed with app/Journal's own sentence and no Recover
//    button, because the alternative -- omitting it -- would look identical
//    to work that was never journalled at all.
bool g_recoveryRequested = false;
std::string g_recoveryStatus;

void drawRecoveryDialog(AppState& st) {
  // Opened once, on the first frame after launch, when there is something to
  // offer. `recoveryOfferPending` is cleared as the popup is opened rather
  // than when it closes, so declining is not re-asked every frame.
  if (st.recoveryOfferPending && !st.recovery.empty()) {
    st.recoveryOfferPending = false;
    g_recoveryRequested = true;
  }
  if (g_recoveryRequested) {
    g_recoveryRequested = false;
    g_recoveryStatus.clear();
    ImGui::OpenPopup("Recover Documents");
  }
  if (!ImGui::BeginPopupModal("Recover Documents", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    return;

  const ImVec4 kWarn(0.92f, 0.78f, 0.35f, 1.0f);
  if (st.recovery.empty()) {
    ImGui::TextDisabled("No unfinished sessions were found.");
  } else {
    ImGui::TextWrapped("These sessions ended without shutting down. Nothing has been opened "
                       "or deleted.");
  }
  if (!journalAvailable()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    ImGui::TextWrapped("%s", journalUnavailableReason().c_str());
    ImGui::PopStyleColor();
  }
  ImGui::Separator();

  for (size_t s = 0; s < st.recovery.size(); ++s) {
    RecoverySession& session = st.recovery[s];
    ImGui::PushID(static_cast<int>(s));
    // PRD O8's "named and dated": the date the session *started*, and the
    // documents' own names.
    ImGui::Text("Session of %s  --  %zu document(s)", session.startedAtLocal.c_str(),
                session.documents.size());
    ImGui::TextDisabled("%s", session.directory.c_str());
    for (const std::string& p : session.problems) {
      ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
      ImGui::TextWrapped("%s", p.c_str());
      ImGui::PopStyleColor();
    }
    for (size_t d = 0; d < session.documents.size(); ++d) {
      const RecoveryDocument& entry = session.documents[d];
      ImGui::PushID(static_cast<int>(d));
      ImGui::Bullet();
      ImGui::SameLine();
      ImGui::Text("%s", entry.displayName.c_str());
      if (!entry.unsavedSummary.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", entry.unsavedSummary.c_str());
      }
      if (entry.intact) {
        ImGui::SameLine();
        if (ImGui::Button("Recover")) {
          OpenDocument recovered;
          const DocumentOpResult r = recoverDocument(entry, &recovered);
          if (r.ok) {
            g_recoveryStatus = "Recovered " + documentDisplayName(recovered) +
                               ". It is unsaved -- save it where you want it kept.";
            st.documents.add(std::move(recovered));
          } else {
            g_recoveryStatus = r.error;
          }
        }
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
        ImGui::TextWrapped("%s", entry.problem.c_str());
        ImGui::PopStyleColor();
      }
      ImGui::PopID();
    }
    if (ImGui::Button("Discard this session")) {
      std::string discardError;
      if (discardRecoverySession(session, &discardError)) {
        g_recoveryStatus = "Discarded " + session.directory;
        st.recovery.erase(st.recovery.begin() + static_cast<std::ptrdiff_t>(s));
        ImGui::PopID();
        break;
      }
      g_recoveryStatus = discardError;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(deletes the files above; there is no undo)");
    ImGui::Separator();
    ImGui::PopID();
  }

  if (!g_recoveryStatus.empty()) ImGui::TextWrapped("%s", g_recoveryStatus.c_str());
  if (ImGui::Button("Later")) ImGui::CloseCurrentPopup();
  ImGui::SameLine();
  ImGui::TextDisabled("Nothing is deleted; this is offered again next launch.");
  ImGui::EndPopup();
}

// The File menu's document half. Split out of drawUI's menu bar block only
// because it is long, not because it is separable.
void drawDocumentMenuItems(AppState& st, uint32_t canvasW, uint32_t canvasH) {
  // Lazy, first time the File menu is opened: PRD A2 and ADR-0001 both say a
  // file nobody asked for costs nothing, and --selftest's idle-RSS assertion
  // is sampled before this can run.
  if (!st.recentDocumentsLoaded) {
    st.recentDocumentsLoaded = true;
    st.recentDocuments.loadFromFile(defaultRecentDocumentsPath());
  }

  OpenDocument* doc = st.documents.active();
  const bool hasDoc = doc != nullptr;
  const bool hasPath = hasDoc && doc->hasPath();

  if (ImGui::MenuItem("New Document")) {
    st.documents.add(makeBlankOpenDocument(static_cast<int32_t>(canvasW),
                                           static_cast<int32_t>(canvasH), WorkingSpace{}));
    g_docStatus.clear();
  }
  if (ImGui::MenuItem("Open...")) {
    g_docPathAction = DocPathAction::Open;
    g_docPathRequested = true;
  }
  if (ImGui::BeginMenu("Open Recent", !st.recentDocuments.entries().empty())) {
    const std::vector<RecentDocument>& entries = st.recentDocuments.entries();
    for (size_t i = 0; i < entries.size(); ++i) {
      // A missing entry is shown, greyed, with the reason -- never dropped
      // behind the user's back. app/DocumentLifecycle.hpp argues why.
      std::string why;
      const bool missing = recentDocumentMissing(entries[i].path, &why);
      if (missing) ImGui::BeginDisabled();
      if (ImGui::MenuItem(entries[i].displayName.c_str()) && !missing) {
        OpenDocument opened;
        const DocumentOpResult r = openRecentDocument(st.recentDocuments, i, &opened);
        if (r.ok) {
          st.documents.add(std::move(opened));
          std::string saveErr;
          st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
        }
        g_docStatus = r.ok ? "Opened " + r.path : r.error;
      }
      if (missing) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("%s", why.c_str());
      } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", entries[i].path.c_str());
      }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Clear Menu")) {
      st.recentDocuments.clear();
      std::string saveErr;
      st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
    }
    ImGui::EndMenu();
  }

  ImGui::Separator();
  if (ImGui::MenuItem("Save", nullptr, false, hasPath)) {
    const DocumentOpResult r = saveDocument(*doc, {}, &st.recentDocuments);
    if (r.ok) {
      std::string saveErr;
      st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
    }
    g_docStatus = r.ok ? "Saved " + r.path : r.error;
  }
  if (ImGui::MenuItem("Save As...", nullptr, false, hasDoc)) {
    g_docPathAction = DocPathAction::SaveAs;
    g_docPathRequested = true;
  }
  if (ImGui::MenuItem("Save a Copy...", nullptr, false, hasDoc)) {
    g_docPathAction = DocPathAction::SaveCopy;
    g_docPathRequested = true;
  }
  if (ImGui::MenuItem("Save Incremental", nullptr, false, hasPath)) {
    const DocumentOpResult r = saveDocumentIncremental(*doc, {}, &st.recentDocuments);
    if (r.ok) {
      std::string saveErr;
      st.recentDocuments.saveToFile(defaultRecentDocumentsPath(), &saveErr);
    }
    g_docStatus = r.ok ? "Saved " + r.path : r.error;
  }

  ImGui::Separator();
  // PLAN.md Phase 4 step 9 / PRD O8. Always enabled, even with nothing to
  // offer: "are there unfinished sessions?" is a question a user who has just
  // had a crash will ask, and a greyed-out item answers it ambiguously. The
  // list is re-scanned rather than reused, so a session another instance
  // finished since launch is not offered from a stale snapshot.
  if (ImGui::MenuItem("Recover Documents...")) {
    st.recovery = discoverRecoverySessions();
    g_recoveryRequested = true;
  }

  ImGui::Separator();
  if (ImGui::MenuItem("Revert", nullptr, false, hasPath)) g_revertConfirmRequested = true;
  if (ImGui::MenuItem("Duplicate Document", nullptr, false, hasDoc)) {
    st.documents.add(duplicateDocument(*doc));
    g_docStatus = "Duplicated (unbound -- Save As gives it a file of its own).";
  }
  if (ImGui::MenuItem("Close Document", nullptr, false, hasDoc)) {
    std::string err;
    // Refuses a dirty document by name. The menu is not the place to offer a
    // discard, so the message says what to do instead.
    if (!st.documents.close(st.documents.activeIndex(), false, &err)) g_docStatus = err;
  }
}

}  // namespace

void drawUI(AppState& st, std::unique_ptr<PaintSim>& sim, GpuContext& gpu,
           const MixboxLut& lut, uint32_t canvasW, uint32_t canvasH) {
  const ImGuiViewport* vp = ImGui::GetMainViewport();

  // ------------------------------------------------------------ menu bar
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      // "New" is the *canvas* command it has always been -- it clears the
      // solver texture. "New Document" below is a different thing entirely,
      // and the two are deliberately not merged: the canvas is not a
      // document in this build, and a menu that implied otherwise would be
      // the first place the gap got papered over.
      if (ImGui::MenuItem("New Canvas", "Cmd+N")) st.requestClear = true;
      ImGui::Separator();
      // PLAN.md Phase 4 step 8 / PRD I18.
      drawDocumentMenuItems(st, canvasW, canvasH);
      ImGui::Separator();
      // PLAN.md Phase 4 step 7 / PRD I15. No shortcut string: docs/shortcuts.md
      // has not assigned one and keymaps/default.json binds no action for it,
      // so advertising a chord that resolves to nothing would be worse than
      // menu-only -- the same call the Rulers item below already makes.
      // Sets a flag rather than calling ImGui::OpenPopup() here: a modal
      // opened from inside BeginMenu() would be opened against the menu's own
      // ID stack, and drawExportAsDialog() below runs outside it.
      if (ImGui::MenuItem("Export As...")) g_exportAsRequested = true;
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
      ImGui::Separator();
      // The open documents, and which one the lifecycle commands act on.
      // Phase 5 step 14's tab strip is this list drawn differently, not a
      // different list (app/DocumentLifecycle.hpp).
      if (st.documents.empty()) {
        ImGui::TextDisabled("(no documents open)");
      } else {
        for (size_t i = 0; i < st.documents.count(); ++i) {
          const OpenDocument* d = st.documents.at(i);
          const std::string label =
              documentDisplayName(*d) + (d->isDirty() ? " *" : "") + "##doc" + std::to_string(i);
          if (ImGui::MenuItem(label.c_str(), nullptr, i == st.documents.activeIndex()))
            st.documents.setActive(i);
          if (ImGui::IsItemHovered() && d->isDirty())
            ImGui::SetTooltip("%s", d->unsavedWorkSummary().c_str());
        }
      }
      ImGui::EndMenu();
    }

    // The active document, always visible rather than only inside a dialog:
    // every lifecycle command in the File menu acts on this one, and a
    // "Save" whose target is not on screen is how the wrong file gets
    // overwritten. The `*` is the dirty marker.
    if (const OpenDocument* activeForBar = st.documents.active()) {
      ImGui::TextDisabled("| %s%s", documentDisplayName(*activeForBar).c_str(),
                          activeForBar->isDirty() ? " *" : "");
    }
    if (!g_docStatus.empty()) {
      const size_t firstLine = g_docStatus.find('\n');
      const std::string oneLine =
          firstLine == std::string::npos ? g_docStatus : g_docStatus.substr(0, firstLine);
      ImGui::TextDisabled("| %s", oneLine.c_str());
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g_docStatus.c_str());
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

  // PLAN.md Phase 4 step 7 ("Export As"), defined out here for the same
  // reason the Add Guide popup above is: a popup opened from a menu item has
  // to be begun outside the menu bar's ID stack.
  drawExportAsDialog(st, canvasW, canvasH);

  // PLAN.md Phase 4 step 8 ("Document lifecycle"), out here for the same
  // reason: a modal opened from a menu item must be begun outside the menu
  // bar's ID stack.
  drawDocumentDialogs(st);

  // PLAN.md Phase 4 step 9 ("the recovery journal"), same placement rule
  // again -- and it must be begun here rather than only from the menu,
  // because PRD O8's offer happens on launch, before any menu is touched.
  drawRecoveryDialog(st);

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

    // PLAN.md Phase 3 step 8 ("Op-stack UI -- reorder, toggle, delete, and
    // a curve widget operating in the shaper domain"), the last step of
    // Phase 3. Same TextUnformatted/Separator/Dummy idiom as every section
    // above -- see drawGradeSection()'s own doc comment for the full
    // breakdown of what's here and what's deliberately left out.
    // PLAN.md Phase 5 step 1 ("Multiple layers in `Document`, with reorder,
    // visibility, lock, opacity"; PRD C4). docs/ui.md's layout puts LAYERS in
    // this right-hand stack, above CHANNELS -- same section idiom as every
    // block above. See drawLayersSection()'s own doc comment for what it does
    // not show, which is the painting canvas.
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextUnformatted("LAYERS");
    ImGui::Separator();
    drawLayersSection(st);

    // PLAN.md Phase 5 step 8 ("History panel ...", PRD O2/O3). docs/ui.md §5:
    // "The History panel (PRD O2) joins the right-hand docked column." Below
    // LAYERS, because a history row names an edit made to the stack above it.
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextUnformatted("HISTORY");
    ImGui::Separator();
    drawHistorySection(st);

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextUnformatted("GRADE");
    ImGui::Separator();
    drawGradeSection(st);
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

    // --- The open document, over the paper (UI detour step 2) -------------
    //
    // The same quad, drawn second, so the document composites over whatever
    // the paper is -- PaintSim's canvas when a stroke has constructed one,
    // the flat blank sheet when nothing has. **A new document is fully
    // transparent**, so this call changes nothing about the picture until a
    // layer holds content; that is this step's regression boundary and it is
    // checkable in a screenshot.
    //
    // ui/DocumentTexture.hpp owns the argument for all three of the
    // decisions behind this one line -- RGBA16Float rather than 8-bit,
    // straight alpha rather than premultiplied because of ImGui's global
    // blend state, and the revision cache that keeps an unchanged frame free.
    //
    // The document is drawn on the *canvas* quad, so a document whose own
    // dimensions differ from kCanvasW/kCanvasH is stretched onto the paper
    // rather than placed at its own size. That is right for today -- the
    // document a session starts with is exactly canvas-sized (see main.cpp),
    // and the two pictures are stacked precisely because they are not yet one
    // thing -- and it stops being a question at all once the stroke bridge
    // makes the document the canvas.
    //
    // No warnings are collected: `compositeDocumentPremultiplied()` would
    // report an unimplemented blend once per layer per upload, and the layers
    // panel already marks exactly those layers `(!)` on their own rows, which
    // is the same fact in the place a user can act on it.
    if (const OpenDocument* activeDocument = st.documents.active()) {
      if (const WGPUTextureView documentView = g_documentTexture.viewFor(gpu, *activeDocument))
        dl->AddImageQuad((ImTextureID)(intptr_t)documentView, q00, q10, q11, q01);
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

const DocumentTexture& canvasDocumentTexture() { return g_documentTexture; }

}  // namespace np
