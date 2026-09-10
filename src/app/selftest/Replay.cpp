#include "app/selftest/Support.hpp"

#include "app/Command.hpp"
#include "app/CommandsOpStack.hpp"
#include "app/LayerEditor.hpp"
#include "app/Recorder.hpp"
#include "app/Replay.hpp"
#include "core/LayerOps.hpp"

namespace np {
namespace {

void fillLayer(TileStore& tiles, float v) {
  Tile& t = tiles.getOrCreate(TileCoord{0, 0});
  for (int32_t y = 0; y < kTileSize; ++y)
    for (int32_t x = 0; x < kTileSize; ++x)
      t.writePixel(PixelCoord{x, y}, {v, v * 0.5f, 1.0f - v, 1.0f});
}

// Adds an RGB layer named `name`, filled with `v`, and returns its index.
size_t addFilledLayer(OpenDocument& od, const char* name, float v) {
  const LayerEditResult added = applyLayerCommand(od, LayerCommand::NewRgbLayer, od.activeLayer);
  if (!added.ok) return od.activeLayer;
  Layer& layer = od.document.layers[added.selected];
  layer.name = name;
  if (layer.rgbTiles) fillLayer(*layer.rgbTiles, v);
  od.activeLayer = added.selected;
  return added.selected;
}

// An order-independent digest of one layer's RGB texels.
//
// **Order-independent on purpose.** `TileStore` iterates an unordered map, so
// a digest that folded tiles in iteration order would differ between two
// stores holding identical pictures and this whole section would assert a
// hash-table implementation detail. Each tile is hashed with its coordinate
// and the results are summed, which is commutative.
uint64_t layerDigest(const Layer& layer) {
  if (!layer.rgbTiles) return 0;
  uint64_t total = 0;
  for (const auto& [coord, tile] : *layer.rgbTiles) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
      h ^= v;
      h *= 1099511628211ull;
    };
    mix(static_cast<uint64_t>(static_cast<int64_t>(coord.x)));
    mix(static_cast<uint64_t>(static_cast<int64_t>(coord.y)));
    const uint16_t* texels = tile.data();
    const size_t count = static_cast<size_t>(kTileSize) * static_cast<size_t>(kTileSize) * 4;
    for (size_t i = 0; i < count; ++i) mix(texels[i]);
    total += h;
  }
  return total;
}

// Everything about a document a replay could get wrong: the pixels of every
// layer, in order, plus the structure and the per-layer settings a command can
// change. Used to compare a replayed document against a directly-applied one.
uint64_t documentDigest(const OpenDocument& od) {
  uint64_t h = 14695981039346656037ull;
  auto mix = [&h](uint64_t v) {
    h ^= v;
    h *= 1099511628211ull;
  };
  auto mixString = [&](const std::string& s) {
    for (char c : s) mix(static_cast<uint64_t>(static_cast<unsigned char>(c)));
    mix(0x5eu);
  };
  mix(od.document.layers.size());
  for (const Layer& layer : od.document.layers) {
    mixString(layer.name);
    mixString(layer.blend);
    mix(static_cast<uint64_t>(layer.opacity * 100000.0f));
    mix(layer.visible ? 1u : 2u);
    mix(layerDigest(layer));
    mix(layer.ops.size());
  }
  mix(od.activeLayer);
  mix(od.selection ? 1u : 2u);
  mix(od.maskIsEditTarget ? 1u : 2u);
  mixString(od.document.channels.empty() ? std::string() : od.document.channels[0].name);
  return h;
}

Command step(const char* id) {
  return Command{id, JsonValue::object()};
}

Command selectLayer(const char* name) {
  JsonValue p = JsonValue::object();
  p.set("layer", JsonValue::string(name));
  return Command{"select_layer", std::move(p)};
}

Command blur(double sigma) {
  JsonValue p = JsonValue::object();
  p.set("sigma", JsonValue::number(sigma));
  return Command{"filter_gaussian_blur", std::move(p)};
}

Command setBlend(const char* layer, const char* mode) {
  JsonValue p = JsonValue::object();
  p.set("layer", JsonValue::string(layer));
  p.set("mode", JsonValue::string(mode));
  return Command{"set_layer_blend", std::move(p)};
}

}  // namespace

bool runReplayTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, std::string_view needle) {
    return s.find(needle) != std::string::npos;
  };

  // The action every section below runs: pin a layer by name, blur it, and
  // set its blend mode. Three steps is the smallest number that exercises all
  // three of "targeting is explicit", "a pixel op runs" and "a setter runs".
  Action action;
  action.name = "Blur base";
  action.steps = {selectLayer("Base"), blur(2.0), setBlend("Base", "multiply")};

  std::printf("  -- A. the gate: recorded on A, replayed on B --\n");
  {
    // A: Base then Detail. B: Spare, Base, Detail -- a different layer count
    // and a different index for "Base", which is the whole point. If replay
    // targeted by index instead of by name, B's blur would land on "Spare".
    OpenDocument a = makeBlankOpenDocument(64, 64, WorkingSpace{}, "A");
    a.document.layers[0].name = "Base";
    fillLayer(*a.document.layers[0].rgbTiles, 0.25f);
    addFilledLayer(a, "Detail", 0.75f);

    OpenDocument b = makeBlankOpenDocument(64, 64, WorkingSpace{}, "B");
    b.document.layers[0].name = "Spare";
    fillLayer(*b.document.layers[0].rgbTiles, 0.10f);
    addFilledLayer(b, "Base", 0.25f);
    addFilledLayer(b, "Detail", 0.75f);

    const uint64_t spareBefore = layerDigest(b.document.layers[0]);

    const ReplayResult ra = replayAction(a, action);
    const ReplayResult rb = replayAction(b, action);
    check(ra.ok && rb.ok, "gate: the action runs on both documents");
    check(ra.steps.size() == 3 && rb.steps.size() == 3, "gate: three steps attempted on each");

    const size_t aBase = layerIndexNamed(a.document, "Base");
    const size_t bBase = layerIndexNamed(b.document, "Base");
    check(aBase == 0 && bBase == 1, "gate: \"Base\" really is at a different index in each");
    check(aBase < a.document.layers.size() && bBase < b.document.layers.size() &&
              layerDigest(a.document.layers[aBase]) == layerDigest(b.document.layers[bBase]),
          "gate: B's \"Base\" holds exactly the pixels A's does");
    check(layerDigest(b.document.layers[0]) == spareBefore,
          "gate: and B's extra layer was not touched");
    check(bBase < b.document.layers.size() &&
              b.document.layers[bBase].blend == blendModeName(BlendMode::Multiply),
          "gate: the setter step landed on the named layer too");
  }

  std::printf("  -- B. a missing layer refuses, having changed nothing --\n");
  {
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "no base");
    od.document.layers[0].name = "Something Else";
    fillLayer(*od.document.layers[0].rgbTiles, 0.4f);
    const uint64_t before = documentDigest(od);
    const size_t historyBefore = od.history.entries().size();
    const uint64_t revisionBefore = od.revision;

    const ReplayResult r = replayAction(od, action);
    check(!r.ok, "missing layer: the run is refused");
    check(contains(r.status, "Base"),
          "missing layer: and the refusal names the layer it wanted");
    check(contains(r.status, "step 1"), "missing layer: and which step wanted it");
    check(documentDigest(od) == before, "missing layer: the document is unchanged");
    check(od.history.entries().size() == historyBefore && od.revision == revisionBefore,
          "missing layer: and no history entry or revision was spent");
    check(r.steps.size() == 1,
          "missing layer: exactly one step was attempted, not all three");
  }

  std::printf("  -- C. the whole replay is one history entry --\n");
  {
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "history");
    od.document.layers[0].name = "Base";
    fillLayer(*od.document.layers[0].rgbTiles, 0.25f);
    const size_t before = od.history.entries().size();

    const ReplayResult r = replayAction(od, action);
    check(r.ok, "history: the three-step action applied");
    check(od.history.entries().size() == before + 1,
          "history: three steps left exactly one entry");
    check(!od.history.entries().empty() && od.history.entries().back().label == "Blur base",
          "history: named for the action, not for its last step");
    check(od.unsavedEdits.size() == 1 && od.unsavedEdits.back() == "Blur base",
          "history: and \"what is unsaved\" names the action once");

    // The point of one entry: one undo takes the whole action back.
    const Document* undone = od.history.undo();
    check(undone != nullptr && layerDigest(undone->layers[0]) != layerDigest(od.document.layers[0]),
          "history: a single undo reaches the pre-action document");
  }

  std::printf("  -- D. the commit carries everything a command changed --\n");
  {
    // The assertion that catches a member `commitReplay()` forgot. Rather than
    // reading its list -- which would only ever agree with itself -- run the
    // same steps two ways and compare. A member left behind shows up here
    // whatever it is called, including one added to `OpenDocument` next year.
    auto fixture = [] {
      OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "commit");
      od.document.layers[0].name = "Base";
      fillLayer(*od.document.layers[0].rgbTiles, 0.25f);
      addFilledLayer(od, "Detail", 0.75f);
      od.activeLayer = 1;
      return od;
    };

    // A longer script than section A's, reaching more of the members: it
    // changes the selection, moves the active layer structurally, and edits a
    // layer setting.
    Action wide;
    wide.name = "Wide";
    wide.steps = {step("select_all"),  selectLayer("Base"), blur(1.5),
                  step("deselect"),    step("flatten_image")};

    OpenDocument direct = fixture();
    bool directOk = true;
    for (const Command& c : wide.steps) directOk = applyCommand(direct, c).ok && directOk;

    OpenDocument replayed = fixture();
    const ReplayResult r = replayAction(replayed, wide);

    check(directOk, "commit: the script runs step-by-step through applyCommand()");
    check(r.ok, "commit: and the same script replays");
    check(documentDigest(direct) == documentDigest(replayed),
          "commit: the replayed document equals the directly-applied one");
    check(direct.activeLayer == replayed.activeLayer,
          "commit: including the active layer a structural step moved");
    check(direct.selection.has_value() == replayed.selection.has_value(),
          "commit: and the selection the script left behind");
  }

  std::printf("  -- E. a step that changed nothing is a warning, not a pass --\n");
  {
    // A blur with nothing to blur: an empty layer. It succeeds -- there is no
    // reason to refuse it -- and reports zero texels, which in a thirty-file
    // batch is thirty files written unmodified and called successes.
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "empty");
    od.document.layers[0].name = "Base";

    Action justBlur;
    justBlur.name = "Just blur";
    justBlur.steps = {selectLayer("Base"), blur(2.0)};

    const ReplayResult r = replayAction(od, justBlur);
    check(r.ok, "zero texels: the run still completes");
    bool warned = false;
    for (const std::string& w : r.warnings)
      if (contains(w, "changed no pixels") && contains(w, "filter_gaussian_blur")) warned = true;
    check(warned, "zero texels: and it is reported, naming the step");
    check(r.steps.size() == 2 && r.steps[1].texelsChanged == 0,
          "zero texels: the per-step report carries the count");
    check(contains(r.status, "warning"), "zero texels: and the summary says so");
  }

  std::printf("  -- F. an op this build cannot evaluate refuses the run --\n");
  {
    // docs/automation-plan.md §7: a document may CARRY an op it cannot
    // evaluate, because a document is read and written. An action is executed,
    // so a step it cannot evaluate is refused -- running it with the step
    // skipped writes a file that looks correct and is graded with one op
    // missing.
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "future op");
    od.document.layers[0].name = "Base";
    fillLayer(*od.document.layers[0].rgbTiles, 0.25f);
    const uint64_t before = documentDigest(od);

    JsonValue futureOp = JsonValue::object();
    futureOp.set("kind", JsonValue::string("chromatic_aberration_from_2027"));
    JsonValue params = JsonValue::object();
    params.set("layer", JsonValue::string("Base"));
    params.set("op", std::move(futureOp));

    Action fromTheFuture;
    fromTheFuture.name = "Newer build";
    fromTheFuture.steps = {selectLayer("Base"), Command{"add_layer_op", std::move(params)}};

    const ReplayResult r = replayAction(od, fromTheFuture);
    check(!r.ok, "future op: the run is refused");
    check(contains(r.status, "chromatic_aberration_from_2027"),
          "future op: and the refusal names the kind it does not know");
    check(r.steps.empty(),
          "future op: refused in pre-flight -- not one step was attempted");
    check(documentDigest(od) == before, "future op: the document is unchanged");
  }

  std::printf("  -- G. an unknown command id refuses before anything runs --\n");
  {
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "unknown id");
    od.document.layers[0].name = "Base";
    fillLayer(*od.document.layers[0].rgbTiles, 0.25f);
    const uint64_t before = documentDigest(od);

    Action bad;
    bad.name = "Unknown";
    // The legal step comes FIRST, so a pre-flight that ran as it went would
    // have applied it before meeting the bad one. `steps.empty()` below is
    // what distinguishes the two.
    bad.steps = {selectLayer("Base"), blur(2.0), step("polish_the_brass")};

    const ReplayResult r = replayAction(od, bad);
    check(!r.ok, "unknown id: the run is refused");
    check(contains(r.status, "polish_the_brass"), "unknown id: named");
    check(contains(r.status, "step 3"), "unknown id: and its position given");
    check(r.steps.empty(), "unknown id: the two legal steps in front of it did not run");
    check(documentDigest(od) == before, "unknown id: the document is unchanged");
  }

  std::printf("  -- H. a live recording does not record a replay --\n");
  {
    Recorder& session = sessionRecorder();
    OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "recording");
    od.document.layers[0].name = "Base";
    fillLayer(*od.document.layers[0].rgbTiles, 0.25f);

    session.arm(od);
    const size_t armed = session.steps().size();
    const ReplayResult r = replayAction(od, action);
    check(r.ok, "interlock: the replay ran");
    check(session.steps().size() == armed,
          "interlock: and the recording gained none of its three steps");
    check(session.isRecording(),
          "interlock: the recording is still live -- suspended, not stopped");

    // And the suspension really did end: the next command is recorded.
    const CommandResult after = applyCommand(od, setBlend("Base", "screen"));
    check(after.ok && session.steps().size() == armed + 1,
          "interlock: a command after the replay is recorded again");

    session.stop();
    check(!session.isRecording(), "teardown: the session recorder is not left armed");
  }

  return ok;
}

}  // namespace np
