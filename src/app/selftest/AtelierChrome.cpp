#include "app/selftest/Support.hpp"

#include "app/AppState.hpp"
#include "color/Space.hpp"
#include "imgui.h"
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
    // 52 px, not the outgoing chrome's 104, not the 64 that fixed the
    // clipping bug, and not the 44 that replaced it once cells started
    // shrinking to fit instead of scrolling. This is the fourth number:
    // "nest similar tools into a flyout to conserve space like photoshop"
    // (the user's own words) collapsed the palette from 28 slots (27
    // `Tool`s + "More") to 18 (17 Photoshop-style groups + "More" --
    // ui/AtelierChrome.hpp's `kToolGroups`, proven complete by Part F2
    // below), which bought back enough room to raise `kToolCellMax` from
    // 28 to 36 -- this file's very first revision's number -- instead of
    // spending the savings on an even smaller floor. `kToolPaletteW` needs
    // room for exactly one cell at its largest (kToolCellMax = 36) plus
    // WindowPadding on both sides (8 * 2 = 16) and nothing else: 36 + 16 =
    // 52. Checked against the *named constant* rather than a second copy
    // of the literal -- docs/ui.md section 2 is where 52 is recorded as
    // the number this build chose.
    check(b.toolPalette.w == kToolPaletteW && kToolPaletteW == 52.0f,
          "the tool palette is 52 px wide (a single column, docs/ui.md section 2)");
    check(b.rightColumn.w == 322.0f, "the right column is 322 px wide");
    check(b.statusBar.bottom() == kH, "the status bar sits flush with the bottom edge");
    check(b.rightColumn.right() == kW, "the right column sits flush with the right edge");

    // The canvas is the remainder, and this is the arithmetic spelled out:
    // three 2px rules vertically (title, options, status) and two horizontally.
    check(b.canvas.w == kW - kToolPaletteW - 322.0f - 2.0f * kRuleThickness,
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

  // --- Part C2: the navigator ---------------------------------------------
  {
    const AtelierBands b = atelierLayout(0.0f, 0.0f, 1366.0f, 1024.0f, false);

    // A square document in a landscape box fits by height, and the box keeps
    // the document's aspect rather than the box's -- a navigator that lies
    // about the shape of the paper is worse than no navigator.
    const AtelierRect sq = atelierNavigatorRect(b.canvas, 1024.0f, 1024.0f);
    check(!sq.empty() && std::fabs(sq.w - sq.h) < 0.01f && sq.h == kNavigatorMaxH,
          "a square document fits the navigator by its limiting axis");
    const AtelierRect wide = atelierNavigatorRect(b.canvas, 4000.0f, 1000.0f);
    check(std::fabs(wide.w / wide.h - 4.0f) < 0.01f && wide.w == kNavigatorMaxW,
          "and a 4:1 document stays 4:1");

    check(std::fabs(sq.right() - (b.canvas.right() - kNavigatorInset)) < 0.01f &&
              std::fabs(sq.bottom() - (b.canvas.bottom() - kNavigatorInset)) < 0.01f,
          "inset from the canvas region's bottom-right corner");

    // It hides itself rather than covering the picture it navigates.
    check(atelierNavigatorRect(AtelierRect{0, 0, 300, 200}, 1024.0f, 1024.0f).empty(),
          "a canvas too small to spare the corner gets no navigator");
    check(atelierNavigatorRect(b.canvas, 0.0f, 0.0f).empty(),
          "and a document with no area gets none either");

    // The viewport indicator. At 100% on a 936 x 910 canvas, a 1024 x 1024
    // document is wider and taller than the view, so the marked region is a
    // proper sub-rect; zoomed far out it covers the whole box and is clamped
    // there rather than drawn outside it.
    const AtelierRect part = atelierNavigatorMap(sq, 1024.0f, 1024.0f, 0.0f, 0.0f, 512.0f, 512.0f);
    check(std::fabs(part.w - sq.w * 0.5f) < 0.01f && std::fabs(part.h - sq.h * 0.5f) < 0.01f,
          "half the document marks half the navigator");
    const AtelierRect over =
        atelierNavigatorMap(sq, 1024.0f, 1024.0f, -800.0f, -800.0f, 1800.0f, 1800.0f);
    check(std::fabs(over.w - sq.w) < 0.01f && std::fabs(over.h - sq.h) < 0.01f &&
              over.x == sq.x && over.y == sq.y,
          "a view larger than the paper clamps to the box, never outside it");
    const AtelierRect flipped =
        atelierNavigatorMap(sq, 1024.0f, 1024.0f, 700.0f, 700.0f, 200.0f, 200.0f);
    check(std::fabs(flipped.w - sq.w * (500.0f / 1024.0f)) < 0.01f,
          "and a mirrored view's reversed corners still give a positive rect");
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
    // the same shape app/selftest/Fonts.cpp uses for LayerKind. 28 now, from
    // 27, from an original 7: the palette rebuild added twenty slot-only
    // cells, and PRD E3's elliptical marquee then added one more. It is a
    // separate Tool value rather than a mode on Marquee because
    // docs/shortcuts.md reserves `M` for "rectangle | ellipse" and a flyout
    // member IS a Tool value (ui/AtelierChrome's kToolGroups).
    check(std::string(toolName(static_cast<Tool>(28))) == "?",
          "Tool still has exactly 28 values, so the walk above covers all of them");
  }

  // --- Part F: the tool palette's icons ------------------------------------
  {
    // Exactly the tools this build actually acts with -- listed here as its
    // own table, independent of ui/AtelierChrome.cpp's kToolMeta, so a row in
    // that table getting its `implemented` flag wrong is something this check
    // can actually catch rather than agreeing with itself.
    //
    // Eleven now, from seven: PRD E3's remaining selection tools landed
    // together, because they all end in the same place -- a Selection built,
    // combined through the ⇧/⌥ modifiers (PRD E7) and installed. Note that
    // "implemented" means the tool DOES something, not that its op exists:
    // ops/Gradient and ops/FloodFill's bucket half are both built and tested,
    // and both stay false here until a drag reaches them.
    const Tool kImplementedTools[] = {Tool::Brush,      Tool::Water,
                                      Tool::DryBrush,   Tool::Eyedropper,
                                      Tool::Marquee,    Tool::EllipseMarquee,
                                      Tool::Lasso,      Tool::PolygonLasso,
                                      Tool::MagicWand,  Tool::Hand,
                                      Tool::Zoom};
    bool implementedOk = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      const bool shouldBe =
          std::find(std::begin(kImplementedTools), std::end(kImplementedTools), t) !=
          std::end(kImplementedTools);
      if (toolImplemented(t) != shouldBe) implementedOk = false;
    }
    check(implementedOk,
          "toolImplemented() is true for exactly the eleven tools with real behaviour");

    // Every tool has an icon, and toolIconCodepoints() is the deduplicated,
    // sorted union of all of them plus the "More" cell's own ellipsis --
    // walked the same way requiredUiCodepoints() walks LayerKind, so a tool
    // whose codepoint is missing from the merge list is a bug this section
    // finds instead of a screenshot finding it.
    bool everyIconListed = true;
    const std::vector<uint32_t>& merged = toolIconCodepoints();
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      const uint32_t cp = toolIconCodepoint(t);
      if (cp == 0u ||
          std::find(merged.begin(), merged.end(), cp) == merged.end())
        everyIconListed = false;
    }
    check(everyIconListed && std::find(merged.begin(), merged.end(), kMoreIconCodepoint) != merged.end(),
          "every tool's icon codepoint, plus the More cell's, is in toolIconCodepoints()");

    // "Every name you use MUST exist in codepoints.json -- verify each one
    // programmatically, do not guess" -- this is that verification, run
    // against the vendored file itself rather than trusted from the table
    // that names it. Not a JSON parser: codepoints.json is a flat
    // `{"name": number, ...}` object (checked -- no nesting, no arrays, no
    // escaped quotes in a key), so a `"name":` substring search followed by
    // a decimal-digit scan is the whole of what reading it needs, the same
    // "hand-roll the small thing rather than take a dependency" call
    // app/Keymap.cpp's own comment makes for a schema one size up from this.
    std::FILE* f = std::fopen(NP_LUCIDE_CODEPOINTS_JSON, "rb");
    if (f == nullptr) {
      check(false, "third_party/lucide/codepoints.json opens (path: " NP_LUCIDE_CODEPOINTS_JSON ")");
    } else {
      std::fseek(f, 0, SEEK_END);
      const long size = std::ftell(f);
      std::fseek(f, 0, SEEK_SET);
      std::string json(static_cast<size_t>(size), '\0');
      const size_t got = std::fread(json.data(), 1, json.size(), f);
      std::fclose(f);
      check(got == json.size(), "codepoints.json read in full");

      const auto lookup = [&](const std::string& name) -> long {
        const std::string needle = "\"" + name + "\":";
        const size_t pos = json.find(needle);
        if (pos == std::string::npos) return -1;
        size_t p = pos + needle.size();
        while (p < json.size() && json[p] == ' ') ++p;
        size_t end = p;
        while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
        if (end == p) return -1;
        return std::stol(json.substr(p, end - p));
      };

      bool codepointsMatch = true;
      for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
        const Tool t = static_cast<Tool>(i);
        const long want = lookup(toolIconName(t));
        if (want < 0 || static_cast<uint32_t>(want) != toolIconCodepoint(t)) {
          codepointsMatch = false;
          std::printf("    %-20s codepoints.json says %ld, toolIconCodepoint() says %u\n",
                      toolIconName(t), want, toolIconCodepoint(t));
        }
      }
      const long moreWant = lookup("ellipsis");
      if (moreWant < 0 || static_cast<uint32_t>(moreWant) != kMoreIconCodepoint) codepointsMatch = false;
      check(codepointsMatch,
            "every icon name's codepoint matches third_party/lucide/codepoints.json exactly");
    }
  }

  // --- Part F2: the flyout groups are complete ------------------------------
  //
  // "nest similar tools into a flyout to conserve space like photoshop"
  // replaced one palette cell per `Tool` with one cell per group
  // (ui/AtelierChrome.hpp's `kToolGroups`), which trades away the
  // exhaustiveness `-Wswitch` used to buy for free: a `switch` over
  // `Tool::Count` cannot compile with a case missing, but a plain array of
  // groups can be missing a `Tool` and compile perfectly -- the tool would
  // just never appear in any flyout, silently unreachable from the
  // palette. This is the check that stands in for the compiler once the
  // exhaustiveness is a runtime table instead of a switch, walking
  // 0..Tool::Count the same way Part E's own distinct-name walk does.
  {
    // **Completeness**: every Tool value is in exactly one group, exactly
    // once -- not zero (unreachable from the palette), not two (ambiguous
    // which slot "owns" it, and toolGroupIndex() would silently return
    // whichever group's loop reached it first).
    bool completeOk = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      int occurrences = 0;
      for (int g = 0; g < kToolGroupCount; ++g)
        for (int m = 0; m < kToolGroups[g].memberCount; ++m)
          if (kToolGroups[g].members[m] == t) ++occurrences;
      if (occurrences != 1) {
        completeOk = false;
        std::printf("    %-16s appears in %d group(s), want exactly 1\n", toolName(t),
                    occurrences);
      }
      // toolGroupIndex() is the same fact through the function under test
      // rather than through this loop's own tally -- both have to agree,
      // or the function itself has a bug this tally alone would not catch.
      if (occurrences == 1 && toolGroupIndex(t) < 0) completeOk = false;
    }
    check(completeOk, "every Tool value is in exactly one ui/AtelierChrome.hpp kToolGroups slot");

    // Every member listed by every group actually exists as a real `Tool`
    // (catches a copy-paste that duplicated a member into the wrong slot's
    // array without the compiler noticing -- `Tool members[4]` accepts any
    // `Tool`, including one that belongs to a different group already).
    // Folded into the same completeness fact above: a member duplicated
    // across two groups already fails the `occurrences != 1` check, so
    // this loop is really re-deriving kToolGroupCount * memberCount ==
    // Tool::Count as an independent cross-check on the tally.
    int totalMembers = 0;
    for (int g = 0; g < kToolGroupCount; ++g) totalMembers += kToolGroups[g].memberCount;
    check(totalMembers == static_cast<int>(Tool::Count),
          "the groups' member counts sum to exactly Tool::Count (27), not more or fewer");

    // **Default member**: the first toolImplemented() member when a group
    // has one, else its first member -- Photoshop's own rule (a group
    // opens on whichever tool actually does something), checked against
    // an independent re-derivation rather than by calling
    // toolGroupDefaultMember() and trusting its own answer.
    bool defaultOk = true;
    for (int g = 0; g < kToolGroupCount; ++g) {
      const ToolGroup& group = kToolGroups[g];
      Tool want = group.members[0];
      for (int m = 0; m < group.memberCount; ++m) {
        if (toolImplemented(group.members[m])) {
          want = group.members[m];
          break;
        }
      }
      const Tool got = toolGroupDefaultMember(g);
      if (got != want) {
        defaultOk = false;
        std::printf("    group %d: toolGroupDefaultMember() = %s, want %s\n", g, toolName(got),
                    toolName(want));
      }
    }
    check(defaultOk,
          "toolGroupDefaultMember() is each group's first implemented member, or its first "
          "member when none of them are implemented yet");
  }

  // --- Part G: the palette's real, live content width ----------------------
  //
  // Part C's band-tiling arithmetic and ui/AtelierLayout.hpp's own
  // static_assert both check `kToolPaletteW` against a *hand-derived*
  // formula for what `ImGuiStyle::WindowPadding` leaves behind -- and a
  // formula agreeing with itself is not evidence that Dear ImGui's actual
  // layout code agrees with it too. That gap is exactly how the previous
  // revision of this file shipped a palette whose cells rendered clipped in
  // half: every number-only check passed, because none of them asked Dear
  // ImGui what `GetContentRegionAvail()` actually is.
  //
  // The question this section asks changed with the user's own correction
  // ("make the toolbar fit without scrolling ... the buttons are too
  // large"). It used to be "does a *scrolling* child still leave room for a
  // whole cell." Now that the grid child carries
  // `ImGuiWindowFlags_NoScrollbar` (ui/MacPaintUI.cpp) and shrinks its cells
  // to fit instead of scrolling in the steady state, the dangerous
  // regression is different: **a scrollbar silently coming back**, which
  // would eat `kScrollbarSize` of the content region this file's own
  // `kToolPaletteW` no longer budgets for, clipping every cell exactly the
  // way the original bug did. So this section forces the same 2000px
  // overflow the previous revision used -- not because the real grid is
  // expected to overflow (it should not, in the steady state), but because
  // forcing overflow is what would flip a scrollbar back on if the
  // `NoScrollbar` flag were ever accidentally dropped from that
  // `BeginChild()` call. A passing check here means: even when the content
  // overflows, `NoScrollbar` really does suppress the width reservation, so
  // `kToolPaletteW`'s 44 = 28 + 2*8 arithmetic is measuring what Dear ImGui
  // actually hands back, not just what the formula says it should.
  //
  // No swapchain and no real renderer backend -- layout (unlike
  // rasterisation) needs neither, and `app/selftest/Fonts.cpp`'s Part C/D
  // already establish that a headless `ImGuiContext` is fine for questions
  // this suite can ask without a window -- but `NewFrame()` is used here for
  // the first time in this suite, and it needs two things neither of those
  // sections needed: `ImGuiBackendFlags_RendererHasTextures` (told this
  // build's new dynamic font/texture system that *something* will
  // rasterise glyphs on demand, since nothing here ever will -- omitting it
  // is a hard `[imgui-error]`/SIGSEGV, found by running this section under
  // `script` rather than a plain redirect, because a redirected run's
  // stdout buffer never reached disk before the crash) and at least one
  // font actually added to the atlas (`AddFontDefault()` --
  // `installUiFonts()` is not called here on purpose, so this section's
  // answer is about the palette's geometry alone, not entangled with which
  // system font happened to load).
  {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    applyAtelierTheme();

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(400.0f, 400.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.Fonts->AddFontDefault();
    // main.cpp's own reason applies here too: "the layout is fixed; don't
    // persist window state" -- without this, a fresh ImGuiContext's default
    // ini path writes an `imgui.ini` into whatever the test binary's current
    // working directory happens to be, a side effect this section has no
    // business having.
    io.IniFilename = nullptr;

    // Two frames, not one, and the first measurement is thrown away -- kept
    // from the previous revision even though `NoScrollbar` makes the effect
    // it was guarding against impossible in the *passing* case. It still
    // matters for the *failing* case this section exists to catch: a child
    // window does not know it will overflow while its first frame's content
    // is still being submitted, so `GetContentRegionAvail()` mid-frame-1
    // reads as if no scrollbar exists regardless of whether one would
    // eventually appear. If `ImGuiWindowFlags_NoScrollbar` were ever
    // accidentally dropped from the real `BeginChild()` call, frame 1 of
    // *this* probe would misreport the width as fine even though frame 2
    // would not -- so throwing frame 1 away is what makes this section able
    // to notice that regression rather than being blind to it the same way
    // frame 1 is. `ui/MacPaintUI.cpp`'s real palette runs at 60+ fps, so it
    // is always well past frame 1 regardless.
    float contentWidth = -1.0f;
    for (int frame = 0; frame < 2; ++frame) {
      ImGui::NewFrame();
      ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
      ImGui::SetNextWindowSize(ImVec2(kToolPaletteW, 200.0f));
      if (ImGui::Begin("##paletteContentWidthProbe", nullptr,
                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                           ImGuiWindowFlags_NoScrollbar)) {
        // 100px child, 2000px of content -- more overflow than any real
        // window size could absorb, so a child *without* NoScrollbar would
        // unambiguously turn its scrollbar on regardless of the host
        // machine's font metrics or DPI. This child carries
        // `ImGuiWindowFlags_NoScrollbar`, matching ui/MacPaintUI.cpp's real
        // "##toolgrid" child exactly, so what this measures is whether that
        // flag really does suppress the width reservation even under forced
        // overflow -- not whether the grid happens not to overflow today.
        if (ImGui::BeginChild("##paletteGridProbe", ImVec2(0.0f, 100.0f), 0,
                              ImGuiWindowFlags_NoScrollbar)) {
          ImGui::Dummy(ImVec2(1.0f, 2000.0f));
          if (frame == 1) contentWidth = ImGui::GetContentRegionAvail().x;
        }
        ImGui::EndChild();
      }
      ImGui::End();
      ImGui::EndFrame();
    }

    std::printf("  [measured] live GetContentRegionAvail().x inside the (forced-overflow, "
                "NoScrollbar) tool grid = %.1f px (kToolCellMax = %.1f)\n",
                contentWidth, kToolCellMax);
    // Exact equality, not >=: with no scrollbar to eat width, the content
    // region of a kToolPaletteW-wide window is kToolPaletteW minus
    // WindowPadding on both sides, full stop -- kToolPaletteW's own
    // static_assert (ui/AtelierLayout.hpp) says that difference is exactly
    // kToolCellMax. A small tolerance guards only against float rounding
    // through the two NewFrame()s, not against any real ambiguity in what
    // the answer should be.
    check(std::fabs(contentWidth - kToolCellMax) < 0.01f,
          "a real ImGui window/child at kToolPaletteW, forced to overflow, still leaves "
          "exactly kToolCellMax of content width -- NoScrollbar is truly suppressing the "
          "scrollbar reservation, not merely hiding the bar while still reserving its width");

    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
  }

  // --- Part H: the tool cell shrinks to fit, or honestly gives up ----------
  //
  // Part G asked whether the *width* Dear ImGui hands back matches
  // kToolPaletteW's arithmetic. This asks the matching question about the
  // *height* side: does atelierToolCellSize() actually pack kToolCellCount
  // cells plus kToolSeparatorsH of separator rules into the grid band at a
  // representative spread of window heights, the way ui/MacPaintUI.cpp's
  // palette draw relies on it to -- or, below the "honest limit"
  // ui/AtelierLayout.hpp's own comment names, does it at least clamp to
  // kToolCellMin instead of shrinking past legibility.
  //
  // Each expected cell size below is hand-derived from the same arithmetic
  // atelierToolCellSize() implements (paletteH -> gridH -> floor/clamp),
  // written out again independently here rather than obtained by calling
  // the function and comparing it to itself -- the same reason Part C
  // recomputes the canvas's expected width from kW/kToolPaletteW/322/rules
  // instead of trusting atelierLayout()'s own answer. 940 and 790 are the
  // same roomy/middling window heights the 28-cell design used, recomputed
  // for kToolCellCount=18 (940 now clamps at kToolCellMax=36 rather than
  // shrinking to 26 the way 28 cells forced it to; 790 lands at 31, not
  // 20). 530 is the one below this revision's honest limit (~540px, down
  // from ~670px now that nesting nearly halved the cell count) -- its raw
  // (unclamped) quotient is floor(314/18)=17.4, one case where the *floor*
  // of the division is not what ships, only the *clamp* to kToolCellMin
  // is, and where the packed total (18*18+4=328) genuinely exceeds the
  // 318px grid band this window leaves, which is exactly the disclosed
  // fallback rather than a bug this check should paper over.
  {
    const struct {
      float winH;
      float wantCell;
      const char* note;
    } kCases[] = {
        {940.0f, kToolCellMax, "roomy: clamps at kToolCellMax (36) with room to spare"},
        {790.0f, 31.0f, "middling: shrinks below kToolCellMax, still fits exactly"},
        {530.0f, kToolCellMin,
         "below the honest limit: the unclamped quotient is 17.4, but "
         "kToolCellMin (18) wins the clamp -- the grid does not fully fit "
         "here, and NoScrollbar's wheel-only fallback carries the rest"},
    };
    bool fitsOk = true;
    for (const auto& c : kCases) {
      // showTabStrip=true: the case this build actually runs in once any
      // document is open, and the case the coordinator's own worked
      // examples (940/790/600) were computed against.
      const AtelierBands b = atelierLayout(0.0f, 0.0f, 1280.0f, c.winH, /*showTabStrip=*/true);
      const float got = atelierToolCellSize(b.toolPalette.h);
      if (std::fabs(got - c.wantCell) > 0.01f) {
        fitsOk = false;
        std::printf("    at window h=%.0f: atelierToolCellSize() = %.1f, want %.1f (%s)\n",
                    c.winH, got, c.wantCell, c.note);
      }
      if (got < kToolCellMin - 0.01f || got > kToolCellMax + 0.01f) {
        fitsOk = false;
        std::printf("    at window h=%.0f: %.1f is outside [kToolCellMin, kToolCellMax]\n",
                    c.winH, got);
      }
      // The cell size either packs the whole grid into the band Dear ImGui
      // actually gave it, or -- only once the clamp has already hit its
      // floor, i.e. shrinking further was not an option this build allows
      // -- it does not, and that shortfall is the disclosed, wheel-only
      // fallback, not a silently broken fit.
      const float used = got * static_cast<float>(kToolCellCount) + kToolSeparatorsH;
      const float gridH = b.toolPalette.h - kToolSwatchAreaH;
      const bool fits = used <= gridH + 0.01f;
      const bool honestlyClamped = std::fabs(got - kToolCellMin) < 0.01f;
      if (!fits && !honestlyClamped) {
        fitsOk = false;
        std::printf("    at window h=%.0f: %d cells at %.1fpx (%.1fpx used) overflow the "
                    "%.1fpx grid band without being clamped to kToolCellMin\n",
                    c.winH, kToolCellCount, got, used, gridH);
      }
    }
    check(fitsOk,
          "atelierToolCellSize() packs kToolCellCount cells into the grid band at a roomy "
          "and a middling window height, and honestly clamps to kToolCellMin rather than "
          "overflowing when even that cannot fit");
  }

  std::printf("[selftest] atelier chrome %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
