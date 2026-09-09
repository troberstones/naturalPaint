#include "io/ActionFile.hpp"

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

// Named `quotedName` and not `quoted`: `<sstream>` drags in `std::quoted`,
// and ADL on a `std::string` argument pulls that into the overload set
// alongside a local `quoted()` -- where it WINS, because it takes
// `const std::string&` while this takes a `string_view`. The compiler then
// fails inside `<iomanip>`, several screens from the call that caused it.
std::string quotedName(std::string_view label) { return "\"" + std::string(label) + "\""; }

}  // namespace

// --------------------------------------------------------------- writing

bool writeAction(const Action& action, std::string* out, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (out == nullptr) return false;

  JsonValue doc = JsonValue::object();
  // The version first, because this is the file a person opens: the line that
  // decides whether the rest can be read should be the first one they see.
  // The reader does not depend on that position (see ActionFile.hpp §2) --
  // this is for the human, not for the parser.
  doc.set(kActionFileVersionKey, JsonValue::number(kActionFileVersion));
  doc.set("name", JsonValue::string(action.name));

  JsonValue steps = JsonValue::array();
  for (size_t i = 0; i < action.steps.size(); ++i) {
    const Command& step = action.steps[i];
    if (!step.params.isObject()) {
      if (errorOut)
        *errorOut = "refused: step " + std::to_string(i + 1) + " of action " +
                    quotedName(action.name) + " has parameters that are " +
                    jsonKindName(step.params.kind()) +
                    " rather than an object. A step's parameters are always an object, "
                    "possibly empty, so that a reader never has to tell \"no parameters\" "
                    "from \"not an object\".";
      return false;
    }
    JsonValue outStep = JsonValue::object();
    outStep.set(kActionStepCommandKey, JsonValue::string(step.id));
    for (const auto& [key, value] : step.params.members()) {
      if (key == kActionStepCommandKey) {
        // ActionFile.hpp §3's reserved key, and the whole price of the flat
        // form. Writing it would emit `cmd` twice; io/Json keeps the first
        // duplicate on read, so the parameter would vanish on the round trip
        // with nothing said anywhere. A named refusal is the cheap half of
        // that trade and this is where it is paid.
        if (errorOut)
          *errorOut = "refused: step " + std::to_string(i + 1) + " of action " +
                      quotedName(action.name) + " has a parameter called " +
                      quotedName(kActionStepCommandKey) +
                      ", which is the key that names the step's command. Parameters sit beside "
                      "\"cmd\" in a .npaction file, so that one name is reserved. Rename the "
                      "parameter.";
        return false;
      }
      outStep.set(key, value);
    }
    steps.push(std::move(outStep));
  }
  doc.set("steps", std::move(steps));

  // Pretty, two spaces -- ActionFile.hpp §4 -- plus a trailing newline,
  // because a file without one is the thing every text tool complains about
  // and `parseJson()` skips trailing whitespace anyway.
  *out = doc.write(2);
  out->push_back('\n');
  return true;
}

// --------------------------------------------------------------- reading

bool readAction(std::string_view text, std::string_view label, Action* out,
                std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (out == nullptr) return false;

  const auto refuse = [&](const std::string& why) {
    if (errorOut) *errorOut = why;
    return false;
  };

  JsonValue doc;
  std::string parseError;
  if (!parseJson(text, label, &doc, &parseError)) {
    return refuse("refused: " + parseError +
                  ". A .npaction file is JSON; fix the syntax at that byte, or open the file "
                  "with the editor that wrote it.");
  }
  if (!doc.isObject()) {
    return refuse("refused: " + quotedName(label) + " holds " + jsonKindName(doc.kind()) +
                  " at its top level, not an object. A .npaction file is one JSON object with "
                  "an \"" + kActionFileVersionKey + "\" key.");
  }

  // ---- the version, before anything else is interpreted ----------------
  //
  // ActionFile.hpp §2. Nothing below this block has looked at "name" or
  // "steps", and nothing has been written into `*out`.
  const JsonValue* version = doc.find(kActionFileVersionKey);
  if (version == nullptr) {
    return refuse("refused: " + quotedName(label) + " has no \"" + kActionFileVersionKey +
                  "\" key, so it is not a naturalPaint action -- or it is one that was "
                  "hand-edited until the key was lost. It is NOT read as version " +
                  std::to_string(kActionFileVersion) +
                  " on the assumption that it probably is one: a file whose steps mean "
                  "something else would then run. Add \"" + kActionFileVersionKey + "\": " +
                  std::to_string(kActionFileVersion) + " if this file really is an action.");
  }
  if (!version->isNumber()) {
    return refuse("refused: " + quotedName(label) + " has an \"" + kActionFileVersionKey +
                  "\" key holding " + jsonKindName(version->kind()) +
                  " rather than a number. The version is a whole number; this build reads " +
                  std::to_string(kActionFileVersion) + ".");
  }
  const double raw = version->asNumber();
  if (raw != std::floor(raw) || raw != static_cast<double>(kActionFileVersion)) {
    // Written with the value the file actually holds, formatted the way
    // io/Json would write it back, so the sentence names the same number the
    // user can see in their editor.
    return refuse("refused: " + quotedName(label) + " is a version " +
                  JsonValue::number(raw).write(-1) + " action and this build reads version " +
                  std::to_string(kActionFileVersion) +
                  ". Open it with the build that wrote it, or re-save it from there.");
  }

  Action parsed;

  const JsonValue* name = doc.find("name");
  if (name != nullptr) {
    if (!name->isString()) {
      return refuse("refused: " + quotedName(label) + " has a \"name\" key holding " +
                    jsonKindName(name->kind()) +
                    " rather than a string. An action's name is text; quote it.");
    }
    parsed.name = name->asString();
  }

  const JsonValue* steps = doc.find("steps");
  if (steps == nullptr) {
    // Distinguished from `"steps": []`, which IS accepted. An empty list is
    // something a person typed and can see; a missing key is a file that was
    // truncated or half-written, and reading it as "an action that does
    // nothing" is how a batch reports thirty successes having changed
    // nothing (docs/automation-plan.md §7).
    return refuse("refused: " + quotedName(label) + " has no \"steps\" key. An action with no "
                  "steps is written \"steps\": [], which says so on purpose; a missing key is "
                  "a file that was not finished, and running it would report success over "
                  "every file it touched without changing one.");
  }
  if (!steps->isArray()) {
    return refuse("refused: " + quotedName(label) + " has a \"steps\" key holding " +
                  jsonKindName(steps->kind()) +
                  " rather than an array. Steps are a JSON array even when there is one of "
                  "them. It is not read as an empty action, because an action that does "
                  "nothing reports success on every file in a batch.");
  }

  for (size_t i = 0; i < steps->size(); ++i) {
    const JsonValue& step = steps->at(i);
    const std::string where = "step " + std::to_string(i + 1) + " of " + quotedName(label);
    if (!step.isObject()) {
      return refuse("refused: " + where + " is " + jsonKindName(step.kind()) +
                    " rather than an object. Every step is an object with a \"" +
                    kActionStepCommandKey + "\" key and its parameters beside it.");
    }
    const JsonValue* cmd = step.find(kActionStepCommandKey);
    if (cmd == nullptr || !cmd->isString() || cmd->asString().empty()) {
      return refuse("refused: " + where + " has no \"" + kActionStepCommandKey +
                    "\" key naming a command. A step is keyed by a stable command id -- never "
                    "by a menu label and never by a number -- so that appending to an enum or "
                    "rewording a menu cannot change what a file that already exists means.");
    }

    Command parsedStep;
    parsedStep.id = cmd->asString();
    // Refused at LOAD, not at run. See ActionFile.hpp's `readAction()` note:
    // this is deliberately not `Op::unrecognised`'s rule, because an action is
    // executed and a skipped step writes a file that looks correct and is not.
    if (findCommand(parsedStep.id) == nullptr) {
      return refuse("refused: " + where + " names a command called " + quotedName(parsedStep.id) +
                    ", which this build does not have. The whole file is refused rather than "
                    "the step skipped: an action is executed, and a skipped step writes a "
                    "file that looks correct and is not. Run it with the build that wrote it.");
    }
    for (const auto& [key, value] : step.members()) {
      if (key == kActionStepCommandKey) continue;
      parsedStep.params.set(key, value);
    }
    parsed.steps.push_back(std::move(parsedStep));
  }

  // Assigned only here: every refusal above leaves the caller's action
  // exactly as it found it.
  *out = std::move(parsed);
  return true;
}

// --------------------------------------------------------------- files

bool saveActionToFile(const std::string& path, const Action& action, std::string* errorOut) {
  if (errorOut) errorOut->clear();

  // Encoded in full BEFORE the file is opened, which is io/ExportAs' own rule
  // and worth restating here: `std::ofstream`'s truncating open destroys what
  // was there the moment it succeeds, so a refusal discovered during encoding
  // would otherwise have already deleted the user's previous action.
  std::string text;
  if (!writeAction(action, &text, errorOut)) return false;

  const std::filesystem::path p(path);
  std::error_code ec;
  if (p.has_parent_path() && !p.parent_path().empty()) {
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec && !std::filesystem::exists(p.parent_path())) {
      if (errorOut)
        *errorOut = "refused: could not create the directory '" + p.parent_path().string() +
                    "' for action " + quotedName(action.name) + " (" + ec.message() + ").";
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

bool loadActionFromFile(const std::string& path, Action* out, std::string* errorOut) {
  if (errorOut) errorOut->clear();
  if (out == nullptr) return false;

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (errorOut)
      *errorOut = "refused: there is no action file at '" + path +
                  "'. List the actions library to see what is there.";
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
  return readAction(buffer.str(), path, out, errorOut);
}

std::string actionsDirectoryPath() {
  // `defaultExportPresetsPath()`'s resolver, one directory over. Copied in
  // shape rather than shared because every one of this codebase's eight
  // Application Support resolvers is this same eleven lines with its own
  // environment variable and its own leaf, and none of them has ever been the
  // thing that broke -- see app/DabLibrary.cpp, app/PanelLayout.cpp and the
  // rest. The override comes first so `--selftest` and a second profile never
  // touch the real library.
  if (const char* explicitPath = std::getenv("NP_ACTION_DIR")) {
    if (*explicitPath != '\0') return explicitPath;
  }
  const char* home = std::getenv("HOME");
#if defined(__APPLE__)
  if (home && *home) return std::string(home) + "/Library/Application Support/naturalPaint/actions";
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
    if (*xdg != '\0') return std::string(xdg) + "/naturalPaint/actions";
  }
  if (home && *home) return std::string(home) + "/.config/naturalPaint/actions";
#endif
  // No HOME at all (a stripped environment). io/ExportAs' own reasoning: the
  // working directory is a poor place for user data but a real, writable one,
  // and an empty path would turn "save action" into a mystery.
  return "actions";
}

std::string actionFileNameFor(std::string_view name) {
  // 96 bytes before the extension. Well inside every filesystem's per-
  // component limit (255 on APFS, HFS+ and ext4), and short enough that a
  // listing stays readable. A name longer than this keeps its head, which is
  // the part that distinguishes it.
  constexpr size_t kMaxStem = 96;

  std::string stem;
  stem.reserve(std::min(name.size(), kMaxStem));
  for (const char c : name) {
    if (stem.size() >= kMaxStem) break;
    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == ' ' || c == '.' || c == '_' || c == '-';
    stem.push_back(safe ? c : '_');
  }
  // Leading dots and spaces go, in that order and repeatedly: a leading dot
  // makes a hidden file, and a stem of "." or ".." would name a directory
  // rather than a file. Doing it after substitution rather than before means
  // a name of "../x" -- whose slash is already an underscore by now -- cannot
  // reach here still holding a separator.
  size_t first = 0;
  while (first < stem.size() && (stem[first] == '.' || stem[first] == ' ')) ++first;
  stem.erase(0, first);
  while (!stem.empty() && stem.back() == ' ') stem.pop_back();
  if (stem.empty()) return {};
  return stem + kActionFileExtension;
}

std::vector<std::string> listActionFiles(const std::string& dir) {
  std::vector<std::string> found;
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return found;

  // The non-throwing overload throughout: a library directory can contain a
  // dangling symlink or a file whose permissions changed under us, and a
  // listing that threw would take the ACTIONS panel down with it.
  // `ec` here is the ITERATOR's, and the per-entry queries below get their own
  // -- sharing one would let a single unreadable entry set the code that the
  // loop condition tests and end the listing early, silently, part way
  // through somebody's library.
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entryEc;
    if (!it->is_regular_file(entryEc) || entryEc) continue;
    if (it->path().extension() != kActionFileExtension) continue;
    found.push_back(it->path().string());
  }
  std::sort(found.begin(), found.end());
  return found;
}

}  // namespace np
