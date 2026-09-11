#include "app/StrokePreferences.hpp"

#include <fcntl.h>
#include <unistd.h>

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
      if (errorOut) *errorOut = "stroke preferences: could not open '" + temp + "' for writing.";
      return false;
    }
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    out.close();
    if (!out) {
      if (errorOut)
        *errorOut = "stroke preferences: '" + temp + "' was opened but not fully written.";
      return false;
    }
  }
  syncPath(temp);
  std::error_code ec;
  fs::rename(temp, path, ec);
  if (ec) {
    if (errorOut)
      *errorOut = "stroke preferences: could not rename '" + temp + "' into place (" +
                 ec.message() + ").";
    fs::remove(temp, ec);
    return false;
  }
  return true;
}

}  // namespace

std::string defaultStrokePreferencesFilePath() {
  if (const char* explicitPath = std::getenv("NP_STROKE_PREFERENCES")) {
    if (*explicitPath != '\0') return explicitPath;
  }
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  if (home && *home)
    return std::string(home) +
          "/Library/Application Support/naturalPaint/stroke-preferences.txt";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
    if (*xdg != '\0') return std::string(xdg) + "/naturalPaint/stroke-preferences.txt";
  }
  if (home && *home) return std::string(home) + "/.config/naturalPaint/stroke-preferences.txt";
#endif
  return "stroke-preferences.txt";
}

void StrokePreferencesStore::parse(const std::string& text, StabiliserParams& global) {
  unknownLines_.clear();
  std::istringstream in(text);
  std::string raw;
  bool firstLine = true;
  while (std::getline(in, raw)) {
    const std::string line = trimmedLine(raw);
    if (line.empty()) {
      firstLine = false;
      continue;
    }
    std::string key, rest;
    splitKey(line, key, rest);
    if (firstLine && key == kStrokePreferencesFileHeader) {
      firstLine = false;
      continue;
    }
    firstLine = false;

    float f;
    if (key == "mode" && takeFloat(rest, f)) {
      const int m = static_cast<int>(f);
      if (m >= static_cast<int>(StabiliserMode::Off) &&
          m <= static_cast<int>(StabiliserMode::WeightedAverage)) {
        global.mode = static_cast<StabiliserMode>(m);
        continue;
      }
    } else if (key == "stringPx" && takeFloat(rest, f)) {
      global.stringPx = f;
      continue;
    } else if (key == "strength" && takeFloat(rest, f)) {
      global.strength = f;
      continue;
    } else if (key == "responsiveness" && takeFloat(rest, f)) {
      global.responsiveness = f;
      continue;
    } else if (key == "catchUpAtEnd" && takeFloat(rest, f)) {
      global.catchUpAtEnd = f != 0.0f;
      continue;
    } else if (key == "catchUpWhilePaused" && takeFloat(rest, f)) {
      global.catchUpWhilePaused = f != 0.0f;
      continue;
    } else if (key == "stabilisePressure" && takeFloat(rest, f)) {
      global.stabilisePressure = f != 0.0f;
      continue;
    } else if (key == "scaleWithZoom" && takeFloat(rest, f)) {
      global.scaleWithZoom = f != 0.0f;
      continue;
    } else if (key == "showString" && takeFloat(rest, f)) {
      global.showString = f != 0.0f;
      continue;
    }
    // A key this build does not know, or a value that did not parse:
    // preserved verbatim rather than dropped (§ header comment).
    unknownLines_.push_back(line);
  }
}

bool StrokePreferencesStore::loadFromFile(const std::string& path, StabiliserParams& global,
                                          std::string* errorOut) {
  if (errorOut) errorOut->clear();
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    parse(std::string(), global);  // fresh install, not an error
    return true;
  }
  std::ostringstream buf;
  buf << f.rdbuf();
  const bool readOk = !f.bad();
  parse(buf.str(), global);
  if (!readOk && errorOut)
    *errorOut = "stroke preferences: '" + path +
               "' could not be read to the end; what was readable has been kept.";
  return readOk;
}

std::string StrokePreferencesStore::serialize(const StabiliserParams& global) const {
  std::string out;
  out += kStrokePreferencesFileHeader;
  out += " " + std::to_string(kStrokePreferencesFileVersion) + "\n";
  out += "mode " + std::to_string(static_cast<int>(global.mode)) + "\n";
  out += "stringPx " + f9(global.stringPx) + "\n";
  out += "strength " + f9(global.strength) + "\n";
  out += "responsiveness " + f9(global.responsiveness) + "\n";
  out += std::string("catchUpAtEnd ") + (global.catchUpAtEnd ? "1" : "0") + "\n";
  out += std::string("catchUpWhilePaused ") + (global.catchUpWhilePaused ? "1" : "0") + "\n";
  out += std::string("stabilisePressure ") + (global.stabilisePressure ? "1" : "0") + "\n";
  out += std::string("scaleWithZoom ") + (global.scaleWithZoom ? "1" : "0") + "\n";
  out += std::string("showString ") + (global.showString ? "1" : "0") + "\n";
  for (const std::string& line : unknownLines_) out += sanitizeOneLine(line) + "\n";
  return out;
}

bool StrokePreferencesStore::saveToFile(const std::string& path, const StabiliserParams& global,
                                        std::string* errorOut) const {
  if (errorOut) errorOut->clear();
  std::error_code ec;
  const fs::path parent = fs::path(path).parent_path();
  if (!parent.empty()) fs::create_directories(parent, ec);
  return writeFileAtomically(path, serialize(global), errorOut);
}

}  // namespace np
