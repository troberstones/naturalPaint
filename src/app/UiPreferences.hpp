#pragma once

#include <string>
#include <vector>

namespace np {

// app/UiPreferences -- the settings the Preferences window edits: how big the
// interface is drawn, and what a single finger does on a touchscreen.
//
// **Why a new file rather than a key in stroke-preferences.txt.** That file's
// own header states the rule this follows: a preferences file is grouped by
// durability contract and change rate, not by convenience. Stroke preferences
// are edited mid-painting and read at every pen-down; these are chrome
// settings, read once at startup and changed rarely. They also have to be
// readable before the first stroke exists, which is what settles it -- the UI
// scale is applied while the first frame is being built.
//
// Everything else follows the established recipe exactly: line-oriented
// `key value` text with a versioned magic header, atomic save (temp + fsync +
// rename), unknown keys preserved verbatim so an older build cannot eat a
// newer build's settings, values clamped rather than rejected on load, and a
// `$NP_UI_PREFERENCES` override so `--selftest` can point the path somewhere
// disposable. `writeFileAtomically()`/`syncPath()` are copied rather than
// shared, which is what every other preferences module here does.

// What a ONE-finger drag on a touchscreen does. Two fingers always pan/zoom/
// rotate, and the pencil always draws, whatever this says.
enum class OneFingerGesture {
  // Drag the canvas around. The behaviour that shipped before this setting
  // existed, so it stays the default.
  Pan,
  // Sample the colour under the finger, the way the eyedropper does.
  ColorPick,
  // Nothing at all -- for a user who rests a hand on the glass and wants the
  // view to stay put no matter what.
  Nothing,
};

struct UiPreferences {
  // Multiplies ImGui's font size and every style metric. 1.0 is the size the
  // app has always drawn at.
  float uiScale = 1.0f;

  // iOS only in effect, but stored and edited on every platform: a preference
  // file that gains and loses keys depending on which machine wrote it is the
  // thing the unknown-line rule exists to prevent, and a desktop build that
  // silently dropped this key would do exactly that on a shared home
  // directory.
  OneFingerGesture oneFinger = OneFingerGesture::Pan;

  // How long a single finger must stay down before its gesture commits, in
  // milliseconds. The complaint this exists for is that touch is "too touchy":
  // a fingertip brushing the glass while the hand moves should not shove the
  // canvas. 0 disables the delay entirely.
  float oneFingerDebounceMs = 60.0f;
};

// The clamp ranges, named once so the loader and the sliders that edit these
// cannot disagree about what is representable.
inline constexpr float kUiScaleMin = 0.75f;
inline constexpr float kUiScaleMax = 2.0f;
inline constexpr float kOneFingerDebounceMinMs = 0.0f;
inline constexpr float kOneFingerDebounceMaxMs = 400.0f;

inline constexpr int kUiPreferencesFileVersion = 1;
inline constexpr const char* kUiPreferencesFileHeader = "naturalPaint-ui-preferences";

// `$NP_UI_PREFERENCES`, else the platform's own application-support directory,
// else a bare relative filename.
std::string defaultUiPreferencesFilePath();

class UiPreferencesStore {
 public:
  void parse(const std::string& text, UiPreferences& prefs);
  // A missing file is a fresh install, not an error: returns true, leaving
  // `prefs` at its defaults.
  bool loadFromFile(const std::string& path, UiPreferences& prefs, std::string* errorOut);
  std::string serialize(const UiPreferences& prefs) const;
  bool saveToFile(const std::string& path, const UiPreferences& prefs,
                  std::string* errorOut) const;

 private:
  // Keys this build does not know, kept in order and re-emitted on save.
  std::vector<std::string> unknownLines_;
};

// Lazy load, exactly the shape `ensureStrokePreferencesLoaded()` established:
// called wherever the values are first needed, rather than only after the
// Preferences window has been opened once -- otherwise a session that never
// opens it would run at the compiled-in defaults and silently ignore the file.
void ensureUiPreferencesLoaded(UiPreferencesStore& store, bool& loaded, UiPreferences& prefs);

}  // namespace np
