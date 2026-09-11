// A tile's on-screen quad. Split out of ui/NaturalPaintUI.hpp so a caller that
// only needs this shape need not pull that header's AppState.hpp cone.
#pragma once

#include "imgui.h"

namespace np {

// A tile's on-screen quad, in the same screen-pixel space MacPaintUI's canvas
// block computes `origin`/`drawSize` in (MacPaintUI.cpp ~line 478-482).
struct TileScreenRect {
  ImVec2 min;
  ImVec2 max;
};

}  // namespace np
