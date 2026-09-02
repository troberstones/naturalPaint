#pragma once

#include <optional>
#include <string>
#include <vector>

// ui/MacNativeFileDialog -- the platform file browser, behind two functions.
//
// **Why this exists.** Every path this build asks the user for came in through
// one spartan modal with one `InputText` in it (ui/MacPaintUI.cpp's "Document
// path"), and that modal's own comment said so at length: *"There is no native
// file picker in this codebase, and adding one is a platform-integration job
// (NSOpenPanel behind an interface) rather than part of a lifecycle step... 
// swapping the text field for a panel later changes this function and nothing
// else."* This is that job, and that promise is kept -- the modal grows one
// button and keeps its text field.
//
// **The text field stays, and that is deliberate.** It is not a fallback for a
// panel that failed; it is the only way to reach a path the panel will not show
// you (inside a package, behind a symlink, on a volume the sandbox has not been
// told about) and the only way a path can be pasted in from somewhere else. A
// browser that removed it would be a narrower dialog wearing a wider one's
// clothes.
//
// --- The shape, and why it is not a callback --------------------------------
//
// `NSOpenPanel`'s `runModal` is a **nested run loop**: it blocks until the user
// answers, and returns the answer. So these return the answer too, rather than
// taking a completion block -- a block would need somewhere to put its result
// and something to poll it, for a call whose whole nature is synchronous.
//
// The cost is one long frame. That is survivable here and it was checked rather
// than assumed: `app/FixedStep.hpp`'s `kMaxCatchUpMs` (250 ms) caps how much
// real time one call may hand the solver's accumulator and `kMaxStepsPerFrame`
// caps what it may spend draining it, so a thirty-second browse advances the
// simulation by a quarter of a second and not by thirty seconds' worth of
// substeps. Without those two caps this would be a freeze, not a hitch.
//
// Nothing here is ImGui-aware. These are called from inside a frame, between
// `NewFrame()` and `Render()`, and touch no ImGui state at all -- so the frame
// that was being built is still being built when the panel closes.
namespace np {

// What to show, and what the accept button should say.
struct FilePanelRequest {
  // Shown above the file list. A sentence, not a title bar -- macOS panels
  // have no title bar to put one in.
  std::string message;
  // The accept button's label ("Open", "Import", "Save"). Empty for the
  // platform default.
  std::string prompt;
  // Lowercase, no leading dot ("abr", "npaint"). Empty means **any file**, and
  // for `File > Open` that is the correct answer rather than a lazy one:
  // app/OpenAnyFile decides what a file is from its *bytes*, so a `.npaint`
  // saved as `.exr` still opens as a document and an extension filter would
  // hide it from the user who saved it.
  std::vector<std::string> extensions;
  // Where to start. May be empty, may not exist -- an unusable value is
  // ignored by the panel rather than refused.
  std::string startDirectory;
  // Save panels only: the name the field is pre-filled with.
  std::string suggestedName;
};

#if defined(__APPLE__)

// True when a panel can actually be shown -- i.e. `NSApp` exists, which it
// does from `SDL_Init(SDL_INIT_VIDEO)` onward.
//
// A **runtime** answer rather than `#ifdef __APPLE__` at the call site, for
// `nativeMenuBarInstalled()`'s reason one file over: `--selftest` and
// `--screenshot` run in this same binary, and a Browse button drawn where no
// panel can open is a button that does nothing.
bool nativeFilePanelAvailable();

// Choose one existing file. `std::nullopt` for a cancel, and for every other
// way of not choosing -- a dismissed panel and a declined one are the same
// thing to the caller, which is "leave the text field alone".
std::optional<std::string> runNativeOpenFilePanel(const FilePanelRequest& req);

// Choose a destination path, which may not exist yet. The panel owns the
// overwrite confirmation, so a caller must not add a second one.
std::optional<std::string> runNativeSaveFilePanel(const FilePanelRequest& req);

#else

// Inline no-ops off Apple, so the call sites in ui/MacPaintUI.cpp need no
// `#if` of their own -- `ui/MacNativeMenu.hpp`'s arrangement, same reasoning.
inline bool nativeFilePanelAvailable() { return false; }
inline std::optional<std::string> runNativeOpenFilePanel(const FilePanelRequest&) {
  return std::nullopt;
}
inline std::optional<std::string> runNativeSaveFilePanel(const FilePanelRequest&) {
  return std::nullopt;
}

#endif

}  // namespace np
