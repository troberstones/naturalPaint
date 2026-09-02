#include "ui/MacNativeFileDialog.hpp"

#import <Cocoa/Cocoa.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// ui/MacNativeFileDialog -- AppKit's file panels, and nothing else.
//
// The third piece of Objective-C++ in this project, and it follows the two
// before it exactly: a portable header (ui/MacNativeFileDialog.hpp) carrying
// the whole argument, an `.mm` carrying no product decisions, `-fobjc-arc`, and
// an explicit framework link rather than one inherited from SDL. See
// src/CMakeLists.txt's `if(APPLE)` block.
//
// --- What is deliberately NOT here ------------------------------------------
//
// No knowledge of what a `.abr` is, no knowledge of which of the five
// path-taking actions is asking, no pre-fill policy. All three live at the one
// call site in ui/MacPaintUI.cpp, because all three are product decisions and
// this file is a platform adapter.
//
// --- UTType rather than allowedFileTypes ------------------------------------
//
// `-[NSSavePanel setAllowedFileTypes:]` takes an array of extension strings and
// is the shorter call. It is also deprecated as of macOS 12, so it compiles
// with a warning naming a `src/` path -- which src/CMakeLists.txt's own rule
// forbids. `setAllowedContentTypes:` is the replacement and needs macOS 11+;
// this build is Apple-Silicon-only (cmake/Dependencies.cmake pins a
// macos-aarch64 wgpu-native binary), so every machine that can run it is on 11
// or later by construction.
//
// A `UTType` that cannot be built from an extension is **skipped, not
// substituted**: an unrecognised extension leaves the filter one type shorter,
// which shows the user more files than asked for. The other way round -- a
// filter that silently matched nothing -- would be a panel that appears to say
// the file is not there.
namespace np {
namespace {

// The half of a request that is identical for both panels. `NSSavePanel` is
// `NSOpenPanel`'s superclass, so this is written once against the base.
void applyCommonRequest(NSSavePanel* panel, const FilePanelRequest& req) {
  if (!req.message.empty()) [panel setMessage:@(req.message.c_str())];
  if (!req.prompt.empty()) [panel setPrompt:@(req.prompt.c_str())];

  if (!req.startDirectory.empty()) {
    NSString* dir = @(req.startDirectory.c_str());
    // `isDirectory:YES` builds a directory URL; the panel ignores one that
    // does not resolve, which is the behaviour wanted for a text field the
    // user may have typed half a path into.
    [panel setDirectoryURL:[NSURL fileURLWithPath:dir isDirectory:YES]];
  }

  if (!req.extensions.empty()) {
    NSMutableArray<UTType*>* types = [NSMutableArray array];
    for (const std::string& ext : req.extensions) {
      if (ext.empty()) continue;
      UTType* t = [UTType typeWithFilenameExtension:@(ext.c_str())];
      if (t != nil) [types addObject:t];
    }
    if ([types count] > 0) [panel setAllowedContentTypes:types];
  }
}

// One chosen URL to a filesystem path, or nothing.
//
// `fileSystemRepresentation` rather than `UTF8String`: the first is the byte
// sequence the POSIX layer will actually be handed (decomposed UTF-8, on this
// platform), and every consumer of the result opens the file with `std::ifstream`
// or `stat`. A path that round-trips through `UTF8String` differs for any name
// carrying a composed accent, which is most European filenames typed on a Mac.
std::optional<std::string> pathOf(NSURL* url) {
  if (url == nil) return std::nullopt;
  const char* p = [url fileSystemRepresentation];
  if (p == nullptr || *p == '\0') return std::nullopt;
  return std::string(p);
}

}  // namespace

bool nativeFilePanelAvailable() { return NSApp != nil; }

std::optional<std::string> runNativeOpenFilePanel(const FilePanelRequest& req) {
  if (NSApp == nil) return std::nullopt;

  NSOpenPanel* panel = [NSOpenPanel openPanel];
  [panel setCanChooseFiles:YES];
  // **No directories and no multiple selection**, both deliberately: every
  // caller is filling in one `char[512]` that names one file. A panel that let
  // the user pick three would have to decide which one won, and the honest
  // answer to that question is a different dialog.
  [panel setCanChooseDirectories:NO];
  [panel setAllowsMultipleSelection:NO];
  // A `.abr` (or a `.npaint`) that lives inside a package should be reachable:
  // the alternative is a file the user can see in Finder and cannot open here.
  [panel setTreatsFilePackagesAsDirectories:YES];
  applyCommonRequest(panel, req);

  if ([panel runModal] != NSModalResponseOK) return std::nullopt;
  return pathOf([[panel URLs] firstObject]);
}

std::optional<std::string> runNativeSaveFilePanel(const FilePanelRequest& req) {
  if (NSApp == nil) return std::nullopt;

  NSSavePanel* panel = [NSSavePanel savePanel];
  // Left ON. The panel then appends the first allowed extension to a name typed
  // without one, which is what a Mac user expects -- and the writers this feeds
  // (io/NpaintFile, io/ExportAs) choose their container from the extension, so a
  // name with none would be a save whose format nobody chose.
  [panel setExtensionHidden:NO];
  [panel setCanCreateDirectories:YES];
  if (!req.suggestedName.empty()) [panel setNameFieldStringValue:@(req.suggestedName.c_str())];
  applyCommonRequest(panel, req);

  // The overwrite warning is the panel's, and it is the only one: a second
  // confirmation of our own would ask the user the same question twice and, at
  // the second asking, about a file they have already agreed to replace.
  if ([panel runModal] != NSModalResponseOK) return std::nullopt;
  return pathOf([panel URL]);
}

}  // namespace np
