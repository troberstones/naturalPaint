#include "app/selftest/Support.hpp"

// Support.hpp no longer drags app/AppState.hpp in; this section names Tool.
#include "app/AppState.hpp"

#include "app/TextTool.hpp"

namespace np {

namespace {

// A local UTF-8 validator, deliberately independent of anything in
// app/TextTool.cpp -- the brief's own instruction: trusting the operation
// under test to also grade itself is exactly how a broken backspace could
// look "valid" to its own author. Rejects overlong 2-byte encodings
// (0xC0/0xC1) and lead bytes above the RFC 3629 range (0xF5+), which a
// looser check would wave through.
bool isValidUtf8(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len;
    if ((c & 0x80) == 0x00) {
      len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      if (c < 0xC2) return false;  // overlong
      len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      if (c > 0xF4) return false;  // above the assigned range
      len = 4;
    } else {
      return false;
    }
    if (i + len > s.size()) return false;
    for (size_t k = 1; k < len; ++k) {
      const unsigned char cc = static_cast<unsigned char>(s[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
    }
    i += len;
  }
  return true;
}

}  // namespace

// app/TextTool -- the headless core of PLAN.md phase 14's Text tool: the
// gate predicate, the caret-editing session's UTF-8-safe string edits, the
// paragraph-frame drag, and block hit-testing.
//
// Headless and GPU-free. Writes no files. Touches no `ui/` file, does not
// exercise `AppState`, and calls none of `core/TextContent.hpp`'s own free
// functions (`textContentToShapes()` etc. are a sibling track's `.cpp`, not
// yet linkable) -- every `TextContent` below is built by field assignment,
// the same way app/selftest/PenTool.cpp builds `VectorShape`s directly
// rather than through a document.
bool runTextToolTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-72s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto ptNear = [](PathPoint a, PathPoint b, float tol = 1e-4f) {
    return std::fabs(a.x - b.x) <= tol && std::fabs(a.y - b.y) <= tol;
  };

  // =======================================================================
  // 1. toolEditsText() -- true for exactly Tool::Text
  // =======================================================================
  {
    bool exactlyText = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      if (toolEditsText(t) != (t == Tool::Text)) exactlyText = false;
    }
    check(exactlyText,
          "toolEditsText(): true for exactly Tool::Text and false for every other Tool");
    // Named explicitly, the way app/selftest/PenTool.cpp's own section 7
    // names Shape against toolEditsPath(): Pen, Curve and Shape all produce
    // or edit vector geometry, which is exactly why they are the tools most
    // likely to be confused for this one.
    check(!toolEditsText(Tool::Pen) && !toolEditsText(Tool::Curve) && !toolEditsText(Tool::Shape),
          "toolEditsText(): Pen, Curve and Shape are NOT text tools, named explicitly since "
          "they are the three easiest to mistake for one");
  }

  // =======================================================================
  // 2. UTF-8 boundary safety -- the heart of this file
  // =======================================================================
  //
  // "A" (1 byte) + "e-acute" (2 bytes) + "euro sign" (3 bytes) + an emoji
  // (4 bytes) + "Z" (1 byte), so every lead-byte length this file has to
  // classify appears exactly once, in a known order, at known offsets:
  // character boundaries are byte 0, 1, 3, 6, 10, 11(end).
  {
    const std::string aChar = "A";
    const std::string eAcute = "\xC3\xA9";          // 2 bytes
    const std::string euro = "\xE2\x82\xAC";        // 3 bytes
    const std::string emoji = "\xF0\x9F\x98\x80";   // 4 bytes (an emoji)
    const std::string zChar = "Z";
    const std::string built = aChar + eAcute + euro + emoji + zChar;
    check(built.size() == 11, "(setup) the built string is 1+2+3+4+1 = 11 bytes");
    check(isValidUtf8(built), "(setup) the local validator accepts the string this file built");

    const std::vector<size_t> boundaries = {0, 1, 3, 6, 10, 11};  // computed from the literals

    // --- 2a. backspace from the end, one press at a time, to empty -------
    {
      TextContent text;
      text.utf8 = built;
      TextEditState st;
      st.caret = text.utf8.size();
      st.anchor = st.caret;  // no selection: every production writer keeps these in step

      const std::vector<size_t> expectedLenAfter = {10, 6, 3, 1, 0};
      bool allValidThroughout = true;
      bool allCaretAtEnd = true;
      bool allPressesSucceeded = true;
      for (size_t step = 0; step < expectedLenAfter.size(); ++step) {
        const bool did = textBackspace(&text, &st);
        if (!did) allPressesSucceeded = false;
        if (!isValidUtf8(text.utf8)) allValidThroughout = false;
        if (st.caret != text.utf8.size()) allCaretAtEnd = false;
        if (text.utf8.size() != expectedLenAfter[step]) allValidThroughout = false;
      }
      check(allPressesSucceeded, "textBackspace(): REQUIRED -- every one of the 5 presses "
                                 "reports it deleted something");
      check(allValidThroughout,
            "textBackspace(): REQUIRED -- after EVERY press the string is still valid UTF-8 "
            "and shrank by exactly one character's bytes (10, 6, 3, 1, 0)");
      check(allCaretAtEnd,
            "textBackspace(): REQUIRED -- after every press the caret sits at the new end");
      check(!textBackspace(&text, &st) && text.utf8.empty(),
            "textBackspace(): on an empty string returns false and edits nothing");
    }

    // --- 2b. caret-left from the end to the start, one boundary at a time -
    {
      TextContent text;
      text.utf8 = built;
      TextEditState st;
      st.caret = text.utf8.size();
      st.anchor = st.caret;  // no selection: every production writer keeps these in step

      std::vector<size_t> visited = {st.caret};
      for (int i = 0; i < 5; ++i) {
        textCaretLeft(text, &st, /*extend=*/false);
        visited.push_back(st.caret);
      }
      const std::vector<size_t> expected = {11, 10, 6, 3, 1, 0};
      check(visited == expected,
            "textCaretLeft(): REQUIRED -- from the end, visits exactly the character "
            "boundaries in order (11, 10, 6, 3, 1, 0), computed from the string built above");
      textCaretLeft(text, &st, /*extend=*/false);
      check(st.caret == 0, "textCaretLeft(): already at 0 stays at 0 rather than underflowing");
    }

    // --- 2c. caret-right from the start to the end -------------------------
    {
      TextContent text;
      text.utf8 = built;
      TextEditState st;
      st.caret = 0;
      st.anchor = st.caret;  // no selection: every production writer keeps these in step

      std::vector<size_t> visited = {st.caret};
      for (int i = 0; i < 5; ++i) {
        textCaretRight(text, &st, /*extend=*/false);
        visited.push_back(st.caret);
      }
      check(visited == boundaries,
            "textCaretRight(): REQUIRED -- from the start, visits exactly the character "
            "boundaries in order (0, 1, 3, 6, 10, 11)");
      textCaretRight(text, &st, /*extend=*/false);
      check(st.caret == text.utf8.size(),
            "textCaretRight(): already at the end stays at the end rather than overflowing");
    }

    // --- 2d. caretHome / caretEnd -------------------------------------------
    {
      TextContent text;
      text.utf8 = built;
      TextEditState st;
      st.caret = 5;  // an arbitrary interior position, not a boundary claim
      st.anchor = st.caret;  // no selection: every production writer keeps these in step
      textCaretHome(&st, /*extend=*/false);
      check(st.caret == 0, "textCaretHome(): caret goes to 0");
      textCaretEnd(text, &st, /*extend=*/false);
      check(st.caret == text.utf8.size(), "textCaretEnd(): caret goes to the byte length");
    }

    // --- 2e. insert a multi-byte character in the MIDDLE --------------------
    {
      TextContent text;
      text.utf8 = built;
      TextEditState st;
      st.caret = 3;  // boundary between e-acute and euro -- neither end
      st.anchor = st.caret;  // no selection: every production writer keeps these in step

      textInsertUtf8(&text, &st, std::string_view(emoji));
      const std::string expectedBytes = aChar + eAcute + emoji + euro + emoji + zChar;
      check(text.utf8 == expectedBytes,
            "textInsertUtf8(): REQUIRED -- inserting a 4-byte character mid-string produces "
            "exactly the expected bytes at exactly that offset");
      check(st.caret == 3 + emoji.size(),
            "textInsertUtf8(): REQUIRED -- the caret advances past what was inserted (3 + 4 "
            "= 7), landing after the new character rather than before it");
      check(isValidUtf8(text.utf8), "textInsertUtf8(): the result is still valid UTF-8");
    }

    // --- 2f. forward-delete a multi-byte character --------------------------
    {
      TextContent text;
      text.utf8 = built;
      TextEditState st;
      st.caret = 3;  // immediately before the 3-byte euro sign
      st.anchor = st.caret;  // no selection: every production writer keeps these in step

      const bool did = textDeleteForward(&text, &st);
      check(did, "textDeleteForward(): reports it deleted something");
      const std::string expectedBytes = aChar + eAcute + emoji + zChar;
      check(text.utf8 == expectedBytes,
            "textDeleteForward(): REQUIRED -- removes exactly the 3-byte character after the "
            "caret, not one byte and not the 4-byte one after it");
      check(st.caret == 3,
            "textDeleteForward(): the caret does not move -- there was nothing before it to "
            "shift");
      check(isValidUtf8(text.utf8), "textDeleteForward(): the result is still valid UTF-8");
    }
    {
      // Forward-delete at the end does nothing.
      TextContent text;
      text.utf8 = built;
      TextEditState st;
      st.caret = text.utf8.size();
      st.anchor = st.caret;  // no selection: every production writer keeps these in step
      check(!textDeleteForward(&text, &st) && text.utf8 == built,
            "textDeleteForward(): at the end returns false and edits nothing");
    }

    // --- 2g. a bogus caret is clamped, and the NEXT operation is correct ---
    {
      // Mid-sequence: byte 4 sits inside the 3-byte euro sign (3..6). The
      // contract (header section 3) is CLAMP then MOVE: 4 clamps down to the
      // boundary at 3 (the start of the character byte 4 falls inside), and
      // moving left from 3 lands on the boundary before it, 1.
      TextContent text;
      text.utf8 = built;
      TextEditState st;
      st.caret = 4;
      st.anchor = st.caret;  // no selection: every production writer keeps these in step
      textCaretLeft(text, &st, /*extend=*/false);
      check(st.caret == 1,
            "textCaretLeft(): REQUIRED -- a caret sitting mid-sequence (byte 4, inside the "
            "3-byte euro sign at 3..6) is clamped to boundary 3 and THEN moved left, landing "
            "on 1 -- not 3 (clamp with no move) and not 0 (moving from the raw, unclamped 4)");
    }
    {
      TextContent text;
      text.utf8 = built;
      TextEditState st;
      st.caret = 4;  // mid-sequence, inside the euro sign
      st.anchor = st.caret;  // no selection: every production writer keeps these in step
      const bool did = textBackspace(&text, &st);
      check(did, "textBackspace(): from a mid-sequence caret still deletes something");
      check(isValidUtf8(text.utf8),
            "textBackspace(): REQUIRED -- starting from a bogus mid-sequence caret (byte 4 of "
            "10, inside the euro sign), the clamp resolves it to boundary 3 first, so the "
            "delete removes the character ENDING at 3 (e-acute) and the result is still valid "
            "UTF-8");
      check(text.utf8 == aChar + euro + emoji + zChar,
            "textBackspace(): REQUIRED -- clamping byte 4 down to boundary 3 and deleting the "
            "character before it removes e-acute, leaving A + euro + emoji + Z");
      check(st.caret == 1, "textBackspace(): the caret lands at 1, the start of the deleted "
                           "character -- a real boundary");

      // The NEXT operation, on the now-clamped state, is still correct.
      textCaretRight(text, &st, /*extend=*/false);
      check(st.caret == 4,
            "textBackspace(): the next operation after a bogus-caret recovery is correct -- "
            "caret-right from 1 lands on 4, the start of the euro sign in the shortened string");
    }
    {
      // Past the end.
      TextContent text;
      text.utf8 = built;
      TextEditState st;
      st.caret = 9999;
      st.anchor = st.caret;  // no selection: every production writer keeps these in step
      const bool did = textBackspace(&text, &st);
      check(did && text.utf8 == aChar + eAcute + euro + emoji,
            "textBackspace(): REQUIRED -- a caret far past the end is clamped to the real end "
            "(11) before deleting, so it removes the LAST character (Z), not nothing and not "
            "undefined behaviour");
      check(st.caret == 10, "textBackspace(): caret lands at 10, the real end after the delete");
    }
  }

  // =======================================================================
  // 3. textBlockHit() -- inside the box, and the pad grows every side
  // =======================================================================
  {
    const PathBounds b{true, 10.0f, 10.0f, 20.0f, 30.0f};
    check(textBlockHit(b, PathPoint{15, 15}, 0.0f), "textBlockHit(): a point inside hits");
    check(!textBlockHit(b, PathPoint{5, 15}, 0.0f), "textBlockHit(): a point outside misses");
    check(textBlockHit(b, PathPoint{10, 10}, 0.0f), "textBlockHit(): inclusive of the box's own "
                                                    "edges and corners");
    check(!textBlockHit(b, PathPoint{9, 15}, 0.0f),
          "textBlockHit(): just outside the left edge misses with no padding");
    check(textBlockHit(b, PathPoint{9, 15}, 2.0f),
          "textBlockHit(): the same point hits once padDoc grows the box past it");
    check(!textBlockHit(b, PathPoint{7, 15}, 2.0f),
          "textBlockHit(): a point still outside the padded box misses");
    const PathBounds invalid{false, 0, 0, 0, 0};
    check(!textBlockHit(invalid, PathPoint{0, 0}, 1000.0f),
          "textBlockHit(): an invalid (empty text) bounds never hits, at any pad");
  }

  // =======================================================================
  // 4. The frame drag -- top-left regardless of direction, and the minimum
  //    size that tells a click from a paragraph frame
  // =======================================================================
  {
    struct Direction {
      const char* name;
      PathPoint start, end;
    };
    const Direction dirs[4] = {
        {"down-right", {10, 10}, {50, 40}},
        {"down-left", {50, 10}, {10, 40}},
        {"up-right", {10, 40}, {50, 10}},
        {"up-left", {50, 40}, {10, 10}},
    };
    for (const Direction& d : dirs) {
      TextEditState st;
      textEditFrameDragBegin(&st, d.start, /*documentId=*/1);
      textEditFrameDragUpdate(&st, d.end);
      TextContent out;
      const bool madeFrame = textEditFrameDragEnd(&st, &out, /*minSizeDoc=*/4.0f);
      check(madeFrame, (std::string("textEditFrameDragEnd(): a ") + d.name +
                        " drag past the minimum produces a frame")
                           .c_str());
      check(ptNear(out.origin, PathPoint{10, 10}),
            (std::string("textEditFrameDragEnd(): REQUIRED -- the ") + d.name +
             " drag's origin is the TOP-LEFT corner (10,10) regardless of drag direction")
                .c_str());
      check(std::fabs(out.frame.width - 40.0f) < 1e-4f && std::fabs(out.frame.height - 30.0f) < 1e-4f,
            (std::string("textEditFrameDragEnd(): the ") + d.name +
             " drag's width/height match the drag's extent (40x30)")
                .c_str());
      check(!st.frameDragActive,
            (std::string("textEditFrameDragEnd(): the ") + d.name + " drag is no longer active")
                .c_str());
    }

    // Too small -> false, and nothing is written.
    {
      TextEditState st;
      textEditFrameDragBegin(&st, PathPoint{0, 0}, 1);
      textEditFrameDragUpdate(&st, PathPoint{2, 2});
      TextContent out;
      out.utf8 = "unchanged sentinel";
      const bool madeFrame = textEditFrameDragEnd(&st, &out, /*minSizeDoc=*/4.0f);
      check(!madeFrame,
            "textEditFrameDragEnd(): REQUIRED -- a drag smaller than minSizeDoc in both "
            "dimensions returns false -- a click means point text, not a zero-width paragraph");
      check(out.utf8 == "unchanged sentinel" && out.frame.width == 0.0f && out.frame.height == 0.0f,
            "textEditFrameDragEnd(): REQUIRED -- on failure, `*out` is left completely "
            "untouched, including fields this function never writes on success either");
      check(!st.frameDragActive, "textEditFrameDragEnd(): the drag still ends, even on failure");
    }

    // Exactly at the threshold in one dimension only -> still too small.
    {
      TextEditState st;
      textEditFrameDragBegin(&st, PathPoint{0, 0}, 1);
      textEditFrameDragUpdate(&st, PathPoint{100, 3.9f});
      TextContent out;
      check(!textEditFrameDragEnd(&st, &out, /*minSizeDoc=*/4.0f),
            "textEditFrameDragEnd(): a drag wide enough but too SHORT (3.9 < 4.0) still fails "
            "-- both dimensions must clear the minimum");
    }
  }

  // =======================================================================
  // 5. Session isolation and cancel
  // =======================================================================
  {
    TextContent contentA;
    contentA.utf8 = "hello";
    TextEditState st;
    textEditBegin(&st, /*documentId=*/1, /*layerIndex=*/2, contentA);
    check(st.documentId == 1 && st.layerIndex == 2 && st.caret == 5,
          "textEditBegin(): (setup) opens a session on document 1, layer 2, caret at the end");

    // Advance the caret and start a frame drag mid-session (an odd sequence
    // in practice, but exactly the kind of leftover state a stale session
    // must not carry into a NEW one).
    textCaretLeft(contentA, &st, /*extend=*/false);
    textEditFrameDragBegin(&st, PathPoint{7, 7}, /*documentId=*/1);
    check(st.frameDragActive, "(setup) a frame drag is live before the document switch");

    TextContent contentB;
    contentB.utf8 = "world!!";
    textEditBegin(&st, /*documentId=*/9, /*layerIndex=*/0, contentB);
    check(st.documentId == 9 && st.layerIndex == 0,
          "textEditBegin(): REQUIRED -- beginning a session on a different document/layer "
          "overwrites the identity fields rather than merging with the old session");
    check(st.caret == contentB.utf8.size(),
          "textEditBegin(): REQUIRED -- the new session's caret is document B's own end (7), "
          "not anything left over from document A's mid-string position");
    check(!st.frameDragActive,
          "textEditBegin(): REQUIRED -- document A's live frame drag does not survive into "
          "document B's session -- a session begun on document A is not applied to document B");
    check(!st.undoOpened, "textEditBegin(): a new session has not opened an undo entry");

    // The frame-drag entry point is the other place a session can begin, and
    // it isolates the same way: beginning a drag on a document drops
    // whatever caret session was live for a different one.
    TextEditState st2;
    TextContent contentC;
    contentC.utf8 = "abc";
    textEditBegin(&st2, /*documentId=*/1, /*layerIndex=*/0, contentC);
    textEditFrameDragBegin(&st2, PathPoint{1, 1}, /*documentId=*/2);
    check(st2.documentId == 2 && st2.layerIndex == kNoLayer,
          "textEditFrameDragBegin(): REQUIRED -- starting a frame drag on document 2 replaces "
          "document 1's caret session -- its layerIndex becomes kNoLayer (no layer exists yet "
          "for a drag that has not finished)");
  }
  {
    TextContent content;
    content.utf8 = "unchanged";
    const std::string before = content.utf8;
    TextEditState st;
    textEditBegin(&st, 1, 0, content);
    textEditFrameDragBegin(&st, PathPoint{3, 3}, 1);
    textEditFrameDragUpdate(&st, PathPoint{30, 30});
    textEditCancel(&st);
    check(!st.frameDragActive && ptNear(st.frameDragStart, PathPoint{}) &&
              ptNear(st.frameDragNow, PathPoint{}),
          "textEditCancel(): REQUIRED -- clears the live drag and its corners");
    check(content.utf8 == before,
          "textEditCancel(): REQUIRED -- the content is untouched (this function never "
          "receives a TextContent* to edit in the first place)");
    check(!st.undoOpened, "textEditCancel(): the undo-opened flag is released with the drag");
  }

  // ==========================================================================
  // THE SELECTION -- caret + anchor, and every edit that has to respect it
  // ==========================================================================
  //
  // Reported as "I cant select text", and there was nothing to select WITH:
  // `TextEditState` had a caret and no other end. The model is a second byte
  // offset, `anchor`; the range is DERIVED from the pair rather than stored
  // beside them, so it cannot go stale against the caret it is built from.
  std::printf("  -- the selection --\n");
  {
    TextContent text;
    text.utf8 = "Handgloves";
    TextEditState st;
    textEditBegin(&st, /*documentId=*/1, /*layerIndex=*/0, text);

    check(textSelection(st).empty(),
          "selection: a fresh session has an EMPTY selection -- just a caret, which is the state "
          "for the whole of ordinary typing");

    textCaretHome(&st, /*extend=*/false);
    textCaretRight(text, &st, /*extend=*/true);
    textCaretRight(text, &st, /*extend=*/true);
    check(textSelection(st).lo == 0 && textSelection(st).hi == 2,
          "selection: REQUIRED -- Shift+Right twice selects the first two bytes; the anchor stays "
          "put at 0 while the caret moves");
    check(textSelectedUtf8(text, st) == "Ha", "selection: and the selected bytes are those two");

    textCaretLeft(text, &st, /*extend=*/true);
    check(textSelection(st).lo == 0 && textSelection(st).hi == 1,
          "selection: Shift+Left shrinks the range from the caret end, leaving the anchor");

    // **The caret can end up LEFT of the anchor.** A reader assuming
    // caret >= anchor produces a backwards, empty or enormous range here.
    textCaretRight(text, &st, /*extend=*/false);
    textCaretRight(text, &st, /*extend=*/false);
    textCaretRight(text, &st, /*extend=*/false);
    textCaretLeft(text, &st, /*extend=*/true);
    textCaretLeft(text, &st, /*extend=*/true);
    check(textSelection(st).lo == 1 && textSelection(st).hi == 3 && st.caret < st.anchor,
          "selection: REQUIRED -- selecting BACKWARDS gives the same range sorted (1,3) with the "
          "caret left of the anchor; textSelection() sorts so no reader has to know the order");

    // Unextended Left/Right collapse to the near EDGE, not one step from the
    // caret end.
    textCaretHome(&st, /*extend=*/false);
    textCaretRight(text, &st, /*extend=*/false);
    for (int i = 0; i < 3; ++i) textCaretRight(text, &st, /*extend=*/true);
    check(textSelection(st).lo == 1 && textSelection(st).hi == 4, "(setup) bytes 1..4 selected");
    textCaretLeft(text, &st, /*extend=*/false);
    check(st.caret == 1 && textSelection(st).empty(),
          "selection: REQUIRED -- plain Left with a range live collapses to its LEFT edge and "
          "stops there; stepping one character back from the caret end is the wrong answer that "
          "every editor agrees about");
    for (int i = 0; i < 3; ++i) textCaretRight(text, &st, /*extend=*/true);
    textCaretRight(text, &st, /*extend=*/false);
    check(st.caret == 4 && textSelection(st).empty(),
          "selection: and plain Right collapses to the RIGHT edge, symmetrically");
  }

  // Every edit respects the range, and does so INSIDE the edit functions so
  // that no call site can forget -- app/TextTool.hpp says why, and these are
  // the checks.
  {
    TextContent t2;
    t2.utf8 = "Handgloves";
    TextEditState s2;
    textEditBegin(&s2, 1, 0, t2);
    textSelectAll(&s2, t2);
    check(textSelection(s2).lo == 0 && textSelection(s2).hi == 10,
          "selection: textSelectAll() covers the whole block");
    textInsertUtf8(&t2, &s2, "X");
    check(t2.utf8 == "X" && s2.caret == 1 && textSelection(s2).empty(),
          "selection: REQUIRED -- typing over a selection REPLACES it. An insert that forgot "
          "would leave \"XHandgloves\" -- inserting into the middle of a range the user believed "
          "they were replacing, which looks like corruption rather than a missing feature");

    TextContent t3;
    t3.utf8 = "Handgloves";
    TextEditState s3;
    textEditBegin(&s3, 1, 0, t3);
    textCaretHome(&s3, false);
    for (int i = 0; i < 4; ++i) textCaretRight(t3, &s3, /*extend=*/true);
    check(textBackspace(&t3, &s3) && t3.utf8 == "gloves" && s3.caret == 0,
          "selection: REQUIRED -- Backspace with a range deletes THE RANGE and nothing else, not "
          "the range plus the character before it");
    check(textSelection(s3).empty(), "selection: and the range goes with the bytes");

    TextContent t4;
    t4.utf8 = "Handgloves";
    TextEditState s4;
    textEditBegin(&s4, 1, 0, t4);
    textSelectAll(&s4, t4);
    check(textDeleteForward(&t4, &s4) && t4.utf8.empty(),
          "selection: Delete with a range deletes the range too");
  }

  // Multi-byte: a range is two offsets, and either can land mid-sequence if
  // anything clamps only one of them.
  {
    const std::string eAcute = "\xC3\xA9";
    TextContent t5;
    t5.utf8 = "caf" + eAcute + "s";  // 6 bytes, 5 characters
    TextEditState s5;
    textEditBegin(&s5, 1, 0, t5);
    textCaretHome(&s5, false);
    for (int i = 0; i < 4; ++i) textCaretRight(t5, &s5, /*extend=*/true);
    check(textSelection(s5).hi == 5,
          "selection: REQUIRED -- four CHARACTERS of a string with an accent in it is five "
          "BYTES; a range counted in characters, or clamped on one end only, splits the accent");
    check(textSelectedUtf8(t5, s5) == "caf" + eAcute,
          "selection: and the selected bytes are valid UTF-8, the accent intact");
    textInsertUtf8(&t5, &s5, "z");
    check(t5.utf8 == "zs", "selection: replacing that range leaves valid UTF-8 behind");
  }

  // The pointer gesture: begin, drag, end -- and end does NOT collapse.
  {
    TextContent t6;
    t6.utf8 = "Handgloves";
    TextEditState s6;
    textEditBegin(&s6, 1, 0, t6);
    textSelectDragBegin(&s6, t6, 2);
    check(s6.selectDragActive && textSelection(s6).empty(),
          "selection: a drag that has not moved yet is exactly a caret -- both ends at the "
          "pen-down, so a click that never drags places a caret and selects nothing");
    textSelectDragUpdate(&s6, t6, 6);
    check(textSelection(s6).lo == 2 && textSelection(s6).hi == 6,
          "selection: dragging moves the caret end and leaves the anchor at the pen-down");
    textSelectDragEnd(&s6);
    check(!s6.selectDragActive && textSelection(s6).lo == 2 && textSelection(s6).hi == 6,
          "selection: REQUIRED -- ending the drag KEEPS the range; collapsing on mouse-up would "
          "make a drag-selection impossible to perform at all");
    textSelectDragUpdate(&s6, t6, 9);
    check(textSelection(s6).hi == 6,
          "selection: and an update after End changes nothing -- a stray move must not keep "
          "re-selecting after a mouse-up the window never saw");
  }

  // A fresh gesture starts with no selection, whatever the last one left --
  // the bug this found in `textEditFrameDragBegin()`, which reset the caret
  // and not the anchor, so a new block began with a phantom range from 0 to
  // the previous session's caret and the first character typed "replaced" it.
  {
    TextContent t7;
    t7.utf8 = "Handgloves";
    TextEditState s7;
    textEditBegin(&s7, 1, 0, t7);
    // **A NON-ZERO anchor, and this detail is the whole test.** The obvious
    // fixture -- `textSelectAll()` -- leaves the anchor at 0, and
    // `textEditFrameDragBegin()` sets the caret to 0, so the range collapses
    // whether or not the anchor is reset and the check passes either way.
    // Sabotaging the fix found exactly that: the assertion below did not
    // move. A backwards selection puts the anchor at the END, where a failure
    // to reset it is visible.
    textCaretHome(&s7, /*extend=*/true);
    check(s7.anchor == 10 && s7.caret == 0 && !textSelection(s7).empty(),
          "(setup) a range is live on the old session with its anchor at the far end, which is "
          "the only arrangement in which the next check can fail");

    textEditFrameDragBegin(&s7, PathPoint{4.0f, 4.0f}, /*documentId=*/1);
    check(textSelection(s7).empty(),
          "selection: REQUIRED -- beginning a frame drag clears the range as well as the caret; "
          "a stale anchor would give the new block a phantom selection for its first keystroke "
          "to replace");

    textEditBegin(&s7, 1, 0, t7);
    textSelectAll(&s7, t7);
    textEditCancel(&s7);
    check(textSelection(s7).empty(),
          "selection: REQUIRED -- and ending the session leaves none behind, so nothing draws a "
          "highlight over a block that is no longer being edited");
  }

  // ==========================================================================
  // PASTED TEXT -- what may come in off the system pasteboard
  // ==========================================================================
  //
  // The stakes, and why this is refused rather than repaired: `shapeText()`
  // fails on invalid UTF-8 and `textContentToShapes()` turns that into NO
  // SHAPES, so one bad byte does not corrupt one character -- it blanks the
  // whole block, silently, including everything that was already in it.
  std::printf("  -- pasted text --\n");
  {
    std::string out;

    check(textSanitizePasted("hello", &out) && out == "hello",
          "paste: plain ASCII passes through unchanged");

    const std::string eAcute = "\xC3\xA9";
    const std::string euro = "\xE2\x82\xAC";
    const std::string emoji = "\xF0\x9F\x8E\xA8";  // 4-byte, outside the BMP
    check(textSanitizePasted(eAcute + euro + emoji, &out) && out == eAcute + euro + emoji,
          "paste: 2-, 3- and 4-byte characters all survive intact");

    // Line endings, which is what text from another application actually
    // looks like.
    check(textSanitizePasted("a\r\nb", &out) && out == "a\nb",
          "paste: REQUIRED -- CRLF becomes ONE newline, not two; text pasted from Windows or a "
          "web page would otherwise double-space itself");
    check(textSanitizePasted("a\rb", &out) && out == "a\nb",
          "paste: a lone CR becomes a newline too");
    check(textSanitizePasted("a\r\n\r\nb", &out) && out == "a\n\nb",
          "paste: and a blank line between two CRLFs stays exactly one blank line");
    check(textSanitizePasted("a\n\rb", &out) && out == "a\n\nb",
          "paste: LF followed by CR is two line breaks, not one -- only CR+LF pairs collapse");

    // Controls: the kept set matches what the typing loop admits, so pasting
    // a character and typing it give the same block.
    check(textSanitizePasted("a\tb\nc", &out) && out == "a\tb\nc",
          "paste: tab and newline are kept -- the same two the typing loop admits");
    check(textSanitizePasted(std::string("a\x01\x1F", 3) + "b", &out) && out == "ab",
          "paste: the other C0 controls are dropped");
    check(textSanitizePasted(std::string("a\x7F", 2) + "b", &out) && out == "ab",
          "paste: DEL is dropped too");

    // --- the refusals, each one a way a block could be blanked -----------
    const std::string before = "untouched";
    out = before;
    check(!textSanitizePasted("\xC3", &out),
          "paste: REQUIRED -- a truncated multi-byte sequence at the very end is REFUSED; this "
          "is what a clipboard buffer cut short looks like");
    check(out == before,
          "paste: REQUIRED -- and a refusal leaves *out untouched, so the caller can say "
          "'nothing was pasted' and mean it");

    check(!textSanitizePasted("a\xC3z", &out),
          "paste: a lead byte followed by a non-continuation byte is refused");
    check(!textSanitizePasted("\x80\x80", &out),
          "paste: a continuation byte with no lead is refused");
    check(!textSanitizePasted("\xFF\xFE", &out),
          "paste: 0xFF/0xFE -- the bytes a UTF-16 BOM arrives as -- are refused");

    // Overlong: the same code point in more bytes than it needs. Two
    // spellings of one character is exactly the ambiguity a byte-offset
    // caret cannot afford.
    check(!textSanitizePasted("\xC0\xAF", &out),
          "paste: REQUIRED -- an OVERLONG encoding of '/' is refused, not silently accepted as a "
          "second spelling of a character the caret already has one offset for");
    check(!textSanitizePasted("\xE0\x80\xAF", &out), "paste: a 3-byte overlong is refused too");

    // A lone surrogate. CESU-8 and WTF-8 both spell one as three
    // plausible-looking bytes, and it is the exact input the typing loop
    // already skips rather than encodes -- for the same reason.
    check(!textSanitizePasted("\xED\xA0\x80", &out),
          "paste: REQUIRED -- a lone surrogate (U+D800 as CESU-8) is refused; three bytes that "
          "pass a shape-only check and blank the block at shapeText()");
    check(!textSanitizePasted("\xF4\x90\x80\x80", &out),
          "paste: a code point past U+10FFFF is refused");

    // The boundaries themselves, so the refusals above are not just "anything
    // unusual fails".
    check(textSanitizePasted("\xED\x9F\xBF", &out),
          "paste: U+D7FF -- the character immediately BELOW the surrogate range -- is accepted, "
          "so the surrogate check is a range and not a blanket refusal of 3-byte sequences");
    check(textSanitizePasted("\xEE\x80\x80", &out),
          "paste: U+E000, immediately above it, is accepted");
    check(textSanitizePasted("\xF4\x8F\xBF\xBF", &out),
          "paste: U+10FFFF exactly -- the last legal code point -- is accepted");

    check(textSanitizePasted("", &out) && out.empty(),
          "paste: the empty string is valid and yields nothing");
    check(!textSanitizePasted("hello", nullptr),
          "paste: a null out pointer is refused rather than dereferenced");
  }

  // The undo bookkeeping a paste or a cut has to do -- app/TextTool.hpp
  // section 3c. Without it, the next character typed amends straight over the
  // paste's own history entry and undo has nothing to stop at between them.
  {
    TextContent t;
    t.utf8 = "abc";
    TextEditState s;
    textEditBegin(&s, 1, 0, t);
    textEditMarkUndoOpened(&s);
    check(s.undoOpened, "(setup) a typing burst has an entry open");
    textEditClearUndoOpened(&s);
    check(!s.undoOpened,
          "textEditClearUndoOpened(): REQUIRED -- releases the burst's entry so a paste's own "
          "entry is not amended over by the next keystroke");
    check(textSessionActive(s) && s.caret == 3,
          "textEditClearUndoOpened(): and it does NOT end the session or move the caret -- it is "
          "the counterpart to markUndoOpened(), not a second cancel");
  }

  return ok;
}

}  // namespace np
