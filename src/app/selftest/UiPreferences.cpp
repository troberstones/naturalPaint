#include "app/selftest/Support.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "app/UiPreferences.hpp"

namespace np {

namespace fs = std::filesystem;

// app/UiPreferences -- the Preferences window's settings on disk.
//
// Follows app/selftest/PanelLayout.cpp's own round-trip pattern: point the
// file at a disposable directory through the module's `$NP_*` override, prove
// the override is what `defaultUiPreferencesFilePath()` returns, save, read the
// raw bytes back, load into a SECOND store and compare, then restore the
// environment. The value of doing it against the real file rather than against
// `serialize()`/`parse()` in memory is that it is the only way the atomic-save
// path, the directory creation and the header are exercised at all.
bool runUiPreferencesTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto near = [](float a, float b) { return std::fabs(a - b) <= 1e-6f; };

  std::printf("[selftest] ui preferences: the Preferences window's settings, on disk\n");

  const fs::path dir = fs::temp_directory_path() / "np-selftest-ui-preferences";
  std::error_code ec;
  fs::remove_all(dir, ec);
  const fs::path path = dir / "ui-preferences.txt";

  const char* previous = std::getenv("NP_UI_PREFERENCES");
  const std::string saved = previous != nullptr ? previous : "";
  ::setenv("NP_UI_PREFERENCES", path.string().c_str(), 1);

  check(defaultUiPreferencesFilePath() == path.string(),
        "the $NP_UI_PREFERENCES override is what the default path resolves to -- without this "
        "the rest of this suite would be testing the developer's real settings file");

  // ------------------------------------------------------------------ (a)
  // Defaults, and a missing file. A fresh install has no file, and that is not
  // an error -- a build that reported one would make every first run noisy.
  {
    UiPreferences prefs;
    check(near(prefs.uiScale, 1.0f) && prefs.oneFinger == OneFingerGesture::Pan,
          "defaults: scale 1.0 and one finger pans -- the behaviour that shipped before "
          "this setting existed");
    UiPreferencesStore store;
    std::string error = "unset";
    const bool loaded = store.loadFromFile(path.string(), prefs, &error);
    check(loaded && near(prefs.uiScale, 1.0f),
          "a missing file loads successfully and leaves the defaults alone -- a fresh "
          "install is not a failure");
  }

  // ------------------------------------------------------------------ (b)
  // The round trip, through the real file.
  {
    UiPreferences written;
    written.uiScale = 1.25f;
    written.oneFinger = OneFingerGesture::ColorPick;
    written.oneFingerDebounceMs = 120.0f;

    UiPreferencesStore store;
    std::string error;
    const bool savedOk = store.saveToFile(path.string(), written, &error);
    check(savedOk && fs::exists(path),
          "saveToFile creates the directory and the file");
    check(!fs::exists(fs::path(path.string() + ".tmp")),
          "...and leaves no .tmp behind -- the rename completed");

    std::ifstream in(path);
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    check(text.find(kUiPreferencesFileHeader) == 0,
          "the file begins with its own versioned magic header");
    check(text.find("oneFinger color-pick") != std::string::npos,
          "the gesture is stored by NAME, not by enum ordinal -- inserting a value into the "
          "middle of the enum must not silently re-point an existing file's setting");

    UiPreferences readBack;
    UiPreferencesStore second;
    check(second.loadFromFile(path.string(), readBack, &error),
          "a second store loads the file");
    check(near(readBack.uiScale, 1.25f) && readBack.oneFinger == OneFingerGesture::ColorPick &&
              near(readBack.oneFingerDebounceMs, 120.0f),
          "every field survives the round trip exactly");
    check(second.serialize(readBack) == store.serialize(written),
          "...and re-serialising is byte-identical, so a load/save cycle cannot drift the file");
  }

  // ------------------------------------------------------------------ (c)
  // Hostile values. The loader clamps rather than rejects, because a scale it
  // refused would leave the interface at a size the user cannot see to fix --
  // but NaN and infinity are rejected outright, since there is no sane clamp
  // for them and they would propagate into every style metric.
  {
    UiPreferencesStore store;
    UiPreferences prefs;
    store.parse("naturalPaint-ui-preferences 1\nuiScale 99\n", prefs);
    check(near(prefs.uiScale, kUiScaleMax),
          "an absurd scale is CLAMPED to the maximum, not rejected and not honoured -- an "
          "unreadable interface cannot be used to repair itself");

    UiPreferences nanPrefs;
    store.parse("naturalPaint-ui-preferences 1\nuiScale nan\n", nanPrefs);
    check(near(nanPrefs.uiScale, 1.0f),
          "a non-finite scale leaves the default in place rather than poisoning every style "
          "metric with NaN");

    UiPreferences badGesture;
    store.parse("naturalPaint-ui-preferences 1\noneFinger wobble\n", badGesture);
    check(badGesture.oneFinger == OneFingerGesture::Pan,
          "an unrecognised gesture name falls back to the default rather than to whatever "
          "integer it cast to");
  }

  // ------------------------------------------------------------------ (d)
  // Forward compatibility: a key this build does not know must survive being
  // loaded and saved by it, or an older build would silently eat a newer one's
  // settings on a shared home directory.
  {
    UiPreferencesStore store;
    UiPreferences prefs;
    store.parse("naturalPaint-ui-preferences 1\nuiScale 1.5\nsomethingNewer 7\n", prefs);
    const std::string out = store.serialize(prefs);
    check(out.find("somethingNewer 7") != std::string::npos,
          "an unknown key is preserved verbatim through a load/save cycle -- this build must "
          "not eat a newer build's settings");
    check(out.find(kUiPreferencesFileHeader) == 0 &&
              out.find(kUiPreferencesFileHeader, 1) == std::string::npos,
          "...and the header is written exactly once, not kept as an unknown line too");
  }

  if (saved.empty())
    ::unsetenv("NP_UI_PREFERENCES");
  else
    ::setenv("NP_UI_PREFERENCES", saved.c_str(), 1);
  fs::remove_all(dir, ec);
  return ok;
}

}  // namespace np
