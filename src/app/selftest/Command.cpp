#include "app/selftest/Support.hpp"

#include "app/Command.hpp"
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

  std::printf("  -- E. the precondition runs before the applier --\n");
  {
    OpenDocument od = makeCommandDocument();
    recordLayerEdit(od, setLayerLocked(od.document, od.activeLayer, true));
    const size_t entriesBefore = od.history.entries().size();
    JsonValue blurParams = JsonValue::object();
    blurParams.set("sigma", JsonValue::number(4.0));
    const CommandResult r = applyCommand(od, Command{"filter_gaussian_blur", blurParams});
    check(!r.ok, "locked layer: the blur is refused");
    check(r.texelsChanged == 0 && od.history.entries().size() == entriesBefore,
          "locked layer: zero texels, and nothing recorded -- the engine was never reached");
    check(contains(r.status, "Detail"), "locked layer: the refusal names the layer");
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

  return ok;
}

}  // namespace np
