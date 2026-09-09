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
//   2. Ending a session KEEPS what was typed (app/TextTool.cpp) --
//      `textEditCancel()` is every exit there is now, Escape included, and
//      none of them may touch `TextContent::utf8`. Escape used to be the
//      exception via a `textEditRevert()` that has since been removed with
//      its caller: it restored the block to what the session opened with,
//      which for a block the same click had just created was the empty
//      string.
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

  // --- 3. ending a session KEEPS the typed text -----------------------------
  //
  // Every way out of a session is an accept, Escape included. Escape used to
  // be the exception -- it called a `textEditRevert()` that restored the
  // block to what `textEditBegin()` opened it with -- and that is gone, both
  // the call and the function, because of how a block is made here: a click
  // on empty canvas creates the layer AND opens the session in one gesture,
  // so the state it reverted to was the empty string. The most reflexive key
  // on the keyboard silently destroying a caption is not a defensible
  // default. Undo is the discard now (section 5 keeps `undo` on the KEEP
  // list precisely so it can be).
  //
  // **What these two checks can and cannot prove, stated because a green
  // assertion that cannot fail is worse than no assertion.** `textEditCancel()`
  // takes no `TextContent*`. It therefore CANNOT erase a caption however it
  // is written, and sabotaging it does not move these lines -- measured, not
  // assumed. What they are is a statement of the design in a place that has
  // to be edited if the design is reversed: reintroducing a revert means
  // giving cancel access to the content, which means changing its signature,
  // which lands here. The behavioural guard is one level up, on the Escape
  // key in `ui/MacPaintUI.cpp`, and it is checked by driving the running
  // application -- there is no unit-test seam for a key press.
  {
    TextEditState st;
    TextContent content;
    const std::string eAcute = "\xC3\xA9";  // multi-byte tail: a sloppy edit would corrupt it
    content.utf8 = "caf" + eAcute;
    textEditBegin(&st, /*documentId=*/1, /*layerIndex=*/0, content);
    textInsertUtf8(&content, &st, "!!!");
    check(content.utf8 == "caf" + eAcute + "!!!",
          "(setup) the simulated typing burst landed in the content");

    textEditCancel(&st);
    check(content.utf8 == "caf" + eAcute + "!!!",
          "textEditCancel(): REQUIRED -- ending a session KEEPS every character typed. This is "
          "the assertion that fails if a revert-on-exit is ever reintroduced");
    check(!textSessionActive(st), "textEditCancel(): and the session is over");

    // The case that made the old behaviour indefensible: a session opened on
    // a block that was empty because the same gesture had just created it.
    // Reverting to THAT snapshot threw the whole caption away.
    TextEditState fresh;
    TextContent made;  // as makeTextContent() leaves it: no text yet
    textEditBegin(&fresh, /*documentId=*/1, /*layerIndex=*/0, made);
    textInsertUtf8(&made, &fresh, "a caption nobody wants to lose");
    textEditCancel(&fresh);
    check(made.utf8 == "a caption nobody wants to lose",
          "REQUIRED -- a session opened on a NEWLY created (empty) block keeps its text when it "
          "ends; this is the exact case where Escape used to erase everything typed");
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

  // --- 5. keymapActionEndsTextSession() -- which hotkeys put the caret away --
  //
  // The chord already passed `keyChordReachesKeymap()` (section 1) and
  // resolved to an action; this is the question that comes after it. A KEEP
  // list with everything else ending -- app/TextTool.hpp section 8 -- so the
  // assertions that matter most are the two ends of that default.
  {
    // The view. Framing a caption while typing it is a real gesture and none
    // of these can move a byte of the document.
    check(!keymapActionEndsTextSession("zoom_in") &&
              !keymapActionEndsTextSession("zoom_out") &&
              !keymapActionEndsTextSession("zoom_100") &&
              !keymapActionEndsTextSession("fit_window") &&
              !keymapActionEndsTextSession("reset_view") &&
              !keymapActionEndsTextSession("mirror_x") &&
              !keymapActionEndsTextSession("mirror_y") &&
              !keymapActionEndsTextSession("reset_rotation") &&
              !keymapActionEndsTextSession("toggle_grayscale") &&
              !keymapActionEndsTextSession("toggle_guides") &&
              !keymapActionEndsTextSession("toggle_snapping") &&
              !keymapActionEndsTextSession("toggle_grid"),
          "keymapActionEndsTextSession(): REQUIRED -- every view command keeps the session");

    // **Undo and redo keep it, and this is the assertion that says so.** A
    // typing burst is a history entry, so Cmd+Z during a session is the user
    // undoing their own typing; answering it by putting the caret away would
    // make the burst un-undoable without first clicking back into the block.
    check(!keymapActionEndsTextSession("undo"),
          "keymapActionEndsTextSession(): REQUIRED -- undo keeps the session; a typing burst is "
          "the entry it undoes");
    check(!keymapActionEndsTextSession("redo"),
          "keymapActionEndsTextSession(): REQUIRED -- redo keeps the session too");

    // Tool and application state, not document state.
    check(!keymapActionEndsTextSession("size_up") && !keymapActionEndsTextSession("size_down") &&
              !keymapActionEndsTextSession("reload_shaders") &&
              !keymapActionEndsTextSession("screenshot") &&
              !keymapActionEndsTextSession("toggle_pause"),
          "keymapActionEndsTextSession(): brush size / reload / screenshot / pause keep it");

    // Everything that can move the document or the selection under a live
    // caret. `free_transform` is the one this began with -- Cmd+T used to
    // drop a gizmo on top of a live caption and leave both claiming Return.
    check(keymapActionEndsTextSession("free_transform"),
          "keymapActionEndsTextSession(): REQUIRED -- Cmd+T ends the session rather than putting "
          "a gizmo over a live caret");
    check(keymapActionEndsTextSession("clear_canvas"),
          "keymapActionEndsTextSession(): REQUIRED -- clearing the canvas ends the session");
    check(keymapActionEndsTextSession("adjust_levels") &&
              keymapActionEndsTextSession("adjust_curves") &&
              keymapActionEndsTextSession("adjust_invert") &&
              keymapActionEndsTextSession("adjust_auto_tone") &&
              keymapActionEndsTextSession("adjust_black_and_white"),
          "keymapActionEndsTextSession(): REQUIRED -- the adjustment commands end the session");
    check(keymapActionEndsTextSession("select_all") && keymapActionEndsTextSession("deselect") &&
              keymapActionEndsTextSession("reselect") &&
              keymapActionEndsTextSession("invert_selection"),
          "keymapActionEndsTextSession(): REQUIRED -- the selection commands end the session");
    check(keymapActionEndsTextSession("copy") && keymapActionEndsTextSession("copy_merged") &&
              keymapActionEndsTextSession("cut") && keymapActionEndsTextSession("paste"),
          "keymapActionEndsTextSession(): REQUIRED -- the clipboard four end it TOGETHER; copy "
          "writes nothing but acts on the canvas selection, and splitting the family would be a "
          "worse rule than keeping it");
    check(keymapActionEndsTextSession("delete_selection") && keymapActionEndsTextSession("quit") &&
              keymapActionEndsTextSession("flats_delete_fill"),
          "keymapActionEndsTextSession(): delete / quit / the flats commands end it");

    // **The default, which is the whole design.** A binding added to
    // keymaps/default.json a year from now gets the safe answer without
    // anyone remembering this file exists -- and this is the assertion that
    // fails if the list is ever inverted into a list of ENDERS.
    check(keymapActionEndsTextSession("an_action_nobody_has_written_yet"),
          "keymapActionEndsTextSession(): REQUIRED -- an unknown action ENDS the session; the "
          "list is a KEEP list and the default is the safe one");
    check(keymapActionEndsTextSession(""),
          "keymapActionEndsTextSession(): the empty action ends it too, same default");
  }

  // --- 6. textEditResyncAfterHistoryMove() ---------------------------------
  //
  // Undo/redo keep the session (section 5) and `core/History` replaces the
  // whole Document, so the block is a different `TextContent` afterwards
  // while `TextEditState` still holds the caret and bookkeeping from before
  // the move. Both hazards are silent; both are asserted here.
  {
    // (a) A caret past the end of the restored string.
    TextEditState st;
    TextContent content;
    content.utf8 = "Handgloves";
    textEditBegin(&st, /*documentId=*/1, /*layerIndex=*/0, content);
    textInsertUtf8(&content, &st, " and mittens");
    textEditMarkUndoOpened(&st);
    check(st.caret == content.utf8.size() && st.undoOpened,
          "(setup) the burst moved the caret to the end and opened an undo entry");

    // What undo restores: the pre-burst string, which is SHORTER than the
    // caret's current offset.
    TextContent restored;
    restored.utf8 = "Handgloves";
    textEditResyncAfterHistoryMove(&st, restored);
    check(st.caret == restored.utf8.size(),
          "textEditResyncAfterHistoryMove(): REQUIRED -- a caret past the end of the restored "
          "string is clamped to it, not left addressing bytes that are gone");
    check(!st.undoOpened,
          "textEditResyncAfterHistoryMove(): REQUIRED -- undoOpened is cleared, so the next "
          "keystroke RECORDS a new entry instead of amending over the state just undone to");
    check(textSessionActive(st),
          "textEditResyncAfterHistoryMove(): the session itself survives -- that is the point");

    // (b) A caret left INSIDE a multi-byte sequence. Section 3's boundary
    // invariant is this file's to hold, and a history move is the one way the
    // content changes without going through any of its own edit functions.
    TextEditState st2;
    TextContent wide;
    const std::string eAcute = "\xC3\xA9";  // 2 UTF-8 bytes
    wide.utf8 = "caf" + eAcute + "s";
    textEditBegin(&st2, /*documentId=*/1, /*layerIndex=*/0, wide);
    textCaretSetOffset(&st2, wide, 3);
    // A restored string whose byte 4 is a CONTINUATION byte -- offset 4 is
    // mid-sequence, which is exactly the state a naive size-only clamp would
    // leave behind and call fixed.
    TextContent restored2;
    restored2.utf8 = "ca" + eAcute + "fes";
    st2.caret = 3;  // deliberately mid-sequence against restored2
    textEditResyncAfterHistoryMove(&st2, restored2);
    check(st2.caret == 2,
          "textEditResyncAfterHistoryMove(): REQUIRED -- a caret left mid-sequence snaps DOWN to "
          "the boundary before it; a clamp that only checked the LENGTH would pass this offset "
          "through unchanged and corrupt the next insert");

    // (c) The session KEEPS RUNNING on the same layer -- the resync puts a
    // session back in step, it does not end one, and the layer it names must
    // still be the layer it named. (This used to also assert that a
    // pre-session `snapshotUtf8` survived the move; those fields went away
    // with `textEditRevert()` -- see section 3.)
    TextEditState st3;
    TextContent orig;
    orig.utf8 = "before";
    textEditBegin(&st3, /*documentId=*/7, /*layerIndex=*/3, orig);
    TextContent restored3;
    restored3.utf8 = "something else entirely";
    textEditResyncAfterHistoryMove(&st3, restored3);
    check(textSessionActive(st3) && st3.documentId == 7 && st3.layerIndex == 3,
          "textEditResyncAfterHistoryMove(): REQUIRED -- the session survives the move still "
          "naming the same document and layer; it is a resync, not an exit");

    // (d) A no-op with no session live: undo/redo happen far more often
    // outside a session than in one, and the caller (moveHistoryCursor())
    // must not have to guard.
    TextEditState st4;
    st4.caret = 999;
    TextContent tiny;
    tiny.utf8 = "x";
    textEditResyncAfterHistoryMove(&st4, tiny);
    check(st4.caret == 999,
          "textEditResyncAfterHistoryMove(): REQUIRED -- a no-op when no session is live; it "
          "touches nothing rather than clamping a caret that names no session");
  }

  std::printf("[selftest] text key capture %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
