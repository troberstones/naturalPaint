#include "app/selftest/Support.hpp"

#include <optional>
#include <string>
#include <vector>

#include "app/AppState.hpp"
#include "app/DocumentLifecycle.hpp"
#include "app/Keymap.hpp"
#include "app/OpenAnyFile.hpp"
#include "core/History.hpp"
#include "sim/PaintSim.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/MacPaintUI.hpp"
#include "ui/MenuModel.hpp"

namespace np {
namespace {

// The Edit menu's contents, as `buildMenuModel(ctx)` produces it, or nullptr
// if the tree has no submenu labelled "Edit" at all.
const MenuNode* editMenu(const MenuContext& ctx, std::vector<MenuNode>& barOut) {
  barOut = buildMenuModel(ctx);
  for (const MenuNode& m : barOut)
    if (m.label == "Edit") return &m;
  return nullptr;
}

// Whether `action` is present in the Edit menu of `ctx`, and if so, enabled.
// `std::nullopt` means "not present at all" -- distinct from "present and
// disabled", which every predicate assertion below needs to tell apart.
std::optional<bool> editEnabled(const MenuContext& ctx, MenuAction action) {
  std::vector<MenuNode> bar;
  const MenuNode* edit = editMenu(ctx, bar);
  if (edit == nullptr) return std::nullopt;
  for (const MenuNode& n : edit->children)
    if (n.action == action) return n.enabled;
  return std::nullopt;
}

}  // namespace

// docs/reachability-audit.md, items D1, D2, D4 and A4. See app/SelfTest.hpp's
// doc block for what is asserted and why it does not duplicate
// app/selftest/MenuModel.cpp, the general suite for the menu model itself.
bool runMenuBasicsTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("  -- A. D1: Undo and Redo exist, and claim the right chords --\n");

  check(std::string(menuActionName(MenuAction::Undo)) == "Undo" &&
            std::string(menuActionName(MenuAction::Redo)) == "Redo",
        "D1: MenuAction::Undo and MenuAction::Redo exist and are named");

  {
    const MenuKeyEquivalent& undoKe = menuItemSpec(MenuAction::Undo).keyEquivalent;
    const MenuKeyEquivalent& redoKe = menuItemSpec(MenuAction::Redo).keyEquivalent;
    check(undoKe.claimed() && undoKe.key == 'z' && undoKe.mods == kMenuModCmd &&
              undoKe.keymapAction != nullptr && std::string(undoKe.keymapAction) == "undo",
          "D1: Undo claims Cmd+Z, resolving to keymap action \"undo\"");
    check(redoKe.claimed() && redoKe.key == 'z' && redoKe.mods == (kMenuModCmd | kMenuModShift) &&
              redoKe.keymapAction != nullptr && std::string(redoKe.keymapAction) == "redo",
          "D1: Redo claims Cmd+Shift+Z, resolving to keymap action \"redo\"");
  }

  {
    // The real, shipped file -- not a hand-built Keymap -- because the
    // question is "does ⌘Z actually work today", not "would it work if the
    // file said what this test assumes it says".
    Keymap km;
    const bool loaded = km.loadFromFile("default.json");
    check(loaded, "D1: keymaps/default.json loads (the file this is checked against)");
    if (loaded) {
      const std::optional<std::string> undoAction =
          km.resolve(KeyChord{static_cast<SDL_Keycode>('z'), kModCmd}, std::nullopt);
      const std::optional<std::string> redoAction =
          km.resolve(KeyChord{static_cast<SDL_Keycode>('z'), kModCmd | kModShift}, std::nullopt);
      check(undoAction == std::optional<std::string>("undo"),
            "D1: keymaps/default.json resolves Cmd+Z to \"undo\"");
      check(redoAction == std::optional<std::string>("redo"),
            "D1: keymaps/default.json resolves Cmd+Shift+Z to \"redo\"");
    }
  }

  std::printf("  -- B. D1: undo/redo reach the SAME implementation the title-bar "
             "buttons do --\n");

  {
    // Observable state, not which function ran: performing the menu action
    // sets the exact flag main.cpp's keymap dispatch sets for the matching
    // chord (AppState::requestUndo's own comment names both callers).
    AppState st;
    check(!st.requestUndo && !st.requestRedo, "D1: a fresh AppState requests neither");
    performMenuAction(st, MenuAction::Undo, 0, 64, 64);
    check(st.requestUndo && !st.requestRedo,
          "D1: performing MenuAction::Undo sets requestUndo and nothing else");
    st.requestUndo = false;
    performMenuAction(st, MenuAction::Redo, 0, 64, 64);
    check(!st.requestUndo && st.requestRedo,
          "D1: performing MenuAction::Redo sets requestRedo and nothing else");
  }

  {
    // moveHistoryCursor() is what the flag above is consumed into, and it is
    // the SAME function the HISTORY panel's and the title bar's buttons call
    // (ui/MacPaintUI.hpp). `sim` is left null -- the idle state ADR-0001
    // already assumes -- so this settles nothing and only moves the cursor,
    // which is the whole of what a headless test can ask it to prove.
    AppState st;
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{});
    od.recordEdit("edit one");
    od.recordEdit("edit two");
    History& h = od.history;
    check(h.canUndo() && !h.canRedo(),
          "D1: two edits leave undo available and redo not, before either is called");
    const size_t startCursor = h.cursor();

    std::unique_ptr<PaintSim> sim;
    GpuContext gpu;
    moveHistoryCursor(st, sim, gpu, od, -1);
    check(h.cursor() + 1 == startCursor,
          "D1: moveHistoryCursor(-1) -- undo's direction -- moves the cursor back one");
    check(h.canRedo(), "D1: after an undo, redo is available");

    moveHistoryCursor(st, sim, gpu, od, +1);
    check(h.cursor() == startCursor,
          "D1: moveHistoryCursor(+1) -- redo's direction -- restores the cursor");
  }

  std::printf("  -- C. D2: all nine clipboard/selection commands are in Edit, "
             "correctly wired --\n");

  {
    MenuContext ctx;
    ctx.hasDocument = true;
    ctx.hasPath = true;
    ctx.canUndo = true;
    ctx.canRedo = true;
    ctx.hasActiveLayer = true;
    ctx.hasEditableLayer = true;
    ctx.clipboardHasContent = true;
    ctx.hasSelection = true;
    ctx.hasLastDeselected = true;

    std::vector<MenuNode> bar;
    const MenuNode* edit = editMenu(ctx, bar);
    check(edit != nullptr, "D2: an Edit menu exists in the tree");

    if (edit != nullptr) {
      // One row per expected item: its action, the label it must carry, and
      // the keymap action name its claimed chord must resolve to (nullptr
      // for Delete, which binds to bare Backspace/Delete and therefore
      // claims no chord at all -- see MenuAction::DeleteSelection's spec).
      // This is the assertion that reddens if an item were wired to the
      // WRONG command, e.g. Copy Merged sharing Copy's action.
      struct Row {
        MenuAction action;
        const char* label;
        const char* keymapAction;
      };
      static const Row kExpected[] = {
          {MenuAction::Undo, "Undo", "undo"},
          {MenuAction::Redo, "Redo", "redo"},
          {MenuAction::Cut, "Cut", "cut"},
          {MenuAction::Copy, "Copy", "copy"},
          {MenuAction::CopyMerged, "Copy Merged", "copy_merged"},
          {MenuAction::Paste, "Paste", "paste"},
          {MenuAction::DeleteSelection, "Delete", nullptr},
          {MenuAction::SelectAll, "Select All", "select_all"},
          {MenuAction::Deselect, "Deselect", "deselect"},
          {MenuAction::Reselect, "Reselect", "reselect"},
          {MenuAction::InvertSelection, "Invert Selection", "invert_selection"},
          {MenuAction::ClearCanvas, "Clear Canvas", "clear_canvas"},
      };
      bool allCorrect = true;
      for (const Row& row : kExpected) {
        const MenuNode* found = nullptr;
        for (const MenuNode& n : edit->children)
          if (n.action == row.action) found = &n;
        if (found == nullptr) {
          allCorrect = false;
          std::printf("      missing from Edit: %s\n", menuActionName(row.action));
          continue;
        }
        if (found->label != row.label) {
          allCorrect = false;
          std::printf("      %s labelled \"%s\", expected \"%s\"\n",
                      menuActionName(row.action), found->label.c_str(), row.label);
        }
        const MenuKeyEquivalent& ke = menuItemSpec(row.action).keyEquivalent;
        const bool keyOk =
            row.keymapAction == nullptr
                ? !ke.claimed()
                : (ke.claimed() && ke.keymapAction != nullptr &&
                  std::string(ke.keymapAction) == row.keymapAction);
        if (!keyOk) {
          allCorrect = false;
          std::printf("      %s claims keymap action \"%s\", expected \"%s\"\n",
                      menuActionName(row.action), ke.keymapAction ? ke.keymapAction : "<none>",
                      row.keymapAction ? row.keymapAction : "<none>");
        }
      }
      check(allCorrect,
            "D2: Undo, Redo, the nine clipboard/selection commands and Clear Canvas are "
            "all in Edit, each labelled correctly and each claiming the chord that "
            "resolves to the SAME action main.cpp's dispatch uses for that keystroke");

      size_t pickable = 0;
      for (const MenuNode& n : edit->children)
        if (n.kind == MenuNodeKind::Command || n.kind == MenuNodeKind::Check) ++pickable;
      check(pickable == 14,
            "Edit holds exactly fourteen pickable rows -- the eleven D1/D2 additions, "
            "Clear Canvas, Free Transform, and the numeric Transform dialog, no more and "
            "no fewer");
    }
  }

  std::printf("  -- D. D1 + D2: the enable predicates, constructed states, no window --\n");

  {
    MenuContext emptyClip;
    emptyClip.hasDocument = true;
    MenuContext fullClip = emptyClip;
    fullClip.clipboardHasContent = true;
    check(editEnabled(emptyClip, MenuAction::Paste) == std::optional<bool>(false) &&
              editEnabled(fullClip, MenuAction::Paste) == std::optional<bool>(true),
          "D2: Paste is disabled against an empty clipboard and enabled against a full one");

    MenuContext noSel;
    noSel.hasDocument = true;
    MenuContext someSel = noSel;
    someSel.hasSelection = true;
    check(editEnabled(noSel, MenuAction::Deselect) == std::optional<bool>(false) &&
              editEnabled(someSel, MenuAction::Deselect) == std::optional<bool>(true),
          "D2: Deselect is disabled with no selection and enabled with one");
    check(editEnabled(noSel, MenuAction::InvertSelection) == std::optional<bool>(false) &&
              editEnabled(someSel, MenuAction::InvertSelection) == std::optional<bool>(true),
          "D2: Invert Selection follows the same predicate as Deselect");

    MenuContext locked;
    locked.hasDocument = true;
    locked.hasActiveLayer = true;
    MenuContext unlocked = locked;
    unlocked.hasEditableLayer = true;
    check(editEnabled(locked, MenuAction::Cut) == std::optional<bool>(false) &&
              editEnabled(unlocked, MenuAction::Cut) == std::optional<bool>(true),
          "D2: Cut is disabled on a locked layer and enabled on an editable one");
    check(editEnabled(locked, MenuAction::DeleteSelection) == std::optional<bool>(false) &&
              editEnabled(unlocked, MenuAction::DeleteSelection) == std::optional<bool>(true),
          "D2: Delete follows the same locked-layer predicate as Cut");
    check(editEnabled(locked, MenuAction::Copy) == std::optional<bool>(true),
          "D2: Copy needs only an active layer, not an unlocked one -- it mutates nothing");

    MenuContext nothingToReselect;
    nothingToReselect.hasDocument = true;
    MenuContext somethingToReselect = nothingToReselect;
    somethingToReselect.hasLastDeselected = true;
    check(editEnabled(nothingToReselect, MenuAction::Reselect) == std::optional<bool>(false) &&
              editEnabled(somethingToReselect, MenuAction::Reselect) == std::optional<bool>(true),
          "D2: Reselect is disabled with nothing to restore and enabled with something");

    MenuContext cannotUndo;
    cannotUndo.hasDocument = true;
    MenuContext canUndoRedo = cannotUndo;
    canUndoRedo.canUndo = true;
    canUndoRedo.canRedo = true;
    check(editEnabled(cannotUndo, MenuAction::Undo) == std::optional<bool>(false) &&
              editEnabled(canUndoRedo, MenuAction::Undo) == std::optional<bool>(true),
          "D1: Undo follows MenuContext::canUndo");
    check(editEnabled(cannotUndo, MenuAction::Redo) == std::optional<bool>(false) &&
              editEnabled(canUndoRedo, MenuAction::Redo) == std::optional<bool>(true),
          "D1: Redo follows MenuContext::canRedo");
  }

  std::printf("  -- E. A4: Goodies enables exactly the implemented tools --\n");

  {
    const std::vector<MenuFamilyEntry> tools = toolMenuFamily(Tool::Brush);
    check(tools.size() == static_cast<size_t>(Tool::Count),
          "A4: the Goodies tool family offers every tool -- disabled, not hidden");

    size_t implementedCount = 0;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i)
      if (toolImplemented(static_cast<Tool>(i))) ++implementedCount;

    size_t enabledCount = 0;
    bool everyDisabledSaysNotBuilt = true;
    bool noEnabledCarriesAReason = true;
    for (const MenuFamilyEntry& e : tools) {
      if (e.enabled) {
        ++enabledCount;
        if (!e.tooltip.empty()) noEnabledCarriesAReason = false;
      } else if (e.tooltip.find("Not built yet.") == std::string::npos) {
        everyDisabledSaysNotBuilt = false;
      }
    }
    // Stated as its own assertion so the count check below cannot pass
    // vacuously: this build must actually have an unimplemented tool, or
    // `enabledCount == implementedCount` would hold whether or not the A4
    // fix is in place.
    check(implementedCount < tools.size(),
          "A4: this build has at least one unimplemented tool to test the guard against");
    check(enabledCount == implementedCount,
          "A4: exactly the tools toolImplemented() says are built are enabled -- counted "
          "against that predicate, never a literal number, so this stays true as tools ship");
    check(everyDisabledSaysNotBuilt,
          "A4: every disabled tool's tooltip says \"Not built yet.\" -- toolTooltip()'s own "
          "words, reused rather than a second copy of the sentence");
    check(noEnabledCarriesAReason, "A4: an implemented tool carries no disabled-reason tooltip");
  }

  std::printf("  -- F. D4: positional-argument routing, as a pure function --\n");

  {
    bool allCorrect = true;
    auto expect = [&](const char* arg, bool wantPositional) {
      if (looksLikePositionalArgument(arg) != wantPositional) {
        allCorrect = false;
        std::printf("      \"%s\": expected %s\n", arg,
                    wantPositional ? "positional" : "NOT positional");
      }
    };
    expect("foo.npaint", true);
    expect("/absolute/path/sketch.png", true);
    expect("relative/path.jpg", true);
    expect("--selftest", false);
    expect("--screenshot", false);
    expect("-x", false);
    expect("", false);
    check(allCorrect,
          "D4: looksLikePositionalArgument() accepts a bare filename and refuses every "
          "flag spelling this build already recognises");
  }

  std::printf("[selftest] menu basics %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
