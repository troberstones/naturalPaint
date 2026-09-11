#pragma once

#include <string>
#include <string_view>

// io/ClipboardText -- **the system pasteboard's TEXT side**, the counterpart
// to io/ClipboardImage.
//
// ==========================================================================
// 1. WHY THIS IS THREE LINES AND NOT AN OBJECTIVE-C++ FILE
// ==========================================================================
//
// The same finding io/ClipboardImage.hpp records, one API along: SDL3 already
// has `SDL_SetClipboardText` / `SDL_HasClipboardText` / `SDL_GetClipboardText`
// (`SDL3/SDL_clipboard.h`), and on macOS `src/video/cocoa/SDL_cocoaclipboard.m`
// bridges them straight to `NSPasteboard`. There is nothing to write here
// except the ownership rule below and a name the rest of the codebase can
// call without including SDL.
//
// **The ownership rule, which is the one thing a caller can get wrong.**
// `SDL_GetClipboardText()` returns a buffer the CALLER must release with
// `SDL_free()`, and returns an empty string (never null) when the pasteboard
// holds no text -- so the obvious `std::string(SDL_GetClipboardText())` leaks
// on every paste. `clipboardGetText()` below owns that call, copies, frees,
// and hands back a `std::string`; no caller ever sees the raw pointer.
//
// ==========================================================================
// 2. WHAT THIS DELIBERATELY DOES NOT DO
// ==========================================================================
//
// **No sanitising.** These functions are the transport and nothing else --
// what comes back is whatever another application put on the pasteboard,
// including invalid UTF-8, CRLFs and control characters. Cleaning that into
// something a `TextContent` can hold is `app/TextTool`'s
// `textSanitizePasted()`, which is where the reasoning about `shapeText()`
// refusing a whole block lives. Splitting them this way keeps the platform
// call testable without a pasteboard and the policy testable without a
// window.
//
// **No image handling.** That is io/ClipboardImage, which reads the same
// pasteboard through the MIME-addressed byte API. A pasteboard can hold both
// at once; which one a paste means is the caller's decision (`ui/MacPaintUI`
// asks for text while a Text session is live and for pixels otherwise), not
// this file's.
namespace np {

// Put `utf8` on the system pasteboard, replacing whatever was there. Returns
// false if the platform refused -- the caller decides whether that is worth
// reporting, and `ui/` does report it, because a copy that silently did
// nothing leaves the user pasting the PREVIOUS clipboard contents and
// wondering why.
bool clipboardSetText(std::string_view utf8);

// Whether the pasteboard currently holds text. Distinct from
// `clipboardGetText().empty()` only in the case that matters: an empty
// string genuinely on the pasteboard, which SDL reports the same way as
// nothing at all.
bool clipboardHasText();

// The pasteboard's text, empty when it holds none. Owns and frees SDL's
// buffer -- see the ownership rule above.
std::string clipboardGetText();

}  // namespace np
