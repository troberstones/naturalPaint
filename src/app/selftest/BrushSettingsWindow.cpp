#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <type_traits>

#include "brush/BrushModel.hpp"
#include "brush/BrushModelFields.hpp"
#include "brush/ToolOptionsBlend.hpp"
#include "ui/BrushPanelLayout.hpp"

namespace np {

// ---------------------------------------------------------------------------
// ui/BrushPanelLayout -- the Brush Settings list and pages, as the window draws them.
//
// `--selftest` cannot open a window (reachability-audit F4), so what can be
// wrong without anyone noticing is asserted on the tables: a row out of order
// in the panel table draws one panel's switch beside another's name; a row
// naming a float as a checkbox draws nothing, because the window looks leaves
// up by type and a miss is silent.
// ---------------------------------------------------------------------------
bool runBrushSettingsWindowTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // The C++ type of every BrushModel leaf, by path.
  std::map<std::string, std::string> typeOf;
  {
    BrushModel model;
    visitBrushModelFields(model, [&](const std::string& path, auto& leaf) {
      using T = std::decay_t<decltype(leaf)>;
      if constexpr (std::is_same_v<T, bool>) typeOf[path] = "bool";
      else if constexpr (std::is_same_v<T, int32_t>) typeOf[path] = "int";
      else if constexpr (std::is_same_v<T, float>) typeOf[path] = "float";
      else if constexpr (std::is_same_v<T, std::string>) typeOf[path] = "string";
      else if constexpr (std::is_same_v<T, VarianceControl>) typeOf[path] = "control";
      else if constexpr (std::is_same_v<T, CoverageBlend>) typeOf[path] = "blend";
      else typeOf[path] = "other";
    });
  }
  auto isType = [&](const std::string& path, const char* type) {
    const auto it = typeOf.find(path);
    return it != typeOf.end() && it->second == type;
  };

  std::printf("  -- A. the list: every panel has its own row, in Photoshop's order --\n");
  {
    check(kBrushPanelCount == 18,
          "panels: Photoshop's thirteen, Tool Options, and naturalPaint's four");
    bool ownId = true, named = true;
    std::set<std::string> names, labels;
    for (size_t i = 0; i < kBrushPanelCount; ++i) {
      const auto panel = static_cast<BrushPanel>(i);
      const BrushPanelSpec& spec = brushPanelSpec(panel);
      if (spec.panel != panel) ownId = false;
      const std::string name = brushPanelName(panel);
      if (name.empty() || name == "UNNAMED") named = false;
      names.insert(name);
      if (spec.label[0] != '\0') labels.insert(spec.label);
    }
    check(ownId, "panels: each row's id equals its index");
    check(named && names.size() == kBrushPanelCount, "panels: every enumerator has its own spelling");
    check(labels.size() == kBrushPanelCount, "panels: eighteen distinct, non-empty labels");

    const std::vector<std::string> photoshop = {
        "Brush Tip Shape", "Shape Dynamics", "Scattering", "Texture",   "Dual Brush",
        "Color Dynamics",  "Transfer",       "Brush Pose", "Noise",     "Wet Edges",
        "Build-up",        "Smoothing",      "Protect Texture"};
    std::vector<std::string> listed;
    bool groupsInOrder = true;
    for (size_t i = 0; i < kBrushPanelCount; ++i) {
      const BrushPanelSpec& spec = brushPanelSpec(static_cast<BrushPanel>(i));
      if (spec.group == BrushPanelGroup::Photoshop) listed.push_back(spec.label);
      if (i > 0 && spec.group < brushPanelSpec(static_cast<BrushPanel>(i - 1)).group)
        groupsInOrder = false;
    }
    check(listed == photoshop, "panels: the Photoshop group is Photoshop's list, in its order");
    check(groupsInOrder, "panels: Photoshop, then Tool Preset, then naturalPaint, never interleaved");

    bool switchesAreBools = true;
    for (size_t i = 0; i < kBrushPanelCount; ++i) {
      const BrushPanelSpec& spec = brushPanelSpec(static_cast<BrushPanel>(i));
      if (spec.enablePath[0] != '\0' && !isType(spec.enablePath, "bool")) switchesAreBools = false;
    }
    check(switchesAreBools, "panels: every list checkbox writes a bool field");
  }

  std::printf("  -- B. the pages: every row edits a field of the type it draws --\n");
  {
    size_t wrongType = 0;
    std::string firstWrong;
    auto expect = [&](const std::string& path, const char* type) {
      if (!isType(path, type) && wrongType++ == 0) firstWrong = path;
    };
    bool rowsOnPages = true, labelled = true, enableRulesValid = true;
    std::set<BrushPanel> panelsWithRows;
    std::vector<std::string> tipPickers;
    for (const BrushRowSpec& row : brushPanelRows()) {
      const BrushPanelSpec& spec = brushPanelSpec(row.panel);
      if (!spec.hasPage || spec.group == BrushPanelGroup::NaturalPaint) rowsOnPages = false;
      panelsWithRows.insert(row.panel);
      if (row.kind != BrushRowKind::Separator && row.label[0] == '\0') labelled = false;
      const std::string path = row.path;
      switch (row.kind) {
        case BrushRowKind::Separator:
          if (!path.empty()) rowsOnPages = false;
          break;
        case BrushRowKind::Check: expect(path, "bool"); break;
        case BrushRowKind::Slider: expect(path, "float"); break;
        case BrushRowKind::IntSlider: expect(path, "int"); break;
        case BrushRowKind::Blend: expect(path, "blend"); break;
        case BrushRowKind::TipPicker:
          expect(path, "string");
          tipPickers.push_back(path);
          break;
        case BrushRowKind::ToolBlend: expect(path, "string"); break;
        case BrushRowKind::Pattern:
          expect(path + ".id", "string");
          expect(path + ".name", "string");
          break;
        case BrushRowKind::Jitter:
          expect(path + ".control", "control");
          expect(path + ".jitter", "float");
          expect(path + ".fadeSteps", "int");
          if (row.minimumLabel != nullptr) expect(path + ".minimum", "float");
          break;
      }
      const std::string enablePath = row.enablePath;
      switch (row.enable) {
        case BrushRowEnable::Always:
        case BrushRowEnable::ProceduralTip:
          if (!enablePath.empty()) enableRulesValid = false;
          break;
        case BrushRowEnable::ControlIsPenTilt:
          if (!isType(enablePath + ".control", "control")) enableRulesValid = false;
          break;
        case BrushRowEnable::BoolOn:
          if (!isType(enablePath, "bool")) enableRulesValid = false;
          break;
      }
    }
    if (wrongType > 0)
      std::printf("  [measured] %zu row leaf/leaves of the wrong type, first: %s\n", wrongType,
                  firstWrong.c_str());
    check(wrongType == 0, "rows: each names a field of the type its control writes");
    check(rowsOnPages, "rows: only on Photoshop and Tool Options pages that have a page");
    check(labelled, "rows: every control has a label");
    check(enableRulesValid, "rows: every greying rule names a field of the type it tests");

    bool everyPageHasRows = true;
    for (size_t i = 0; i < kBrushPanelCount; ++i) {
      const auto panel = static_cast<BrushPanel>(i);
      const BrushPanelSpec& spec = brushPanelSpec(panel);
      const bool modelPage = spec.hasPage && spec.group != BrushPanelGroup::NaturalPaint;
      if (modelPage && panelsWithRows.count(panel) == 0) everyPageHasRows = false;
    }
    check(everyPageHasRows, "rows: no Photoshop page opens empty");

    const std::vector<std::string> wantPickers = {"tip.dab.id", "dual.tip.dab.id"};
    check(tipPickers == wantPickers,
          "rows: a tip grid for the brush's own tip and one for the Dual Brush's second tip");

    size_t live = 0;
    bool onlyPattern = true;
    for (const BrushRowSpec& row : brushPanelRows()) {
      if (!brushRowLiveWhilePanelOff(row)) continue;
      ++live;
      if (row.kind != BrushRowKind::Pattern) onlyPattern = false;
    }
    check(live == 1 && onlyPattern,
          "rows: only the pattern grid stays live while its panel is off -- picking turns it on");
  }

  std::printf("  -- C. opening a page does not edit the brush --\n");
  {
    float stored = 0.36f;
    const float before = stored;
    const bool wrote = brushRowStore(brushRowShown(stored, 100.0f), 100.0f, stored);
    check(!wrote && std::memcmp(&stored, &before, sizeof(float)) == 0,
          "brushRowStore: the value it was shown writes nothing back");
    const bool moved = brushRowStore(37.0f, 100.0f, stored);
    check(moved && std::fabs(stored - 0.37f) < 1e-6f, "brushRowStore: a moved value is stored");
  }

  std::printf("  -- D. the menus offer Photoshop's lists --\n");
  {
    using C = VarianceControl;
    auto has = [](const std::vector<C>& list, C c) {
      for (C x : list)
        if (x == c) return true;
      return false;
    };
    const auto& standard = brushControlChoices(BrushControlSet::Standard);
    const auto& direction = brushControlChoices(BrushControlSet::WithDirection);
    const auto& noRotation = brushControlChoices(BrushControlSet::NoRotation);
    check(standard.size() == 6 && standard.front() == C::Off && !has(standard, C::Direction),
          "controls: Standard starts at Off and has no Direction");
    check(direction.size() == 8 && has(direction, C::Direction) &&
              has(direction, C::InitialDirection),
          "controls: Angle Jitter's list adds Initial Direction and Direction");
    check(noRotation.size() == 5 && !has(noRotation, C::Rotation),
          "controls: Depth Jitter's list has no Rotation");

    using B = CoverageBlend;
    const auto& texture = brushBlendChoices(false);
    const auto& dual = brushBlendChoices(true);
    std::set<B> textureSet(texture.begin(), texture.end());
    std::set<B> dualSet(dual.begin(), dual.end());
    check(texture.size() == 10 && textureSet.size() == 10, "blends: Texture offers all ten modes once");
    check(dual.size() == 8 && dualSet.size() == 8 && dualSet.count(B::Height) == 0 &&
              dualSet.count(B::Subtract) == 0,
          "blends: Dual Brush offers eight, without Height and Subtract");

    size_t painted = 0;
    for (const BrushToolBlendChoice& choice : brushToolBlendChoices()) {
      BlendMode mode = BlendMode::Normal;
      if (blendModeFromPsToolOptions(choice.id, mode)) ++painted;
    }
    check(brushToolBlendChoices().size() == 5 && painted == 3,
          "tool blends: five ids offered, Normal/Darken/Multiply painted");
  }

  std::printf("[selftest] brush settings window %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
