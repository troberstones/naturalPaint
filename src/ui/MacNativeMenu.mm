#include "ui/MacNativeMenu.hpp"

#import <Cocoa/Cocoa.h>

#include <vector>

#include "ui/MenuModel.hpp"

// ui/MacNativeMenu -- ui/MenuModel's AppKit backend.
//
// **This file contains no actions.** Every item it creates gets the same
// target and the same selector, and that selector does exactly one thing:
// enqueue an id. `performMenuAction()` in ui/MacPaintUI.cpp is where the id
// becomes behaviour, and it is the same function the ImGui bar calls. That is
// the property the whole extraction exists to buy -- two menu bars that cannot
// disagree about what a command does, because there is only one command.
//
// --- What SDL3 has already put in the menu bar before we get here -----------
//
// Found in SDL 3.2.24's `src/video/cocoa/SDL_cocoaevents.m` rather than
// assumed, because "what is already there" is the whole question a native menu
// port turns on. `Cocoa_RegisterApp()` runs during `SDL_Init(SDL_INIT_VIDEO)`
// and, guarded on `[NSApp mainMenu] == nil` (and on no `NSMainNibFile` in the
// bundle, which this build has no bundle to carry), calls
// `CreateApplicationMenus()`. That installs **two** top-level menus:
//
//   0. The application menu -- About, Preferences… (a dead item with a nil
//      action), Services, Hide, Hide Others, Show All, and **Quit ⌘Q**.
//   1. "Window" -- Close ⌘W, Minimize ⌘M, Zoom, Toggle Full Screen ⌃⌘F.
//
// There is no File menu, no Edit menu and no Help menu, and there is no SDL
// hint that turns any of this off.
//
// So this file **adds to** that bar rather than replacing it: our six menus go
// in at index 1, ahead of SDL's Window menu, and our Window items are appended
// to SDL's Window menu instead of creating a second menu with the same name.
// Replacing `[NSApp mainMenu]` wholesale would have been one line shorter and
// would have thrown away the standard application menu, which on macOS is not
// ours to throw away.
//
// --- Quit, and why this file does not create one ----------------------------
//
// `File > Quit` is suppressed on this platform (`MenuContext::nativeAppMenuPresent`
// -> `MenuItemSpec::omitWhenNativeAppMenu`), because SDL's application menu
// already ends with "Quit naturalPaint ⌘Q" and two Quit items in one menu bar
// is the classic result of this port.
//
// **The one this file must never write is `[NSApp terminate:]`.** SDL's Quit
// item is wired to `@selector(terminate:)`, and it is survivable only because
// SDL subclasses `NSApplication` and overrides `terminate:` to do nothing but
// `SDL_SendQuit()` -- so ⌘Q becomes an `SDL_EVENT_QUIT`, which main.cpp turns
// into `AppState::requestQuit`, which `app/QuitSequence` answers against every
// dirty document. A `terminate:` sent to a *plain* `NSApplication` tears the
// process down at once: every unsaved document gone, and the recovery
// journal's copy of them deleted with the scratch directory. That is why
// `installNativeMenuBar()` must run **after** `SDL_Init(SDL_INIT_VIDEO)` (see
// its comment in the header) and why no item created here is ever given a
// system selector.

namespace np {
namespace {

// The (action, param) pair, packed into an `NSMenuItem`'s tag.
//
// `tag` rather than `representedObject` deliberately: a tag is a plain
// integer that survives the item being copied, archived or handed around by
// AppKit, and it costs no object lifetime at all. Twenty bits of param is
// four orders of magnitude more than the largest family (the tool list, at
// 27), and the encoding is asserted round-trippable below rather than
// eyeballed.
constexpr int kParamBits = 20;
constexpr int kParamMask = (1 << kParamBits) - 1;

NSInteger packTag(MenuAction action, int param) {
  return (static_cast<NSInteger>(action) << kParamBits) |
         (static_cast<NSInteger>(param) & kParamMask);
}
MenuAction actionFromTag(NSInteger tag) {
  return static_cast<MenuAction>(tag >> kParamBits);
}
int paramFromTag(NSInteger tag) { return static_cast<int>(tag & kParamMask); }

NSEventModifierFlags cocoaModifiers(uint16_t mods) {
  NSEventModifierFlags f = 0;
  if (mods & kMenuModCmd) f |= NSEventModifierFlagCommand;
  if (mods & kMenuModShift) f |= NSEventModifierFlagShift;
  if (mods & kMenuModOption) f |= NSEventModifierFlagOption;
  if (mods & kMenuModControl) f |= NSEventModifierFlagControl;
  return f;
}

bool g_installed = false;
bool g_haveBuilt = false;
uint64_t g_builtGeneration = 0;

// How many top-level menus SDL had before we touched the bar, and how many
// items its Window menu had. Captured once, at install, and used on every
// rebuild to remove exactly what we added and nothing else.
//
// Counting rather than remembering the objects is the safer of the two: an
// `NSMenuItem*` we held across a rebuild would be a lifetime question, and a
// stale one would have us removing an item SDL owns.
NSInteger g_sdlTopLevelCount = 0;
NSInteger g_sdlWindowItemCount = 0;

}  // namespace
}  // namespace np

// The single target and the single selector.
//
// One object for the whole bar, not one per item: an item's identity is its
// tag, and a per-item target would be N objects whose only distinguishing
// feature is a number they could have carried anyway.
@interface NpMenuTarget : NSObject
- (void)npMenuItemPicked:(id)sender;
@end

@implementation NpMenuTarget

- (void)npMenuItemPicked:(id)sender {
  NSMenuItem* item = (NSMenuItem*)sender;
  if (![item isKindOfClass:[NSMenuItem class]]) return;
  // **Enqueue, never perform.** This runs on the AppKit main thread from
  // inside SDL's Cocoa pump -- the same thread as the frame loop, but at a
  // point where there is no ImGui frame in progress, no `AppState&` in scope
  // and no canvas size to hand `NewDocument`. `drawUI()` drains the queue at
  // the top of the next frame, where all three exist.
  np::enqueueMenuAction(np::actionFromTag([item tag]), np::paramFromTag([item tag]));
}

// AppKit asks this immediately before a menu is displayed, which is the native
// half of "enabled and checked have to be live". The ImGui bar asks the model
// every frame; this asks it at open time. Both read the same published tree,
// so the two bars cannot disagree by more than the one frame the ImGui bar has
// always been behind by.
//
// The check mark is set here too. It looks like a side effect in a predicate,
// and it is the documented AppKit idiom: validation is the one call guaranteed
// to happen before the item is drawn.
- (BOOL)validateMenuItem:(NSMenuItem*)item {
  const np::MenuAction action = np::actionFromTag([item tag]);
  const int param = np::paramFromTag([item tag]);
  [item setState:(np::menuItemChecked(action, param) ? NSControlStateValueOn
                                                     : NSControlStateValueOff)];
  return np::menuItemEnabled(action, param) ? YES : NO;
}

@end

namespace np {
namespace {

NpMenuTarget* g_target = nil;

// One `MenuNode` list into one `NSMenu`. Recursive, because the tree is.
void fillMenu(NSMenu* menu, const std::vector<MenuNode>& nodes) {
  for (const MenuNode& n : nodes) {
    switch (n.kind) {
      case MenuNodeKind::Separator:
        [menu addItem:[NSMenuItem separatorItem]];
        break;

      case MenuNodeKind::Note: {
        // The ImGui bar draws these with `TextDisabled`. The native analogue
        // is an item with no action at all, which `NSMenu`'s auto-enabling
        // greys out and makes unpickable -- the same "this is a label, not a
        // control" reading, in the platform's own vocabulary.
        NSMenuItem* item =
            [[NSMenuItem alloc] initWithTitle:@(n.label.c_str()) action:nil keyEquivalent:@""];
        [item setEnabled:NO];
        [menu addItem:item];
        break;
      }

      case MenuNodeKind::Submenu: {
        NSMenuItem* item =
            [[NSMenuItem alloc] initWithTitle:@(n.label.c_str()) action:nil keyEquivalent:@""];
        NSMenu* sub = [[NSMenu alloc] initWithTitle:@(n.label.c_str())];
        // Auto-enabling is what routes every item through
        // `-validateMenuItem:`. A submenu built with it off would show
        // last-rebuild's enabled state forever, which is precisely the "one of
        // them is lying" failure ui/MenuModel.hpp's header warns about.
        [sub setAutoenablesItems:YES];
        fillMenu(sub, n.children);
        [item setSubmenu:sub];
        // A disabled submenu (`Open Recent` with an empty list) has no tag to
        // validate against, so its enabled state is set here and the submenu
        // item is taken out of auto-enabling for that one purpose.
        if (!n.enabled) {
          [item setEnabled:NO];
          [sub setAutoenablesItems:NO];
          for (NSMenuItem* child in [sub itemArray]) [child setEnabled:NO];
        }
        [menu addItem:item];
        break;
      }

      case MenuNodeKind::Command:
      case MenuNodeKind::Check: {
        // **Every item, without exception, gets this target and this
        // selector.** There is no per-item `action:`, and in particular there
        // is no `@selector(terminate:)` anywhere in this file -- see the
        // header comment, and `MenuItemSpec::mayUseSystemSelector`.
        NSString* keyEquiv = @"";
        if (n.keyEquivalent.claimed()) {
          const char one[2] = {n.keyEquivalent.key, '\0'};
          keyEquiv = @(one);
        }
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:@(n.label.c_str())
                                                      action:@selector(npMenuItemPicked:)
                                               keyEquivalent:keyEquiv];
        [item setTarget:g_target];
        [item setTag:packTag(n.action, n.param)];
        if (n.keyEquivalent.claimed())
          [item setKeyEquivalentModifierMask:cocoaModifiers(n.keyEquivalent.mods)];
        // The shortcut *text* is deliberately not written into the title for
        // the chords with no key equivalent (`F`, `⇧F`, `⇧R`, `Space`). A
        // native menu that printed "Mirror Left/Right    F" without owning the
        // key would look like an accelerator and behave like decoration, and
        // the user's complaint would be that the menu is broken rather than
        // that the label is odd. ui/MenuModel.hpp's `MenuKeyEquivalent`
        // carries the argument for why those chords cannot be claimed.
        [menu addItem:item];
        break;
      }
    }
  }
}

void rebuild() {
  NSMenu* mainMenu = [NSApp mainMenu];
  if (mainMenu == nil) return;

  // Take our previous contribution back out, by count. Our top-level menus are
  // always inserted at index 1 and upward, so anything above SDL's own count
  // is ours, and removing at index 1 repeatedly removes exactly those.
  while ([mainMenu numberOfItems] > g_sdlTopLevelCount) [mainMenu removeItemAtIndex:1];

  NSMenu* windowMenu = [NSApp windowsMenu];
  if (windowMenu != nil) {
    while ([windowMenu numberOfItems] > g_sdlWindowItemCount)
      [windowMenu removeItemAtIndex:([windowMenu numberOfItems] - 1)];
  }

  NSInteger insertAt = 1;  // straight after the application menu
  for (const MenuNode& menu : publishedMenuModel()) {
    if (menu.kind != MenuNodeKind::Submenu) continue;

    // **The Window menu is merged, not duplicated.** SDL already owns a menu
    // called Window, holding Close/Minimize/Zoom/Full Screen, and macOS puts
    // exactly one of those in a bar. Our two Window entries (the ImGui demo
    // toggle and the open-document list) are appended to it after a rule,
    // which is where a Mac application puts its document list anyway.
    if (menu.label == "Window") {
      if (windowMenu == nil) continue;
      [windowMenu addItem:[NSMenuItem separatorItem]];
      fillMenu(windowMenu, menu.children);
      continue;
    }

    NSMenuItem* top =
        [[NSMenuItem alloc] initWithTitle:@(menu.label.c_str()) action:nil keyEquivalent:@""];
    NSMenu* sub = [[NSMenu alloc] initWithTitle:@(menu.label.c_str())];
    [sub setAutoenablesItems:YES];
    fillMenu(sub, menu.children);
    [top setSubmenu:sub];
    [mainMenu insertItem:top atIndex:insertAt];
    ++insertAt;
  }
}

}  // namespace

void installNativeMenuBar() {
  if (g_installed) return;

  // `NSApp` is nil only if this ran before `SDL_Init(SDL_INIT_VIDEO)`, which
  // is the ordering error the header warns about. Declining rather than
  // realising `NSApp` ourselves is the whole point: a plain `NSApplication`
  // would not carry SDL's `terminate:` override, and ⌘Q would stop reaching
  // the unsaved-work guard. `nativeMenuBarInstalled()` stays false, `File >
  // Quit` therefore stays in the ImGui menu, and the application keeps a way
  // out that asks about documents.
  if (NSApp == nil) return;
  NSMenu* mainMenu = [NSApp mainMenu];
  if (mainMenu == nil) return;

  g_target = [[NpMenuTarget alloc] init];
  g_sdlTopLevelCount = [mainMenu numberOfItems];
  NSMenu* windowMenu = [NSApp windowsMenu];
  g_sdlWindowItemCount = windowMenu != nil ? [windowMenu numberOfItems] : 0;
  g_installed = true;

  // Nothing is built here. `publishedMenuModel()` is empty until the first
  // `drawUI()` frame has published one, and a bar built from an empty model
  // would flash six empty menus. `updateNativeMenuBar()` builds it on the
  // first frame, which is the same frame the window becomes visible.
}

void updateNativeMenuBar() {
  if (!g_installed) return;
  const uint64_t generation = menuModelShapeGeneration();
  // Shape only. Enabled and checked are AppKit's to ask for, per item, via
  // `-validateMenuItem:` -- folding them into the generation would rebuild the
  // entire bar every time the pointer moved over a different layer.
  if (g_haveBuilt && generation == g_builtGeneration) return;
  g_builtGeneration = generation;
  g_haveBuilt = true;
  rebuild();
}

bool nativeMenuBarInstalled() { return g_installed; }

}  // namespace np
