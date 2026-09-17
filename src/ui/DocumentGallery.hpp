#pragma once

#include <cstdint>
#include <string>
#include <vector>

// ui/DocumentGallery -- the iOS launch screen: a full-screen grid of every
// `.npaint` in the app's sandboxed Documents folder, like Procreate's
// Gallery, so working on iPad doesn't require bouncing out to the Files app
// just to open a document (docs/ios-spike-plan.md).
//
// **macOS and Linux are unchanged.** This module exists and compiles on
// every platform (nothing about scanning a directory or drawing a grid is
// Apple-specific), but main.cpp only ever calls into it from behind
// `NP_PLATFORM_IOS`, guarded on `AppState::showDocumentGallery` -- see that
// field's own comment in app/AppState.hpp. Desktop keeps opening straight to
// a blank/last-open canvas exactly as it does today.
//
// --- Why this is `ui/` and not `app/` --------------------------------------
//
// The directory scan and the thumbnail decode (`scanDocumentGallery()`) are
// plain data-in-data-out and could live in `app/`. What decides the split is
// `drawDocumentGallery()`: it owns an ImGui full-viewport window, a
// GPU-texture atlas and the WebGpu upload calls that go with it -- the same
// mix ui/LayerThumbnail's sibling `ui/MacPaintUI.cpp`'s `LayerThumbAtlas`
// has, and for the identical reason app/LayerThumbnail.hpp gives for keeping
// the *builder* (app/) separate from the *atlas* (ui/): this file has both
// halves and is one module because, unlike the layers panel, nothing else in
// the codebase wants the scan without the grid.
//
// --- The transfer-function landmine (read app/LayerThumbnail.hpp first) ---
//
// A `.npaint`'s embedded composite (io/NpaintFile's part 0) is linear light,
// exactly like a layer's tiles. `scanDocumentGallery()` therefore does the
// same thing `app/LayerThumbnail::layerContentThumbnail()` does: decode with
// `io/NpaintFile`'s `loadNpaintPreviewOnly()`, box-average down to the cell
// in **premultiplied** linear light, un-premultiply once, sRGB-encode the
// three colour channels on the CPU, and leave alpha (a coverage) unencoded.
// What `GalleryThumbnail::rgba` holds is therefore already display bytes --
// the same kind of value `ui/AtelierChrome.hpp`'s `atelierToken()` colours
// are -- and may go through `ImDrawList::AddImage()` on this application's
// deliberately non-sRGB swapchain (`ui/CanvasQuad.hpp`) without the
// linear-drawn-as-if-sRGB defect `app/selftest/PresentTransfer.cpp` measured.
// Uploading `DecodedImage::pixels` (linear) straight into the atlas and
// drawing it with `AddImage()` would be that defect again, one gallery tile
// at a time.
namespace np {

struct AppState;
class GpuContext;

// The square cell a thumbnail is built into, in texels. Larger than
// app/LayerThumbnail's 24 px cell on purpose -- a gallery tile is the whole
// point of this screen, not a 40 px row's accessory, and an iPad Pro's
// canvas has the room. Still small enough that a fresh scan of a few dozen
// documents costs milliseconds, not seconds (see scanDocumentGallery()'s own
// comment for the actual sample count).
inline constexpr int kGalleryThumbPx = 160;

// One tile's picture: `kGalleryThumbPx` square, RGBA8, straight alpha,
// sRGB-encoded colour (see this header's landmine section). The document's
// aspect ratio is preserved and centred, exactly like
// `app/LayerThumbnail::LayerThumbnail`'s letterbox -- `x`/`y`/`w`/`h` are the
// rect the document actually occupies within the cell.
struct GalleryThumbnail {
  std::vector<uint8_t> rgba;  // kGalleryThumbPx * kGalleryThumbPx * 4, or empty
  int x = 0, y = 0, w = 0, h = 0;
};

// One file in the gallery.
struct GalleryEntry {
  std::string path;
  // The file name without its extension, for the caption under the tile.
  std::string displayName;
  // A short, locale-independent "modified" caption ("2026-09-13"), also for
  // the caption. Not a full timestamp -- a grid of thumbnails is a poor place
  // for a clock, and the sort order already carries the ordering a time of
  // day would add.
  std::string modifiedLabel;
  // Filesystem modification time, seconds since epoch. What the gallery
  // sorts on; kept alongside `modifiedLabel` rather than re-derived from it
  // because the label is lossy (a day, not a second) and two documents saved
  // the same day must still sort by which was newer.
  int64_t mtimeEpoch = 0;
  // Empty (`thumb.w == thumb.h == 0`) when the composite could not be read or
  // decoded -- a foreign or damaged file, or one from a future build this
  // reader does not understand. The entry is kept regardless (see
  // scanDocumentGallery()'s own comment): a tile with a placeholder mark is
  // still a way to open the file, and a file that exists and is the user's
  // own must never silently vanish from the one screen that lists it.
  GalleryThumbnail thumb;
};

// The sandboxed container's Documents directory -- `$HOME/Documents` --
// which is also exactly what `UIFileSharingEnabled` /
// `LSSupportsOpeningDocumentsInPlace` (icons/ios/Info.plist.in) expose to the
// Files app. The gallery and Files app therefore show the same folder by
// design: a document saved from one appears in the other with no import
// step, which is the entire point of "the gallery is not a second place
// documents live."
//
// `$HOME` resolves inside the app's own sandboxed container on iOS -- the
// same fact core/Platform.hpp records for the `Library/Application Support`
// paths this codebase already builds off `getenv("HOME")` on every Apple
// platform. Only ever called on iOS; harmless (and unused) elsewhere.
//
// Declared in app/DocumentLifecycle.hpp, not here: `app/autosaveDocumentToGallery()`
// needs it too, and `app/` may not depend on `ui/`. This header pulls it in
// transitively (`app/DocumentLifecycle.hpp` is already included below).

// Scans `dir` for `*.npaint`, newest-modified first (the conventional
// choice -- a working iPad session's most relevant document is almost always
// the one just closed), and builds each entry's thumbnail from its embedded
// composite via `io/NpaintFile`'s `loadNpaintPreviewOnly()` -- **never**
// `loadNpaint()`, which would reconstruct every layer of every file just to
// throw all but the composite away (that function's own header comment
// states the cost this avoids).
//
// An unreadable or nonexistent directory (a fresh install, before Documents
// has been created; a permissions problem) returns an empty list rather than
// failing -- the caller draws just the "+" tile, which is the correct
// picture of "no documents yet," not an error.
std::vector<GalleryEntry> scanDocumentGallery(const std::string& dir);

// Draws the full-screen grid and acts on a tap. Call once per frame, in
// place of `drawUI()`, exactly while `AppState::showDocumentGallery` is true
// (main.cpp's job; this function does not read platform macros itself).
//
// * Tapping a thumbnail opens that file through `app/OpenAnyFile.hpp`'s
//   `openAnyFileAsDocument()` -- the same function a drag-and-drop or
//   File > Open already uses, so a `.npaint` that has drifted out of date or
//   picked up a newer build's part gets the identical warnings and refusal
//   messages it would from any other entry point -- adds the result to
//   `st.documents`, and clears `showDocumentGallery`.
// * Tapping "+" calls `app/DocumentLifecycle.hpp`'s `makeBlankOpenDocument()`
//   directly, at the same size main.cpp seeds a session with, adds it, and
//   clears the flag. This is the minimal "new document" path the brief
//   asks for -- it does not open ui/NewDocumentDialog.hpp's size-and-preset
//   modal, which is a reasonable follow-up but is not free to reuse from a
//   single tap with no size to negotiate.
// * A long press (`kGalleryLongPressSeconds`) on a thumbnail opens a context
//   menu offering Rename, Duplicate and Delete. The press that opened the
//   menu is not also a tap, so the document does not open underneath it.
//   Rename and Delete go through ui/Dialog (ADR-0010); Delete confirms
//   first, because a `.npaint` removed here does not go to a Trash the Files
//   app can restore it from.
void drawDocumentGallery(AppState& st, GpuContext& gpu);

// Scan `dir` instead of `iosDocumentsDirectory()`. `--gallery-demo`'s hook,
// and the reason it takes a directory at all: this screen has no photograph
// on any platform, and pointing a capture at the real `$HOME/Documents`
// would put the machine's own files in the frame
// ([[golden-must-not-photograph-home]] is a defect this project has already
// shipped once). An empty string restores the default. Takes effect on the
// next scan, so the caller sets it before the gallery is first drawn.
void setDocumentGalleryDirectory(const std::string& dir);

// `--gallery-menu-demo`'s hook: opens the first tile's long-press menu, or
// one of the two dialogs that menu leads to, on the first frame the gallery
// has entries. Same justification as `AppState::openToolFlyoutDemo` -- the
// menu needs a ~0.5 s press-and-hold and a dialog needs a tap inside it, and
// `--screenshot` has neither. Latched, so the popup opens once rather than
// re-opening the instant it is closed.
//
// Lives here rather than as an `AppState` field (which is where
// `openToolFlyoutDemo` lives) because nothing outside this module reads it
// and `app/AppState.hpp` is a 112-TU compile hub.
enum class GalleryDemoStage { None, Menu, Rename, Delete };
void setDocumentGalleryDemoStage(GalleryDemoStage stage);

// How long a finger must rest on a tile to mean "menu" rather than "open".
// 0.5 s is UIKit's own `UILongPressGestureRecognizer` default; a tap that
// drags past ImGui's mouse-drag threshold cancels it, so a scroll that begins
// slowly is still a scroll.
inline constexpr float kGalleryLongPressSeconds = 0.5f;

// --- the three long-press actions, as pure functions ----------------------
//
// The grid itself is unreachable from `--selftest` (no ImGui frame -- docs/
// reachability-audit.md F4), so everything about these actions that can be
// wrong independently of a mouse lives here instead of inside
// `drawDocumentGallery()`: what a name may be, where a rename moves a file,
// and which path a duplicate takes. app/selftest/DocumentGallery.cpp is what
// exercises them.

// Empty when `newName` is an acceptable document name; otherwise the sentence
// the rename dialog shows and refuses on. Guards the cases that would write
// outside the gallery directory or produce a file the scan cannot see again:
// a blank name, a path separator, `.`/`..`, a leading dot, and a name long
// enough to overrun a filesystem's 255-byte component limit once `.npaint`
// is appended.
std::string galleryRenameRefusal(const std::string& newName);

// Where `path` moves under `newName`: the same directory, `newName` trimmed
// of surrounding blanks, and exactly one `.npaint` extension however the user
// spelled it. Only meaningful when `galleryRenameRefusal(newName)` is empty.
std::string galleryRenamedPath(const std::string& path, const std::string& newName);

// The first free `"<stem> copy"`, `"<stem> copy 2"`, ... beside `path`, or
// empty when a thousand of them are already taken (which is a refusal, not a
// silent overwrite of someone's document).
std::string galleryDuplicatePath(const std::string& path);

}  // namespace np
