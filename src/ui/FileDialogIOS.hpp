#pragma once

// The UIKit half of ui/FileDialog, split into its own header/.mm the same way
// ui/MacNativeMenu is split from the platform-neutral MenuModel: FileDialog.cpp
// stays plain C++ and buildable everywhere, and only this file needs Xcode's
// Objective-C++ compiler and the UIKit/UniformTypeIdentifiers frameworks. Built
// only under NP_PLATFORM_IOS (src/CMakeLists.txt), so it is safe to include
// from FileDialog.cpp behind the same `#if`.
//
// See ui/FileDialog.hpp's header comment for why a picker's two directions are
// not symmetric on iOS: opening returns a URL the app can read once; exporting
// needs an already-written source file and copies it out, it does not hand
// back an empty destination to write into later.

#include "ui/FileDialog.hpp"

#include <string>
#include <vector>

struct SDL_Window;

namespace np {

// Presents a `UIDocumentPickerViewController` in its opening mode for
// `OpenDocument` / `ImportImage` / `ImportBrushes`. `rows`' patterns become
// `allowedContentTypes` via `[UTType typeWithFilenameExtension:]` -- the same
// per-extension, nil-skipping approach the cocoa backend already uses on
// macOS (this header's macOS-side comment measured it there; nothing here
// assumes the result is different on iOS, so nothing here re-measures it).
//
// The picked file is copied into this app's own sandbox before the outcome
// is posted (`asCopy:YES` plus an explicit copy off the picker's own
// transient temp location -- see the .mm for why one copy is not enough), so
// `FileDialogOutcome.path` is an ordinary, indefinitely-readable path and
// every existing macOS-shaped caller in ui/MacPaintUI.cpp needs no iOS
// branch of its own to read it.
//
// Returns false, having posted nothing, only when there is no window to
// present from -- `parentWindow` is null or SDL has not created its
// `UIWindow` yet. The caller must post its own outcome in that case, exactly
// as FileDialog.cpp's `g_activeProps == 0` branch already does for a genuine
// SDL failure.
bool showIOSOpenPicker(FileDialogPurpose purpose, const std::vector<FileDialogFilterRow>& rows,
                       SDL_Window* parentWindow, FileDialogMailbox* mailbox);

// Presents a `UIDocumentPickerViewController` in its exporting mode
// (`initForExporting:asCopy:`) for `SaveDocument` / `SaveCopy` /
// `ExportImage`. `sourcePath` must already exist and hold the final bytes --
// the picker copies it to wherever the user picks; nothing is written
// afterward. On success the mailbox receives `chose=true`,
// `alreadyWritten=true`, `path=<the destination the user picked>`.
//
// Same false/no-window contract as `showIOSOpenPicker()` above.
bool showIOSExportPicker(const std::string& sourcePath, SDL_Window* parentWindow,
                         FileDialogMailbox* mailbox);

}  // namespace np
