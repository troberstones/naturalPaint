#include "io/GradientPresetFile.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include "io/Json.hpp"

namespace np {
namespace {

const char* jsonKindName(JsonValue::Kind kind) {
  switch (kind) {
    case JsonValue::Kind::Null:   return "null";
    case JsonValue::Kind::Bool:   return "a true/false";
    case JsonValue::Kind::Number: return "a number";
    case JsonValue::Kind::String: return "a string";
    case JsonValue::Kind::Array:  return "an array";
    case JsonValue::Kind::Object: return "an object";
  }
  return "an unknown value";
}

// `io/ActionFile.cpp`'s own `quotedName()` and the same reason: `<sstream>`
// drags in `std::quoted`, which ADL prefers over a local `quoted()` taking
// `string_view`.
std::string quotedName(std::string_view label) { return "\"" + std::string(label) + "\""; }

JsonValue colorStopJson(const GradientColorStopSpec& s) {
  JsonValue one = JsonValue::object();
  one.set("position", JsonValue::number(s.position));
  if (s.foreground) {
    // No "color" key at all -- this header's own note on why a stop cannot
    // carry both a foreground flag and a colour that would silently outrank
    // or be outranked by it.
    one.set("foreground", JsonValue::boolean(true));
  } else {
    JsonValue color = JsonValue::array();
    color.push(JsonValue::number(s.color[0]));
    color.push(JsonValue::number(s.color[1]));
    color.push(JsonValue::number(s.color[2]));
    one.set("color", std::move(color));
  }
  one.set("midpoint", JsonValue::number(s.midpoint));
  return one;
}

JsonValue opacityStopJson(const GradientOpacityStopSpec& s) {
  JsonValue one = JsonValue::object();
  one.set("position", JsonValue::number(s.position));
  one.set("opacity", JsonValue::number(s.opacity));
  one.set("midpoint", JsonValue::number(s.midpoint));
  return one;
}

}  // namespace

void writeGradientPreset(const std::string& name, const GradientPresetStops& stops,
                         std::string* out) {
  JsonValue doc = JsonValue::object();
  doc.set(kGradientPresetFileVersionKey, JsonValue::number(kGradientPresetFileVersion));
  doc.set("name", JsonValue::string(name));

  JsonValue colorStops = JsonValue::array();
  for (const GradientColorStopSpec& s : stops.colorStops) colorStops.push(colorStopJson(s));
  doc.set("color_stops", std::move(colorStops));

  JsonValue opacityStops = JsonValue::array();
  for (const GradientOpacityStopSpec& s : stops.opacityStops) opacityStops.push(opacityStopJson(s));
  doc.set("opacity_stops", std::move(opacityStops));

  *out = doc.write(2);
  out->push_back('\n');
}

bool readGradientPreset(std::string_view text, std::string_view label, std::string* nameOut,
                        GradientPresetStops* out, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (nameOut == nullptr || out == nullptr) return false;

  const auto refuse = [&](const std::string& why) {
    if (errorOut) *errorOut = why;
    return false;
  };

  JsonValue doc;
  std::string parseError;
  if (!parseJson(text, label, &doc, &parseError)) {
    return refuse("refused: " + parseError +
                  ". A .npgradient file is JSON; fix the syntax at that byte, or open the file "
                  "with the editor that wrote it.");
  }
  if (!doc.isObject()) {
    return refuse("refused: " + quotedName(label) + " holds " + jsonKindName(doc.kind()) +
                  " at its top level, not an object. A .npgradient file is one JSON object "
                  "with an \"" + std::string(kGradientPresetFileVersionKey) + "\" key.");
  }

  const JsonValue* version = doc.find(kGradientPresetFileVersionKey);
  if (version == nullptr) {
    return refuse("refused: " + quotedName(label) + " has no \"" +
                  std::string(kGradientPresetFileVersionKey) +
                  "\" key, so it is not a naturalPaint gradient preset -- or it is one that "
                  "was hand-edited until the key was lost. It is NOT read as version " +
                  std::to_string(kGradientPresetFileVersion) + " on the assumption that it "
                  "probably is one.");
  }
  if (!version->isNumber()) {
    return refuse("refused: " + quotedName(label) + " has a \"" +
                  std::string(kGradientPresetFileVersionKey) + "\" key holding " +
                  jsonKindName(version->kind()) + " rather than a number.");
  }
  const double rawVersion = version->asNumber();
  if (rawVersion != std::floor(rawVersion) ||
      rawVersion != static_cast<double>(kGradientPresetFileVersion)) {
    return refuse("refused: " + quotedName(label) + " is a version " +
                  JsonValue::number(rawVersion).write(-1) +
                  " gradient preset and this build reads version " +
                  std::to_string(kGradientPresetFileVersion) +
                  ". Open it with the build that wrote it, or re-save it from there.");
  }

  std::string parsedName;
  const JsonValue* name = doc.find("name");
  if (name != nullptr) {
    if (!name->isString())
      return refuse("refused: " + quotedName(label) + " has a \"name\" key holding " +
                    jsonKindName(name->kind()) + " rather than a string.");
    parsedName = name->asString();
  }

  GradientPresetStops parsed;

  const JsonValue* colorStops = doc.find("color_stops");
  if (colorStops != nullptr) {
    if (!colorStops->isArray())
      return refuse("refused: " + quotedName(label) + " has a \"color_stops\" key holding " +
                    jsonKindName(colorStops->kind()) + " rather than an array.");
    for (size_t i = 0; i < colorStops->size(); ++i) {
      const JsonValue& stop = colorStops->at(i);
      const std::string where =
          "colour stop " + std::to_string(i + 1) + " of " + quotedName(label);
      if (!stop.isObject())
        return refuse("refused: " + where + " is " + jsonKindName(stop.kind()) +
                      " rather than an object.");
      GradientColorStopSpec cs;
      const JsonValue* pos = stop.find("position");
      if (pos == nullptr || !pos->isNumber())
        return refuse("refused: " + where + " has no numeric \"position\".");
      cs.position = static_cast<float>(pos->asNumber());

      const JsonValue* fg = stop.find("foreground");
      const bool isForeground = fg != nullptr && fg->isBool() && fg->asBool();
      if (isForeground) {
        cs.foreground = true;
      } else {
        const JsonValue* color = stop.find("color");
        if (color == nullptr || !color->isArray() || color->size() != 3)
          return refuse("refused: " + where +
                        " has neither \"foreground\": true nor a 3-number \"color\" array.");
        for (int c = 0; c < 3; ++c) {
          if (!color->at(static_cast<size_t>(c)).isNumber())
            return refuse("refused: " + where + "'s \"color\" entry " + std::to_string(c) +
                          " is not a number.");
          cs.color[static_cast<size_t>(c)] =
              static_cast<float>(color->at(static_cast<size_t>(c)).asNumber());
        }
      }
      cs.midpoint = static_cast<float>(stop.numberOr("midpoint", 0.5));
      parsed.colorStops.push_back(cs);
    }
  }

  const JsonValue* opacityStops = doc.find("opacity_stops");
  if (opacityStops != nullptr) {
    if (!opacityStops->isArray())
      return refuse("refused: " + quotedName(label) + " has an \"opacity_stops\" key holding " +
                    jsonKindName(opacityStops->kind()) + " rather than an array.");
    for (size_t i = 0; i < opacityStops->size(); ++i) {
      const JsonValue& stop = opacityStops->at(i);
      const std::string where =
          "opacity stop " + std::to_string(i + 1) + " of " + quotedName(label);
      if (!stop.isObject())
        return refuse("refused: " + where + " is " + jsonKindName(stop.kind()) +
                      " rather than an object.");
      GradientOpacityStopSpec os;
      const JsonValue* pos = stop.find("position");
      if (pos == nullptr || !pos->isNumber())
        return refuse("refused: " + where + " has no numeric \"position\".");
      os.position = static_cast<float>(pos->asNumber());
      const JsonValue* opacity = stop.find("opacity");
      if (opacity == nullptr || !opacity->isNumber())
        return refuse("refused: " + where + " has no numeric \"opacity\".");
      os.opacity = static_cast<float>(opacity->asNumber());
      os.midpoint = static_cast<float>(stop.numberOr("midpoint", 0.5));
      parsed.opacityStops.push_back(os);
    }
  }

  *nameOut = std::move(parsedName);
  *out = std::move(parsed);
  return true;
}

bool saveGradientPresetToFile(const std::string& path, const std::string& name,
                              const GradientPresetStops& stops, std::string* errorOut) {
  if (errorOut) errorOut->clear();

  std::string text;
  writeGradientPreset(name, stops, &text);

  const std::filesystem::path p(path);
  std::error_code ec;
  if (p.has_parent_path() && !p.parent_path().empty()) {
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec && !std::filesystem::exists(p.parent_path())) {
      if (errorOut)
        *errorOut = "refused: could not create the directory '" + p.parent_path().string() +
                    "' for gradient preset " + quotedName(name) + " (" + ec.message() + ").";
      return false;
    }
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (errorOut)
      *errorOut = "refused: could not open '" + path + "' for writing. Check the directory "
                  "exists and is writable.";
    return false;
  }
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  out.close();
  if (!out) {
    if (errorOut)
      *errorOut = "refused: '" + path +
                  "' was opened but not fully written; the file on disk is incomplete.";
    return false;
  }
  return true;
}

bool loadGradientPresetFromFile(const std::string& path, std::string* nameOut,
                                GradientPresetStops* out, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (nameOut == nullptr || out == nullptr) return false;

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (errorOut)
      *errorOut = "refused: there is no gradient preset file at '" + path +
                  "'. List the gradient library to see what is there.";
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (errorOut)
      *errorOut = "refused: '" + path + "' exists but could not be opened for reading.";
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return readGradientPreset(buffer.str(), path, nameOut, out, errorOut);
}

bool deleteGradientPresetFile(const std::string& path, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (errorOut)
      *errorOut = "refused: there is no gradient preset file at '" + path + "' to delete.";
    return false;
  }
  if (!std::filesystem::remove(path, ec) || ec) {
    if (errorOut)
      *errorOut = "refused: '" + path + "' could not be removed (" + ec.message() + ").";
    return false;
  }
  return true;
}

std::string gradientPresetsDirectoryPath() {
  // `actionsDirectoryPath()`'s resolver, one leaf over. The override comes
  // first so `--selftest` and the golden harness never touch the real
  // library -- see this header's own note and `tools/golden/run_golden.sh`'s
  // `NP_GRADIENT_DIR`.
  if (const char* explicitPath = std::getenv("NP_GRADIENT_DIR")) {
    if (*explicitPath != '\0') return explicitPath;
  }
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  if (home && *home)
    return std::string(home) + "/Library/Application Support/naturalPaint/gradients";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
    if (*xdg != '\0') return std::string(xdg) + "/naturalPaint/gradients";
  }
  if (home && *home) return std::string(home) + "/.config/naturalPaint/gradients";
#endif
  return "gradients";
}

std::string gradientPresetFileNameFor(std::string_view name) {
  // `actionFileNameFor()`'s sanitiser, unchanged: a name is user text and a
  // file name is not.
  constexpr size_t kMaxStem = 96;

  std::string stem;
  stem.reserve(std::min(name.size(), kMaxStem));
  for (const char c : name) {
    if (stem.size() >= kMaxStem) break;
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == ' ' || c == '.' || c == '_' || c == '-';
    stem.push_back(safe ? c : '_');
  }
  size_t first = 0;
  while (first < stem.size() && (stem[first] == '.' || stem[first] == ' ')) ++first;
  stem.erase(0, first);
  while (!stem.empty() && stem.back() == ' ') stem.pop_back();
  if (stem.empty()) return {};
  return stem + kGradientPresetFileExtension;
}

std::vector<std::string> listGradientPresetFiles(const std::string& dir) {
  std::vector<std::string> found;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return found;

  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entryEc;
    if (!it->is_regular_file(entryEc) || entryEc) continue;
    if (it->path().extension() != kGradientPresetFileExtension) continue;
    found.push_back(it->path().string());
  }
  std::sort(found.begin(), found.end());
  return found;
}

}  // namespace np
