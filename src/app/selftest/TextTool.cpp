#include "app/selftest/Support.hpp"

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

      std::vector<size_t> visited = {st.caret};
      for (int i = 0; i < 5; ++i) {
        textCaretLeft(text, &st);
        visited.push_back(st.caret);
      }
      const std::vector<size_t> expected = {11, 10, 6, 3, 1, 0};
      check(visited == expected,
            "textCaretLeft(): REQUIRED -- from the end, visits exactly the character "
            "boundaries in order (11, 10, 6, 3, 1, 0), computed from the string built above");
      textCaretLeft(text, &st);
      check(st.caret == 0, "textCaretLeft(): already at 0 stays at 0 rather than underflowing");
    }

    // --- 2c. caret-right from the start to the end -------------------------
    {
      TextContent text;
      text.utf8 = built;
      TextEditState st;
      st.caret = 0;

      std::vector<size_t> visited = {st.caret};
      for (int i = 0; i < 5; ++i) {
        textCaretRight(text, &st);
        visited.push_back(st.caret);
      }
      check(visited == boundaries,
            "textCaretRight(): REQUIRED -- from the start, visits exactly the character "
            "boundaries in order (0, 1, 3, 6, 10, 11)");
      textCaretRight(text, &st);
      check(st.caret == text.utf8.size(),
            "textCaretRight(): already at the end stays at the end rather than overflowing");
    }

    // --- 2d. caretHome / caretEnd -------------------------------------------
    {
      TextContent text;
      text.utf8 = built;
      TextEditState st;
      st.caret = 5;  // an arbitrary interior position, not a boundary claim
      textCaretHome(&st);
      check(st.caret == 0, "textCaretHome(): caret goes to 0");
      textCaretEnd(text, &st);
      check(st.caret == text.utf8.size(), "textCaretEnd(): caret goes to the byte length");
    }

    // --- 2e. insert a multi-byte character in the MIDDLE --------------------
    {
      TextContent text;
      text.utf8 = built;
      TextEditState st;
      st.caret = 3;  // boundary between e-acute and euro -- neither end

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
      textCaretLeft(text, &st);
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
      textCaretRight(text, &st);
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
    textCaretLeft(contentA, &st);
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

  return ok;
}

}  // namespace np
