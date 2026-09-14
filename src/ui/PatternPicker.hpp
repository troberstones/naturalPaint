#pragma once

#include <string>

// ui/PatternPicker -- the Texture panel's grid of papers.
//
// The same grid as ui/DabPicker -- its layout and hit test, `dabPickerLayoutFor()`
// and `dabPickerCellAt()`, so a click lands on the cell drawn under it -- over
// app/PatternLibrary, with a thumbnail atlas of its own: a paper is an opaque
// height field, not a coverage mask to tint. Cell 0 is None, which leaves the
// brush on its own Paper Grain.

namespace np {

struct AppState;
class GpuContext;
class PatternLibrary;

struct PatternPickerAction {
  bool selected = false;
  std::string id;  // "" for None
  bool rescanRequested = false;
  bool revealRequested = false;
};

PatternPickerAction drawPatternPicker(const char* id, PatternLibrary& patterns, GpuContext& gpu,
                                      const std::string& currentId);

// Lists the pattern folders, and gives saved presets and the current brush back
// their papers. Runs once, and again whenever the brush library has grown: an
// import extracts its papers, and they should be in the grid when it is opened.
void ensurePatternLibraryScanned(AppState& st);

// The grid on the Texture page, applied to the brush. Returns true when the
// brush's pattern changed.
bool drawBrushPatternPicker(AppState& st, GpuContext& gpu);

}  // namespace np
