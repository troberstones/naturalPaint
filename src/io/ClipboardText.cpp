#include "io/ClipboardText.hpp"

#include <SDL3/SDL.h>

namespace np {

bool clipboardSetText(std::string_view utf8) {
  // SDL takes a NUL-terminated C string; a `string_view` is not guaranteed to
  // be one, so this materialises rather than calling `.data()` and hoping.
  const std::string owned(utf8);
  return SDL_SetClipboardText(owned.c_str());
}

bool clipboardHasText() { return SDL_HasClipboardText(); }

std::string clipboardGetText() {
  char* raw = SDL_GetClipboardText();
  if (raw == nullptr) return std::string();  // documented as never null; cheap to not trust
  std::string out(raw);
  SDL_free(raw);  // the ownership rule this file exists for
  return out;
}

}  // namespace np
