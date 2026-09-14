#include "app/selftest/Support.hpp"

#include <utility>

#include "app/ToolSwitch.hpp"
#include "flats/Tool.hpp"

// docs/shortcuts.md §1.1: Y, ⇧K, ⇧U, ⇧B, ⇧V pick a FLATS TOOLS palette cell
// (a sticky FlatsTool), and ⇧Enter accepts every pending gap suggestion in
// one undo step. Three things to prove, none of which the pinned single-gap
// coverage in flats/FlatsSelfTest.cpp touches:
//
//  1. The six chords resolve to the right action ONLY under Flats scope,
//     against the real, shipped keymaps/default.json -- and resolve to
//     nothing outside it, exactly as they did before this track (none of
//     the six were bound to anything, in any scope, until now).
//  2. A keymap action selects the identical FlatsTool the FLATS TOOLS
//     palette cell does, through the identical toggle rule
//     (app/ToolSwitch's toggleFlatsTool() -- ui/MacPaintUI.cpp's
//     flatsToolButton() calls the same function now, so this is not two
//     implementations that happen to agree).
//  3. Accepting all pending gaps really does accept every one of them,
//     through the same per-gap path flatsAcceptSuggestion() already proves
//     in flats/FlatsSelfTest.cpp.
//
// Headless and GPU-free.
namespace np {

bool runFlatsKeysTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- 1. the six chords, in the real shipped keymap -----------------------
  Keymap km;
  const bool loaded = km.loadFromFile("default.json");
  check(loaded, "keymaps/default.json loads");
  if (loaded) {
    check(!km.hasConflicts(),
          "keymaps/default.json has no conflicts -- including the six new Flats bindings");

    struct Row {
      KeyChord chord;
      const char* label;
      const char* action;
    };
    const Row rows[] = {
        {{SDLK_Y, 0}, "Y", "flats_tool_shape_fill"},
        {{SDLK_K, kModShift}, "shift-K", "flats_tool_group"},
        {{SDLK_U, kModShift}, "shift-U", "flats_tool_draw_merge"},
        {{SDLK_B, kModShift}, "shift-B", "flats_tool_bridge_pen"},
        {{SDLK_V, kModShift}, "shift-V", "flats_tool_select_edits"},
        {{SDLK_RETURN, kModShift}, "shift-Return", "flats_accept_all_gaps"},
    };
    for (const Row& row : rows) {
      char msg[112];
      std::snprintf(msg, sizeof(msg), "%s: no global binding (pre-existing, unchanged)", row.label);
      check(km.resolve(row.chord, std::nullopt) == std::nullopt, msg);
      std::snprintf(msg, sizeof(msg), "%s: still nothing outside Flats scope (RGB active)", row.label);
      check(km.resolve(row.chord, LayerKind::RGB) == std::nullopt, msg);
      std::snprintf(msg, sizeof(msg), "%s (Flats) -> %s", row.label, row.action);
      check(km.resolve(row.chord, LayerKind::Flats) == std::optional<std::string>(row.action), msg);
    }
  }

  // --- 2. a keymap action picks the same FlatsTool the palette cell does ---
  {
    struct ToolRow {
      const char* action;
      FlatsTool tool;
    };
    const ToolRow toolRows[] = {
        {"flats_tool_shape_fill", FlatsTool::ShapeFill},
        {"flats_tool_group", FlatsTool::Group},
        {"flats_tool_draw_merge", FlatsTool::DrawMerge},
        {"flats_tool_bridge_pen", FlatsTool::BridgePen},
        {"flats_tool_select_edits", FlatsTool::SelectEdits},
    };
    for (const ToolRow& row : toolRows) {
      check(flatsToolForKeyAction(row.action) == std::optional<FlatsTool>(row.tool),
            "flatsToolForKeyAction() maps the action to the tool the palette cell picks");
    }
    check(!flatsToolForKeyAction("flats_delete_fill").has_value(),
          "a one-shot FlatsAction name is not also a tool-selection action");
    check(!flatsToolForKeyAction("bogus_action").has_value(),
          "an unknown action name maps to nothing");

    // toggleFlatsTool() IS ui/MacPaintUI.cpp's flatsToolButton() click rule
    // (this track pointed the palette cell at this same function), so
    // proving it here proves the palette cell too, not a second
    // implementation that merely looks like it.
    AppState st;
    check(st.flatsTool == FlatsTool::None, "fixture: no flats tool armed at the start");
    check(toggleFlatsTool(st, FlatsTool::ShapeFill) && st.flatsTool == FlatsTool::ShapeFill,
          "the key selects SHAPE, same as clicking its palette cell once");
    check(toggleFlatsTool(st, FlatsTool::ShapeFill) && st.flatsTool == FlatsTool::None,
          "pressing the same key again deselects, same as clicking the already-lit cell");
    check(toggleFlatsTool(st, FlatsTool::Group) && st.flatsTool == FlatsTool::Group,
          "picking a different tool while none is active selects it");
    check(toggleFlatsTool(st, FlatsTool::BridgePen) && st.flatsTool == FlatsTool::BridgePen,
          "picking a different tool while one IS active switches to it, not a toggle-off");
  }

  // --- 3. accept-all-gaps: every pending suggestion, one undo step's worth -
  {
    Layer layer;
    layer.kind = LayerKind::Flats;
    FlatEvaluation e;
    e.suggestions = {FlatPolyline{10.f, 10.f, 20.f, 20.f}, FlatPolyline{50.f, 50.f, 60.f, 60.f}};
    check(e.suggestions.size() == 2, "fixture: exactly two pending gaps before accepting");
    check(layer.flats.edits.bridges.empty(), "fixture: no bridges recorded yet");

    const int accepted = flatsAcceptAllSuggestions(layer, e);
    check(accepted == 2, "accept-all accepts both of the two pending gaps");
    check(layer.flats.edits.bridges.size() == 2,
          "...through flatsAcceptSuggestion() once per gap, so ui/MacPaintUI.cpp's case needs "
          "only ONE recordEdit() call after the loop to make this one undo step");
  }
  {
    Layer layer;
    layer.kind = LayerKind::Flats;
    const FlatEvaluation e;  // no suggestions
    check(e.suggestions.empty(), "fixture: nothing pending");
    check(flatsAcceptAllSuggestions(layer, e) == 0 && layer.flats.edits.bridges.empty(),
          "accept-all with nothing pending accepts nothing and records no bridge");
  }

  std::printf("  -- the one-shot Flats keys' action table --\n");
  {
    const std::pair<const char*, FlatsAction> kTable[] = {
        {"flats_delete_fill", FlatsAction::DeleteFill},
        {"flats_merge_pair", FlatsAction::MergePair},
        {"flats_prev_gap", FlatsAction::PrevGap},
        {"flats_next_gap", FlatsAction::NextGap},
        {"flats_accept_gap", FlatsAction::AcceptGap},
        {"flats_cluster_small", FlatsAction::ClusterSmall},
        {"flats_accept_all_gaps", FlatsAction::AcceptAllGaps},
    };
    bool allMapped = true;
    for (const auto& [name, expected] : kTable) {
      const std::optional<FlatsAction> got = flatsActionForKeyAction(name);
      if (!got || *got != expected) {
        std::printf("    %s mapped wrongly\n", name);
        allMapped = false;
      }
    }
    check(allMapped, "each one-shot Flats key action maps to its own FlatsAction");
    check(!flatsActionForKeyAction("flats_tool_group") && !flatsActionForKeyAction("brush"),
          "a tool-picking or unrelated action raises no one-shot FlatsAction");
  }

  std::printf("[selftest] flats keys %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
