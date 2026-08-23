#pragma once
#include <cstddef>

#include "ui/AtelierTheme.hpp"  // kWindowPaddingX, kScrollbarSize -- see kToolPaletteW below

// ui/AtelierLayout -- docs/ui.md section 2's window layout, as arithmetic.
//
// The ASCII diagram in that section is dimensioned, and those dimensions are
// the whole content of this file: bands 36 / 34 / 46 / 26 px tall, a 104 px
// tool palette, a 322 px right-hand column, and 2 px rules between major
// regions. What the outgoing chrome had instead were three constants invented
// in place -- a 78 px palette (`kToolCol * kToolSize + 18`), a 300 px controls
// column and a 62 px swatch strip along the bottom that the design does not
// have at all.
//
// Deliberately free of ImGui itself: the geometry is the part worth testing,
// and a test that needs a window, a device and a font atlas to check that
// four bands tile a rectangle is a test nobody runs. `--selftest` computes
// layouts at several window sizes and asserts they tile exactly, with no
// ImGui context anywhere near it. It depends on ui/AtelierTheme.hpp, which
// is under the same rule (`#include <cstdint>` and nothing else) -- that is
// what lets kToolPaletteW below reference the *actual* WindowPadding/
// ScrollbarSize the live ImGuiStyle uses, rather than a second, hand-copied
// pair of literals.
namespace np {

struct AtelierRect {
  float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

  constexpr float right() const noexcept { return x + w; }
  constexpr float bottom() const noexcept { return y + h; }
  constexpr bool empty() const noexcept { return w <= 0.0f || h <= 0.0f; }
};

// docs/ui.md section 2. The trailing numbers in the diagram are heights; the
// two under it (52, 322) are the column widths -- kToolPaletteW is defined
// just below, next to the arithmetic that produces its 52.
constexpr float kTitleBarH   = 36.0f;
constexpr float kTabStripH   = 34.0f;
constexpr float kOptionsBarH = 46.0f;
constexpr float kStatusBarH  = 26.0f;
constexpr float kRightColumnW = 322.0f;

// **The user's own correction, stated plainly: "make the toolbar fit
// without scrolling ... the buttons are too large."** A fixed 36px cell
// with a permanently-visible scrollbar (two revisions back) satisfied the
// letter of "every cell fits inside the palette" while failing the actual
// design brief; the revision after that replaced the fixed cell with one
// computed **per frame**, shrinking before the column ever reached for a
// scrollbar -- correct, but with 28 cells still to fit, the shrink
// clamped most windows down to a 20-26px cell, small enough that the
// user's *next* words were "nest similar tools into a flyout to conserve
// space like photoshop." **This is that revision.** ui/AtelierChrome.hpp's
// `kToolGroups` collapses the 27 `Tool` values plus the "More" cell from
// 28 palette slots to 18 -- one slot per Photoshop-style group, each
// showing whichever member was last used, with a flyout for the rest
// (ui/MacPaintUI.cpp's `toolGroupButton()`) -- and the space that frees up
// is spent raising `kToolCellMax` back to the design's original generous
// desktop target rather than shrinking further than it has to.
//
// docs/ui.md section 2 keeps naming 15px Lucide icons; what changes here
// is only how many cells the column holds and how tall each is allowed to
// be around that fixed glyph size.
constexpr float kToolCellMax = 36.0f;  // the size a roomy window gets --
                                       // back to this file's very first
                                       // revision's number, now that
                                       // grouping bought the room for it
constexpr float kToolCellMin = 18.0f;  // the floor -- a 15px glyph needs at
                                       // least this much cell to keep ~1.5px
                                       // of breathing room on every side
// ui/AtelierChrome.hpp's `kToolGroups` has 17 slots (one per Photoshop-
// style group; app/selftest/AtelierChrome.cpp's completeness check proves
// every one of the 27 `Tool` values lands in exactly one) plus the "More"
// overflow cell (ui/MacPaintUI.cpp's `moreToolsButton()`, not a group and
// not a `Tool`) = 18 cells, always -- named here rather than left as a
// bare `18` in the arithmetic below, so a future palette entry changes one
// number instead of two disagreeing ones. Coincidentally the same numeral
// as `kToolCellMin`'s 18 *pixels* -- two unrelated quantities that happen
// to share a value, not one constant doing two jobs.
constexpr int kToolCellCount = 18;
// The palette's four group-separator rules (docs/ui.md section 2's groups:
// selection/sampling, retouch/fill, paint, vector/text -- a fifth group,
// navigation, closes the column and needs no rule after it), each
// `kDividerThickness` (1px) tall. Still four: nesting changed which
// `Tool`s sit in each design group, not how many design groups there are
// (ui/AtelierChrome.hpp's `kToolGroups` marks the same four boundaries via
// `ruleAfter`, just on different slots than before).
constexpr float kToolSeparatorsH = 4.0f;

// The palette's own arithmetic can't feed the FG swatch's reservation
// *and* be fed by it -- computing the tool grid's height needs to know how
// much room the swatch below it takes, but the swatch's own size (this
// file's earlier revision) was `kToolSize - 8`, and `kToolSize` is exactly
// the number the grid computation is trying to produce. Anchoring the
// swatch reservation to `kToolCellMax` instead of the live cell size
// breaks that circularity, and buys the swatch a second property the user
// asked for implicitly by asking for a stable, non-scrolling palette: **it
// does not resize or reflow as the window resizes**, because its size is a
// compile-time constant now, not a function of the live layout.
//
//     swatchAreaH = (kToolCellMax - 8) + 34 = 62
//
// `kToolCellMax - 8` is the swatch's own side (8px of margin, split evenly,
// the same accounting every earlier revision of this constant used); `34`
// is room for its "FG" caption and the padding around it.
constexpr float kToolSwatchAreaH = kToolCellMax - 8.0f + 34.0f;  // 62

// `kToolPaletteW` needs room for exactly one cell at its *largest*
// (`kToolCellMax`) plus `ImGuiStyle::WindowPadding` on both sides --
// **and nothing for a scrollbar**, because there no longer is one: the
// tool grid's `BeginChild()` now carries `ImGuiWindowFlags_NoScrollbar`
// (ui/MacPaintUI.cpp), which an earlier revision of this file argued at
// length was permanently necessary and is now the opposite of true. A
// window that cannot show every cell at `kToolCellMin` still scrolls --
// the mouse wheel keeps working inside a `NoScrollbar` child, it is only
// the bar itself that stops being drawn and stops reserving width -- but
// that is the honest, disclosed fallback for a window too short to hold
// the design (see the cell-size computation below), not the steady state
// this width is sized for.
//
// kWindowPaddingX (ui/AtelierTheme.hpp) is what `applyAtelierTheme()`
// actually assigns to `ImGuiStyle::WindowPadding.x` -- not a second,
// hand-copied literal that could drift from it (ui/AtelierTheme.hpp's own
// comment is the fuller account of why that pairing matters).
constexpr float kToolPaletteW = kToolCellMax + 2.0f * kWindowPaddingX;  // 52
static_assert(kToolPaletteW - 2.0f * kWindowPaddingX >= kToolCellMax,
              "docs/ui.md section 2: the palette's content region -- width minus "
              "WindowPadding on both sides, no scrollbar to subtract now that the grid "
              "shrinks instead of scrolling -- has to fit the largest cell the layout "
              "below can produce.");

// The tool grid's actual, per-frame cell size: as large as `kToolCellMax`
// when the window is roomy, shrinking to fit `kToolCellCount` cells (plus
// `kToolSeparatorsH` of separator rules) into whatever the palette band
// leaves after `kToolSwatchAreaH`, floored at `kToolCellMin` rather than
// shrinking indefinitely.
//
// Pure arithmetic, free of ImGui like the rest of this file, so
// `--selftest` can assert it fits at a table of window heights without a
// window -- see app/selftest/AtelierChrome.cpp's Part G/H.
//
// **The honest limit.** Below roughly a 540px window (18 cells *
// kToolCellMin + kToolSeparatorsH + kToolSwatchAreaH, worked back through
// atelierLayout()'s band arithmetic -- lower than the 28-cell design's
// ~670px, because nesting cut the cell count nearly in half), the grid
// genuinely cannot fit even at the smallest legible cell size -- clamping
// stops the cells from shrinking past legibility, it does not stop the
// window from being too short. ui/MacPaintUI.cpp does not clip the grid or
// hide any tool in that case: the child keeps `ImGuiWindowFlags_NoScrollbar`
// (no bar drawn, no width reserved for one) but Dear ImGui's mouse-wheel
// scroll still works inside a `NoScrollbar` child, so every cell stays
// reachable, just not simultaneously visible, on a window this short.
float atelierToolCellSize(float paletteH) noexcept;

// The six regions of docs/ui.md section 2, plus the rules between them.
//
// Every rect is in the same coordinate space as the value handed to
// `atelierLayout()` -- ImGui viewport work coordinates at the call site, but
// this file does not know that and does not care.
struct AtelierBands {
  AtelierRect titleBar;     // wordmark, menus, undo/redo/panels
  AtelierRect tabStrip;     // documents; empty until PLAN.md Phase 5 step 14
  AtelierRect optionsBar;   // the active tool's options
  AtelierRect toolPalette;  // left column, 2 x n grid + FG/BG swatch
  AtelierRect canvas;       // canvas + rulers + navigator
  AtelierRect rightColumn;  // COLOR / BRUSH / LAYERS / HISTORY / ...
  AtelierRect statusBar;    // zoom, working space, resident/budget, view state

  // 2 px `#f3f2f2` between major regions (docs/ui.md section 1). Four
  // horizontal and two vertical at most; a rule adjacent to an empty band is
  // itself empty, so that suppressing the tab strip suppresses exactly one
  // rule and the bands still tile.
  static constexpr size_t kMaxRules = 6;
  AtelierRect rules[kMaxRules];
  size_t ruleCount = 0;
};

// Lay out a window of `w` x `h` at origin (`x`, `y`).
//
// `showTabStrip` is false until documents present as tabs (PRD A5, PLAN.md
// Phase 5 step 14). It is a parameter rather than a `constexpr false` because
// the band belongs to the layout now -- what is missing is the thing that
// fills it, and a layout that cannot express the design's own diagram would
// have to be rewritten rather than switched on.
//
// Undersized windows: the bands are absolute and the canvas is the remainder,
// so a window shorter than the bands would give the canvas a negative height.
// The canvas is clamped at zero and the bands keep their sizes -- chrome that
// shrinks is chrome that lies about its hit targets, and the honest failure
// mode for a window too small to hold the design is a canvas you cannot see.
AtelierBands atelierLayout(float x, float y, float w, float h, bool showTabStrip);

// ------------------------------------------------------------- navigator
//
// docs/ui.md section 2 draws a NAVIGATOR floating over the bottom-right of the
// canvas. Its geometry is here with the rest of the layout, and for the same
// reason: it is arithmetic, and arithmetic checked by looking at a screenshot
// is arithmetic nobody has checked.
constexpr float kNavigatorMaxW = 180.0f;
constexpr float kNavigatorMaxH = 140.0f;
constexpr float kNavigatorInset = 16.0f;

// The navigator's box: the document's aspect fitted inside
// kNavigatorMaxW x kNavigatorMaxH, inset from the canvas region's bottom-right
// corner.
//
// Returns an empty rect when the document has no area, or when the canvas is
// too small to hold the box and its inset without covering the paint area --
// a navigator that hides the picture it navigates is worse than none, and at
// that size the user is already scrolled into a corner of a window that cannot
// hold the design.
AtelierRect atelierNavigatorRect(const AtelierRect& canvas, float docW, float docH);

// Map a document-space rect (`x0,y0`-`x1,y1`, in document pixels) onto the
// navigator box. Used for the viewport indicator; clamped to the box, because
// the visible region of a zoomed-out view extends past the paper on every side
// and an unclamped indicator would draw outside the navigator.
AtelierRect atelierNavigatorMap(const AtelierRect& nav, float docW, float docH, float x0,
                                float y0, float x1, float y1);

// ------------------------------------------------------------ split panes
//
// PRD **A5** (P1): "Documents present as tabs, with an optional split showing
// two." docs/ui.md section 5 says where the control is: "The `columns-2` and
// `layout-grid` icons in the tab strip are the two-tab split from ADR-0001's
// amendment. Wire them to that, not to a floating-window manager."
//
// **Two icons, one feature, and the design does not say which is which.**
// `layout-grid` is a 2x2 icon, but ADR-0001's amendment caps visible documents
// at two, so a four-pane grid is not a state this build is allowed to reach.
// Read as a pair the two icons are the two ways to cut a rectangle in half, so
// `columns-2` is the side-by-side split and `layout-grid` is the stacked one.
// That is an interpretation, and it is recorded here rather than left implicit
// in a click handler.
//
// The split is in the *layout* rather than in the canvas code for this file's
// standing reason: dividing a rectangle in two is arithmetic, and arithmetic
// checked by looking at a screenshot is arithmetic nobody has checked.
enum class AtelierSplit { Single, Columns, Rows };

// The panes the canvas region divides into, and the rule between them.
//
// `count` is 1 or 2. It is 1 for `Single` **and** for a canvas too small to
// divide -- see `atelierSplitPanes()`.
struct AtelierPanes {
  AtelierRect pane[2];
  AtelierRect divider;
  size_t count = 1;
};

// The smallest pane this will produce. Below it the split collapses back to a
// single pane rather than handing the caller two slivers.
//
// The number is the navigator's own box plus its inset on each side
// (`kNavigatorMaxW + 2 * kNavigatorInset`), which is not a coincidence and not
// a guess: it is the width at which the canvas region already stops being able
// to show its own overlay, so a pane narrower than this is one the rest of the
// chrome has already given up on.
constexpr float kMinPaneW = kNavigatorMaxW + 2.0f * kNavigatorInset;
constexpr float kMinPaneH = kNavigatorMaxH + 2.0f * kNavigatorInset;

// Divide `canvas` per `split`, with a 2 px rule between the panes.
//
// The rule is *between* the panes and belongs to neither, exactly as
// `atelierLayout()`'s rules are between bands -- so the two panes plus the
// divider tile `canvas` exactly, with no overlap and nothing left over.
// `--selftest` asserts that at several window sizes rather than trusting it.
//
// Pane 0 is the left one under `Columns` and the top one under `Rows`. Which
// pane is *focused* is not decided here: it is session state, it lives with
// the tab strip, and the geometry is the same either way.
AtelierPanes atelierSplitPanes(const AtelierRect& canvas, AtelierSplit split);

}  // namespace np
