#include "app/selftest/Support.hpp"

#include "app/AppState.hpp"
#include "color/Space.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierLayout.hpp"
#include "ui/AtelierTheme.hpp"

namespace np {

// The Atelier chrome: docs/ui.md sections 1 and 2, checked against the
// document rather than against a screenshot.
//
// **Why this section exists at all.** A design token table and a dimensioned
// layout diagram are exactly the kind of specification that rots without
// anyone noticing: nothing fails when `#2d2b2b` drifts to `#2e2c2c`, and
// nothing fails when a band is 40 px instead of 36. The outgoing chrome is the
// proof -- it had a 78 px palette, a 300 px column, a 62 px strip the design
// does not contain, and a wet-ink cyan accent, and every one of those was
// invented in place while docs/ui.md sat in the repository saying otherwise.
//
// So the tokens are compared to the hex in the table by exact integer equality
// and the bands are compared to the numbers in the diagram, and the layout is
// additionally checked for the property the diagram asserts implicitly and
// which is the one worth having a test for: **the regions tile the window** --
// every pixel belongs to exactly one band or one rule, with no gap and no
// overlap. A layout can satisfy every named dimension and still leave a seam.
//
// Headless and GPU-free: ui/AtelierLayout and the top half of
// ui/AtelierChrome are free of ImGui precisely so this can run without a
// window (ui/AtelierTheme.hpp's own comment says why the tokens are integers).
bool runAtelierChromeTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- Part A: the tokens (docs/ui.md section 1) --------------------------
  {
    bool tokensOk = true;
    const struct { uint32_t got; uint32_t want; const char* role; } kTable[] = {
        {kChromeBase,    0x2d2b2bu, "chrome base"},
        {kChromeDeep,    0x201e1du, "chrome deep"},
        {kChromeMid,     0x444141u, "chrome mid"},
        {kRule,          0xf3f2f2u, "rule"},
        {kDivider,       0x444141u, "divider"},
        {kHairline,      0x9b9797u, "hairline"},
        {kTextPrimary,   0xf3f2f2u, "text primary"},
        {kTextSecondary, 0x9b9797u, "text secondary"},
        {kAccent,        0xff563cu, "accent"},
        {kRowSelected,   0x7c1405u, "row selected"},
        {kCanvasPaper,   0xf8f4f4u, "canvas paper"},
        {kOnAccent,      0x201e1du, "on-accent foreground"},
    };
    for (const auto& row : kTable)
      if (row.got != row.want) {
        tokensOk = false;
        std::printf("    %-22s is #%06x, docs/ui.md says #%06x\n", row.role, row.got, row.want);
      }
    check(tokensOk, "all twelve tokens match docs/ui.md section 1's table");
    check(sizeof(kTable) / sizeof(kTable[0]) == 12, "the table has twelve rows, as the doc does");
  }

  check(kRuleThickness == 2.0f && kDividerThickness == 1.0f,
        "2px between major regions, 1px internally");

  // The accent brightened from the light revision's #ec3013 -- docs/ui.md
  // calls that "correct practice, since the same hue needs more luminance to
  // hold up against dark chrome". Asserted as luminance rather than as the old
  // literal, so it is the reasoning that is pinned and not the history.
  //
  // The tokens are sRGB-encoded -- they are chrome, drawn straight into the
  // swapchain -- so every luminance below decodes first, through this
  // project's own `color::srgbDecode()`. Comparing the encoded bytes would be
  // the mistake the whole colour library exists to prevent, and it is a
  // mistake that *passes*: on the encoded values #bab6b6 looks like 0.72 and
  // would fail a mid-grey test it should pass by a wide margin.
  const auto relLuminance = [](uint32_t rgb) {
    float c[3];
    unpackRgb(rgb, c);
    return 0.2126f * srgbDecode(c[0]) + 0.7152f * srgbDecode(c[1]) + 0.0722f * srgbDecode(c[2]);
  };
  // WCAG's contrast ratio, which is what "reads against" means as a number.
  const auto contrast = [&](uint32_t a, uint32_t b) {
    const float la = relLuminance(a), lb = relLuminance(b);
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
  };

  {
    check(relLuminance(kAccent) > relLuminance(0xec3013u),
          "the dark-chrome accent is brighter than the light one");
    // 3:1 is WCAG's threshold for a UI component against its background, which
    // is exactly what the accent is: it marks the active tool and the dirty
    // document, and neither is body text.
    std::printf("  [measured] accent on chrome %.2f:1, text on chrome %.2f:1\n",
                contrast(kAccent, kChromeBase), contrast(kTextPrimary, kChromeBase));
    check(contrast(kAccent, kChromeBase) >= 3.0f, "the accent reads against the chrome it marks");
    check(contrast(kTextPrimary, kChromeBase) >= 7.0f,
          "and primary text clears WCAG AAA against the same chrome");
    check(contrast(kTextSecondary, kChromeBase) >= 4.5f,
          "secondary text clears AA, which is what a sub-line has to");
  }

  // --- Part B: the surround is NOT the chrome (PRD L6) ---------------------
  //
  // docs/ui.md's warning callout is the only place in the document that
  // *disagrees* with the wireframe it is describing, so it is the one most
  // likely to be quietly undone by someone matching the design more
  // faithfully. This asserts the disagreement.
  {
    check(kCanvasSurroundDefault == 0xbab6b6u, "the surround defaults to the light revision's mid-grey");
    check(kCanvasSurroundDefault != kChromeBase,
          "the surround is a separate value from the chrome, which is the whole callout");
    const float sl = relLuminance(kCanvasSurroundDefault);
    const float cl = relLuminance(kChromeBase);
    std::printf("  [measured] surround luminance %.3f against chrome %.3f -- the wireframe's\n"
                "             surround reflects %.0fx less light than the one it is judged on\n",
                sl, cl, sl / cl);
    check(sl > 0.35f && sl < 0.65f, "mid-grey, which is what a colour judgement needs");

    // And it is *adjustable*, which is the requirement (L6 is "user-adjustable",
    // not "a lighter constant"). Restored afterwards so the section leaves no
    // state behind -- this is process-global.
    const uint32_t before = atelierSurround();
    setAtelierSurround(0x123456u);
    const bool moved = atelierSurround() == 0x123456u;
    setAtelierSurround(before);
    check(moved && atelierSurround() == kCanvasSurroundDefault, "the surround is settable");
  }

  // --- Part C: the bands tile the window (docs/ui.md section 2) -----------
  {
    // The design's own window size. Not a tablet target -- docs/ui.md opens by
    // saying so -- but it is the size the diagram is dimensioned at, which
    // makes it the size whose numbers can be read straight off the document.
    constexpr float kW = 1366.0f, kH = 1024.0f;

    const auto tiles = [&](const AtelierBands& b, float w, float h, const char* what) {
      AtelierRect all[7 + AtelierBands::kMaxRules];
      size_t n = 0;
      all[n++] = b.titleBar;
      all[n++] = b.tabStrip;
      all[n++] = b.optionsBar;
      all[n++] = b.toolPalette;
      all[n++] = b.canvas;
      all[n++] = b.rightColumn;
      all[n++] = b.statusBar;
      for (size_t i = 0; i < b.ruleCount; ++i) all[n++] = b.rules[i];

      double area = 0.0;
      bool overlap = false;
      for (size_t i = 0; i < n; ++i) {
        if (all[i].empty()) continue;
        area += static_cast<double>(all[i].w) * all[i].h;
        for (size_t j = i + 1; j < n; ++j) {
          if (all[j].empty()) continue;
          const float x0 = std::max(all[i].x, all[j].x);
          const float y0 = std::max(all[i].y, all[j].y);
          const float x1 = std::min(all[i].right(), all[j].right());
          const float y1 = std::min(all[i].bottom(), all[j].bottom());
          if (x1 - x0 > 0.01f && y1 - y0 > 0.01f) overlap = true;
        }
      }
      const double want = static_cast<double>(w) * h;
      std::printf("  %s: %zu regions, %.0f of %.0f px^2 covered, %s\n", what, n, area, want,
                  overlap ? "OVERLAPPING" : "disjoint");
      return !overlap && std::fabs(area - want) < 1.0;
    };

    const AtelierBands b = atelierLayout(0.0f, 0.0f, kW, kH, /*showTabStrip=*/false);
    check(tiles(b, kW, kH, "no tab strip"), "the bands and rules tile the window exactly");

    check(b.titleBar.h == kTitleBarH && kTitleBarH == 36.0f, "title bar is 36 px");
    check(b.optionsBar.h == kOptionsBarH && kOptionsBarH == 46.0f, "options bar is 46 px");
    check(b.statusBar.h == kStatusBarH && kStatusBarH == 26.0f, "status bar is 26 px");
    check(b.toolPalette.w == 104.0f, "the tool palette is 104 px wide");
    check(b.rightColumn.w == 322.0f, "the right column is 322 px wide");
    check(b.statusBar.bottom() == kH, "the status bar sits flush with the bottom edge");
    check(b.rightColumn.right() == kW, "the right column sits flush with the right edge");

    // The canvas is the remainder, and this is the arithmetic spelled out:
    // three 2px rules vertically (title, options, status) and two horizontally.
    check(b.canvas.w == kW - 104.0f - 322.0f - 2.0f * kRuleThickness,
          "the canvas is what the two columns and their rules leave");
    check(b.canvas.h == kH - kTitleBarH - kOptionsBarH - kStatusBarH - 3.0f * kRuleThickness,
          "and what the three horizontal bands and their rules leave");
    std::printf("  [measured] at 1366x1024 the canvas gets %.0f x %.0f of %.0f x %.0f\n",
                b.canvas.w, b.canvas.h, kW, kH);

    // The tab strip is a band of the layout even though nothing fills it yet
    // (PRD A5 / PLAN.md Phase 5 step 14). Switching it on has to keep the
    // tiling exact and cost the canvas exactly the strip plus its rule --
    // which is the property that makes step 14 a change to one bool.
    const AtelierBands withTabs = atelierLayout(0.0f, 0.0f, kW, kH, /*showTabStrip=*/true);
    check(tiles(withTabs, kW, kH, "with tab strip"), "and still tile with the tab strip on");
    check(withTabs.tabStrip.h == kTabStripH && kTabStripH == 34.0f, "the tab strip is 34 px");
    check(withTabs.canvas.h == b.canvas.h - kTabStripH - kRuleThickness,
          "turning it on costs the canvas exactly the strip and one rule");
    check(b.ruleCount == 5 && withTabs.ruleCount == 6,
          "a suppressed band suppresses exactly one rule");

    // Non-square and odd sizes, because the real window is neither: the design
    // size is the only one whose numbers are quotable and the only one a
    // hand-checked layout would ever be tried at.
    const float kSizes[][2] = {{1280.0f, 790.0f}, {2560.0f, 1580.0f}, {1367.0f, 1025.0f},
                              {900.0f, 600.0f}};
    bool allTile = true;
    for (const auto& wh : kSizes) {
      const AtelierBands t = atelierLayout(7.0f, 3.0f, wh[0], wh[1], false);
      if (!tiles(t, wh[0], wh[1], "odd size")) allTile = false;
      if (t.titleBar.x != 7.0f || t.titleBar.y != 3.0f) allTile = false;
    }
    check(allTile, "and tile at four other sizes, from a non-zero origin");

    // Undersized: the bands keep their heights and the canvas goes to zero
    // rather than negative. ui/AtelierLayout.hpp argues for this -- chrome
    // that shrinks lies about its hit targets.
    const AtelierBands tiny = atelierLayout(0.0f, 0.0f, 200.0f, 80.0f, false);
    check(tiny.canvas.w == 0.0f && tiny.canvas.h == 0.0f,
          "a window too small for the design loses the canvas, not the chrome");
    check(tiny.titleBar.h == kTitleBarH && tiny.statusBar.h == kStatusBarH,
          "and the bands keep the sizes their hit targets are sized for");
  }

  // --- Part D: what the status bar says ------------------------------------
  {
    Document doc = Document::createBlank(64, 64, WorkingSpace{});
    check(std::string(workingSpaceLabel(doc)) == "LIN16",
          "PRD L1: the chrome reports the working space, not a bit depth");

    const ResidentReading mem = atelierResident();
    check(mem.budget == 512u * 1024u * 1024u, "the budget is the design's 512 MB");
    check(mem.bytes > 0, "PRD L7: the numerator is real resident memory, not an estimate");
    std::printf("  [measured] resident %.1f MB of a %.0f MB budget\n",
                static_cast<double>(mem.bytes) / (1024.0 * 1024.0),
                static_cast<double>(mem.budget) / (1024.0 * 1024.0));

    // docs/ui.md section 5's three toggles, and the rule that a clean view
    // shows nothing. Every combination, because the document's own argument is
    // about *combinations* -- "both on looks like a deliberate composition".
    CanvasView v;
    check(atelierViewStateMarkers(v).empty(), "a clean view puts no markers in the status bar");
    v.mirrorX = true;
    check(atelierViewStateMarkers(v) == "MIRROR L/R", "one mirror axis is named by its axis");
    v.mirrorY = true;
    check(atelierViewStateMarkers(v) == "MIRROR L/R  MIRROR U/D",
          "and two are two markers, not one that says MIRROR");
    v.grayscale = true;
    check(atelierViewStateMarkers(v) == "MIRROR L/R  MIRROR U/D  GRAYSCALE",
          "grayscale joins them, in the order the document lists them");
    v = CanvasView{};
    v.grayscale = true;
    check(atelierViewStateMarkers(v) == "GRAYSCALE",
          "and grayscale alone is grayscale alone -- a user who forgets it mixes colour blind");
  }

  // --- Part E: the tool names ---------------------------------------------
  {
    bool namesOk = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const std::string a = toolName(static_cast<Tool>(i));
      if (a.empty() || a == "?") namesOk = false;
      for (int j = i + 1; j < static_cast<int>(Tool::Count); ++j)
        if (a == toolName(static_cast<Tool>(j))) namesOk = false;
    }
    check(namesOk, "every tool has a distinct non-empty name");
    // The tripwire that makes the walk above complete rather than merely long,
    // the same shape app/selftest/Fonts.cpp uses for LayerKind.
    check(std::string(toolName(static_cast<Tool>(6))) == "?",
          "Tool still has exactly 6 values, so the walk above covers all of them");
  }

  std::printf("[selftest] atelier chrome %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
