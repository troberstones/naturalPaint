#include "ui/ToolCursor.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>

#include "app/StrokeSession.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/PointerScale.hpp"

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

    // T17 (docs/testing-issues.md): these five used to fall through to the
    // same `Select` case above, which is what made a lasso drag and a wand
    // click show the identical "drag a rectangle" diagonal arrow. Each now
    // has its own intent, and hpp §7 is the bitmap each one now gets. All
    // five still project to `SDL_SYSTEM_CURSOR_NWSE_RESIZE` through
    // `sdlCursorFor()` below -- that projection is now the FALLBACK path
    // (§7's flag off, or a bitmap that failed to rasterise), not the normal
    // one, and it is deliberately left as it was so "bitmaps off" remains
    // byte-identical to the behaviour before §7 existed.
    case Tool::Marquee:
      return ToolCursor::SelectMarquee;
    case Tool::EllipseMarquee:
      return ToolCursor::SelectEllipseMarquee;
    case Tool::Lasso:
      return ToolCursor::SelectLasso;
    case Tool::PolygonLasso:
      return ToolCursor::SelectPolygonLasso;
    case Tool::MagicWand:
      return ToolCursor::SelectMagicWand;

    // --- the view, and content within it ----------------------------------
    case Tool::Hand:
      return ToolCursor::Pan;
    case Tool::Zoom:
      return ToolCursor::Zoom;

    // Move and Frame share `kToolGroups`' first slot, and share a meaning:
    // something already on the canvas follows the drag.
    case Tool::Move:
    case Tool::Frame:
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
    case ToolCursor::Select:
    // The five values hpp §7 split out of `Select` project to the SAME shape
    // here as `Select` itself -- this line is the whole of "flag off is
    // byte-identical to today": before the split, all five tools answered
    // `Select` and got this cursor; after it, each answers its own intent but
    // still arrives here, because `SystemCursorTable::apply()` only reaches
    // for a bitmap when `bitmapsEnabled_` is true. Changing any one of these
    // five without also changing what the tool showed before this split
    // would BE the accessibility-flag default drifting silently, which
    // `app/selftest/ToolCursor.cpp` section G checks per tool, not just per
    // enum value.
    case ToolCursor::SelectMarquee:
    case ToolCursor::SelectEllipseMarquee:
    case ToolCursor::SelectLasso:
    case ToolCursor::SelectPolygonLasso:
    case ToolCursor::SelectMagicWand:
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
// written in these 32 units and multiplied by a `scale` on the way to
// pixels, because a cursor now has TWO independent reasons to be bigger than
// 32 pixels: the user's accessibility pointer size (ui/PointerScale.hpp --
// macOS will not scale a custom cursor for us, so we scale it ourselves) and
// the display's backing scale (a 2x alternate representation, which SDL turns
// into the second rep of one NSImage). Those multiply, so the largest canvas
// this file rasterises is 32 * 4.0 * 2 = 256 px square.
//
// 32 was chosen because Windows' own cursor bitmaps are 32x32 and macOS
// accepts whatever a surface hands `SDL_CreateColorCursor()`; as a design
// space rather than a pixel count it now only sets the proportions.
constexpr int kCursorDesignUnits = 32;

void setPixel(std::vector<uint8_t>& rgba, int w, int h, int x, int y, uint8_t a) {
  if (x < 0 || y < 0 || x >= w || y >= h) return;
  // Black ink at the sampled coverage, opaque everywhere it is drawn at all
  // -- every shape this file draws is one colour, so there is no blending
  // beyond the alpha stb_truetype itself already anti-aliased.
  uint8_t* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
  p[0] = p[1] = p[2] = 0;
  p[3] = a;
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

// The marquee composite hpp §7 promised: ONE generator taking the shape as a
// parameter, not two hand-drawn cursors, because the crosshair and its
// position are identical between the two marquees and the shape is the only
// thing the report asked to vary.
CursorBitmap drawMarqueeCrosshair(CursorMarqueeShape shape, float scale) {
  CursorBitmap out;
  out.width = out.height = px(kCursorDesignUnits, scale);
  out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
  const int t = strokeWidth(scale);

  // The shape: a 20x20 box in the canvas's upper-right, offset so its own
  // bottom-left corner (10, 22) sits well clear of the crosshair below.
  // Design units; `px()` is the only place they become pixels.
  constexpr int kLeft = 10, kTop = 2, kSize = 20;
  const int left = px(kLeft, scale), top = px(kTop, scale), size = px(kSize, scale);
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

  // The crosshair: bottom-left of the canvas. 4 units of clearance from the
  // shape's own bottom-left corner on both axes -- close enough to read as
  // "attached to the same cursor", never touching it.
  constexpr int kCrossX = 6, kCrossY = 26, kArm = 5;
  const int crossX = px(kCrossX, scale), crossY = px(kCrossY, scale), arm = px(kArm, scale);
  drawLine(out.rgba, out.width, out.height, crossX - arm, crossY, crossX + arm, crossY, t);
  drawLine(out.rgba, out.width, out.height, crossX, crossY - arm, crossX, crossY + arm, t);

  // **The hotspot is the crosshair's own centre, not the shape's corner and
  // not the canvas's centre.** This is the bug the report is actually
  // describing: a marquee's drag starts at the exact pixel under the
  // pointer, and the crosshair is what tells the user which pixel that is --
  // so the OS has to agree. `app/selftest/ToolCursor.cpp` section G asserts
  // this specifically, not merely that the hotspot sits somewhere inked.
  //
  // Scaled with everything else, and by the SAME `px()` the crosshair's own
  // arms went through -- not by a second rounding of the same product, which
  // is how a hotspot drifts a pixel off its own crosshair at some scales and
  // not others. `app/selftest/ToolCursor.cpp` section G checks the identity
  // at several scales rather than only at 1.0.
  out.hotspotX = crossX;
  out.hotspotY = crossY;
  return out;
}

// Rasterises one Lucide codepoint through stb_truetype directly -- no
// `ImFontAtlas`, no `GImGui`, see this section's own opening comment for why.
// Returns an all-transparent, zero-hotspot `CursorBitmap` (which
// `rasterizeToolCursorBitmap()`'s generic non-blank scan will then correctly
// call blank) for a missing file, an unreadable file, or a codepoint the
// vendored font build does not contain -- the exact three failure modes
// `ui/ToolCursor.hpp`'s original §1 worried a bitmap cursor could hit
// silently.
CursorBitmap rasterizeLucideGlyphCursor(uint32_t codepoint, float scale) {
  CursorBitmap out;
  out.width = out.height = px(kCursorDesignUnits, scale);
  out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);

#ifndef NP_LUCIDE_TTF
  return out;
#else
  std::ifstream file(NP_LUCIDE_TTF, std::ios::binary);
  if (!file.is_open()) return out;
  const std::vector<unsigned char> buffer((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
  if (buffer.empty()) return out;

  stbtt_fontinfo info;
  if (!stbtt_InitFont(&info, buffer.data(), stbtt_GetFontOffsetForIndex(buffer.data(), 0)))
    return out;
  if (stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint)) == 0)
    return out;  // codepoint absent from this build of the vendored font

  // 22 units of glyph height inside the 32-unit canvas: 5 units of margin top
  // and bottom for the anti-aliased edge, the same headroom
  // `installToolIconFont()` leaves around its own 15px icons, scaled up for a
  // cursor. Multiplied by the caller's scale like every other coordinate --
  // and note that this is the one shape in this file that needs no stroke
  // thickening, because asking stb_truetype for a taller glyph thickens its
  // strokes as a matter of course, which a Bresenham line does not.
  const float glyphPx = 22.0f * scale;
  const float fontScale = stbtt_ScaleForPixelHeight(&info, glyphPx);
  int gw = 0, gh = 0, xoff = 0, yoff = 0;
  unsigned char* bitmap = stbtt_GetCodepointBitmap(&info, fontScale, fontScale,
                                                   static_cast<int>(codepoint), &gw, &gh, &xoff, &yoff);
  if (bitmap == nullptr) return out;
  if (gw <= 0 || gh <= 0) {
    stbtt_FreeBitmap(bitmap, nullptr);
    return out;
  }

  // Centred in the canvas -- a cursor has no baseline to align to the way a
  // line of text does, so centring the glyph's own tight bitmap is the only
  // placement rule that means anything here.
  const int originX = (out.width - gw) / 2;
  const int originY = (out.height - gh) / 2;
  for (int y = 0; y < gh; ++y)
    for (int x = 0; x < gw; ++x) {
      const uint8_t coverage = bitmap[static_cast<size_t>(y) * gw + x];
      if (coverage != 0) setPixel(out.rgba, out.width, out.height, originX + x, originY + y, coverage);
    }
  stbtt_FreeBitmap(bitmap, nullptr);

  // Hotspot: the glyph's own bounding-box centre. This file has no per-icon
  // design input to place it more precisely -- a lasso's natural hotspot is
  // where its loop closes, a wand's is its tip, and neither is recoverable
  // from rasterised coverage alone -- so centre-of-glyph is the one placement
  // every shape can justify without guessing. It is provably inside the
  // glyph (section G), and it is still strictly more informative than the
  // corner-ish hotspot of the `NWSE_RESIZE` arrow it replaces.
  out.hotspotX = originX + gw / 2;
  out.hotspotY = originY + gh / 2;
  return out;
#endif
}

}  // namespace

bool toolCursorHasBitmap(ToolCursor cursor) noexcept {
  switch (cursor) {
    case ToolCursor::SelectMarquee:
    case ToolCursor::SelectEllipseMarquee:
    case ToolCursor::SelectLasso:
    case ToolCursor::SelectPolygonLasso:
    case ToolCursor::SelectMagicWand:
      return true;
    case ToolCursor::Arrow:
    case ToolCursor::Paint:
    case ToolCursor::Select:
    case ToolCursor::Sample:
    case ToolCursor::Pan:
    case ToolCursor::Zoom:
    case ToolCursor::MoveObject:
    case ToolCursor::Text:
    case ToolCursor::Refuse:
      return false;
  }
  return false;
}

CursorBitmap rasterizeToolCursorBitmap(ToolCursor cursor, float scale) noexcept {
  // A scale of zero or a NaN would produce a zero-sized canvas that then
  // "fails" the non-blank check for a reason that has nothing to do with the
  // font. Clamped to the same range ui/PointerScale.hpp clamps its own
  // reading to, times the 2x backing alternate, so no caller can ask this
  // function for a size `create()` would not have allocated anyway.
  if (!(scale > 0.0f)) scale = 1.0f;
  scale = std::min(scale, 8.0f);

  CursorBitmap out;
  switch (cursor) {
    case ToolCursor::SelectMarquee:
      out = drawMarqueeCrosshair(CursorMarqueeShape::Rectangle, scale);
      break;
    case ToolCursor::SelectEllipseMarquee:
      out = drawMarqueeCrosshair(CursorMarqueeShape::Ellipse, scale);
      break;
    // The codepoints are read from `toolIconCodepoint()` (ui/AtelierChrome)
    // -- the tool palette's own single source of truth for which Lucide
    // glyph a tool means -- rather than a second, independent constant here.
    // A future change to the palette's icon choice then changes this cursor
    // too, with no edit in this file, instead of the two silently drifting
    // apart the way `strokeRouteFor()`'s own comment warns a restated
    // predicate always eventually does.
    case ToolCursor::SelectLasso:
      out = rasterizeLucideGlyphCursor(toolIconCodepoint(Tool::Lasso), scale);
      break;
    case ToolCursor::SelectPolygonLasso:
      out = rasterizeLucideGlyphCursor(toolIconCodepoint(Tool::PolygonLasso), scale);
      break;
    case ToolCursor::SelectMagicWand:
      out = rasterizeLucideGlyphCursor(toolIconCodepoint(Tool::MagicWand), scale);
      break;
    default:
      // `toolCursorHasBitmap()` is false for every other value; returning the
      // all-transparent default here is correct for those AND is what a
      // future bitmap-less value falls back to if this switch is ever
      // reached before that function is updated to match.
      return out;
  }

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

bool shouldUseBitmapCursor(bool bitmapsEnabled, std::optional<ToolCursor> toolRequest,
                            bool hasBitmap) noexcept {
  // The whole of §7's flag. `bitmapsEnabled == false` makes this `false`
  // regardless of the other two arguments -- checked exhaustively for every
  // `ToolCursor` value in `app/selftest/ToolCursor.cpp` section G, which is
  // the mechanical proof `ui/ToolCursor.hpp` §7 promises rather than an
  // assertion about one value.
  return bitmapsEnabled && toolRequest.has_value() && hasBitmap;
}

// --- the table itself (header §6, and §7's bitmap cursors) -----------------

void SystemCursorTable::create() noexcept {
  if (created_) return;
  for (int i = 0; i < SDL_SYSTEM_CURSOR_COUNT; ++i)
    cursors_[i] = SDL_CreateSystemCursor(static_cast<SDL_SystemCursor>(i));

  // §7's five bitmap cursors, built here unconditionally alongside the
  // system set above. The FLAG decides which source `apply()` reaches for,
  // not whether this table exists -- `setBitmapCursorsEnabled()` is a plain
  // setter that can run after `create()` already has, so there is no other
  // point in the lifecycle to build these from.
  pointerScale_ = osPointerSizeScale();
  buildBitmapCursors();

  // Marked created even if some entries came back null: `apply()`'s fallback
  // covers a hole, and refusing to mark the table created because one exotic
  // resize cursor is unavailable on some platform would disable the pointer
  // entirely rather than degrade one shape.
  created_ = true;
}

void SystemCursorTable::buildBitmapCursors() noexcept {
  for (int i = 0; i < kToolCursorCount; ++i) {
    const ToolCursor cursor = static_cast<ToolCursor>(i);
    if (bitmapCursors_[i] != nullptr) {
      // A rebuild, not a first build. Destroyed before the slot is
      // overwritten, or every pointer-size change would leak five OS cursors.
      // `last_` is cleared below for the same reason `destroy()` clears it:
      // it may be pointing at one of these.
      SDL_DestroyCursor(bitmapCursors_[i]);
      bitmapCursors_[i] = nullptr;
    }
    if (!toolCursorHasBitmap(cursor)) continue;

    // The BASE representation, at the user's accessibility pointer size. Its
    // pixel dimensions are also the cursor's POINT dimensions on macOS (SDL's
    // `Cocoa_CreateImage()` sets `NSImage.size` from this surface), which is
    // what makes the scaling here the thing the user actually sees.
    const CursorBitmap bitmap = rasterizeToolCursorBitmap(cursor, pointerScale_);
    // §7's fallback rule, enforced at the one place that knows both the
    // pixels and the table they would join: a blank rasterisation is never
    // installed as a cursor. Left null, `bitmapCursorFor()` answers exactly
    // what it answers for a tool with no bitmap at all, and `apply()` falls
    // back to `sdlCursorFor()`'s system shape -- objection 1's answer.
    if (!bitmap.nonBlank) continue;
    SDL_Surface* surface =
        SDL_CreateSurfaceFrom(bitmap.width, bitmap.height, SDL_PIXELFORMAT_RGBA32,
                              const_cast<uint8_t*>(bitmap.rgba.data()), bitmap.width * 4);
    if (surface == nullptr) continue;

    // The 2x ALTERNATE representation -- the Retina half of §7's scaling
    // story, and a different axis from the accessibility scale above. SDL
    // adds one `NSBitmapImageRep` per image to a single `NSImage` sized from
    // the base, so AppKit picks this one on a 2x display and the base on a 1x
    // display. Without it the base is stretched and the cursor is visibly
    // soft on every Mac made in the last decade.
    //
    // Best-effort: a failure here leaves a perfectly usable 1x cursor rather
    // than no cursor, which is why nothing below is conditional on it.
    const CursorBitmap retina = rasterizeToolCursorBitmap(cursor, pointerScale_ * 2.0f);
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

    // `SDL_CreateColorCursor()` copies what it needs out of the surface, so
    // it can be destroyed immediately after -- the same pattern SDL's own
    // docs show, and there is no reason to keep `bitmap.rgba` alive past
    // this call either; it goes out of scope with the loop body. The hotspot
    // is in BASE-surface pixels, which is the same coordinate space as the
    // NSImage's points, so it does not get a second scaling here.
    bitmapCursors_[i] = SDL_CreateColorCursor(surface, bitmap.hotspotX, bitmap.hotspotY);
    SDL_DestroySurface(surface);
  }
  last_ = nullptr;
}

bool SystemCursorTable::refreshPointerScale() noexcept {
  if (!created_) return false;
  const float now = osPointerSizeScale();
  // A float compare with a real epsilon, not `!=`: the preference is a slider
  // position stored as a double (2.0724539756774902 on the machine this was
  // written on), so an exact compare would be at the mercy of the last bit
  // and a rebuild of five OS cursors is not free enough to do on noise.
  // 0.01 is finer than any size change a user could see.
  if (std::abs(now - pointerScale_) < 0.01f) return false;
  pointerScale_ = now;
  buildBitmapCursors();
  return true;
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
  // Cleared so a `destroy()`/`create()` pair leaves no pointer to a freed
  // cursor behind for the skip-if-unchanged check to compare against -- that
  // comparison would be against a dangling value, and a freed allocation can
  // be handed back at the same address.
  last_ = nullptr;
  created_ = false;
}

SDL_Cursor* SystemCursorTable::bitmapCursorFor(ToolCursor cursor) const noexcept {
  const int index = static_cast<int>(cursor);
  if (index < 0 || index >= kToolCursorCount) return nullptr;
  return bitmapCursors_[index];
}

void SystemCursorTable::apply(std::optional<SDL_SystemCursor> request,
                              std::optional<ToolCursor> toolRequest) noexcept {
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
  // `toolCursorHasBitmap()` (is this one of the five) and `create()`'s own
  // blank-rasterisation check, since a blank one was never stored. Passed
  // through `shouldUseBitmapCursor()` rather than inlined so the identical
  // decision `--selftest` proves flag-off-identical for is the one actually
  // running here, not a lookalike.
  SDL_Cursor* chosen = nullptr;
  const bool hasBitmap = toolRequest.has_value() && bitmapCursorFor(*toolRequest) != nullptr;
  if (shouldUseBitmapCursor(bitmapsEnabled_, toolRequest, hasBitmap)) chosen = bitmapCursorFor(*toolRequest);

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
    case ToolCursor::SelectMarquee:
      return "select-marquee";
    case ToolCursor::SelectEllipseMarquee:
      return "select-ellipse-marquee";
    case ToolCursor::SelectLasso:
      return "select-lasso";
    case ToolCursor::SelectPolygonLasso:
      return "select-polygon-lasso";
    case ToolCursor::SelectMagicWand:
      return "select-magic-wand";
  }
  return "?";
}

}  // namespace np
