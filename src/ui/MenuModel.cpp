#include "ui/MenuModel.hpp"

#include <mutex>

namespace np {
namespace {

// The static half of every action, indexed by `MenuAction`. A designated
// initialiser per row would be prettier and is not available for an array in
// C++20 without naming every field in order, so the table is built once into a
// function-local static and validated by `--selftest` instead: every action in
// the enum has a row, every row's `action` matches its own index, and the ids
// are unique. A row that disagrees with its index is the exact failure that
// makes File > Save perform File > Revert, and it is invisible on inspection.
const MenuItemSpec* specTable() {
  static MenuItemSpec table[static_cast<size_t>(MenuAction::Count)];
  static const bool built = [] {
    auto set = [](MenuAction a, const char* label, const char* shortcutText,
                  MenuKeyEquivalent ke = {}) {
      MenuItemSpec& s = table[static_cast<size_t>(a)];
      s.action = a;
      s.label = label;
      s.shortcutText = shortcutText;
      s.keyEquivalent = ke;
    };
    // A family member's label comes from `MenuContext`, so the table carries
    // the family's own identity and an empty label rather than a plausible
    // placeholder that could reach a user if a `buildMenuModel()` branch were
    // ever to fall through to it.
    auto family = [](MenuAction a) {
      table[static_cast<size_t>(a)].action = a;
    };

    set(MenuAction::NewCanvas, "New Canvas", "Cmd+N",
        MenuKeyEquivalent{'n', kMenuModCmd, "clear_canvas"});
    set(MenuAction::NewDocument, "New Document", "");
    set(MenuAction::Open, "Open...", "");
    family(MenuAction::OpenRecentEntry);
    set(MenuAction::ClearRecentMenu, "Clear Menu", "");
    set(MenuAction::ImportImage, "Import Image...", "");
    set(MenuAction::Save, "Save", "");
    set(MenuAction::SaveAs, "Save As...", "");
    set(MenuAction::SaveCopy, "Save a Copy...", "");
    set(MenuAction::SaveIncremental, "Save Incremental", "");
    set(MenuAction::RecoverDocuments, "Recover Documents...", "");
    set(MenuAction::Revert, "Revert", "");
    set(MenuAction::DuplicateDocument, "Duplicate Document", "");
    set(MenuAction::CloseDocument, "Close Document", "");
    set(MenuAction::ExportAs, "Export As...", "");
    set(MenuAction::ExportStates, "Export Comps / Layers To Files...", "");

    // **Quit.** No key equivalent and omitted from File on a platform whose
    // own menu bar carries an application menu -- both for the same reason,
    // and it is worth being explicit because "add Quit to File" is the first
    // thing a native port does and the first thing that goes wrong.
    //
    // SDL3 installs the standard macOS application menu itself, inside
    // `SDL_Init(SDL_INIT_VIDEO)` (`Cocoa_RegisterApp()` ->
    // `CreateApplicationMenus()`, guarded on `[NSApp mainMenu] == nil`), and
    // that menu already ends with "Quit naturalPaint  ⌘Q". A File > Quit
    // beside it is a second Quit item in the same bar; claiming ⌘Q for it
    // would additionally be claiming a chord another menu item already owns,
    // which AppKit resolves by first-match and nobody can predict by reading.
    set(MenuAction::Quit, "Quit", "Cmd+Q");
    table[static_cast<size_t>(MenuAction::Quit)].omitWhenNativeAppMenu = true;

    set(MenuAction::ClearCanvas, "Clear Canvas", "Cmd+K",
        MenuKeyEquivalent{'k', kMenuModCmd, "clear_canvas"});

    family(MenuAction::LayerCommandItem);
    family(MenuAction::LayerSetCommandItem);
    family(MenuAction::PaintModeItem);
    family(MenuAction::ToolItem);

    // Space, and therefore display-only: a bare-letter key equivalent on the
    // main menu swallows that key application-wide, including inside every
    // text field. Space is also the pan gesture.
    set(MenuAction::PauseSolver, "Pause solver", "Space");
    set(MenuAction::ReloadShaders, "Reload shaders", "Cmd+R",
        MenuKeyEquivalent{'r', kMenuModCmd, "reload_shaders"});

    set(MenuAction::FitToWindow, "Fit to Window", "Cmd+0",
        MenuKeyEquivalent{'0', kMenuModCmd, "fit_window"});
    set(MenuAction::Zoom100, "100%", "Cmd+1",
        MenuKeyEquivalent{'1', kMenuModCmd, "zoom_100"});
    set(MenuAction::ZoomIn, "Zoom In", "Cmd+=",
        MenuKeyEquivalent{'=', kMenuModCmd, "zoom_in"});
    set(MenuAction::ZoomOut, "Zoom Out", "Cmd+-",
        MenuKeyEquivalent{'-', kMenuModCmd, "zoom_out"});

    // The three view chords with no Command modifier. `docs/shortcuts.md` §3
    // assigns them and `keymaps/default.json` binds them; both stay true,
    // because SDL keeps delivering the key -- it is only the *menu* that
    // declines to claim it.
    set(MenuAction::MirrorX, "Mirror Left/Right", "F");
    set(MenuAction::MirrorY, "Mirror Up/Down", "Shift+F");
    set(MenuAction::ResetRotation, "Reset Rotation", "Shift+R");

    set(MenuAction::GrayscalePreview, "Grayscale Preview", "Cmd+Y",
        MenuKeyEquivalent{'y', kMenuModCmd, "toggle_grayscale"});

    // Rulers has no shortcut string, and the reason is a spec conflict rather
    // than an oversight: `docs/shortcuts.md` §3 assigns rulers ⌘R, but ⌘R is
    // already bound to `reload_shaders` (main.cpp's dispatch carries the full
    // argument). Menu-only until a product decision resolves it -- advertising
    // a chord that resolves to something else would be worse than none.
    set(MenuAction::Rulers, "Rulers", "");
    set(MenuAction::Navigator, "Navigator", "");
    set(MenuAction::Guides, "Guides", "Cmd+;",
        MenuKeyEquivalent{';', kMenuModCmd, "toggle_guides"});
    set(MenuAction::AddGuide, "Add Guide...", "");
    set(MenuAction::ClearGuides, "Clear Guides", "");
    set(MenuAction::Grid, "Grid", "Cmd+'",
        MenuKeyEquivalent{'\'', kMenuModCmd, "toggle_grid"});
    set(MenuAction::Snap, "Snap", "Cmd+Shift+;",
        MenuKeyEquivalent{';', kMenuModCmd | kMenuModShift, "toggle_snapping"});

    set(MenuAction::ImGuiDemo, "ImGui demo", "");
    family(MenuAction::ActivateDocument);

    // --- Filter -------------------------------------------------------
    set(MenuAction::GaussianBlur, "Gaussian Blur...", "");
    set(MenuAction::Sharpen, "Sharpen...", "");
    set(MenuAction::UnsharpMask, "Unsharp Mask...", "");
    set(MenuAction::AddNoise, "Add Noise...", "");

    // --- Image ----------------------------------------------------------
    set(MenuAction::ImageSize, "Image Size...", "");
    set(MenuAction::CanvasSize, "Canvas Size...", "");
    return true;
  }();
  (void)built;
  return table;
}

// A `Command` row from the table, with enabled/checked applied.
MenuNode item(MenuAction action, bool enabled = true, bool checked = false,
              MenuNodeKind kind = MenuNodeKind::Command) {
  const MenuItemSpec& s = menuItemSpec(action);
  MenuNode n;
  n.kind = kind;
  n.action = action;
  n.label = s.label;
  n.shortcutText = s.shortcutText;
  n.keyEquivalent = s.keyEquivalent;
  n.enabled = enabled;
  n.checked = checked;
  return n;
}

// A `Check` row -- an item that both acts and shows its current state.
MenuNode check(MenuAction action, bool checked, bool enabled = true) {
  return item(action, enabled, checked, MenuNodeKind::Check);
}

MenuNode separator() {
  MenuNode n;
  n.kind = MenuNodeKind::Separator;
  return n;
}

MenuNode note(std::string text) {
  MenuNode n;
  n.kind = MenuNodeKind::Note;
  n.label = std::move(text);
  return n;
}

MenuNode submenu(std::string label, bool enabled = true) {
  MenuNode n;
  n.kind = MenuNodeKind::Submenu;
  n.label = std::move(label);
  n.enabled = enabled;
  return n;
}

// One family loop: `entries` rows, all carrying `action` and their index as
// `param`, with the rules the caller asked for. Written once because all six
// families are the same shape, and because a hand-rolled sixth copy is where
// the index and the label come apart.
void appendFamily(std::vector<MenuNode>& into, MenuAction action,
                  const std::vector<MenuFamilyEntry>& entries, MenuNodeKind kind) {
  for (size_t i = 0; i < entries.size(); ++i) {
    MenuNode n;
    n.kind = kind;
    n.action = action;
    n.param = static_cast<int>(i);
    n.label = entries[i].label;
    n.tooltip = entries[i].tooltip;
    n.enabled = entries[i].enabled;
    n.checked = entries[i].checked;
    into.push_back(std::move(n));
    if (entries[i].separatorAfter) into.push_back(separator());
  }
}

// ---- the published snapshot, and the queue the native backend feeds -------
//
// One mutex for both. They are touched a handful of times a second at most,
// and two locks whose ordering nobody has written down is a worse trade than
// a moment of contention that cannot be measured.
std::mutex g_menuMutex;
std::vector<MenuNode> g_publishedMenus;
uint64_t g_shapeGeneration = 0;
std::vector<std::pair<MenuAction, int>> g_pendingActions;

// The shape signature: everything a native backend would have to rebuild an
// `NSMenu` for. Deliberately excludes `enabled` and `checked`, which AppKit
// re-asks for through `validateMenuItem:` on every menu open -- folding them
// in here would rebuild the entire menu bar every time the user moved the
// mouse over a layer.
void hashShape(const std::vector<MenuNode>& nodes, uint64_t& h) {
  auto mix = [&h](uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  };
  for (const MenuNode& n : nodes) {
    mix(static_cast<uint64_t>(n.kind));
    mix(static_cast<uint64_t>(n.action));
    mix(static_cast<uint64_t>(n.param));
    for (const char c : n.label) mix(static_cast<uint64_t>(static_cast<unsigned char>(c)));
    for (const char c : n.shortcutText) mix(static_cast<uint64_t>(static_cast<unsigned char>(c)));
    mix(static_cast<uint64_t>(n.keyEquivalent.key));
    mix(static_cast<uint64_t>(n.keyEquivalent.mods));
    mix(n.children.size());
    hashShape(n.children, h);
  }
}

// Depth-first search for one id. Linear over a tree of a few hundred nodes,
// called when a menu opens -- which is to say, never in any quantity.
const MenuNode* findNode(const std::vector<MenuNode>& nodes, MenuAction action, int param) {
  for (const MenuNode& n : nodes) {
    if (n.action == action && n.param == param &&
        (n.kind == MenuNodeKind::Command || n.kind == MenuNodeKind::Check))
      return &n;
    if (const MenuNode* found = findNode(n.children, action, param)) return found;
  }
  return nullptr;
}

}  // namespace

const MenuItemSpec& menuItemSpec(MenuAction action) noexcept {
  static const MenuItemSpec kNone{};
  const size_t i = static_cast<size_t>(action);
  if (i >= static_cast<size_t>(MenuAction::Count)) return kNone;
  return specTable()[i];
}

const char* menuActionName(MenuAction action) noexcept {
  switch (action) {
    case MenuAction::None: return "None";
    case MenuAction::NewCanvas: return "NewCanvas";
    case MenuAction::NewDocument: return "NewDocument";
    case MenuAction::Open: return "Open";
    case MenuAction::OpenRecentEntry: return "OpenRecentEntry";
    case MenuAction::ClearRecentMenu: return "ClearRecentMenu";
    case MenuAction::ImportImage: return "ImportImage";
    case MenuAction::Save: return "Save";
    case MenuAction::SaveAs: return "SaveAs";
    case MenuAction::SaveCopy: return "SaveCopy";
    case MenuAction::SaveIncremental: return "SaveIncremental";
    case MenuAction::RecoverDocuments: return "RecoverDocuments";
    case MenuAction::Revert: return "Revert";
    case MenuAction::DuplicateDocument: return "DuplicateDocument";
    case MenuAction::CloseDocument: return "CloseDocument";
    case MenuAction::ExportAs: return "ExportAs";
    case MenuAction::ExportStates: return "ExportStates";
    case MenuAction::Quit: return "Quit";
    case MenuAction::ClearCanvas: return "ClearCanvas";
    case MenuAction::LayerCommandItem: return "LayerCommandItem";
    case MenuAction::LayerSetCommandItem: return "LayerSetCommandItem";
    case MenuAction::PaintModeItem: return "PaintModeItem";
    case MenuAction::ToolItem: return "ToolItem";
    case MenuAction::PauseSolver: return "PauseSolver";
    case MenuAction::ReloadShaders: return "ReloadShaders";
    case MenuAction::FitToWindow: return "FitToWindow";
    case MenuAction::Zoom100: return "Zoom100";
    case MenuAction::ZoomIn: return "ZoomIn";
    case MenuAction::ZoomOut: return "ZoomOut";
    case MenuAction::MirrorX: return "MirrorX";
    case MenuAction::MirrorY: return "MirrorY";
    case MenuAction::ResetRotation: return "ResetRotation";
    case MenuAction::GrayscalePreview: return "GrayscalePreview";
    case MenuAction::Rulers: return "Rulers";
    case MenuAction::Navigator: return "Navigator";
    case MenuAction::Guides: return "Guides";
    case MenuAction::AddGuide: return "AddGuide";
    case MenuAction::ClearGuides: return "ClearGuides";
    case MenuAction::Grid: return "Grid";
    case MenuAction::Snap: return "Snap";
    case MenuAction::ImGuiDemo: return "ImGuiDemo";
    case MenuAction::ActivateDocument: return "ActivateDocument";
    case MenuAction::GaussianBlur: return "GaussianBlur";
    case MenuAction::Sharpen: return "Sharpen";
    case MenuAction::UnsharpMask: return "UnsharpMask";
    case MenuAction::AddNoise: return "AddNoise";
    case MenuAction::ImageSize: return "ImageSize";
    case MenuAction::CanvasSize: return "CanvasSize";
    case MenuAction::Count: break;
  }
  // Not a fallback string: reaching this means an enumerator was added without
  // a name, and a plausible-looking "Unknown" in --selftest output is how that
  // ships. ui/Fonts' own rule, one module over.
  return "<UNNAMED MenuAction -- add it to menuActionName()>";
}

MenuEffect menuActionEffect(MenuAction action) noexcept {
  switch (action) {
    // **The one that matters.** See MenuEffect::QuitRequest's comment: this
    // must never become `Inline` with an `AppState::quit` behind it, and must
    // never be handed to Cocoa's `terminate:`.
    case MenuAction::Quit:
      return MenuEffect::QuitRequest;

    // The four modals. Each sets a request flag that the next frame reads,
    // because an ImGui popup cannot be opened from a native menu callback --
    // and, for the three that predate the native backend, because a popup
    // opened inside `BeginMenu()` is opened against the menu's own ID stack.
    case MenuAction::ExportAs:
    case MenuAction::ExportStates:
    case MenuAction::RecoverDocuments:
    case MenuAction::AddGuide:
      return MenuEffect::Deferred;

    // Also modals, but of a different sort: these raise the file-path dialog
    // or the revert confirmation through the same one-flag-per-frame route.
    case MenuAction::Open:
    case MenuAction::ImportImage:
    case MenuAction::SaveAs:
    case MenuAction::SaveCopy:
    case MenuAction::Revert:
      return MenuEffect::Deferred;

    // The Filter and Image dialogs, for the identical reason: each is an
    // `ImGui::BeginPopupModal()` a native menu's AppKit-thread callback
    // cannot open directly. `performMenuAction()` sets a request flag; the
    // next ImGui frame opens the popup, exactly as `ExportAs` and
    // `AddGuide` already do above.
    case MenuAction::GaussianBlur:
    case MenuAction::Sharpen:
    case MenuAction::UnsharpMask:
    case MenuAction::AddNoise:
    case MenuAction::ImageSize:
    case MenuAction::CanvasSize:
      return MenuEffect::Deferred;

    default:
      return MenuEffect::Inline;
  }
}

std::vector<MenuNode> buildMenuModel(const MenuContext& ctx) {
  std::vector<MenuNode> bar;

  // ------------------------------------------------------------------ File
  {
    MenuNode file = submenu("File");
    std::vector<MenuNode>& f = file.children;

    // "New Canvas" is the *canvas* command it has always been -- it clears the
    // solver texture. "New Document" below is a different thing entirely, and
    // the two are deliberately not merged: the canvas is not a document in
    // this build, and a menu that implied otherwise would be the first place
    // that gap got papered over.
    f.push_back(item(MenuAction::NewCanvas));
    f.push_back(separator());

    f.push_back(item(MenuAction::NewDocument));
    f.push_back(item(MenuAction::Open));

    // Open Recent is the model's own worked example of a live predicate: the
    // submenu is disabled when the list is empty, so it greys out rather than
    // opening onto nothing. A missing entry is shown, greyed, with the reason
    // in its tooltip -- never dropped behind the user's back
    // (app/DocumentLifecycle.hpp argues why).
    {
      MenuNode recent = submenu("Open Recent", !ctx.recentDocuments.empty());
      appendFamily(recent.children, MenuAction::OpenRecentEntry, ctx.recentDocuments,
                   MenuNodeKind::Command);
      recent.children.push_back(separator());
      recent.children.push_back(item(MenuAction::ClearRecentMenu));
      f.push_back(std::move(recent));
    }

    // Beside Open rather than beside Export, because it is the other half of
    // the same idea: Open turns a file into a document, Import puts a file
    // *into* the document already open, as a new RGB layer on top. Disabled
    // with no document, and the tooltip says which of the two commands the
    // user wanted -- a greyed item with no explanation is how a user concludes
    // the feature is missing rather than inapplicable.
    {
      MenuNode n = item(MenuAction::ImportImage, ctx.hasDocument);
      if (!ctx.hasDocument)
        n.tooltip =
            "Import adds an image to an open document. With nothing open, use Open... "
            "instead, which turns the image into a document of its own.";
      f.push_back(std::move(n));
    }

    f.push_back(separator());
    f.push_back(item(MenuAction::Save, ctx.hasPath));
    f.push_back(item(MenuAction::SaveAs, ctx.hasDocument));
    f.push_back(item(MenuAction::SaveCopy, ctx.hasDocument));
    f.push_back(item(MenuAction::SaveIncremental, ctx.hasPath));

    f.push_back(separator());
    // Always enabled, even with nothing to offer: "are there unfinished
    // sessions?" is a question a user who has just had a crash will ask, and a
    // greyed-out item answers it ambiguously.
    f.push_back(item(MenuAction::RecoverDocuments));

    f.push_back(separator());
    f.push_back(item(MenuAction::Revert, ctx.hasPath));
    f.push_back(item(MenuAction::DuplicateDocument, ctx.hasDocument));
    f.push_back(item(MenuAction::CloseDocument, ctx.hasDocument));

    f.push_back(separator());
    f.push_back(item(MenuAction::ExportAs));
    f.push_back(item(MenuAction::ExportStates));

    // See MenuItemSpec::omitWhenNativeAppMenu. Under a native menu bar the
    // separator goes with the item, otherwise the File menu ends on a rule.
    if (!(ctx.nativeAppMenuPresent && menuItemSpec(MenuAction::Quit).omitWhenNativeAppMenu)) {
      f.push_back(separator());
      f.push_back(item(MenuAction::Quit));
    }

    bar.push_back(std::move(file));
  }

  // ------------------------------------------------------------------ Edit
  {
    MenuNode edit = submenu("Edit");
    edit.children.push_back(item(MenuAction::ClearCanvas));
    bar.push_back(std::move(edit));
  }

  // ----------------------------------------------------------------- Layer
  //
  // The whole menu is `app::allLayerCommands()` walked in order -- resolved by
  // the caller, but still one list -- so a command added to that list without a
  // menu entry is impossible. That is the failure this menu exists to have
  // fixed: five built features with no entry point at all.
  {
    MenuNode layer = submenu("Layer");
    std::vector<MenuNode>& l = layer.children;
    if (ctx.layerCommands.empty()) {
      l.push_back(note("(no document open)"));
    } else {
      // The row every one of these acts on, named rather than assumed: the
      // menu bar is a long way from the panel and "which layer is this about"
      // is otherwise invisible from here.
      l.push_back(note(ctx.activeLayerTitle));
      l.push_back(separator());
      appendFamily(l, MenuAction::LayerCommandItem, ctx.layerCommands, MenuNodeKind::Check);

      l.push_back(separator());
      {
        MenuNode sel = submenu("Selection");
        sel.children.push_back(note(ctx.layerSelectionNote));
        sel.children.push_back(separator());
        appendFamily(sel.children, MenuAction::LayerSetCommandItem, ctx.layerSetCommands,
                     MenuNodeKind::Command);
        l.push_back(std::move(sel));
      }
      l.push_back(separator());
      l.push_back(note("refusals appear in the LAYERS panel"));
    }
    bar.push_back(std::move(layer));
  }

  // ------------------------------------------------------------------ Image
  //
  // PRD D17, through ops/DocumentTransform (docs/reachability-audit.md C1).
  // Photoshop-style and deliberately small: two of D17's three operations,
  // both reached through app/FilterOps.hpp's `applyImageSize()` /
  // `applyCanvasSize()`. Crop and rotate/flip-canvas are not here -- see
  // `MenuAction::ImageSize`'s own comment in the header for why an unwired
  // item is left out of the tree rather than added disabled.
  {
    MenuNode image = submenu("Image");
    image.children.push_back(item(MenuAction::ImageSize, ctx.hasDocument));
    image.children.push_back(item(MenuAction::CanvasSize, ctx.hasDocument));
    bar.push_back(std::move(image));
  }

  // ---------------------------------------------------------------- Medium
  {
    MenuNode medium = submenu("Medium");
    appendFamily(medium.children, MenuAction::PaintModeItem, ctx.paintModes,
                 MenuNodeKind::Check);
    medium.children.push_back(separator());
    medium.children.push_back(note("switching clears the canvas"));
    bar.push_back(std::move(medium));
  }

  // --------------------------------------------------------------- Goodies
  {
    MenuNode goodies = submenu("Goodies");
    appendFamily(goodies.children, MenuAction::ToolItem, ctx.tools, MenuNodeKind::Check);
    goodies.children.push_back(separator());
    goodies.children.push_back(check(MenuAction::PauseSolver, ctx.paused));
    goodies.children.push_back(item(MenuAction::ReloadShaders));
    bar.push_back(std::move(goodies));
  }

  // ---------------------------------------------------------------- Filter
  //
  // ops/Blur + ops/Filters, through app/FilterOps.hpp (PRD D4/D5;
  // docs/reachability-audit.md C1). Four items share one enable predicate
  // and one refusal sentence -- `ctx.filterLayerUsable` /
  // `ctx.filterRefusalNote` -- because all four ask the identical question
  // of the active layer ("can it take a pixel op"), the same one the paint
  // bucket and the gradient already ask via `PixelOpRefusal`.
  //
  // Grouped as `ops/Filters.hpp` itself groups them: the blur-based
  // sharpening pair together, Add Noise set apart -- Sharpen is
  // `unsharpMaskTiles()` with the radius fixed (that header's own section 3
  // says so), so it sits beside Unsharp Mask rather than beside Gaussian
  // Blur, which is a different engine entirely.
  {
    MenuNode filter = submenu("Filter");
    std::vector<MenuNode>& flt = filter.children;
    auto filterItem = [&](MenuAction action) {
      MenuNode n = item(action, ctx.filterLayerUsable);
      if (!ctx.filterLayerUsable) n.tooltip = ctx.filterRefusalNote;
      return n;
    };
    flt.push_back(filterItem(MenuAction::GaussianBlur));
    flt.push_back(separator());
    flt.push_back(filterItem(MenuAction::Sharpen));
    flt.push_back(filterItem(MenuAction::UnsharpMask));
    flt.push_back(separator());
    flt.push_back(filterItem(MenuAction::AddNoise));
    bar.push_back(std::move(filter));
  }

  // ------------------------------------------------------------------ View
  {
    MenuNode view = submenu("View");
    std::vector<MenuNode>& v = view.children;
    v.push_back(item(MenuAction::FitToWindow));
    v.push_back(item(MenuAction::Zoom100));
    v.push_back(item(MenuAction::ZoomIn));
    v.push_back(item(MenuAction::ZoomOut));
    v.push_back(separator());
    v.push_back(check(MenuAction::MirrorX, ctx.mirrorX));
    v.push_back(check(MenuAction::MirrorY, ctx.mirrorY));
    v.push_back(item(MenuAction::ResetRotation));
    v.push_back(separator());
    v.push_back(check(MenuAction::GrayscalePreview, ctx.grayscale));
    v.push_back(separator());
    v.push_back(check(MenuAction::Rulers, ctx.showRulers));
    v.push_back(check(MenuAction::Navigator, ctx.showNavigator));
    v.push_back(check(MenuAction::Guides, ctx.showGuides));
    v.push_back(item(MenuAction::AddGuide));
    v.push_back(item(MenuAction::ClearGuides, ctx.hasGuides));
    v.push_back(check(MenuAction::Grid, ctx.showGrid));
    v.push_back(check(MenuAction::Snap, ctx.snappingEnabled));
    bar.push_back(std::move(view));
  }

  // ---------------------------------------------------------------- Window
  {
    MenuNode window = submenu("Window");
    window.children.push_back(check(MenuAction::ImGuiDemo, ctx.showDemo));
    window.children.push_back(separator());
    // The open documents, and which one the lifecycle commands act on. The tab
    // strip is this list drawn differently, not a different list.
    if (ctx.openDocuments.empty()) {
      window.children.push_back(note("(no documents open)"));
    } else {
      appendFamily(window.children, MenuAction::ActivateDocument, ctx.openDocuments,
                   MenuNodeKind::Check);
    }
    bar.push_back(std::move(window));
  }

  return bar;
}

void enqueueMenuAction(MenuAction action, int param) {
  std::lock_guard<std::mutex> lock(g_menuMutex);
  g_pendingActions.emplace_back(action, param);
}

bool dequeueMenuAction(MenuAction* action, int* param) {
  std::lock_guard<std::mutex> lock(g_menuMutex);
  if (g_pendingActions.empty()) return false;
  // FIFO. The user picked these in an order and two picks in one frame is not
  // impossible (a native menu accepts a click while the frame loop is between
  // polls); reversing them would be a bug nobody could reproduce on purpose.
  if (action != nullptr) *action = g_pendingActions.front().first;
  if (param != nullptr) *param = g_pendingActions.front().second;
  g_pendingActions.erase(g_pendingActions.begin());
  return true;
}

void publishMenuModel(std::vector<MenuNode> menus) {
  uint64_t shape = 0xcbf29ce484222325ull;
  hashShape(menus, shape);
  std::lock_guard<std::mutex> lock(g_menuMutex);
  if (shape != g_shapeGeneration) g_shapeGeneration = shape;
  g_publishedMenus = std::move(menus);
}

uint64_t menuModelShapeGeneration() {
  std::lock_guard<std::mutex> lock(g_menuMutex);
  return g_shapeGeneration;
}

std::vector<MenuNode> publishedMenuModel() {
  std::lock_guard<std::mutex> lock(g_menuMutex);
  return g_publishedMenus;
}

bool menuItemEnabled(MenuAction action, int param) {
  std::lock_guard<std::mutex> lock(g_menuMutex);
  const MenuNode* n = findNode(g_publishedMenus, action, param);
  return n != nullptr && n->enabled;
}

bool menuItemChecked(MenuAction action, int param) {
  std::lock_guard<std::mutex> lock(g_menuMutex);
  const MenuNode* n = findNode(g_publishedMenus, action, param);
  return n != nullptr && n->checked;
}

}  // namespace np
