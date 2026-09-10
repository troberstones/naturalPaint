#include "app/selftest/Support.hpp"

#include "app/Command.hpp"
#include "app/CommandCoverage.hpp"
// Section H compares each row's precondition against `pixelOpUnavailable`
// itself. It is an `inline` function, so its address is the same in every
// translation unit -- which is what makes "this row goes through the
// selection-aware pixel bridge" a thing a test can ask rather than a list a
// human keeps.
#include "app/CommandSupport.hpp"
#include "app/LayerEditor.hpp"
#include "core/LayerOps.hpp"
#include "ops/Transform.hpp"

namespace np {
namespace {

// A two-layer RGB document with content on both, built here rather than
// borrowed: this section is about the command layer, and a fixture that came
// from one of the appliers it drives would share that applier's assumptions.
OpenDocument makeCommandDocument() {
  OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "commands");
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
  od.recordEdit("command fixture", EditKind::Content);
  return od;
}

}  // namespace

// app/Command (docs/automation-plan.md step 1) -- the one door every
// recordable document edit goes through.
//
// **What this section is for.** The appliers it dispatches to are each already
// asserted by their own sections; nothing here re-tests a blur. What is new,
// and what nothing else can see, is the layer between them:
//
//  * that a command's identity is a *string* which resolves to exactly one
//    row, so a `.npaction` file cannot be moved under by an enum gaining a
//    value;
//  * that an id this build does not know is **refused**, not skipped -- the
//    difference between a batch that stops and a batch that writes thirty
//    files that look right;
//  * that the precondition runs BEFORE the applier, so a step the UI would
//    have greyed out becomes a named refusal rather than a silent no-op; and
//  * that a layer is addressed by NAME, so replaying an action on a document
//    whose layers are in a different order cannot act on a different layer.
//
// The composite case in section D is the one from the plan's own goal --
// flatten, blur, set blend to Subtract, threshold -- driven entirely through
// `applyCommand()`, which is the proof that the door is wide enough to carry a
// real action and not only a single-parameter one. Headless, GPU-free and
// filesystem-free.
bool runCommandTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  std::printf("  -- A. the table --\n");
  {
    const std::vector<CommandSpec>& all = allCommands();
    check(!all.empty(), "table: there is at least one registered command");

    bool idsWellFormed = true;
    bool labelsPresent = true;
    bool paramsWellFormed = true;
    std::vector<std::string> seen;
    for (const CommandSpec& spec : all) {
      const std::string id = spec.id;
      if (id.empty()) idsWellFormed = false;
      for (char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) idsWellFormed = false;
      if (std::find(seen.begin(), seen.end(), id) != seen.end()) idsWellFormed = false;
      seen.push_back(id);
      if (spec.label == nullptr || spec.label[0] == '\0') labelsPresent = false;
      for (size_t i = 0; i < spec.paramNames.size(); ++i) {
        if (spec.paramNames[i].empty()) paramsWellFormed = false;
        for (size_t j = i + 1; j < spec.paramNames.size(); ++j)
          if (spec.paramNames[i] == spec.paramNames[j]) paramsWellFormed = false;
      }
      if (spec.apply == nullptr || spec.unavailableReason == nullptr) paramsWellFormed = false;
    }
    check(idsWellFormed, "table: every id is unique and lower_snake_case");
    check(labelsPresent, "table: every row carries panel text");
    check(paramsWellFormed, "table: parameter names are unique and non-empty; both hooks are set");

    bool findable = true;
    for (const CommandSpec& spec : all)
      if (findCommand(spec.id) != &spec) findable = false;
    check(findable, "table: findCommand() returns the row itself for every id");
    check(findCommand("no_such_command") == nullptr, "table: an unknown id resolves to nothing");
  }

  std::printf("  -- B. an unknown id is refused, not skipped --\n");
  {
    OpenDocument od = makeCommandDocument();
    const size_t entriesBefore = od.history.entries().size();
    const CommandResult r = applyCommand(od, Command{"filter_kaleidoscope", JsonValue::object()});
    check(!r.ok, "unknown id: refused");
    check(contains(r.status, "filter_kaleidoscope"), "unknown id: the refusal names the id");
    check(!r.status.empty() && od.history.entries().size() == entriesBefore,
          "unknown id: nothing was recorded, and the sentence is never empty");
  }

  std::printf("  -- C. targeting is by name --\n");
  {
    OpenDocument od = makeCommandDocument();
    JsonValue p = JsonValue::object();
    p.set("layer", JsonValue::string("Base"));
    const CommandResult r = applyCommand(od, Command{"select_layer", p});
    check(r.ok && od.activeLayer == 0, "select_layer: a name resolves to its index");

    JsonValue missing = JsonValue::object();
    missing.set("layer", JsonValue::string("Nonexistent"));
    const size_t activeBefore = od.activeLayer;
    const CommandResult bad = applyCommand(od, Command{"select_layer", missing});
    check(!bad.ok && contains(bad.status, "Nonexistent"),
          "select_layer: a name no layer has refuses, naming it");
    check(od.activeLayer == activeBefore, "select_layer: a refusal moves nothing");

    // The property the whole by-name rule exists for: the same action on a
    // document whose layers are in the other order still finds "Base".
    OpenDocument reordered = makeCommandDocument();
    std::swap(reordered.document.layers[0], reordered.document.layers[1]);
    const CommandResult again = applyCommand(reordered, Command{"select_layer", p});
    check(again.ok && reordered.activeLayer == 1,
          "select_layer: the same step finds \"Base\" at a different index");
  }

  std::printf("  -- D. the plan's own composite case --\n");
  {
    // flatten image -> blur it -> set its blend mode to Subtract -> threshold.
    // Every step through applyCommand(), with no applier called directly.
    OpenDocument od = makeCommandDocument();
    check(od.document.layers.size() == 2, "composite: the fixture starts with two layers");

    const CommandResult flat = applyCommand(od, Command{"flatten_image", JsonValue::object()});
    check(flat.ok && od.document.layers.size() == 1, "composite: flatten leaves one layer");
    // The step that found the defect: before `fromLayerEdit()` adopted
    // `LayerEditResult::selected`, `activeLayer` stayed at 1 over a stack that
    // now had one row, and everything after this in the action addressed a
    // layer that was not there.
    check(activeLayerIndex(od) == std::optional<size_t>(0),
          "composite: flatten moved the active layer to the survivor");

    JsonValue blurParams = JsonValue::object();
    blurParams.set("sigma", JsonValue::number(2.0));
    const CommandResult blurred = applyCommand(od, Command{"filter_gaussian_blur", blurParams});
    check(blurred.ok && blurred.texelsChanged > 0 && blurred.changesPixels,
          "composite: the blur ran and reports the texels it moved");

    JsonValue blendParams = JsonValue::object();
    blendParams.set("mode", JsonValue::string("subtract"));
    const CommandResult blended = applyCommand(od, Command{"set_layer_blend", blendParams});
    // `Layer::blend` is the wire NAME (core/Blend.hpp: nothing stores the
    // enum), so this asserts the stored string -- and the command still goes
    // through `blendModeFromName()`, which is what makes a mode nothing knows
    // a refusal rather than a value stored and silently ignored later.
    const Layer* blendTarget = activeLayerOf(od);
    check(blended.ok && blendTarget != nullptr &&
              blendTarget->blend == blendModeName(BlendMode::Subtract),
          "composite: the blend mode is Subtract, resolved from its wire name");

    JsonValue thresholdParams = JsonValue::object();
    thresholdParams.set("threshold", JsonValue::number(0.5));
    thresholdParams.set("amount", JsonValue::number(1.0));
    const CommandResult thresholded = applyCommand(od, Command{"adjust_threshold", thresholdParams});
    check(thresholded.ok && thresholded.texelsChanged > 0, "composite: the threshold ran");

    check(!flat.status.empty() && !blurred.status.empty() && !blended.status.empty() &&
              !thresholded.status.empty(),
          "composite: every step said what it did");
  }

  std::printf("  -- E. the precondition is a hook a replayer can ask --\n");
  {
    // **Deliberately NOT phrased as "the precondition runs first".** The
    // appliers refuse a locked layer themselves -- `applyPixelFilter()` calls
    // `pixelOpRefusalFor()` before it touches the engine -- so an assertion
    // that only drove `applyCommand()` would pass with the precondition hook
    // deleted entirely, and would be measuring the applier while claiming to
    // measure the table. It was written that way first and a sabotage caught
    // it. What the hook uniquely buys is that a REPLAYER can ask before
    // applying, so that is what is asserted here: the hook itself, called
    // directly, on a document where the answer differs.
    OpenDocument od = makeCommandDocument();
    const CommandSpec* blur = findCommand("filter_gaussian_blur");
    check(blur != nullptr && blur->unavailableReason(od, JsonValue::object()).empty(),
          "precondition: a writable RGB layer reports the blur available");

    recordLayerEdit(od, setLayerLocked(od.document, od.activeLayer, true));
    const std::string why = blur == nullptr ? std::string()
                                            : blur->unavailableReason(od, JsonValue::object());
    check(!why.empty() && contains(why, "Detail"),
          "precondition: a locked layer reports it unavailable, naming the layer");

    const size_t entriesBefore = od.history.entries().size();
    JsonValue blurParams = JsonValue::object();
    blurParams.set("sigma", JsonValue::number(4.0));
    const CommandResult r = applyCommand(od, Command{"filter_gaussian_blur", blurParams});
    check(!r.ok && r.texelsChanged == 0 && od.history.entries().size() == entriesBefore,
          "locked layer: refused, zero texels, nothing recorded");

    // The precondition also answers for a document that has no layer at all,
    // which no applier can be asked about without one.
    OpenDocument empty = makeCommandDocument();
    empty.document.layers.clear();
    const CommandSpec* flatten = findCommand("flatten_image");
    const CommandSpec* create = findCommand("new_rgb_layer");
    check(flatten != nullptr && !flatten->unavailableReason(empty, JsonValue::object()).empty(),
          "precondition: an empty stack reports flatten unavailable");
    check(create != nullptr && create->unavailableReason(empty, JsonValue::object()).empty(),
          "precondition: an empty stack can still take a layer-creating command");
  }

  std::printf("  -- E2. a structural command moves the selection --\n");
  {
    // The property `fromLayerEdit()` exists for, probed with a CREATE rather
    // than a flatten. A flatten is the weak case: `activeLayerIndex()` clamps
    // into the stack, so a stale index after one still reads as the survivor
    // and an assertion built on it passes with the adoption deleted -- a
    // sabotage caught that too. A create is the strong case: the new layer's
    // index is not a clamp of anything, so failing to adopt it leaves every
    // later step painting on the layer the user had selected BEFORE.
    OpenDocument od = makeCommandDocument();
    const std::string before = activeLayerOf(od) != nullptr ? activeLayerOf(od)->name : "";
    const CommandResult made = applyCommand(od, Command{"new_rgb_layer", JsonValue::object()});
    const Layer* now = activeLayerOf(od);
    check(made.ok && od.document.layers.size() == 3, "new layer: the stack grew");
    check(now != nullptr && now->name != before,
          "new layer: the active layer is the NEW one, not the one selected before");
    check(now != nullptr && layerIndexNamed(od.document, now->name) == od.activeLayer,
          "new layer: the index and the name agree about which layer is active");
  }

  std::printf("  -- F. parameters are validated, not assumed --\n");
  {
    OpenDocument od = makeCommandDocument();
    const CommandResult noSigma =
        applyCommand(od, Command{"filter_gaussian_blur", JsonValue::object()});
    check(!noSigma.ok && contains(noSigma.status, "sigma"),
          "params: a missing sigma refuses, and says which key");

    JsonValue zero = JsonValue::object();
    zero.set("sigma", JsonValue::number(0.0));
    const CommandResult zeroSigma = applyCommand(od, Command{"filter_gaussian_blur", zero});
    check(!zeroSigma.ok, "params: a sigma of zero refuses rather than running a no-op filter");

    JsonValue size = JsonValue::object();
    size.set("width", JsonValue::number(32));
    size.set("height", JsonValue::number(32));
    size.set("kernel", JsonValue::string("Catmull-Rom"));
    const CommandResult resized = applyCommand(od, Command{"image_size", size});
    check(resized.ok && od.document.width == 32 && od.document.height == 32,
          "params: image_size resizes, with the kernel named in the file");

    JsonValue badKernel = size;
    badKernel.set("kernel", JsonValue::string("bicubic-ish"));
    const CommandResult refusedKernel = applyCommand(od, Command{"image_size", badKernel});
    check(!refusedKernel.ok && contains(refusedKernel.status, "bicubic-ish"),
          "params: a kernel name nothing knows refuses, naming it");

    JsonValue fractional = JsonValue::object();
    fractional.set("width", JsonValue::number(511.5));
    fractional.set("height", JsonValue::number(512));
    const CommandResult refusedSize = applyCommand(od, Command{"image_size", fractional});
    check(!refusedSize.ok, "params: a fractional pixel count refuses");

    // The pair this file format depends on: every kernel's name reads back as
    // the kernel it names, case-insensitively.
    bool kernelsRoundTrip = true;
    for (const ResampleKernel k : {ResampleKernel::Nearest, ResampleKernel::Bilinear,
                                   ResampleKernel::CatmullRom, ResampleKernel::Mitchell,
                                   ResampleKernel::Lanczos3}) {
      const std::optional<ResampleKernel> back = resampleKernelFromName(resampleKernelName(k));
      if (!back || *back != k) kernelsRoundTrip = false;
    }
    check(kernelsRoundTrip && resampleKernelFromName("catmull-rom") == ResampleKernel::CatmullRom,
          "params: every resample kernel name round-trips, case-insensitively");
  }

  std::printf("  -- G. exhaustiveness, across every vocabulary at once --\n");
  {
    // **The gate no single track could write.** Six branches filled this table
    // in parallel and each could see only its own family; this is the one
    // assertion that looks at all of them together, and at what is NOT in any
    // of them. app/CommandCoverage.hpp argues why the classification is an
    // exhaustive `switch` (so a new menu action fails the BUILD, not merely a
    // test) and why it has three answers rather than a tidy two.
    size_t registered = 0, notRecordable = 0, notYet = 0;
    bool everyIdResolves = true;
    bool everyExclusionGivesAReason = true;
    std::vector<std::string> gaps;

    for (int i = 0; i <= static_cast<int>(MenuAction::Count); ++i) {
      const auto action = static_cast<MenuAction>(i);
      const CommandCoverage c = coverageFor(action);
      switch (c.kind) {
        case CommandCoverageKind::Registered:
          ++registered;
          // A null id is one of the two family actions, whose rows are walked
          // exhaustively by app/selftest/CommandsLayers.cpp instead.
          if (c.commandId != nullptr && findCommand(c.commandId) == nullptr) {
            everyIdResolves = false;
            std::printf("      unregistered id claimed by coverage: %s\n", c.commandId);
          }
          break;
        case CommandCoverageKind::NotRecordable:
          ++notRecordable;
          if (c.reason == nullptr || c.reason[0] == '\0') everyExclusionGivesAReason = false;
          break;
        case CommandCoverageKind::NotYetRegistered:
          ++notYet;
          if (c.reason == nullptr || c.reason[0] == '\0') everyExclusionGivesAReason = false;
          gaps.push_back(std::to_string(i));
          break;
      }
    }

    check(everyIdResolves,
          "coverage: every id the classification claims is registered really is");
    check(everyExclusionGivesAReason,
          "coverage: every action left out of the table carries the reason it is out");
    check(registered >= 40, "coverage: the registered set is the size the table says it is");

    // **The number is the review.** This list may shrink freely; it cannot grow
    // without someone editing the literal below, which is the only moment a new
    // gap gets looked at by a human. Folding these into "not recordable" would
    // have given each of them a fake justification and made the table look
    // complete -- see app/CommandCoverage.hpp §2.
    std::printf("      %zu document edits are classified as not-yet-registered\n", notYet);
    // Was eight. Six closed at once: PRD E4/E8/E9's five refines were
    // registered (app/CommandsOpStack.cpp §4), and `SelectUndoRefine` --
    // listed beside them as a sixth gap -- turned out on reading the code not
    // to be one at all. It pops `OpenDocument::refineUndoStack`, which that
    // member's own comment is explicit is per-session state outside both
    // core::History and the file, so it is NotRecordable for the reason Undo
    // and Redo are. The two left are `NumericTransform` and `DeleteSelection`,
    // each of which states what it is waiting for.
    check(notYet == 2,
          "coverage: exactly the two known gaps, and no new one has appeared");
    check(notRecordable > registered,
          "coverage: most menu actions are session state, which is the rule doing its job");
  }

  std::printf("  -- H. selectionBounded, over the whole table --\n");
  {
    // **What this section is for.** app/Recorder §4 refuses to record a step
    // taken under a live marquee that no saved channel matches, and it used to
    // decide which steps to police from `CommandResult::changesPixels`. That
    // proxy was wrong in both directions (app/Command.hpp on the field). The
    // flag replacing it is a per-row claim, and a per-row claim that nobody
    // checks is the shape that goes stale the first time somebody registers a
    // filter -- so it is checked structurally, not spot-checked.
    const std::vector<CommandSpec>& all = allCommands();

    // The structural half. Every command that reaches `applyPixelFilter()` --
    // the bridge that composites its result THROUGH `doc.selection` -- carries
    // `pixelOpUnavailable` as its precondition, because that is the shared
    // precondition of exactly that set. So: bounded and pixel-op are the same
    // set, up to a named exception list. A filter registered next month with
    // the same precondition and no flag fails here by name.
    std::vector<std::string> pixelOpsMissingTheFlag;
    std::vector<std::string> boundedWithoutTheBridge;
    for (const CommandSpec& spec : all) {
      const bool bridged = spec.unavailableReason == &pixelOpUnavailable;
      if (bridged && !spec.selectionBounded) pixelOpsMissingTheFlag.push_back(spec.id);
      if (spec.selectionBounded && !bridged) boundedWithoutTheBridge.push_back(spec.id);
    }
    for (const std::string& id : pixelOpsMissingTheFlag)
      std::printf("      a pixel-op row with no selectionBounded flag: %s\n", id.c_str());
    check(pixelOpsMissingTheFlag.empty(),
          "bounded: every row that composites through the selection says so");

    // **The number is the review**, the discipline section G uses for the
    // coverage gaps. `crop_to_selection` is bounded and does not go through
    // the pixel bridge -- the region it crops to IS the selection. It is the
    // only such row, and a second one has to be added to this literal by a
    // human who has thought about it.
    for (const std::string& id : boundedWithoutTheBridge)
      std::printf("      bounded outside the pixel bridge: %s\n", id.c_str());
    check(boundedWithoutTheBridge.size() == 1 && boundedWithoutTheBridge[0] == "crop_to_selection",
          "bounded: exactly one row is bounded outside the pixel bridge, and it is the crop");

    // The by-name half, in both directions, because a structural rule that
    // happened to be vacuous -- no row bounded at all -- would pass everything
    // above.
    auto boundedById = [&](const char* id) {
      const CommandSpec* spec = findCommand(id);
      return spec != nullptr && spec->selectionBounded;
    };
    check(boundedById("filter_gaussian_blur") && boundedById("adjust_levels") &&
              boundedById("adjust_invert") && boundedById("fill_with_pattern") &&
              boundedById("crop_to_selection"),
          "bounded: the destructive ops that act through the selection are bounded");

    // The rows the field was added for. Each acts on the whole document; none
    // is restricted by the selection. `image_size`, `canvas_size` and
    // `trim_to_content` were refused at record time under a live marquee for a
    // reason that does not apply to them; `flatten_image` belongs with them by
    // meaning and escaped only because `fromLayerEdit()` never set
    // `changesPixels` (app/selftest/Recorder.cpp section G says so where it is
    // asserted).
    check(!boundedById("flatten_image") && !boundedById("image_size") &&
              !boundedById("canvas_size") && !boundedById("trim_to_content"),
          "bounded: the whole-document ops are NOT bounded by the selection");

    // The commands that operate ON the selection are not bounded BY it. The
    // last of these is load-bearing beyond tidiness: a bounded
    // `save_selection_as_channel` would be refused at record time under the
    // very marquee it exists to name, which is the fix the refusal tells the
    // user to apply -- a closed loop.
    check(!boundedById("select_all") && !boundedById("deselect") &&
              !boundedById("invert_selection") && !boundedById("select_grow") &&
              !boundedById("save_selection_as_channel"),
          "bounded: a command that operates on the selection is not bounded by it");

    // The one row where `selectionBounded` and `changesPixels` genuinely
    // disagree, asserted by RUNNING it rather than by reading the table --
    // `changesPixels` is a result field and the table cannot see it. This is
    // what the old proxy let through: a step whose meaning is the marquee and
    // whose texel count is zero.
    {
      OpenDocument od = makeCommandDocument();
      JsonValue p = JsonValue::object();
      p.set("name", JsonValue::string("Swatch"));
      const CommandResult defined = applyCommand(od, Command{"define_pattern", p});
      check(defined.ok && !defined.changesPixels && boundedById("define_pattern"),
            "bounded: define_pattern changes no pixel and is bounded anyway");
    }
  }

  return ok;
}

}  // namespace np
