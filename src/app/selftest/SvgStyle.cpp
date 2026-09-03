#include "app/selftest/Support.hpp"

#include "io/SvgStyle.hpp"

namespace np {

// io/SvgStyle -- the CSS cascade for SVG presentation (which declaration
// wins), ahead of the XML parser it will eventually sit behind.
//
// Headless, GPU-free, writes no files. Every case below drives the same four
// public functions the eventual SVG importer will call; none of it touches
// an XML node, because io/SvgStyle.hpp's whole point is that it does not
// need one -- `SvgElementView` chains here are built by hand, a few lines
// each.
bool runSvgStyleTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-66s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  const SvgStyleSheet emptySheet;
  const std::vector<SvgDeclaration> noDecls;
  const std::map<std::string, std::string> noInherited;

  auto elem = [](const char* tag, const char* id,
                std::vector<std::string> classes,
                const SvgElementView* parent) {
    SvgElementView e;
    e.tag = tag;
    e.id = id;
    e.classes = std::move(classes);
    e.parent = parent;
    return e;
  };
  auto decl = [](const char* prop, const char* value, bool important = false) {
    return SvgDeclaration{prop, value, important};
  };

  // --- 1. specificity ordering -----------------------------------------

  {
    const SvgSpecificity oneId{1, 0, 0};
    const SvgSpecificity manyClassesTypes{0, 100, 100};
    check(!(oneId < manyClassesTypes),
         "specificity: one id outranks 100 classes and 100 types");

    const SvgSpecificity oneClass{0, 1, 0};
    const SvgSpecificity manyTypes{0, 0, 100};
    check(!(oneClass < manyTypes),
         "specificity: one class outranks 100 types");

    const SvgSpecificity oneType{0, 0, 1};
    const SvgSpecificity twoTypes{0, 0, 2};
    check(oneType < twoTypes, "specificity: fewer types is lower");
    check(!(twoTypes < oneType), "specificity: strict, not a total tie");
  }

  // --- 2. source-order tiebreak between equally specific rules ----------

  {
    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet("rect { fill: red; } rect { fill: blue; }",
                             &sheet, &refusals),
         "source-order: two same-specificity rules parse");
    const SvgElementView e = elem("rect", "", {}, nullptr);
    const auto style =
        svgComputeStyle(e, noDecls, noDecls, sheet, noInherited);
    check(style.at("fill") == "blue",
         "source-order: later same-specificity rule wins");
  }

  // --- 3. !important beats a more specific ordinary rule -----------------

  {
    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet(
              "rect { fill: red !important; } #target { fill: blue; }",
              &sheet, &refusals),
         "important: low- and high-specificity rules parse");
    const SvgElementView e = elem("rect", "target", {}, nullptr);
    const auto style =
        svgComputeStyle(e, noDecls, noDecls, sheet, noInherited);
    check(style.at("fill") == "red",
         "important: type selector + !important beats a plain id selector");
  }

  // --- 4. a matching sheet rule beats a presentation attribute -----------
  // The counter-intuitive one: SVG 1.1 section 6.4 ranks presentation
  // attributes as specificity-zero author rules that precede the sheet, so
  // ANY matching sheet rule outranks them, however low its own specificity.

  {
    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet("rect { fill: blue; }", &sheet, &refusals),
         "presentation-vs-sheet: bare type-selector sheet rule parses");
    const std::vector<SvgDeclaration> presentation = {decl("fill", "red")};
    const SvgElementView e = elem("rect", "", {}, nullptr);
    const auto style =
        svgComputeStyle(e, presentation, noDecls, sheet, noInherited);
    check(style.at("fill") == "blue",
         "presentation-vs-sheet: matching sheet rule beats presentation attribute");
  }

  // --- 5. style="" beats a matching sheet rule ---------------------------

  {
    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet("rect { fill: blue; }", &sheet, &refusals),
         "inline-vs-sheet: sheet rule parses");
    const std::vector<SvgDeclaration> inlineStyle = {decl("fill", "green")};
    const SvgElementView e = elem("rect", "", {}, nullptr);
    const auto style =
        svgComputeStyle(e, noDecls, inlineStyle, sheet, noInherited);
    check(style.at("fill") == "green",
         "inline-vs-sheet: style=\"\" beats a matching sheet rule");
  }

  // --- 6. inheritance: an inheriting property flows, a non-inheriting one
  //        does not -- driven the way a real caller would drive it -----

  {
    const SvgElementView parentElem = elem("g", "", {}, nullptr);
    const std::vector<SvgDeclaration> parentPresentation = {
        decl("fill", "red"), decl("opacity", "0.5")};
    const auto parentStyle = svgComputeStyle(parentElem, parentPresentation,
                                             noDecls, emptySheet, noInherited);
    check(parentStyle.at("fill") == "red" && parentStyle.at("opacity") == "0.5",
         "inheritance: parent's own computed style holds both properties");

    std::map<std::string, std::string> forChild;
    for (const auto& [property, value] : parentStyle) {
      if (svgPropertyInherits(property)) forChild[property] = value;
    }
    check(forChild.count("fill") == 1 && forChild.count("opacity") == 0,
         "inheritance: caller's filter keeps fill, drops opacity");

    const SvgElementView childElem = elem("rect", "", {}, &parentElem);
    const auto childStyle =
        svgComputeStyle(childElem, noDecls, noDecls, emptySheet, forChild);
    check(childStyle.count("fill") == 1 && childStyle.at("fill") == "red",
         "inheritance: fill (inheriting) reaches the child");
    check(childStyle.count("opacity") == 0,
         "inheritance: opacity (non-inheriting) does not reach the child");
  }

  // --- 7. the literal `inherit` keyword ----------------------------------

  {
    // `opacity` does not normally inherit, but the literal keyword forces
    // cascade step 1 for it anyway.
    check(!svgPropertyInherits("opacity"),
         "inherit-keyword: opacity is not in the inheriting list");
    const std::map<std::string, std::string> inherited = {{"opacity", "0.3"}};
    const std::vector<SvgDeclaration> presentation = {decl("opacity", "inherit")};
    const SvgElementView e = elem("rect", "", {}, nullptr);
    const auto style =
        svgComputeStyle(e, presentation, noDecls, emptySheet, inherited);
    check(style.at("opacity") == "0.3",
         "inherit-keyword: literal 'inherit' pulls the parent's value even for "
         "a non-inheriting property");

    // No parent value to inherit: the property is dropped, not left as the
    // unparseable text "inherit".
    const std::vector<SvgDeclaration> presentationNoParent = {
        decl("opacity", "inherit")};
    const auto styleNoParent = svgComputeStyle(e, presentationNoParent, noDecls,
                                               emptySheet, noInherited);
    check(styleNoParent.count("opacity") == 0,
         "inherit-keyword: with nothing to inherit, the property is dropped");
  }

  // --- 8. made-up property defaults to not inheriting --------------------

  check(!svgPropertyInherits("np-made-up-property"),
       "unknown-property: an unrecognised property does not inherit");

  // --- 9. each supported selector form, matching and not matching --------

  {
    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet(
              "rect { fill: t1; } "
              ".foo { fill: t2; } "
              "#bar { fill: t3; } "
              "* { stroke: universal; } "
              "rect.foo#bar { stroke-width: compound; }",
              &sheet, &refusals),
         "selector forms: sheet with type/class/id/universal/compound parses");

    const SvgElementView matchAll =
        elem("rect", "bar", {"foo"}, nullptr);  // matches every rule above
    const auto styleAll =
        svgComputeStyle(matchAll, noDecls, noDecls, sheet, noInherited);
    check(styleAll.at("fill") == "t3",
         "selector forms: type, class and id selectors all matched (id wins "
         "on specificity)");
    check(styleAll.at("stroke") == "universal",
         "selector forms: universal selector matched");
    check(styleAll.at("stroke-width") == "compound",
         "selector forms: compound selector rect.foo#bar matched in full");

    const SvgElementView wrongTag = elem("circle", "bar", {"foo"}, nullptr);
    const auto styleWrongTag =
        svgComputeStyle(wrongTag, noDecls, noDecls, sheet, noInherited);
    check(styleWrongTag.count("fill") == 1 && styleWrongTag.at("fill") == "t3",
         "selector forms: type selector does not match a different tag "
         "(class and id selectors still do, id winning on specificity)");
    check(styleWrongTag.count("stroke-width") == 0,
         "selector forms: compound selector needs its type to match too");

    const SvgElementView noClass = elem("rect", "bar", {}, nullptr);
    const auto styleNoClass =
        svgComputeStyle(noClass, noDecls, noDecls, sheet, noInherited);
    check(styleNoClass.count("fill") == 1 && styleNoClass.at("fill") == "t3",
         "selector forms: class selector does not match without the class");
    check(styleNoClass.count("stroke-width") == 0,
         "selector forms: compound selector needs its class too");

    const SvgElementView noId = elem("rect", "", {"foo"}, nullptr);
    const auto styleNoId =
        svgComputeStyle(noId, noDecls, noDecls, sheet, noInherited);
    check(styleNoId.count("fill") == 1 && styleNoId.at("fill") == "t2",
         "selector forms: id selector does not match without the id");
  }

  // --- 10. selector lists (comma) -----------------------------------------

  {
    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet("circle, rect { fill: listed; }", &sheet,
                             &refusals),
         "selector list: 'circle, rect' parses as two alternatives");
    const SvgElementView r = elem("rect", "", {}, nullptr);
    const SvgElementView c = elem("circle", "", {}, nullptr);
    const SvgElementView p = elem("path", "", {}, nullptr);
    check(svgComputeStyle(r, noDecls, noDecls, sheet, noInherited)
              .at("fill") == "listed",
         "selector list: matches the first alternative");
    check(svgComputeStyle(c, noDecls, noDecls, sheet, noInherited)
              .at("fill") == "listed",
         "selector list: matches the second alternative");
    check(svgComputeStyle(p, noDecls, noDecls, sheet, noInherited)
              .count("fill") == 0,
         "selector list: matches neither for an unlisted tag");
  }

  // --- 11. descendant vs child combinator, over a two-level parent chain --

  {
    SvgStyleSheet childSheet;
    SvgStyleSheet descendantSheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet("g > rect { fill: child; }", &childSheet,
                             &refusals),
         "combinators: 'g > rect' parses");
    check(parseSvgStyleSheet("g rect { fill: descendant; }", &descendantSheet,
                             &refusals),
         "combinators: 'g rect' parses");

    const SvgElementView root = elem("g", "", {}, nullptr);
    const SvgElementView middle = elem("x", "", {}, &root);
    const SvgElementView grandchild = elem("rect", "", {}, &middle);
    const SvgElementView directChild = elem("rect", "", {}, &root);

    check(svgComputeStyle(grandchild, noDecls, noDecls, childSheet, noInherited)
              .count("fill") == 0,
         "combinators: child combinator does not reach through an "
         "intermediate element");
    check(svgComputeStyle(grandchild, noDecls, noDecls, descendantSheet,
                          noInherited)
              .at("fill") == "descendant",
         "combinators: descendant combinator does reach through an "
         "intermediate element");
    check(svgComputeStyle(directChild, noDecls, noDecls, childSheet,
                          noInherited)
              .at("fill") == "child",
         "combinators: child combinator matches a direct child");
  }

  // --- 12. refused constructs, named, rest of the sheet still parses -----

  {
    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet(
              "[href] { fill: red; } "
              ":hover { fill: red; } "
              "::before { fill: red; } "
              "a + b { fill: red; } "
              "a ~ b { fill: red; } "
              "@media screen { rect { fill: red; } } "
              "@import url(x.css); "
              "rect { fill: survives; }",
              &sheet, &refusals),
         "refusals: a sheet full of unsupported constructs still parses");

    auto anyRefusalContains = [&](const char* needle) {
      for (const auto& r : refusals) {
        if (r.find(needle) != std::string::npos) return true;
      }
      return false;
    };
    check(anyRefusalContains("attribute selector"),
         "refusals: attribute selector named");
    check(anyRefusalContains("pseudo-class"),
         "refusals: pseudo-class named");
    check(anyRefusalContains("pseudo-element") ||
             anyRefusalContains("pseudo-class"),
         "refusals: pseudo-element named (shares the pseudo message)");
    check(anyRefusalContains("sibling combinator"),
         "refusals: sibling combinators ('+' and '~') named");
    check(anyRefusalContains("@media"), "refusals: @media at-rule named");
    check(anyRefusalContains("@import"), "refusals: @import at-rule named");

    const SvgElementView r = elem("rect", "", {}, nullptr);
    const auto style =
        svgComputeStyle(r, noDecls, noDecls, sheet, noInherited);
    check(style.count("fill") == 1 && style.at("fill") == "survives",
         "refusals: the rest of the sheet parsed and matched despite every "
         "earlier rule being refused");
  }

  // --- 13. CSS comments, including between a selector and its '{' --------

  {
    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet(
              "/* leading */ rect /* between selector and brace */ { "
              "fill: /* between colon and value */ red; /* trailing */ }",
              &sheet, &refusals),
         "comments: sheet with comments in four positions parses");
    check(refusals.empty(), "comments: none of them are a refusal");
    const SvgElementView r = elem("rect", "", {}, nullptr);
    const auto style =
        svgComputeStyle(r, noDecls, noDecls, sheet, noInherited);
    check(style.count("fill") == 1 && style.at("fill") == "red",
         "comments: stripped correctly on every side, selector still matches");
  }

  // --- 14. malformed input: does not crash, states a result --------------

  {
    std::vector<SvgDeclaration> decls;
    check(parseSvgInlineStyle("fill red; stroke: none;", &decls),
         "malformed: declaration with no colon does not crash the parser");
    check(decls.size() == 1 && decls[0].property == "stroke",
         "malformed: the colon-less chunk is dropped, the valid one kept");
  }
  {
    std::vector<SvgDeclaration> decls;
    check(parseSvgInlineStyle("content: \"unterminated string; more text",
                              &decls),
         "malformed: unterminated quoted value does not hang");
    check(decls.size() == 1 && decls[0].property == "content",
         "malformed: the unterminated value is still captured as one "
         "declaration");
  }
  {
    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet("rect { fill: red", &sheet, &refusals),
         "malformed: '{' with no matching '}' does not hang");
    check(sheet.rules.empty(),
         "malformed: the unterminated rule is not kept as a partial rule");
    bool sawUnmatchedBrace = false;
    for (const auto& r : refusals) {
      if (r.find("no matching '}'") != std::string::npos) sawUnmatchedBrace = true;
    }
    check(sawUnmatchedBrace, "malformed: the unmatched brace is named");
  }
  {
    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet(" { fill: red; }", &sheet, &refusals),
         "malformed: a rule with an empty selector does not crash");
    check(sheet.rules.empty(), "malformed: the empty-selector rule is dropped");
    bool sawEmpty = false;
    for (const auto& r : refusals) {
      if (r.find("empty selector") != std::string::npos) sawEmpty = true;
    }
    check(sawEmpty, "malformed: the empty selector is named");
  }
  {
    // Braces nested inside a declaration block: the nested '{'/'}' must not
    // end the rule early, and parsing must recover for the next rule.
    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet(
              "rect { fill: red; { bogus: 1; } } circle { fill: blue; }",
              &sheet, &refusals),
         "malformed: nested braces inside a declaration block do not crash");
    check(sheet.rules.size() == 2,
         "malformed: both rules parsed despite the nested braces in the first");
    const SvgElementView r = elem("rect", "", {}, nullptr);
    const SvgElementView c = elem("circle", "", {}, nullptr);
    check(svgComputeStyle(r, noDecls, noDecls, sheet, noInherited)
              .at("fill") == "red",
         "malformed: the rule with nested braces still yields its own "
         "declaration");
    check(svgComputeStyle(c, noDecls, noDecls, sheet, noInherited)
              .at("fill") == "blue",
         "malformed: the following rule parsed correctly, undisturbed");
  }
  {
    // An unterminated comment: bounded (consumes to end of input), and
    // whatever was parsed before it is kept.
    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet("rect { fill: red; } /* never closed",
                             &sheet, &refusals),
         "malformed: unterminated comment does not hang");
    check(sheet.rules.size() == 1,
         "malformed: the rule before the unterminated comment is kept");
    bool sawUnterminatedComment = false;
    for (const auto& r : refusals) {
      if (r.find("unterminated comment") != std::string::npos) {
        sawUnterminatedComment = true;
      }
    }
    check(sawUnterminatedComment, "malformed: the unterminated comment is named");
  }
  {
    // A dangling or leading combinator: neither is a rule this module can
    // express, so both refuse rather than guess a meaning.
    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet("> rect { fill: red; } g > { fill: red; }",
                             &sheet, &refusals),
         "malformed: leading and dangling combinators do not crash");
    check(sheet.rules.empty(),
         "malformed: neither malformed combinator produced a rule");
  }
  {
    // 10 000 rules: bounded work, no crash, no hang, well under the cap.
    std::string bigCss;
    bigCss.reserve(10000 * 16);
    for (int i = 0; i < 10000; ++i) bigCss += "a{fill:red}";
    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet(bigCss, &sheet, &refusals),
         "malformed: a sheet of 10 000 rules parses");
    check(sheet.rules.size() == 10000,
         "malformed: all 10 000 rules were kept (under the cap)");
  }
  {
    // ---------------------------------------------------------------------
    // A deep tree against a many-combinator selector: bounded, not
    // exponential
    // ---------------------------------------------------------------------
    //
    // `matchChain()`'s backtracking blows up without its failure memo, and
    // both the selector text and the document depth come from a file this
    // build did not write -- so this is the assertion that pins the memo.
    //
    // **Getting the shape right took a sabotage.** The first version of this
    // case ended the selector in `circle` against a `rect` leaf, and passed
    // with the memo deliberately disabled: `matchChain()` tests the RIGHTMOST
    // compound first, so it rejected on the leaf and never recursed once. The
    // search only explores when the right-hand end MATCHES and the failure is
    // at the far left.
    //
    // The cost is also not `depth ^ combinators`. Ancestors strictly decrease,
    // so a k-combinator selector picks an ordered subset of the chain:
    // C(depth, k), which peaks at k = depth/2. Hence 30 levels and 15 `g`
    // combinators -- C(30, 15) = 155 117 520 explorations unmemoised, a
    // second or two, against 30 x 17 = 510 memoised states. Deliberately sized
    // to be SLOW rather than hung when the memo is removed, so the sabotage
    // stays practical to re-run.
    constexpr size_t kDepth = 30;
    constexpr int kCombinators = 15;
    std::vector<SvgElementView> chain(kDepth);
    for (size_t i = 0; i < kDepth; ++i) {
      chain[i].tag = "g";
      chain[i].parent = (i == 0) ? nullptr : &chain[i - 1];
    }
    // The leaf matches the selector's rightmost compound, so the walk starts.
    SvgElementView leaf;
    leaf.tag = "rect";
    leaf.parent = &chain[kDepth - 1];

    // "nomatch g g ... g rect": every `g` matches somewhere, and `nomatch`
    // matches nothing, so every ordered choice of ancestors is tried and all
    // of them fail.
    std::string css = "nomatch";
    for (int i = 0; i < kCombinators; ++i) css += " g";
    css += " rect { fill: red; }";

    SvgStyleSheet sheet;
    std::vector<std::string> refusals;
    check(parseSvgStyleSheet(css, &sheet, &refusals),
         "deep tree: the 15-combinator selector parses");
    check(sheet.rules.size() == 1,
         "deep tree: and produced exactly one rule to match against");

    const auto t0 = std::chrono::steady_clock::now();
    const std::map<std::string, std::string> computed =
        svgComputeStyle(leaf, {}, {}, sheet, {});
    const double ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0)
            .count();

    check(computed.find("fill") == computed.end(),
         "deep tree: the non-matching selector does not apply its declaration");
    std::printf("    [measured] 30-deep chain x 15 descendant combinators: %.3f ms\n",
                ms);
    check(ms < 100.0,
         "deep tree: matching is bounded, not exponential (memo is present)");
  }

  return ok;
}

}  // namespace np
