#include "app/selftest/Support.hpp"

#include "app/Command.hpp"
#include "app/CommandsLayers.hpp"
#include "app/LayerEditor.hpp"
#include "core/LayerOps.hpp"
#include "core/LayerSetOps.hpp"

namespace np {
namespace {

// Three named RGB layers, bottom to top: "Base", "Mid", "Detail", with
// "Detail" active. Built through `applyLayerCommand()` rather than by pushing
// into `doc.layers`, because the insertion index and the selection it leaves
// behind are part of what this section is about -- a fixture that assembled
// the stack by hand would be asserting against a stack no gesture can produce.
OpenDocument makeLayerCommandDocument() {
  OpenDocument od = makeBlankOpenDocument(32, 32, WorkingSpace{}, "layer commands");
  od.document.layers[0].name = "Base";
  for (const char* name : {"Mid", "Detail"}) {
    const LayerEditResult e = applyLayerCommand(od, LayerCommand::NewRgbLayer, od.activeLayer);
    if (e.ok && e.selected < od.document.layers.size()) {
      od.document.layers[e.selected].name = name;
      od.activeLayer = e.selected;
    }
  }
  return od;
}

// Everything a refusal must leave untouched, in one comparable value. The
// names carry the structure (count and order), the flags carry the properties
// a set command would have written, and the revision carries whether anything
// was recorded at all -- `recordLayerEdit()` bumps it and appends a history
// entry, so a refusal that changed nothing visible but still recorded an edit
// is caught here rather than being invisible until an undo produced a step
// that undid nothing.
struct StackShape {
  std::vector<std::string> names;
  std::vector<int> flags;
  uint64_t revision = 0;
  size_t historyEntries = 0;

  friend bool operator==(const StackShape&, const StackShape&) = default;
};

StackShape shapeOf(const OpenDocument& doc) {
  StackShape s;
  for (const Layer& layer : doc.document.layers) {
    s.names.push_back(layer.name);
    s.flags.push_back((layer.visible ? 1 : 0) | (layer.locked ? 2 : 0) | (layer.clipped ? 4 : 0) |
                      (layer.alphaLocked ? 8 : 0));
  }
  s.revision = doc.revision;
  s.historyEntries = doc.history.entries().size();
  return s;
}

JsonValue layerParam(const char* name) {
  JsonValue p = JsonValue::object();
  p.set("layer", JsonValue::string(name));
  return p;
}

JsonValue layersParam(std::initializer_list<const char*> names) {
  JsonValue list = JsonValue::array();
  for (const char* name : names) list.push(JsonValue::string(name));
  JsonValue p = JsonValue::object();
  p.set("layers", list);
  return p;
}

}  // namespace

// app/CommandsLayers (docs/automation-plan.md step 1) -- the three layer
// vocabularies as recordable commands.
//
// **What this section is for, and what it deliberately does not re-test.**
// app/selftest/LayerEditor.cpp already asserts what each gesture does to a
// stack, and app/selftest/Command.cpp already asserts the door itself -- the
// string identity, the unknown-id refusal, the precondition hook. Nothing here
// repeats either. What is new is the registration between them, and it has
// exactly four claims worth making:
//
//  * **Exhaustiveness.** Every enumerator of `LayerCommand` and of
//    `LayerSetCommand` has a row. This is the assertion that makes a future
//    enumerator FAIL rather than be silently unrecordable, which is the plan's
//    own step-1 gate -- and it is checked by walking `allLayerCommands()` and
//    `allLayerSetCommands()` and asking the registration which id each landed
//    under, never against a list kept here (app/CommandsLayers.hpp says why a
//    second list would pass for the wrong reason).
//  * **A set is all-or-nothing at RESOLUTION time.** core/LayerSetOps already
//    refuses to apply a set operation to some of its members; a step naming
//    five layers on a document that has four must be refused the same way,
//    with nothing touched. "Skip the ones that resolved" would silently be a
//    different edit from the one recorded.
//  * **A create adopts the layer it created**, asserted BY NAME. An index
//    assertion passes even with the adoption deleted, because
//    `activeLayerIndex()` clamps -- app/selftest/Command.cpp's own E2 section
//    records the sabotage that taught this.
//  * **The lock is the model's, not the adapter's.** The setters that a locked
//    layer refuses refuse, and -- more easily broken and less often checked --
//    the three that a locked layer deliberately ALLOWS still go through. An
//    adapter that added a blanket lock check would look careful and would
//    break the label, the link and the eye icon on every locked layer.
//
// Headless, GPU-free and filesystem-free.
bool runCommandsLayersTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

  std::printf("  -- A. every layer gesture has a row --\n");
  {
    // The gate: walk the vocabulary itself. A `LayerCommand` added tomorrow
    // and not registered has no id, and this goes red naming the label -- the
    // discipline `allLayerCommands()` already established for the two menus,
    // one level up.
    std::vector<std::string> missingId;
    std::vector<std::string> missingRow;
    for (const LayerCommand command : allLayerCommands()) {
      const char* id = layerCommandId(command);
      if (id == nullptr || id[0] == '\0') {
        missingId.push_back(layerCommandLabel(command));
        continue;
      }
      if (findCommand(id) == nullptr) missingRow.push_back(id);
    }
    for (const std::string& label : missingId)
      std::printf("     unregistered LayerCommand: %s\n", label.c_str());
    for (const std::string& id : missingRow)
      std::printf("     id with no row in allCommands(): %s\n", id.c_str());
    check(missingId.empty(), "exhaustive: every LayerCommand enumerator has a command id");
    check(missingRow.empty(), "exhaustive: every LayerCommand id resolves to a registered row");

    std::vector<std::string> missingSetId;
    std::vector<std::string> missingSetRow;
    for (const LayerSetCommand command : allLayerSetCommands()) {
      const char* id = layerSetCommandId(command);
      if (id == nullptr || id[0] == '\0') {
        missingSetId.push_back(layerSetCommandLabel(command));
        continue;
      }
      if (findCommand(id) == nullptr) missingSetRow.push_back(id);
    }
    for (const std::string& label : missingSetId)
      std::printf("     unregistered LayerSetCommand: %s\n", label.c_str());
    for (const std::string& id : missingSetRow)
      std::printf("     id with no row in allCommands(): %s\n", id.c_str());
    check(missingSetId.empty(), "exhaustive: every LayerSetCommand enumerator has a command id");
    check(missingSetRow.empty(), "exhaustive: every LayerSetCommand id resolves to a row");

    // The value setters have no walked table to check against -- they are
    // free functions in core/LayerOps -- so this is the honest weaker claim:
    // the ids this build registered are still there. It catches a deletion,
    // not an addition, and says so rather than pretending otherwise.
    bool settersPresent = true;
    for (const char* id :
         {"select_layer", "set_layer_blend", "set_layer_opacity", "set_layer_visible",
          "set_layer_locked", "set_layer_alpha_locked", "set_layer_clipped",
          "set_layer_flats_reference", "set_layer_name", "set_layer_color_label",
          "set_layer_link_group"}) {
      if (findCommand(id) == nullptr) {
        std::printf("     missing value-setter row: %s\n", id);
        settersPresent = false;
      }
    }
    check(settersPresent, "setters: every core/LayerOps value setter has a row");

    // A set command's params advertise a LIST, and a single-layer command's a
    // single name. The panel and the replayer both read `paramNames`, so a row
    // that advertised `"layer"` for a set command would send a recorder to
    // write a step no resolver here accepts.
    bool setParamsAreLists = true;
    for (const LayerSetCommand command : allLayerSetCommands()) {
      const char* id = layerSetCommandId(command);
      const CommandSpec* spec = id == nullptr ? nullptr : findCommand(id);
      if (spec == nullptr) continue;
      if (spec->paramNames.empty() || spec->paramNames[0] != "layers") setParamsAreLists = false;
    }
    check(setParamsAreLists, "setters: every set row advertises its \"layers\" list");
  }

  std::printf("  -- B. a set command names its layers, and one bad name refuses --\n");
  {
    // First the positive half, so the refusal below cannot pass merely because
    // the command does nothing on any document.
    OpenDocument od = makeLayerCommandDocument();
    const CommandResult hidden =
        applyCommand(od, Command{"hide_layers", layersParam({"Base", "Mid"})});
    check(hidden.ok, "set: hide_layers over two named layers goes ahead");
    check(od.document.layers.size() == 3 && !od.document.layers[0].visible &&
              !od.document.layers[1].visible && od.document.layers[2].visible,
          "set: it hid exactly the two it named, and nothing else");

    OpenDocument refuse = makeLayerCommandDocument();
    const StackShape before = shapeOf(refuse);
    const CommandResult bad =
        applyCommand(refuse, Command{"hide_layers", layersParam({"Base", "Ghost"})});
    check(!bad.ok, "set: one unresolvable name refuses the whole step");
    check(contains(bad.status, "Ghost"), "set: the refusal names the layer it could not find");
    // The claim the whole rule exists for: NOT "skip the ones that resolved".
    // "Base" resolved perfectly well and must still be visible.
    check(shapeOf(refuse) == before,
          "set: the layers that DID resolve are untouched, and nothing was recorded");

    // A missing list is refused rather than falling back to the active layer:
    // a set step whose target depended on the previous step's cursor would
    // mean something different on every document it replayed on.
    OpenDocument noList = makeLayerCommandDocument();
    const StackShape beforeNoList = shapeOf(noList);
    const CommandResult none = applyCommand(noList, Command{"hide_layers", JsonValue::object()});
    check(!none.ok && contains(none.status, "layers"),
          "set: an absent \"layers\" list refuses, naming the key");
    check(shapeOf(noList) == beforeNoList, "set: that refusal changed nothing either");

    // Two names resolving to one row. Layer names are explicitly not unique
    // (docs/document-format.md), so this is reachable from a real document and
    // not only from a typo -- and silently narrowing a five-layer step to four
    // is the same wrong answer as skipping.
    OpenDocument twins = makeLayerCommandDocument();
    twins.document.layers[1].name = "Base";
    const StackShape beforeTwins = shapeOf(twins);
    const CommandResult narrowed =
        applyCommand(twins, Command{"hide_layers", layersParam({"Base", "Base"})});
    check(!narrowed.ok, "set: two names resolving to one row refuses rather than narrowing");
    check(shapeOf(twins) == beforeTwins, "set: the narrowing refusal changed nothing");
  }

  std::printf("  -- C. a create adopts the layer it created, by name --\n");
  {
    // **By name, never by index.** `activeLayerIndex()` clamps into the stack,
    // so an index assertion here passes with the adoption deleted -- that is
    // not a hypothetical, it is the sabotage app/selftest/Command.cpp's E2
    // section records having been caught by.
    OpenDocument od = makeLayerCommandDocument();
    const std::string before = activeLayerOf(od) != nullptr ? activeLayerOf(od)->name : "";
    check(before == "Detail", "create: the fixture starts on \"Detail\"");

    const CommandResult made =
        applyCommand(od, Command{"new_pigment_layer", JsonValue::object()});
    const Layer* now = activeLayerOf(od);
    check(made.ok && od.document.layers.size() == 4, "create: new_pigment_layer added a layer");
    check(now != nullptr && now->kind == LayerKind::Pigment,
          "create: the layer it made is a Pigment layer, not an RGB one");
    check(now != nullptr && now->name != before,
          "create: the active layer is the NEW one, not the one selected before");

    // A create is offered on a document with no layers at all, which is the
    // one case `resolveTarget()` alone would refuse -- a zero-layer document
    // is representable (core/LayerOps.hpp) and a create is what it can take.
    OpenDocument empty = makeLayerCommandDocument();
    empty.document.layers.clear();
    const CommandSpec* create = findCommand("new_rgb_layer");
    const CommandSpec* merge = findCommand("merge_down");
    check(create != nullptr && create->unavailableReason(empty, JsonValue::object()).empty(),
          "create: an empty stack still reports a create available");
    check(merge != nullptr && !merge->unavailableReason(empty, JsonValue::object()).empty(),
          "create: an empty stack reports merge_down unavailable");
    const CommandResult made2 = applyCommand(empty, Command{"new_rgb_layer", JsonValue::object()});
    check(made2.ok && empty.document.layers.size() == 1,
          "create: and the create goes through on it");

    // A named target that does not exist is refused on every command, create
    // included -- the empty-stack latitude above is only for an ABSENT key.
    OpenDocument named = makeLayerCommandDocument();
    const CommandResult ghost = applyCommand(named, Command{"duplicate_layer", layerParam("Ghost")});
    check(!ghost.ok && contains(ghost.status, "Ghost"),
          "create: a \"layer\" naming nothing refuses, naming it");

    // A structural gesture that is not a create also lands the selection
    // where the gesture left it, and this one is checkable by name because a
    // duplicate makes a row whose name it chose.
    OpenDocument dup = makeLayerCommandDocument();
    const CommandResult copied = applyCommand(dup, Command{"duplicate_layer", layerParam("Base")});
    const Layer* onCopy = activeLayerOf(dup);
    check(copied.ok && onCopy != nullptr && onCopy->name == "Base copy",
          "create: a duplicate leaves the selection on the copy, by name");
  }

  std::printf("  -- D. the lock is core/LayerOps', in both directions --\n");
  {
    OpenDocument od = makeLayerCommandDocument();
    JsonValue lockIt = layerParam("Mid");
    lockIt.set("locked", JsonValue::boolean(true));
    check(applyCommand(od, Command{"set_layer_locked", lockIt}).ok,
          "lock: set_layer_locked locks the layer it names");
    check(od.document.layers[1].locked, "lock: and the layer really is locked");

    // The refusals. Each is core/LayerOps' own sentence, reached through the
    // adapter -- an adapter that pre-checked would be a second copy of a rule
    // that already exists, which is how a UI comes to refuse what the model
    // allows.
    const StackShape before = shapeOf(od);
    JsonValue opacity = layerParam("Mid");
    opacity.set("opacity", JsonValue::number(0.5));
    const CommandResult noOpacity = applyCommand(od, Command{"set_layer_opacity", opacity});
    check(!noOpacity.ok && contains(noOpacity.status, "Mid"),
          "lock: set_layer_opacity is refused on a locked layer, naming it");

    JsonValue rename = layerParam("Mid");
    rename.set("name", JsonValue::string("Renamed"));
    check(!applyCommand(od, Command{"set_layer_name", rename}).ok,
          "lock: set_layer_name is refused on a locked layer");

    JsonValue blend = layerParam("Mid");
    blend.set("mode", JsonValue::string("multiply"));
    check(!applyCommand(od, Command{"set_layer_blend", blend}).ok,
          "lock: set_layer_blend is refused on a locked layer");

    check(!applyCommand(od, Command{"delete_layer", layerParam("Mid")}).ok,
          "lock: delete_layer is refused on a locked layer");
    check(shapeOf(od) == before, "lock: not one of those four refusals recorded anything");

    // **And the three the lock deliberately does NOT freeze.** core/LayerOps
    // states each: hiding a locked layer changes nothing about the layer, a
    // lock that cannot be removed is not a lock, and labelling and linking are
    // what a user does TO a finished layer -- which is the commonest reason to
    // have locked it. These are the assertions a "careful" blanket lock check
    // in the adapter would go red on, which is exactly why they are here.
    JsonValue hide = layerParam("Mid");
    hide.set("visible", JsonValue::boolean(false));
    check(applyCommand(od, Command{"set_layer_visible", hide}).ok &&
              !od.document.layers[1].visible,
          "lock: a locked layer can still be hidden");

    JsonValue label = layerParam("Mid");
    label.set("label", JsonValue::string("red"));
    check(applyCommand(od, Command{"set_layer_color_label", label}).ok &&
              od.document.layers[1].colorLabel == "red",
          "lock: a locked layer can still be labelled");

    JsonValue unlock = layerParam("Mid");
    unlock.set("locked", JsonValue::boolean(false));
    check(applyCommand(od, Command{"set_layer_locked", unlock}).ok &&
              !od.document.layers[1].locked,
          "lock: a lock can always be taken off again");

    // The same rule at set level, where the all-or-nothing contract makes it
    // sharper: a two-layer delete with one locked member deletes neither.
    OpenDocument set = makeLayerCommandDocument();
    JsonValue lockOne = layerParam("Base");
    lockOne.set("locked", JsonValue::boolean(true));
    check(applyCommand(set, Command{"set_layer_locked", lockOne}).ok, "lock: set fixture locked");
    const StackShape beforeSet = shapeOf(set);
    const CommandResult deleted =
        applyCommand(set, Command{"delete_layers", layersParam({"Base", "Mid"})});
    check(!deleted.ok, "lock: delete_layers refuses a set containing a locked layer");
    check(shapeOf(set) == beforeSet, "lock: and the unlocked member of that set survives");
  }

  std::printf("  -- E. the value setters read their parameter, and validate it --\n");
  {
    OpenDocument od = makeLayerCommandDocument();
    // A missing required value is a refusal, not a default. `boolOr()` takes a
    // fallback, which is right for an optional key and wrong here: a
    // misspelt "visible" that silently defaulted would make a replayed action
    // do the opposite of what its file says.
    const CommandResult noValue =
        applyCommand(od, Command{"set_layer_visible", layerParam("Base")});
    check(!noValue.ok && contains(noValue.status, "visible"),
          "params: a missing \"visible\" refuses, naming the key");
    check(od.document.layers[0].visible, "params: and the layer is still visible");

    JsonValue wrongType = layerParam("Base");
    wrongType.set("visible", JsonValue::string("false"));
    check(!applyCommand(od, Command{"set_layer_visible", wrongType}).ok,
          "params: the string \"false\" is not a boolean, and is refused");

    // The bound is core/LayerOps', not clamped in the adapter: an action that
    // says 1.4 must be refused rather than quietly meaning 1.0, because
    // io/NpaintFile refuses to SAVE an out-of-range opacity (PRD I11).
    JsonValue tooMuch = layerParam("Base");
    tooMuch.set("opacity", JsonValue::number(1.4));
    const CommandResult over = applyCommand(od, Command{"set_layer_opacity", tooMuch});
    check(!over.ok, "params: an opacity above 1 is refused, not clamped");
    JsonValue half = layerParam("Base");
    half.set("opacity", JsonValue::number(0.25));
    check(applyCommand(od, Command{"set_layer_opacity", half}).ok &&
              od.document.layers[0].opacity == 0.25f,
          "params: an opacity inside the range goes through unchanged");

    // `Layer::blend` stores the wire NAME, so it would have taken this string
    // verbatim. Going through `blendModeFromName()` anyway is what makes a
    // mode nothing knows a refusal instead of a value written to the file and
    // silently ignored by the compositor forever after.
    JsonValue nonsense = layerParam("Base");
    nonsense.set("mode", JsonValue::string("hard_mix_ish"));
    const CommandResult noMode = applyCommand(od, Command{"set_layer_blend", nonsense});
    check(!noMode.ok && contains(noMode.status, "hard_mix_ish"),
          "params: a blend mode nothing knows refuses, naming it");
    check(od.document.layers[0].blend != "hard_mix_ish",
          "params: and the unknown name was not written to the layer");

    JsonValue real = layerParam("Base");
    real.set("mode", JsonValue::string("subtract"));
    check(applyCommand(od, Command{"set_layer_blend", real}).ok &&
              od.document.layers[0].blend == blendModeName(BlendMode::Subtract),
          "params: a known mode is stored as its wire name");

    // A colour label this build has no swatch for is accepted, deliberately:
    // the member is a NAME so a label a newer build invented round-trips
    // rather than being normalised away (PRD I10). An adapter that validated
    // against this build's swatch list would be that normalisation.
    JsonValue future = layerParam("Detail");
    future.set("label", JsonValue::string("chartreuse"));
    check(applyCommand(od, Command{"set_layer_color_label", future}).ok &&
              od.document.layers[2].colorLabel == "chartreuse",
          "params: a colour label this build has no swatch for round-trips");
  }

  std::printf("  -- F. a set command's result is one edit, and moves the cursor --\n");
  {
    // core/LayerSetOps' single-history-entry contract, seen from the command
    // layer: N layers changed, one entry, because N entries for one gesture
    // would make undo restore a state the user never saw.
    OpenDocument od = makeLayerCommandDocument();
    const size_t entriesBefore = od.history.entries().size();
    const CommandResult r =
        applyCommand(od, Command{"lock_layers", layersParam({"Base", "Mid", "Detail"})});
    check(r.ok && od.history.entries().size() == entriesBefore + 1,
          "set: locking three layers is ONE history entry");
    check(od.document.layers[0].locked && od.document.layers[1].locked &&
              od.document.layers[2].locked,
          "set: and all three are locked");

    // A set delete must not leave `activeLayer` pointing past the stack: the
    // next step in an action would then act on a clamped-away row.
    OpenDocument del = makeLayerCommandDocument();
    del.activeLayer = 2;
    const CommandResult gone =
        applyCommand(del, Command{"delete_layers", layersParam({"Mid", "Detail"})});
    check(gone.ok && del.document.layers.size() == 1, "set: delete_layers removed both");
    check(activeLayerIndex(del) == std::optional<size_t>(0) &&
              activeLayerOf(del) != nullptr && activeLayerOf(del)->name == "Base",
          "set: the cursor landed on the survivor, by name");
  }

  return ok;
}

}  // namespace np
