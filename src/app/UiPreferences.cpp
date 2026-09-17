#include "app/UiPreferences.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace np {
namespace {

namespace fs = std::filesystem;

std::string f9(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
  return buf;
}

std::string trimmedLine(const std::string& s) {
  size_t b = 0, e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
    --e;
  return s.substr(b, e - b);
}

void splitKey(const std::string& line, std::string& key, std::string& rest) {
  const size_t sp = line.find(' ');
  if (sp == std::string::npos) {
    key = line;
    rest.clear();
    return;
  }
  key = line.substr(0, sp);
  size_t at = sp;
  while (at < line.size() && line[at] == ' ') ++at;
  rest = line.substr(at);
}

bool takeFloat(const std::string& text, float& out) {
  const char* p = text.c_str();
  char* end = nullptr;
  const float v = std::strtof(p, &end);
  if (end == p) return false;
  out = v;
  return true;
}

std::string sanitizeOneLine(std::string s) {
  for (char& c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20u || u == 0x7fu) c = ' ';
  }
  return s;
}

// A NAME per gesture rather than the enum's ordinal. `PanelLayout.hpp`'s own
// section on ordinal keys has the argument: an ordinal silently re-points at a
// different meaning the day someone inserts a value into the middle of the
// enum, and the file gives no sign that it happened.
const char* gestureName(OneFingerGesture g) {
  switch (g) {
    case OneFingerGesture::Pan: return "pan";
    case OneFingerGesture::ColorPick: return "color-pick";
    case OneFingerGesture::Nothing: return "nothing";
  }
  return "pan";
}

bool gestureFromName(const std::string& name, OneFingerGesture& out) {
  if (name == "pan") {
    out = OneFingerGesture::Pan;
    return true;
  }
  if (name == "color-pick") {
    out = OneFingerGesture::ColorPick;
    return true;
  }
  if (name == "nothing") {
    out = OneFingerGesture::Nothing;
    return true;
  }
  return false;
}

void syncPath(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return;
  ::fsync(fd);
  ::close(fd);
}

bool writeFileAtomically(const std::string& path, const std::string& contents,
                         std::string* errorOut) {
  const std::string temp = path + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (errorOut) *errorOut = "ui preferences: could not open '" + temp + "' for writing.";
      return false;
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
    if (!out) {
      if (errorOut) *errorOut = "ui preferences: '" + temp + "' was opened but not fully written.";
      return false;
    }
  }
  syncPath(temp);
  std::error_code ec;
  fs::rename(temp, path, ec);
  if (ec) {
    if (errorOut)
      *errorOut = "ui preferences: could not rename '" + temp + "' into place (" + ec.message() +
                  ").";
    fs::remove(temp, ec);
    return false;
  }
  return true;
}

}  // namespace

std::string defaultUiPreferencesFilePath() {
  if (const char* explicitPath = std::getenv("NP_UI_PREFERENCES")) {
    if (*explicitPath != '\0') return explicitPath;
  }
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  if (home && *home)
    return std::string(home) + "/Library/Application Support/naturalPaint/ui-preferences.txt";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
    if (*xdg != '\0') return std::string(xdg) + "/naturalPaint/ui-preferences.txt";
  }
  if (home && *home) return std::string(home) + "/.config/naturalPaint/ui-preferences.txt";
#endif
  return "ui-preferences.txt";
}

void UiPreferencesStore::parse(const std::string& text, UiPreferences& prefs) {
  unknownLines_.clear();
  std::istringstream in(text);
  std::string raw;
  bool sawHeader = false;
  while (std::getline(in, raw)) {
    const std::string line = trimmedLine(raw);
    if (line.empty()) continue;
    std::string key, rest;
    splitKey(line, key, rest);
    if (!sawHeader) {
      // The header is consumed and NOT kept as an unknown line, or saving
      // would emit two of them.
      if (key == kUiPreferencesFileHeader) {
        sawHeader = true;
        continue;
      }
      sawHeader = true;  // headerless file: read it anyway rather than refuse
    }
    if (key == "uiScale") {
      float v = 0.0f;
      // Reject non-finite, clamp out-of-range. A garbage scale that got
      // through would make the interface unusable and unfixable, since the
      // Preferences window itself would be drawn at it.
      if (takeFloat(rest, v) && std::isfinite(v))
        prefs.uiScale = std::clamp(v, kUiScaleMin, kUiScaleMax);
      continue;
    }
    if (key == "oneFinger") {
      OneFingerGesture g = OneFingerGesture::Pan;
      if (gestureFromName(rest, g)) prefs.oneFinger = g;
      continue;
    }
    if (key == "oneFingerDebounceMs") {
      float v = 0.0f;
      if (takeFloat(rest, v) && std::isfinite(v))
        prefs.oneFingerDebounceMs = std::clamp(v, kOneFingerDebounceMinMs, kOneFingerDebounceMaxMs);
      continue;
    }
    unknownLines_.push_back(sanitizeOneLine(line));
  }
}

bool UiPreferencesStore::loadFromFile(const std::string& path, UiPreferences& prefs,
                                      std::string* errorOut) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    // No file yet is the normal state of a fresh install.
    unknownLines_.clear();
    return true;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  if (in.bad()) {
    if (errorOut) *errorOut = "ui preferences: '" + path + "' could not be read.";
    return false;
  }
  parse(buf.str(), prefs);
  return true;
}

std::string UiPreferencesStore::serialize(const UiPreferences& prefs) const {
  std::ostringstream out;
  out << kUiPreferencesFileHeader << ' ' << kUiPreferencesFileVersion << '\n';
  out << "uiScale " << f9(prefs.uiScale) << '\n';
  out << "oneFinger " << gestureName(prefs.oneFinger) << '\n';
  out << "oneFingerDebounceMs " << f9(prefs.oneFingerDebounceMs) << '\n';
  for (const std::string& line : unknownLines_) out << line << '\n';
  return out.str();
}

bool UiPreferencesStore::saveToFile(const std::string& path, const UiPreferences& prefs,
                                    std::string* errorOut) const {
  const fs::path parent = fs::path(path).parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec && !fs::exists(parent)) {
      if (errorOut)
        *errorOut = "ui preferences: could not create '" + parent.string() + "' (" + ec.message() +
                    ").";
      return false;
    }
  }
  return writeFileAtomically(path, serialize(prefs), errorOut);
}

void ensureUiPreferencesLoaded(UiPreferencesStore& store, bool& loaded, UiPreferences& prefs) {
  if (loaded) return;
  loaded = true;
  store.loadFromFile(defaultUiPreferencesFilePath(), prefs, nullptr);
}

}  // namespace np
