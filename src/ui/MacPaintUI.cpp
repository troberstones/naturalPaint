#include "ui/MacPaintUI.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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

}  // namespace

void drawUI(AppState& st, PaintSim& sim, GpuContext& gpu) {
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
    if (ImGui::BeginMenu("Window")) {
      ImGui::MenuItem("ImGui demo", nullptr, &st.showDemo);
      ImGui::EndMenu();
    }

    // Right-aligned status, the way the classic apps put the zoom box.
    char status[128];
    std::snprintf(status, sizeof(status), "%s   %.1f fps   %ux%u   %.0f%%",
                  paintModeName(st.mode),
                  st.frameMs > 0.0f ? 1000.0f / st.frameMs : 0.0f, sim.width(),
                  sim.height(), st.view.zoom * 100.0f);
    const float w = ImGui::CalcTextSize(status).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - w - 12.0f);
    ImGui::TextDisabled("%s", status);
    ImGui::EndMainMenuBar();
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
      ImGui::SliderInt("Lattice steps", &sim.inkSubsteps, 1, 20);
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

    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextUnformatted("SOLVER");
    ImGui::Separator();
    if (st.mode == PaintMode::Watercolor)
      ImGui::SliderInt("Jacobi iters", &sim.jacobiIterations, 1, 60);
    ImGui::SliderInt("Substeps", &sim.substeps, 1, 6);
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
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float texW = static_cast<float>(sim.width());
    const float texH = static_cast<float>(sim.height());
    const ImVec2 drawSize(texW * st.view.zoom, texH * st.view.zoom);

    const ImVec2 origin(
        canvasPos.x + std::max(0.0f, (avail.x - drawSize.x) * 0.5f) + st.view.panX,
        canvasPos.y + std::max(0.0f, (avail.y - drawSize.y) * 0.5f) + st.view.panY);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Drop shadow so the sheet reads as paper lying on a dark desk.
    dl->AddRectFilled(ImVec2(origin.x + 6, origin.y + 6),
                      ImVec2(origin.x + drawSize.x + 6, origin.y + drawSize.y + 6),
                      IM_COL32(0, 0, 0, 110));
    dl->AddImage((ImTextureID)(intptr_t)sim.canvasView(), origin,
                 ImVec2(origin.x + drawSize.x, origin.y + drawSize.y));
    dl->AddRect(origin, ImVec2(origin.x + drawSize.x, origin.y + drawSize.y),
                ImGui::GetColorU32(ImGuiCol_Border));

    ImGui::SetCursorScreenPos(canvasPos);
    ImGui::InvisibleButton("##canvasHit", canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float tx = (mouse.x - origin.x) / st.view.zoom;
    const float ty = (mouse.y - origin.y) / st.view.zoom;

    // --- zoom on wheel, anchored under the cursor ---
    if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
      const float old = st.view.zoom;
      st.view.zoom = std::clamp(st.view.zoom * (1.0f + ImGui::GetIO().MouseWheel * 0.12f),
                                0.1f, 8.0f);
      const float k = st.view.zoom / old;
      st.view.panX = (st.view.panX + (origin.x - canvasPos.x)) * k - (origin.x - canvasPos.x);
      st.view.panY = (st.view.panY + (origin.y - canvasPos.y)) * k - (origin.y - canvasPos.y);
    }

    const bool panning =
        ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
        (st.brush.tool == Tool::Hand && ImGui::IsMouseDragging(ImGuiMouseButton_Left));
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
    if (paintTool && down && hovered && inside && !panning) {
      if (!st.strokeActive) {
        st.strokeActive = true;
        st.lastX = tx;
        st.lastY = ty;
        // A fresh stroke recharges the oil brush with the selected paint.
        st.sim.brushReload = 1;
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
      st.sim.brushAx = st.lastX;
      st.sim.brushAy = st.lastY;
      st.sim.brushBx = tx;
      st.sim.brushBy = ty;
      st.sim.brushActive = 1;

      st.lastX = tx;
      st.lastY = ty;
    } else if (!down) {
      st.strokeActive = false;
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

  if (st.requestMode) {
    sim.setMode(gpu, st.mode);
    st.requestMode = false;
  }
  if (st.requestClear) {
    sim.clearCanvas(gpu);
    st.requestClear = false;
  }
  if (st.requestReload) {
    if (sim.reloadShaders(gpu)) std::printf("[shader] reloaded\n");
    st.requestReload = false;
  }
}

}  // namespace np
