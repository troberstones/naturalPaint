#include "app/selftest/Support.hpp"

namespace np {

// Phase 2 step 15 / PRD R7, R8. Two halves:
//
// 1. The real, shipped keymaps/default.json loads clean (no false-positive
//    conflicts -- it only binds four distinct chords to four distinct
//    global actions today, so it shouldn't have any) and resolves its real
//    chords to the action names main.cpp actually dispatches on.
//
// 2. A small in-memory fixture keymap -- not keymaps/default.json --
//    purpose-built to exercise the layer-kind-scope-aware conflict
//    detector. The shipped default keeps to real, currently-existing
//    actions only (see Keymap.hpp's header comment for why binding a key to
//    a dead action would be worse than not building the scope machinery
//    generally); the scope-conflict behaviour genuinely has nothing to
//    exercise it in the real file today, since no Pigment/Media/Strokes/
//    Adjustment/Text/Flats layer exists anywhere in the codebase yet. This
//    fixture is that exercise: two different, currently-nonexistent
//    layer-kind scopes sharing a key (must NOT conflict), a global binding
//    added on top of one of them (must conflict, twice -- global overlaps
//    every scope), and a scope-only binding checked through resolve().
bool runKeymapTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- 1. the real default keymap ---
  Keymap km;
  const bool loaded = km.loadFromFile("default.json");
  check(loaded, "keymaps/default.json loads");
  if (loaded) {
    check(!km.hasConflicts(), "keymaps/default.json has no false-positive conflicts");

    const auto space = km.resolve(KeyChord{SDLK_SPACE, 0}, std::nullopt);
    check(space == std::optional<std::string>("toggle_pause"),
          "resolve(Space) -> toggle_pause");

    const auto cmdK = km.resolve(KeyChord{SDLK_K, kModCmd}, std::nullopt);
    const auto cmdN = km.resolve(KeyChord{SDLK_N, kModCmd}, std::nullopt);
    check(cmdK == std::optional<std::string>("clear_canvas") &&
              cmdN == std::optional<std::string>("clear_canvas"),
          "resolve(Cmd+K) and resolve(Cmd+N) both -> clear_canvas");

    const auto cmdR = km.resolve(KeyChord{SDLK_R, kModCmd}, std::nullopt);
    check(cmdR == std::optional<std::string>("reload_shaders"),
          "resolve(Cmd+R) -> reload_shaders");

    const auto cmdQ = km.resolve(KeyChord{SDLK_Q, kModCmd}, std::nullopt);
    check(cmdQ == std::optional<std::string>("quit"), "resolve(Cmd+Q) -> quit");

    const auto unbound = km.resolve(KeyChord{SDLK_Z, 0}, std::nullopt);
    check(unbound == std::nullopt, "resolve() on an unbound chord returns nothing");
  }

  // --- 2. the scope-conflict fixture (test-only; never shipped) ---
  const char* fixture = R"json(
{
  "name": "keymap test fixture",
  "bindings": [
    { "key": "G", "mods": ["Cmd"], "scope": "Flats",   "action": "fixture.flats_action" },
    { "key": "G", "mods": ["Cmd"], "scope": "Media",    "action": "fixture.media_action" },
    { "key": "G", "mods": ["Cmd"],                      "action": "fixture.global_action" },
    { "key": "H",                  "scope": "Strokes",  "action": "fixture.strokes_h_action" }
  ]
}
)json";

  Keymap fx;
  const bool fxLoaded = fx.loadFromString(fixture, "keymap test fixture");
  check(fxLoaded, "fixture keymap parses");
  if (fxLoaded) {
    auto hasConflict = [&](const std::string& a, const std::string& b) {
      for (const auto& c : fx.conflicts())
        if ((c.actionA == a && c.actionB == b) || (c.actionA == b && c.actionB == a))
          return true;
      return false;
    };

    check(!hasConflict("fixture.flats_action", "fixture.media_action"),
          "same key, disjoint layer-kind scopes (Flats vs Media) is NOT a conflict");
    check(hasConflict("fixture.global_action", "fixture.flats_action"),
          "same key, global vs Flats-scoped IS a conflict (global overlaps every scope)");
    check(hasConflict("fixture.global_action", "fixture.media_action"),
          "same key, global vs Media-scoped IS a conflict (global overlaps every scope)");
    check(fx.conflicts().size() == 2,
          "exactly the two global-vs-scoped pairs are reported, not the disjoint pair");

    const KeyChord h{SDL_GetKeyFromName("H"), 0};
    check(fx.resolve(h, LayerKind::Strokes) ==
              std::optional<std::string>("fixture.strokes_h_action"),
          "resolve(H, scope=Strokes) returns the Strokes-scoped action");
    check(fx.resolve(h, LayerKind::Media) == std::nullopt,
          "resolve(H, scope=Media) returns nothing -- H is only bound under Strokes");
    check(fx.resolve(h, std::nullopt) == std::nullopt,
          "resolve(H, no active layer) returns nothing -- no global H binding exists");
  }

  std::printf("[selftest] keymap %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


}  // namespace np
