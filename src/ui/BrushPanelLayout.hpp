#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "brush/CoverageBlend.hpp"
#include "brush/Variance.hpp"

// ui/BrushPanelLayout -- what the Brush Settings window shows, as data.
//
// The window is laid out like Photoshop's Brush Settings panel: a list of panels
// down the left, each with its own switch, and one page per panel in Photoshop's
// order and units. `--selftest` cannot open a window (docs/reachability-audit.md
// F4), so the list, every row of every page, and the fields deliberately left
// without a control are tables here. runBrushPanelBindingTest() proves that
// together they account for every `BrushModel` field exactly once, and
// runBrushSettingsWindowTest() that each row names a field of the type it edits.

namespace np {

enum class BrushPanel : uint8_t {
  // Photoshop's own list, in its order.
  TipShape,
  ShapeDynamics,
  Scattering,
  Texture,
  DualBrush,
  ColorDynamics,
  Transfer,
  BrushPose,
  Noise,
  WetEdges,
  BuildUp,
  Smoothing,
  ProtectTexture,
  // What a tool preset saves alongside the brush.
  ToolOptions,
  // naturalPaint's own settings, which no .abr carries.
  Paint,
  PaperGrain,
  TaperStabiliser,
  LinkMatrix,  // listed only under --advanced-dynamics
  Count,
};

inline constexpr size_t kBrushPanelCount = static_cast<size_t>(BrushPanel::Count);

enum class BrushPanelGroup : uint8_t { Photoshop, ToolPreset, NaturalPaint };

struct BrushPanelSpec {
  BrushPanel panel = BrushPanel::TipShape;
  const char* label = "";
  BrushPanelGroup group = BrushPanelGroup::Photoshop;
  // The `BrushModel` bool the list's checkbox writes, or "" for a panel with no
  // switch.
  const char* enablePath = "";
  // False for the entries Photoshop shows as a checkbox and nothing more.
  bool hasPage = true;
  // Why naturalPaint does not paint this panel yet, or nullptr when it does.
  const char* notPainted = nullptr;
};

enum class BrushRowKind : uint8_t {
  Separator,
  Check,      // bool
  Slider,     // float, shown as stored * scale
  IntSlider,  // int32_t
  Jitter,     // a Variance: Jitter, then Control (with Fade Steps), then an optional Minimum
  Blend,      // CoverageBlend
  TipPicker,  // the dab grid; `path` is the tip's `.dab.id`
  Pattern,    // the Texture panel's paper; `path` is `texture.pattern`
  ToolBlend,  // the tool preset's own blend mode id
};

// When a row can be edited. Photoshop greys a control whose setting cannot apply.
enum class BrushRowEnable : uint8_t {
  Always,
  ControlIsPenTilt,  // `enablePath` names a Variance whose Control is Pen Tilt
  BoolOn,            // `enablePath` names a bool that is on
  ProceduralTip,     // the brush stamps no bitmap; Hardness shapes only the round tip
};

// Which Controls a Jitter row offers. Photoshop's list differs by setting.
enum class BrushControlSet : uint8_t {
  Standard,       // Off, Fade, Pen Pressure, Pen Tilt, Stylus Wheel, Rotation
  WithDirection,  // Standard, then Initial Direction and Direction
  NoRotation,     // Off, Fade, Pen Pressure, Pen Tilt, Stylus Wheel
};

struct BrushRowSpec {
  BrushPanel panel = BrushPanel::TipShape;
  BrushRowKind kind = BrushRowKind::Separator;
  const char* path = "";  // a leaf; a Jitter row's Variance; a Pattern row's PatternRef
  const char* label = "";
  float scale = 1.0f;  // Slider and Jitter: shown value = stored value * scale
  float lo = 0.0f;
  float hi = 100.0f;
  const char* fmt = "%.0f%%";
  const char* minimumLabel = nullptr;  // Jitter: Minimum's label, or nullptr for none
  BrushControlSet controls = BrushControlSet::Standard;
  bool dualModes = false;  // Blend: the Dual Brush's shorter list
  BrushRowEnable enable = BrushRowEnable::Always;
  const char* enablePath = "";
  const char* notPainted = nullptr;  // why naturalPaint saves this but does not paint it
};

struct BrushFieldOmission {
  const char* path = "";
  const char* reason = "";
};

const BrushPanelSpec& brushPanelSpec(BrushPanel panel) noexcept;

// The enumerator's spelling, for --brush-settings-demo and test output. Never shown.
const char* brushPanelName(BrushPanel panel) noexcept;

const std::vector<BrushRowSpec>& brushPanelRows();

// Every `BrushModel` leaf that deliberately has no control, and why.
const std::vector<BrushFieldOmission>& brushFieldOmissionTable();

// The `BrushModel` leaves a row edits.
std::vector<std::string> brushRowLeafPaths(const BrushRowSpec& row);

// Photoshop's Control list for a row, and its blend lists, in Photoshop's order.
const std::vector<VarianceControl>& brushControlChoices(BrushControlSet set);
const std::vector<CoverageBlend>& brushBlendChoices(bool dualModes);

struct BrushToolBlendChoice {
  const char* id;     // `Md  ` as the file writes it
  const char* label;
};
const std::vector<BrushToolBlendChoice>& brushToolBlendChoices();

// The shown value for a stored one, and the write back. The write back happens
// only when the shown value moved, so opening a page cannot turn a stored 0.36
// into 0.35999998 and mark the brush edited.
// Whether a row stays usable while its panel's switch is off. Only the pattern
// grid: picking a paper turns Texture on, so greying it would hide the one
// control that does that.
bool brushRowLiveWhilePanelOff(const BrushRowSpec& row) noexcept;

float brushRowShown(float stored, float scale) noexcept;
bool brushRowStore(float shown, float scale, float& stored) noexcept;

}  // namespace np
