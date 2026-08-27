#pragma once

// ui/NewDocumentDialog -- the modal docs/testing-issues.md T9 asks for:
// "Making a new document should let you set the resolution -- a simple
// dialog with standard presets, a way to make new presets, and a preset
// that creates a document at the system clipboard's resolution and pastes
// its contents in."
//
// Everything under this dialog already exists and is landed:
// app/DocumentPresets.hpp (the preset model and its persisted store),
// io/ClipboardImage.hpp (the pasteboard probe) and
// app/DocumentLifecycle.hpp's makeBlankOpenDocument() (the one new-document
// path -- this file does not invent a second one). This header is the
// widgets that sit on top of them, split out of ui/MacPaintUI.cpp -- which a
// different task is editing concurrently in an unrelated region -- rather
// than grown inside it.
//
// Follows ui/MacPaintUI.cpp's own MenuEffect::Deferred shape for every
// modal reached from a menu item: `MacPaintUI.cpp:7288`'s `performMenuAction`
// case for `MenuAction::NewDocument` runs on a native menu's callback, with
// no ImGui frame in progress and therefore no `ImGui::OpenPopup()` available
// to call directly (see that file's own header comment on
// `performMenuAction`). It calls `requestNewDocumentDialog()` instead, which
// only sets a flag; `drawNewDocumentDialog()` is called once per frame from
// inside the ImGui frame (alongside `drawExportAsDialog()` and the Image
// menu's two size dialogs) and is what actually opens the popup, exactly the
// two-step split `g_exportAsRequested` / `g_imageSizeRequested` already use
// in that file -- as functions here rather than raw externs only because
// this is now a second translation unit.
namespace np {

struct AppState;

// Called from MacPaintUI.cpp's File > New Document handler. Safe to call
// with no ImGui frame in progress (see this header's comment above).
void requestNewDocumentDialog();

// Draws the New Document modal if `requestNewDocumentDialog()` was called
// since the last frame. A no-op otherwise (`ImGui::BeginPopupModal()` returns
// false and this returns immediately, exactly like every other dialog in
// ui/MacPaintUI.cpp). Lazily loads `st.documentPresets` on first open --
// never at startup -- so `--selftest`'s headless run, which never calls this,
// never touches `document-presets.txt` (AppState::documentPresetsLoaded).
void drawNewDocumentDialog(AppState& st);

}  // namespace np
