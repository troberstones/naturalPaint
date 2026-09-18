#include "io/ClipboardImage.hpp"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>

#include "io/ImageDecode.hpp"

// io/ClipboardImage -- implementation. Design and the SDL3-capability
// finding are argued in ClipboardImage.hpp; this file is the mechanics.

namespace np {
namespace {

bool startsWith(const std::string& s, const char* prefix) {
  const size_t n = std::char_traits<char>::length(prefix);
  return s.size() >= n && s.compare(0, n, prefix) == 0;
}

// Fixed preference order for which image MIME type to try first when more
// than one is on the pasteboard at once (a Preview.app copy, for instance,
// puts several representations of the same picture on the pasteboard
// together). Lower index wins; anything not listed here keeps whatever
// order SDL_GetClipboardMimeTypes() returned it in, appended after every
// listed type that IS present.
int preferenceRank(const std::string& mimeType) {
  static const char* const kOrder[] = {"image/png", "image/tiff", "image/jpeg", "image/bmp"};
  for (size_t i = 0; i < std::size(kOrder); ++i)
    if (mimeType == kOrder[i]) return static_cast<int>(i);
  return static_cast<int>(std::size(kOrder));
}

}  // namespace

ClipboardImageProbe decodeClipboardImageBytes(const std::string& mimeType, const uint8_t* data,
                                              size_t size) {
  ClipboardImageProbe out;
  std::string decodeError;
  const DecodedImage img = decodeImageLinear(data, size, &decodeError);
  if (!img.valid()) {
    out.status = ClipboardImageStatus::Unreadable;
    out.detail = "the pasteboard's '" + mimeType + "' data did not decode as an image" +
                (decodeError.empty() ? "." : (": " + decodeError));
    return out;
  }
  out.status = ClipboardImageStatus::Image;
  out.width = img.width;
  out.height = img.height;
  out.pixels = std::move(img.pixels);
  out.mimeType = mimeType;
  out.detail = "decoded a " + std::to_string(img.width) + " x " + std::to_string(img.height) +
              " image from the pasteboard's '" + mimeType + "' data.";
  return out;
}

ClipboardImageProbe probeClipboardImage() {
  ClipboardImageProbe out;

  size_t numTypes = 0;
  char** mimeTypes = SDL_GetClipboardMimeTypes(&numTypes);
  // SDL_GetClipboardMimeTypes() documents a NULL/empty result as "the
  // clipboard is empty" -- but that is only true on a backend that actually
  // populates SDL's mime-type list from the OS pasteboard, which is this
  // header's own §0 case for macOS (Cocoa_GetClipboardData() bridges to
  // NSPasteboard). **iOS does not**:
  // `src/video/uikit/SDL_uikitclipboard.m`'s UIKit_SetClipboardText() writes
  // straight to `[UIPasteboard generalPasteboard].string` through SDL's
  // separate text-only hooks, never touching the mime-type-addressed path
  // SDL_GetClipboardMimeTypes() reads (`_this->clipboard_mime_types`, only
  // ever filled in by SDL_SetClipboardData()). So on iOS, text set the normal
  // way (SDL_SetClipboardText(), which is what every other part of this
  // application calls) makes `numTypes` read 0 even though the pasteboard is
  // not empty -- which would misreport as Empty rather than NotAnImage.
  // SDL_HasClipboardText() reads UIKit_HasClipboardText(), which DOES see the
  // real pasteboard.string, so it is the one signal available here that
  // tells the two apart on this platform; on a platform where the mime-type
  // list already reflects the truth, this check is redundant but harmless --
  // a truly empty pasteboard has no text either.
  if (numTypes == 0) {
    if (mimeTypes) SDL_free(mimeTypes);
    if (SDL_HasClipboardText()) {
      out.status = ClipboardImageStatus::NotAnImage;
      out.detail =
          "the pasteboard holds text, not an image (seen via SDL_HasClipboardText() -- "
          "this platform's SDL backend does not expose plain text through the mime-type "
          "API SDL_GetClipboardMimeTypes() otherwise reads).";
      return out;
    }
    out.status = ClipboardImageStatus::Empty;
    out.detail = "the system pasteboard is empty.";
    return out;
  }

  std::vector<std::string> allTypes;
  std::vector<std::string> imageTypes;
  allTypes.reserve(numTypes);
  for (size_t i = 0; i < numTypes; ++i) {
    if (!mimeTypes[i]) continue;
    allTypes.emplace_back(mimeTypes[i]);
    if (startsWith(allTypes.back(), "image/")) imageTypes.push_back(allTypes.back());
  }
  SDL_free(mimeTypes);

  if (imageTypes.empty()) {
    out.status = ClipboardImageStatus::NotAnImage;
    std::string joined;
    for (size_t i = 0; i < allTypes.size(); ++i) {
      if (i) joined += ", ";
      joined += allTypes[i];
    }
    out.detail = "the pasteboard holds " + std::to_string(allTypes.size()) +
                " type(s) (" + joined + "), none of them an image.";
    return out;
  }

  std::stable_sort(imageTypes.begin(), imageTypes.end(),
                   [](const std::string& a, const std::string& b) {
                     return preferenceRank(a) < preferenceRank(b);
                   });

  std::string lastDetail;
  for (const std::string& mimeType : imageTypes) {
    size_t size = 0;
    void* data = SDL_GetClipboardData(mimeType.c_str(), &size);
    if (!data || size == 0) {
      if (data) SDL_free(data);
      continue;
    }
    ClipboardImageProbe decoded =
        decodeClipboardImageBytes(mimeType, static_cast<const uint8_t*>(data), size);
    SDL_free(data);
    if (decoded.status == ClipboardImageStatus::Image) return decoded;
    lastDetail = decoded.detail;
  }

  out.status = ClipboardImageStatus::Unreadable;
  out.detail = "the pasteboard offered an image type but it could not be read" +
              (lastDetail.empty() ? "." : (" -- " + lastDetail));
  return out;
}

}  // namespace np
