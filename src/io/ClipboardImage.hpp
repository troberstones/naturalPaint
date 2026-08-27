#pragma once

#include <cstdint>
#include <string>
#include <vector>

// io/ClipboardImage -- **the system pasteboard bridge** docs/testing-
// issues.md T9 (P0) asks for: "a preset that creates a document at the
// system clipboard's resolution and pastes its contents in" needs something
// that can see an image copied from another application, and this build had
// nothing (T9's own finding: "no NSPasteboard and no SDL_GetClipboard* call
// anywhere in src/").
//
// ==========================================================================
// 0. What SDL3 actually offers (checked before writing a line of
//    Objective-C++, per this brief and per ui/FileDialog.hpp's own
//    precedent -- "a platform-integration job in a comment is an estimate
//    someone once made, not a fact")
// ==========================================================================
//
// SDL3's clipboard API (`SDL3/SDL_clipboard.h`, vendored under
// `build/_deps/sdl3-src`) is not text-only. Alongside `SDL_GetClipboardText`
// it has a generic, **MIME-type-addressed** byte API:
//
//   * `SDL_GetClipboardMimeTypes(size_t* count)` -- every MIME type
//     currently on the pasteboard.
//   * `SDL_HasClipboardData(const char* mime_type)` -- is a given type
//     present.
//   * `SDL_GetClipboardData(const char* mime_type, size_t* size)` -- the
//     raw bytes for that type.
//
// On macOS this is not a stub: `src/video/cocoa/SDL_cocoaclipboard.m`'s
// `Cocoa_GetClipboardData()`/`Cocoa_HasClipboardData()` bridge straight to
// `NSPasteboard`, converting a MIME type to a UTI with
// `UTTypeCreatePreferredIdentifierForTag()` and reading
// `-[NSPasteboardItem dataForType:]` -- and `Cocoa_CheckClipboardUpdate()`'s
// `GetMimeTypes()` does the reverse conversion (UTI -> `-preferredMIMEType`)
// to build the list `SDL_GetClipboardMimeTypes()` returns. An image copied
// from Preview, Photos, Safari or a screenshot lands on the pasteboard as
// one or more UTIs (`public.png`, `public.tiff`, ...) that macOS itself maps
// to MIME types (`image/png`, `image/tiff`, ...) through exactly this path.
//
// **Conclusion: no Objective-C++ was needed.** `SDL_GetClipboardData()`
// already covers what this module needs; the only new code is asking for
// the right MIME type and decoding the bytes it returns -- which
// `io/ImageDecode.hpp`'s `decodeImageLinear()` already does for every raster
// format this build reads (PNG/JPEG/TGA/BMP unconditionally, TIFF and more
// through the OIIO fallback when `NP_USE_OIIO=ON`). This header is a thin
// probe on top of two functions this codebase already had.
//
// ==========================================================================
// 1. What "holds an image" means here, and the three ways it can fail
//    cleanly
// ==========================================================================
//
// An empty pasteboard and one holding text are BOTH the common case (T9:
// "never a crash, never a zero-sized document") and are reported the same
// way non-image data always is here -- through `ClipboardImageProbe::status`
// -- never through an exception, an assert, or a document created at 0x0.
//
//   * `Empty`     -- the pasteboard has no data of any kind
//                    (`SDL_GetClipboardMimeTypes()` returned zero types).
//   * `NotAnImage`-- the pasteboard has data, but none of its MIME types
//                    begins with `"image/"` (text, a file-path list, RTF,
//                    ...).
//   * `Unreadable`-- an `image/*` MIME type IS present, but every one this
//                    build tried failed to decode (a format this build's
//                    decoder does not handle -- `image/svg+xml`, say -- or
//                    truncated/corrupt bytes). `detail` names what was tried
//                    and the last decoder's own error.
//   * `Image`     -- decoded successfully. `width`/`height`/`pixels` are
//                    populated in `io/ImageDecode.hpp`'s own contract:
//                    linear-light float RGBA, straight alpha, row-major,
//                    top-to-bottom.
//
// ==========================================================================
// 2. Split for testability: probing the real OS pasteboard versus decoding
//    bytes already in hand
// ==========================================================================
//
// `--selftest` runs headless and must never depend on a human having copied
// an image into the real system pasteboard (this brief's own rule) -- and,
// separately, it should not overwrite whatever the person running it
// actually has on their clipboard as a side effect of running a test suite.
// So the decode logic that matters -- "given these bytes and this MIME
// type, do they become an `Image` with the right size and pixels" -- is
// pulled out as `decodeClipboardImageBytes()`, a pure function with no SDL
// dependency at all, and `--selftest` (app/selftest/ClipboardImage.cpp)
// asserts THAT directly against a real, freshly-encoded PNG built with
// io/Export's own encoder. `probeClipboardImage()` is the thin, untestable-
// without-a-live-pasteboard wrapper around it; `--selftest` exercises only
// its `Empty` and `NotAnImage` outcomes, which it can produce by asking SDL
// for the CURRENT state of the real pasteboard without ever writing an
// image to it. See that selftest file's header comment for exactly what is
// and is not covered, and for how the image path was verified manually
// (real dimensions observed, reported there and in this change's own
// report).
namespace np {

enum class ClipboardImageStatus {
  Empty,
  NotAnImage,
  Unreadable,
  Image,
};

struct ClipboardImageProbe {
  ClipboardImageStatus status = ClipboardImageStatus::Empty;
  // Populated only when status == Image.
  uint32_t width = 0;
  uint32_t height = 0;
  // width * height * 4 floats -- io/ImageDecode.hpp's DecodedImage contract,
  // exactly, because decodeImageLinear() is what produced it. Populated only
  // when status == Image.
  std::vector<float> pixels;
  // Which MIME type actually decoded (e.g. "image/png"). Empty unless
  // status == Image.
  std::string mimeType;
  // Human-readable detail for every status, including Image (names the MIME
  // type picked when more than one image type was on offer). Never a crash-
  // worthy condition -- always safe to just show this string.
  std::string detail;
};

// Decodes `data`/`size` bytes that were already read off the pasteboard for
// `mimeType`, through io/ImageDecode.hpp's decodeImageLinear(). Pure: no SDL
// call, no I/O, nothing but the decode -- which is what makes it directly
// unit-testable (see §2 above and app/selftest/ClipboardImage.cpp).
ClipboardImageProbe decodeClipboardImageBytes(const std::string& mimeType, const uint8_t* data,
                                              size_t size);

// Queries the real system pasteboard through SDL3's clipboard API (§0) and
// returns what it found. Tries every `image/*` MIME type
// `SDL_GetClipboardMimeTypes()` reports, in a fixed preference order (PNG,
// then TIFF, then JPEG, then BMP, then whatever else was offered in the
// order SDL returned it), stopping at the first one that decodes. Never
// throws, never asserts on empty or non-image content -- see this header's
// §1.
ClipboardImageProbe probeClipboardImage();

}  // namespace np
