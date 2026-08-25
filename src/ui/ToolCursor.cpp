#include "ui/ToolCursor.hpp"

#include "app/StrokeSession.hpp"
#include "ui/AtelierChrome.hpp"

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
    case Tool::Marquee:
    case Tool::EllipseMarquee:
    case Tool::Lasso:
    case Tool::PolygonLasso:
    case Tool::MagicWand:
    case Tool::Crop:
    case Tool::Slice:
    case Tool::Pen:
    case Tool::Curve:
    case Tool::Shape:
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

// --- the table itself (header §6) -----------------------------------------

void SystemCursorTable::create() noexcept {
  if (created_) return;
  for (int i = 0; i < SDL_SYSTEM_CURSOR_COUNT; ++i)
    cursors_[i] = SDL_CreateSystemCursor(static_cast<SDL_SystemCursor>(i));
  // Marked created even if some entries came back null: `apply()`'s fallback
  // covers a hole, and refusing to mark the table created because one exotic
  // resize cursor is unavailable on some platform would disable the pointer
  // entirely rather than degrade one shape.
  created_ = true;
}

void SystemCursorTable::destroy() noexcept {
  for (SDL_Cursor*& c : cursors_) {
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

void SystemCursorTable::apply(std::optional<SDL_SystemCursor> request) noexcept {
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

  SDL_Cursor* chosen = cursors_[want];
  // `SDL_CreateSystemCursor()` can fail for a shape a platform does not
  // provide. Falling back to the arrow keeps a pointer on screen; passing the
  // null through would set no cursor at all.
  if (chosen == nullptr) chosen = cursors_[SDL_SYSTEM_CURSOR_DEFAULT];
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
