#pragma once

#include <string>
#include <vector>

#include "brush/Stabiliser.hpp"

namespace np {

// app/StrokePreferences -- the global stabiliser setting
// (`StabiliserParams`), persisted beside `brush-libraries.txt`
// (`app/BrushLibraryFile.cpp`'s own path/override pattern, re-derived here
// for the reason `app/UserBrushLibrary.hpp`'s `defaultUserPresetsFilePath()`
// gives for its own: a different file, a different durability contract, not
// worth a dependency between the two for one function each).
//
// One record, not a list -- simpler than `UserBrushLibraryStore`: no name to
// key unknown lines by, just "every line before this build's own keys, plus
// every key this build does not recognise", re-emitted verbatim on save
// (app/BrushLibraryFile.hpp §3's forward-compatibility rule).

inline constexpr int kStrokePreferencesFileVersion = 1;
inline constexpr const char* kStrokePreferencesFileHeader = "naturalPaint-stroke-preferences";

// `~/Library/Application Support/naturalPaint/stroke-preferences.txt` on
// macOS, `${XDG_CONFIG_HOME:-~/.config}/naturalPaint/stroke-preferences.txt`
// elsewhere, overridable with `$NP_STROKE_PREFERENCES`.
std::string defaultStrokePreferencesFilePath();

class StrokePreferencesStore {
 public:
  // Parse `text` into `global`. Never fails outright: a missing or malformed
  // key simply leaves `global`'s corresponding field at whatever it already
  // held (its caller's default-constructed `StabiliserParams{}`, normally).
  void parse(const std::string& text, StabiliserParams& global);

  // A missing file is a fresh install, not an error.
  bool loadFromFile(const std::string& path, StabiliserParams& global, std::string* errorOut);

  std::string serialize(const StabiliserParams& global) const;

  bool saveToFile(const std::string& path, const StabiliserParams& global,
                  std::string* errorOut) const;

  // Every line this build did not recognise, in file order -- what
  // `serialize()` re-emits so an older build's save, or a hand edit, is not
  // silently dropped.
  const std::vector<std::string>& unknownLines() const noexcept { return unknownLines_; }

 private:
  std::vector<std::string> unknownLines_;
};

// The lazy-load-on-first-need gate `AppState::strokePreferencesLoaded`
// wraps, taking the three fields it needs by reference rather than
// `AppState&` itself -- `AppState.hpp` already includes THIS header, so a
// function here taking `AppState&` back would be circular. That also keeps
// it callable from plain app/ code (`ui/MacPaintUI.cpp`'s pen-down path,
// `--selftest`) with no UI dependency, which is the point: the global
// setting must be current the first time a stroke resolves it, not only
// once the Stabiliser popover has happened to be drawn.
void ensureStrokePreferencesLoaded(StrokePreferencesStore& store, bool& loaded,
                                   StabiliserParams& global);

}  // namespace np
