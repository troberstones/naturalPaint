#include "app/selftest/Support.hpp"

#include "app/Keymap.hpp"
#include "app/TextTool.hpp"

namespace np {

// The Text tool must own the keyboard while a session is live -- before this
// fix, main.cpp's key-down handler resolved EVERY key through
// `Keymap::resolve()` with no check on a live text session, so typing "f"
// into a Text layer also fired `mirror_x`, Backspace deleted the canvas
// selection instead of a character, and Space toggled pause mid-sentence.
// Three things, none of which need a window, a font or a platform shaper:
//
//   1. `keyChordReachesKeymap()` (app/Keymap.hpp) -- the gate itself. A bare
//      or Shift/Alt-only chord must NOT reach the keymap while a session is
//      active; a Cmd/Ctrl chord must, so Cmd+Z/Cmd+S keep working while
//      typing a caption; and with no session live every chord must reach it
//      exactly as it always has.
//   2. `textEditRevert()` (app/TextTool.cpp) -- Escape's undo-the-session
//      path restores `TextContent::utf8` byte-for-byte and leaves the caret
//      on a real boundary, while plain `textEditCancel()` (every OTHER way a
//      session ends) must leave the content untouched.
//   3. `textSessionActive()` -- the predicate main.cpp's key-down handler
//      actually gates on -- transitions true on `textEditBegin()` AND on
//      `textEditFrameDragBegin()` ("frame-drag counts", app/TextTool.hpp),
//      and false on `textEditCancel()`, which is the only place either
//      `active` writer resets it.
bool runTextKeyCaptureTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- 1. the gate table ---------------------------------------------------
  {
    // Bare or Shift/Alt-only chords straight off keymaps/default.json's own
    // bindings: Space -> toggle_pause, F -> mirror_x, Backspace ->
    // delete_selection, Return (Flats-scoped, but the gate cares only about
    // modifiers) -- every one of them must be blocked while a session is
    // active, because the character queue and the arrow/Backspace/Delete/
    // Return handling in ui/MacPaintUI.cpp's text block are what own these
    // keys during a session.
    const KeyChord space{SDLK_SPACE, 0};
    const KeyChord bareF{SDLK_F, 0};
    const KeyChord shiftF{SDLK_F, kModShift};
    const KeyChord backspace{SDLK_BACKSPACE, 0};
    const KeyChord ret{SDLK_RETURN, 0};
    check(!keyChordReachesKeymap(space, /*textSessionActive=*/true),
          "keyChordReachesKeymap(Space, active): REQUIRED -- blocked, belongs to the session");
    check(!keyChordReachesKeymap(bareF, /*textSessionActive=*/true),
          "keyChordReachesKeymap(F, active): REQUIRED -- blocked, mirror_x must not fire");
    check(!keyChordReachesKeymap(shiftF, /*textSessionActive=*/true),
          "keyChordReachesKeymap(Shift+F, active): REQUIRED -- Shift alone is not Cmd/Ctrl");
    check(!keyChordReachesKeymap(backspace, /*textSessionActive=*/true),
          "keyChordReachesKeymap(Backspace, active): REQUIRED -- must delete a character, not "
          "the canvas selection");
    check(!keyChordReachesKeymap(ret, /*textSessionActive=*/true),
          "keyChordReachesKeymap(Return, active): REQUIRED -- newline, not a Flats action");

    // Cmd and Ctrl chords still reach the keymap -- undo/redo/save must keep
    // working while a caption is being typed.
    const KeyChord cmdZ{SDLK_Z, kModCmd};
    const KeyChord cmdS{SDLK_S, kModCmd};
    const KeyChord ctrlZ{SDLK_Z, kModCtrl};
    check(keyChordReachesKeymap(cmdZ, /*textSessionActive=*/true),
          "keyChordReachesKeymap(Cmd+Z, active): REQUIRED -- undo must still reach the keymap");
    check(keyChordReachesKeymap(cmdS, /*textSessionActive=*/true),
          "keyChordReachesKeymap(Cmd+S, active): REQUIRED -- save must still reach the keymap");
    check(keyChordReachesKeymap(ctrlZ, /*textSessionActive=*/true),
          "keyChordReachesKeymap(Ctrl+Z, active): REQUIRED -- Ctrl is the other app modifier");

    // With no session live, every one of the above reaches the keymap
    // exactly as it always has -- this gate must not change behaviour
    // outside a text session.
    check(keyChordReachesKeymap(space, /*textSessionActive=*/false) &&
              keyChordReachesKeymap(bareF, /*textSessionActive=*/false) &&
              keyChordReachesKeymap(shiftF, /*textSessionActive=*/false) &&
              keyChordReachesKeymap(backspace, /*textSessionActive=*/false) &&
              keyChordReachesKeymap(ret, /*textSessionActive=*/false) &&
              keyChordReachesKeymap(cmdZ, /*textSessionActive=*/false) &&
              keyChordReachesKeymap(cmdS, /*textSessionActive=*/false),
          "keyChordReachesKeymap(*, inactive): REQUIRED -- every chord passes with no session");
  }

  // --- 2. textSessionActive() transitions -----------------------------------
  {
    TextEditState st;
    check(!textSessionActive(st), "textSessionActive(): a default-constructed state is inactive");

    TextContent content;
    content.utf8 = "hello";
    textEditBegin(&st, /*documentId=*/1, /*layerIndex=*/0, content);
    check(textSessionActive(st),
          "textSessionActive(): REQUIRED -- true after textEditBegin()");
    textEditCancel(&st);
    check(!textSessionActive(st),
          "textSessionActive(): REQUIRED -- false after textEditCancel() ends the session");

    // "frame-drag counts" (app/TextTool.hpp) -- a candidate drag with no
    // layer yet is still a live session for the keymap gate's purposes.
    textEditFrameDragBegin(&st, PathPoint{5, 5}, /*documentId=*/1);
    check(textSessionActive(st),
          "textSessionActive(): REQUIRED -- true after textEditFrameDragBegin() too");
    textEditCancel(&st);
    check(!textSessionActive(st),
          "textSessionActive(): REQUIRED -- false again after cancelling the drag");
  }

  // --- 3. textEditRevert() vs plain textEditCancel() ------------------------
  {
    TextEditState st;
    TextContent content;
    const std::string eAcute = "\xC3\xA9";  // "e" with an acute accent, 2 UTF-8 bytes
    content.utf8 = "caf" + eAcute;  // multi-byte tail -- a sloppy revert would corrupt it
    textEditBegin(&st, /*documentId=*/1, /*layerIndex=*/0, content);
    const size_t caretAtBegin = st.caret;

    // Simulate a burst of typing after the session opened.
    textInsertUtf8(&content, &st, "!!!");
    check(content.utf8 == "caf" + eAcute + "!!!",
          "(setup) the simulated typing burst landed in the content");

    textEditRevert(&content, &st);
    check(content.utf8 == "caf" + eAcute,
          "textEditRevert(): REQUIRED -- content restored byte-for-byte to the pre-session "
          "snapshot");
    check(st.caret == caretAtBegin,
          "textEditRevert(): REQUIRED -- caret restored to where the session began");
    check(st.caret <= content.utf8.size(),
          "textEditRevert(): the restored caret is a valid, in-bounds offset");

    // Reverting again (nothing new was typed) is a documented no-op on the
    // content -- callers do not need to guard on undoOpened before calling
    // it.
    textEditRevert(&content, &st);
    check(content.utf8 == "caf" + eAcute,
          "textEditRevert(): calling it again with no new edits changes nothing further");

    // Plain textEditCancel() -- every OTHER way a session ends (document
    // switch, the layer disappearing) -- must NOT revert: it does not even
    // take a TextContent* to revert with.
    TextEditState st2;
    TextContent content2;
    content2.utf8 = "original";
    textEditBegin(&st2, /*documentId=*/1, /*layerIndex=*/0, content2);
    textInsertUtf8(&content2, &st2, " typed");
    check(content2.utf8 == "original typed", "(setup) the second session's typing landed too");
    textEditCancel(&st2);
    check(content2.utf8 == "original typed",
          "textEditCancel(): REQUIRED -- cancel-on-doc-switch/layer-gone does NOT revert; the "
          "typed text survives exactly as ui/MacPaintUI.cpp's document-switch and layer-gone "
          "call sites rely on");
  }

  std::printf("[selftest] text key capture %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
