#pragma once

// app/Keymap -- PLAN.md Phase 2 step 15 (PRD R7, R8).
//
// Bindings loaded from a data file, not `if (key == ...)` scattered through
// the UI (main.cpp used to do exactly that; see Keymap.cpp's header comment
// and main.cpp's key-down handling for what replaced it). Actions are
// named strings, not function pointers, so a tool can register an action
// without this file knowing it exists. Conflicts -- two bindings on the
// identical key+modifier chord whose scopes overlap -- are detected at load
// time and reported, including conflicts that only exist within one
// layer-kind scope. Default keymap and the reasoning behind every binding
// in it: docs/shortcuts.md.
//
// Scope note: only a small, real subset of docs/shortcuts.md's full
// Photoshop-matching keymap is shipped in keymaps/default.json today --
// pause, clear canvas, reload shaders, quit are the only key-triggered
// behaviours that exist anywhere in this codebase as of Phase 2 step 15.
// Everything else in that document (tools, view controls, layer/document
// commands) belongs to the phases that build the things those keys would
// operate on; binding a key to an action that does nothing yet would be
// worse than not building this machinery generally enough for those phases
// to add their own actions later without touching this file.

#include <SDL3/SDL_keycode.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/Layer.hpp"

namespace np {

// The layer kinds a binding can be scoped to (CONTEXT.md "Layer kinds").
// LayerKind itself -- and its layerKindName()/layerKindFromName() helpers --
// live in core/Layer.hpp, not here: a layer's kind is a core domain concept
// ("what kind of Layer this is"), and app/ depends on core/, never the
// reverse. core/Document + core/Layer (PLAN.md Phase 2 step 4) ship only
// `RGB` for real -- "design for N, ship 1" -- but all seven kinds exist in
// the enum, which is what lets the keymap's scope-awareness be a real,
// working capability now, exercised by a test (SelfTest.cpp:
// runKeymapTest()), rather than something bolted on once
// Pigment/Media/Strokes/Adjustment/Text/Flats layers actually exist.

// The four modifier bits a binding can express -- the same SDL_KMOD_*
// vocabulary main.cpp already reads off a live key event (see its old
// `e.key.mod & SDL_KMOD_GUI` check), just folded to one bit per logical
// modifier so a chord doesn't care whether it was the left or right
// Cmd/Shift/Alt/Ctrl key. Not a parallel vocabulary -- keyModsFromSDL()
// below is the one and only place an SDL_Keymod becomes one of these.
enum KeyMod : uint16_t {
  kModCmd = 1u << 0,    // SDL_KMOD_GUI -- Command on macOS
  kModShift = 1u << 1,  // SDL_KMOD_SHIFT
  kModAlt = 1u << 2,    // SDL_KMOD_ALT -- Option on macOS
  kModCtrl = 1u << 3,   // SDL_KMOD_CTRL
};

// Reduces a live SDL_Keymod bitmask (e.g. from SDL_KeyboardEvent::mod) to
// the four bits above.
uint16_t keyModsFromSDL(SDL_Keymod sdlMods);

// One key chord: an SDL virtual keycode plus the modifier bits above.
struct KeyChord {
  SDL_Keycode key = SDLK_UNKNOWN;
  uint16_t mods = 0;

  bool operator==(const KeyChord& o) const { return key == o.key && mods == o.mods; }
};

// One row of the keymap file: a chord bound to a named action, optionally
// scoped to a single layer kind. `scope == std::nullopt` means global --
// binds regardless of what kind of layer is active (or if there is no
// active layer at all).
struct KeyBinding {
  KeyChord chord;
  std::string action;
  std::optional<LayerKind> scope;
};

// A detected conflict: two bindings whose chords are identical and whose
// scopes overlap. See Keymap.cpp's detectConflicts() for the exact overlap
// rule (PRD R8's "including conflicts that occur only within one
// layer-kind scope").
struct KeymapConflict {
  std::string actionA;
  std::string actionB;
  KeyChord chord;
  // Human-readable, e.g. "global", "Flats", "global vs Flats".
  std::string scopeDescription;
};

// Loads bindings from a data file, resolves a key event + active-layer-kind
// context to an action name, and detects binding conflicts at load time.
// See docs/shortcuts.md §7 for the design this implements.
class Keymap {
 public:
  // Loads from `${NP_KEYMAP_DIR}/relativePath` -- the same on-disk-under-a-
  // compile-time-root convention gfx/ShaderLoader.cpp uses for shaders, so
  // the keymap stays hand-editable and hot-reloadable like everything else
  // this project keeps external to the binary. Returns false if the file
  // can't be read or fails to parse. A successfully parsed keymap that
  // *does* have conflicts still loads and is still usable -- see
  // conflicts() -- because the point of detecting them is to show a human
  // (an in-app editor, later) something to fix, not to refuse to run.
  bool loadFromFile(std::string_view relativePath);

  // Same, but from an in-memory JSON string. `sourceLabel` is only used in
  // error/conflict messages (e.g. "test fixture" instead of a file path) --
  // this is what lets a test exercise the parser and conflict detector
  // without touching disk.
  bool loadFromString(std::string_view json, std::string_view sourceLabel);

  // Resolves a chord to an action name given the kind of the currently
  // active layer, if any. Returns nullopt if nothing binds this chord in
  // this scope. See Keymap.cpp for the scope tie-break.
  std::optional<std::string> resolve(KeyChord chord,
                                      std::optional<LayerKind> activeScope) const;

  const std::string& name() const { return name_; }
  const std::vector<KeyBinding>& bindings() const { return bindings_; }
  const std::vector<KeymapConflict>& conflicts() const { return conflicts_; }
  bool hasConflicts() const { return !conflicts_.empty(); }

  // Prints every recorded conflict to stderr, one line each -- which two
  // actions, which key, which scope -- matching the codebase's existing
  // std::fprintf(stderr, ...) convention (e.g. gfx/ShaderLoader.cpp).
  void reportConflicts() const;

 private:
  std::string name_;
  std::vector<KeyBinding> bindings_;
  std::vector<KeymapConflict> conflicts_;

  bool parse(std::string_view json, std::string_view sourceLabel);
  void detectConflicts();
};

}  // namespace np
