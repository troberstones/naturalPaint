#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "app/AppState.hpp"
#include "core/Layer.hpp"

#include <SDL3/SDL_mouse.h>

#include "imgui.h"

// ui/ToolCursor -- **what the pointer says the next click will do.**
//
// The user's report was "we need to change the cursor to match the tool", and
// before this file the application made exactly zero cursor calls: no
// `SetMouseCursor`, no `SDL_SetCursor`, no `SDL_CreateCursor` anywhere in
// `src/`. The OS arrow sat over the canvas whether the next press would deposit
// pigment, drag a marquee, sample a colour, pan the view -- or be refused
// outright and do nothing at all.
//
// ==========================================================================
// 1. SDL system cursors, and why this module owns the cursor outright
// ==========================================================================
//
// **Decision: SDL3 system cursors, with this build as the only writer.** Not
// custom bitmaps, and not Dear ImGui's built-in set. Each was rejected for its
// own reason and the reasons are worth keeping.
//
// **Not bitmaps.** The Lucide glyph path (`ui/Fonts.hpp`, `NP_LUCIDE_TTF`) is a
// **compile-time absolute path that degrades silently** -- a build whose font is
// missing draws no icons and says nothing. A cursor cut from that font would
// then be a *blank* cursor: the pointer would vanish over the canvas. An arrow
// that conveys too little is a poor cursor; a pointer that is not there is a
// broken application. Bitmaps also do not scale with the OS cursor-size
// accessibility setting, ignore the user's cursor theme, and need a hand-picked
// hotspot per tool that nothing in `--selftest` could check. System cursors get
// all three right for free.
//
// **Not ImGui's set, because it has no crosshair.** The whole enum is Arrow,
// TextInput, ResizeAll, ResizeNS, ResizeEW, ResizeNESW, ResizeNWSE, Hand, Wait,
// Progress, NotAllowed (imgui.h `enum ImGuiMouseCursor_`) and that is the
// complete list. SDL3 *does* have `SDL_SYSTEM_CURSOR_CROSSHAIR`, but the ImGui
// SDL3 backend builds exactly one system cursor per ImGui value
// (imgui_impl_sdl3.cpp lines 624-634), so nothing reaches it through
// `ImGui::SetMouseCursor()`. Routing the tools through ImGui would have left
// **Brush, Marquee, Lasso, MagicWand, Eyedropper and PaintBucket all a plain
// arrow** -- the six tools anyone actually uses, sharing one pointer. That is
// not "change the cursor to match the tool"; that is the refusal cursor plus
// nothing.
//
// **How the fight with the backend is removed rather than won.**
// `ImGui_ImplSDL3_UpdateMouseCursor()` caches its last applied cursor in
// `bd->MouseLastCursor` and skips `SDL_SetCursor()` when it has not changed. So
// a bare `SDL_SetCursor()` override from the canvas would **stick when the
// pointer left the canvas**: the backend would go on believing it had already
// applied the arrow and never correct us, and the crosshair would follow the
// user out over the panels. There is no way to invalidate that cache from
// outside -- it is a static struct in the backend's translation unit.
//
// So this build sets `ImGuiConfigFlags_NoMouseCursorChange`, which is the very
// first thing that function tests, and it early-returns. **The backend stops
// touching the cursor at all**: no cache to go stale, nothing to fight, and no
// dependence on where in the frame we run. In exchange this module becomes
// responsible for *every* cursor in the application, not only the canvas's --
// see §6, which is the part that has to be right or the panels break.
//
// Neither that flag nor `io.MouseDrawCursor` was set anywhere in this build
// before this change; the flag is now set in `main.cpp` beside `io.IniFilename`
// and is the only one.
//
// ==========================================================================
// 2. Two layers, so a change of shape is a change to one function
// ==========================================================================
//
// `cursorForTool()` answers in **`ToolCursor`, an intent** -- Paint, Select,
// Sample, Pan, Zoom, MoveObject, Text, Refuse -- and not in a backend constant.
// `sdlCursorFor()` is the separate projection of that intent onto a shape.
//
// The split is what kept this change small when the mechanism moved from ImGui
// to SDL: the tool table, the refusal rule and every assertion about them are
// written against the intent, so only the projection was rewritten. It is the
// same seam a later bitmap layer would use to replace individual entries --
// `sdlCursorFor()` is the one function that would have to change, and the
// entries most worth replacing are named in §3.
//
// ==========================================================================
// 3. The mapping, and the one collision left
// ==========================================================================
//
//   Paint       -> SDL_SYSTEM_CURSOR_CROSSHAIR     the tip's exact centre
//   Select      -> SDL_SYSTEM_CURSOR_NWSE_RESIZE   a dragged-out extent
//   Sample      -> SDL_SYSTEM_CURSOR_POINTER       point at the pixel to read
//   Pan         -> SDL_SYSTEM_CURSOR_MOVE          the view follows the drag
//   Zoom        -> SDL_SYSTEM_CURSOR_NESW_RESIZE   magnification is scaling
//   MoveObject  -> SDL_SYSTEM_CURSOR_MOVE          content follows the drag
//   Text        -> SDL_SYSTEM_CURSOR_TEXT
//   Refuse      -> SDL_SYSTEM_CURSOR_NOT_ALLOWED
//   Arrow       -> SDL_SYSTEM_CURSOR_DEFAULT
//
// **Paint, Select and Sample are now three different shapes**, which is the
// whole point of the move to SDL and is asserted as a distinctness rather than
// as an equality. The brush finally looks like a brush tool.
//
// **Pan and MoveObject remain one shape, deliberately.** SDL has exactly one
// "drag and something follows" cursor and both of these mean precisely that --
// the view for one, the content for the other. This is a fair collision rather
// than a forced one: it would survive any richer set, because the two really do
// mean the same thing about the pointer. `--selftest` asserts it as an equality
// with that reasoning attached, so a later split is a deliberate act.
//
// **The two weakest entries, named so they are the first replaced.** `Select`
// and `Zoom` are the mirrored diagonal resize arrows. They are distinguishable
// from everything else and from each other, and each has a reading -- a marquee
// drags out an extent, a zoom scales -- but neither is *evocative* the way the
// crosshair and the I-beam are, and a user could momentarily read either as
// "something is about to be resized". They were chosen for distinguishability
// over suggestion because a wrong-but-distinct shape is corrected by one
// gesture, while a shared shape conveys nothing at all, ever. The conventional
// alternative is a crosshair for `Select` too -- which is what Photoshop does,
// and which this build could almost carry because the canvas's own brush radius
// ring already tells a brush from a marquee. That trade wants judging with the
// application actually running, and it is one line in `sdlCursorFor()`.
//
// ==========================================================================
// 4. The cursor must describe the OUTCOME, not the selection
// ==========================================================================
//
// This is the part worth having, and it is the same principle the refusal
// sentences in `ui/MacPaintUI.cpp` and `app/StrokeSession.hpp` §§1 and 6 were
// written for: **tell the user before they waste the gesture.** A brush over a
// locked layer, or over an Adjustment layer, is going to be refused; a bucket
// over a Pigment layer is going to be refused. Those refusals already produce a
// sentence in the options bar -- but the options bar is a different band, and
// the user is looking at the canvas with a pen in their hand.
//
// `toolCursorOnTarget()` therefore runs the refusal predicates that already
// exist -- `strokeRouteFor()` and `pixelOpRefusalFor()`, not a second opinion
// about them -- and answers `Refuse` when the gesture will not land. It costs
// one predicate call per frame and turns a silent wasted stroke into a slashed
// circle that appears the instant the pointer crosses onto the sheet.
//
// **Which tool strokes at all is derived from the route table, not restated.**
// `strokeRouteFor(tool, nullptr)` is `PaintSim` for exactly Brush, DryBrush and
// Water and `None` for everything else, so "does this tool begin a stroke" is a
// question that table already answers. Spelling out `tool == Brush || tool ==
// Water || tool == DryBrush` a fourth time is how the options bar's route
// indicator came to disagree with the canvas, which is the defect
// `strokeRouteWritesLayer()`'s own comment describes.
//
// **A null target is not a refusal for a stroke tool**, and that is a decision
// rather than an oversight. `strokeRouteFor()`'s last row sends a stroke with
// no document at all to `sim::PaintSim`'s dense canvas texture, which is a real
// destination -- every medium demo paints it. Showing a slashed circle over a
// canvas that is about to accept watercolour would be a lie. For the bucket and
// the gradient the opposite is true: `pixelOpRefusalFor(nullptr)` is `NoLayer`,
// they have nowhere to write without a document, and `Refuse` is the truth.
//
// ==========================================================================
// 5. What an unimplemented tool shows, and why it is the harsh answer
// ==========================================================================
//
// **`Refuse`.** Fifteen of the twenty-eight `Tool` values are name/icon/slot
// only (`app/AppState.hpp`'s own comment, `toolImplemented()` is the list), and
// picking one of them and dragging across the canvas does nothing whatsoever.
//
// The gentler option -- show each unbuilt tool its natural intent cursor -- was
// rejected because it produces exactly the failure this whole file exists to
// end: the user scrubs, nothing happens, and the application never says why.
// That is the "invisible wrong-target" defect `app/StrokeSession.hpp` §1 and
// `app/selftest/BucketRefusal.cpp` are both written about, and answering it
// with a shrug for fifteen tools while answering it properly for the brush
// would be inconsistent on top of unhelpful.
//
// The cost is admitted: a slashed circle over the canvas for most of the
// palette reads as a blunt application. It is *true*, which the arrow was not,
// and the palette already dims these cells and tooltips them as unbuilt -- so
// the cursor agrees with the palette rather than contradicting it. When a tool
// lands, `toolImplemented()` flips and this rule stops applying to it with no
// edit here: `cursorForTool()` already holds the intent that tool will want.
//
// ==========================================================================
// 6. One writer, every frame -- the part that breaks the panels if it is wrong
// ==========================================================================
//
// Setting `ImGuiConfigFlags_NoMouseCursorChange` means the backend no longer
// applies **any** cursor, including the ones that have nothing to do with
// tools: the I-beam in the LAYERS filter box, the resize arrows on a window
// border, the pointer over a menu. Those must go on working exactly as before,
// so `SystemCursorTable` covers *both* populations -- every `ImGuiMouseCursor_`
// value ImGui might ask for, mapped to its SDL equivalent by
// `sdlCursorForImGui()`, and every `ToolCursor` intent by `sdlCursorFor()`.
//
// `SystemCursorTable::apply()` runs **once per frame from one place**
// (`main.cpp`, just before `ImGui::Render()`), and its rule is one line: if the
// canvas asked for a tool cursor this frame, use it; otherwise use whatever
// ImGui asked for. One writer, one decision, no ordering to reason about.
//
// **It reproduces every branch of the function it replaced**, not only the
// common one, because each of the others is a state the pointer disappears or
// misbehaves in:
//
//   * `io.MouseDrawCursor`, or ImGui asking for `ImGuiMouseCursor_None`
//     -- `SDL_HideCursor()`. Skipping this leaves a doubled pointer when ImGui
//     draws its own, and shows a cursor in the state that asked for none.
//     `None` is -1, so it must also be caught *before* it is used as an index.
//   * A null table entry falls back to the default arrow.
//     `SDL_CreateSystemCursor()` can fail, and indexing a hole would set a null
//     cursor rather than leaving the last one alone.
//   * `SDL_ShowCursor()` on **every** frame of the visible branch, not only
//     when the shape changed -- that is what brings the pointer back after a
//     frame that hid it.
//
// The skip-if-unchanged check on `SDL_SetCursor()` is carried over as the
// optimisation it is (SDL has no early-out of its own; see ImGui issue #6113),
// not as the mechanism: correctness here does not depend on it.
//
// ==========================================================================
// 7. T17 reopens §1's decision -- bitmaps, gated by a flag nobody has flipped
// ==========================================================================
//
// §1 above rejected custom bitmaps outright, "with this build as the only
// writer" of SDL *system* cursors. docs/testing-issues.md's T17 asks for a
// lasso, a polygon-lasso outline, a magic-wand sparkle and a marquee with an
// offset crosshair -- and there is no SDL system cursor that is any of those
// four shapes, so this request cannot be answered inside §1's decision. It
// has to be reopened, not worked around, which is why §1's four objections
// are re-quoted and re-judged here rather than silently overridden.
//
// **Two of the four objections are answerable, and this section is the
// answer:**
//
//   * *"A missing font gives a blank cursor."* `rasterizeToolCursorBitmap()`
//     rasterises through stb_truetype directly (not through Dear ImGui's font
//     atlas, which needs a live `GImGui` this code must run without -- see
//     that function's own comment) and reports `CursorBitmap::nonBlank`,
//     which is false for a missing file, an absent codepoint, or a
//     zero-coverage glyph. `--selftest` asserts every shipped bitmap is
//     non-blank -- turning `ui/Fonts.cpp:333`'s silent degradation loud for
//     the FIRST time anywhere in this codebase -- and `SystemCursorTable`
//     never installs a blank bitmap as a cursor: a tool whose rasterisation
//     failed falls back to `sdlCursorFor()`'s system cursor exactly as it did
//     before this section existed.
//   * *"A hotspot nothing in `--selftest` could check."* A hotspot is two
//     integers. `--selftest` asserts each one lands inside its bitmap's own
//     drawn (non-transparent) bounding box, and -- the case the report is
//     actually describing -- that the marquee pair's hotspot is the
//     crosshair's own centre pixel, not the shape's corner or the canvas's
//     centre. See `app/selftest/ToolCursor.cpp` section G.
//
//   * *"Bitmaps do not track the OS pointer-size accessibility setting."*
//     **This objection turned out to be false on macOS, and finding that out
//     cost a shipped bug worth recording.** Published claims -- and a first
//     implementation built on them -- said macOS scales only the cursors it
//     draws itself. It does not: it scales an application's own `NSCursor`
//     too. That first implementation therefore read the Pointer size
//     preference and rasterised bigger, and on a real machine set to 2.07x
//     the OS's scaling multiplied by ours and produced a cursor roughly three
//     times the size it should have been.
//
//     So this file draws at ONE size, `kCursorBasePoints` (24, which is
//     `[NSCursor crosshairCursor].image.size` measured rather than picked),
//     and lets the OS enlarge it. Nothing here reads an accessibility
//     preference. The lesson is narrower than "check your sources": a claim
//     about how a platform composites something is checkable on the machine
//     in front of you, and a size bug is exactly the kind that a test suite
//     full of internal assertions cannot see -- every scale assertion in
//     `app/selftest/ToolCursor.cpp` section H passed while the cursor on
//     screen was three times too big.
//
//     Crispness on a Retina display is the one scale axis that remains, and
//     it belongs to SDL: `SDL_AddSurfaceAlternateImage()` attaches a 2x
//     alternate to the cursor surface, and SDL's Cocoa backend builds one
//     multi-representation `NSImage` whose POINT size is the base surface's
//     size. So the bitmap is 48x48 pixels and the cursor is 24 points.
//
// **The one that still does not get answered:**
//
//   * Bitmaps ignore the user's cursor theme. Milder, for the reason §1
//     already gives: a drawing tool overriding the pointer over its own
//     canvas is conventional.
//
// **So the mechanism is built, the trade was decided, and the flag stays.**
// `SystemCursorTable::setBitmapCursorsEnabled(bool)` is the ONE flag that
// switches between the bitmap and system paths. It now defaults to `true`:
// the product decision that was deferred has been made, and the reason it
// could be made is the paragraph above -- the accessibility cost that made it
// a decision at all no longer exists. The flag itself is kept rather than
// deleted, because "fall back to system cursors" remains a real answer for a
// platform where the rasterisation is wrong, and because §7's flag-off
// identity proof is still the cheapest evidence that this whole section is
// additive. With the flag off, `shouldUseBitmapCursor()` returns `false`
// unconditionally (proved by exhaustive loop, not asserted for one case --
// `app/selftest/ToolCursor.cpp` section G again), which is the one branch
// `apply()` gained; every other line in `apply()` is the code §6 already
// argued, untouched.
namespace np {

// What the pointer should *mean* over the canvas, independent of which shape
// the current backend happens to draw for it. See §2.
enum class ToolCursor {
  // No better answer than the platform default.
  Arrow,
  // A tip that deposits: the brush family, and the two fill ops.
  Paint,
  // A boundary being drawn: marquees, lassos, the wand, crop, slice, and the
  // path tools that define geometry by clicking points.
  Select,
  // Reading the canvas rather than writing it: the eyedropper and the measure
  // tool.
  Sample,
  // Dragging the view itself.
  Pan,
  // Changing magnification.
  Zoom,
  // Repositioning content rather than the view.
  MoveObject,
  // A text insertion point.
  Text,
  // **The gesture will not land.** Either the tool is not built, or the
  // refusal predicates say this target cannot take it. See §§4-5.
  Refuse,

  // T17 (docs/testing-issues.md) reported that the five selection tools shared
  // one cursor. An earlier revision answered that here, with five new
  // enumerators; §7 now keys its bitmaps by `Tool` instead, which distinguishes
  // all twenty-eight tools rather than five and needs no enumerator at all. So
  // this enum stayed what §2 argues it should be -- what a tool MEANS, coarser
  // than `Tool` on purpose -- rather than becoming a key another table needed.
};

// The table. One arm per `Tool` value and **no `default:`**, so `-Wswitch`
// stops the build when a tool is added rather than letting it inherit an
// answer nobody chose -- the same guard `strokeRouteFor()` and
// `strokeEditLabel()` spell out their own twenty-tool lists for.
//
// Pure: this is what the tool *means*, with no view of the document. It
// deliberately does **not** know whether the tool is implemented or whether the
// target will refuse it -- both of those belong to `toolCursorOnTarget()`, so
// that the day an unbuilt tool ships, its intent is already written here.
ToolCursor cursorForTool(Tool tool) noexcept;

// The same question against what the pointer is actually over: the tool's
// intent, downgraded to `Refuse` when the gesture cannot land. §§4-5.
//
// `target` is the active layer, or nullptr when there is no document -- a legal
// argument with its own answer, and not the same answer for every tool (§4).
ToolCursor toolCursorOnTarget(Tool tool, const Layer* target) noexcept;

// The intent as a shape. §3 is the table and the argument for each entry; this
// is the one function a richer cursor set would replace.
SDL_SystemCursor sdlCursorFor(ToolCursor cursor) noexcept;

// The other half of §6's job: what ImGui asked for, as the same kind of shape,
// so that suppressing the backend costs the panels and menus nothing.
//
// Total over the `ImGuiMouseCursor_` range. `ImGuiMouseCursor_None` is not a
// shape and must be handled by the caller before it gets here (`apply()`
// does); passed anyway it answers the default arrow rather than reading out of
// range.
SDL_SystemCursor sdlCursorForImGui(ImGuiMouseCursor cursor) noexcept;

// The intent's own name, for `--selftest`'s printed table. Never null.
const char* toolCursorName(ToolCursor cursor) noexcept;

// -------------------------------------------------------- §7: bitmap cursors
//
// The marquee composite's shape parameter (§7): both marquees ask for the
// same crosshair, offset to the bottom-left of a shape, and the shape is the
// only thing that differs between them. One generator taking this as a
// parameter, not two hand-drawn cursors -- see `drawMarqueeCrosshair()` in
// the .cpp, which is what actually reads it.
enum class CursorMarqueeShape { Rectangle, Ellipse };

// One rasterised cursor: straight (non-premultiplied) RGBA8 pixels, row-major
// from the top-left -- `SDL_PIXELFORMAT_RGBA32`'s own layout, so
// `SystemCursorTable` can hand `rgba.data()` to `SDL_CreateSurfaceFrom()`
// without a repack -- plus the hotspot the OS should treat as the exact pixel
// the tool acts on.
struct CursorBitmap {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4 bytes, possibly empty
  int hotspotX = 0;
  int hotspotY = 0;
  // False when every pixel's alpha is 0: a font whose glyph did not
  // rasterise (missing file, absent codepoint, zero-size glyph) or a
  // generator that produced nothing. §7's answer to "a missing font gives a
  // blank cursor" -- this is the flag that turns that silent failure loud.
  bool nonBlank = false;
};

// **The size a cursor ships at, in points, and the scale that produces it.**
// Exposed rather than left file-local for one reason, and it is a lesson
// rather than a convenience: a revision that shipped cursors roughly three
// times too big passed `--selftest` completely, because every scale assertion
// in `app/selftest/ToolCursor.cpp` section H exercised the rasteriser with a
// scale the test itself chose. Nothing read the number `create()` actually
// uses, so changing that number broke nothing. These two are what make the
// shipping size assertable.
//
// 24 is `[NSCursor crosshairCursor].image.size` on macOS, measured; the scale
// is that divided by the 32-unit design space. `create()` builds the base at
// `cursorBaseScale()` and the Retina alternate at twice it.
int cursorBasePoints() noexcept;
float cursorBaseScale() noexcept;

// Where a tool's cursor actually points, as a fraction of its glyph's own
// inked bounding box: (0,0) is that box's top-left, (1,1) its bottom-right.
//
// **A fraction rather than a pixel, so it survives every scale**, and per-tool
// rather than one rule, because the working point of an icon is not a property
// of its pixels -- a lasso draws from the end of its tail and a magnifier
// points at the middle of its lens, and no amount of looking at coverage
// recovers either. An earlier revision used the glyph's centre for everything,
// which put the lasso's hotspot in the middle of its loop: the same class of
// defect T17 started from, a cursor that does not say where the click lands.
struct CursorHotspotAnchor {
  float fx = 0.5f;
  float fy = 0.5f;
};

// The anchor for one tool. Total over `Tool` with no `default:` arm, so adding
// a tool stops the build here rather than silently inheriting the centre.
CursorHotspotAnchor cursorHotspotAnchorFor(Tool tool) noexcept;

// Which tools have a bitmap cursor: the two marquees (§7's procedural
// composite) and every tool the palette has an icon for -- which today is all
// of them. `Tool::Count` is the enum's bound, not a tool, and answers false.
bool toolHasBitmapCursor(Tool tool) noexcept;

// Renders the bitmap for one `ToolCursor`. Pure and headless: it reads the
// vendored Lucide TTF (`NP_LUCIDE_TTF`) off disk through stb_truetype
// directly for the three glyph-based shapes (Lasso, PolygonLasso, MagicWand)
// rather than through Dear ImGui's font atlas -- `ui/Fonts.cpp`'s
// `installToolIconFont()` already proves the ImGui path works for the tool
// *palette*, but its `ImFontAtlas`/`ImFontBaked` machinery needs a live
// `GImGui`, and this function has to run inside `--selftest`, which never
// creates one (see `app/selftest/ToolCursor.cpp`'s own file comment on why
// this whole test file is headless). The marquee pair does not touch the
// font at all -- `drawMarqueeCrosshair()` in the .cpp draws them
// procedurally, since neither shape exists as a glyph anywhere.
//
// Returns `nonBlank == false` (with a best-effort but possibly empty `rgba`)
// for any `Tool` `toolHasBitmapCursor()` answers false for, and for one it
// answers true for whose source failed to rasterise. The caller
// (`SystemCursorTable::create()`) is what turns a false `nonBlank` into "skip
// this bitmap, fall back to `sdlCursorFor()`" -- this function only reports
// the fact.
//
// `scale` multiplies a 32-unit design space into pixels. `create()` passes
// `kCursorBasePoints / 32` for the base surface and twice that for the Retina
// alternate -- the display backing scale is the only reason a cursor bitmap is
// bigger than the cursor, because macOS applies the user's Accessibility
// pointer size on top of whatever this file produces (see `kCursorBasePoints`
// in the .cpp for the measurement, and for the revision that scaled here too
// and shipped a cursor three times too big). Out-of-range values are clamped
// rather than trusted, so a caller cannot turn a bad argument into a
// zero-sized canvas that then reports itself blank for a reason that has
// nothing to do with the font.
CursorBitmap rasterizeToolCursorBitmap(Tool tool, float scale = 1.0f) noexcept;

// The one decision `apply()` gains once bitmap cursors exist: does the
// bitmap win this frame, or does the system-cursor fallback? Pulled out as a
// pure function -- rather than left inline in `apply()` -- because `apply()`
// needs live SDL video and cannot run under `--selftest` (§6's own
// admission), while this decision needs nothing: it is a comparison of an
// `optional` and two `bool`s. `app/selftest/ToolCursor.cpp` section G calls
// this function directly, the same one `apply()` calls, and proves it
// answers `false` for every `Tool` value when `bitmapsEnabled` is
// `false` -- the mechanical proof §7 promises for "flag off is byte-identical
// to today", rather than an assertion about one case.
//
// `hasBitmap` is true only when BOTH `toolHasBitmapCursor(*toolRequest)` and
// `rasterizeToolCursorBitmap(*toolRequest).nonBlank` hold -- both facts the
// caller already has to know to build the bitmap cursor table, so they are
// passed in rather than recomputed here.
bool shouldUseBitmapCursor(bool bitmapsEnabled, std::optional<Tool> toolRequest,
                            bool hasBitmap) noexcept;

// The application's cursors, created once and owned here. §6.
//
// Deliberately not a singleton and not self-initialising: `create()` needs SDL
// video up, `destroy()` must run before `SDL_Quit()`, and both facts are
// visible at the call site in `main.cpp` rather than buried in a first-use
// branch that would make the lifetime impossible to check by reading.
class SystemCursorTable {
 public:
  // After SDL video init. Safe to call twice; the second is a no-op.
  //
  // Builds every SDL system cursor rather than only the nine §3 names, so that
  // adding a mapping is a change to `sdlCursorFor()` alone and can never index
  // a hole. The cost is a couple of dozen OS cursor handles for the process
  // lifetime, which is not the kind of allocation ADR-0001's idle-memory rule
  // is about -- there is no per-document or per-tile growth here.
  //
  // Also builds the five §7 bitmap cursors, unconditionally -- not gated on
  // `bitmapsEnabled_`, because the flag can be flipped after `create()` runs
  // (it is a plain setter, not a construction argument) and there is nowhere
  // else to build them from. A cursor whose `rasterizeToolCursorBitmap()`
  // came back blank is left null here rather than installed: §7's fallback
  // rule, enforced at the one place that knows both the flag and the pixels.
  void create() noexcept;

  // Before `SDL_Quit()`. Safe to call twice, and safe without `create()`.
  void destroy() noexcept;

  // §7's ONE flag. Defaults to **`true`**: the per-tool bitmaps are what the
  // application shows. Kept as a flag rather than deleted so a platform whose
  // rasterisation is wrong has a working fallback that is one call away, and
  // so §7's flag-off identity proof stays runnable. See ui/ToolCursor.hpp §7
  // for why the accessibility objection that once kept this `false` no longer
  // applies.
  void setBitmapCursorsEnabled(bool enabled) noexcept { bitmapsEnabled_ = enabled; }
  bool bitmapCursorsEnabled() const noexcept { return bitmapsEnabled_; }


  // One frame's decision, from one place. `request` is what the canvas asked
  // for as a system-cursor shape, or `nullopt` when the pointer is not over
  // it -- in which case ImGui's own request is honoured, which is what keeps
  // every panel, menu and window border behaving as it did before the
  // backend was suppressed. `toolRequest` is the SAME frame's request as a
  // `ToolCursor` intent rather than a projected shape, or `nullopt` under the
  // identical circumstances `request` is -- it exists only so this function
  // can ask `shouldUseBitmapCursor()` whether a bitmap should win instead.
  //
  // With `bitmapsEnabled_ == false` this parameter is read by
  // `shouldUseBitmapCursor()`, which is proved (§7, and
  // `app/selftest/ToolCursor.cpp` section G) to answer `false` for every
  // value regardless -- so passing it costs nothing behaviourally and every
  // other line here is §6's original function, unedited.
  //
  // A no-op when `create()` has not run, so a code path that never made a
  // window (`--selftest`) cannot trip over it.
  void apply(std::optional<SDL_SystemCursor> request,
             std::optional<Tool> toolRequest = std::nullopt) noexcept;

 private:
  SDL_Cursor* cursors_[SDL_SYSTEM_CURSOR_COUNT] = {};
  // §7's bitmap cursors, indexed by `static_cast<int>(Tool)`. Sized to the
  // whole enum so the index IS the tool value -- no second mapping function to
  // keep in sync with `toolHasBitmapCursor()` -- and a slot whose tool has no
  // bitmap, or whose rasterisation came back blank, simply stays null forever,
  // which `bitmapCursorFor()` treats identically.
  static constexpr int kToolCount = static_cast<int>(Tool::Count);
  SDL_Cursor* bitmapCursors_[kToolCount] = {};
  // Purely to skip a redundant `SDL_SetCursor()`; see §6's last paragraph.
  SDL_Cursor* last_ = nullptr;
  bool created_ = false;
  bool bitmapsEnabled_ = true;  // §7's flag; the bitmaps are what ships.

  // Builds the §7 bitmap cursors, destroying whatever was there first. A
  // function rather than a loop inside `create()` so the destroy-then-build
  // order is stated once, where a future rebuild path would also use it.
  void buildBitmapCursors() noexcept;

  // Null when `tool` has no bitmap, when `create()` has not run, or when that
  // tool's rasterisation was blank. Shared by `create()` (to decide what to
  // log) and `apply()` (to decide what to draw).
  SDL_Cursor* bitmapCursorFor(Tool tool) const noexcept;
};

}  // namespace np
