#include "app/selftest/Support.hpp"

#include "app/AppState.hpp"
#include "app/Keymap.hpp"
#include "app/ToolSwitch.hpp"
#include "ui/AtelierChrome.hpp"

namespace np {

// ---------------------------------------------------------------------------
// Tool hotkeys -- docs/shortcuts.md §1, and the assertion that keeps it true.
//
// **The state this section exists to make unreachable.** Before it, no tool in
// this build had a working letter key. `ui/AtelierChrome.cpp`'s `kToolMeta`
// carried a `shortcut` column -- "B", "V", "Shift+M" -- which twenty-one
// palette tooltips printed and *nothing else read*: `keymaps/default.json`
// held roughly forty-six bindings and not one of them switched a tool, and
// `src/main.cpp`'s dispatch chain had no tool arm to switch one with. Three
// tiers, each internally consistent, jointly telling the user a lie. That
// failure did not need a bug to happen; it needed only a table whose two
// halves nobody was obliged to keep in step.
//
// So the load-bearing part of this file is not "the Brush is on B". It is
// **section 2, which walks both directions**:
//
//   forward  -- every `kToolMeta` row with a non-empty `shortcut` has exactly
//               one binding in the shipped default keymap, on exactly that
//               chord, naming exactly that tool's slug.
//   backward -- every `tool_`-prefixed binding in that file names a slug some
//               `Tool` actually declares, on the chord that tool's own row
//               reserves.
//
// One direction alone would restore the defect in miniature: forward-only lets
// `keymaps/default.json` accumulate bindings for tools that do not exist (a
// slug renamed in the table, the data file not followed -- a key that presses
// and does nothing, which is what we started with); backward-only lets a
// shortcut letter be added to a tooltip with no binding behind it, which is
// *exactly* what we started with. The pair is the deliverable.
//
// **The chord is re-derived, not read twice.** `expectedChord()` below parses
// the tooltip string -- "Shift+M" -> shift plus the key SDL names "M" -- and
// compares it to the chord `Keymap` parsed out of the JSON. Asking the keymap
// what it bound and then asserting that against itself is the shape of test
// this repo has already shipped and had to fix; the two sides here come from
// two files and meet in the middle.
//
// Headless, GPU-free: a keymap file, a `constexpr` table and one `AppState`.
// ---------------------------------------------------------------------------

namespace {

// The chord `label` denotes, in `kToolMeta`'s own display spelling: an
// optional "Shift+" prefix and then a key name SDL knows. `std::nullopt` for
// anything else -- including "Cmd+..." and "Alt+...", which no tool row uses
// today and which this deliberately refuses rather than silently ignores, so
// a future row spelled that way arrives as a red line instead of a chord with
// its modifier quietly dropped.
std::optional<KeyChord> expectedChord(const std::string& label) {
  if (label.empty()) return std::nullopt;
  std::string key = label;
  uint16_t mods = 0;
  const std::string shiftPrefix = "Shift+";
  if (key.rfind(shiftPrefix, 0) == 0) {
    mods |= kModShift;
    key = key.substr(shiftPrefix.size());
  }
  if (key.find('+') != std::string::npos) return std::nullopt;
  const SDL_Keycode k = SDL_GetKeyFromName(key.c_str());
  if (k == SDLK_UNKNOWN) return std::nullopt;
  return KeyChord{k, mods};
}

std::string chordText(KeyChord c) {
  std::string out;
  if (c.mods & kModCmd) out += "Cmd+";
  if (c.mods & kModCtrl) out += "Ctrl+";
  if (c.mods & kModAlt) out += "Alt+";
  if (c.mods & kModShift) out += "Shift+";
  const char* n = SDL_GetKeyName(c.key);
  out += (n && n[0]) ? n : "?";
  return out;
}

}  // namespace

bool runToolHotkeysTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const size_t toolCount = static_cast<size_t>(Tool::Count);

  // --- 1. the slug column itself -------------------------------------------
  //
  // Before either direction of the cross-check can mean anything, the slugs
  // have to be usable as identities: present on every row, distinct, and
  // reversible. A duplicated slug would make the forward walk pass while two
  // tools fought over one key, and an empty one would make
  // `toolFromSelectAction("tool_")` select a tool.
  {
    bool allNonEmpty = true, roundTrips = true, allUnique = true;
    for (size_t i = 0; i < toolCount; ++i) {
      const Tool t = static_cast<Tool>(i);
      const std::string slug = toolSlug(t);
      if (slug.empty()) {
        allNonEmpty = false;
        std::printf("    %s has no slug\n", toolName(t));
        continue;
      }
      if (toolForSlug(slug) != std::optional<Tool>(t)) {
        roundTrips = false;
        std::printf("    slug \"%s\" does not resolve back to %s\n", slug.c_str(), toolName(t));
      }
      for (size_t j = 0; j < i; ++j)
        if (slug == toolSlug(static_cast<Tool>(j))) {
          allUnique = false;
          std::printf("    slug \"%s\" is on both %s and %s\n", slug.c_str(),
                      toolName(static_cast<Tool>(j)), toolName(t));
        }
    }
    check(allNonEmpty, "every Tool declares a non-empty slug");
    check(allUnique, "no two Tools share a slug");
    check(roundTrips, "toolForSlug(toolSlug(t)) == t for every Tool");

    // The unknown row's contract, the same one `toolName()` gives -- a stray
    // cast must not name a tool, and the empty slug must not match it.
    check(std::string(toolSlug(Tool::Count)).empty(),
          "toolSlug(Tool::Count) is empty, not a real tool's slug");
    check(toolForSlug("") == std::nullopt, "the empty slug names no tool");

    check(toolSelectActionName(Tool::Brush) == "tool_brush",
          "toolSelectActionName(Brush) == \"tool_brush\"");
    check(toolFromSelectAction("tool_brush") == std::optional<Tool>(Tool::Brush),
          "toolFromSelectAction(\"tool_brush\") == Tool::Brush");
    // The three ways the dispatch arm must decline. `"undo"` stands for every
    // other arm in main.cpp's chain, which has to fall past the tool arm
    // untouched; the other two are a malformed keymap, which must select
    // nothing rather than the first row of the table.
    check(toolFromSelectAction("undo") == std::nullopt,
          "a non-tool action name selects no tool");
    check(toolFromSelectAction("tool_") == std::nullopt,
          "the bare prefix with no slug selects no tool");
    check(toolFromSelectAction("tool_not_a_tool") == std::nullopt,
          "an unknown slug selects no tool, rather than tool zero");
    // A prefix match, not a substring one: `"retool_brush"` contains
    // `"tool_brush"` and must not select the Brush.
    check(toolFromSelectAction("retool_brush") == std::nullopt,
          "the prefix is anchored -- \"retool_brush\" selects no tool");
  }

  // --- 2. the two directions ------------------------------------------------
  Keymap km;
  const bool loaded = km.loadFromFile("default.json");
  check(loaded, "keymaps/default.json loads");
  if (!loaded) {
    std::printf("[selftest] tool hotkeys %s\n", "FAIL");
    return false;
  }

  // A tool binding that collides with a command binding is a conflict the
  // load-time detector already reports; this section has a direct stake in it
  // (twenty-one new global chords is where a collision would come from), so
  // it is asserted here too rather than left to the keymap section alone.
  check(!km.hasConflicts(),
        "the twenty-one tool chords collide with nothing already bound");

  // forward: table -> file
  {
    size_t withShortcut = 0;
    bool everyOneBound = true;
    for (size_t i = 0; i < toolCount; ++i) {
      const Tool t = static_cast<Tool>(i);
      const std::string label = toolShortcutLabel(t);
      if (label.empty()) continue;
      ++withShortcut;
      const std::optional<KeyChord> want = expectedChord(label);
      if (!want) {
        everyOneBound = false;
        std::printf("    %s's shortcut \"%s\" is not a chord this build can bind\n",
                    toolName(t), label.c_str());
        continue;
      }
      const std::string wantAction = toolSelectActionName(t);
      size_t matches = 0;
      for (const KeyBinding& b : km.bindings()) {
        if (b.action != wantAction) continue;
        ++matches;
        if (!(b.chord == *want)) {
          everyOneBound = false;
          std::printf("    %s: table says %s, keymap binds %s\n", toolName(t),
                      chordText(*want).c_str(), chordText(b.chord).c_str());
        }
        // Tool switches are global. A scoped one would be a tool that
        // silently stops responding to its own letter on some layer kinds --
        // the failure mode of a modal keymap nobody announced.
        if (b.scope.has_value()) {
          everyOneBound = false;
          std::printf("    %s's binding is scoped to %s; tool switches are global\n",
                      toolName(t), layerKindName(*b.scope));
        }
      }
      if (matches != 1) {
        everyOneBound = false;
        std::printf("    %s (\"%s\"): %zu bindings for \"%s\", expected exactly 1\n",
                    toolName(t), label.c_str(), matches, wantAction.c_str());
      }
    }
    check(withShortcut > 0, "kToolMeta reserves at least one shortcut letter");
    check(everyOneBound,
          "every reserved shortcut letter is bound to its own tool");
    // Reported, not claimed: the line above is the assertion, this is the
    // scale of what it walked. It said "all bound" until a sabotage run
    // printed it under a red line.
    std::printf("    %zu of %zu tools reserve a letter\n", withShortcut, toolCount);

    // backward: file -> table
    size_t toolBindings = 0;
    bool everyBindingReal = true;
    for (const KeyBinding& b : km.bindings()) {
      if (b.action.rfind("tool_", 0) != 0) continue;
      ++toolBindings;
      const std::optional<Tool> named = toolFromSelectAction(b.action);
      if (!named) {
        everyBindingReal = false;
        std::printf("    keymap binds \"%s\" -- no Tool declares that slug\n",
                    b.action.c_str());
        continue;
      }
      const std::optional<KeyChord> want = expectedChord(toolShortcutLabel(*named));
      if (!want || !(*want == b.chord)) {
        everyBindingReal = false;
        std::printf("    keymap binds %s to \"%s\", but %s's row reserves \"%s\"\n",
                    chordText(b.chord).c_str(), b.action.c_str(), toolName(*named),
                    toolShortcutLabel(*named).c_str());
      }
    }
    check(everyBindingReal,
          "every tool_* binding names a real tool on its own chord");
    // The count equality is what closes the loop: forward proves each letter
    // has a binding, backward proves each binding is real, and this proves
    // there is no *third* tool binding hiding on a chord no tooltip mentions.
    check(toolBindings == withShortcut,
          "the file holds exactly one tool binding per reserved letter");
  }

  // --- 3. the runtime path, not just the two tables -------------------------
  //
  // Sections 1 and 2 would both pass on a keymap that parsed correctly and a
  // `resolve()` that returned nothing, so this presses three real chords the
  // way main.cpp's key-down handler does and follows the answer through the
  // same `toolFromSelectAction()` -> `setActiveTool()` the dispatch arm calls.
  {
    const auto b = km.resolve(KeyChord{SDLK_B, 0}, std::nullopt);
    check(b == std::optional<std::string>("tool_brush"), "resolve(B) -> tool_brush");

    const auto shiftM = km.resolve(KeyChord{SDLK_M, kModShift}, std::nullopt);
    check(shiftM == std::optional<std::string>("tool_ellipse_marquee"),
          "resolve(Shift+M) -> tool_ellipse_marquee (the flyout sibling)");

    // The Marquee's own letter, pressed while a Flats layer is active.
    // `resolve()` prefers a scoped binding, so a Flats-scoped `M` -- which is
    // what keymaps/default.json carried until this commit moved the two-click
    // merge to `U`, per docs/shortcuts.md §1.1 -- would shadow it and leave
    // the Marquee the one tool unreachable by key while flatting.
    const auto mOnFlats = km.resolve(KeyChord{SDLK_M, 0}, LayerKind::Flats);
    check(mOnFlats == std::optional<std::string>("tool_rect_marquee"),
          "resolve(M) on a Flats layer still selects the Marquee");
    check(km.resolve(KeyChord{SDLK_U, 0}, LayerKind::Flats) ==
              std::optional<std::string>("flats_merge_pair"),
          "the two-click merge moved to U, where §1.1 puts it");

    AppState st;
    st.brush.tool = Tool::Brush;
    const auto zoom = km.resolve(KeyChord{SDLK_Z, 0}, std::nullopt);
    const std::optional<Tool> picked =
        zoom ? toolFromSelectAction(*zoom) : std::nullopt;
    if (picked) setActiveTool(st, *picked);
    check(st.brush.tool == Tool::Zoom,
          "pressing Z runs the whole route and leaves the Zoom active");
    // app/ToolSwitch is the single writer, and a key press is a deliberate
    // pick like a palette click -- so the ledger has to record where the user
    // came from, or T20's spring-loaded Hand gives back the wrong tool.
    check(st.tools.previous == Tool::Brush,
          "a hotkey switch records the previous tool, like any other pick");
  }

  // --- 4. a bare letter must not fire mid-word ------------------------------
  //
  // `keyChordReachesKeymap()` (app/Keymap.hpp) already gated this before the
  // tool bindings existed, and it is not re-implemented here -- but until this
  // commit the gate protected four bare chords, and it now protects
  // twenty-one letters that would otherwise retype the user's caption as a
  // sequence of tool changes. So the claim is asserted over the *actual set*
  // of tool chords rather than a representative one: every chord this file's
  // section 2 just proved is bound must be refused while a session is live,
  // and reach the keymap when none is.
  {
    bool allGated = true, allReachWhenIdle = true;
    size_t checked = 0;
    for (const KeyBinding& bnd : km.bindings()) {
      if (bnd.action.rfind("tool_", 0) != 0) continue;
      ++checked;
      if (keyChordReachesKeymap(bnd.chord, /*textSessionActive=*/true)) {
        allGated = false;
        std::printf("    %s (%s) still reaches the keymap while typing\n",
                    chordText(bnd.chord).c_str(), bnd.action.c_str());
      }
      if (!keyChordReachesKeymap(bnd.chord, /*textSessionActive=*/false))
        allReachWhenIdle = false;
    }
    check(checked > 0 && allGated,
          "no tool chord reaches the keymap while a text session is live");
    check(allReachWhenIdle, "every tool chord reaches the keymap when none is");
    // The other half of the gate, so this section fails an implementation that
    // simply returned false always -- which would pass the two claims above
    // and silently kill Cmd+Z.
    check(keyChordReachesKeymap(KeyChord{SDLK_Z, kModCmd}, /*textSessionActive=*/true),
          "Cmd chords still reach the keymap while a text session is live");
  }

  std::printf("[selftest] tool hotkeys %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
