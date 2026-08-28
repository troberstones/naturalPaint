#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ui/MenuModel -- what the menus ARE, separated from how they are drawn.
//
// --- The defect this module repairs ----------------------------------------
//
// Every menu entry in this application used to be one `ImGui::MenuItem()` call
// that did four things at once: it *declared* the item (label, shortcut text,
// enabled state, checked state), it *decided* whether the item applied right
// now, it *drew* the item, and -- in the body of the `if` -- it *performed the
// action*. Forty-one such call sites sat inside `ui/MacPaintUI.cpp`'s
// `BeginMainMenuBar()` block, and the only thing that could read any of them
// was Dear ImGui, mid-frame.
//
// That is fine while there is exactly one menu bar. It stops being fine the
// moment a second backend exists, because a native `NSMenu` is built **once,
// out of band**, and calls back on the AppKit main thread when the user picks
// something -- outside any ImGui frame, with no `if` to be the body of.
//
// The naive port is to write the actions out a second time in the native
// backend. That is the failure this file exists to prevent: two copies of
// "what File > Save does" drift within a release, and the way a user meets the
// drift is that the menu bar at the top of the screen and the menu bar inside
// the window do different things to their document.
//
// --- The three-part split ---------------------------------------------------
//
//  1. **This file** describes the menu tree as data: a stable `MenuAction` id,
//     a label, an optional shortcut, and resolved enabled/checked state.
//     Free of ImGui, free of AppKit, free of `AppState`, free of
//     `core::Document` -- see `MenuContext` below for why that last one
//     matters more than it looks.
//  2. **`performMenuAction()`** is the single place an action is carried out.
//     It is *declared* here and *defined* in `ui/MacPaintUI.cpp`, deliberately:
//     roughly half the actions poke that file's own file-local dialog flags
//     (`g_exportAsRequested`, `g_docPathAction`, `g_docStatus` ...), and moving
//     nine globals into `AppState` to satisfy a header would have been a much
//     larger and much riskier change than declaring one function. Both
//     backends call this and nothing else, so there is exactly one copy of
//     every action.
//  3. **The backends** -- `ui/MacPaintUI.cpp`'s ImGui walker and
//     `ui/MacNativeMenu.mm`'s AppKit builder -- render the tree and, on a
//     click, name an id. Neither contains an action.
//
// --- Why `MenuContext` holds no `Document` ----------------------------------
//
// The obvious shape for the context is "a pointer to the live `Document` plus
// the active layer index", and then `buildMenuModel()` calls
// `app::layerCommandAvailable()` itself. It was rejected for two reasons, the
// second of which is the real one:
//
//   * `core/Document.hpp` would then be in this header, and this header is
//     included by an **Objective-C++** translation unit. Dragging the document
//     model, the tile store and everything they include through `clang -x
//     objective-c++` is a compile-time cost with no benefit.
//   * More importantly, a native menu backend must be able to answer "is this
//     item enabled?" from a snapshot, at `menuNeedsUpdate:` time, on the
//     AppKit main thread -- and a raw `const Document*` captured a frame ago
//     is a **dangling pointer** waiting for the user to close a document with
//     a menu open. Availability is therefore *resolved by the caller*, against
//     the live document, inside the frame, and arrives here as plain `bool`s.
//
// So the layer-availability *rule* still lives in `app/LayerEditor` and
// `core/LayerSetOps` where it belongs -- this file never re-implements it, it
// only carries the answer. What this file owns are the predicates that are
// genuinely about the *menu*: "Open Recent is disabled when the list is
// empty", "Save needs a path", "Import needs a document".
//
// --- Enabled and checked, for two backends with different clocks ------------
//
// ImGui asks every frame. AppKit asks its delegate when the menu is about to
// open. A model that served only one of them would make the other lie: an
// ImGui-shaped model (re-evaluate constantly) makes the native menu show
// last-frame's state, and an AppKit-shaped model (evaluate on open) makes the
// ImGui menu stale for as long as it is held down.
//
// The resolution is that `buildMenuModel()` is a **pure function of a
// snapshot** and is cheap enough to call every frame. The ImGui backend calls
// it and draws the result immediately. The native backend has the frame loop
// publish the result (`publishMenuModel()`) and reads the published copy from
// `menuItemEnabled()` / `menuItemChecked()` when AppKit asks. Neither backend
// evaluates a predicate of its own, so neither can disagree with the other by
// more than one frame -- and one frame is the same staleness the ImGui bar has
// always had.
namespace np {

// ---------------------------------------------------------------- the ids
//
// One enumerator per menu action this application can perform, and the
// enumerators are the stable identity: a backend names one of these, never a
// label, so renaming "Goodies" to "Tools" cannot silently unwire an item.
//
// **There are exactly 41 of them, and that is not a coincidence.** The
// `BeginMainMenuBar()` block this replaced contained exactly 41
// `ImGui::MenuItem()` call sites (ui/MacPaintUI.cpp, the block that ran from
// the File menu to `EndMainMenuBar()`), and the extraction is one-to-one:
// every call site became one enumerator, and no enumerator was invented.
// `app/selftest/MenuModel.cpp` asserts the count, so an item quietly dropped
// during a later edit fails the suite instead of vanishing from the product.
//
// Six of the 41 are **families** rather than single items -- `OpenRecentEntry`,
// `LayerCommandItem`, `LayerSetCommandItem`, `PaintModeItem`, `ToolItem` and
// `ActivateDocument` were loops over a list in the original block, and are
// loops here too. They carry an integer `param` (the index within the family);
// every other action ignores `param` entirely.
enum class MenuAction : uint16_t {
  None = 0,

  // --- File ---------------------------------------------------------------
  NewCanvas,           // clears the solver texture; NOT "new document"
  NewDocument,
  Open,
  OpenRecentEntry,     // family: param = index into the recent-documents list
  ClearRecentMenu,
  ImportImage,
  Save,
  SaveAs,
  SaveCopy,
  SaveIncremental,
  RecoverDocuments,
  Revert,
  DuplicateDocument,
  CloseDocument,
  ExportAs,
  ExportStates,
  Quit,

  // --- Edit ---------------------------------------------------------------
  //
  // D1 and D2 of docs/reachability-audit.md. `ClearCanvas` was the entire
  // Edit menu; everything from `Undo` to `DeleteSelection` below is new.
  //
  // Undo/Redo were reachable from nowhere but the mouse -- no keymap action
  // name, no menu enumerator -- despite PRD O1/R2 both being P0. The other
  // nine were the opposite defect: fully written, fully keyboard-reachable
  // (main.cpp's dispatch already resolves every one of them), and invisible
  // the moment `keymap.loadFromFile()` fails, because a stderr line was the
  // only thing standing between a user and Cut, Copy, Copy Merged, Paste,
  // Select All, Deselect, Reselect, Invert Selection and Delete.
  //
  // `SelectAll` through `DeleteSelection` belong, in Photoshop, to a
  // **Select** menu this application does not have yet -- `track7/selectmenu`
  // is building one in parallel. They sit in Edit for now because File, Edit
  // and Goodies are the only menus this step may touch; a later step that
  // adds Select is expected to relocate them, and nothing here should make
  // that relocation harder than moving five enumerators between two `switch`
  // blocks.
  Undo,
  Redo,
  // The interactive transform (app/TransformSession). Starts a session on the
  // active layer, or on the pixels under a selection when there is one; the
  // gizmo then owns the canvas until Return commits or Escape cancels. Sits
  // in Edit because that is where Photoshop's Free Transform lives and this
  // build has no Transform submenu to put it in -- if one is ever added, this
  // is one enumerator to move, the same relocation note the selection items
  // below already carry.
  FreeTransform,
  Cut,
  Copy,
  CopyMerged,
  Paste,
  DeleteSelection,
  SelectAll,
  Deselect,
  Reselect,
  InvertSelection,
  ClearCanvas,

  // --- Layer --------------------------------------------------------------
  LayerCommandItem,     // family: param = index into app::allLayerCommands()
  LayerSetCommandItem,  // family: param = index into core::allLayerSetCommands()

  // --- Select ---------------------------------------------------------------
  //
  // docs/reachability-audit.md C5: five engines (PRD E4/E8's grow, shrink and
  // feather; PRD E9's colour range and luminance range) proven only by
  // `--selftest`, because there was no Select menu to reach them from. Select
  // All / Deselect / Reselect / Invert are NOT here -- they are the Edit
  // menu's (this codebase already dispatches them through
  // `AppState::requestSelectAll` &c., ui/MacPaintUI.cpp's "selection and
  // clipboard commands" block), and duplicating them in a second menu would
  // give a user two places that could disagree about what is selected.
  //
  // Each of the five opens a small modal (a radius, or a colour/band plus
  // tolerance) rather than acting immediately -- unlike Invert, which has
  // nothing to ask the user and needs no dialog.
  SelectGrow,
  SelectShrink,
  SelectFeather,
  SelectColourRange,
  SelectLuminanceRange,

  // Pops one entry off `OpenDocument::refineUndoStack` (app/DocumentLifecycle.hpp).
  // **Deliberately not routed through core::History's Undo/Redo pair.**
  // `core::HistoryEntry` holds nothing but a `core::Document`
  // (core/History.hpp), by the same deliberate choice `OpenDocument::selection`'s
  // own comment makes for the identical reason: a selection is session state,
  // not document data, and folding it into the pixel history would make the
  // History panel show a step with no pixel change, and would make an
  // ordinary pixel Undo silently revert a selection drawn afterwards. The
  // five operations above change only the selection, so they get their own
  // one-entry-per-op undo instead, in the same shape `lastDeselected` already
  // uses for Reselect.
  SelectUndoRefine,

  // --- Medium / Goodies ---------------------------------------------------
  PaintModeItem,        // family: param = the PaintMode's integer value
  ToolItem,             // family: param = the Tool's integer value
  PauseSolver,
  ReloadShaders,

  // --- View ---------------------------------------------------------------
  FitToWindow,
  Zoom100,
  ZoomIn,
  ZoomOut,
  MirrorX,
  MirrorY,
  ResetRotation,
  ResetView,
  GrayscalePreview,
  Rulers,
  Navigator,
  Guides,
  AddGuide,
  ClearGuides,
  Grid,
  Snap,

  // --- Window -------------------------------------------------------------
  ImGuiDemo,
  ActivateDocument,     // family: param = index into the open-document session

  // --- Filter ---------------------------------------------------------
  //
  // ops/Blur + ops/Filters (docs/reachability-audit.md C1: ~93 tested entry
  // points across six ops/ modules with no UI path to any of them). Four of
  // them, chosen for depth over breadth -- see app/FilterOps.hpp's header
  // for why these four and app/selftest/FilterMenu.cpp for the assertions
  // that pin each one to its own engine call and its own parameters.
  GaussianBlur,
  Sharpen,
  UnsharpMask,
  AddNoise,

  // --- Image ------------------------------------------------------------
  //
  // ops/DocumentTransform, Photoshop-style: geometry that changes the
  // document's own extent, kept apart from Filter's pixel ops for the reason
  // app/FilterOps.hpp states -- these move every layer, including locked
  // ones, and refuse for a document-shaped reason (a zero extent) rather
  // than a layer-shaped one (`PixelOpRefusal`). Two items, deliberately: PRD
  // D17's crop and its rotate/flip canvas are left out, not left dead --
  // see docs/reachability-audit.md C1's own instruction that an unwired
  // operation must be absent from the menu, not present and inert.
  ImageSize,
  CanvasSize,

  Count,
};

// How many real actions there are (`Count` minus the `None` sentinel).
constexpr size_t kMenuActionCount = static_cast<size_t>(MenuAction::Count) - 1;

// The enumerator's own spelling, for `--selftest` output and for a native
// backend's accessibility identifiers. Never shown to a user -- labels are
// data on the node, and are localisable in a way an enumerator name is not.
const char* menuActionName(MenuAction action) noexcept;

// ------------------------------------------------------------- the effect
//
// **What performing an action does, declared as data, because one of the three
// answers is a data-loss hazard and a comment is not enough to hold it.**
enum class MenuEffect : uint8_t {
  // Performed there and then. The overwhelming majority: a flag flip, a
  // request bool, a call into `core/LayerOps`.
  Inline,

  // Sets a request flag that the **next** frame's UI reads, because the thing
  // it opens is an ImGui modal.
  //
  // This distinction is not decoration. A native menu item's callback fires on
  // the AppKit main thread, *outside* any ImGui frame -- `ImGui::OpenPopup()`
  // called from there is undefined at best and asserts at worst. The original
  // File menu already knew half of this (`Export As...` set
  // `g_exportAsRequested` rather than calling `OpenPopup()`, because a modal
  // opened inside `BeginMenu()` is opened against the menu's own ID stack);
  // `View > Add Guide...` did not, and called `ImGui::OpenPopup()` inline. It
  // got away with it only because that popup is identified by a global string
  // ID. Under a native backend it would not have got away with it, so it is
  // `Deferred` now like the other two.
  Deferred,

  // **The quit, and the reason this enum exists.**
  //
  // Quitting this application must go through `AppState::requestQuit`, which
  // `main.cpp`'s guard answers against the open documents via
  // `app/QuitSequence` -- Save / Don't Save / Cancel, once per dirty document.
  // `AppState::quit` is a *different* flag that stops the frame loop outright
  // with nothing in the way; it exists for `--screenshot`, and writing it from
  // a menu would discard every unsaved document without a word. That was the
  // shipped behaviour once, and it was repaired by exactly this distinction.
  //
  // A native backend has a second, even quieter way to make the same mistake:
  // wiring Quit to Cocoa's own `@selector(terminate:)`. On this build that
  // happens to survive -- SDL3 subclasses `NSApplication` and overrides
  // `terminate:` to do nothing but `SDL_SendQuit()` -- but it survives by
  // accident of a vendored dependency's implementation detail, and the failure
  // mode if that ever changes is silent, total loss of the user's work. See
  // `MenuItemSpec::mayUseSystemSelector` below, which is how that is nailed
  // down rather than trusted.
  QuitRequest,
};

// The effect of an action. Pure, total, and free of everything -- which is
// what lets `--selftest` assert the Quit routing without an `AppState`, a
// window, a GPU or an `NSApplication`.
MenuEffect menuActionEffect(MenuAction action) noexcept;

// ------------------------------------------------------- key equivalents
//
// Modifier bits for `MenuKeyEquivalent`. Deliberately this file's own bits
// rather than `app/Keymap.hpp`'s: that header takes `SDL_Keymod` and therefore
// drags SDL into every translation unit that includes it, and this one is
// included by Objective-C++. `app/selftest/MenuModel.cpp` converts between the
// two and cross-checks every claimed chord against the real
// `keymaps/default.json`, so the two vocabularies cannot drift unnoticed.
constexpr uint16_t kMenuModCmd     = 1u << 0;
constexpr uint16_t kMenuModShift   = 1u << 1;
constexpr uint16_t kMenuModOption  = 1u << 2;
constexpr uint16_t kMenuModControl = 1u << 3;

// A chord a **native** menu may claim.
//
// --- Why claiming one is a decision and not a formatting detail -------------
//
// An `NSMenuItem` with a key equivalent does not merely *display* the chord --
// it **consumes** it. `NSApplication`'s `sendEvent:` runs
// `performKeyEquivalent:` over the main menu before the event reaches the
// window, so a key equivalent the menu owns never becomes an
// `SDL_EVENT_KEY_DOWN`, never reaches `main.cpp`'s dispatch, and therefore
// never resolves through `keymaps/default.json` at all.
//
// That is fine -- and invisible -- **only when the menu item's action is the
// same action the keymap would have run.** Where they differ, the user presses
// a documented key and something else happens. So every chord claimed here is
// cross-checked against the shipped keymap in `--selftest`.
//
// --- The rule that keeps a bare letter out of a menu ------------------------
//
// `key` is empty for every chord with no Command modifier, and that is
// enforced rather than merely observed. `View > Mirror Left/Right` shows
// **`F`**; `Goodies > Pause solver` shows **`Space`**. Claiming those as key
// equivalents would make the menu bar swallow `F` and the space bar
// **globally** -- including while the user is typing a layer name into a text
// field, and including the space-bar pan gesture, which is the single most
// used key in the application. There is no way for the user to diagnose that;
// the text field simply stops accepting a letter.
//
// Those chords therefore live on as `shortcutText` only. The ImGui bar prints
// them exactly as it always did; the native bar shows no accelerator for them
// and SDL keeps delivering the key. `docs/shortcuts.md` still documents them,
// truthfully, because they still work.
struct MenuKeyEquivalent {
  // The AppKit key-equivalent character, lowercase, or 0 for "claim nothing".
  // Also the SDL3 keycode for every character used here: SDL3 keycodes for
  // printable keys ARE their Unicode codepoints, which is what lets the
  // selftest resolve this straight through `Keymap::resolve()`.
  char key = 0;
  uint16_t mods = 0;

  // The `keymaps/default.json` action name this chord resolves to today, or
  // `nullptr` when the chord is deliberately claimed with no keymap binding
  // behind it. **`nullptr` is a recorded fact, not a shrug** -- the selftest
  // asserts that a chord with a name really does resolve to it and that a
  // chord with `nullptr` really does resolve to nothing.
  const char* keymapAction = nullptr;

  constexpr bool claimed() const noexcept { return key != 0; }
};

// --------------------------------------------------------- the item spec
//
// The *static* half of an item: everything true about it regardless of the
// live state. `buildMenuModel()` pairs one of these with a `MenuContext` to
// produce the `MenuNode` a backend draws.
struct MenuItemSpec {
  MenuAction action = MenuAction::None;

  // The label, for the single (non-family) items. Family items get their
  // labels from `MenuContext`, because only the caller knows them.
  const char* label = "";

  // What the ImGui bar prints in its right-hand column. Display only, and
  // deliberately independent of `keyEquivalent` -- see `MenuKeyEquivalent`.
  const char* shortcutText = "";

  MenuKeyEquivalent keyEquivalent{};

  // **May a native backend wire this item to a system selector?**
  //
  // False for every item in this application, and the field exists so that
  // stays true by assertion rather than by nobody having thought of it. The
  // one item a Mac developer's fingers would reach for a system selector on is
  // Quit (`@selector(terminate:)`), and that is precisely the item where doing
  // so routes around `AppState::requestQuit` and discards unsaved documents.
  //
  // `ui/MacNativeMenu.mm` gives every item it creates the same target and the
  // same selector, which funnels into `performMenuAction()`. The standard
  // macOS application menu -- About, Services, Hide, Hide Others, Show All and
  // the system Quit -- is **SDL3's**, installed by `Cocoa_RegisterApp()`
  // during `SDL_Init(SDL_INIT_VIDEO)`, and this build does not touch it.
  bool mayUseSystemSelector = false;

  // Suppressed on a platform whose native menu bar already provides it.
  //
  // Today this is Quit alone: SDL3's application menu already carries
  // "Quit naturalPaint" with ⌘Q, and a File > Quit beside it would be the
  // second Quit item in the same menu bar -- the exact duplication a native
  // port is expected to produce and expected not to ship.
  bool omitWhenNativeAppMenu = false;
};

// ---------------------------------------------------------- the tree node
enum class MenuNodeKind : uint8_t {
  Command,    // does something when picked
  Check,      // does something when picked, and shows a tick
  Separator,  // a rule
  Note,       // dimmed, unpickable text ("switching clears the canvas")
  Submenu,    // holds `children`
};

// One resolved row. This is what a backend renders; it contains no predicate,
// no pointer into the document model and nothing that can go stale between
// being produced and being drawn.
struct MenuNode {
  MenuNodeKind kind = MenuNodeKind::Command;
  MenuAction action = MenuAction::None;
  int param = 0;

  std::string label;
  std::string shortcutText;
  MenuKeyEquivalent keyEquivalent{};

  // Shown on hover. The original bar used these to explain *why* an item is
  // greyed ("Import adds an image to an open document..."), which is the
  // difference between a user concluding a feature is inapplicable and
  // concluding it is missing. Carried in the model so both backends can offer
  // it rather than only the one that happened to have the call.
  std::string tooltip;

  bool enabled = true;
  bool checked = false;

  std::vector<MenuNode> children;
};

// --------------------------------------------------------- the context
//
// One row of a family whose contents are only known at run time. The caller
// resolves these against the live document/session, inside the frame, and
// hands them over as plain values -- see this file's header comment for why
// the alternative (a `const Document*` on the context) is a dangling pointer
// with a native backend attached to it.
struct MenuFamilyEntry {
  std::string label;
  std::string tooltip;
  bool enabled = true;
  bool checked = false;

  // The original block drew rules at fixed points inside these loops (after
  // `NewAdjustmentLayer`, after `MoveLayerDown`, ...). The grouping is the
  // caller's knowledge -- it comes from the command enums' own order -- so it
  // travels with the row rather than being re-derived here from an index this
  // file would have to keep in step with two enums it does not own.
  bool separatorAfter = false;
};

// Everything `buildMenuModel()` needs, and nothing else. Constructible by
// hand, which is most of why the model was worth extracting: `--selftest`
// builds a menu tree for a state that would otherwise need a document, a
// window, a GPU and a user.
struct MenuContext {
  // --- File ---------------------------------------------------------------
  bool hasDocument = false;
  bool hasPath = false;
  std::vector<MenuFamilyEntry> recentDocuments;

  // --- Edit -----------------------------------------------------------------
  //
  // Resolved by the caller against `core/History`, `core/Clipboard.hpp`'s
  // rules and the active `OpenDocument`, for the identical reason the header
  // comment gives for keeping `Document*` out of this struct entirely: a
  // native menu asks "is this enabled?" from a snapshot, on a thread with no
  // safe way to dereference a pointer into a document that may have closed.
  //
  // Every one of these mirrors a guard that already exists at the one place
  // each command is actually performed
  // (`ui/MacPaintUI.cpp`'s request-flag consumption block for the clipboard
  // nine, `moveHistoryCursor()`'s callers for undo/redo) -- copied rather
  // than shared because the two call sites read a live `OpenDocument&` and
  // this one reads a `bool` a frame old, and folding them into one function
  // would mean giving that function a dangling-pointer problem to solve for
  // no reason.
  bool canUndo = false;
  bool canRedo = false;
  // An active layer exists at all -- Copy's only requirement, since a copy
  // reads and changes nothing.
  bool hasActiveLayer = false;
  // An active layer exists AND is not locked -- Cut's and Delete's
  // requirement, since both mutate it. `core/Clipboard.hpp`'s
  // `cutThroughSelection()` already refuses a locked layer on its own, but a
  // menu item that is clickable and silently does nothing is exactly the
  // "silent no-op" category this audit exists to stop shipping.
  bool hasEditableLayer = false;
  bool clipboardHasContent = false;   // Paste
  bool hasSelection = false;          // Deselect, Invert Selection
  bool hasLastDeselected = false;     // Reselect

  // --- Layer --------------------------------------------------------------
  // Empty `layerCommands` means "no document open", which the Layer menu shows
  // as a note rather than as a list of eighteen dead rows.
  std::string activeLayerTitle;      // "3 · Sketch", or "(no layer selected)"
  std::vector<MenuFamilyEntry> layerCommands;
  std::string layerSelectionNote;    // "2 layer(s) selected, some hidden by the filter"
  std::vector<MenuFamilyEntry> layerSetCommands;

  // --- Select ---------------------------------------------------------------
  //
  // Three bools, not a `const Document*` or a `const Selection*` -- this
  // file's own header explains why (a snapshot the native backend reads
  // later must not be a pointer into something a user can close first).
  //
  // Grow, shrink and feather all take a `const Selection&`
  // (core/SelectionRefine.hpp, ops/Feather.hpp) -- not a `const Selection*`
  // -- so there is no way to hand them "no restriction" (`std::nullopt`).
  // They need an ENGAGED selection to move.
  bool hasEngagedSelection = false;

  // Colour range and luminance range take a `const TileStore&` -- the active
  // layer's own pixels -- and no `Selection` at all (PRD E9: "it takes a
  // colour, not a seed coordinate"). They need an RGB layer to sample, and
  // need no selection already drawn.
  bool hasRgbSource = false;

  // `MenuAction::SelectUndoRefine` -- see that enumerator's own comment for
  // why this is a separate stack from core::History's Undo/Redo.
  bool hasRefineUndo = false;

  // --- Medium / Goodies ---------------------------------------------------
  std::vector<MenuFamilyEntry> paintModes;
  std::vector<MenuFamilyEntry> tools;
  bool paused = false;

  // --- View ---------------------------------------------------------------
  bool mirrorX = false;
  bool mirrorY = false;
  bool grayscale = false;
  bool showRulers = false;
  bool showNavigator = false;
  bool showGuides = false;
  bool showGrid = false;
  bool snappingEnabled = false;
  bool hasGuides = false;           // Clear Guides is dead with none placed

  // --- Window -------------------------------------------------------------
  bool showDemo = false;
  std::vector<MenuFamilyEntry> openDocuments;

  // --- Filter / Image -------------------------------------------------
  //
  // `filterLayerUsable` is `app/StrokeSession.hpp`'s `PixelOpRefusal`
  // resolved against the active layer -- the same predicate the paint
  // bucket and the gradient already gate on -- and `filterRefusalNote` is
  // the sentence to show when it is false, built from the live layer inside
  // the frame that produced this context (this file's own header explains
  // why a `Layer*` cannot be carried on the context itself: a native menu
  // reads this after the frame that built it has ended). All four
  // Filter-menu items share one predicate and one sentence, because they
  // share one question -- "can the active layer take a pixel op" -- and a
  // per-item refusal would only ever repeat the same answer four times.
  //
  // Image-menu items use `hasDocument` alone: `ops/DocumentTransform`'s
  // document-level ops move every layer, including locked ones, so there is
  // no layer-shaped refusal for them to carry.
  bool filterLayerUsable = false;
  std::string filterRefusalNote;

  // True when the platform's own menu bar already carries the standard
  // application menu, so `MenuItemSpec::omitWhenNativeAppMenu` items are left
  // out. Set on macOS by the native backend; false everywhere else.
  bool nativeAppMenuPresent = false;
};

// The whole tree: one `MenuNode` of kind `Submenu` per top-level menu, in bar
// order (File, Edit, Layer, Medium, Goodies, View, Window).
//
// Pure. Called every frame by the ImGui backend and once per publish by the
// native one; it allocates strings and is not on any hot path that matters
// next to submitting a frame of chrome.
std::vector<MenuNode> buildMenuModel(const MenuContext& ctx);

// The static spec for one action, or a spec with `action == None` for a family
// member (whose label is context-supplied) and for `MenuAction::None`.
const MenuItemSpec& menuItemSpec(MenuAction action) noexcept;

// ------------------------------------------------- performing an action
//
// **The one place an action happens.**
//
// Declared here and defined in `ui/MacPaintUI.cpp` -- see this file's header
// comment for why the definition is not here. Both backends route through it;
// neither contains an action of its own, which is the property that makes the
// two menu bars incapable of disagreeing about what a command does.
//
// `param` is the family index for the six family actions and is ignored by the
// other thirty-five. `canvasW`/`canvasH` are the solver's dimensions --
// unused now that `NewDocument` opens ui/NewDocumentDialog.hpp's modal (T9)
// instead of reading them directly, but kept on this signature rather than
// threaded out, since every other action already ignores them the same way.
//
// Safe to call outside an ImGui frame: every action that opens a modal is
// `MenuEffect::Deferred` and sets a flag instead.
struct AppState;
void performMenuAction(AppState& st, MenuAction action, int param, uint32_t canvasW,
                       uint32_t canvasH);

// ------------------------------------------------------- the deferred queue
//
// A native menu's callback arrives on the AppKit main thread, from inside
// `SDL_PollEvent()`'s Cocoa pump -- the same thread as the frame loop, but at a
// point where there is no ImGui frame, no `AppState&` in scope and no canvas
// size to hand `NewDocument`. So the native backend does not perform anything:
// it enqueues an id, and `drawUI()` drains the queue at the top of the next
// frame, where all three of those things exist.
//
// Guarded by a mutex despite being same-thread in this build. The cost is a
// mutex acquisition per menu click, which is free; the alternative is an
// invariant about AppKit's threading that is true today, invisible in the
// code, and catastrophic in exactly the way that is hardest to reproduce.
void enqueueMenuAction(MenuAction action, int param);
bool dequeueMenuAction(MenuAction* action, int* param);

// ------------------------------------------------- the published snapshot
//
// The frame loop publishes the tree it just built; the native backend reads it
// when AppKit asks. See the header comment's "two backends with different
// clocks".
void publishMenuModel(std::vector<MenuNode> menus);

// Bumped whenever the published tree's **shape** changes -- a label, a row
// added or removed, a submenu appearing. Not bumped for enabled/checked, which
// AppKit re-asks for on its own via `validateMenuItem:`.
//
// This is what stops the native backend rebuilding an `NSMenu` sixty times a
// second: it rebuilds when the generation moves, which in practice is when a
// document opens, closes, is renamed or goes dirty.
uint64_t menuModelShapeGeneration();

// The published tree, for a backend that is rebuilding. Returns a copy: the
// caller is on the AppKit main thread and the publisher is the frame loop, and
// handing out a reference into a mutex-guarded member is how that becomes a
// race the first time those stop being the same thread.
std::vector<MenuNode> publishedMenuModel();

// Enabled/checked for one item of the published tree, for `validateMenuItem:`.
// A `false` for an id that is not in the published tree at all, which is the
// right answer: an item that has gone away should not be pickable.
bool menuItemEnabled(MenuAction action, int param);
bool menuItemChecked(MenuAction action, int param);

}  // namespace np
