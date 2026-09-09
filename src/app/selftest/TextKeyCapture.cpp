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

  // --- 4. textInputAction() -- the platform hand-off ------------------------
  //
  // The half that made the whole feature inert: `ui/MacPaintUI.cpp`'s typing
  // loop reads `io.InputQueueCharacters`, ImGui fills that from
  // `SDL_EVENT_TEXT_INPUT` and nothing else, and SDL generates that event only
  // while text input has been STARTED for the window. Nothing in this
  // application called `SDL_StartTextInput()`; ImGui's backend calls it only
  // for its own `InputText()` widgets, and a Text-tool caret session is not
  // one. Measured on the running app before this existed: `sdlTextInput=0`
  // and `chars=0` on every frame of a live session -- the gate in section 1
  // was correctly keeping bare keys AWAY from the keymap, and there was
  // nothing on the other side to receive them.
  //
  // Pure and headless, which is the point of it being a function rather than
  // four lines inline in the frame loop (app/TextTool.hpp section 7).
  {
    using A = TextInputAction;
    // A session opens while the platform is off: start, every time.
    check(textInputAction(/*sessionActive=*/true, /*platformActive=*/false,
                          /*imguiWantsText=*/false, /*startedHere=*/false) == A::Start,
          "textInputAction(): REQUIRED -- a live session with text input off must START it");
    // Already on because we turned it on: nothing to do, every frame, forever.
    check(textInputAction(true, true, false, true) == A::Leave,
          "textInputAction(): a live session with text input already on is left alone");
    // **The re-start.** ImGui's backend stops text input when one of its own
    // widgets loses focus, and its UpdateIme() will never restart it for a
    // caret it knows nothing about. Asking SDL every frame is what repairs
    // that; an edge-triggered version would leave the caret mute for the rest
    // of the session.
    check(textInputAction(/*sessionActive=*/true, /*platformActive=*/false,
                          /*imguiWantsText=*/false, /*startedHere=*/true) == A::Start,
          "textInputAction(): REQUIRED -- a live session must RE-start after ImGui's backend "
          "stopped text input behind our back");
    // Session over, we own it, nobody else wants it: stop, so a CJK input
    // method does not keep swallowing bare hotkeys.
    check(textInputAction(/*sessionActive=*/false, /*platformActive=*/true,
                          /*imguiWantsText=*/false, /*startedHere=*/true) == A::Stop,
          "textInputAction(): REQUIRED -- the session ending stops the text input we started");
    // Session over, but ImGui started it: NOT ours to stop. Stopping it here
    // leaves ImGui_ImplSDL3_UpdateIme()'s `ImeWindow == window` early-return
    // convinced text input is still on, and it never restarts it -- a
    // permanently dead layer-rename box.
    check(textInputAction(/*sessionActive=*/false, /*platformActive=*/true,
                          /*imguiWantsText=*/false, /*startedHere=*/false) == A::Leave,
          "textInputAction(): REQUIRED -- text input ImGui started is never stopped by us");
    // Session over, and a text widget took focus in the same frame: leave it
    // on, it is being used.
    check(textInputAction(/*sessionActive=*/false, /*platformActive=*/true,
                          /*imguiWantsText=*/true, /*startedHere=*/true) == A::Leave,
          "textInputAction(): REQUIRED -- an ImGui widget wanting text input keeps it on");
    // Nothing live, nothing on: no call at all.
    check(textInputAction(false, false, false, false) == A::Leave,
          "textInputAction(): idle with text input off asks SDL for nothing");
    check(textInputAction(false, false, false, true) == A::Leave,
          "textInputAction(): a stop already performed is not repeated");
    // The session wins over an ImGui widget's flag: they cannot both be
    // typing, and the session's answer is the one that needs the platform on.
    check(textInputAction(/*sessionActive=*/true, /*platformActive=*/false,
                          /*imguiWantsText=*/true, /*startedHere=*/false) == A::Start,
          "textInputAction(): a live session starts text input regardless of io.WantTextInput");
  }

  std::printf("[selftest] text key capture %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
