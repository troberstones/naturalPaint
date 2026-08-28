#include "app/selftest/Support.hpp"

#include "app/StrokeSession.hpp"
#include "core/LayerOps.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/ToolCursor.hpp"

namespace np {

// ---------------------------------------------------------------------------
// The pointer, and whether it describes what the next click will actually do
// (ui/ToolCursor).
//
// **This section exists because of an absence.** Before it there were zero
// cursor calls in the whole of `src/` -- no `ImGui::SetMouseCursor`, no
// `SDL_SetCursor`, no `SDL_CreateCursor` -- so the OS arrow sat over the canvas
// identically whether the next press would deposit pigment, drag a marquee,
// sample a colour, pan the view, or be refused outright and do nothing.
//
// Two things are asserted here and they are not equally interesting.
//
// The cheap half is that the table is total and non-degenerate: every one of
// the twenty-eight `Tool` values has an intent, and the five tool families a
// user actually distinguishes -- paint, select, sample, pan, zoom -- are five
// different answers rather than one answer repeated. `-Wswitch` already stops
// the build when a tool is added (there is no `default:` arm, deliberately, the
// same guard `strokeRouteFor()` spells its own twenty-tool list out for), so
// what section A adds on top of the compiler is the *quality* check the
// compiler cannot make: that nobody satisfied `-Wswitch` by pointing every new
// arm at the fallback.
//
// **The half worth having is section D**, and it is the same principle the
// refusal sentences follow: tell the user before they waste the gesture. A
// brush over a locked layer, or over an Adjustment layer, and a bucket over a
// Pigment layer are all going to be refused -- `app/StrokeSession` §§1 and 6
// already know it, and already produce a sentence for the options bar. But the
// options bar is a different band and the user is looking at the canvas with a
// pen in their hand. The cursor is the one piece of chrome that is guaranteed
// to be where they are looking, and making it say "no" costs one predicate
// call.
//
// **Section E is what stops D passing vacuously.** A `toolCursorOnTarget()`
// that returned `Refuse` unconditionally would satisfy every assertion in D,
// so E asserts the successes: the brush on both paintable kinds, the bucket on
// an RGB layer, and -- the case that catches an over-eager refusal -- the
// *non-writing* tools over a layer that refuses writes. A lock stops the brush;
// it must not stop the eyedropper, and it must not stop the Hand.
//
// **Section C is where the choice of mechanism shows.** Dear ImGui's cursor set
// has no crosshair -- Arrow, TextInput, four Resize shapes, Hand, Wait,
// Progress, NotAllowed is the whole enum, and its SDL3 backend builds exactly
// one system cursor per value, so SDL's own `SDL_SYSTEM_CURSOR_CROSSHAIR` is
// unreachable through it. Routed that way, Paint, Select and Sample all became
// a plain arrow, which left the brush, the four selection tools, the eyedropper
// and the bucket sharing one pointer. So this build talks to SDL directly and
// suppresses the backend's cursor handling
// (`ImGuiConfigFlags_NoMouseCursorChange`), and section C asserts the three are
// **distinct** -- a table collapsing any two of them fails.
//
// The one collision left, Pan and MoveObject on SDL's single four-pointed
// arrow, is asserted as an equality with its reasoning attached: it is fair
// rather than forced, since panning the view and dragging content really do
// mean the same thing about the pointer.
//
// **Section C also covers the cost of that suppression.** Nothing else will now
// apply ImGui's own cursors, so `sdlCursorForImGui()` has to reproduce the
// backend's table -- all eleven values, plus the `None` sentinel that is a hide
// request rather than a shape. A wrong entry there never shows on the canvas;
// it shows as a panel behaving oddly, which is the kind of defect a test has to
// catch because nobody goes looking for it.
//
// Headless and GPU-free: needs no window, no ImGui context, no SDL video and no
// document -- every function under test is a pure projection or takes a
// `const Layer*`, and every fixture below is a bare `core::Layer`. The one
// piece that genuinely cannot be covered here is `SystemCursorTable` itself:
// creating a cursor needs SDL's video subsystem, and what it does is call into
// the platform. Its branches are argued in ui/ToolCursor.hpp §6 against the
// backend function it replaces, line by line, and are stated as untested rather
// than given a test that asserts something adjacent to them.
// ---------------------------------------------------------------------------
bool runToolCursorTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // The fixtures, each one a layer kind the refusal predicates answer
  // differently about. Built bare rather than through a Document because
  // nothing here needs history, tiles with content, or a revision -- the
  // predicates read `kind`, `locked` and which store is engaged, and that is
  // all `core::makeRgbLayer()` and friends set.
  const Layer rgb = makeRgbLayer("Sky");            // writable by everything
  const Layer pigment = makePigmentLayer("Wash");   // brush yes, bucket no
  const Layer adjustment = makeAdjustmentLayer("Curves");  // nothing writes it
  Layer lockedRgb = makeRgbLayer("Sky");
  lockedRgb.locked = true;
  Layer lockedPigment = makePigmentLayer("Wash");
  lockedPigment.locked = true;

  std::printf("  -- A. every Tool value has an intent, and none is the fallback --\n");

  {
    // The whole enum, printed. A reader scanning a run can see the table it is
    // being asked to trust, which is the same reason BucketRefusal prints its
    // refusal sentences rather than only asserting on them.
    bool allNamed = true;
    bool anyFellBack = false;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      const ToolCursor c = cursorForTool(t);
      const char* name = toolCursorName(c);
      std::printf("    %-20s -> %-8s %s\n", toolName(t), name,
                  toolImplemented(t) ? "" : "(not built)");
      // `toolCursorName()` answers "?" only for a value outside the enum, so
      // this catches a `cursorForTool()` that returned a cast integer.
      if (std::string(name) == "?") allNamed = false;
      // **The assertion `-Wswitch` cannot make.** A switch is forced to be
      // exhaustive, but nothing forces the arms to be *considered*: a new tool
      // added as `case Tool::Whatever: return ToolCursor::Arrow;` compiles
      // silently and conveys exactly what the OS arrow conveyed before this
      // file existed. `Arrow` is reserved for `Tool::Count`, which is not a
      // tool, so no real tool may answer it.
      if (c == ToolCursor::Arrow) anyFellBack = true;
    }

    check(allNamed,
          "table: every Tool value answers a real ToolCursor -- a cast integer would "
          "print '?' here rather than being caught by the compiler");
    check(!anyFellBack,
          "table: NO tool answers Arrow -- Arrow is the non-tool bound's answer, so a "
          "tool wearing it is an arm somebody added to satisfy -Wswitch without thinking");

    // The bound itself, which is a `Tool` value the switch must still cover and
    // is the one legitimate `Arrow`.
    check(cursorForTool(Tool::Count) == ToolCursor::Arrow,
          "table: Tool::Count answers Arrow -- it is the enum's bound, not a tool, and "
          "it is listed rather than swept up by a default: arm");
  }

  std::printf("  -- B. the five tool families are five different answers --\n");

  {
    // The tools the task of "which one is selected" actually turns on, and the
    // ones a user reaches for most. If any two of these share an intent the
    // table has stopped carrying information, whatever the projection does with
    // it afterwards.
    const ToolCursor paint = cursorForTool(Tool::Brush);
    const ToolCursor select = cursorForTool(Tool::Crop);  // T17 did NOT split this one
    const ToolCursor sample = cursorForTool(Tool::Eyedropper);
    const ToolCursor pan = cursorForTool(Tool::Hand);
    const ToolCursor zoom = cursorForTool(Tool::Zoom);

    check(paint != select && paint != sample && paint != pan && paint != zoom &&
              select != sample && select != pan && select != zoom && sample != pan &&
              sample != zoom && pan != zoom,
          "intent: paint, select, sample, pan and zoom are five distinct intents -- a "
          "degenerate table returning one value for everything dies on this line");

    // Family members agree with each other, which is the other half of
    // non-degeneracy: a table where every tool got its OWN intent would pass
    // the line above and still be wrong.
    check(cursorForTool(Tool::DryBrush) == paint && cursorForTool(Tool::Water) == paint &&
              cursorForTool(Tool::PaintBucket) == paint,
          "intent: the whole paint family shares one intent -- the dry brush and the "
          "bucket put colour down the same way the brush does");
    // T17 did NOT report Crop, Slice, Pen, Curve or Shape as confusable with
    // one another, so this half of the old "whole selection family shares
    // one intent" claim still holds for them.
    check(cursorForTool(Tool::Slice) == select && cursorForTool(Tool::Pen) == select &&
              cursorForTool(Tool::Curve) == select && cursorForTool(Tool::Shape) == select,
          "intent: crop, slice, pen, curve and shape still share plain Select -- T17 "
          "verified the fall-through defect only for the five tools split out below");

    // **This is the assertion T17 exists to flip.** Before this work, this
    // line asserted the OPPOSITE -- that Marquee, EllipseMarquee, Lasso,
    // PolygonLasso and MagicWand all answered the SAME intent as `select`
    // above, which is exactly the "one cursor for five different gestures"
    // defect the report is about. Now each is its own value, and a table
    // that collapsed any two of them back together would be reintroducing
    // that defect silently.
    const ToolCursor marquee = cursorForTool(Tool::Marquee);
    const ToolCursor ellipseMarquee = cursorForTool(Tool::EllipseMarquee);
    const ToolCursor lasso = cursorForTool(Tool::Lasso);
    const ToolCursor polygonLasso = cursorForTool(Tool::PolygonLasso);
    const ToolCursor magicWand = cursorForTool(Tool::MagicWand);
    check(marquee != select && ellipseMarquee != select && lasso != select &&
              polygonLasso != select && magicWand != select && marquee != ellipseMarquee &&
              marquee != lasso && marquee != polygonLasso && marquee != magicWand &&
              ellipseMarquee != lasso && ellipseMarquee != polygonLasso &&
              ellipseMarquee != magicWand && lasso != polygonLasso && lasso != magicWand &&
              polygonLasso != magicWand,
          "intent: T17's five -- Marquee, EllipseMarquee, Lasso, PolygonLasso, MagicWand "
          "-- are now SIX-WAY distinct from each other AND from plain Select, which is the "
          "whole of what this task was asked to build");
    check(marquee == ToolCursor::SelectMarquee && ellipseMarquee == ToolCursor::SelectEllipseMarquee &&
              lasso == ToolCursor::SelectLasso && polygonLasso == ToolCursor::SelectPolygonLasso &&
              magicWand == ToolCursor::SelectMagicWand,
          "intent: and each is named the way ui/ToolCursor.hpp's enum comment promises -- a "
          "silent renumbering would pass the distinctness check above and still be wrong");
    check(cursorForTool(Tool::Text) == ToolCursor::Text &&
              cursorForTool(Tool::Move) == ToolCursor::MoveObject,
          "intent: text and move keep their own intents -- folding either into a "
          "neighbour would lose a distinction SDL is perfectly able to draw");
  }

  std::printf("  -- C. the shapes: fourteen intents, eight distinct SDL cursors --\n");

  {
    // **This is the section the move to SDL was for.** Routed through Dear
    // ImGui, Paint, Select and Sample were all `ImGuiMouseCursor_Arrow`,
    // because that enum contains no crosshair -- Arrow, TextInput, four Resize
    // shapes, Hand, Wait, Progress, NotAllowed is the whole of it, and the SDL3
    // backend builds exactly one system cursor per value, so SDL's own
    // `SDL_SYSTEM_CURSOR_CROSSHAIR` was unreachable. That left the brush, all
    // four selection tools, the eyedropper and the bucket sharing one plain
    // arrow: the six tools anyone actually uses.
    //
    // Talking to SDL directly ends that, and the assertion is now the
    // *opposite* of what it was: these three must be DISTINCT, and a table that
    // collapses any two of them fails here.
    const SDL_SystemCursor paint = sdlCursorFor(ToolCursor::Paint);
    const SDL_SystemCursor select = sdlCursorFor(ToolCursor::Select);
    const SDL_SystemCursor sample = sdlCursorFor(ToolCursor::Sample);
    check(paint != select && paint != sample && select != sample,
          "shape: paint, select and sample are three DIFFERENT cursors -- the whole point "
          "of owning the cursor rather than borrowing ImGui's crosshair-less set");

    // Named, not merely distinct: a mapping that swapped two of these would
    // pass the line above while showing an I-beam over the brush.
    check(paint == SDL_SYSTEM_CURSOR_CROSSHAIR,
          "shape: the brush is a CROSSHAIR -- the shape a painter expects over a tip "
          "whose exact centre is about to matter, and the one ImGui could not give");
    check(select == SDL_SYSTEM_CURSOR_NWSE_RESIZE && sample == SDL_SYSTEM_CURSOR_POINTER &&
              sdlCursorFor(ToolCursor::Zoom) == SDL_SYSTEM_CURSOR_NESW_RESIZE &&
              sdlCursorFor(ToolCursor::Text) == SDL_SYSTEM_CURSOR_TEXT &&
              sdlCursorFor(ToolCursor::Arrow) == SDL_SYSTEM_CURSOR_DEFAULT,
          "shape: select, sample, zoom, text and arrow are each the shape ui/ToolCursor "
          "section 3 names -- so a silent re-map has to change the documented table too");

    // **The one collision left, asserted as an equality on purpose.** SDL has
    // exactly one "drag and something follows" cursor, and panning the view and
    // moving content both mean precisely that. Unlike the ImGui collisions this
    // section used to pin, this one is fair rather than forced: it would
    // survive a richer set, because the two really do mean the same thing about
    // the pointer. Written as an equality so that splitting them later is a
    // deliberate act with a test to update, not an accident.
    check(sdlCursorFor(ToolCursor::Pan) == SDL_SYSTEM_CURSOR_MOVE &&
              sdlCursorFor(ToolCursor::MoveObject) == SDL_SYSTEM_CURSOR_MOVE,
          "shape: pan and move DELIBERATELY share the four-pointed arrow -- the view and "
          "the content both follow the drag, and SDL has one shape that means that");

    // Everything, counted. Fourteen intents onto eight shapes, with exactly
    // two collisions -- Pan/MoveObject, and Select plus T17's five newly-split
    // selection intents -- the arithmetic that catches a projection quietly
    // collapsing a THIRD group while every named assertion above still
    // passes.
    {
      const ToolCursor all[] = {
          ToolCursor::Arrow,       ToolCursor::Paint,        ToolCursor::Select,
          ToolCursor::Sample,      ToolCursor::Pan,          ToolCursor::Zoom,
          ToolCursor::MoveObject,  ToolCursor::Text,         ToolCursor::Refuse,
          // T17's five. Each is a genuinely new INTENT (section B proved
          // that), but with the bitmap flag off every one of them still
          // projects to `Select`'s own shape -- this is where that
          // collapse is checked at the shape level, not just named for the
          // five individually as section 3's header comment does.
          ToolCursor::SelectMarquee, ToolCursor::SelectEllipseMarquee,
          ToolCursor::SelectLasso,   ToolCursor::SelectPolygonLasso,
          ToolCursor::SelectMagicWand};
      std::vector<SDL_SystemCursor> shapes;
      for (const ToolCursor c : all) {
        const SDL_SystemCursor s = sdlCursorFor(c);
        std::printf("    %-22s -> SDL system cursor %d\n", toolCursorName(c),
                    static_cast<int>(s));
        if (std::find(shapes.begin(), shapes.end(), s) == shapes.end()) shapes.push_back(s);
      }
      // 14 intents, 8 distinct shapes: Pan/MoveObject share one collision,
      // and Select plus the five T17 split out of it share a second --
      // deliberately, and it is exactly the "flag off changes nothing"
      // guarantee hpp §7 promises. A third, UNPLANNED collision would still
      // land on 8 only by coincidence of which two shapes it merged, so this
      // count is a real check, not an approximation.
      check(shapes.size() == 8,
            "shape: fourteen intents use eight distinct cursors -- two deliberate "
            "collisions (Pan/MoveObject, and Select plus its five T17 offspring), so a "
            "third cannot creep in unnoticed");
    }

    check(sdlCursorFor(ToolCursor::Refuse) == SDL_SYSTEM_CURSOR_NOT_ALLOWED,
          "shape: a refusal is the slashed circle -- the one shape whose meaning a user "
          "does not have to be taught");

    // **The other half of owning the cursor**, and the half that breaks the
    // panels if it is wrong. With `ImGuiConfigFlags_NoMouseCursorChange` set,
    // nothing else will apply the I-beam in a text box or the resize arrows on
    // a window border, so `sdlCursorForImGui()` has to reproduce the backend's
    // own table. Checked against the mappings imgui_impl_sdl3.cpp makes at its
    // lines 624-634, since a wrong entry here is invisible in the canvas and
    // shows up only as a panel behaving oddly.
    check(sdlCursorForImGui(ImGuiMouseCursor_Arrow) == SDL_SYSTEM_CURSOR_DEFAULT &&
              sdlCursorForImGui(ImGuiMouseCursor_TextInput) == SDL_SYSTEM_CURSOR_TEXT &&
              sdlCursorForImGui(ImGuiMouseCursor_ResizeAll) == SDL_SYSTEM_CURSOR_MOVE &&
              sdlCursorForImGui(ImGuiMouseCursor_ResizeNS) == SDL_SYSTEM_CURSOR_NS_RESIZE &&
              sdlCursorForImGui(ImGuiMouseCursor_ResizeEW) == SDL_SYSTEM_CURSOR_EW_RESIZE,
          "backend: ImGui's own cursors map to the SDL shapes its backend used -- the "
          "suppressed backend's job, and a wrong entry shows only as an odd panel");
    check(sdlCursorForImGui(ImGuiMouseCursor_ResizeNESW) == SDL_SYSTEM_CURSOR_NESW_RESIZE &&
              sdlCursorForImGui(ImGuiMouseCursor_ResizeNWSE) == SDL_SYSTEM_CURSOR_NWSE_RESIZE &&
              sdlCursorForImGui(ImGuiMouseCursor_Hand) == SDL_SYSTEM_CURSOR_POINTER &&
              sdlCursorForImGui(ImGuiMouseCursor_Wait) == SDL_SYSTEM_CURSOR_WAIT &&
              sdlCursorForImGui(ImGuiMouseCursor_Progress) == SDL_SYSTEM_CURSOR_PROGRESS &&
              sdlCursorForImGui(ImGuiMouseCursor_NotAllowed) == SDL_SYSTEM_CURSOR_NOT_ALLOWED,
          "backend: and so do the remaining six -- all eleven are covered, which is what "
          "the static_assert on ImGuiMouseCursor_COUNT keeps true");

    // `ImGuiMouseCursor_None` is -1. `apply()` catches it before it is ever
    // used as a subscript, but the projection must not read off the front of a
    // table either if a later caller forgets.
    check(sdlCursorForImGui(ImGuiMouseCursor_None) == SDL_SYSTEM_CURSOR_DEFAULT,
          "backend: the None sentinel answers the default arrow rather than indexing at "
          "-1 -- it is a hide request, not a shape, and apply() handles it first");
  }

  std::printf("  -- D. a tool that will be REFUSED says so, before the gesture --\n");

  {
    // Each of these is a gesture that lands nowhere today, and produces a
    // sentence in the options bar afterwards. The cursor is the same answer,
    // delivered where the user is already looking and before the stroke is
    // spent rather than after.
    struct Case {
      const char* what;
      Tool tool;
      const Layer* target;
    };
    const Case refusals[] = {
        // The locked layer, for both paintable kinds -- `strokeRouteFor()`
        // tests locked before kind, so a locked Pigment layer must refuse for
        // being locked and not accidentally succeed on being Pigment.
        {"brush on locked RGB", Tool::Brush, &lockedRgb},
        {"brush on locked Pigment", Tool::Brush, &lockedPigment},
        // The kind with nowhere to put paint.
        {"brush on Adjustment", Tool::Brush, &adjustment},
        {"dry brush on Adjustment", Tool::DryBrush, &adjustment},
        // **The bucket on a Pigment layer**, which is the one a user reaches by
        // accident: CONTEXT.md makes Pigment the default kind for a new layer
        // and it is the NEW popup's first entry, so "add a layer, pick the
        // bucket, click" lands exactly here.
        {"bucket on Pigment", Tool::PaintBucket, &pigment},
        {"bucket on Adjustment", Tool::PaintBucket, &adjustment},
        {"bucket on locked RGB", Tool::PaintBucket, &lockedRgb},
        {"gradient on Pigment", Tool::Gradient, &pigment},
        // No document at all: the fill ops have nowhere to write, which is
        // `PixelOpRefusal::NoLayer` and a genuine refusal.
        {"bucket with no document", Tool::PaintBucket, nullptr},
    };

    for (const Case& c : refusals) {
      const ToolCursor got = toolCursorOnTarget(c.tool, c.target);
      std::printf("    %-26s -> %s\n", c.what, toolCursorName(got));
      check(got == ToolCursor::Refuse,
            "refusal: the pointer says no BEFORE the gesture -- without this the stroke "
            "or click is spent and the only feedback is a sentence in another band");
    }

    // And the refusal really is read from the shared predicates rather than
    // decided again here. If these two disagreed with the cursor, the cursor
    // would be a fourth opinion about a question `app/StrokeSession` exists to
    // keep down to one.
    check(strokeRouteFor(Tool::Brush, &adjustment) == StrokeRoute::None &&
              !pixelOpWritesLayer(&pigment),
          "refusal: the predicates the cursor reads still answer refuse for the same two "
          "layers -- so the cursor is that answer rendered, not a second opinion");
  }

  std::printf("  -- E. a tool that will SUCCEED is not shown as refused --\n");

  {
    // **The negative case, and the reason D cannot pass vacuously.** A
    // `toolCursorOnTarget()` hard-wired to `Refuse` satisfies every line above.
    struct Case {
      const char* what;
      Tool tool;
      const Layer* target;
      ToolCursor want;
    };
    const Case successes[] = {
        {"brush on Pigment", Tool::Brush, &pigment, ToolCursor::Paint},
        {"brush on RGB", Tool::Brush, &rgb, ToolCursor::Paint},
        {"dry brush on RGB", Tool::DryBrush, &rgb, ToolCursor::Paint},
        {"bucket on RGB", Tool::PaintBucket, &rgb, ToolCursor::Paint},
        {"gradient on RGB", Tool::Gradient, &rgb, ToolCursor::Paint},
        // **No document is NOT a refusal for a stroke tool**, and this is the
        // row most likely to be got wrong by treating "does not write a layer"
        // as "does nothing". `strokeRouteFor()`'s last row sends a stroke with
        // no document to the solver's dense canvas texture, which is a real
        // destination -- every medium demo paints it -- so a slashed circle
        // over a canvas about to accept watercolour would be a lie.
        {"brush with no document", Tool::Brush, nullptr, ToolCursor::Paint},
        {"water with no document", Tool::Water, nullptr, ToolCursor::Paint},
        // Water routes to the solver on EVERY layer kind, so it never refuses
        // -- including over the Adjustment layer that refuses the brush.
        {"water on Adjustment", Tool::Water, &adjustment, ToolCursor::Paint},
        // **The over-eager-refusal cases.** A lock and an unwritable kind stop
        // tools that WRITE. They must not stop reading, selecting or panning:
        // sampling a colour off a locked layer, dragging a marquee across an
        // Adjustment layer and panning the view are all perfectly legal, and a
        // refusal rule that keyed on the layer rather than on the tool would
        // wrongly slash all three.
        {"eyedropper on locked RGB", Tool::Eyedropper, &lockedRgb, ToolCursor::Sample},
        // T17: these four used to want plain `Select` here, back when all
        // four fell into that one arm of `cursorForTool()`. Each now wants
        // its own split-out intent -- the case most likely to be missed by a
        // patch that updated `cursorForTool()` but forgot this table still
        // asserted the pre-split answer, which would make this section pass
        // while the defect T17 reported was still showing on screen.
        {"marquee on Adjustment", Tool::Marquee, &adjustment, ToolCursor::SelectMarquee},
        {"ellipse marquee on Adjustment", Tool::EllipseMarquee, &adjustment,
         ToolCursor::SelectEllipseMarquee},
        {"lasso on locked Pigment", Tool::Lasso, &lockedPigment, ToolCursor::SelectLasso},
        {"polygon lasso on Adjustment", Tool::PolygonLasso, &adjustment,
         ToolCursor::SelectPolygonLasso},
        {"wand on Adjustment", Tool::MagicWand, &adjustment, ToolCursor::SelectMagicWand},
        {"hand on locked RGB", Tool::Hand, &lockedRgb, ToolCursor::Pan},
        {"zoom on Adjustment", Tool::Zoom, &adjustment, ToolCursor::Zoom},
    };

    for (const Case& c : successes) {
      const ToolCursor got = toolCursorOnTarget(c.tool, c.target);
      std::printf("    %-26s -> %s\n", c.what, toolCursorName(got));
      check(got != ToolCursor::Refuse,
            "success: a gesture that WILL land is not slashed -- a function that always "
            "answers 'not allowed' passes section D and dies here");
      check(got == c.want,
            "success: and it keeps the tool's own intent -- downgrading a working tool to "
            "a plain arrow would lose the signal without admitting it");
    }
  }

  std::printf("  -- F. the palette cells that are not built yet --\n");

  {
    // **Chosen answer: Refuse.** Fifteen of the twenty-eight Tool values are
    // name/icon/slot only, and dragging one across the canvas does nothing
    // whatsoever. The gentler option -- show each its natural intent -- was
    // rejected because it reproduces the exact failure this file exists to end:
    // the user scrubs, nothing happens, and nothing says why. The palette
    // already dims these cells; the cursor now agrees with it rather than
    // contradicting it. See ui/ToolCursor.hpp §5, which also argues the case
    // against.
    //
    // Checked against a perfectly writable RGB layer, so "refused" is a claim
    // about the tool and not about the target.
    int unbuilt = 0;
    int built = 0;
    bool everyUnbuiltRefuses = true;
    bool everyBuiltWorks = true;
    for (int i = 0; i < static_cast<int>(Tool::Count); ++i) {
      const Tool t = static_cast<Tool>(i);
      const ToolCursor got = toolCursorOnTarget(t, &rgb);
      if (toolImplemented(t)) {
        ++built;
        // Every implemented tool over an unlocked RGB layer does something:
        // the paint family deposits, the fills fill, the selection tools
        // select, the Hand pans and the Zoom zooms. None of them may be
        // slashed here.
        if (got == ToolCursor::Refuse) everyBuiltWorks = false;
      } else {
        ++unbuilt;
        if (got != ToolCursor::Refuse) everyUnbuiltRefuses = false;
      }
    }

    std::printf("    %d built, %d not built\n", built, unbuilt);

    check(unbuilt > 0 && built > 0,
          "unbuilt: the palette really does hold both kinds -- an all-or-nothing split "
          "would make both assertions below vacuous");
    check(everyUnbuiltRefuses,
          "unbuilt: every tool toolImplemented() calls unbuilt is slashed -- picking one "
          "and dragging does nothing, and the pointer is where the user will find out");
    check(everyBuiltWorks,
          "unbuilt: and every BUILT tool is usable over an unlocked RGB layer -- the "
          "unbuilt rule must not spill onto a tool that works");

    // The rule is keyed on `toolImplemented()` and nothing else, so a tool
    // shipping flips its cursor with no edit in ui/ToolCursor -- the intent it
    // will want is already written in `cursorForTool()`. Pinned on two of them
    // so a future implementation that forgot the cursor cannot go unnoticed.
    check(!toolImplemented(Tool::Pencil) && cursorForTool(Tool::Pencil) == ToolCursor::Paint &&
              !toolImplemented(Tool::Text) && cursorForTool(Tool::Text) == ToolCursor::Text,
          "unbuilt: an unbuilt tool still has its intent recorded -- the day it ships, "
          "toolImplemented() flips and the right cursor appears with no edit here");

    // **And that claim has already been tested for real.** This assertion
    // originally named the Eraser as one of its two unbuilt examples. The
    // Eraser shipped in the same batch as this file -- `brush/RgbErase`, and
    // `toolImplemented()` flipped with it -- so the line went red on the merge
    // for the one reason a prediction can go red: it came true. The cursor
    // needed no edit in `ui/ToolCursor`, which is exactly what the line above
    // promises about every remaining unbuilt tool.
    //
    // Kept as its own assertion rather than folded into that one, because the
    // two say different things: the line above is a promise about tools that
    // have not shipped, and this is the evidence that the promise held once.
    check(toolImplemented(Tool::Eraser) && cursorForTool(Tool::Eraser) == ToolCursor::Paint,
          "unbuilt: the Eraser shipped carrying the intent it was written with while it "
          "was still unbuilt -- the promise above, already collected once");
  }

  std::printf("  -- G. §7's bitmap cursors: non-blank, hotspot bounds, and flag-off identity --\n");

  {
    // Headless the same way sections A-F are: `rasterizeToolCursorBitmap()`
    // reads the vendored Lucide TTF through stb_truetype directly (no
    // `ImFontAtlas`, no `GImGui`) and `shouldUseBitmapCursor()` is a pure
    // comparison -- neither needs SDL video or a window. What genuinely
    // cannot be covered here is `SystemCursorTable` itself, for the same
    // reason §6's own untested branches cannot: it calls into SDL and the
    // platform. See ui/ToolCursor.hpp §7's own comment.

    // -- G1. every bitmap-backed tool rasterises to something, not nothing --
    //
    // **This is the answer to objection 1** -- "a missing font gives a
    // blank cursor" -- turned into a check for the first time anywhere in
    // this codebase touching `NP_LUCIDE_TTF`. `installToolIconFont()`
    // (ui/Fonts.cpp) reports a missing font as a string nobody is required
    // to read; this fails the build's own `--selftest`.
    const ToolCursor bitmapCursors[] = {ToolCursor::SelectMarquee, ToolCursor::SelectEllipseMarquee,
                                        ToolCursor::SelectLasso, ToolCursor::SelectPolygonLasso,
                                        ToolCursor::SelectMagicWand};
    bool everyBitmapNonBlank = true;
    CursorBitmap bitmaps[5];
    for (size_t i = 0; i < std::size(bitmapCursors); ++i) {
      bitmaps[i] = rasterizeToolCursorBitmap(bitmapCursors[i]);
      std::printf("    %-22s -> %dx%d, hotspot (%d,%d), %s\n", toolCursorName(bitmapCursors[i]),
                  bitmaps[i].width, bitmaps[i].height, bitmaps[i].hotspotX, bitmaps[i].hotspotY,
                  bitmaps[i].nonBlank ? "non-blank" : "BLANK");
      if (!bitmaps[i].nonBlank) everyBitmapNonBlank = false;
    }
    check(everyBitmapNonBlank,
          "bitmap: every one of T17's five cursors rasterises to at least one visible pixel "
          "-- a blank one here is sabotage (a)'s target, and is what SHOULD go red for a "
          "font that silently failed to load or a codepoint the vendored build dropped");

    // **What a blank one here does NOT get to do: become an installed OS
    // cursor.** That half of objection 1's answer lives in
    // `SystemCursorTable::create()`'s own `if (!bitmap.nonBlank) continue;`
    // (ui/ToolCursor.cpp) -- code this section cannot exercise, because
    // `create()` needs live SDL video and `--selftest` never opens a window,
    // exactly the limitation this file's own top comment already admits for
    // every other `SystemCursorTable` branch. It is argued at that call site
    // rather than asserted here for the same reason those other branches
    // are: a headless test of a function that is a no-op without SDL video
    // would be testing something adjacent to the real code, not the code
    // itself. What THIS section proves is the fact `create()`'s guard reads
    // -- `nonBlank` -- which is the check immediately above, and which
    // sabotage (a) below is shown against directly rather than through a
    // second function that would only restate the same boolean.

    // Every OTHER `ToolCursor` value has no bitmap at all, and answers a
    // fully transparent, zero-sized-content result -- not merely `false` for
    // `toolCursorHasBitmap()`, but a `rasterizeToolCursorBitmap()` that
    // agrees with it, since `SystemCursorTable::create()` calls the second
    // function guarded by the first and both have to tell the truth for that
    // guard to mean anything.
    check(!toolCursorHasBitmap(ToolCursor::Arrow) && !toolCursorHasBitmap(ToolCursor::Paint) &&
              !toolCursorHasBitmap(ToolCursor::Select) && !toolCursorHasBitmap(ToolCursor::Refuse) &&
              !rasterizeToolCursorBitmap(ToolCursor::Select).nonBlank,
          "bitmap: a tool T17 did not name has no bitmap and rasterises to nothing -- "
          "`toolCursorHasBitmap()` and `rasterizeToolCursorBitmap()` agree on every value "
          "they were not both told yes about");
    check(toolCursorHasBitmap(ToolCursor::SelectMarquee) && toolCursorHasBitmap(ToolCursor::SelectEllipseMarquee) &&
              toolCursorHasBitmap(ToolCursor::SelectLasso) && toolCursorHasBitmap(ToolCursor::SelectPolygonLasso) &&
              toolCursorHasBitmap(ToolCursor::SelectMagicWand),
          "bitmap: and exactly T17's five answer yes -- the set this task was asked to give "
          "a real shape to, no more and no fewer");

    // -- G2. every hotspot lands inside the pixels it is a hotspot OF --
    //
    // **This is the answer to objection 2** -- "a hotspot nothing in
    // --selftest could check". A hotspot outside the drawn glyph would put
    // the OS's notion of "where this cursor points" on a transparent pixel,
    // which is sabotage (b)'s target.
    auto alphaBounds = [](const CursorBitmap& b, int* minX, int* minY, int* maxX, int* maxY) {
      bool any = false;
      *minX = *minY = 0;
      *maxX = *maxY = 0;
      for (int y = 0; y < b.height; ++y)
        for (int x = 0; x < b.width; ++x)
          if (b.rgba[(static_cast<size_t>(y) * b.width + x) * 4 + 3] != 0) {
            if (!any) {
              *minX = *maxX = x;
              *minY = *maxY = y;
              any = true;
            } else {
              *minX = std::min(*minX, x);
              *maxX = std::max(*maxX, x);
              *minY = std::min(*minY, y);
              *maxY = std::max(*maxY, y);
            }
          }
      return any;
    };

    bool everyHotspotInBounds = true;
    for (size_t i = 0; i < std::size(bitmapCursors); ++i) {
      int minX, minY, maxX, maxY;
      const bool any = alphaBounds(bitmaps[i], &minX, &minY, &maxX, &maxY);
      const bool inBounds = any && bitmaps[i].hotspotX >= minX && bitmaps[i].hotspotX <= maxX &&
                            bitmaps[i].hotspotY >= minY && bitmaps[i].hotspotY <= maxY;
      std::printf("    %-22s glyph bounds x[%d,%d] y[%d,%d], hotspot %s\n",
                  toolCursorName(bitmapCursors[i]), minX, maxX, minY, maxY,
                  inBounds ? "inside" : "OUTSIDE");
      if (!inBounds) everyHotspotInBounds = false;
    }
    check(everyHotspotInBounds,
          "bitmap: every hotspot lies inside its own bitmap's drawn (non-transparent) "
          "bounding box -- a hotspot on a transparent pixel points at nothing, which is "
          "sabotage (b)'s target");

    // **The specific claim the report makes for the marquee pair**: the
    // hotspot is the CROSSHAIR's centre, not the shape's own centre and not
    // the canvas's centre. `drawMarqueeCrosshair()` places the crosshair at
    // (6, 26) and the shape's centre near (20, 12) in its 32x32 canvas --
    // both pinned here so a generator that moved the crosshair without
    // moving the hotspot with it, or vice-versa, cannot pass by accident.
    const CursorBitmap& marqueeBitmap = bitmaps[0];
    const CursorBitmap& ellipseMarqueeBitmap = bitmaps[1];
    check(marqueeBitmap.hotspotX == 6 && marqueeBitmap.hotspotY == 26 &&
              ellipseMarqueeBitmap.hotspotX == 6 && ellipseMarqueeBitmap.hotspotY == 26,
          "bitmap: both marquees' hotspot is the crosshair's own centre pixel (6, 26) -- "
          "the exact bug report ('a crosshair at the bottom-left') is about this point, not "
          "merely about SOME pixel inside the composite");
    check(marqueeBitmap.hotspotX != marqueeBitmap.width / 2 || marqueeBitmap.hotspotY != marqueeBitmap.height / 2,
          "bitmap: ...and that point is NOT the canvas centre -- a generator that centred "
          "the hotspot instead of tracking the crosshair would still pass G2 (16,16) sits "
          "inside the drawn bounds) and only this line catches it");

    // -- G3. flag-off identity, PROVED rather than asserted for one case --
    //
    // **This is the mechanical proof ui/ToolCursor.hpp §7 promises.**
    // `shouldUseBitmapCursor()` is the ONLY branch `SystemCursorTable::apply()`
    // gained; everything else in that function is §6's original code,
    // unedited (visible by reading it, not provable by a headless test since
    // `apply()` itself needs live SDL video). What CAN be proved headlessly
    // is that this one new branch is inert whenever the flag is off, for
    // EVERY `ToolCursor` value and both possible `hasBitmap` answers -- not
    // merely for one tool, which is what "proved" means here as opposed to
    // "asserted".
    const ToolCursor allCursors[] = {
        ToolCursor::Arrow,      ToolCursor::Paint,       ToolCursor::Select,
        ToolCursor::Sample,     ToolCursor::Pan,         ToolCursor::Zoom,
        ToolCursor::MoveObject, ToolCursor::Text,        ToolCursor::Refuse,
        ToolCursor::SelectMarquee, ToolCursor::SelectEllipseMarquee, ToolCursor::SelectLasso,
        ToolCursor::SelectPolygonLasso, ToolCursor::SelectMagicWand};
    bool flagOffAlwaysFalse = true;
    for (const ToolCursor c : allCursors) {
      if (shouldUseBitmapCursor(/*bitmapsEnabled=*/false, c, /*hasBitmap=*/true)) flagOffAlwaysFalse = false;
      if (shouldUseBitmapCursor(/*bitmapsEnabled=*/false, c, /*hasBitmap=*/false)) flagOffAlwaysFalse = false;
    }
    if (shouldUseBitmapCursor(/*bitmapsEnabled=*/false, std::nullopt, /*hasBitmap=*/true))
      flagOffAlwaysFalse = false;
    check(flagOffAlwaysFalse,
          "flag: shouldUseBitmapCursor(false, ...) answers false for EVERY ToolCursor value "
          "and both hasBitmap answers, exhaustively -- SystemCursorTable::apply()'s one new "
          "branch is provably inert with the flag off, which is what sabotage (c) below "
          "would have to defeat by breaking sdlCursorFor() instead, not this function");

    // **And the flag really is OFF to begin with.** Everything above proves
    // what happens WHEN it is false; nothing above proves that it IS. Those
    // are different claims, and only the second one is the promise this whole
    // step rests on -- ui/ToolCursor.hpp §7 keeps bitmaps behind one flag
    // precisely so the accessibility cost of losing OS cursor-size scaling
    // stays a decision someone makes on purpose. Flipping the initialiser to
    // `true` reddens NOTHING without this line (measured, not supposed), so a
    // one-word edit could ship that decision by accident.
    //
    // Default-constructed and never `create()`d: the flag is a plain member
    // and this getter touches no SDL, which is what makes the one property
    // worth pinning reachable from a headless suite at all.
    const SystemCursorTable freshTable;
    check(!freshTable.bitmapCursorsEnabled(),
          "flag: a freshly constructed SystemCursorTable has bitmap cursors OFF -- today's "
          "behaviour is the DEFAULT, not merely what the flag would give if someone set it; "
          "turning bitmaps on is a deliberate act because it trades away OS cursor-size "
          "accessibility scaling (docs/testing-issues.md T17)");

    // The gate is not vacuously false, either -- section D's own lesson,
    // repeated: a function that always answers false would pass the loop
    // above and still be useless. Flip the flag on for these two lines only.
    check(shouldUseBitmapCursor(/*bitmapsEnabled=*/true, ToolCursor::SelectLasso, /*hasBitmap=*/true) &&
              !shouldUseBitmapCursor(/*bitmapsEnabled=*/true, ToolCursor::SelectLasso, /*hasBitmap=*/false) &&
              !shouldUseBitmapCursor(/*bitmapsEnabled=*/true, std::nullopt, /*hasBitmap=*/true),
          "flag: with bitmapsEnabled TRUE the gate answers true only when there both IS a "
          "tool request and it has a real bitmap -- the positive case D's own vacuous-refusal "
          "lesson requires this section to also cover");

    // -- G4. the end-to-end claim: today's five tools show today's shape --
    //
    // The strongest form of "flag off is byte-identical to today": not the
    // enum value (section B already changed that on purpose) and not the
    // gate function in isolation (G3), but the FULL `Tool -> ToolCursor ->
    // SDL_SystemCursor` pipeline for the five actual tools a user picks from
    // the palette, compared against `SDL_SYSTEM_CURSOR_NWSE_RESIZE` -- the
    // literal value this whole task started from (docs/testing-issues.md
    // T17: "and sdlCursorFor() turns that into SDL_SYSTEM_CURSOR_NWSE_RESIZE").
    check(sdlCursorFor(cursorForTool(Tool::Marquee)) == SDL_SYSTEM_CURSOR_NWSE_RESIZE &&
              sdlCursorFor(cursorForTool(Tool::EllipseMarquee)) == SDL_SYSTEM_CURSOR_NWSE_RESIZE &&
              sdlCursorFor(cursorForTool(Tool::Lasso)) == SDL_SYSTEM_CURSOR_NWSE_RESIZE &&
              sdlCursorFor(cursorForTool(Tool::PolygonLasso)) == SDL_SYSTEM_CURSOR_NWSE_RESIZE &&
              sdlCursorFor(cursorForTool(Tool::MagicWand)) == SDL_SYSTEM_CURSOR_NWSE_RESIZE,
          "flag: end to end, all five tools STILL show the diagonal resize arrow today's "
          "build showed -- sabotage (c) targets exactly this line, by changing what one of "
          "these five (or plain Select) maps to in sdlCursorFor()");
  }

  // The section verdict every other section prints. Without it this file's
  // assertions still reach main.cpp's `ok` chain, but a reader scanning a run
  // for section names cannot see that it ran at all -- which is how a section
  // that silently stopped being called gets missed.
  std::printf("[selftest] tool cursor %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
