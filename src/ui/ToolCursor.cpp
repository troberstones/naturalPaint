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
CursorBitmap rasterizeLucideGlyphCursor(uint32_t codepoint, float scale,
                                        CursorHotspotAnchor anchor) {
  CursorBitmap out;
  out.width = out.height = px(kCursorDesignUnits, scale);
  out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);

  // core/ResourcePaths.hpp: tries the executable-relative and override
  // locations before the compile-time path, so a copied binary still finds
  // the vendored font here, same as installToolIconFont() above it in
  // ui/Fonts.cpp. A missing file already falls through to the blank,
  // zero-hotspot bitmap this function's own header comment documents, so no
  // separate report is added here -- resolveResourcePath() already wrote
  // every location it tried to stderr if none of them existed.
  std::ifstream file(lucideTtfPath(), std::ios::binary);
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

  // **The hotspot, placed against the glyph's own bounding box rather than at
  // its centre.** An earlier revision used centre-of-glyph for every icon, on
  // the reasoning that a tip is not recoverable from rasterised coverage --
  // which is true, and was the wrong conclusion: it is not recoverable from
  // the PIXELS, but it is perfectly well known to whoever chose the icon. A
  // lasso draws from the end of its tail, not from the middle of its loop,
  // and pointing at the middle of the loop is the same class of defect as the
  // resize arrow T17 started from: a cursor that does not say where the click
  // lands. `cursorHotspotAnchorFor()` is that per-tool knowledge, expressed
  // as a fraction of this bounding box so it survives every scale.
  // Resolved against the INKED bounding box, not against stb_truetype's glyph
  // metrics. The two differ: a glyph's metric box can carry an edge row whose
  // anti-aliased coverage rounds to zero, and `setPixel()` skips those, so an
  // anchor of 1.0 against the metric box lands one row PAST the last visible
  // pixel -- a hotspot on a transparent texel, which is precisely what section
  // G exists to catch. Measured, not reasoned about: eight of the twenty-eight
  // tools failed that assertion when this was resolved against `gw`/`gh`.
  int minX = out.width, minY = out.height, maxX = -1, maxY = -1;
  for (int y = 0; y < out.height; ++y)
    for (int x = 0; x < out.width; ++x)
      if (out.rgba[(static_cast<size_t>(y) * out.width + x) * 4 + 3] != 0) {
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
      }
  if (maxX < 0) return out;  // nothing inked: `nonBlank` stays false, hotspot stays (0,0)
  out.hotspotX = minX + static_cast<int>(std::lround(anchor.fx * (maxX - minX)));
  out.hotspotY = minY + static_cast<int>(std::lround(anchor.fy * (maxY - minY)));
  return out;
}

// **The funnel-point outline.** A black glyph is invisible against a black
// canvas -- exactly the report this whole change answers -- so every
// rasterised cursor gets a white halo drawn behind it, run here rather than
// inside `drawMarqueeCrosshair()` or `rasterizeLucideGlyphCursor()` so both
// glyph-drawing paths stay unaware of it and a tool added to either one gets
// the outline for free, with nothing to duplicate.
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

CursorHotspotAnchor cursorHotspotAnchorFor(Tool tool) noexcept {
  // **Read off the glyphs, not guessed.** Each fraction below was chosen by
  // rasterising the Lucide icon this tool actually uses and looking at where
  // its working point is: the end of the lasso's tail, the nib of the pen,
  // the drip under the tipped bucket, the centre of the magnifier's lens.
  // Anything genuinely symmetric -- a hand, a move cross, the gradient's two
  // circles -- is the centre because the centre IS its working point, not
  // because nothing better was available.
  switch (tool) {
    // A tip that touches the canvas at its lower-left end.
    case Tool::Lasso:
    case Tool::Brush:
    case Tool::Water:
    case Tool::DryBrush:
    case Tool::Pencil:
    case Tool::Eraser:
    case Tool::Smudge:
    case Tool::CloneStamp:
    case Tool::Eyedropper:
    case Tool::Measure:
    case Tool::Pen:
    case Tool::Curve:
      return {0.0f, 1.0f};

    // The pentagon standing in for the polygon lasso: a polygon is built
    // vertex by vertex and its apex is the vertex the icon leads with.
    case Tool::PolygonLasso:
      return {0.5f, 0.0f};

    // The wand's tip is the top-right end of the shaft, where its sparkles are.
    case Tool::MagicWand:
      return {1.0f, 0.0f};

    // The bucket is drawn tipped, pouring; the drip leaves at lower-right, and
    // that drip is where the fill lands.
    case Tool::PaintBucket:
      return {0.9f, 1.0f};

    // The magnifier's LENS centre, not the icon's centre -- the icon is a lens
    // plus a handle running to the lower right, so its bounding-box centre sits
    // on the glass's edge rather than in the middle of it.
    case Tool::Zoom:
      return {0.4f, 0.4f};

    // Genuinely centred: what these tools act on is under the middle of the
    // shape, so the centre is a choice here rather than a default.
    case Tool::Hand:
    case Tool::Move:
    case Tool::Frame:
    case Tool::Gradient:
    case Tool::Crop:
    case Tool::Slice:
    case Tool::Shape:
    case Tool::Text:
    case Tool::Dodge:
    case Tool::Burn:
    case Tool::Marquee:
    case Tool::EllipseMarquee:
    case Tool::Count:
      return {0.5f, 0.5f};
  }
  return {0.5f, 0.5f};
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

  // The two the report described in words rather than by icon: "a circle or
  // square with a crosshair to the bottom left". No Lucide glyph is that
  // composite, so §7 draws it -- one generator with the shape as a parameter.
  if (tool == Tool::Marquee) {
    out = drawMarqueeCrosshair(CursorMarqueeShape::Rectangle, scale);
  } else if (tool == Tool::EllipseMarquee) {
    out = drawMarqueeCrosshair(CursorMarqueeShape::Ellipse, scale);
  } else {
    // The codepoint comes from `toolIconCodepoint()` (ui/AtelierChrome) -- the
    // tool palette's own single source of truth for which Lucide glyph a tool
    // means -- rather than a second, independent constant here. A future change
    // to the palette's icon choice then changes this cursor too, with no edit
    // in this file, instead of the two silently drifting apart the way
    // `strokeRouteFor()`'s own comment warns a restated predicate always
    // eventually does.
    out = rasterizeLucideGlyphCursor(toolIconCodepoint(tool), scale,
                                     cursorHotspotAnchorFor(tool));
  }

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
    SDL_Surface* surface =
        SDL_CreateSurfaceFrom(bitmap.width, bitmap.height, SDL_PIXELFORMAT_RGBA32,
                              const_cast<uint8_t*>(bitmap.rgba.data()), bitmap.width * 4);
    if (surface == nullptr) continue;

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
    const CursorBitmap retina = rasterizeToolCursorBitmap(tool, cursorBaseScale() * 2.0f);
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

SDL_Cursor* SystemCursorTable::bitmapCursorFor(Tool tool) const noexcept {
  const int index = static_cast<int>(tool);
  if (index < 0 || index >= kToolCount) return nullptr;
  return bitmapCursors_[index];
}

void SystemCursorTable::apply(std::optional<SDL_SystemCursor> request,
                              std::optional<Tool> toolRequest) noexcept {
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
  }
  return "?";
}

}  // namespace np
