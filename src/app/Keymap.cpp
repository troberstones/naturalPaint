#include "app/Keymap.hpp"

#include <SDL3/SDL_keyboard.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/Layer.hpp"
#include "io/Json.hpp"
#include "core/ResourcePaths.hpp"

namespace np {
namespace {

namespace fs = std::filesystem;

// The JSON reader this file used to carry privately now lives in
// io/Json.hpp, which merged it with io/ExportAs.cpp's copy: that file had
// already written down the rule that a *third* consumer promotes the pair to
// a shared header, and io/ActionFile (docs/automation-plan.md) is the third.
// The merged reader stores its first error instead of printing it, so the
// `[keymap]` stderr line this file has always written is emitted by
// `Keymap::parse()` below rather than from inside the reader.

// Parses one `{ "key": ..., "mods": [...], "action": ..., "scope": ... }`
// binding object. `key` and `action` are required; `mods` and `scope`
// default to "no modifiers" and "global" respectively when absent.
bool parseBindingObject(JsonReader& r, KeyBinding& out) {
  if (!r.expect('{')) return false;

  std::string keyName, action, scopeName;
  bool haveKey = false, haveAction = false, haveScope = false;
  std::vector<std::string> modNames;

  if (r.peek() != '}') {
    while (true) {
      std::string field;
      if (!r.parseString(&field)) return false;
      if (!r.expect(':')) return false;

      if (field == "key") {
        std::string v;
        if (!r.parseString(&v)) return false;
        keyName = v;
        haveKey = true;
      } else if (field == "action") {
        std::string v;
        if (!r.parseString(&v)) return false;
        action = v;
        haveAction = true;
      } else if (field == "scope") {
        std::string v;
        if (!r.parseString(&v)) return false;
        scopeName = v;
        haveScope = true;
      } else if (field == "mods") {
        if (!r.parseStringArray(&modNames)) return false;
      } else {
        r.fail("unknown binding field \"" + field + "\"");
        return false;
      }

      if (r.peek() == ',') {
        r.expect(',');
        continue;
      }
      break;
    }
  }
  if (!r.expect('}')) return false;

  if (!haveKey || !haveAction) {
    r.fail("binding is missing required \"key\" or \"action\"");
    return false;
  }

  out.chord.key = SDL_GetKeyFromName(keyName.c_str());
  if (out.chord.key == SDLK_UNKNOWN) {
    r.fail("unrecognized key name \"" + keyName + "\"");
    return false;
  }
  out.chord.mods = 0;
  for (const auto& m : modNames) {
    if (m == "Cmd") out.chord.mods |= kModCmd;
    else if (m == "Shift") out.chord.mods |= kModShift;
    else if (m == "Alt") out.chord.mods |= kModAlt;
    else if (m == "Ctrl") out.chord.mods |= kModCtrl;
    else {
      r.fail("unrecognized modifier \"" + m + "\"");
      return false;
    }
  }
  out.action = action;
  out.scope.reset();
  if (haveScope) {
    auto k = layerKindFromName(scopeName);
    if (!k) {
      r.fail("unrecognized layer-kind scope \"" + scopeName + "\"");
      return false;
    }
    out.scope = k;
  }
  return true;
}

std::string chordDescription(const KeyChord& c) {
  std::string out;
  if (c.mods & kModCmd) out += "Cmd+";
  if (c.mods & kModCtrl) out += "Ctrl+";
  if (c.mods & kModAlt) out += "Alt+";
  if (c.mods & kModShift) out += "Shift+";
  const char* name = SDL_GetKeyName(c.key);
  out += (name && name[0]) ? name : "?";
  return out;
}

}  // namespace

// layerKindName()/layerKindFromName() now live in core/Layer.hpp (relocated
// there in the same step that added core/Document + core/Layer -- see this
// file's header comment) -- both used below via that include, no local
// definition needed.

uint16_t keyModsFromSDL(SDL_Keymod sdlMods) {
  uint16_t m = 0;
  if (sdlMods & SDL_KMOD_GUI) m |= kModCmd;
  if (sdlMods & SDL_KMOD_SHIFT) m |= kModShift;
  if (sdlMods & SDL_KMOD_ALT) m |= kModAlt;
  if (sdlMods & SDL_KMOD_CTRL) m |= kModCtrl;
  return m;
}

bool keyChordReachesKeymap(KeyChord chord, bool textSessionActive) noexcept {
  if (!textSessionActive) return true;
  return (chord.mods & (kModCmd | kModCtrl)) != 0;
}

bool Keymap::loadFromFile(std::string_view relativePath) {
  const fs::path p = fs::path(keymapDir()) / relativePath;
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "[keymap] cannot read %s\n", p.string().c_str());
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return loadFromString(ss.str(), p.string());
}

bool Keymap::loadFromString(std::string_view json, std::string_view sourceLabel) {
  bindings_.clear();
  conflicts_.clear();
  name_.clear();
  if (!parse(json, sourceLabel)) return false;
  detectConflicts();
  return true;
}

bool Keymap::parse(std::string_view json, std::string_view sourceLabel) {
  JsonReader r(json, sourceLabel);
  // io/Json's reader STORES its first error rather than printing it, so the
  // one `[keymap]` stderr line this loader has always written is emitted here
  // instead -- once, at the single point every `return false` below funnels
  // through. That is what the lambda is for: it gives a body with a dozen
  // early exits one place to report from, without rewriting any of them.
  const bool ok = [&]() -> bool {
    if (!r.expect('{')) return false;

    bool sawBindings = false;
    if (r.peek() != '}') {
      while (true) {
        std::string key;
        if (!r.parseString(&key)) return false;
        if (!r.expect(':')) return false;

        if (key == "name") {
          std::string v;
          if (!r.parseString(&v)) return false;
          name_ = v;
        } else if (key == "bindings") {
          if (!r.expect('[')) return false;
          sawBindings = true;
          if (r.peek() != ']') {
            while (true) {
              KeyBinding b;
              if (!parseBindingObject(r, b)) return false;
              bindings_.push_back(std::move(b));
              if (r.peek() == ',') {
                r.expect(',');
                continue;
              }
              break;
            }
          }
          if (!r.expect(']')) return false;
        } else {
          r.fail("unknown top-level key \"" + key + "\"");
          return false;
        }

        if (r.peek() == ',') {
          r.expect(',');
          continue;
        }
        break;
      }
    }
    if (!r.expect('}')) return false;
    if (!r.ok()) return false;

    if (!sawBindings) {
      std::fprintf(stderr, "[keymap] %.*s: missing \"bindings\" array\n",
                   static_cast<int>(sourceLabel.size()), sourceLabel.data());
      return false;
    }
    return true;
  }();
  if (!ok && !r.error().empty())
    std::fprintf(stderr, "[keymap] %s\n", r.error().c_str());
  return ok;
}

// Two bindings sharing an identical chord are a real conflict exactly when
// their scopes overlap (PRD R8):
//   - both global                                -> overlap (trivial dup)
//   - same specific layer kind                   -> overlap
//   - one global, one scoped                      -> overlap (global applies
//     everywhere, so it always collides with any specific scope)
//   - two different, specific layer kinds          -> NOT an overlap; a
//     layer is never simultaneously two kinds, so e.g. a Flats-scoped and a
//     Media-scoped binding on the same key never actually contend at
//     runtime, and flagging them would be a false positive.
// Two bindings sharing a chord *and* the same action name are a harmless
// redundant duplicate (e.g. hand-editing a file into having the same row
// twice), not a collision between two different behaviours, so those are
// skipped rather than reported.
void Keymap::detectConflicts() {
  conflicts_.clear();
  for (size_t i = 0; i < bindings_.size(); ++i) {
    for (size_t j = i + 1; j < bindings_.size(); ++j) {
      const KeyBinding& a = bindings_[i];
      const KeyBinding& b = bindings_[j];
      if (!(a.chord == b.chord)) continue;
      if (a.action == b.action) continue;

      const bool overlaps = !a.scope.has_value() || !b.scope.has_value() ||
                            (*a.scope == *b.scope);
      if (!overlaps) continue;

      std::string desc;
      if (!a.scope && !b.scope) {
        desc = "global";
      } else if (a.scope && b.scope) {
        desc = layerKindName(*a.scope);
      } else {
        desc = std::string("global vs ") +
               layerKindName(a.scope ? *a.scope : *b.scope);
      }

      conflicts_.push_back(KeymapConflict{a.action, b.action, a.chord, desc});
    }
  }
}

std::optional<std::string> Keymap::resolve(KeyChord chord,
                                            std::optional<LayerKind> activeScope) const {
  // Scope-specific bindings are checked first, then global -- this is what
  // makes "layer-kind-scoped overrides rather than new global letters"
  // (docs/shortcuts.md §1 principle 4) an actual runtime behaviour: a
  // binding scoped to the active layer's kind takes the chord even if a
  // global binding also claims it. In a *conflict-free* keymap this
  // ordering rarely matters -- detectConflicts() already flags any chord
  // that is both globally bound and bound to the active-eligible scope, so
  // a well-formed keymap essentially never has both to choose between; the
  // ordering exists to make resolution deterministic while such a conflict
  // is being fixed, not to encode "scoped wins" as a silent conflict
  // resolution policy.
  if (activeScope) {
    for (const auto& b : bindings_)
      if (b.chord == chord && b.scope == activeScope) return b.action;
  }
  for (const auto& b : bindings_)
    if (b.chord == chord && !b.scope) return b.action;
  return std::nullopt;
}

void Keymap::reportConflicts() const {
  for (const auto& c : conflicts_) {
    std::fprintf(stderr,
                 "[keymap] conflict: \"%s\" and \"%s\" both bind %s (scope: %s)\n",
                 c.actionA.c_str(), c.actionB.c_str(), chordDescription(c.chord).c_str(),
                 c.scopeDescription.c_str());
  }
}

}  // namespace np
