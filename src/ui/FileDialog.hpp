#pragma once

// The OS file panel, and the two things about it that are not obvious
// ===================================================================
//
// Until this file existed, `ui/MacPaintUI.cpp`'s document-lifecycle section
// opened a small ImGui modal with a text field in it, and said so at length:
//
//     **There is no native file picker in this codebase**, and adding one is
//     a platform-integration job (NSOpenPanel behind an interface) rather
//     than part of a lifecycle step. So Open, Save As and Save a Copy take a
//     typed path in a small modal.
//
// That was true when it was written and it is what this module replaces. It
// turns out **no Objective-C++ was needed at all**: SDL3 ships a real AppKit
// backend for exactly this, at `src/dialog/cocoa/SDL_cocoadialog.m` in the
// vendored copy, built into `libSDL3.a` in this configuration
// (`SDL_DIALOG_DISABLED` is `#undef`ed in the generated
// `SDL_build_config.h`, and `SDL_ShowFileDialogWithProperties` resolves in
// the archive). It puts up a genuine `NSOpenPanel`/`NSSavePanel`, as a sheet
// on our own window when we hand it one.
//
// Two properties of that call decide the whole shape of this module, and
// both are stated outright in `SDL3/SDL_dialog.h`:
//
//  1. **It is asynchronous, and "the callback may be called from a different
//     thread than the one the function was invoked on."** So the callback
//     cannot touch `AppState`, cannot touch ImGui, and cannot even append to
//     a status string that the render thread reads. It posts to a mailbox
//     under a mutex; the frame loop takes from that mailbox on the main
//     thread and does the work there. (On this platform the cocoa backend's
//     completion handler happens to run on the main run loop, so the mailbox
//     is currently a formality -- but "currently, on one backend" is not a
//     property to build on, and the mailbox costs one mutex per file open.)
//
//  2. **The `filters` array "must remain valid at least until the callback
//     is invoked."** It therefore cannot be a stack local in the caller,
//     which is the mistake this API is shaped to make impossible: the rows
//     live in module-owned storage (`fileDialogFilters()`'s function-local
//     cache, plus one slot for a per-request override), and the caller never
//     names an `SDL_DialogFileFilter` at all.
//
// One request at a time
// ---------------------
// `requestFileDialog()` refuses while a panel is up and returns false. That
// is not defensiveness about double-clicks: the in-flight request's filter
// rows are module storage, and a second request would rewrite them while the
// first panel still points at them. The refusal is the enforcement of
// constraint 2 above, not a nicety.
//
// What --selftest can and cannot reach
// ------------------------------------
// It cannot put up a panel: that needs a window and a person. What it can
// do, and does (`app/selftest/FileDialog.cpp`), is assert the two things
// that can actually be wrong without anyone noticing:
//
//  * **Every filter pattern this build ships is one SDL will accept.** SDL
//    validates patterns (`[a-zA-Z0-9_.-]`, `;`-separated, or a lone `*`) and
//    a bad one does not degrade -- `validate_filters()` fails the call and
//    invokes the callback with `NULL`, so the panel simply never appears and
//    the user sees File > Open do nothing. The test calls **SDL's own**
//    `validate_filters()`, which is a linkable symbol in the archive, rather
//    than a reimplementation of its rule: a copy of the rule tests the copy.
//
//  * **The mailbox's transitions**, including that a second request while
//    one is pending is refused, and that a taken outcome is not taken twice.
//
// One measured thing, because assuming it would have been a silent no-op
// ----------------------------------------------------------------------
// The cocoa backend turns each extension into a `UTType` via
// `[UTType typeWithFilenameExtension:]` and **silently skips any that comes
// back nil** ("still failed? Don't add the pattern", its own comment). Three
// of the extensions here are not registered types on macOS -- `abr`,
// `npaint` and `dpx` -- and if any of them resolved to nil, its filter row
// would vanish, `setAllowedContentTypes:` would be handed an empty array,
// and Import Brushes would come up with **every file greyed out**. Nothing
// would report it.
//
// So it was measured rather than assumed, on this machine, with a small
// program calling that same selector: all eleven resolve. The unregistered
// three get *dynamic* identifiers, which is a real type and matches by
// extension exactly as a registered one does:
//
//     abr    -> dyn.ah62d4rv4ge80c2xw          npaint -> dyn.ah62d4rv4ge8066dbrf1hk
//     dpx    -> dyn.ah62d4rv4ge80k6d2          exr    -> com.ilm.openexr-image
//     png    -> public.png                     jpg    -> public.jpeg
//     tga    -> com.truevision.tga-image       bmp    -> com.microsoft.bmp
//     tif    -> public.tiff                    hdr    -> public.radiance
//     psd    -> com.adobe.photoshop-image
//
// This is not asserted in `--selftest`, and deliberately: it is a question
// about the running OS's type database, not about this code, and the
// assertion would be testing macOS.
//
// What was given up, and it is real
// ---------------------------------
// The typed-path modal pre-filled Save As with the document's own path. SDL's
// backend exposes only a *directory* (`SDL_PROP_FILE_DIALOG_LOCATION_STRING`
// -> `setDirectoryURL:`); it does not expose `NSSavePanel`'s
// `nameFieldStringValue`. So Save As on an already-saved document opens in
// that document's folder with an empty name field, rather than with its name
// filled in. Fixing that means either patching the vendored SDL or running
// our own `NSSavePanel` beside SDL's, and neither is worth a filename.

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;

namespace np {

// Which panel, in the application's own terms rather than the platform's.
//
// `SaveDocument` and `SaveCopy` put up the same kind of panel and differ only
// in what they say; they are separate cases because the words are the whole
// point of a panel -- a user who meant "save a copy" and read "Save As" has
// been told the wrong thing about what is about to happen to their document.
enum class FileDialogPurpose {
  OpenDocument,
  ImportImage,
  ImportBrushes,
  SaveDocument,
  SaveCopy,
  ExportImage,
};
inline constexpr std::size_t kFileDialogPurposeCount = 6;

// Which of the four filter sets a purpose wants. Named separately from the
// purpose because two purposes share a set (`SaveDocument`/`SaveCopy`), and
// because the sets are what `--selftest` walks.
enum class FileDialogFilterSet {
  // A `.npaint` or any picture this build can read. Open dispatches on the
  // file's *bytes* (app/OpenAnyFile.hpp), so this list is a convenience for
  // finding the file, not a statement about what the reader will accept.
  Openable,
  // Only pictures -- an import adds a layer, and offering a `.npaint` here
  // would offer something app/ImportImage does not do.
  Images,
  // Photoshop brush libraries.
  Brushes,
  // What a `.npaint` may be written as. `.exr` is the same container under a
  // different name (io/NpaintFile.hpp), so both are offered; `npaint` is
  // first because macOS appends the first allowed type to a bare filename.
  Documents,
};
inline constexpr std::size_t kFileDialogFilterSetCount = 4;

// Everything about a panel that does not depend on the running document.
// Pure, so `--selftest` can assert it without a window.
struct FileDialogPlan {
  // A save panel (`NSSavePanel`) rather than an open panel (`NSOpenPanel`).
  bool save = false;
  // The panel's title, and the label on its accept button. Both reach AppKit
  // (`setTitle:` / `setPrompt:` in the cocoa backend), which is why the verb
  // is spelled out rather than left as the OS default "Open".
  const char* title = "";
  const char* accept = "";
  FileDialogFilterSet filters = FileDialogFilterSet::Openable;
};

// Total. Every purpose has a plan; there is no default case and no failure
// mode where a panel comes up unlabelled.
FileDialogPlan fileDialogPlanFor(FileDialogPurpose purpose) noexcept;

// One row of the panel's type list.
struct FileDialogFilterRow {
  std::string name;
  // SDL's pattern syntax: bare extensions without dots, `;`-separated, e.g.
  // "npaint;exr". **Never `*`** in any set here -- the cocoa backend treats a
  // `*` anywhere in any row as "do not filter at all", which would silently
  // turn one row's convenience into no filtering for the whole panel.
  std::string pattern;
};

// The rows for a set, built once on first use from **this build's own
// capability table** (io/Capabilities.hpp's `allFormatCapabilities()`) rather
// than from a hard-coded list -- so a build without OpenImageIO offers PNG,
// JPEG, TGA and BMP and does not offer EXR, TIFF, HDR, DPX or PSD, without
// anyone maintaining a second copy of that fact.
//
// The returned reference is to a function-local static and is stable for the
// life of the process, which is what makes constraint 2 in this header's
// opening comment hold structurally.
const std::vector<FileDialogFilterRow>& fileDialogFilters(FileDialogFilterSet set);

// What came back. `completed` is false until the callback has run at all.
struct FileDialogOutcome {
  // The user chose a file, and `path` names it.
  bool chose = false;
  // The panel closed with nothing chosen. Not an error, and must not be
  // reported as one: it is how a user says "never mind".
  bool cancelled = false;
  std::string path;
  // Non-empty only when SDL reported an error (`filelist == NULL`), carrying
  // `SDL_GetError()`. Distinct from `cancelled`, because "the panel could not
  // be shown" and "you closed the panel" are different sentences.
  std::string error;
};

// The hand-off from whatever thread SDL calls back on to the frame loop.
//
// Exposed as a type, rather than hidden behind the free functions below, so
// `--selftest` can drive its transitions from a second thread without a
// window in sight.
class FileDialogMailbox {
 public:
  // Claims the mailbox for a request. Returns false if one is already in
  // flight, in which case the caller must not show a panel -- see this
  // header's "One request at a time".
  bool beginRequest(FileDialogPurpose purpose);

  // Called from SDL's callback, on whatever thread that is. Ignored if no
  // request is in flight, which is the case a spurious or duplicated
  // callback lands in.
  void post(FileDialogOutcome outcome);

  // Main thread. Returns the outcome exactly once, and ends the request.
  std::optional<FileDialogOutcome> take();

  // True from `beginRequest()` until the matching `take()`. Deliberately
  // *not* "the panel is up": it stays true for the frame that consumes the
  // outcome, because a caller waiting on a file name (the pending-close
  // path in ui/MacPaintUI.cpp) must not see a gap in which no panel is up
  // and no path has been applied yet.
  bool pending() const;

  // Which purpose the in-flight request was for. `OpenDocument` when idle;
  // callers check `pending()` first.
  FileDialogPurpose purpose() const;

 private:
  mutable std::mutex mutex_;
  bool pending_ = false;
  FileDialogPurpose purpose_ = FileDialogPurpose::OpenDocument;
  std::optional<FileDialogOutcome> outcome_;
};

// The window the panel is presented as a sheet on. Called once from
// `main.cpp` after the window exists.
//
// Without it the cocoa backend falls back to `[dialog runModal]`, which is
// application-modal and **blocks the calling thread** -- i.e. it would stop
// our render loop dead for as long as the panel is up. With it, the panel is
// a sheet and the loop keeps running behind it. So this is not cosmetic.
void setFileDialogParentWindow(SDL_Window* window);

// Puts a panel up. Returns false, having done nothing, if one is already in
// flight.
//
// `defaultDirectory` may be empty, in which case the OS chooses. A *file*
// path is not accepted here and would be wrong: the backend passes it to
// `setDirectoryURL:`. Callers pass `fileDialogDirectoryOf(somePath)`.
//
// **Main thread only** -- SDL's header says so, and the AppKit backend means
// it.
bool requestFileDialog(FileDialogPurpose purpose, const std::string& defaultDirectory);

// As above, but with a single filter row of the caller's own instead of the
// plan's set. The one caller is Export As, whose output format is chosen by
// its own panel controls: offering every writable extension there would let
// macOS append `.npaint` to a file the exporter is about to write as a PNG.
//
// The row is copied into module storage, so the caller may pass a temporary.
bool requestFileDialogWithFilter(FileDialogPurpose purpose, const std::string& defaultDirectory,
                                 const FileDialogFilterRow& row);

// True while a panel is up, or while its outcome is waiting to be consumed.
bool fileDialogPending();

// The purpose of the in-flight request. Meaningful only while
// `fileDialogPending()`.
FileDialogPurpose fileDialogPurpose();

// Main thread, once a frame. Returns the outcome exactly once.
std::optional<FileDialogOutcome> takeFileDialogOutcome();

// The directory part of a path, for `defaultDirectory` above. Empty for an
// empty path or a bare filename, which is the "let the OS choose" case.
std::string fileDialogDirectoryOf(const std::string& path);

}  // namespace np
