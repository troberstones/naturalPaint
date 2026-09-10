#include "ui/ToolCursor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include "app/StrokeSession.hpp"
#include "core/ResourcePaths.hpp"
#include "ui/AtelierChrome.hpp"

// stb_truetype's IMPLEMENTATION lives in this translation unit, and only this
// one, at global scope -- the same scope `imgui_draw.cpp` (third_party/imgui)
// puts its own copy in. That file already compiles stb_truetype with
// `STBTT_STATIC` set, which makes every `stbtt_*` symbol file-local (internal
// linkage), so there is nothing of ImGui's to link against here, and defining
// a second, equally file-local copy in this file is not an ODR risk: it is
// the documented way to use a single-header library from more than one
// translation unit (`imstb_truetype.h`'s own top-of-file comment calls this
// out). Deliberately included here rather than nested inside `namespace np`
// below -- a nested `#include` would declare every `stbtt_*` symbol as
// `np::stbtt_*` instead, which is legal but is not how the rest of this
// codebase's few third-party single-header uses do it.
//
// This is also why §7's rasterisation talks to stb_truetype directly rather
// than through Dear ImGui's `ImFontAtlas`: that path needs a live `GImGui`
// (`ui/Fonts.cpp`'s own merge functions run after `ImGui::CreateContext()`),
// and `rasterizeToolCursorBitmap()` has to run inside `--selftest`, which
// creates no ImGui context at all -- see `app/selftest/ToolCursor.cpp`'s file
// comment on why this whole test file is headless.
//
// The pragma pair below is the equivalent of `src/CMakeLists.txt`'s own
// SYSTEM-include treatment of `third_party` (see that file's comment on
// `stb_image.h`), applied locally instead of at the include-path level:
// `imstb_truetype.h` sits in the `imgui` FetchContent target's own source
// directory, which this project does not mark SYSTEM, so its
// `STB_TRUETYPE_IMPLEMENTATION` block trips this build's `-Werror=unused-*`
// guard on every packing/kerning function this file never calls. Scoped to
// exactly the `#include` line, so a real unused-function mistake in code
// this project owns is still caught everywhere else.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace np {

ToolCursor cursorForTool(Tool tool) noexcept {
  switch (tool) {
    // --- deposits pigment or colour under the tip -------------------------
    //
    // The whole brush family, including the five retouch cells that are not
    // built yet: every one of them is "a tip is dragged and the pixels under
    // it change", which is what `Paint` names. Whether they *work* is
    // `toolCursorOnTarget()`'s question, not this one.
    case Tool::Brush:
    case Tool::Water:
    case Tool::DryBrush:
    case Tool::Pencil:
    case Tool::Eraser:
    case Tool::Smudge:
    case Tool::Dodge:
    case Tool::Burn:
    case Tool::CloneStamp:
      return ToolCursor::Paint;

    // The two fill ops. `Paint` rather than a `Fill` of their own: a bucket
    // click and a brush dab both put the foreground colour into the layer, and
    // an intent that only ever projected to the same shape as `Paint` would be
    // a distinction the user could never see. `app/StrokeSession` §6 keeps
    // them apart where it matters -- which predicate gates them -- and
    // `toolCursorOnTarget()` below reads that one, not this arm.
    case Tool::PaintBucket:
    case Tool::Gradient:
      return ToolCursor::Paint;

    // --- reads the canvas instead of writing it ---------------------------
    case Tool::Eyedropper:
    case Tool::Measure:
      return ToolCursor::Sample;

    // --- defines a region or a path ---------------------------------------
    //
    // Crop and Slice are here rather than with Move because what the user does
    // with them is drag out a rectangle; that the rectangle later changes the
    // document's bounds is the *result*, not the gesture. Pen, Curve and Shape
    // are here for the same reading: all three define geometry by placing
    // points, and none of them deposits under the pointer the way `Paint` does.
    // T17 did not report these five as confusable with one another, so they
    // stay folded into plain `Select` -- see hpp §7 on the split just below.
    case Tool::Crop:
    case Tool::Slice:
    case Tool::Pen:
    case Tool::Curve:
    case Tool::Shape:
      return ToolCursor::Select;

    // T17 (docs/testing-issues.md) named these five as sharing one cursor, and
    // an earlier revision answered that by giving each its own `ToolCursor`
    // value. That turned out to be the wrong layer: what a user needs to tell
    // apart is the TOOL, and §7 now keys its bitmaps by `Tool` directly, which
    // covers all twenty-eight rather than five and needs no enumerator here.
    // So these five are back where §2's argument puts them -- one INTENT,
    // "a boundary being drawn" -- and `ToolCursor` is an intent enum again
    // rather than a key some other table needed.
    case Tool::Marquee:
    case Tool::EllipseMarquee:
    case Tool::Lasso:
    case Tool::PolygonLasso:
    case Tool::MagicWand:
      return ToolCursor::Select;

    // --- the view, and content within it ----------------------------------
    case Tool::Hand:
      return ToolCursor::Pan;
    case Tool::Zoom:
      return ToolCursor::Zoom;

    // Move and Frame share `kToolGroups`' first slot, and share a meaning:
    // something already on the canvas follows the drag.
    case Tool::Move:
    case Tool::Frame:
    // Path Select shares that MEANING while sharing neither slot nor pixels:
    // its whole job is that existing geometry follows the drag. It is
    // deliberately NOT folded in with Pen and Curve above, even though the
    // three are flyout siblings -- those two define geometry by clicking
    // points, which is `Select`'s intent, and this one moves geometry that is
    // already there. Sorting the group's members by what they DO rather than
    // by which slot they live in is the whole reason this switch lists
    // enumerators instead of groups. §7's per-`Tool` bitmap still gives it
    // its own arrow, so it does not look like the Move tool.
    case Tool::PathSelect:
      return ToolCursor::MoveObject;

    case Tool::Text:
      return ToolCursor::Text;

    // Not a tool -- the enum's bound. Listed so `-Wswitch` is satisfied
    // without a `default:` arm that would also swallow a genuinely new tool,
    // exactly as `strokeRouteFor()` lists it.
    case Tool::Count:
      return ToolCursor::Arrow;
  }
  // Unreachable for any `Tool` value; present because a stray cast is not a
  // `Tool` value and a function returning `ToolCursor` must still return one.
  return ToolCursor::Arrow;
}

ToolCursor toolCursorOnTarget(Tool tool, const Layer* target) noexcept {
  // **The unbuilt cells first**, because "this tool does nothing at all" is a
  // stronger statement than anything the target could add: an Eraser over a
  // perfectly writable RGB layer still erases nothing. Header §5 argues the
  // choice, including the case against it.
  if (!toolImplemented(tool)) return ToolCursor::Refuse;

  // The paint bucket and the gradient, gated by the predicate that actually
  // gates them in the canvas block -- `app/StrokeSession` §6's, not the stroke
  // table's. Reading the stroke table here would be the "lying indicator"
  // defect `app/selftest/BucketRefusal.cpp` section D was written about:
  // `strokeRouteFor()` answers `None` for both fill tools on *every* layer,
  // correctly, because neither begins a stroke -- so a cursor that consulted it
  // would show a slashed circle over a layer the bucket was about to fill.
  if (toolWritesRgbPixels(tool))
    return pixelOpWritesLayer(target) ? cursorForTool(tool) : ToolCursor::Refuse;

  // Whether this tool begins a stroke, asked of the route table rather than
  // restated as a third copy of `ui/MacPaintUI.cpp`'s `paintTool` bool. With no
  // target the table sends exactly Brush, DryBrush and Water to `PaintSim` and
  // everything else to `None`, so this is precisely "is it a stroke tool" --
  // see header §4.
  if (strokeRouteFor(tool, nullptr) != StrokeRoute::None) {
    // `None` here can only mean a target that refused, since the no-target case
    // is `PaintSim` and Water never refuses at all. That is the locked layer
    // and the layer kind with nowhere to put paint -- the two refusals the
    // options bar already prints a sentence for.
    if (strokeRouteFor(tool, target) == StrokeRoute::None) return ToolCursor::Refuse;
  }

  return cursorForTool(tool);
}

SDL_SystemCursor sdlCursorFor(ToolCursor cursor) noexcept {
  switch (cursor) {
    case ToolCursor::Arrow:
      return SDL_SYSTEM_CURSOR_DEFAULT;

    // **The reason this module talks to SDL rather than to ImGui.** ImGui's
    // enum has no crosshair at all, so routing the tools through it left the
    // brush, all four selection tools, the eyedropper and the bucket sharing
    // one plain arrow. The crosshair is the shape a painter expects over a tip
    // whose exact centre is about to matter.
    case ToolCursor::Paint:
      return SDL_SYSTEM_CURSOR_CROSSHAIR;

    // A marquee, a lasso or a crop is a dragged-out extent, and the diagonal
    // double arrow is the set's "this defines a rectangle" shape. Header §3
    // names this as one of the two weakest entries and says what the
    // conventional alternative is (a crosshair here too, as Photoshop does)
    // and why distinguishability won for now.
    //
    // This is also the FALLBACK every selection tool lands on when §7's
    // bitmaps are off or one failed to rasterise, which is why it is left
    // exactly as it was: "bitmaps off" has to be byte-identical to the build
    // before §7 existed, and `app/selftest/ToolCursor.cpp` section G checks
    // that per tool rather than per enum value.
    case ToolCursor::Select:
      return SDL_SYSTEM_CURSOR_NWSE_RESIZE;

    // The pointing finger: "the pixel under this exact spot is the one I am
    // about to read". Reading rather than writing is the distinction, and it is
    // now visible where under ImGui it could not be.
    case ToolCursor::Sample:
      return SDL_SYSTEM_CURSOR_POINTER;

    // SDL's four-pointed MOVE cursor -- "drag and something follows". True of
    // the Hand moving the view and of the Move tool moving content, which is
    // why these two share it. §3 argues that this collision is fair rather
    // than forced: it would survive a richer set, because the two really do
    // mean the same thing about the pointer.
    case ToolCursor::Pan:
    case ToolCursor::MoveObject:
      return SDL_SYSTEM_CURSOR_MOVE;

    // The other diagonal, mirrored from `Select`. Magnification is scaling, and
    // this is the set's second scale-ish shape. The other weak entry of §3's
    // two, kept because the alternative was sharing a shape with `Sample` --
    // and a zoom that looks like an eyedropper says less than one that looks
    // vaguely like scaling.
    case ToolCursor::Zoom:
      return SDL_SYSTEM_CURSOR_NESW_RESIZE;

    case ToolCursor::Text:
      return SDL_SYSTEM_CURSOR_TEXT;

    // The one this file was mostly written for.
    case ToolCursor::Refuse:
      return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
  }
  return SDL_SYSTEM_CURSOR_DEFAULT;
}

// Every shape ImGui itself asks for, so that suppressing the backend costs the
// panels, the menus, the window borders and the filter text box nothing.
// Header §6.
//
// The static_assert is the guard that matters here. `ImGuiMouseCursor` is a
// plain `int` typedef rather than an enum class, so `-Wswitch` cannot force
// this switch to stay exhaustive the way it forces `cursorForTool()`. An ImGui
// upgrade that adds a cursor would otherwise land silently in the `default:`
// arm and quietly show an arrow where the new shape belonged -- on a code path
// nothing in this build tests, because it is ImGui's own chrome behaviour.
// Pinning the count means that upgrade stops the build instead.
static_assert(ImGuiMouseCursor_COUNT == 11,
              "Dear ImGui's cursor set changed size -- add the new value to "
              "sdlCursorForImGui() below, then update this count. The mapping is what "
              "keeps every panel and menu behaving as it did before this build took "
              "ImGuiConfigFlags_NoMouseCursorChange.");

SDL_SystemCursor sdlCursorForImGui(ImGuiMouseCursor cursor) noexcept {
  switch (cursor) {
    case ImGuiMouseCursor_Arrow:
      return SDL_SYSTEM_CURSOR_DEFAULT;
    case ImGuiMouseCursor_TextInput:
      return SDL_SYSTEM_CURSOR_TEXT;
    case ImGuiMouseCursor_ResizeAll:
      return SDL_SYSTEM_CURSOR_MOVE;
    case ImGuiMouseCursor_ResizeNS:
      return SDL_SYSTEM_CURSOR_NS_RESIZE;
    case ImGuiMouseCursor_ResizeEW:
      return SDL_SYSTEM_CURSOR_EW_RESIZE;
    case ImGuiMouseCursor_ResizeNESW:
      return SDL_SYSTEM_CURSOR_NESW_RESIZE;
    case ImGuiMouseCursor_ResizeNWSE:
      return SDL_SYSTEM_CURSOR_NWSE_RESIZE;
    case ImGuiMouseCursor_Hand:
      return SDL_SYSTEM_CURSOR_POINTER;
    case ImGuiMouseCursor_Wait:
      return SDL_SYSTEM_CURSOR_WAIT;
    case ImGuiMouseCursor_Progress:
      return SDL_SYSTEM_CURSOR_PROGRESS;
    case ImGuiMouseCursor_NotAllowed:
      return SDL_SYSTEM_CURSOR_NOT_ALLOWED;
    default:
      // `ImGuiMouseCursor_None` (-1) reaches here only if a caller failed to
      // handle it before indexing; `apply()` does handle it. Anything else is
      // out of range. The default arrow is the safe answer either way -- what
      // must not happen is reading past the table.
      return SDL_SYSTEM_CURSOR_DEFAULT;
  }
}

// ---------------------------------------------------------------- §7: bitmaps

namespace {

// **A design space, not a pixel size.** Every coordinate in this section is
// written in these 32 units and multiplied by a `scale` on the way to pixels,
// so there is one layout and not one per size. 32 is a comfortable grid for
// the shapes, nothing more; it is deliberately NOT the size anything ships
// at.
constexpr int kCursorDesignUnits = 32;

// **The size a cursor actually is, in points, and the measurement behind it.**
//
// macOS's own cursors are 24x24 points: `[NSCursor crosshairCursor].image.size`
// reads exactly 24.0 x 24.0 (and `openHandCursor` 32x32), measured rather than
// assumed. A cursor noticeably larger than the system's own does not read as a
// design choice, it reads as a bug -- which is exactly the report this constant
// exists to answer.
//
// **What is NOT applied here, and the correction that removed it.** An earlier
// revision multiplied this by the user's Accessibility ▸ Pointer size setting,
// on the strength of published claims that macOS does not scale custom
// cursors. **That is wrong on this platform: macOS scales an application's own
// `NSCursor` along with its own.** Confirmed against a real machine with the
// setting at 2.07x, where the result was a cursor roughly three times the size
// it should have been -- the OS's scaling multiplied by ours. So this file
// draws at the *unscaled* size and lets the OS enlarge it, which is also the
// behaviour that keeps working if a future macOS changes the multiplier.
//
// The remaining reason a bitmap is bigger than 24 pixels is the display
// backing scale, and that is SDL's job, not this constant's: `create()` builds
// a 2x alternate through `SDL_AddSurfaceAlternateImage()`, and SDL's Cocoa
// backend folds base and alternate into one multi-representation `NSImage`
// whose POINT size stays the base surface's. So the base surface is 24x24 and
// the alternate 48x48, and the cursor is 24 points either way.
constexpr int kCursorBasePoints = 24;

// The base surface's scale: what `kCursorDesignUnits` has to be multiplied by
// to land at `kCursorBasePoints`. Named rather than written as `0.75f` so the
// two constants above stay the only numbers to change.
constexpr float kCursorBaseScale =
    static_cast<float>(kCursorBasePoints) / static_cast<float>(kCursorDesignUnits);

void setPixel(std::vector<uint8_t>& rgba, int w, int h, int x, int y, uint8_t a) {
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  // Black ink at the sampled coverage, opaque everywhere it is drawn at all
  // -- every shape this file draws is one colour, so there is no blending
  // beyond the alpha stb_truetype itself already anti-aliased.
  uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
  p[0] = p[1] = p[2] = 0;
  p[3] = a;
}

// The inverse of `setPixel()`: makes a texel transparent again. Used only to
// carve §10's nib slit out of a filled shape, and kept next to `setPixel()` so
// the two ways this file writes a texel are read together.
//
// The carved texels do not stay empty: `applyCursorOutline()` dilates the
// surrounding ink into every transparent neighbour, so a one-unit slit comes
// out WHITE rather than showing the canvas through it -- which is what a nib's
// slit looks like and is why nothing here has to composite a second colour.
void clearPixel(std::vector<uint8_t>& rgba, int w, int h, int x, int y) {
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
  p[0] = p[1] = p[2] = 0;
  p[3] = 0;
}

// One design unit, in pixels, for this bitmap. Rounded rather than truncated
// so a scale of 2.07 does not systematically pull every coordinate toward the
// top-left of where it was drawn.
int px(int designUnits, float scale) {
  return static_cast<int>(std::lround(static_cast<double>(designUnits) * scale));
}

// How thick a one-unit stroke is at this scale. Without this the shapes stay
// hairlines as they grow -- a 66-pixel marquee drawn with 1-pixel edges reads
// as a faint wireframe, not as an enlarged cursor, which would defeat the
// entire point of honouring the accessibility setting.
int strokeWidth(float scale) { return std::max(1, static_cast<int>(std::lround(scale))); }

// Bresenham, stamping a `t`x`t` square at each step. Every procedural cursor
// in this file is line segments, so one rasteriser covers the rectangle, the
// polygon standing in for the ellipse, and both crosshair arms.
void drawLine(std::vector<uint8_t>& rgba, int w, int h, int x0, int y0, int x1, int y1, int t) {
  int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    for (int oy = 0; oy < t; ++oy)
      for (int ox = 0; ox < t; ++ox) setPixel(rgba, w, h, x0 + ox, y0 + oy, 255);
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
  }
}

// ============================ §8: the composite every cursor is now made of
//
// **Two slots, and the division of labour between them.** A tool GLYPH in the
// upper-right box says WHICH tool; a CROSSHAIR at the lower-left says WHERE
// the click lands, and the hotspot is that crosshair's own centre pixel.
//
// This layout is not new -- it is exactly what the two marquees have shipped
// since T17, generalised from two tools to all of them. What changed is the
// reason: the marquee pair got it because the report named a crosshair, and
// every other tool kept a bare glyph whose hotspot was resolved against its
// own bounding box. Measured at the shipping 24x24, that rule put **nine of
// the twenty-nine hotspots on a fully transparent pixel** (Measure five pixels
// from its own ink; Dry Brush, Smudge and Pen three) and six more on ink below
// alpha 22 -- the outermost anti-aliased fringe, which is to say invisible.
// See `ui/ToolCursor.hpp` §8 for the measurement and for what it cost.
//
// **The glyph stops pointing at anything, and that is the simplification.**
// Under the old rule each icon had to nominate its own working end, because
// the hotspot was a fraction of that icon's bounding box -- per-tool knowledge
// that had to be right twenty-nine times and was wrong fifteen. Here the glyph
// is an identifier and nothing more, so there is no per-tool placement to get
// right at all, and a tool added tomorrow inherits a correct hotspot with no
// entry in any table. `cursorHotspotAnchorFor()` is gone for that reason and
// not because its per-tool judgements were bad ones.
// **Both slots are inset by two units from every canvas edge, and that margin
// is load-bearing rather than tidy.** `applyCursorOutline()` draws the white
// halo INSIDE the existing canvas, so a shape touching the edge simply has no
// halo on that side -- and the crosshair is the one shape that cannot afford
// it, being the whole of what tells the user where the click lands. The
// marquee pair shipped at (6, 26) with 5-unit arms, whose bottom pixel is row
// 31 of 31: against a dark canvas its lower arm ended in nothing. Moving it to
// (8, 23) costs three units of glyph and buys the crosshair an outline on all
// four arms.
constexpr int kGlyphLeft = 12, kGlyphTop = 2, kGlyphSize = 17;
constexpr int kCrossX = 8, kCrossY = 23, kCrossArm = 6;

// Caps Lock's bare crosshair (§9): no glyph at all, centred, arms running most
// of the canvas. The hotspot is the centre, where the arms cross -- inked by
// construction, the same property that makes the composite's own crosshair the
// right thing to hang a hotspot on.
constexpr int kPreciseArm = 12;

// **The crosshair, and the hotspot that IS its centre.** One function, called
// by every cursor this file produces, so "the hotspot is a drawn pixel" is a
// structural fact rather than twenty-nine separate placements that happen to
// agree. The alternative -- each generator setting its own hotspot -- is what
// was there before, and section G's assertion could only check the weaker
// claim that the point fell somewhere inside the glyph's bounding box.
//
// Both coordinates go through the SAME `px()` the arms themselves went
// through, not a second rounding of the same product, which is how a hotspot
// drifts a pixel off its own crosshair at some scales and not others.
void drawHotspotCrosshair(CursorBitmap& out, float scale) {
  const int t = strokeWidth(scale);
  const int crossX = px(kCrossX, scale), crossY = px(kCrossY, scale);
  const int arm = px(kCrossArm, scale);
  drawLine(out.rgba, out.width, out.height, crossX - arm, crossY, crossX + arm, crossY, t);
  drawLine(out.rgba, out.width, out.height, crossX, crossY - arm, crossX, crossY + arm, t);
  out.hotspotX = crossX;
  out.hotspotY = crossY;
}

// ================================= §10: the tool whose icon IS a pointer
//
// **The exception §8 predicted, and the reason it is a narrow one.** §8 moved
// every glyph out from under the pointer because per-tool placement is what
// put fifteen hotspots on transparent pixels. For an icon that is *itself a
// pointing thing*, though, displacing it is worse than the disease: every
// arrow ever drawn aims from its own tip, and a user who has to discover
// otherwise has already mis-clicked. Illustrator keeps its selection arrow's
// hotspot at the tip and badges it with modifiers rather than offsetting it.
//
// So this is one exception with one member, gated by a predicate rather than
// by an `if` in the middle of the rasteriser, and it does not reopen §8: the
// tip is a coordinate this file CHOOSES, not a fraction of a picture it has to
// infer. That is the whole difference. `cursorHotspotAnchorFor()` failed
// because it guessed where a glyph's working end was; here the working end is
// vertex zero of a polygon drawn on purpose.
//
// **Why the arrow is drawn rather than taken from Lucide**, which is the part
// worth recording. `mouse-pointer-2` -- the palette's own Path Select icon --
// is a HOLLOW stroked outline. At the shipping 24x24 its apex is one or two
// pixels of anti-aliased ink at partial alpha, so a hotspot on that apex would
// be exactly the invisible-fringe case §8 measured: Brush at alpha 1,
// Eyedropper at 4. Making the tip land on a pixel a human eye can see would
// have meant weakening §8's own assertion from "fully opaque" back to "some
// ink", which is the trap that let fifteen of these ship in the first place.
//
// A filled arrow has no such apex problem, and it is also what every pointer
// on every platform actually looks like. Same precedent as the marquee pair
// four functions down: no Lucide glyph is this shape, so this file draws it.
// The cost is that the cursor no longer matches its palette cell pixel for
// pixel -- it is still an arrow, and it is still the only arrow in the set.
constexpr int kArrowTipX = 2, kArrowTipY = 2;

// The classic seven-vertex pointer, clockwise from the tip, in design units.
// Vertex 0 IS the hotspot, which is why it is written first and why nothing
// below reorders this list.
constexpr int kArrow[7][2] = {
    {kArrowTipX, kArrowTipY},  // the tip, and the hotspot
    {2, 24},                   // straight down the left edge
    {8, 19},                   // in to the notch
    {12, 27},                  // down the tail's left side
    {15, 26},                  // across the tail's foot
    {10, 17},                  // back up the tail's right side
    {17, 17},                  // out to the wing, and closed back to the tip
};

// Fills a closed polygon and then strokes its own boundary. Shared by §10's
// two shapes, which is why it is a function rather than a loop inside one of
// them: the arrow and the nib both taper to the vertex their hotspot sits on,
// and they must taper the same way.
//
// Scanline fill sampling each row's centre. Even-odd is the same as non-zero
// for both of these -- neither self-intersects -- so the cheaper rule is the
// honest one.
//
// **The boundary stroke is not decoration.** Sampling row centres means a
// shape tapering to a point can lose its last row or two entirely, which is
// exactly what happens at the vertex a §10 hotspot hangs on. Stroking puts
// them back, and `drawLine()` stamps at its start coordinate, so vertex 0 is
// opaque black by construction rather than by luck.
void fillClosedPolygon(CursorBitmap& out, const int* xs, const int* ys, int n, int t) {
  int minY = ys[0], maxY = ys[0];
  for (int i = 1; i < n; ++i) {
    minY = std::min(minY, ys[i]);
    maxY = std::max(maxY, ys[i]);
  }
  for (int y = minY; y <= maxY; ++y) {
    const float sy = static_cast<float>(y) + 0.5f;
    float xsAt[16];
    int hits = 0;
    for (int i = 0; i < n && hits < 16; ++i) {
      const int j = (i + 1) % n;
      const float y0 = static_cast<float>(ys[i]), y1 = static_cast<float>(ys[j]);
      if ((sy >= y0) == (sy >= y1)) continue;  // this edge does not cross the row
      const float u = (sy - y0) / (y1 - y0);
      xsAt[hits++] = static_cast<float>(xs[i]) + u * static_cast<float>(xs[j] - xs[i]);
    }
    std::sort(xsAt, xsAt + hits);
    for (int k = 0; k + 1 < hits; k += 2)
      for (int x = static_cast<int>(std::lround(xsAt[k]));
           x <= static_cast<int>(std::lround(xsAt[k + 1])); ++x)
        setPixel(out.rgba, out.width, out.height, x, y, 255);
  }
  for (int i = 0; i < n; ++i) {
    const int j = (i + 1) % n;
    drawLine(out.rgba, out.width, out.height, xs[i], ys[i], xs[j], ys[j], t);
  }
}

void drawPointerArrow(CursorBitmap& out, float scale) {
  const int t = strokeWidth(scale);
  int xs[7], ys[7];
  for (int i = 0; i < 7; ++i) {
    xs[i] = px(kArrow[i][0], scale);
    ys[i] = px(kArrow[i][1], scale);
  }

  fillClosedPolygon(out, xs, ys, 7, t);
  out.hotspotX = xs[0];
  out.hotspotY = ys[0];
}

// §10's second member: a pen nib, tip first.
//
// **Why this is drawn rather than taken from Lucide, measured rather than
// assumed.** `pen-tool` -- the palette's own Pen icon -- is a hollow stroked
// outline whose nib points up and left. Probed at the shipping scale, the
// apex is three rows of PARTIAL-alpha ink and the first fully opaque pixel is
// two rows inside it. So the choice would have been a hotspot on an
// anti-aliased fringe (§8's measured defect, Brush at alpha 1) or a hotspot
// two pixels back from the nib the user is aiming with. Neither is "point
// from the nib", and the third option -- relaxing §8's opacity assertion for
// this one tool -- is how the original fifteen shipped.
//
// A drawn nib has no apex problem: the tip is vertex zero and `drawLine()`
// stamps it. Same trade the arrow above makes, and taken the same way on
// purpose -- two members of one exception behaving differently would be worse
// than either rule alone.
//
// **`Tool::Curve` deliberately stays on §8's composite.** Its icon is
// `spline`, a curve through control points, which has no nib and no tip; it
// places anchors exactly as the Pen does but it does not LOOK like a thing
// that points, and §10's bar is what the icon is, not what the tool does.
constexpr int kNibTipX = 2, kNibTipY = 2;

// The nib, clockwise from the tip, symmetric about the tip's own diagonal --
// reflecting (x, y) to (y, x) maps this list onto itself, which is what makes
// it read as a nib rather than as a leaf leaning one way.
constexpr int kNib[5][2] = {
    {kNibTipX, kNibTipY},  // the tip, and the hotspot
    {19, 10},              // the right shoulder
    {25, 19},              // the widest point, right
    {19, 25},              // the back, where a holder would meet it
    {10, 19},              // the widest point, left, and closed back to the tip
};

void drawPenNib(CursorBitmap& out, float scale) {
  const int t = strokeWidth(scale);
  int xs[5], ys[5];
  for (int i = 0; i < 5; ++i) {
    xs[i] = px(kNib[i][0], scale);
    ys[i] = px(kNib[i][1], scale);
  }
  fillClosedPolygon(out, xs, ys, 5, t);

  // The slit and the vent, carved out of the fill rather than drawn into it.
  // They start four units back from the tip so the hotspot's own pixel is
  // never one of them -- a nib whose slit reached the point would put the
  // hotspot on a transparent texel, which is the entire defect §8 is about.
  // `applyCursorOutline()` fills both with white afterwards, which is what a
  // slit looks like.
  const int slitFrom = px(6, scale), slitTo = px(15, scale);
  for (int d = slitFrom; d <= slitTo; ++d)
    for (int w = 0; w < t; ++w) clearPixel(out.rgba, out.width, out.height, d + w, d);
  const int ventX = px(18, scale), ventY = px(18, scale), ventR = std::max(1, px(2, scale));
  for (int dy = -ventR; dy <= ventR; ++dy)
    for (int dx = -ventR; dx <= ventR; ++dx)
      if (dx * dx + dy * dy <= ventR * ventR)
        clearPixel(out.rgba, out.width, out.height, ventX + dx, ventY + dy);

  out.hotspotX = xs[0];
  out.hotspotY = ys[0];
}

// The two shapes no font carries. Drawn into §8's glyph slot exactly as a
// Lucide glyph is, so the marquees are no longer a special case of the
// LAYOUT -- only of where their picture comes from.
void drawMarqueeShape(CursorBitmap& out, CursorMarqueeShape shape, float scale) {
  const int t = strokeWidth(scale);
  const int left = px(kGlyphLeft, scale), top = px(kGlyphTop, scale);
  const int size = px(kGlyphSize, scale);
  if (shape == CursorMarqueeShape::Rectangle) {
    drawLine(out.rgba, out.width, out.height, left, top, left + size, top, t);
    drawLine(out.rgba, out.width, out.height, left + size, top, left + size, top + size, t);
    drawLine(out.rgba, out.width, out.height, left + size, top + size, left, top + size, t);
    drawLine(out.rgba, out.width, out.height, left, top + size, left, top, t);
  } else {
    // A stepped polygon around the box's inscribed circle -- precision
    // beyond "reads as round rather than square" is not this glyph's job,
    // and a 20px cursor icon cannot show the difference between this and a
    // true midpoint ellipse anyway. The step count does not scale: at 4x the
    // 32 steps are 8 pixels apart on a 160-pixel circumference, still inside
    // the stroke width.
    constexpr int kSteps = 32;
    const float cx = left + size / 2.0f, cy = top + size / 2.0f, r = size / 2.0f;
    int prevX = 0, prevY = 0;
    for (int i = 0; i <= kSteps; ++i) {
      const float a = static_cast<float>(i) / kSteps * 6.2831853f;
      const int x = static_cast<int>(cx + r * std::cos(a));
      const int y = static_cast<int>(cy + r * std::sin(a));
      if (i > 0) drawLine(out.rgba, out.width, out.height, prevX, prevY, x, y, t);
      prevX = x;
      prevY = y;
    }
  }
}

// Draws one Lucide codepoint into §8's glyph slot, through stb_truetype
// directly -- no `ImFontAtlas`, no `GImGui`, see this section's own opening
// comment for why.
//
// **Returns whether anything was drawn**, and draws nothing at all for a
// missing file, an unreadable file, or a codepoint the vendored font build
// does not contain -- the exact three failure modes `ui/ToolCursor.hpp`'s
// original §1 worried a bitmap cursor could hit silently. The bool is what
// `rasterizeToolCursorBitmap()` needs and a scan of the finished bitmap can no
// longer give it: once the crosshair is drawn the composite is non-blank
// whatever the font did, so a failed glyph would otherwise ship as a
// crosshair-only cursor for every tool -- twenty-nine identical pointers, and
// a font failure that stayed exactly as silent as before §7 was written.
bool drawLucideGlyph(CursorBitmap& out, uint32_t codepoint, float scale) {
  // core/ResourcePaths.hpp: tries the executable-relative and override
  // locations before the compile-time path, so a copied binary still finds
  // the vendored font here, same as installToolIconFont() above it in
  // ui/Fonts.cpp. A missing file already falls through to the `false` this
  // function's own header comment documents, so no separate report is added
  // here -- resolveResourcePath() already wrote every location it tried to
  // stderr if none of them existed.
  std::ifstream file(lucideTtfPath(), std::ios::binary);
  if (!file.is_open()) return false;
  const std::vector<unsigned char> buffer((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
  if (buffer.empty()) return false;

  stbtt_fontinfo info;
  if (!stbtt_InitFont(&info, buffer.data(), stbtt_GetFontOffsetForIndex(buffer.data(), 0)))
    return false;
  if (stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint)) == 0)
    return false;  // codepoint absent from this build of the vendored font

  // The glyph fills §8's slot rather than the whole canvas: `kGlyphSize` tall,
  // in the box the crosshair leaves free. Multiplied by the caller's scale
  // like every other coordinate -- and note that this is the one shape in this
  // file that needs no stroke thickening, because asking stb_truetype for a
  // taller glyph thickens its strokes as a matter of course, which a Bresenham
  // line does not.
  const float glyphPx = static_cast<float>(kGlyphSize) * scale;
  const float fontScale = stbtt_ScaleForPixelHeight(&info, glyphPx);
  int gw = 0, gh = 0, xoff = 0, yoff = 0;
  unsigned char* bitmap = stbtt_GetCodepointBitmap(&info, fontScale, fontScale,
                                                   static_cast<int>(codepoint), &gw, &gh, &xoff, &yoff);
  if (bitmap == nullptr) return false;
  if (gw <= 0 || gh <= 0) {
    stbtt_FreeBitmap(bitmap, nullptr);
    return false;
  }

  // Centred inside the SLOT -- a cursor has no baseline to align to the way a
  // line of text does, so centring the glyph's own tight bitmap is the only
  // placement rule that means anything here. Centred in the slot rather than
  // in the canvas, so a glyph narrower than its box does not drift toward the
  // crosshair and crowd it.
  const int left = px(kGlyphLeft, scale), top = px(kGlyphTop, scale);
  const int size = px(kGlyphSize, scale);
  const int originX = left + (size - gw) / 2;
  const int originY = top + (size - gh) / 2;
  bool inked = false;
  for (int y = 0; y < gh; ++y)
    for (int x = 0; x < gw; ++x) {
      const uint8_t coverage = bitmap[static_cast<size_t>(y) * gw + x];
      if (coverage != 0) {
        setPixel(out.rgba, out.width, out.height, originX + x, originY + y, coverage);
        inked = true;
      }
    }
  stbtt_FreeBitmap(bitmap, nullptr);
  return inked;
}

// One `SDL_Cursor` from a base bitmap and its 2x alternate. Extracted so the
// per-tool loop and §9's single precise cursor build theirs the same way
// rather than by two copies of the same ownership dance.
//
// Null when the base surface could not be made; the caller treats that
// exactly as it treats a blank rasterisation.
SDL_Cursor* createColorCursorFrom(const CursorBitmap& base, const CursorBitmap& retina) {
  SDL_Surface* surface =
      SDL_CreateSurfaceFrom(base.width, base.height, SDL_PIXELFORMAT_RGBA32,
                            const_cast<uint8_t*>(base.rgba.data()), base.width * 4);
  if (surface == nullptr) return nullptr;

  // The 2x ALTERNATE representation -- the display backing scale, which is
  // the ONE axis this file still scales along. SDL adds one
  // `NSBitmapImageRep` per image to a single `NSImage` sized from the base,
  // so AppKit picks this one on a 2x display and the base on a 1x display,
  // and the cursor stays `kCursorBasePoints` points either way. Without it
  // the base is stretched and the cursor is visibly soft on every Mac made
  // in the last decade.
  //
  // Best-effort: a failure here leaves a perfectly usable 1x cursor rather
  // than no cursor, which is why nothing below is conditional on it.
  if (retina.nonBlank) {
    SDL_Surface* alt =
        SDL_CreateSurfaceFrom(retina.width, retina.height, SDL_PIXELFORMAT_RGBA32,
                              const_cast<uint8_t*>(retina.rgba.data()), retina.width * 4);
    if (alt != nullptr) {
      // `SDL_AddSurfaceAlternateImage()` takes its own reference, so this
      // surface is destroyed here and the alternate outlives it -- SDL's
      // own documented ownership for this call.
      SDL_AddSurfaceAlternateImage(surface, alt);
      SDL_DestroySurface(alt);
    }
  }

  // `SDL_CreateColorCursor()` copies what it needs out of the surface, so it
  // can be destroyed immediately after -- the same pattern SDL's own docs
  // show. The hotspot is in BASE-surface pixels, which is the same coordinate
  // space as the NSImage's points, so it does not get a second scaling here.
  SDL_Cursor* cursor = SDL_CreateColorCursor(surface, base.hotspotX, base.hotspotY);
  SDL_DestroySurface(surface);
  return cursor;
}

// **The funnel-point outline.** A black glyph is invisible against a black
// canvas -- exactly the report this whole change answers -- so every
// rasterised cursor gets a white halo drawn behind it, run here rather than
// inside `drawMarqueeShape()`, `drawLucideGlyph()` or `drawHotspotCrosshair()`,
// so every one of them stays unaware of it and a shape added to any of them
// gets the outline for free, with nothing to duplicate.
//
// A grayscale dilation of the ORIGINAL alpha channel, not a binary one: the
// stamped alpha is the source pixel's own coverage, so an anti-aliased glyph
// edge grows a softly anti-aliased halo instead of a hard-edged ring. A core
// (already-inked) pixel is never touched by the loop below -- only a
// neighbour that was fully transparent in the ORIGINAL bitmap gets painted --
// so "white ring, black glyph on top" falls out of that skip rather than
// needing a second compositing pass over the whole buffer.
//
// Drawn inside the EXISTING canvas, not a grown one: `rasterizeToolCursorBitmap()`
// hands back `round(kCursorDesignUnits * scale)` exactly, which
// `app/selftest/ToolCursor.cpp` section H pins byte-for-byte, so widening the
// buffer here to fit the halo would break that pin for a reason unrelated to
// what it actually guards. The margin already left in the 32-unit design
// space -- 5 units above and below the glyph's 22-unit height, and a couple
// of units around the marquee's own crosshair -- covers the 1-2px this draws
// at every scale `create()` actually asks for; the one place it does not (the
// marquee's crosshair arm already reaches its own canvas edge at the base
// scale) the halo simply clips there the same way the arm itself already
// does, which is a cosmetic loss on one edge of one cursor, not a defect.
void applyCursorOutline(CursorBitmap& bmp, float scale) {
  const int w = bmp.width, h = bmp.height;
  if (w <= 0 || h <= 0 || bmp.rgba.empty()) return;

  // The same formula `strokeWidth()` uses for a stroke's own thickness, so the
  // halo grows with scale the way every other shape in this file does, rather
  // than via a second constant that could quietly drift from it.
  const int r = strokeWidth(scale);

  const std::vector<uint8_t> core = bmp.rgba;  // the black ink, before any halo
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint8_t a = core[(static_cast<size_t>(y) * w + x) * 4 + 3];
      if (a == 0) continue;  // only an inked pixel spreads a halo
      for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
          if (dx * dx + dy * dy > r * r) continue;  // a disc footprint, not a square one
          const int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          const size_t idx = (static_cast<size_t>(ny) * w + nx) * 4;
          if (core[idx + 3] != 0) continue;  // core ink already lives here -- never overwritten
          uint8_t* p = &bmp.rgba[idx];
          // More than one inked pixel can reach the same halo texel; keep the
          // strongest coverage that reaches it rather than whichever wrote
          // last, so the result does not depend on iteration order.
          if (a > p[3]) {
            p[0] = p[1] = p[2] = 255;
            p[3] = a;
          }
        }
      }
    }
  }
}

}  // namespace

int cursorBasePoints() noexcept { return kCursorBasePoints; }

float cursorBaseScale() noexcept { return kCursorBaseScale; }


bool toolCursorPointsFromItsTip(Tool tool) noexcept {
  // Two members, and §10 argues why it is a short list rather than a policy.
  // A tool added here loses §8's crosshair, so the bar is "this cursor IS a
  // pointing thing" -- an arrow, a nib -- not "this icon has a pointy end",
  // which is most of them. `Tool::Curve` places anchors exactly as the Pen
  // does and is deliberately not here: `spline` is a curve, not a nib.
  return tool == Tool::PathSelect || tool == Tool::Pen;
}

bool toolHasBitmapCursor(Tool tool) noexcept {
  // The two marquees get §7's procedural composite -- the shape plus an offset
  // crosshair the report asked for by name -- and everything else gets its own
  // palette glyph. `Tool::Count` is the enum's bound rather than a tool.
  if (tool == Tool::Count) return false;
  if (tool == Tool::Marquee || tool == Tool::EllipseMarquee) return true;
  // Every other tool is a bitmap iff the palette has an icon for it. Asking
  // `toolIconCodepoint()` rather than restating a list is what keeps a change
  // to the palette's icon choice from silently leaving a cursor behind.
  return toolIconCodepoint(tool) != 0u;
}

CursorBitmap rasterizeToolCursorBitmap(Tool tool, float scale) noexcept {
  // A scale of zero or a NaN would produce a zero-sized canvas that then
  // "fails" the non-blank check for a reason that has nothing to do with the
  // font, sending a reader hunting for a missing Lucide file. Clamped instead.
  // The upper bound is generous rather than tight -- `create()` only ever asks
  // for `kCursorBaseScale` and twice that -- because this is a guard against
  // nonsense, not a policy about size.
  if (!(scale > 0.0f)) scale = 1.0f;
  scale = std::min(scale, 8.0f);

  CursorBitmap out;
  if (!toolHasBitmapCursor(tool)) return out;

  out.width = out.height = px(kCursorDesignUnits, scale);
  out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);

  // **The glyph first, and a failed glyph is a blank cursor.** The crosshair
  // below inks the canvas unconditionally, so the order here is what preserves
  // §7's fallback contract: a font that did not load must still produce a
  // bitmap `create()` refuses to install, not a crosshair with no tool on it.
  bool inked = false;
  if (toolCursorPointsFromItsTip(tool)) {
    // §10: no glyph slot and no crosshair. The shape occupies the canvas and
    // the hotspot is its own tip -- both generators set it, so the
    // `drawHotspotCrosshair()` call below must be skipped rather than merely
    // producing a mark nobody looks at: it would overwrite the hotspot.
    if (tool == Tool::Pen)
      drawPenNib(out, scale);
    else
      drawPointerArrow(out, scale);
    applyCursorOutline(out, scale);
    for (size_t i = 3; i < out.rgba.size(); i += 4)
      if (out.rgba[i] != 0) {
        out.nonBlank = true;
        break;
      }
    return out;
  }
  if (tool == Tool::Marquee) {
    // The two shapes no Lucide glyph carries -- the report described them in
    // words rather than by icon ("a circle or square with a crosshair to the
    // bottom left"), so §8 draws them. One generator with the shape as a
    // parameter, and it draws into the same slot a font glyph would.
    drawMarqueeShape(out, CursorMarqueeShape::Rectangle, scale);
    inked = true;
  } else if (tool == Tool::EllipseMarquee) {
    drawMarqueeShape(out, CursorMarqueeShape::Ellipse, scale);
    inked = true;
  } else {
    // The codepoint comes from `toolIconCodepoint()` (ui/AtelierChrome) -- the
    // tool palette's own single source of truth for which Lucide glyph a tool
    // means -- rather than a second, independent constant here. A future change
    // to the palette's icon choice then changes this cursor too, with no edit
    // in this file, instead of the two silently drifting apart the way
    // `strokeRouteFor()`'s own comment warns a restated predicate always
    // eventually does.
    inked = drawLucideGlyph(out, toolIconCodepoint(tool), scale);
  }
  if (!inked) return CursorBitmap{};  // blank, hotspot (0,0): §7's fallback

  // **The crosshair, and with it the hotspot -- for every tool, not five.**
  // This one call is the answer to "the click point is not always logical or
  // visible": logical because the hotspot is the crosshair's own centre rather
  // than a fraction of an icon's bounding box, and visible because there is
  // now something drawn at it.
  drawHotspotCrosshair(out, scale);

  // The white halo, applied here rather than inside either generator above --
  // see `applyCursorOutline()`'s own comment for why this one call covers
  // both the marquee composite and every Lucide glyph.
  applyCursorOutline(out, scale);

  // Non-blank iff at least one pixel actually carries ink -- computed here,
  // generically, over whatever the font path or the procedural path above
  // produced, rather than trusted from either generator's own return value.
  // This is the check hpp §7 promised: it is what turns a missing font file,
  // an absent codepoint, or (sabotage (a)) a deliberately zeroed rasteriser
  // into a loud `--selftest` line instead of a silent blank cursor.
  for (size_t i = 3; i < out.rgba.size(); i += 4) {
    if (out.rgba[i] != 0) {
      out.nonBlank = true;
      break;
    }
  }
  return out;
}

CursorBitmap rasterizePreciseCursorBitmap(float scale) noexcept {
  // The same clamp `rasterizeToolCursorBitmap()` applies, and for the same
  // reason -- see its comment. Restated rather than shared because there is no
  // third caller to hang a helper off, and the two are four lines apart.
  if (!(scale > 0.0f)) scale = 1.0f;
  scale = std::min(scale, 8.0f);

  CursorBitmap out;
  out.width = out.height = px(kCursorDesignUnits, scale);
  out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);

  // Centred, and the hotspot is the centre. Written here rather than through
  // `drawHotspotCrosshair()` because this crosshair is a different shape in a
  // different place -- sharing the function would mean parameterising it on
  // position and arm length, which is two arguments existing only so two
  // callers can disagree about both of them.
  const int t = strokeWidth(scale);
  const int c = px(kCursorDesignUnits, scale) / 2;
  const int arm = px(kPreciseArm, scale);
  drawLine(out.rgba, out.width, out.height, c - arm, c, c + arm, c, t);
  drawLine(out.rgba, out.width, out.height, c, c - arm, c, c + arm, t);
  out.hotspotX = out.hotspotY = c;

  applyCursorOutline(out, scale);
  for (size_t i = 3; i < out.rgba.size(); i += 4)
    if (out.rgba[i] != 0) {
      out.nonBlank = true;
      break;
    }
  return out;
}

bool shouldUsePreciseCursor(bool bitmapsEnabled, bool capsLock, std::optional<Tool> toolRequest,
                            bool hasPreciseBitmap) noexcept {
  // **Only over the canvas.** `toolRequest` is `nullopt` whenever the pointer
  // is over a panel, a menu or a window border, and Caps Lock must not turn
  // the I-beam in the LAYERS filter box into a crosshair -- Photoshop's own
  // override is a painting-cursor override, not a global one.
  //
  // Gated on `bitmapsEnabled` as well, because that flag means "this
  // platform's rasterisation is wrong"; a build that has fallen back to system
  // cursors has no business drawing this one either.
  return bitmapsEnabled && capsLock && toolRequest.has_value() && hasPreciseBitmap;
}

bool shouldUseBitmapCursor(bool bitmapsEnabled, std::optional<Tool> toolRequest,
                            bool hasBitmap) noexcept {
  // The whole of §7's flag. `bitmapsEnabled == false` makes this `false`
  // regardless of the other two arguments -- checked exhaustively for every
  // `Tool` value in `app/selftest/ToolCursor.cpp` section G, which is the
  // mechanical proof `ui/ToolCursor.hpp` §7 promises rather than an assertion
  // about one value.
  return bitmapsEnabled && toolRequest.has_value() && hasBitmap;
}

// --- the table itself (header §6, and §7's bitmap cursors) -----------------

void SystemCursorTable::create() noexcept {
  if (created_) return;
  for (int i = 0; i < SDL_SYSTEM_CURSOR_COUNT; ++i)
    cursors_[i] = SDL_CreateSystemCursor(static_cast<SDL_SystemCursor>(i));

  // §7's per-tool bitmap cursors, built here unconditionally alongside the
  // system set above. The FLAG decides which source `apply()` reaches for,
  // not whether this table exists -- `setBitmapCursorsEnabled()` is a plain
  // setter that can run after `create()` already has, so there is no other
  // point in the lifecycle to build these from.
  buildBitmapCursors();

  // Marked created even if some entries came back null: `apply()`'s fallback
  // covers a hole, and refusing to mark the table created because one exotic
  // resize cursor is unavailable on some platform would disable the pointer
  // entirely rather than degrade one shape.
  created_ = true;
}

void SystemCursorTable::buildBitmapCursors() noexcept {
  for (int i = 0; i < kToolCount; ++i) {
    const Tool tool = static_cast<Tool>(i);
    if (bitmapCursors_[i] != nullptr) {
      // A rebuild, not a first build. Destroyed before the slot is
      // overwritten, or every rebuild would leak one OS cursor per tool.
      // `last_` is cleared below for the same reason `destroy()` clears it:
      // it may be pointing at one of these.
      SDL_DestroyCursor(bitmapCursors_[i]);
      bitmapCursors_[i] = nullptr;
    }
    if (!toolHasBitmapCursor(tool)) continue;

    // The BASE representation, at `kCursorBasePoints`. Its pixel dimensions
    // are also the cursor's POINT dimensions on macOS -- SDL's
    // `Cocoa_CreateImage()` sets `NSImage.size` from this surface -- and the
    // OS applies the user's Accessibility pointer size on top of that itself.
    // See `kCursorBasePoints` for the measurement, and for the earlier
    // revision that scaled here too and shipped a cursor three times too big.
    const CursorBitmap bitmap = rasterizeToolCursorBitmap(tool, cursorBaseScale());
    // §7's fallback rule, enforced at the one place that knows both the
    // pixels and the table they would join: a blank rasterisation is never
    // installed as a cursor. Left null, `bitmapCursorFor()` answers exactly
    // what it answers for a tool with no bitmap at all, and `apply()` falls
    // back to `sdlCursorFor()`'s system shape -- objection 1's answer.
    if (!bitmap.nonBlank) continue;
    bitmapCursors_[i] = createColorCursorFrom(
        bitmap, rasterizeToolCursorBitmap(tool, cursorBaseScale() * 2.0f));
  }

  // §9's one precise cursor, built beside the per-tool ones and under the same
  // blank-means-fall-back rule. Null here simply means Caps Lock does nothing,
  // which `shouldUsePreciseCursor()`'s `hasPreciseBitmap` argument is how
  // `apply()` finds out.
  if (preciseCursor_ != nullptr) {
    SDL_DestroyCursor(preciseCursor_);
    preciseCursor_ = nullptr;
  }
  const CursorBitmap precise = rasterizePreciseCursorBitmap(cursorBaseScale());
  if (precise.nonBlank)
    preciseCursor_ =
        createColorCursorFrom(precise, rasterizePreciseCursorBitmap(cursorBaseScale() * 2.0f));

  last_ = nullptr;
}

void SystemCursorTable::destroy() noexcept {
  for (SDL_Cursor*& c : cursors_) {
    if (c != nullptr) SDL_DestroyCursor(c);
    c = nullptr;
  }
  for (SDL_Cursor*& c : bitmapCursors_) {
    if (c != nullptr) SDL_DestroyCursor(c);
    c = nullptr;
  }
  if (preciseCursor_ != nullptr) SDL_DestroyCursor(preciseCursor_);
  preciseCursor_ = nullptr;
  // Cleared so a `destroy()`/`create()` pair leaves no pointer to a freed
  // cursor behind for the skip-if-unchanged check to compare against -- that
  // comparison would be against a dangling value, and a freed allocation can
  // be handed back at the same address.
  last_ = nullptr;
  created_ = false;
}

SDL_Cursor* SystemCursorTable::bitmapCursorFor(Tool tool) const noexcept {
  const int index = static_cast<int>(tool);
  if (index < 0 || index >= kToolCount) return nullptr;
  return bitmapCursors_[index];
}

void SystemCursorTable::apply(std::optional<SDL_SystemCursor> request,
                              std::optional<Tool> toolRequest, bool capsLock) noexcept {
  // `--selftest` and the demo paths never call `create()`, and must not be made
  // to: several of them make a window but none draws a frame through this.
  if (!created_) return;

  const ImGuiIO& io = ImGui::GetIO();
  const ImGuiMouseCursor imguiCursor = ImGui::GetMouseCursor();

  // Branch one of the function this replaces. **Before any indexing**, because
  // `ImGuiMouseCursor_None` is -1 and using it as a subscript reads off the
  // front of the table. `io.MouseDrawCursor` means ImGui is drawing a pointer
  // into the vertex stream itself, so leaving the OS one visible would show
  // two.
  if (io.MouseDrawCursor || imguiCursor == ImGuiMouseCursor_None) {
    SDL_HideCursor();
    // `last_` is deliberately NOT cleared, matching the backend: SDL remembers
    // the active cursor across hide/show, so the shape is still correct when
    // the next frame shows it again and re-setting it would be a wasted call.
    return;
  }

  // **The whole rule.** The canvas's request wins when there is one -- which is
  // only ever while the pointer is over the canvas, or holding a drag that
  // started there -- and ImGui's own request is honoured everywhere else. That
  // second half is what keeps the I-beam in the filter box, the resize arrows
  // on a window border and the pointer over a menu behaving exactly as they did
  // before the backend was suppressed.
  const SDL_SystemCursor want =
      request.has_value() ? *request : sdlCursorForImGui(imguiCursor);

  // §7's one new branch. `hasBitmap` is true only when a bitmap cursor was
  // actually built for this tool -- which already folds in both
  // `toolHasBitmapCursor()` and `create()`'s own blank-rasterisation check,
  // since a blank one was never stored. Passed
  // through `shouldUseBitmapCursor()` rather than inlined so the identical
  // decision `--selftest` proves flag-off-identical for is the one actually
  // running here, not a lookalike.
  SDL_Cursor* chosen = nullptr;
  const bool hasBitmap = toolRequest.has_value() && bitmapCursorFor(*toolRequest) != nullptr;

  // **§9's override, tested BEFORE the per-tool bitmap**, because that is what
  // "regardless of what the tool would otherwise show" means. Both decisions
  // go through their own pure predicate rather than being inlined here, so the
  // logic `--selftest` proves is the logic that runs -- `apply()` itself needs
  // live SDL video and is the one function in this file no test can reach.
  if (shouldUsePreciseCursor(bitmapsEnabled_, capsLock, toolRequest, preciseCursor_ != nullptr))
    chosen = preciseCursor_;
  else if (shouldUseBitmapCursor(bitmapsEnabled_, toolRequest, hasBitmap))
    chosen = bitmapCursorFor(*toolRequest);

  // §6's original fallback, byte-for-byte: reached whenever the branch above
  // did not choose a bitmap -- flag off, no tool request this frame, or a
  // tool request whose bitmap does not exist -- which is EVERY frame before
  // this section existed and every frame with the flag off after it.
  if (chosen == nullptr) {
    chosen = cursors_[want];
    // `SDL_CreateSystemCursor()` can fail for a shape a platform does not
    // provide. Falling back to the arrow keeps a pointer on screen; passing
    // the null through would set no cursor at all.
    if (chosen == nullptr) chosen = cursors_[SDL_SYSTEM_CURSOR_DEFAULT];
  }
  if (chosen != nullptr && chosen != last_) {
    SDL_SetCursor(chosen);
    last_ = chosen;
  }

  // Every frame of the visible branch, not only when the shape changed -- this
  // is what brings the pointer back after a frame that hid it, and it is why
  // the backend called it unconditionally here too.
  SDL_ShowCursor();
}

const char* toolCursorName(ToolCursor cursor) noexcept {
  switch (cursor) {
    case ToolCursor::Arrow:
      return "arrow";
    case ToolCursor::Paint:
      return "paint";
    case ToolCursor::Select:
      return "select";
    case ToolCursor::Sample:
      return "sample";
    case ToolCursor::Pan:
      return "pan";
    case ToolCursor::Zoom:
      return "zoom";
    case ToolCursor::MoveObject:
      return "move";
    case ToolCursor::Text:
      return "text";
    case ToolCursor::Refuse:
      return "refuse";
  }
  return "?";
}

}  // namespace np
