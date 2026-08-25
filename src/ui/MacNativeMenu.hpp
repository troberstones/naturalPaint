#pragma once

// ui/MacNativeMenu -- the AppKit backend for ui/MenuModel.
//
// **Three functions, and deliberately nothing else.** Everything about *what*
// the menus are lives in ui/MenuModel.hpp; everything about *what an item
// does* lives in `performMenuAction()`. This header is the entire surface
// between the portable application and 250 lines of Objective-C++, which is
// what keeps the platform code from growing an opinion about the product.
//
// The implementation is `ui/MacNativeMenu.mm`, compiled only on Apple (see
// src/CMakeLists.txt's `enable_language(OBJCXX)` block). Off Apple these are
// inline no-ops rather than an `#if` at every call site: the Linux and Windows
// builds keep the ImGui menu bar they have always had, and they keep it
// without the four call sites in ui/MacPaintUI.cpp and src/main.cpp having to
// know that.
namespace np {

#if defined(__APPLE__)

// Take over `[NSApp mainMenu]`, once, after `SDL_Init(SDL_INIT_VIDEO)`.
//
// **After, not before, and that ordering is load-bearing.** SDL3 builds the
// standard macOS application menu itself during video init
// (`Cocoa_RegisterApp()` -> `CreateApplicationMenus()`), and it does so only
// while `NSApp == nil`, because that is also where it installs its own
// `NSApplication` subclass. That subclass overrides `terminate:` to do nothing
// but `SDL_SendQuit()`, which is the only reason ⌘Q on this platform reaches
// `AppState::requestQuit` and the unsaved-work guard at all. Realising `NSApp`
// ourselves first would give us a plain `NSApplication` whose `terminate:`
// really does tear the process down -- silently discarding every unsaved
// document. So: SDL first, always.
//
// Called with the menu bar already populated by SDL, this **adds** its menus
// between the application menu and SDL's Window menu, and merges its own
// Window items into SDL's rather than creating a second menu of that name. It
// does not touch the application menu, which is why there is exactly one Quit
// item in the bar and not two.
//
// A no-op on a second call, and a no-op if `NSApp` is somehow still nil.
void installNativeMenuBar();

// Reconcile the native menu with `publishedMenuModel()`. Cheap and safe to
// call every frame: it compares `menuModelShapeGeneration()` against what it
// last built and returns immediately when nothing structural has changed,
// which is every frame except the ones where a document opens, closes, is
// renamed or goes dirty. Enabled and checked state is NOT rebuilt here --
// AppKit asks for that itself, per item, when a menu is about to open.
void updateNativeMenuBar();

// True once `installNativeMenuBar()` has actually taken over the bar.
//
// Read by `menuContextFromState()` to set `MenuContext::nativeAppMenuPresent`,
// which is what suppresses `File > Quit` -- the platform's own application
// menu already has one. It is a *runtime* answer rather than `#ifdef __APPLE__`
// because the install can decline (no `NSApp`), and a File menu that dropped
// its Quit item on a build where the native bar never appeared would leave the
// application with no Quit item anywhere.
bool nativeMenuBarInstalled();

#else

inline void installNativeMenuBar() {}
inline void updateNativeMenuBar() {}
inline bool nativeMenuBarInstalled() { return false; }

#endif

}  // namespace np
