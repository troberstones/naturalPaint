#include "app/selftest/Support.hpp"

#include <set>
#include <utility>

#include "ui/MenuModel.hpp"

namespace np {
namespace {

// Everything a backend could pick, in tree order, with the state that decides
// whether picking it does anything.
struct Pickable {
  MenuAction action = MenuAction::None;
  int param = 0;
  bool enabled = false;
  bool checked = false;
  std::string label;
  std::string shortcutText;
  char key = 0;
  uint16_t mods = 0;
};

void collect(const std::vector<MenuNode>& nodes, std::vector<Pickable>& out) {
  for (const MenuNode& n : nodes) {
    if (n.kind == MenuNodeKind::Command || n.kind == MenuNodeKind::Check) {
      Pickable p;
      p.action = n.action;
      p.param = n.param;
      p.enabled = n.enabled;
      p.checked = n.checked;
      p.label = n.label;
      p.shortcutText = n.shortcutText;
      p.key = n.keyEquivalent.key;
      p.mods = n.keyEquivalent.mods;
      out.push_back(std::move(p));
    }
    collect(n.children, out);
  }
}

size_t countNodes(const std::vector<MenuNode>& nodes) {
  size_t n = nodes.size();
  for (const MenuNode& node : nodes) n += countNodes(node.children);
  return n;
}

// Submenu titles and their enabled state, in tree order. Collected separately
// from the pickable items because the sweep treats them differently on purpose,
// and a deliberate exception that nothing asserts is an exception the next
// tidy-up removes.
void collectSubmenus(const std::vector<MenuNode>& nodes,
                     std::vector<std::pair<std::string, bool>>& out) {
  for (const MenuNode& n : nodes) {
    if (n.kind == MenuNodeKind::Submenu) out.emplace_back(n.label, n.enabled);
    collectSubmenus(n.children, out);
  }
}

MenuFamilyEntry row(const char* label, bool enabled = true, bool checked = false) {
  MenuFamilyEntry e;
  e.label = label;
  e.enabled = enabled;
  e.checked = checked;
  return e;
}

// A state in which as much as possible is live, so that "nothing is enabled
// under the gate" is a claim about the gate rather than about a state that had
// nothing enabled to begin with. Same fixture shape as
// `app/selftest/MenuModel.cpp`'s `richContext()`, kept local rather than shared
// for the reason the agent brief gives: a section owns its own fixture, and a
// second section reaching into this one's is how two tests start failing
// together for one reason.
MenuContext liveContext() {
  MenuContext ctx;
  ctx.hasDocument = true;
  ctx.hasPath = true;
  ctx.recentDocuments = {row("wash.npaint"), row("sketch.npaint")};
  ctx.canUndo = true;
  ctx.canRedo = true;
  ctx.hasActiveLayer = true;
  ctx.hasEditableLayer = true;
  ctx.clipboardHasContent = true;
  ctx.hasSelection = true;
  ctx.hasLastDeselected = true;
  ctx.activeLayerTitle = "0 " "\xC2\xB7" " Wash";
  ctx.layerCommands = {row("New RGB Layer"), row("Toggle Visibility", true, true)};
  ctx.layerSelectionNote = "1 layer(s) selected";
  ctx.layerSetCommands = {row("Delete Layers")};
  ctx.hasEngagedSelection = true;
  ctx.hasRgbSource = true;
  ctx.hasRefineUndo = true;
  ctx.paintModes = {row("Watercolour", true, true)};
  ctx.tools = {row("Brush", true, true), row("Eraser")};
  ctx.openDocuments = {row("wash.npaint", true, true)};
  ctx.hasGuides = true;
  ctx.filterLayerUsable = true;
  return ctx;
}

// The five chords the bug report named, each with the keymap action it resolves
// to and what that action does to the document. Written out rather than derived
// so that the sentence in the failure output names the actual damage.
struct NamedChord {
  MenuAction action;
  char key;
  const char* keymapAction;
  const char* damage;
};

}  // namespace

// --------------------------------------------------------------------------
// Modal keyboard capture -- a dialog's keystrokes must not reach the document.
//
// **The defect, in the shape it was found.** With the brush-library import
// dialog open and focused (the `+` in the BRUSH LIBRARY pane -> `Import
// Brushes`), a user typing a file path pressed Delete to rub out a character.
// The dialog took the character. `delete_selection` ALSO ran, clearing the
// selection out of the active layer of the document behind the dialog, with a
// history entry and no word about it. `⌘V` did something stranger: it never
// reached the text field at all, and pasted a *layer* into the document.
//
// **Two dispatch routes, and only one of them is a key event.**
//
//  1. `SDL_EVENT_KEY_DOWN` -> `Keymap::resolve()` -> `main.cpp`'s action
//     dispatch. This is how Delete arrives. Closed by that block's new
//     `!st.keyboardOwnedByUi` guard, and **not reachable from this section** --
//     see the honesty note at the bottom.
//  2. AppKit's `performKeyEquivalent:`, which runs over `[NSApp mainMenu]`
//     *before* the event reaches the window. A chord an `NSMenuItem` claims
//     never becomes an SDL event at all, so guard (1) cannot see it. This is
//     how ⌘V, ⌘X, ⌘C, ⌘Z and ⌘A arrive -- ui/MenuModel.hpp's
//     `MenuKeyEquivalent` has documented that consumption from the day the
//     native bar landed. Closed by `MenuContext::keyboardOwnedByUi`, which
//     disables every item in the tree; AppKit validates before it performs, so
//     a disabled item neither acts nor honours its chord.
//
// Route 2 is what this section asserts, headlessly and completely: no window,
// no GPU, no ImGui context, no `NSApplication`.
//
// **What this section deliberately does NOT claim.** It does not prove that a
// keystroke fails to reach the document. It proves that the menu model stops
// authorising the *edit* -- which is the whole of the fix for route 2 that
// lives in this codebase, and none of the fix for route 1.
// docs/reachability-audit.md **F4** is the reason and it has not moved:
// `--selftest` cannot reach a single ImGui or SDL dispatch site. So `main.cpp`'s
// guard is covered by nothing here, and deleting that one line reintroduces the
// Delete leak with this suite green. The two facts in part D below are as close
// as a headless test can get: they establish that Delete could only ever have
// arrived by route 1, which is why route 1 needed a guard of its own.
bool runModalKeyboardTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-88s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const std::vector<MenuAction> allActions = [] {
    std::vector<MenuAction> v;
    for (size_t i = 1; i < static_cast<size_t>(MenuAction::Count); ++i)
      v.push_back(static_cast<MenuAction>(i));
    return v;
  }();

  MenuContext open = liveContext();
  MenuContext gated = liveContext();
  gated.keyboardOwnedByUi = true;

  const std::vector<MenuNode> openTree = buildMenuModel(open);
  const std::vector<MenuNode> gatedTree = buildMenuModel(gated);

  std::vector<Pickable> openItems;
  std::vector<Pickable> gatedItems;
  collect(openTree, openItems);
  collect(gatedTree, gatedItems);

  std::printf("  -- A. the sweep is total --\n");

  {
    size_t enabledOpen = 0;
    for (const Pickable& p : openItems) enabledOpen += p.enabled ? 1 : 0;
    // The baseline. Without it, "nothing is enabled under the gate" would pass
    // just as well on a context in which nothing was enabled anyway, and the
    // section would be asserting that an empty set is empty.
    std::printf("      %zu pickable items, %zu of them enabled with no dialog up\n",
                openItems.size(), enabledOpen);
    check(enabledOpen > 0,
          "gate: with no dialog up, this fixture leaves items enabled -- so the "
          "assertion below has something to take away");

    size_t enabledGated = 0;
    for (const Pickable& p : gatedItems)
      if (p.enabled) {
        ++enabledGated;
        std::printf("      still enabled under the gate: %s (param %d)\n",
                    menuActionName(p.action), p.param);
      }
    check(enabledGated == 0,
          "gate: with the UI holding the keyboard, NOT ONE pickable item in any menu is "
          "enabled -- a single survivor is a chord AppKit would still consume");
  }

  {
    // A gate that removed items instead of disabling them would be a menu bar
    // whose shape changed whenever a text field gained focus -- and part B
    // below could not then be true.
    bool sameSet = openItems.size() == gatedItems.size();
    bool sameLabels = sameSet;
    bool sameChecks = sameSet;
    bool sameChords = sameSet;
    for (size_t i = 0; sameSet && i < openItems.size(); ++i) {
      if (openItems[i].action != gatedItems[i].action ||
          openItems[i].param != gatedItems[i].param)
        sameSet = false;
      if (openItems[i].label != gatedItems[i].label ||
          openItems[i].shortcutText != gatedItems[i].shortcutText)
        sameLabels = false;
      if (openItems[i].checked != gatedItems[i].checked) sameChecks = false;
      if (openItems[i].key != gatedItems[i].key || openItems[i].mods != gatedItems[i].mods)
        sameChords = false;
    }
    check(sameSet,
          "gate: the same items in the same order -- the gate disables, it does not "
          "remove, so no menu loses or gains a row when a text field takes focus");
    check(sameLabels,
          "gate: every label and every shortcut column is untouched -- a greyed menu "
          "still documents what the key does once the dialog is gone");
    check(sameChecks,
          "gate: every check mark still reads its own flag -- a gate that cleared them "
          "would have the View menu claim Grid and Snap were off while they were on");
    check(sameChords,
          "gate: the key-equivalent table is untouched -- what changes is whether AppKit "
          "HONOURS a chord, never which chord an item claims");
    check(countNodes(openTree) == countNodes(gatedTree),
          "gate: separators, notes and submenu titles are all still there -- the user can "
          "still LOOK at a menu while a dialog is up, they simply cannot pick from it");

    // The deliberate exception, pinned. A disabled `Submenu` greys its own
    // title, and the AppKit backend additionally takes its children out of
    // auto-enabling -- so sweeping submenus too would leave the user staring at
    // six greyed menu titles unable to OPEN a menu, while ImGui's modal is what
    // is actually blocking them. A submenu carries no action and no key
    // equivalent, so it consumes nothing and gating it buys nothing.
    std::vector<std::pair<std::string, bool>> openSubs;
    std::vector<std::pair<std::string, bool>> gatedSubs;
    collectSubmenus(openTree, openSubs);
    collectSubmenus(gatedTree, gatedSubs);
    check(!openSubs.empty() && openSubs == gatedSubs,
          "gate: submenu titles keep their own enabled state -- Open Recent is greyed by "
          "an empty list and by nothing else, and every menu can still be opened");
  }

  {
    // The failure mode this catches: a sweep written inside one of
    // `buildMenuModel()`'s populated branches rather than over the finished
    // tree. With nothing open, most menus fall to their "(no document open)"
    // shape, and an items-only sweep that lived in the Edit block would still
    // leave Window and View live.
    MenuContext bare;
    bare.keyboardOwnedByUi = true;
    const std::vector<MenuNode> bareTree = buildMenuModel(bare);
    std::vector<Pickable> bareItems;
    collect(bareTree, bareItems);
    size_t enabled = 0;
    for (const Pickable& p : bareItems) enabled += p.enabled ? 1 : 0;
    check(!bareItems.empty() && enabled == 0,
          "gate: holds on an EMPTY context too (no document, no families) -- the sweep is "
          "over the finished tree, not inside a branch that happened to be populated");
  }

  std::printf("  -- B. the native bar is not rebuilt for it --\n");

  {
    // Load-bearing, and the reason the gate touches `enabled` and nothing else.
    // `hashShape()` is what `updateNativeMenuBar()` compares against to decide
    // whether to tear the NSMenu down and build it again. Enabled state is
    // asked for per item, live, by `-validateMenuItem:`. If the gate moved the
    // generation, every keystroke into every text field in the application
    // would rebuild six NSMenus under the user's pointer.
    publishMenuModel(buildMenuModel(open));
    const uint64_t base = menuModelShapeGeneration();
    publishMenuModel(buildMenuModel(gated));
    const uint64_t afterGate = menuModelShapeGeneration();
    check(afterGate == base,
          "shape: gating does NOT move the shape generation -- AppKit asks for enabled "
          "per item, so the bar keeps the NSMenus it already built");
  }

  std::printf("  -- C. the chords AppKit would otherwise consume --\n");

  {
    // Derived from the spec table rather than hand-listed, so a chord claimed
    // by some future item is covered by this loop on the day it is added.
    // `gatedTree` is published from part B above, which is the state
    // `-validateMenuItem:` would be asking about.
    size_t claimed = 0;
    bool liveBefore = true;
    bool deadAfter = true;
    bool seamAgrees = true;
    for (const MenuAction a : allActions) {
      const MenuKeyEquivalent& ke = menuItemSpec(a).keyEquivalent;
      if (!ke.claimed()) continue;
      ++claimed;

      const Pickable* before = nullptr;
      const Pickable* after = nullptr;
      for (const Pickable& p : openItems)
        if (p.action == a) before = &p;
      for (const Pickable& p : gatedItems)
        if (p.action == a) after = &p;

      // A chord whose item is absent or already disabled in the open tree is
      // not evidence of anything: the interesting claim is that a LIVE chord
      // goes dead.
      if (before == nullptr || !before->enabled) {
        liveBefore = false;
        std::printf("      %s claims '%c' but is not live in the open tree\n",
                    menuActionName(a), ke.key);
      }
      if (after == nullptr || after->enabled) {
        deadAfter = false;
        std::printf("      %s claims '%c' and is STILL live under the gate\n",
                    menuActionName(a), ke.key);
      }
      // The exact function `-validateMenuItem:` calls, against the published
      // gated tree -- not a re-derivation of it. This is the seam, so this is
      // what gets asserted.
      if (menuItemEnabled(a, 0)) {
        seamAgrees = false;
        std::printf("      menuItemEnabled(%s) still answers YES under the gate\n",
                    menuActionName(a));
      }
    }
    std::printf("      %zu claimed chords checked\n", claimed);
    check(claimed > 0, "chords: the spec table claims some -- otherwise this loop is empty");
    check(liveBefore,
          "chords: every claimed chord IS live with no dialog up, so each one below is a "
          "key AppKit really was consuming");
    check(deadAfter,
          "chords: every claimed chord's item is disabled under the gate -- AppKit "
          "validates before it performs, so none of them can act");
    check(seamAgrees,
          "chords: menuItemEnabled() -- the one function -validateMenuItem: calls -- "
          "answers NO for every claimed chord against the published gated tree");
  }

  {
    // The five from the bug report, named, so that a failure here says which
    // user-visible act came back rather than only that a count moved.
    const NamedChord kReported[] = {
        {MenuAction::Paste, 'v', "paste",
         "pastes a LAYER into the document instead of text into the field"},
        {MenuAction::Cut, 'x', "cut", "cuts pixels out of the active layer"},
        {MenuAction::Copy, 'c', "copy", "overwrites the clipboard mid-edit"},
        {MenuAction::Undo, 'z', "undo", "moves the document's history cursor"},
        {MenuAction::SelectAll, 'a', "select_all", "replaces the selection"},
    };
    bool allClaimed = true;
    bool allGated = true;
    for (const NamedChord& c : kReported) {
      const MenuKeyEquivalent& ke = menuItemSpec(c.action).keyEquivalent;
      if (!ke.claimed() || ke.key != c.key || (ke.mods & kMenuModCmd) == 0 ||
          ke.keymapAction == nullptr || std::string(ke.keymapAction) != c.keymapAction) {
        allClaimed = false;
        std::printf("      %s no longer claims Cmd+%c -> %s\n", menuActionName(c.action),
                    c.key, c.keymapAction);
      }
      if (menuItemEnabled(c.action, 0)) {
        allGated = false;
        std::printf("      Cmd+%c still live under the gate -- it %s\n", c.key, c.damage);
      }
    }
    check(allClaimed,
          "reported: Cmd+V/X/C/Z/A are each still claimed by the native bar and still "
          "name their keymap action -- this section's premise, restated from the table");
    check(allGated,
          "reported: and every one of the five is refused while the UI holds the "
          "keyboard, which is the half of the defect that reached the document");
  }

  std::printf("  -- D. why Delete needed a guard somewhere else --\n");

  {
    // Two facts, and together they are the argument for the one line this
    // section cannot test.
    const bool deleteClaimsNothing =
        !menuItemSpec(MenuAction::DeleteSelection).keyEquivalent.claimed();
    check(deleteClaimsNothing,
          "delete: Delete Selection claims NO key equivalent -- ui/MenuModel.hpp's "
          "'every claimed chord carries Command' rule is why, and it should stay that way");

    Keymap km;
    const bool loaded = km.loadFromFile("default.json");
    check(loaded, "delete: keymaps/default.json loads (the file this is checked against)");
    if (loaded) {
      const std::optional<std::string> del =
          km.resolve(KeyChord{SDLK_DELETE, 0}, std::nullopt);
      const std::optional<std::string> back =
          km.resolve(KeyChord{SDLK_BACKSPACE, 0}, std::nullopt);
      check(del == std::optional<std::string>("delete_selection") &&
                back == std::optional<std::string>("delete_selection"),
            "delete: bare Delete and bare Backspace both resolve to delete_selection in "
            "the shipped keymap -- unmodified, so every text field competes with them");
      // The conclusion, spelled out because it is the boundary of this
      // section's coverage: the chord is claimed by NOTHING in the menu bar
      // and IS bound in the keymap, so the only route it ever had into the
      // document is main.cpp's SDL dispatch. That guard is one `if` in a file
      // no headless test reaches (docs/reachability-audit.md F4), and this is
      // the closest a green line can come to covering it.
      check(deleteClaimsNothing && del.has_value(),
            "delete: therefore its ONLY route was main.cpp's SDL dispatch -- guarded "
            "there, and NOT covered by this section (reachability-audit F4)");
    }
  }

  // Leave the published tree as an ungated one. The publish above is process
  // -global state and a later section that grew a `menuItemEnabled()` call
  // would otherwise inherit a bar in which nothing is enabled -- a failure
  // whose cause would be this file, three sections earlier.
  publishMenuModel(buildMenuModel(open));

  std::printf("[selftest] modal keyboard capture %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
