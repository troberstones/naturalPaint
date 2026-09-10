#include "app/selftest/Support.hpp"

#include "app/Command.hpp"
#include "app/LayerEditor.hpp"
#include "app/Recorder.hpp"
#include "core/Channels.hpp"
#include "core/LayerOps.hpp"
#include "core/SelectionMask.hpp"

namespace np {
namespace {

// A two-layer RGB document with content on both and "Detail" selected, which
// is the shape docs/automation-plan.md's own worked case starts from.
//
// Built here rather than borrowed from app/selftest/Command.cpp's fixture:
// internal linkage does not cross a translation unit, and a shared fixture
// would make this section's answers depend on edits made for that one.
OpenDocument makeRecorderDocument() {
  OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "recorder");
  od.document.layers[0].name = "Base";
  auto fill = [](TileStore& tiles, float v) {
    Tile& t = tiles.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y)
      for (int32_t x = 0; x < kTileSize; ++x)
        t.writePixel(PixelCoord{x, y}, {v, v * 0.5f, 1.0f - v, 1.0f});
  };
  fill(*od.document.layers[0].rgbTiles, 0.25f);
  const LayerEditResult added = applyLayerCommand(od, LayerCommand::NewRgbLayer, 0);
  if (added.ok && od.document.layers.size() == 2) {
    od.document.layers[added.selected].name = "Detail";
    if (od.document.layers[added.selected].rgbTiles)
      fill(*od.document.layers[added.selected].rgbTiles, 0.75f);
    od.activeLayer = added.selected;
  }
  od.recordEdit("recorder fixture", EditKind::Structural);
  return od;
}

// The recorded step's id at `i`, or "" past the end -- so a size assertion
// that has already failed does not become an out-of-range read in the next
// line.
std::string idAt(const std::vector<Command>& steps, size_t i) {
  return i < steps.size() ? steps[i].id : std::string();
}

std::string layerAt(const std::vector<Command>& steps, size_t i) {
  return i < steps.size() ? steps[i].params.stringOr("layer", "") : std::string();
}

size_t countId(const std::vector<Command>& steps, const char* id) {
  size_t n = 0;
  for (const Command& c : steps)
    if (c.id == id) ++n;
  return n;
}

Command blurStep(double sigma) {
  JsonValue p = JsonValue::object();
  p.set("sigma", JsonValue::number(sigma));
  return Command{"filter_gaussian_blur", p};
}

Command thresholdStep() {
  JsonValue p = JsonValue::object();
  p.set("threshold", JsonValue::number(0.5));
  p.set("amount", JsonValue::number(1.0));
  return Command{"adjust_threshold", p};
}

Command blendStep(const char* mode) {
  JsonValue p = JsonValue::object();
  p.set("mode", JsonValue::string(mode));
  return Command{"set_layer_blend", p};
}

}  // namespace

// app/Recorder (docs/automation-plan.md step 3) -- what `applyCommand()`
// writes down while a recording is armed.
//
// **What this section is for.** Nothing here re-tests a blur or a flatten;
// app/selftest/Command.cpp already drives those through the same door. What is
// new is the *sink*, and every property below is a way a recording can be
// wrong while every assertion about the document stays green:
//
//   * a step that never happened (a refused command recorded anyway) replays
//     onto a document where its precondition holds and does something the user
//     never did;
//   * a step whose target was never written down (no `select_layer` after the
//     active layer moved) replays onto whatever layer the other document
//     happens to have selected, and reports success;
//   * a step recorded under a marquee replays with nothing selected, and
//     nothing selected means NO RESTRICTION -- so it covers the whole canvas,
//     and reports success.
//
// All three are silent. That is why this section asserts the recorded LIST --
// ids, order and parameters -- rather than that a recording happened.
// Headless, GPU-free and filesystem-free.
bool runRecorderTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, std::string_view needle) {
    return s.find(needle) != std::string::npos;
  };

  Recorder& session = sessionRecorder();

  std::printf("  -- A. arm, stop, re-arm --\n");
  {
    check(&sessionRecorder() == &session,
          "session: there is exactly one recorder, and it is the tap's");

    // Idle is only reachable on a recorder nothing has armed, so this one is
    // local. `note()` is the sink itself, so driving it directly is the whole
    // of what "an idle recorder appends nothing" means.
    Recorder fresh;
    check(fresh.state() == Recorder::State::Idle && fresh.steps().empty(),
          "idle: a new recorder is idle and holds nothing");
    CommandResult success;
    success.ok = true;
    success.status = "did a thing";
    fresh.note(Command{"flatten_image", JsonValue::object()}, success, RecorderPreState{});
    check(fresh.steps().empty(), "idle: an idle recorder appends nothing");
    fresh.stop();
    check(fresh.state() == Recorder::State::Idle,
          "idle: stopping something that was never armed leaves it idle");

    OpenDocument od = makeRecorderDocument();
    session.arm(od);
    check(session.state() == Recorder::State::Recording && session.isRecording(),
          "arm: the session recorder is recording");
    applyCommand(od, blurStep(1.5));
    check(session.steps().size() == 1, "arm: an armed recorder took the step");

    session.stop();
    check(session.state() == Recorder::State::Stopped && session.steps().size() == 1,
          "stop: stopped, and the steps are still readable");
    applyCommand(od, thresholdStep());
    check(session.steps().size() == 1, "stop: a stopped recorder appends nothing");

    session.arm(od);
    check(session.state() == Recorder::State::Recording && session.steps().empty(),
          "re-arm: a new recording starts empty");
    session.stop();
  }

  std::printf("  -- B. the plan's own case records as five steps, in order --\n");
  {
    // flatten -> blur -> set blend to Subtract -> threshold, every one of them
    // through `applyCommand()`. The fifth step is the one nobody asked for and
    // everything depends on: the flatten moved the active layer, so the blur
    // that follows means nothing until the recording says which layer it is.
    OpenDocument od = makeRecorderDocument();
    session.arm(od);
    const CommandResult flat = applyCommand(od, Command{"flatten_image", JsonValue::object()});
    const CommandResult blurred = applyCommand(od, blurStep(2.0));
    const CommandResult blended = applyCommand(od, blendStep("subtract"));
    const CommandResult thresholded = applyCommand(od, thresholdStep());
    session.stop();
    check(flat.ok && blurred.ok && blended.ok && thresholded.ok,
          "case: all four commands succeeded (the recording is of real work)");

    const std::vector<Command>& steps = session.steps();
    check(steps.size() == 5, "case: four commands recorded as exactly five steps");
    check(idAt(steps, 0) == "flatten_image" && idAt(steps, 1) == "select_layer" &&
              idAt(steps, 2) == "filter_gaussian_blur" && idAt(steps, 3) == "set_layer_blend" &&
              idAt(steps, 4) == "adjust_threshold",
          "case: the ids are in the order the work was done");

    // The survivor's real name, read off the document rather than repeated
    // from the plan: docs/automation-plan.md §5's example says "Background",
    // which is a name this build's flatten does not produce.
    const Layer* survivor = activeLayerOf(od);
    const std::string survivorName = survivor != nullptr ? survivor->name : std::string();
    check(!survivorName.empty() && layerAt(steps, 1) == survivorName,
          "case: the pin names the layer the flatten actually left selected");
    check(layerAt(steps, 3) == survivorName,
          "case: set_layer_blend carries its target's name, not an implied active layer");

    // A recorder that dropped or rewrote a parameter would still produce five
    // ids in the right order.
    check(steps.size() == 5 && steps[2].params.numberOr("sigma", -1.0) == 2.0,
          "case: the blur's sigma survived into the step");
    check(steps.size() == 5 && steps[3].params.stringOr("mode", "") == "subtract" &&
              steps[4].params.numberOr("threshold", -1.0) == 0.5 &&
              steps[4].params.numberOr("amount", -1.0) == 1.0,
          "case: every other parameter survived too");
    check(session.usable() && session.refusals().empty() && session.warnings().empty(),
          "case: a clean recording refuses nothing and warns about nothing");
  }

  std::printf("  -- C. a refused command is not a step --\n");
  {
    OpenDocument od = makeRecorderDocument();
    session.arm(od);

    // Route 1: an id this build has no row for. `applyCommand()` returns
    // before the tap is constructed.
    const CommandResult unknown =
        applyCommand(od, Command{"filter_kaleidoscope", JsonValue::object()});
    check(!unknown.ok && session.steps().empty(), "refused: an unknown id records nothing");

    // Route 2: the precondition says no. A locked layer is the case the UI
    // greys out.
    recordLayerEdit(od, setLayerLocked(od.document, od.activeLayer, true));
    const CommandResult locked = applyCommand(od, blurStep(2.0));
    check(!locked.ok && session.steps().empty(),
          "refused: a precondition refusal records nothing");
    recordLayerEdit(od, setLayerLocked(od.document, od.activeLayer, false));

    // Route 3: the adapter itself refuses after the precondition passed --
    // the only route where the tap exists and has to read `ok` for itself.
    // **This is the one the other two cannot stand in for**: it is the only
    // one where deleting the recorder's `ok` check still leaves every
    // assertion above green.
    const CommandResult noSigma =
        applyCommand(od, Command{"filter_gaussian_blur", JsonValue::object()});
    check(!noSigma.ok && session.steps().empty(),
          "refused: an adapter's own refusal records nothing either");

    // And the recorder is still live afterwards -- a refusal is not a stop.
    const CommandResult good = applyCommand(od, blurStep(1.0));
    check(good.ok && session.steps().size() == 1 &&
              idAt(session.steps(), 0) == "filter_gaussian_blur",
          "refused: the recording continues, and the next real step is the first one");
    session.stop();
  }

  std::printf("  -- D. the select_layer emission --\n");
  {
    // A CREATE is the strong case for the rule, and the one a recorder built
    // on "did the user click a layer row?" would miss entirely: nobody clicks
    // anything, `fromLayerEdit()` adopts `LayerEditResult::selected`, and
    // every step after it addresses a layer that was not selected before.
    OpenDocument od = makeRecorderDocument();
    const Layer* beforeCreate = activeLayerOf(od);
    const std::string wasActive = beforeCreate != nullptr ? beforeCreate->name : std::string();
    session.arm(od);
    check(applyCommand(od, Command{"new_rgb_layer", JsonValue::object()}).ok &&
              applyCommand(od, blurStep(1.0)).ok && applyCommand(od, thresholdStep()).ok,
          "create: the three commands ran");
    session.stop();

    const std::vector<Command>& steps = session.steps();
    const Layer* created = activeLayerOf(od);
    const std::string newName = created != nullptr ? created->name : std::string();
    check(steps.size() == 4, "create: three commands recorded as four steps");
    check(idAt(steps, 0) == "new_rgb_layer" && idAt(steps, 1) == "select_layer" &&
              idAt(steps, 2) == "filter_gaussian_blur" && idAt(steps, 3) == "adjust_threshold",
          "create: the pin lands between the create and the step that needs it");
    check(!newName.empty() && newName != wasActive && layerAt(steps, 1) == newName,
          "create: the pin names the NEW layer, not the one selected before");

    // Two edits on one layer are two steps, not four. A recorder that pinned
    // every step would also pass every assertion above.
    check(countId(steps, "select_layer") == 1,
          "same layer: back-to-back steps do not each re-pin the layer");
  }

  std::printf("  -- D2. a select_layer the user issued is not duplicated --\n");
  {
    // The recording's own `select_layer` IS the statement of which layer is
    // active, so the next step must not be preceded by the recorder's copy of
    // it. This is what "the baseline is what the recording SAID" buys, and a
    // recorder that instead compared against the previous command's starting
    // layer would emit two.
    OpenDocument od = makeRecorderDocument();
    session.arm(od);
    JsonValue pick = JsonValue::object();
    pick.set("layer", JsonValue::string("Base"));
    check(applyCommand(od, Command{"select_layer", pick}).ok && applyCommand(od, blurStep(1.0)).ok,
          "issued: the pick and the blur ran");
    session.stop();
    const std::vector<Command>& steps = session.steps();
    check(steps.size() == 2 && idAt(steps, 0) == "select_layer" && layerAt(steps, 0) == "Base" &&
              idAt(steps, 1) == "filter_gaussian_blur",
          "issued: two steps -- the user's pick, then the blur");
  }

  std::printf("  -- E. a live marquee no channel names --\n");
  {
    OpenDocument od = makeRecorderDocument();
    od.selection = selectRectangle(8.0f, 8.0f, 32.0f, 32.0f);
    session.arm(od);
    const CommandResult blurred = applyCommand(od, blurStep(2.0));
    session.stop();

    // The command itself is not interfered with: the user drew a marquee and
    // asked for a blur, and they get the blur. What is refused is writing it
    // down as a step, which is a recorder-level decision and nothing else.
    check(blurred.ok, "marquee: the command still ran -- the recorder refuses the STEP");
    check(session.steps().empty(), "marquee: nothing was recorded");
    check(session.refusals().size() == 1, "marquee: the refusal was reported");
    const std::string why = session.refusals().empty() ? std::string() : session.refusals()[0];
    check(contains(why, "filter_gaussian_blur"), "marquee: the refusal names the command");
    check(contains(why, "channel"), "marquee: the refusal names the fix");
    check(!session.usable(),
          "marquee: a recording with a hole in it is not offered as an action");
  }

  std::printf("  -- E2. the same step under a SAVED selection --\n");
  {
    OpenDocument od = makeRecorderDocument();
    const Selection marquee = selectRectangle(8.0f, 8.0f, 32.0f, 32.0f);
    const size_t index = saveSelectionAsChannel(od.document, marquee, "Panel");
    const std::string channelName =
        index < od.document.channels.size() ? od.document.channels[index].name : std::string();
    od.selection = marquee;

    session.arm(od);
    const CommandResult blurred = applyCommand(od, blurStep(2.0));
    session.stop();
    check(blurred.ok && session.steps().size() == 1 &&
              idAt(session.steps(), 0) == "filter_gaussian_blur",
          "saved: a selection the document carries by name IS recorded");
    check(session.refusals().empty() && session.usable(),
          "saved: nothing was refused");
    check(session.warnings().size() == 1 && contains(session.warnings()[0], channelName),
          "saved: and the step carries a warning naming the channel it needs");
  }

  std::printf("  -- E3. channelMatchingSelection() itself --\n");
  {
    OpenDocument od = makeRecorderDocument();
    const Selection marquee = selectRectangle(8.0f, 8.0f, 32.0f, 32.0f);
    saveSelectionAsChannel(od.document, marquee, "Panel");
    check(channelMatchingSelection(od.document, marquee) == "Panel",
          "match: a selection matches the channel it was saved as");

    // Same area, different shape. A comparison by coverage total, or by
    // bounding-box size, would put "Panel" on a marquee the user never drew.
    check(channelMatchingSelection(od.document, selectRectangle(0.0f, 0.0f, 24.0f, 24.0f)).empty(),
          "match: an equal-area rectangle elsewhere does not match");
    // One texel row taller: the same tile, one row of bytes apart. Only a
    // texel-exact comparison sees this.
    check(channelMatchingSelection(od.document, selectRectangle(8.0f, 8.0f, 32.0f, 33.0f)).empty(),
          "match: one texel row of difference does not match");
    // An empty selection has no non-zero tile, so a one-directional
    // containment test would report it as matching everything -- and every
    // marquee-under-recording would then be silently \"saved\".
    check(channelMatchingSelection(od.document, Selection{}).empty(),
          "match: an empty selection does not match a channel that covers something");

    Document bare = makeRecorderDocument().document;
    check(channelMatchingSelection(bare, marquee).empty(),
          "match: a document with no channels matches nothing");
  }

  std::printf("  -- F. a duplicated layer name is reported, not resolved silently --\n");
  {
    // Layer names are deliberately not unique (core/Channels.hpp says so while
    // contrasting them with channel names), and by-name targeting takes the
    // first match. A recording that wrote down an ambiguous name and said
    // nothing would replay onto a different layer of the same name -- on the
    // very document it was recorded from.
    OpenDocument od = makeRecorderDocument();
    od.document.layers[1].name = "Base";
    od.activeLayer = 1;
    session.arm(od);
    const CommandResult blended = applyCommand(od, blendStep("subtract"));
    session.stop();
    check(blended.ok && session.steps().size() == 1 && layerAt(session.steps(), 0) == "Base",
          "ambiguous: the step is recorded with the name it ran under");
    check(session.warnings().size() == 1 && contains(session.warnings()[0], "Base"),
          "ambiguous: and the ambiguity is reported, naming the layer");
  }

  std::printf("  -- G. the marquee rule reads the TABLE, not the texel count --\n");
  {
    // §4 used to decide which steps to police from
    // `CommandResult::changesPixels`. `CommandSpec::selectionBounded` replaced
    // it, and the two disagree about four commands -- three that were refused
    // for a reason that does not apply to them, and one that was not refused
    // and should have been. All four are asserted here through the real
    // recorder under a real live marquee, because the table walk in
    // app/selftest/Command.cpp section H can see only the claim and not the
    // behaviour it is supposed to produce.
    const Selection marquee = selectRectangle(8.0f, 8.0f, 32.0f, 32.0f);

    // The three that change pixels and are NOT bounded. Each acts on the whole
    // document by construction, so the marquee is not part of what the step
    // meant and nothing is missing from the file.
    {
      OpenDocument od = makeRecorderDocument();
      od.selection = marquee;
      session.arm(od);
      const CommandResult flat = applyCommand(od, Command{"flatten_image", JsonValue::object()});
      session.stop();
      check(flat.ok && session.refusals().empty() && session.usable(),
            "unbounded: a flatten under a live marquee is no longer refused");
      check(countId(session.steps(), "flatten_image") == 1,
            "unbounded: and it really is in the step list");
    }
    {
      OpenDocument od = makeRecorderDocument();
      od.selection = marquee;
      session.arm(od);
      JsonValue size = JsonValue::object();
      size.set("width", JsonValue::number(32));
      size.set("height", JsonValue::number(32));
      const CommandResult resized = applyCommand(od, Command{"image_size", size});
      session.stop();
      check(resized.ok && session.refusals().empty() &&
                countId(session.steps(), "image_size") == 1,
            "unbounded: a resize under a live marquee is recorded too");
    }
    {
      OpenDocument od = makeRecorderDocument();
      od.selection = marquee;
      session.arm(od);
      JsonValue canvas = JsonValue::object();
      canvas.set("width", JsonValue::number(96));
      canvas.set("height", JsonValue::number(96));
      const CommandResult grown = applyCommand(od, Command{"canvas_size", canvas});
      session.stop();
      check(grown.ok && session.refusals().empty() &&
                countId(session.steps(), "canvas_size") == 1,
            "unbounded: a canvas resize under a live marquee is recorded too");
    }

    // **The direction the old proxy got wrong silently.** `define_pattern`
    // reports `changesPixels == false` and IS bounded: its source rectangle is
    // the selection's bounds, absent meaning the whole canvas. Recorded under
    // a marquee and replayed with nothing selected, it defined a pattern the
    // size of the document and reported success.
    {
      OpenDocument od = makeRecorderDocument();
      od.selection = marquee;
      session.arm(od);
      JsonValue p = JsonValue::object();
      p.set("name", JsonValue::string("Swatch"));
      const CommandResult defined = applyCommand(od, Command{"define_pattern", p});
      session.stop();
      check(defined.ok, "bounded: define_pattern itself still ran");
      check(session.steps().empty() && session.refusals().size() == 1,
            "bounded: but a define_pattern under an unnamed marquee is now refused");
      check(!session.refusals().empty() && contains(session.refusals()[0], "define_pattern"),
            "bounded: and the refusal names it");
    }

    // The control. If the flag were simply read as false everywhere, all four
    // assertions above would pass and the guard would be gone entirely; this
    // is the one that says it is still there.
    {
      OpenDocument od = makeRecorderDocument();
      od.selection = marquee;
      session.arm(od);
      const CommandResult blurred = applyCommand(od, blurStep(2.0));
      session.stop();
      check(blurred.ok && session.steps().empty() && session.refusals().size() == 1,
            "bounded: a blur under an unnamed marquee is still refused");
    }
  }

  // Leave the session recorder stopped: it is process-wide, and a section that
  // left it armed would have every later section's `applyCommand()` appending
  // to a recording nobody is looking at.
  session.stop();
  check(!session.isRecording(), "teardown: the session recorder is not left armed");

  return ok;
}

}  // namespace np
