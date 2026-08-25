#include "app/selftest/Support.hpp"

#include <set>
#include <utility>

#include "app/AppState.hpp"
#include "ui/MenuModel.hpp"

namespace np {
namespace {

// Walk the tree and collect every (action, param) a backend could pick.
// Separators and notes are not pickable and are deliberately not collected --
// a `Note` that claimed an action would be an item the user can see and cannot
// press, which is the failure this whole section is built to catch.
void collectPickable(const std::vector<MenuNode>& nodes,
                     std::vector<std::pair<MenuAction, int>>& out) {
  for (const MenuNode& n : nodes) {
    if (n.kind == MenuNodeKind::Command || n.kind == MenuNodeKind::Check)
      out.emplace_back(n.action, n.param);
    collectPickable(n.children, out);
  }
}

// Count every node of any kind, so "the tree changed shape" can be asserted
// without depending on which node changed.
size_t countNodes(const std::vector<MenuNode>& nodes) {
  size_t n = nodes.size();
  for (const MenuNode& node : nodes) n += countNodes(node.children);
  return n;
}

MenuFamilyEntry row(const char* label, bool enabled = true, bool checked = false) {
  MenuFamilyEntry e;
  e.label = label;
  e.enabled = enabled;
  e.checked = checked;
  return e;
}

// A state in which **every** menu is populated: a document is open with a
// path, the recent list has entries, and all six families have rows. Anything
// less and a menu would be legitimately absent, and "is every action
// reachable?" would be asking a question the state had already answered no to.
MenuContext richContext() {
  MenuContext ctx;
  ctx.hasDocument = true;
  ctx.hasPath = true;
  ctx.recentDocuments = {row("wash.npaint"), row("sketch.npaint")};
  ctx.activeLayerTitle = "0 " "\xC2\xB7" " Wash";
  ctx.layerCommands = {row("New RGB Layer"), row("Delete Layer", false), row("Toggle Visibility", true, true)};
  ctx.layerSelectionNote = "2 layer(s) selected";
  ctx.layerSetCommands = {row("Delete Layers"), row("Link Layers", false)};
  ctx.paintModes = {row("Watercolour", true, true), row("Gouache")};
  ctx.tools = {row("Brush", true, true), row("Eraser")};
  ctx.openDocuments = {row("wash.npaint", true, true), row("sketch.npaint *")};
  ctx.hasGuides = true;
  // docs/reachability-audit.md C5: the Select menu (track7/selectmenu). All
  // three true, matching this function's own "every menu is populated" rule
  // -- anything less and Grow/Shrink/Feather/Colour Range/Luminance
  // Range/Undo Refine would be legitimately absent from the reachable set
  // rather than merely disabled, which is a different failure from the one
  // section B below is checking for.
  ctx.hasEngagedSelection = true;
  ctx.hasRgbSource = true;
  ctx.hasRefineUndo = true;
  return ctx;
}

}  // namespace

// ui/MenuModel -- what the menus ARE, separated from how they are drawn.
//
// The `BeginMainMenuBar()` block in ui/MacPaintUI.cpp used to hold **41**
// `ImGui::MenuItem()` call sites, each of which declared an item and performed
// its action in the body of an `if`. Nothing but Dear ImGui, mid-frame, could
// read any of them. That shape is what a native `NSMenu` cannot be built from,
// because a native menu is built once, out of band, and calls back with no
// `if` to be the body of -- so the honest choices were "extract the model" or
// "write every action out a second time in Objective-C".
//
// This section is what makes the first choice checkable. It is entirely
// headless: no window, no GPU, no ImGui context, no `NSApplication`. Being
// able to ask "is Open Recent disabled with an empty list?" without any of
// those is most of why the model was worth extracting in the first place.
//
// What is asserted:
//
//  - **The ids.** Every `MenuAction` has exactly one spec, no two specs claim
//    the same id, and the count is pinned at 41 -- one per call site the
//    extraction replaced. An item quietly dropped by a later edit fails here
//    rather than vanishing from the product.
//  - **Reachability.** Every action the application can perform appears in the
//    tree, and nothing appears that is not an action.
//  - **The predicates**, which are pure functions of a constructed state:
//    `Open Recent` disabled on an empty list and enabled with one entry,
//    `Save` needing a path where `Save As...` needs only a document, and the
//    check marks tracking the state they are supposed to mirror.
//  - **The quit**, at length, because it is the one place in this file where
//    getting it wrong costs the user their work rather than their patience.
//  - **The modals**, each pinned as deferred-to-the-next-frame rather than
//    performed inline, because "inline" for these means calling
//    `ImGui::OpenPopup()` from an AppKit callback with no frame in progress.
//  - **The key equivalents**, cross-checked against the *real*
//    keymaps/default.json, because a native menu item does not merely display
//    a chord -- it consumes it, before SDL ever sees the key.
bool runMenuModelTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // Every real action, i.e. the enum without its two sentinels. Built by
  // iteration rather than written out, so an enumerator added tomorrow is
  // covered by every assertion below without anyone remembering to add it.
  std::vector<MenuAction> allActions;
  for (size_t i = 1; i < static_cast<size_t>(MenuAction::Count); ++i)
    allActions.push_back(static_cast<MenuAction>(i));

  // The six families -- the actions that carry a meaningful `param`. Named
  // here because two separate assertions below need to know which they are.
  const std::set<MenuAction> kFamilies = {
      MenuAction::OpenRecentEntry, MenuAction::LayerCommandItem,
      MenuAction::LayerSetCommandItem, MenuAction::PaintModeItem,
      MenuAction::ToolItem, MenuAction::ActivateDocument};

  std::printf("  -- A. the ids: one action, one spec, and no two the same --\n");

  // **41 was the count of `ImGui::MenuItem()` call sites in the block this
  // model originally replaced**, counted rather than taken on trust (the brief
  // that commissioned this said 43). It is pinned because the number is the
  // only thing standing between "the extraction is complete" and "the
  // extraction dropped two items nobody has opened that menu since".
  //
  // **64 now, and the arithmetic is the point.** Three tracks landed menus in
  // one window and each pinned this literal to its own total in its own
  // branch -- 52, 47, and a third -- so all three were wrong about the merge
  // and none could have known. The union, counted from the merged enum rather
  // than taken from any one of them:
  //
  //     41  the original extraction
  //   + 11  D1/D2: Undo, Redo, Cut, Copy, Copy Merged, Paste, Delete,
  //         Select All, Deselect, Reselect, Invert Selection -- all written,
  //         tested, and reachable by nothing but a key
  //   +  6  C5: the Select menu -- Grow, Shrink, Feather, Colour Range,
  //         Luminance Range, Undo Refine
  //   +  6  C1: the Filter and Image menus -- Gaussian Blur, Sharpen,
  //         Unsharp Mask, Add Noise, Image Size, Canvas Size
  //   = 64
  //
  // This literal is exactly as brittle to the NEXT track that adds an action
  // as it was before, and that brittleness is the point -- an enumerator added
  // without a menu entry fails here rather than shipping unreachable.
  check(kMenuActionCount == 64,
        "ids: exactly 64 actions -- the original 41-item extraction plus D1/D2's "
        "eleven, C5's six and C1's six, so an item lost in a later edit fails here");

  {
    std::set<MenuAction> seen;
    bool everySpecMatchesItsId = true;
    bool everyOneNamed = true;
    for (const MenuAction a : allActions) {
      if (!seen.insert(a).second) everySpecMatchesItsId = false;
      // The spec table is an array indexed by the enum. A row whose `action`
      // disagrees with its own index is the failure that makes File > Save
      // perform File > Revert, and it is completely invisible on inspection.
      if (menuItemSpec(a).action != a) everySpecMatchesItsId = false;
      const std::string name = menuActionName(a);
      if (name.empty() || name.find("UNNAMED") != std::string::npos) everyOneNamed = false;
    }
    check(everySpecMatchesItsId,
          "ids: every action's spec carries its OWN id -- a row that disagrees with "
          "its index silently wires one menu item to another's behaviour");
    check(everyOneNamed,
          "ids: every action has a name -- an unnamed one reaches --selftest output and "
          "a native accessibility label as a plausible-looking placeholder");
  }

  {
    // A label collision is not automatically wrong (two menus may legitimately
    // hold "Clear..."), but a *duplicate id* always is: `menuItemEnabled()`
    // and the native backend's tag both resolve by id, so two items sharing
    // one means picking either performs the first.
    std::set<std::pair<MenuAction, int>> ids;
    bool unique = true;
    std::vector<std::pair<MenuAction, int>> pickable;
    collectPickable(buildMenuModel(richContext()), pickable);
    for (const auto& p : pickable)
      if (!ids.insert(p).second) unique = false;
    check(unique,
          "ids: no two items in the tree share an (action, param) -- they resolve by id, "
          "so a shared one makes picking either perform the first");
    check(!pickable.empty(), "ids: the tree is not empty for a fully-populated state");
  }

  std::printf("  -- B. reachability: nothing lost in the extraction --\n");

  {
    std::vector<std::pair<MenuAction, int>> pickable;
    collectPickable(buildMenuModel(richContext()), pickable);
    std::set<MenuAction> reached;
    for (const auto& p : pickable) reached.insert(p.first);

    std::vector<MenuAction> missing;
    for (const MenuAction a : allActions)
      if (reached.count(a) == 0) missing.push_back(a);
    for (const MenuAction a : missing)
      std::printf("      unreachable: %s\n", menuActionName(a));
    check(missing.empty(),
          "reach: EVERY action is reachable from the tree -- an action with no item is a "
          "built feature with no way to invoke it, which is the defect the Layer menu "
          "was written to fix once already");

    // The families' *rows* are not this section's to prove complete: they come
    // from `app::allLayerCommands()` and `core::allLayerSetCommands()`, and
    // runLayerPanel2aTest / runLayerMultiSelectTest already walk those exact
    // lists. What is proven here is that the model has a place to put them.
    bool onlyFamiliesUseParam = true;
    for (const auto& p : pickable)
      if (p.second != 0 && kFamilies.count(p.first) == 0) onlyFamiliesUseParam = false;
    check(onlyFamiliesUseParam,
          "reach: only the six family actions ever carry a non-zero param -- a single "
          "item with a stray param resolves to an id no backend has an item for");

    check(reached.count(MenuAction::None) == 0,
          "reach: no pickable item carries MenuAction::None -- an item that draws, "
          "highlights and does nothing is indistinguishable from a broken feature");
  }

  std::printf("  -- C. the predicates: pure, and the documented answer --\n");

  {
    // The brief's own worked example, both ways round.
    MenuContext empty;
    const std::vector<MenuNode> emptyBar = buildMenuModel(empty);
    const MenuNode* recentEmpty = nullptr;
    for (const MenuNode& n : emptyBar[0].children)
      if (n.label == "Open Recent") recentEmpty = &n;

    MenuContext one;
    one.recentDocuments = {row("wash.npaint")};
    const std::vector<MenuNode> oneBar = buildMenuModel(one);
    const MenuNode* recentOne = nullptr;
    for (const MenuNode& n : oneBar[0].children)
      if (n.label == "Open Recent") recentOne = &n;

    check(recentEmpty != nullptr && !recentEmpty->enabled,
          "pred: Open Recent is DISABLED on an empty list -- a submenu that opens onto "
          "nothing reads as a broken menu, not as an empty one");
    check(recentOne != nullptr && recentOne->enabled,
          "pred: Open Recent is ENABLED with one entry -- the same predicate, the other "
          "way round, so a hard-coded false could not pass both");
  }

  {
    // Save needs a *path*; Save As... needs only a document. Conflating the
    // two is how Save silently writes to a file the user never chose.
    MenuContext docNoPath;
    docNoPath.hasDocument = true;
    docNoPath.hasPath = false;
    const std::vector<MenuNode> docNoPathBar = buildMenuModel(docNoPath);
    bool saveDisabled = false, saveAsEnabled = false, importEnabled = false;
    for (const MenuNode& n : docNoPathBar[0].children) {
      if (n.action == MenuAction::Save) saveDisabled = !n.enabled;
      if (n.action == MenuAction::SaveAs) saveAsEnabled = n.enabled;
      if (n.action == MenuAction::ImportImage) importEnabled = n.enabled;
    }
    check(saveDisabled && saveAsEnabled,
          "pred: with a document but no path, Save is disabled and Save As... is not -- "
          "the two predicates are genuinely different, not one copied twice");
    check(importEnabled,
          "pred: Import Image... needs a document, and this state has one");

    MenuContext nothing;
    const std::vector<MenuNode> bareBar = buildMenuModel(nothing);
    bool importHasReason = false;
    for (const MenuNode& n : bareBar[0].children)
      if (n.action == MenuAction::ImportImage) importHasReason = !n.enabled && !n.tooltip.empty();
    check(importHasReason,
          "pred: a greyed Import Image... carries its own explanation -- a disabled item "
          "with no reason is how a user concludes a feature is missing, not inapplicable");
  }

  {
    // The checked half. A `bool*` in an ImGui call is state the native backend
    // cannot see at all, so every one of them had to become data.
    MenuContext on;
    on.grayscale = true;
    on.showGrid = true;
    on.snappingEnabled = true;
    MenuContext off;
    auto viewFlag = [](const MenuContext& c, MenuAction a) {
      for (const MenuNode& menu : buildMenuModel(c))
        for (const MenuNode& n : menu.children)
          if (n.action == a) return n.checked;
      return false;
    };
    check(viewFlag(on, MenuAction::GrayscalePreview) &&
              !viewFlag(off, MenuAction::GrayscalePreview),
          "pred: Grayscale Preview's tick follows the state both ways -- it was a bool* "
          "in the ImGui call, which is exactly the state a native menu cannot read");
    check(viewFlag(on, MenuAction::Grid) && viewFlag(on, MenuAction::Snap) &&
              !viewFlag(off, MenuAction::Snap),
          "pred: Grid and Snap track their own flags, not one flag read twice");

    MenuContext noGuides;
    MenuContext someGuides;
    someGuides.hasGuides = true;
    auto clearGuidesEnabled = [](const MenuContext& c) {
      for (const MenuNode& menu : buildMenuModel(c))
        for (const MenuNode& n : menu.children)
          if (n.action == MenuAction::ClearGuides) return n.enabled;
      return false;
    };
    check(!clearGuidesEnabled(noGuides) && clearGuidesEnabled(someGuides),
          "pred: Clear Guides is dead with no guides placed and live with some");
  }

  {
    // Purity, stated as a property rather than assumed: the same context in
    // must give the same tree out, or the native backend's published snapshot
    // means nothing.
    const MenuContext ctx = richContext();
    const std::vector<MenuNode> a = buildMenuModel(ctx);
    const std::vector<MenuNode> b = buildMenuModel(ctx);
    std::vector<std::pair<MenuAction, int>> pa, pb;
    collectPickable(a, pa);
    collectPickable(b, pb);
    check(pa == pb && countNodes(a) == countNodes(b),
          "pred: buildMenuModel() is pure -- the same context twice gives the same tree, "
          "which is what lets one backend draw it and the other publish it");
  }

  std::printf("  -- D. the quit: the data-loss assertions --\n");

  // `AppState::quit` stops the frame loop outright; `AppState::requestQuit` is
  // answered against every open document by app/QuitSequence. A menu that
  // wrote the first discards unsaved work without a word -- which is what this
  // application shipped once, and what these five assertions exist to stop
  // being reintroduced by either backend.
  {
    check(menuActionEffect(MenuAction::Quit) == MenuEffect::QuitRequest,
          "quit: Quit's declared effect is QuitRequest, NOT Inline -- Inline here would "
          "mean a menu item that writes AppState::quit and skips the guard");

    size_t quitLike = 0;
    for (const MenuAction a : allActions)
      if (menuActionEffect(a) == MenuEffect::QuitRequest) ++quitLike;
    check(quitLike == 1,
          "quit: exactly ONE action can end the session -- a second exit route is a "
          "second place for the unsaved-work question to be forgotten");
  }

  {
    // The assertion a backend calling `[NSApp terminate:]` could not pass, in
    // its portable half: the *action* must set `requestQuit` and must leave
    // `quit` alone. Constructing an AppState is enough -- no window, no GPU.
    AppState st;
    const bool before = st.quit;
    performMenuAction(st, MenuAction::Quit, 0, 64, 64);
    check(st.requestQuit,
          "quit: performing Quit sets AppState::requestQuit -- main.cpp's guard answers "
          "it against every open document (app/QuitSequence)");
    check(!st.quit && before == false,
          "quit: performing Quit does NOT set AppState::quit -- that flag stops the loop "
          "with nothing in the way, and a menu that wrote it would discard every "
          "unsaved document AND delete the recovery journal's copy of them");
  }

  {
    // The other half, and the one aimed squarely at the AppKit backend: no
    // item in this application may be wired to a system selector. The item a
    // Mac developer's fingers reach for `@selector(terminate:)` on is Quit,
    // and that is precisely the item where doing so routes around the guard.
    bool anySystemSelector = false;
    for (const MenuAction a : allActions)
      if (menuItemSpec(a).mayUseSystemSelector) anySystemSelector = true;
    check(!anySystemSelector,
          "quit: NO item may use a system selector -- terminate: on Quit survives today "
          "only because SDL subclasses NSApplication to override it, and the failure "
          "mode if that ever changes is total, silent loss of the user's work");

    check(!menuItemSpec(MenuAction::Quit).keyEquivalent.claimed(),
          "quit: File > Quit claims no key equivalent -- SDL's own application menu "
          "already owns Cmd+Q, and two items claiming one chord resolve by first match");
    check(menuItemSpec(MenuAction::Quit).omitWhenNativeAppMenu,
          "quit: File > Quit is suppressed where the platform has an application menu -- "
          "two Quit items in one bar is the classic result of this port");
  }

  {
    // Suppression must remove Quit and nothing else. A backend that dropped
    // three items while claiming to drop one would otherwise pass silently.
    MenuContext withNative = richContext();
    withNative.nativeAppMenuPresent = true;
    MenuContext withoutNative = richContext();

    std::vector<std::pair<MenuAction, int>> a, b;
    collectPickable(buildMenuModel(withNative), a);
    collectPickable(buildMenuModel(withoutNative), b);
    std::set<std::pair<MenuAction, int>> sa(a.begin(), a.end()), sb(b.begin(), b.end());

    bool quitGone = sa.count({MenuAction::Quit, 0}) == 0;
    bool quitThere = sb.count({MenuAction::Quit, 0}) == 1;
    size_t otherDifferences = 0;
    for (const auto& p : sb)
      if (p.first != MenuAction::Quit && sa.count(p) == 0) ++otherDifferences;

    check(quitGone && quitThere,
          "quit: the File menu drops Quit under a native application menu and keeps it "
          "without one -- so a build where the native bar declined still has a way out");
    check(otherDifferences == 0,
          "quit: suppressing Quit removes EXACTLY one item -- nothing else may go missing "
          "under the cover of the platform check");
  }

  std::printf("  -- E. the modals: deferred, never performed inline --\n");

  {
    // Every one of these opens an ImGui popup or dialog. `Inline` for any of
    // them means calling into ImGui from an AppKit callback, on a context that
    // is not between NewFrame() and Render().
    const MenuAction kModals[] = {
        MenuAction::Open,     MenuAction::ImportImage, MenuAction::SaveAs,
        MenuAction::SaveCopy, MenuAction::Revert,      MenuAction::RecoverDocuments,
        MenuAction::ExportAs, MenuAction::ExportStates, MenuAction::AddGuide};
    bool allDeferred = true;
    for (const MenuAction a : kModals)
      if (menuActionEffect(a) != MenuEffect::Deferred) {
        allDeferred = false;
        std::printf("      not deferred: %s\n", menuActionName(a));
      }
    check(allDeferred,
          "modal: all nine modal-opening actions are Deferred -- Inline for any of them "
          "means ImGui::OpenPopup() from an AppKit callback with no frame in progress");

    // Add Guide... is called out on its own because it is the one that was
    // genuinely wrong before this change: it called ImGui::OpenPopup() inline
    // from inside BeginMenu(), and got away with it only because that popup
    // has a global string ID rather than one on the menu's ID stack.
    check(menuActionEffect(MenuAction::AddGuide) == MenuEffect::Deferred,
          "modal: Add Guide... is deferred -- it used to call OpenPopup() inline, which "
          "a native menu callback has no frame to open a popup against");

    // And the converse, so "deferred" has not simply been applied to
    // everything: the plain flag flips must stay inline, or a menu tick would
    // lag a frame behind the click that set it.
    check(menuActionEffect(MenuAction::Grid) == MenuEffect::Inline &&
              menuActionEffect(MenuAction::ToolItem) == MenuEffect::Inline,
          "modal: the plain state flips are still Inline -- marking everything deferred "
          "would put a frame between every click and its own check mark");
  }

  std::printf("  -- F. key equivalents: what a native menu CONSUMES --\n");

  // An NSMenuItem's key equivalent is not a label. NSApplication runs
  // performKeyEquivalent: over the main menu before the event reaches the
  // window, so a chord the menu owns never becomes an SDL_EVENT_KEY_DOWN and
  // never resolves through keymaps/default.json at all. That is invisible --
  // and correct -- only while the menu item does what the keymap would have
  // done, which is what this part checks against the real shipped file.
  {
    // The conversion below is the identity, and that is asserted rather than
    // assumed: ui/MenuModel.hpp defines its own modifier bits precisely so it
    // does not have to include app/Keymap.hpp (which drags SDL into an
    // Objective-C++ translation unit), and two independent enums that happen
    // to agree today are two enums that can stop agreeing.
    check(static_cast<uint16_t>(kMenuModCmd) == static_cast<uint16_t>(kModCmd) &&
              static_cast<uint16_t>(kMenuModShift) == static_cast<uint16_t>(kModShift) &&
              static_cast<uint16_t>(kMenuModOption) == static_cast<uint16_t>(kModAlt) &&
              static_cast<uint16_t>(kMenuModControl) == static_cast<uint16_t>(kModCtrl),
          "keys: MenuModel's modifier bits match app/Keymap's exactly, so the chord "
          "comparison below is comparing like with like");
  }

  {
    std::set<std::pair<char, uint16_t>> chords;
    bool unique = true;
    bool allCommandModified = true;
    size_t claimed = 0;
    for (const MenuAction a : allActions) {
      const MenuKeyEquivalent& ke = menuItemSpec(a).keyEquivalent;
      if (!ke.claimed()) continue;
      ++claimed;
      if (!chords.insert({ke.key, ke.mods}).second) {
        unique = false;
        std::printf("      duplicate chord on %s\n", menuActionName(a));
      }
      // **The rule that keeps a bare letter out of the menu bar.** A key
      // equivalent with no Command modifier is swallowed application-wide,
      // including inside every text field. `F`, `Shift+F`, `Shift+R` and
      // `Space` are all display-only for exactly this reason -- and Space is
      // also the pan gesture, the most-used key in the application.
      if ((ke.mods & kMenuModCmd) == 0) {
        allCommandModified = false;
        std::printf("      no Command modifier on %s\n", menuActionName(a));
      }
    }
    check(unique,
          "keys: no two items claim the same chord -- AppKit resolves a collision by "
          "first match, which nobody can predict by reading the menu");
    check(allCommandModified,
          "keys: every claimed chord carries Command -- a bare-letter key equivalent is "
          "consumed globally, including while the user is typing into a text field");
    // 11 + D1/D2's ten (⌘Z, ⇧⌘Z, ⌘X, ⌘C, ⇧⌘C, ⌘V, ⌘A, ⌘D, ⇧⌘D, ⇧⌘I). Delete
    // Selection is the deliberate eleventh of the eleven new actions that does
    // NOT claim one -- `keymaps/default.json` binds it to bare Backspace/
    // Delete, and the rule two blocks up ("every claimed chord carries
    // Command") is exactly why it stays unclaimed rather than swallowing the
    // Delete key out of every text field in the application.
    check(claimed == 21,
          "keys: exactly 21 chords are claimed -- pinned, because claiming one more "
          "silently takes that key away from SDL and from keymaps/default.json");
  }

  {
    // The four display-only chords, pinned in BOTH directions: shortcut text
    // present, key equivalent absent. Reversing either half is the failure --
    // dropping the text loses documented behaviour from the menu, and claiming
    // the chord breaks typing.
    const MenuAction kDisplayOnly[] = {MenuAction::MirrorX, MenuAction::MirrorY,
                                       MenuAction::ResetRotation, MenuAction::PauseSolver};
    bool correct = true;
    for (const MenuAction a : kDisplayOnly) {
      const MenuItemSpec& s = menuItemSpec(a);
      if (s.keyEquivalent.claimed() || std::string(s.shortcutText).empty()) correct = false;
    }
    check(correct,
          "keys: F, Shift+F, Shift+R and Space are shown as text and claimed as chords by "
          "NOTHING -- the menu still documents them and SDL still delivers them");
  }

  {
    // The cross-check against the real, shipped file.
    Keymap km;
    const bool loaded = km.loadFromFile("default.json");
    check(loaded, "keys: keymaps/default.json loads (the file this is checked against)");
    if (loaded) {
      bool everyChordAgrees = true;
      for (const MenuAction a : allActions) {
        const MenuKeyEquivalent& ke = menuItemSpec(a).keyEquivalent;
        if (!ke.claimed()) continue;
        // SDL3 keycodes for printable keys ARE their Unicode codepoints, which
        // is what lets an AppKit key-equivalent character be resolved straight
        // through the keymap without a translation table between them.
        const KeyChord chord{static_cast<SDL_Keycode>(ke.key), ke.mods};
        const std::optional<std::string> resolved = km.resolve(chord, std::nullopt);
        const bool agrees = ke.keymapAction == nullptr
                                ? !resolved.has_value()
                                : resolved == std::optional<std::string>(ke.keymapAction);
        if (!agrees) {
          everyChordAgrees = false;
          std::printf("      %s claims '%c' but the keymap says %s (model says %s)\n",
                      menuActionName(a), ke.key,
                      resolved ? resolved->c_str() : "<unbound>",
                      ke.keymapAction ? ke.keymapAction : "<unbound>");
        }
      }
      check(everyChordAgrees,
            "keys: every claimed chord resolves in keymaps/default.json to the action the "
            "model says it does -- a native menu CONSUMES the chord, so a disagreement is "
            "a documented key that quietly does something else");
    }
  }

  std::printf("  -- G. the published snapshot and the native queue --\n");

  {
    // The seam the AppKit backend actually reads. `validateMenuItem:` asks
    // these two questions, per item, when a menu is about to open.
    MenuContext ctx = richContext();
    publishMenuModel(buildMenuModel(ctx));
    check(menuItemEnabled(MenuAction::Save, 0) && menuItemChecked(MenuAction::ToolItem, 0),
          "snap: the published tree answers enabled/checked for a live id");
    check(!menuItemEnabled(MenuAction::LayerCommandItem, 1),
          "snap: a disabled row publishes as disabled -- the native bar greys the same "
          "items the ImGui bar does, from the same answer");
    check(!menuItemEnabled(MenuAction::ActivateDocument, 999) &&
              !menuItemChecked(MenuAction::ActivateDocument, 999),
          "snap: an id that is not in the published tree is NOT enabled -- a native menu "
          "outlives the state it was built from, and a stale row must not be pickable");
  }

  {
    // The generation is what stops the native backend rebuilding an NSMenu
    // sixty times a second, and it must move for a shape change and stay put
    // for a state change -- getting the second half wrong is a menu bar that
    // rebuilds itself under the user's pointer.
    MenuContext ctx = richContext();
    publishMenuModel(buildMenuModel(ctx));
    const uint64_t base = menuModelShapeGeneration();

    MenuContext stateOnly = ctx;
    stateOnly.grayscale = !ctx.grayscale;
    stateOnly.showGrid = !ctx.showGrid;
    publishMenuModel(buildMenuModel(stateOnly));
    const uint64_t afterStateChange = menuModelShapeGeneration();

    MenuContext shape = ctx;
    shape.openDocuments.push_back(row("third.npaint"));
    publishMenuModel(buildMenuModel(shape));
    const uint64_t afterShapeChange = menuModelShapeGeneration();

    check(afterStateChange == base,
          "snap: flipping a check mark does NOT move the shape generation -- AppKit asks "
          "for that per item, and rebuilding the bar for it would rebuild it constantly");
    check(afterShapeChange != base,
          "snap: opening a document DOES move it -- a document list that never rebuilt "
          "would show the documents the user had open when the app started");
  }

  {
    // The queue between an AppKit callback and the next frame. FIFO, because
    // two picks in one frame is not impossible and reversing them is a bug
    // nobody could reproduce on purpose.
    MenuAction a = MenuAction::None;
    int p = -1;
    while (dequeueMenuAction(&a, &p)) {  // drain anything a prior section left
    }
    check(!dequeueMenuAction(&a, &p), "queue: empty when drained");

    enqueueMenuAction(MenuAction::ZoomIn, 0);
    enqueueMenuAction(MenuAction::ActivateDocument, 2);
    const bool first = dequeueMenuAction(&a, &p) && a == MenuAction::ZoomIn && p == 0;
    const bool second = dequeueMenuAction(&a, &p) && a == MenuAction::ActivateDocument && p == 2;
    check(first && second,
          "queue: picks come back in the order they were made, with their params intact");
    check(!dequeueMenuAction(&a, &p),
          "queue: drains to empty -- a pick left in it would fire again next frame");
  }

  std::printf("[selftest] menu model %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
