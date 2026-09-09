#include "app/selftest/Support.hpp"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/Path.hpp"
#include "core/PathFlatten.hpp"
#include "core/TextContent.hpp"
#include "core/VectorShape.hpp"
#include "text/Shaper.hpp"

namespace np {

// core/TextContent -- PLAN.md phase 14 (PRD K1-K3): what a `LayerKind::Text`
// layer holds, and the claim core/TextContent.hpp's section 1 is built
// around -- "text is not a second rendering path", `textContentToShapes()`
// produces exactly the `std::vector<VectorShape>` a Vector layer already
// holds. Headless, GPU-free, writes no files.
//
// **Guarded on `shaperAvailable()`, exactly like app/selftest/TextShaper.cpp**
// -- every assertion below shapes real text through the platform shaper, so
// a build with no CoreText (text/StubShaper.cpp) must see a skipped section,
// not a red one for a font this build was never going to have.
//
// The load-bearing sections are the ones that fail SILENTLY otherwise:
//
//  1. **Glyphs are positioned, not stacked** -- dropping `ShapedGlyph::{x,
//     y}` in the translation is a very visible bug in the app and an easy
//     one to leave uncaught in a test that only checks glyph COUNT.
//  2. **The hash covers every field** -- core/TextContent.hpp's own words:
//     "a forgotten field means a user changes the font and sees the old
//     one". Written as a table over mutations, one per field, so a field
//     added later without a matching entry here is a visible gap in the
//     test file rather than a silent one in the hash.
//  3. **Empty text vs. invalid UTF-8 are distinguishable** -- both leave the
//     shape vector empty; only `errorOut` tells them apart, and the whole
//     point of carrying `errorOut` at all is that distinction.
//  4. **The descender direction** -- see section 8's own comment on why the
//     obvious form of this assertion (`bounds.maxY > 0`) is useless, per
//     app/selftest/TextShaper.cpp's own documented trap.
bool runTextContentTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-66s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  if (!shaperAvailable()) {
    std::printf("  [skip] core/TextContent: %s\n", shaperUnavailableReason());
    return true;
  }

  // ==========================================================================
  // 1. Glyphs are positioned, not stacked.
  // ==========================================================================
  std::printf("  -- 1. glyphs are positioned by ShapedGlyph::{x,y}, not stacked --\n");
  {
    // "AV": two DIFFERENT glyphs, deliberately not "AA". Two identical
    // glyphs would still produce two overlapping-but-plausible-looking
    // shapes even if the per-glyph translation were dropped and both copies
    // landed at the very same (x, y) -- their bounds would coincide, which
    // looks like "two glyphs, stacked" only if you already suspect it, not
    // like an obvious failure. Two different letters make the failure
    // mechanical: if glyph 2 is not translated by its own pen position, its
    // bounds sit under glyph 1's rather than to the right of them, and nasty
    // "did they actually move" ambiguity has nowhere to hide.
    TextContent t = makeTextContent("AV", PathPoint{0.0f, 0.0f});
    const std::vector<VectorShape> shapes = textContentToShapes(t);
    check(shapes.size() == 2, "\"AV\" produces exactly 2 shapes, one per glyph");
    if (shapes.size() == 2) {
      const PathBounds bA = pathTightBounds(shapes[0].path);
      const PathBounds bV = pathTightBounds(shapes[1].path);
      check(bA.valid && bV.valid, "both glyph shapes have valid tight bounds");
      std::printf("  [measured] 'A' bounds x %.2f..%.2f, 'V' bounds x %.2f..%.2f\n", bA.minX,
                  bA.maxX, bV.minX, bV.maxX);
      // NOT a bare `bV.minX > bA.minX`. Sabotage found that useless: 'A' and
      // 'V' have slightly different left side-bearings even drawn AT THE
      // SAME (x, y) (dropping the per-glyph translation entirely still
      // measured bV.minX 0.26px to the right of bA.minX, purely from the two
      // outlines' own shapes), so a bare ">" passed on a build that stacked
      // every glyph at the origin. The real per-glyph advance at 24px
      // Helvetica is roughly a glyph-width, ~14-15px here (see the
      // [measured] line), so the threshold below sits an order of magnitude
      // above what two coincidentally-offset outlines could produce and
      // comfortably below a real advance -- separating "moved" from "didn't"
      // rather than merely "not identical".
      constexpr float kMinSeparationPx = 5.0f;
      check(bV.minX > bA.minX + kMinSeparationPx,
            "'V' sits well to the RIGHT of 'A' (>5px), not just nominally right of it -- "
            "the per-glyph x translation ran");
    }
  }

  // ==========================================================================
  // 2. `origin` translates the whole block.
  // ==========================================================================
  std::printf("  -- 2. origin translates the whole block --\n");
  {
    const TextContent atZero = makeTextContent("Hello", PathPoint{0.0f, 0.0f});
    const TextContent atOffset = makeTextContent("Hello", PathPoint{100.0f, 50.0f});
    const PathBounds b0 = textContentBounds(atZero);
    const PathBounds b1 = textContentBounds(atOffset);
    check(b0.valid && b1.valid, "both blocks have valid bounds");
    // 0.01px tolerance: this is a straight float add with no accumulation
    // (one translation per glyph, not a running sum), so agreement should be
    // exact bar the last bit or two of float rounding; the tolerance is
    // generous rather than tight because the intent is "the origin moved
    // it", not "prove float addition works".
    const float tol = 0.01f;
    check(std::fabs((b1.minX - b0.minX) - 100.0f) < tol &&
              std::fabs((b1.maxX - b0.maxX) - 100.0f) < tol &&
              std::fabs((b1.minY - b0.minY) - 50.0f) < tol &&
              std::fabs((b1.maxY - b0.maxY) - 50.0f) < tol,
          "bounds at origin (100,50) are the (0,0) bounds shifted by exactly (100,50), tol 0.01px");
  }

  // ==========================================================================
  // 3. Point vs. paragraph text: wrapping is the whole difference.
  // ==========================================================================
  std::printf("  -- 3. point text vs. a narrow paragraph frame --\n");
  {
    const char* sentence = "The quick brown fox jumps over the lazy dog and then some more";
    TextContent point = makeTextContent(sentence, PathPoint{0.0f, 0.0f});
    // point.frame stays TextFrame{} == {0,0} from makeTextContent -- point
    // text, per core/TextContent.hpp section 2.
    TextContent paragraph = point;
    paragraph.frame = TextFrame{80.0f, 0.0f};

    const PathBounds bPoint = textContentBounds(point);
    const PathBounds bParagraph = textContentBounds(paragraph);
    check(bPoint.valid && bParagraph.valid, "both blocks have valid bounds");
    const float widthPoint = bPoint.maxX - bPoint.minX;
    const float widthParagraph = bParagraph.maxX - bParagraph.minX;
    const float heightPoint = bPoint.maxY - bPoint.minY;
    const float heightParagraph = bParagraph.maxY - bParagraph.minY;
    std::printf("  [measured] point %.2fx%.2f, 80px-wrapped paragraph %.2fx%.2f\n", widthPoint,
                heightPoint, widthParagraph, heightParagraph);
    check(widthPoint > widthParagraph,
          "unwrapped point text is wider than the same text wrapped to 80px");
    check(heightParagraph > heightPoint,
          "...and the wrapped paragraph is taller -- more lines, not a shrink");
  }

  // ==========================================================================
  // 4. Alignment does something, under paragraph text only.
  // ==========================================================================
  std::printf("  -- 4. TextAlign::Right sits further right than Left, under a wide frame --\n");
  {
    // text/Shaper.hpp is explicit that alignment is meaningless for point
    // text (frame.width == 0, "there is nothing to align against"), so this
    // asserts alignment under a genuinely wide PARAGRAPH frame -- wide
    // enough that "Hi" does not wrap, isolating alignment as the only
    // difference between the two blocks rather than conflating it with
    // wrapping (section 3's concern).
    TextContent left = makeTextContent("Hi", PathPoint{0.0f, 0.0f});
    left.frame = TextFrame{800.0f, 0.0f};
    left.align = TextAlign::Left;
    TextContent right = left;
    right.align = TextAlign::Right;

    const PathBounds bLeft = textContentBounds(left);
    const PathBounds bRight = textContentBounds(right);
    check(bLeft.valid && bRight.valid, "both blocks have valid bounds");
    std::printf("  [measured] Left minX=%.2f, Right minX=%.2f (frame width 800)\n", bLeft.minX,
                bRight.minX);
    check(bRight.minX > bLeft.minX,
          "under an 800px frame, TextAlign::Right sits further right than TextAlign::Left");
  }

  // ==========================================================================
  // 5. The hash covers every field.
  // ==========================================================================
  //
  // core/TextContent.hpp's own words: "a forgotten field means a user
  // changes the font and sees the old one." Written as a table over
  // mutations rather than fifteen copy-pasted blocks, one entry per field
  // named in core/TextContent.hpp, so a field this table does not name is a
  // visible gap in the test rather than a silent one in the hash.
  // ==========================================================================
  std::printf("  -- 5. the hash covers every field --\n");
  {
    TextContent base = makeTextContent("Hello", PathPoint{10.0f, 20.0f});
    base.style.fontFamily = "Helvetica";
    base.style.sizePx = 24.0f;
    base.style.tracking = 0.0f;
    base.style.leading = 0.0f;
    base.style.bold = false;
    base.style.italic = false;
    base.frame = TextFrame{0.0f, 0.0f};
    base.align = TextAlign::Left;
    base.fill.on = true;
    base.fill.rgba = {0.0f, 0.0f, 0.0f, 1.0f};
    base.stroke.on = false;
    base.strokeStyle = StrokeStyle{};

    const uint64_t h0 = textContentHash(base);
    check(textContentHash(base) == h0, "hash: the same content hashes the same twice (stability)");

    const std::vector<std::pair<const char*, std::function<void(TextContent&)>>> mutations = {
        {"utf8", [](TextContent& t) { t.utf8 = "Hellp"; }},
        {"style.fontFamily", [](TextContent& t) { t.style.fontFamily = "Times New Roman"; }},
        {"style.sizePx", [](TextContent& t) { t.style.sizePx += 1.0f; }},
        {"style.tracking", [](TextContent& t) { t.style.tracking += 1.0f; }},
        {"style.leading", [](TextContent& t) { t.style.leading += 1.0f; }},
        {"style.bold", [](TextContent& t) { t.style.bold = !t.style.bold; }},
        {"style.italic", [](TextContent& t) { t.style.italic = !t.style.italic; }},
        {"frame.width", [](TextContent& t) { t.frame.width += 10.0f; }},
        {"frame.height", [](TextContent& t) { t.frame.height += 10.0f; }},
        {"align", [](TextContent& t) { t.align = TextAlign::Right; }},
        {"origin.x", [](TextContent& t) { t.origin.x += 1.0f; }},
        {"origin.y", [](TextContent& t) { t.origin.y += 1.0f; }},
        {"fill.on", [](TextContent& t) { t.fill.on = !t.fill.on; }},
        {"fill.rgba", [](TextContent& t) { t.fill.rgba[0] += 0.1f; }},
        {"stroke", [](TextContent& t) { t.stroke.on = true; }},
        {"strokeStyle", [](TextContent& t) { t.strokeStyle.width += 1.0f; }},
    };

    for (const auto& [field, mutate] : mutations) {
      TextContent mutated = base;
      mutate(mutated);
      const uint64_t hm = textContentHash(mutated);
      const std::string what = std::string("hash: changing ") + field + " changes the hash";
      check(hm != h0, what.c_str());
    }
  }

  // ==========================================================================
  // 6. Empty text vs. invalid UTF-8 -- distinguishable, not just both empty.
  // ==========================================================================
  std::printf("  -- 6. empty text is not a failure; invalid UTF-8 is, and they must differ --\n");
  {
    const TextContent empty = makeTextContent("", PathPoint{0.0f, 0.0f});
    // A sentinel, not an empty string, so this actually proves `errorOut` is
    // left UNTOUCHED (core/TextContent.hpp's own word) rather than merely
    // ending up empty, which a function that clears it on every call would
    // also produce.
    std::string errEmpty = "sentinel-must-not-change";
    const std::vector<VectorShape> emptyShapes = textContentToShapes(empty, &errEmpty);
    check(emptyShapes.empty(), "empty utf8 produces zero shapes");
    check(errEmpty == "sentinel-must-not-change", "empty utf8 leaves errorOut UNTOUCHED");
    check(!textContentDraws(empty), "empty text does not draw, though fill is on by default");
    check(!textContentBounds(empty).valid,
          "empty text has invalid bounds -- not a zero-area box at origin");

    // A lone continuation byte, exactly app/selftest/TextShaper.cpp's own
    // invalid-UTF-8 fixture -- not valid UTF-8 on its own.
    const TextContent invalid = makeTextContent(std::string("\x80\x80"), PathPoint{0.0f, 0.0f});
    std::string errInvalid;
    const std::vector<VectorShape> invalidShapes = textContentToShapes(invalid, &errInvalid);
    check(invalidShapes.empty(), "invalid UTF-8 also produces zero shapes");
    check(!errInvalid.empty(),
          "...but UNLIKE empty text, invalid UTF-8 SETS errorOut -- this is the whole "
          "distinction errorOut exists to carry");
  }

  // ==========================================================================
  // 7. textContentDraws() is false with fill and stroke both off.
  // ==========================================================================
  std::printf("  -- 7. textContentDraws() needs paint, not just glyphs --\n");
  {
    TextContent invisible = makeTextContent("Hello", PathPoint{0.0f, 0.0f});
    invisible.fill.on = false;
    invisible.stroke.on = false;
    check(!invisible.utf8.empty(), "sanity: the string is genuinely non-empty");
    check(!textContentDraws(invisible),
          "textContentDraws() is false with fill and stroke both off, despite non-empty text");
  }

  // ==========================================================================
  // 8. A descender is below the baseline -- the y-DOWN convention.
  // ==========================================================================
  std::printf("  -- 8. 'p' extends further down than 'b' -- the y-DOWN convention --\n");
  {
    // "p" has a descender AND a bowl reaching x-height; "b" has an ascender
    // and NO descender at all. The naive assertion here would be
    // `bounds(p).maxY > 0`, and per app/selftest/TextShaper.cpp's own
    // documented trap that is TRUE IN BOTH y conventions -- 'p' straddles
    // the baseline either way up, so its max-y clears zero whichever
    // direction is "down". What actually distinguishes the conventions is
    // comparing 'p' against 'b' at the SAME origin: only in y-DOWN does 'p's
    // descender (below the baseline) read as a LARGER y than 'b's flat
    // bottom (which sits at the baseline, having no descender of its own).
    // Get the flip backwards and 'b's ascender -- the tallest excursion
    // either glyph has -- becomes the larger maxY instead, which is exactly
    // what the sabotage run below is checked to catch.
    const TextContent p = makeTextContent("p", PathPoint{0.0f, 0.0f});
    const TextContent b = makeTextContent("b", PathPoint{0.0f, 0.0f});
    const PathBounds bp = textContentBounds(p);
    const PathBounds bb = textContentBounds(b);
    check(bp.valid && bb.valid, "'p' and 'b' both have valid bounds");
    std::printf("  [measured] 'p' y-range %.2f..%.2f, 'b' y-range %.2f..%.2f (block origin at 0)\n",
                bp.minY, bp.maxY, bb.minY, bb.maxY);
    check(bp.maxY > bb.maxY,
          "'p' reaches further DOWN than 'b' (larger maxY) -- true only y-DOWN; the naive "
          "bp.maxY>0 form passes either way up and proves nothing (see comment above)");
  }

  // --- the anchor: point text is pinned by its BASELINE ---------------------
  //
  // core/TextContent.hpp section 2b. Two user-visible promises, and each was
  // broken before `origin` meant the baseline.
  {
    const PathPoint org{200.0f, 300.0f};

    // 1. Making the type bigger grows it UP and to the right out of a fixed
    //    bottom-left, rather than pushing the block down the page.
    {
      TextContent small = makeTextContent("Handgloves", org);
      small.style.sizePx = 12.0f;
      TextContent big = small;
      big.style.sizePx = 96.0f;
      const PathBounds bs = textContentBounds(small);
      const PathBounds bb = textContentBounds(big);
      check(bs.valid && bb.valid, "anchor: both sizes shape");
      std::printf("  [measured] 12px box y %.2f..%.2f, 96px box y %.2f..%.2f (baseline at %.1f)\n",
                  bs.minY, bs.maxY, bb.minY, bb.maxY, org.y);

      // The load-bearing one: the box grows UPWARD. Under the old top-left
      // anchor `minY` stayed put and `maxY` ran down the page instead.
      check(bb.minY < bs.minY - 10.0f,
            "anchor: REQUIRED -- enlarging the type extends the block UPWARD (a smaller minY). "
            "Pinning the top of the line box instead pushes the text DOWN the page, away from "
            "the corner the user placed");

      // And the left edge does not move -- the other half of "bottom left".
      check(std::fabs(bb.minX - bs.minX) < 8.0f,
            "anchor: and the LEFT edge stays put, so the growth is up-and-right");

      // The baseline itself is exactly `origin.y` at BOTH sizes. Asserted on
      // the caret rather than the ink, because ink dips below the baseline by
      // a descender ('g' here) and that dip legitimately deepens with size --
      // the baseline is the thing that must not move.
      //
      // The baseline is recovered from the segment's own 0.8/0.2 split rather
      // than from the private `caretHeightFor()`: top + 0.8 of the span.
      const TextCaretSegment cs = textCaretSegment(small, 0);
      const TextCaretSegment cb = textCaretSegment(big, 0);
      const float baseS = cs.top.y + 0.8f * (cs.bottom.y - cs.top.y);
      const float baseB = cb.top.y + 0.8f * (cb.bottom.y - cb.top.y);
      check(std::fabs(baseS - org.y) < 0.01f && std::fabs(baseB - org.y) < 0.01f,
            "anchor: REQUIRED -- the first baseline is exactly origin.y at BOTH sizes, so what is "
            "pinned is a baseline and not a bounding box that happens to be near one");
    }

    // 2. The caret does not JUMP when the first character is typed. An empty
    //    block's caret was drawn at `origin` while the first glyph landed an
    //    ascent lower, so the text appeared a whole ascent below where the
    //    insertion point had been sitting.
    {
      TextContent empty = makeTextContent("", org);
      empty.style.sizePx = 48.0f;
      TextContent typed = empty;
      typed.utf8 = "H";
      const TextCaretSegment before = textCaretSegment(empty, 0);
      const TextCaretSegment after = textCaretSegment(typed, 1);
      std::printf("  [measured] empty caret bottom %.2f, after typing 'H' %.2f\n", before.bottom.y,
                  after.bottom.y);
      check(std::fabs(before.bottom.y - after.bottom.y) < 0.01f,
            "anchor: REQUIRED -- the caret sits at the SAME height before and after the first "
            "keystroke. It used to drop by a full ascent, so the text did not appear where the "
            "insertion point promised it would");
    }

    // 3. Paragraph text is NOT moved: its `origin` is the frame's top-left,
    //    which is the box the user dragged out. Enlarging the type fills
    //    further down the frame; the frame stays where it is.
    {
      TextContent para = makeTextContent("Paragraph text wraps inside its frame.", org);
      para.frame.width = 300.0f;
      para.style.sizePx = 12.0f;
      TextContent bigPara = para;
      bigPara.style.sizePx = 24.0f;
      const PathBounds bs = textContentBounds(para);
      const PathBounds bb = textContentBounds(bigPara);
      check(bs.valid && bb.valid && bb.minY > org.y - 1.0f && bs.minY > org.y - 1.0f,
            "anchor: REQUIRED -- a PARAGRAPH block still hangs DOWN from its frame's top-left at "
            "both sizes. Pinning a frame by its first baseline would slide the whole box up the "
            "page whenever the type size changed");
    }
  }

  // --- Return moves the caret, before anything is typed ---------------------
  //
  // CoreText's framesetter lays out no line for a newline that ENDS the text,
  // so the caret used to stay at the end of the previous line: you pressed
  // Return and nothing moved. Press it twice and start typing and you were
  // two lines down, because both newlines had been in the string all along.
  {
    const PathPoint org{200.0f, 300.0f};
    auto para = [&](const char* utf8) {
      TextContent t = makeTextContent(utf8, org);
      t.style.sizePx = 48.0f;
      t.frame.width = 400.0f;
      return t;
    };

    // THE assertion: the caret after Return is exactly where the next
    // character will be drawn. Compared against the real thing -- the caret
    // sitting before a 'Y' that IS on the second line -- rather than against
    // a number this test worked out for itself.
    const TextContent afterReturn = para("Hi\n");
    const TextContent thenTyped = para("Hi\nY");
    const TextCaretSegment cr = textCaretSegment(afterReturn, 3);
    const TextCaretSegment ct = textCaretSegment(thenTyped, 3);
    std::printf("  [measured] caret after Return (%.2f,%.2f) vs before a typed 'Y' (%.2f,%.2f)\n",
                cr.bottom.x, cr.bottom.y, ct.bottom.x, ct.bottom.y);
    check(std::fabs(cr.bottom.x - ct.bottom.x) < 0.01f &&
              std::fabs(cr.bottom.y - ct.bottom.y) < 0.01f,
          "newline: REQUIRED -- the caret after Return sits exactly where the next character "
          "will be drawn. It used to stay on the previous line until something was typed");

    // And a second Return moves it a second time, by exactly one line -- the
    // spacing taken from a block that really does have two lines.
    const TextContent twoLines = para("Hi\nYo");
    const TextCaretSegment l1 = textCaretSegment(twoLines, 0);
    const TextCaretSegment l2 = textCaretSegment(twoLines, 3);
    const float lineStep = l2.bottom.y - l1.bottom.y;
    const TextCaretSegment cr2 = textCaretSegment(para("Hi\n\n"), 4);
    check(lineStep > 1.0f && std::fabs((cr2.bottom.y - cr.bottom.y) - lineStep) < 0.01f,
          "newline: REQUIRED -- a SECOND Return moves the caret exactly one more line, by the "
          "same spacing the block uses between two real lines");

    // The empty line's caret honours alignment: a centred paragraph puts it
    // in the middle, not hard against the left edge.
    TextContent centred = para("Hi\n");
    centred.align = TextAlign::Center;
    const TextCaretSegment cc = textCaretSegment(centred, 3);
    check(std::fabs(cc.bottom.x - (org.x + 400.0f * 0.5f)) < 0.01f,
          "newline: the empty line's caret sits at the alignment point -- centred here, not at "
          "the left edge");

    // A freshly dragged, still-EMPTY text frame puts its caret on the first
    // baseline, not on the frame's top edge. With the caret's baseline on the
    // top edge the bar draws almost entirely above the box, and then the
    // first character typed appears a whole ascent lower -- the same "the
    // caret is not where the text will be" complaint as the newline case.
    {
      TextContent blank = para("");
      TextContent oneChar = para("H");
      const TextCaretSegment cb = textCaretSegment(blank, 0);
      const TextCaretSegment c1 = textCaretSegment(oneChar, 0);
      std::printf("  [measured] empty frame caret y %.2f, after one character %.2f (frame top %.1f)\n",
                  cb.bottom.y, c1.bottom.y, org.y);
      check(std::fabs(cb.bottom.y - c1.bottom.y) < 0.01f,
            "empty frame: REQUIRED -- an empty PARAGRAPH block's caret sits exactly where the "
            "first character will, not on the frame's top edge with the bar hanging above the box");
      check(cb.bottom.y > org.y,
            "empty frame: and that is BELOW the frame's top-left, which is what `origin` means "
            "for paragraph text -- an assertion the equality above would pass even if both were "
            "wrong in the same direction");
    }

    // Point text is deliberately NOT given a phantom line. Its shaper never
    // breaks a line at all, so a caret dropped to a second one would sit
    // under text that is not there.
    auto pointBlock = [&](const char* utf8) {
      TextContent t = makeTextContent(utf8, org);
      t.style.sizePx = 48.0f;   // the SAME size on both sides, or the y's differ for that reason
      return t;                 // and the comparison below would prove nothing
    };
    const TextCaretSegment pc0 = textCaretSegment(pointBlock("Hi"), 2);
    const TextCaretSegment pc1 = textCaretSegment(pointBlock("Hi\n"), 3);
    check(std::fabs(pc1.bottom.y - pc0.bottom.y) < 0.01f,
          "newline: POINT text keeps its caret on the one line it draws -- a newline there is a "
          "separate gap (core/TextContent.hpp section 2), and moving the caret alone would "
          "disguise it by pointing at a line with no text on it");
  }

  // --- the frame's resize handles -------------------------------------------
  //
  // core/TextContent.hpp section 4b. A frame is dragged out before a word is
  // typed, so its size is a guess; these are how the guess gets corrected.
  {
    const PathPoint org{200.0f, 300.0f};
    auto frame = [&](float w, float h) {
      TextContent t = makeTextContent("Paragraph text that will reflow when the frame changes.",
                                      org);
      t.style.sizePx = 24.0f;
      t.frame.width = w;
      t.frame.height = h;
      return t;
    };

    // Point text has no box, so it reports no handles -- and the hit test
    // therefore cannot return one, however close the pointer gets.
    {
      TextContent point = makeTextContent("Handgloves", org);
      TextFrameHandles h;
      check(!textFrameHandles(point, &h),
            "handles: POINT text reports none -- it has no frame to resize");
      check(textFrameHandleAt(point, org, 1000.0f) == TextFrameHandle::None,
            "handles: and no hit test on it can find one, at any radius");
    }

    // Eight handles on the frame's own corners and edge midpoints.
    {
      const TextContent t = frame(400.0f, 200.0f);
      TextFrameHandles h;
      check(textFrameHandles(t, &h), "handles: a paragraph frame reports them");
      check(std::fabs(h.at[0].x - 200.0f) < 0.01f && std::fabs(h.at[0].y - 300.0f) < 0.01f,
            "handles: TopLeft is the frame's origin");
      check(std::fabs(h.at[7].x - 600.0f) < 0.01f && std::fabs(h.at[7].y - 500.0f) < 0.01f,
            "handles: BottomRight is origin + (width, height)");
      check(textFrameHandleAt(t, PathPoint{600.0f, 500.0f}, 5.0f) == TextFrameHandle::BottomRight,
            "handles: the hit test finds the corner under the pointer");
      check(textFrameHandleAt(t, PathPoint{400.0f, 400.0f}, 5.0f) == TextFrameHandle::None,
            "handles: and finds nothing in the middle of the frame");
    }

    // Dragging the right edge changes the WIDTH and nothing else -- which is
    // what makes the text reflow, since `frame.width` is an input to shaping.
    {
      TextContent t = frame(400.0f, 0.0f);
      const PathBounds before = textContentBounds(t);
      check(textFrameResize(&t, TextFrameHandle::MiddleRight, PathPoint{500.0f, 400.0f}, 4.0f),
            "handles: dragging the right edge is accepted");
      check(std::fabs(t.frame.width - 300.0f) < 0.01f,
            "handles: REQUIRED -- the width becomes the drag's own (500 - 200 = 300)");
      check(std::fabs(t.origin.x - org.x) < 0.01f && std::fabs(t.origin.y - org.y) < 0.01f,
            "handles: and the origin did NOT move -- only the far edge did");
      const PathBounds after = textContentBounds(t);
      check(before.valid && after.valid && after.maxY > before.maxY,
            "handles: REQUIRED -- the text REFLOWED: a narrower frame is taller, because the "
            "same words wrap onto more lines. This is what fails if the width is stored but "
            "shaping never sees it");
    }

    // Dragging the LEFT edge moves the origin as well as the width, or the
    // box would grow from the wrong side and slide out from under the cursor.
    {
      TextContent t = frame(400.0f, 200.0f);
      check(textFrameResize(&t, TextFrameHandle::MiddleLeft, PathPoint{150.0f, 400.0f}, 4.0f),
            "handles: dragging the left edge is accepted");
      check(std::fabs(t.origin.x - 150.0f) < 0.01f && std::fabs(t.frame.width - 450.0f) < 0.01f,
            "handles: REQUIRED -- the left edge moves the ORIGIN and widens to match, so the "
            "right edge stays where it was");
    }

    // A block sizing its own height keeps doing so when merely widened.
    {
      TextContent t = frame(400.0f, 0.0f);
      check(textFrameResize(&t, TextFrameHandle::MiddleRight, PathPoint{700.0f, 400.0f}, 4.0f) &&
                t.frame.height == 0.0f,
            "handles: REQUIRED -- widening a block leaves its height AUTOMATIC. Pinning it here "
            "would clip the next line typed inside a box the user never set");
      check(textFrameResize(&t, TextFrameHandle::BottomCenter, PathPoint{400.0f, 560.0f}, 4.0f) &&
                std::fabs(t.frame.height - 260.0f) < 0.01f,
            "handles: and a bottom handle DOES pin it, because that is the one the user grabbed "
            "to set a height");
    }

    // A collapse is refused outright, leaving the frame untouched -- a clamp
    // would stick at the floor and then invert under the cursor.
    {
      TextContent t = frame(400.0f, 200.0f);
      const TextContent before = t;
      check(!textFrameResize(&t, TextFrameHandle::MiddleRight, PathPoint{100.0f, 400.0f}, 4.0f),
            "handles: dragging the right edge past the left is REFUSED");
      check(t.frame.width == before.frame.width && t.origin.x == before.origin.x,
            "handles: and the refusal changed nothing at all");
    }

    // A rotated block resizes along ITS OWN axes. Dragging its right edge by
    // 100 in the block's frame must widen it by 100 -- not by the projection
    // of a document-space delta, which is what an unmapped pointer gives.
    {
      TextContent t = frame(400.0f, 200.0f);
      t.transform = transformRotateDegrees(90.0f);
      // The block's own right edge, under a 90-degree turn, lies along
      // document Y. Ask for a point 100 further along the block's local x by
      // mapping it forward through the same matrix the resize will invert.
      const Point2 want = mat3MapPoint(t.transform, Point2{org.x + 500.0f, org.y + 100.0f});
      check(textFrameResize(&t, TextFrameHandle::MiddleRight, PathPoint{want.x, want.y}, 4.0f),
            "handles: a rotated block's edge drag is accepted");
      check(std::fabs(t.frame.width - 500.0f) < 0.01f,
            "handles: REQUIRED -- and it widens along the BLOCK's axis, not the document's. "
            "Without the inverse map the frame shears away from the cursor");
    }
  }

  // --- the transform: a scaled or rotated block is STILL TEXT ---------------
  //
  // core/TextContent.hpp section 4. The failure these guard against is not
  // "the matrix is stored wrong" -- that is loud -- but the two quiet ones:
  // a matrix that is stored and never APPLIED (the handle looks dead), and a
  // matrix that is applied to the drawing but not to the hit test (the block
  // draws at an angle and can only be clicked where it used to be).
  {
    const PathPoint org{40.0f, 90.0f};

    // 1. Identity changes nothing at all. This is the assertion that fails if
    //    the mapping pass runs unconditionally with a subtly wrong matrix --
    //    every other check here uses a non-identity transform and would not
    //    notice the default case regressing.
    {
      TextContent plain = makeTextContent("Handgloves", org);
      TextContent explicitId = plain;
      explicitId.transform = mat3Identity();
      const PathBounds a = textContentBounds(plain);
      const PathBounds b = textContentBounds(explicitId);
      check(a.valid && b.valid && a.minX == b.minX && a.maxX == b.maxX && a.minY == b.minY &&
                a.maxY == b.maxY,
            "transform: an explicit IDENTITY matrix is byte-for-byte the same geometry as the "
            "default -- the fast path and the general path agree");
    }

    // 2. A scale reaches the geometry, and leaves the string and size alone.
    {
      TextContent t = makeTextContent("Handgloves", org);
      const PathBounds before = textContentBounds(t);
      t.transform = transformScale(2.0f, 2.0f);
      const PathBounds after = textContentBounds(t);
      check(before.valid && after.valid &&
                std::fabs((after.maxX - after.minX) - 2.0f * (before.maxX - before.minX)) < 0.01f,
            "transform: a 2x scale doubles the shaped width -- the matrix is APPLIED by "
            "textContentToShapes(), not merely stored");
      check(t.utf8 == "Handgloves" && t.style.sizePx == makeTextContent("x", org).style.sizePx,
            "transform: and neither the string nor the type size changed -- this is what makes a "
            "scaled block still editable text rather than a rasterised picture of text");
    }

    // 3. **The round trip that matters**: a caret drawn at byte N, clicked on,
    //    comes back as byte N -- under a rotation. This fails if the drawing
    //    path maps and the hit test does not (the block is unclickable where
    //    it is drawn), if the inverse is composed on the wrong side, or if
    //    either side forgets `origin`.
    {
      TextContent t = makeTextContent("Handgloves", org);
      // About the block's own middle, which is what a rotate handle produces.
      const PathBounds box = textContentBounds(t);
      const Point2 pivot{(box.minX + box.maxX) * 0.5f, (box.minY + box.maxY) * 0.5f};
      t.transform = transformRotateDegreesAbout(30.0f, pivot);

      bool everyOffsetRoundTrips = true;
      size_t firstBad = 0;
      for (size_t n = 0; n <= t.utf8.size(); ++n) {
        const TextCaretSegment seg = textCaretSegment(t, n);
        // Sample ON the baseline, a hair inside the caret: the segment's own
        // endpoints are the ascender top and descender bottom, and a click at
        // the very top of a line legitimately belongs to the line above.
        const PathPoint at{(seg.top.x + seg.bottom.x) * 0.5f, (seg.top.y + seg.bottom.y) * 0.5f};
        if (textOffsetAtPoint(t, at) != n) {
          everyOffsetRoundTrips = false;
          firstBad = n;
          break;
        }
      }
      if (!everyOffsetRoundTrips)
        std::printf("  [measured] first caret offset that did not round-trip: %zu\n", firstBad);
      check(everyOffsetRoundTrips,
            "transform: REQUIRED -- under a 30-degree rotation, clicking the middle of the caret "
            "drawn for byte N returns byte N, for EVERY N. This is the assertion that fails if "
            "the hit test does not invert the matrix the drawing applied");
    }

    // 4. The selection quad turns with the block. Under a rotation its top
    //    edge is no longer horizontal -- which an axis-aligned rect could
    //    never express, and is why the API returns four corners.
    {
      TextContent t = makeTextContent("Handgloves", org);
      const std::vector<TextQuad> square = textSelectionQuads(t, 0, 4);
      check(square.size() == 1 && std::fabs(square[0].corner[0].y - square[0].corner[1].y) < 0.01f,
            "transform: unrotated, a selection quad's top edge is horizontal");
      t.transform = transformRotateDegrees(30.0f);
      const std::vector<TextQuad> turned = textSelectionQuads(t, 0, 4);
      check(turned.size() == 1 && std::fabs(turned[0].corner[0].y - turned[0].corner[1].y) > 1.0f,
            "transform: REQUIRED -- rotated, it is NOT. A highlight that stayed axis-aligned "
            "under rotated type is the bug the quad API exists to prevent");
    }

    // 5. The hash covers the matrix, or core/VectorRaster's cache serves the
    //    pre-rotation picture and the handle looks dead.
    {
      TextContent a = makeTextContent("Handgloves", org);
      TextContent b = a;
      b.transform = transformRotateDegrees(15.0f);
      check(textContentHash(a) != textContentHash(b),
            "transform: REQUIRED -- the content hash changes with the matrix, so the raster cache "
            "cannot serve an unrotated block for a rotated one");
    }

    // 6. A degenerate matrix has no inverse, so the hit test refuses rather
    //    than dividing by ~0 and returning a garbage offset.
    {
      TextContent t = makeTextContent("Handgloves", org);
      t.transform = transformScale(0.0f, 1.0f);
      check(textOffsetAtPoint(t, PathPoint{50.0f, 95.0f}) == 0,
            "transform: a collapsed matrix makes the hit test answer 0 rather than reading an "
            "infinity -- and ops/DocumentTransform refuses to store one in the first place");
    }

    // 7. Curve handles are mapped too, not just anchors. `in`/`out` are
    //    absolute positions, so a transform that moved only `pt` would turn
    //    every round glyph inside out -- silent on straight-edged letters.
    {
      TextContent t = makeTextContent("o", org);   // a glyph that is all curve
      t.transform = transformTranslate(1000.0f, 0.0f);
      const std::vector<VectorShape> shapes = textContentToShapes(t, nullptr);
      bool everyHandleMoved = !shapes.empty();
      for (const VectorShape& sh : shapes)
        for (const SubPath& sub : sh.path.subpaths)
          for (const Anchor& an : sub.anchors)
            if (an.in.x < 900.0f || an.out.x < 900.0f) everyHandleMoved = false;
      check(everyHandleMoved,
            "transform: REQUIRED -- the bezier control points move with their anchors. Mapping "
            "only `pt` leaves every curve's handles behind, which is invisible on 'H' and "
            "turns 'o' inside out");
    }
  }

  return ok;
}

}  // namespace np
