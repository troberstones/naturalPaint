#pragma once

#include <optional>

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
  void create() noexcept;

  // Before `SDL_Quit()`. Safe to call twice, and safe without `create()`.
  void destroy() noexcept;

  // One frame's decision, from one place. `request` is what the canvas asked
  // for, or `nullopt` when the pointer is not over it -- in which case ImGui's
  // own request is honoured, which is what keeps every panel, menu and window
  // border behaving as it did before the backend was suppressed.
  //
  // A no-op when `create()` has not run, so a code path that never made a
  // window (`--selftest`) cannot trip over it.
  void apply(std::optional<SDL_SystemCursor> request) noexcept;

 private:
  SDL_Cursor* cursors_[SDL_SYSTEM_CURSOR_COUNT] = {};
  // Purely to skip a redundant `SDL_SetCursor()`; see §6's last paragraph.
  SDL_Cursor* last_ = nullptr;
  bool created_ = false;
};

}  // namespace np
