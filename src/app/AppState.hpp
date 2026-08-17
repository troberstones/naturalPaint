#pragma once
#include <cstdint>

#include "paint/Palette.hpp"
#include "sim/PaintSim.hpp"

namespace np {

// Only tools that actually do something are listed. Lasso, marquee and text
// belong to the MacPaint chrome but have no meaning yet for a fluid canvas, so
// they are deliberately absent rather than present and dead.
enum class Tool {
  Brush,       // water + pigment
  Water,       // pre-wet the paper, no pigment
  DryBrush,    // little water, hard edge, pigment sits on the tooth
  Eyedropper,
  Hand,
  Zoom,
  Count
};

struct BrushState {
  Tool tool = Tool::Brush;
  int pigment = 6;  // Ultramarine Blue
  float radius = 20.0f;
  float load = 0.9f;      // pigment concentration
  float wetness = 1.3f;   // water deposited
  float hardness = 0.35f;
  bool pressureSize = true;
  bool pressureFlow = true;
};

struct CanvasView {
  float zoom = 1.0f;
  float panX = 0.0f;
  float panY = 0.0f;
};

struct AppState {
  PaintMode mode = PaintMode::Watercolor;
  // Seconds a wash keeps moving before it sets. Drives evaporation and
  // absorption together via setWorkingTime(); 15 matches the shipped defaults.
  float workingTime = 15.0f;
  BrushState brush;
  CanvasView view;
  SimParams sim;

  // Stroke, in canvas texel space.
  float lastX = 0.0f, lastY = 0.0f;
  bool strokeActive = false;
  bool strokeStarting = false;

  // SDL3 pen state. penSeen stays false on a mouse-only machine, in which case
  // pressure is pinned to 1.
  bool penSeen = false;
  bool penDown = false;
  float penPressure = 1.0f;

  bool showDemo = false;
  bool paused = false;
  bool requestClear = false;
  bool requestMode = false;
  bool requestReload = false;
  bool quit = false;

  float frameMs = 0.0f;
};

}  // namespace np
