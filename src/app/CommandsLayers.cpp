#include "app/CommandsLayers.hpp"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "app/CommandSupport.hpp"
#include "core/Blend.hpp"
#include "core/LayerOps.hpp"
#include "core/LayerSetOps.hpp"

// app/CommandsLayers -- the command rows for layer structure and layer
// properties: `allLayerCommands()`'s gestures, `allLayerSetCommands()`'s
// multi-selection gestures, and core/LayerOps' value setters.
//
// The two flat command enums are already walked tables, which is what makes
// registering them mechanical rather than a judgement call per row -- the
// discipline app/LayerEditor.hpp established so that "a command not in this
// enum is a command exactly one menu can reach".
//
// ==========================================================================
// (1) Three vocabularies, and the line between them, because two of them
//     overlap in what they can reach and MUST NOT overlap in ids
// ==========================================================================
//
// A `.npaction` step names one command. If two ids reached the same edit, a
// recorder would have to choose between them and a reader of the file would
// have to know that the choice meant nothing -- so the boundary below is a
// format decision, not a tidiness one:
//
//   `LayerCommand`     a gesture with **no value attached** (app/LayerEditor
//                      .hpp's own definition): "New Pigment Layer", "Merge
//                      Down", "Toggle Lock". Registered here one row per
//                      enumerator, walked from `allLayerCommands()`.
//   `LayerSetCommand`  the same, over a **set** of layers. Its params take a
//                      list of names, never an index (§3 below).
//   core/LayerOps      the **value** setters -- the opacity slider, the blend
//                      dropdown, the rename field. `set_layer_opacity` carries
//                      a number; `toggle_layer_locked` carries nothing and
//                      reads the layer to decide. Different edits, different
//                      ids, and a recorder picks by which control the user
//                      touched.
//
// **`addLayerMask()` and `removeLayerMask()` are deliberately NOT given a
// second id here**, though they sit inside the line range the plan's step-1
// checklist points at. They carry no value, so they are `LayerCommand::AddMask`
// and `LayerCommand::RemoveMask`, and they are registered under
// `add_layer_mask` / `remove_layer_mask` by the walk below. A second
// `layer_add_mask` id calling the core setter directly would be two spellings
// of one edit in a file format whose whole point is that a step means exactly
// one thing.
//
// The same argument excludes `addLayer()`, `removeLayer()`, `moveLayer()` and
// `duplicateLayer()`: every one of them is already a `LayerCommand`, and the
// gesture -- not the core function -- is what a user performed.
//
// The five op-stack mutators (`addLayerOp` and friends) are absent for a
// different reason entirely: they carry an **op**, which needs io/OpSerial's
// text encoding to be written into a step at all, and that is
// app/CommandsOpStack's row to register (docs/automation-plan.md §5).
//
// ==========================================================================
// (2) Why every row here goes through `fromLayerEdit()` or its set-shaped twin
// ==========================================================================
//
// app/CommandSupport.hpp's `fromLayerEdit()` adopts `LayerEditResult::selected`
// into `OpenDocument::activeLayer`, and the reason is docs/automation-plan.md
// §7's "the active layer is the state most likely to make a replay wrong and
// green". It is tempting to think the toggles and the merges could skip it
// because they "do not move the selection" -- they do: a delete moves it to the
// row that took the deleted one's place, a merge moves it to the row the merge
// created, and a create moves it to a layer that did not exist a moment ago.
// **A create is the case that cannot be papered over**, because the new layer's
// index is not a clamp of anything: `activeLayerIndex()` clamps a stale index
// back into the stack, so a stale index after a *flatten* still reads as the
// survivor and an assertion built on one passes with the adoption deleted. The
// suite therefore asserts adoption by NAME, on a create.
//
// ==========================================================================
// (3) A set command's target is a LIST OF NAMES, and one bad name refuses the
//     whole step
// ==========================================================================
//
// core/LayerSetOps.hpp §3 already refuses to apply a set operation to some of
// its members -- "'hide these five' that hides three produces a picture nobody
// asked for and nobody was told about". Resolution has exactly the same
// property one level up: a step recorded against five layers and replayed on a
// document that has four of them is **not** that step. So a name nothing
// matches refuses the whole step, naming the name, and nothing is touched.
//
// The one non-obvious case is two names that resolve to the SAME row --
// either the list repeats a name, or the document has two layers with one name
// (which is legal, docs/document-format.md). `makeLayerSelection()` de-
// duplicates, so the edit would silently act on fewer layers than the step
// named. That is refused too, with the counts, rather than quietly narrowed.
namespace np {
namespace {

// --- parameter reading ----------------------------------------------------
//
// Each of these refuses BY NAME and by type. `JsonValue::boolOr()` and friends
// take a fallback, which is the right shape for an optional parameter and the
// wrong one for a required value: a `set_layer_visible` step whose `"visible"`
// key was misspelt would silently hide the layer, and a replayed action that
// does the opposite of what it says is precisely the silent-wrong-answer
// failure docs/automation-plan.md §7 is built around.

std::string requireBool(const JsonValue& params, const char* key, bool* out) {
  const JsonValue* v = params.find(key);
  if (v == nullptr || !v->isBool())
    return std::string("refused: this step needs a true/false \"") + key +
           "\" value, and this one has none.";
  *out = v->asBool();
  return {};
}

std::string requireNumber(const JsonValue& params, const char* key, double* out) {
  const JsonValue* v = params.find(key);
  if (v == nullptr || !v->isNumber())
    return std::string("refused: this step needs a numeric \"") + key +
           "\" value, and this one has none.";
  *out = v->asNumber();
  return {};
}

std::string requireString(const JsonValue& params, const char* key, std::string* out) {
  const JsonValue* v = params.find(key);
  if (v == nullptr || !v->isString())
    return std::string("refused: this step needs a text \"") + key +
           "\" value, and this one has none.";
  *out = v->asString();
  return {};
}

// --- single-layer targeting ------------------------------------------------

// `resolveTarget()`, plus the one case it is right to refuse and this
// vocabulary is not: a document with **no layers at all**.
//
// `resolveTarget()` answers "this document has no layer to act on", which is
// the correct answer for a blur and the wrong one for `new_rgb_layer` -- a
// create is the one gesture an empty stack can take (core/LayerOps.hpp: a
// zero-layer document is representable and `Document::createBlank()` is not
// the only way to reach one). So an absent `"layer"` key over an empty stack
// resolves to `layers.size()`, the one-past-the-end index
// `insertionIndexFor()` already reads as "no selection", and
// `layerCommandAvailable()` -- not this function -- decides whether the
// command survives it.
//
// A `"layer"` key that is PRESENT is never given this latitude: a named layer
// that does not exist is a refusal on every command, empty stack or not.
std::string resolveLayerCommandTarget(const OpenDocument& doc, const JsonValue& params,
                                      size_t* out) {
  if (params.find("layer") == nullptr && doc.document.layers.empty()) {
    *out = 0;
    return {};
  }
  return resolveTarget(doc, params, out);
}

// The sentence for a gesture this row cannot take. `layerCommandAvailable()`
// returns a bool and no reason -- it is what a menu greys an item out on -- so
// the reason is reconstructed here from what a replayer can see, which is the
// document and the row. Naming the layer matters more than naming the rule: an
// action refused at step 4 of 6 is read by someone who has to find the layer.
std::string unavailableSentence(const OpenDocument& doc, const char* label, size_t index) {
  const std::string where = index < doc.document.layers.size()
                                ? "layer \"" + doc.document.layers[index].name + "\""
                                : "a document with no layer selected";
  return "refused: \"" + std::string(label) + "\" is not available for " + where +
         " in this document. This is the state the menu greys the item out in; the action was "
         "recorded against a document where the gesture had a meaning.";
}

template <LayerCommand kCommand>
std::string layerCommandUnavailable(const OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveLayerCommandTarget(doc, params, &index);
  if (!why.empty()) return why;
  if (!layerCommandAvailable(doc.document, kCommand, index))
    return unavailableSentence(doc, layerCommandLabel(kCommand), index);
  return {};
}

// The adapter, and it is nothing more than one: resolve, call the gesture that
// already exists, translate. Templated on the enumerator because
// `CommandSpec::apply` is a bare function pointer with nowhere to carry the
// command -- which is the property that keeps a row from smuggling state in.
template <LayerCommand kCommand>
CommandResult doLayerCommand(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveLayerCommandTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  return fromLayerEdit(doc, applyLayerCommand(doc, kCommand, index),
                       layerCommandLabel(kCommand));
}

// --- the LayerCommand table ------------------------------------------------
//
// One entry per enumerator, each naming the enumerator exactly once. This is
// the ONLY place the enumerator-to-id mapping exists; `layerCommandId()` reads
// it back so `--selftest` can walk `allLayerCommands()` and find the gap
// rather than consult a second list that would have to be maintained in step
// with this one (app/CommandsLayers.hpp says why that matters).
struct LayerCommandRow {
  LayerCommand command;
  const char* id;
  std::string (*unavailableReason)(const OpenDocument&, const JsonValue&);
  CommandResult (*apply)(OpenDocument&, const JsonValue&);
};

template <LayerCommand kCommand>
LayerCommandRow layerRow(const char* id) {
  return {kCommand, id, &layerCommandUnavailable<kCommand>, &doLayerCommand<kCommand>};
}

const std::vector<LayerCommandRow>& layerCommandRows() {
  static const std::vector<LayerCommandRow> kRows = {
      // Ids are lower_snake_case and are NOT derived from
      // `layerCommandLabel()`: menu text is UI copy and gets reworded, and a
      // file keyed by it would break when someone improved a menu
      // (app/Command.hpp §3). They are not derived from the ordinal either.
      layerRow<LayerCommand::NewRgbLayer>("new_rgb_layer"),
      layerRow<LayerCommand::NewPigmentLayer>("new_pigment_layer"),
      layerRow<LayerCommand::NewAdjustmentLayer>("new_adjustment_layer"),
      layerRow<LayerCommand::NewVectorLayer>("new_vector_layer"),
      layerRow<LayerCommand::NewTextLayer>("new_text_layer"),
      layerRow<LayerCommand::NewFlatsLayer>("new_flats_layer"),
      layerRow<LayerCommand::DuplicateLayer>("duplicate_layer"),
      layerRow<LayerCommand::DeleteLayer>("delete_layer"),
      layerRow<LayerCommand::MoveLayerUp>("move_layer_up"),
      layerRow<LayerCommand::MoveLayerDown>("move_layer_down"),
      layerRow<LayerCommand::AddMask>("add_layer_mask"),
      layerRow<LayerCommand::RemoveMask>("remove_layer_mask"),
      layerRow<LayerCommand::ToggleVisible>("toggle_layer_visible"),
      layerRow<LayerCommand::ToggleLocked>("toggle_layer_locked"),
      layerRow<LayerCommand::ToggleClipped>("toggle_layer_clipped"),
      layerRow<LayerCommand::ToggleAlphaLock>("toggle_layer_alpha_lock"),
      layerRow<LayerCommand::ToggleFlatsReference>("toggle_layer_flats_reference"),
      layerRow<LayerCommand::MergeDown>("merge_down"),
      layerRow<LayerCommand::MergeVisible>("merge_visible"),
      layerRow<LayerCommand::StampVisible>("stamp_visible"),
      layerRow<LayerCommand::FlattenImage>("flatten_image"),
      layerRow<LayerCommand::RasteriseLayer>("rasterise_layer"),
      layerRow<LayerCommand::CaptureComp>("capture_comp"),
  };
  return kRows;
}

// --- set targeting ---------------------------------------------------------

// Resolves `"layers": ["Base", "Detail"]` into a `LayerSelection`. §3 above is
// the contract; this is the whole of its implementation.
//
// There is no fall back to the active layer, deliberately. A set command over
// an unnamed set is a step whose meaning depends on where the previous step
// left the cursor, and the two commands that most need naming -- delete and
// ungroup -- are the two where guessing wrong destroys work.
std::string resolveLayerSet(const OpenDocument& doc, const JsonValue& params,
                           LayerSelection* out) {
  const JsonValue* named = params.find("layers");
  if (named == nullptr || !named->isArray() || named->size() == 0)
    return "refused: a layer-set command needs a \"layers\" list naming the layers it acts on. "
           "A set is not an index, and there is no active-layer fallback: a set step whose "
           "target depended on where the previous step left the selection would mean something "
           "different on every document it replayed on.";
  std::vector<size_t> indices;
  indices.reserve(named->size());
  for (size_t i = 0; i < named->size(); ++i) {
    const JsonValue& entry = named->at(i);
    if (!entry.isString())
      return "refused: every entry of \"layers\" must be a layer name. An index would address a "
             "different layer on a document whose stack is in another order.";
    const size_t index = layerIndexNamed(doc.document, entry.asString());
    if (index >= doc.document.layers.size())
      return "refused: this document has no layer named \"" + entry.asString() +
             "\". The whole step is refused; the layers that did resolve are deliberately NOT "
             "acted on, because a set edit over some of its members is a different edit from "
             "the one that was recorded.";
    indices.push_back(index);
  }
  LayerSelection sel = makeLayerSelection(std::move(indices));
  if (sel.size() != named->size())
    return "refused: \"layers\" names " + std::to_string(named->size()) +
           " layers but resolves to only " + std::to_string(sel.size()) +
           " rows -- a name is repeated, or two layers share one name. Layer names are not "
           "unique, so the step is refused rather than silently applied to fewer layers than "
           "it names.";
  *out = std::move(sel);
  return {};
}

// `fromLayerEdit()`'s job for a set result.
//
// **Local to this file rather than a fifth helper in app/CommandSupport.hpp,
// and the reason is a merge one**: that header is shared by the four command
// families being written in parallel (app/Command.hpp's own note on why they
// are separate translation units), and this is the only family that will ever
// have a `LayerSetEditResult` to translate. Promote it there when a second one
// appears, which is io/ExportAs.cpp's own "a third consumer is when this
// becomes a shared header" rule one copy earlier.
//
// The selection it adopts is the TOP row of where the set landed. A set
// command has no single "the layer this produced", and `OpenDocument` has no
// multi-selection member to adopt into -- the panel's multi-selection is
// session state and `applyCommand()` may not reach for it (§2 of
// app/Command.hpp). What it must not do is leave `activeLayer` pointing at a
// row a delete removed, so an empty landing re-clamps the index that is
// already there rather than inventing a new one.
CommandResult fromLayerSetEdit(OpenDocument& doc, const LayerSetEditResult& e,
                               const char* opName) {
  CommandResult r;
  r.ok = e.ok;
  r.warnings = e.warnings;
  r.status = e.ok ? std::string(opName) + ": done" : e.error;
  if (e.ok) {
    if (!e.selection.empty())
      setActiveLayer(doc, e.selection.indices.back());
    else
      setActiveLayer(doc, doc.activeLayer);
    r.texelsChanged = 1;
  }
  return r;
}

template <LayerSetCommand kCommand>
std::string layerSetUnavailable(const OpenDocument& doc, const JsonValue& params) {
  LayerSelection sel;
  const std::string why = resolveLayerSet(doc, params, &sel);
  if (!why.empty()) return why;
  if (!layerSetCommandAvailable(doc.document, kCommand, sel))
    return "refused: \"" + std::string(layerSetCommandLabel(kCommand)) +
           "\" is not available for the " + std::to_string(sel.size()) +
           " layers this step names. This is the state the menu greys the item out in -- the "
           "gesture has no meaning for this selection, which is not the same as being refused "
           "by a lock or a clip, and those still answer with core/LayerSetOps' own sentence.";
  return {};
}

template <LayerSetCommand kCommand>
CommandResult doLayerSetCommand(OpenDocument& doc, const JsonValue& params) {
  LayerSelection sel;
  const std::string why = resolveLayerSet(doc, params, &sel);
  if (!why.empty()) return commandRefused(why);
  return fromLayerSetEdit(doc, applyLayerSetCommand(doc, kCommand, sel),
                          layerSetCommandLabel(kCommand));
}

struct LayerSetCommandRow {
  LayerSetCommand command;
  const char* id;
  std::string (*unavailableReason)(const OpenDocument&, const JsonValue&);
  CommandResult (*apply)(OpenDocument&, const JsonValue&);
};

template <LayerSetCommand kCommand>
LayerSetCommandRow setRow(const char* id) {
  return {kCommand, id, &layerSetUnavailable<kCommand>, &doLayerSetCommand<kCommand>};
}

const std::vector<LayerSetCommandRow>& layerSetCommandRows() {
  static const std::vector<LayerSetCommandRow> kRows = {
      setRow<LayerSetCommand::DeleteLayers>("delete_layers"),
      setRow<LayerSetCommand::DuplicateLayers>("duplicate_layers"),
      setRow<LayerSetCommand::MoveLayersUp>("move_layers_up"),
      setRow<LayerSetCommand::MoveLayersDown>("move_layers_down"),
      setRow<LayerSetCommand::GroupLayers>("group_layers"),
      setRow<LayerSetCommand::UngroupLayers>("ungroup_layers"),
      setRow<LayerSetCommand::ShowLayers>("show_layers"),
      setRow<LayerSetCommand::HideLayers>("hide_layers"),
      setRow<LayerSetCommand::LockLayers>("lock_layers"),
      setRow<LayerSetCommand::UnlockLayers>("unlock_layers"),
      setRow<LayerSetCommand::ClipLayers>("clip_layers"),
      setRow<LayerSetCommand::UnclipLayers>("unclip_layers"),
      setRow<LayerSetCommand::LinkLayers>("link_layers"),
      setRow<LayerSetCommand::UnlinkLayers>("unlink_layers"),
      // **One command per label, and that is a decision rather than an
      // oversight** (core/LayerSetOps.hpp: "the label *is* the menu item, the
      // way 'New Pigment Layer' is"). Collapsing the eight into one
      // `label_layers` with a `"colour"` parameter would read better in a
      // file and would be wrong in the model: the enum is what both menus
      // walk, so a ninth label added there would then reach the menus and not
      // this table, which is the exact drift `allLayerSetCommands()` exists to
      // make impossible. The per-layer *value* setter below
      // (`set_layer_color_label`) is the one that carries an arbitrary string,
      // because core/LayerOps' setter is the one that accepts one.
      setRow<LayerSetCommand::LabelNone>("label_layers_none"),
      setRow<LayerSetCommand::LabelRed>("label_layers_red"),
      setRow<LayerSetCommand::LabelOrange>("label_layers_orange"),
      setRow<LayerSetCommand::LabelYellow>("label_layers_yellow"),
      setRow<LayerSetCommand::LabelGreen>("label_layers_green"),
      setRow<LayerSetCommand::LabelBlue>("label_layers_blue"),
      setRow<LayerSetCommand::LabelViolet>("label_layers_violet"),
      setRow<LayerSetCommand::LabelGrey>("label_layers_grey"),
      setRow<LayerSetCommand::AlignSelectionLeft>("align_selection_left"),
      setRow<LayerSetCommand::AlignSelectionCenterX>("align_selection_center_x"),
      setRow<LayerSetCommand::AlignSelectionRight>("align_selection_right"),
      setRow<LayerSetCommand::AlignSelectionTop>("align_selection_top"),
      setRow<LayerSetCommand::AlignSelectionCenterY>("align_selection_center_y"),
      setRow<LayerSetCommand::AlignSelectionBottom>("align_selection_bottom"),
      setRow<LayerSetCommand::AlignCanvasLeft>("align_canvas_left"),
      setRow<LayerSetCommand::AlignCanvasCenterX>("align_canvas_center_x"),
      setRow<LayerSetCommand::AlignCanvasRight>("align_canvas_right"),
      setRow<LayerSetCommand::AlignCanvasTop>("align_canvas_top"),
      setRow<LayerSetCommand::AlignCanvasCenterY>("align_canvas_center_y"),
      setRow<LayerSetCommand::AlignCanvasBottom>("align_canvas_bottom"),
      setRow<LayerSetCommand::DistributeHorizontally>("distribute_horizontally"),
      setRow<LayerSetCommand::DistributeVertically>("distribute_vertically"),
  };
  return kRows;
}

// --- the value setters -----------------------------------------------------
//
// Each is `resolveTarget()` -> read the value -> the core setter ->
// `recordLayerEdit()`, and each **refuses through the setter it already has**
// rather than pre-checking: the lock, the [0,1] opacity bound, the `Mix`-pair
// rule and the clip's three refusals all live in core/LayerOps and are the
// same sentences the panel shows. A second copy here is how a UI comes to
// offer what the model refuses (core/LayerSetOps.hpp §3 makes the argument at
// length; it applies unchanged to an adapter).
//
// `fromDocumentOpResult()` rather than `fromLayerEdit()`, and the distinction
// is real: none of these moves the selection. They change a property of a row
// that is already there, and `recordLayerEdit()` returns no index to adopt.

CommandResult doSelectLayer(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  setActiveLayer(doc, index);
  CommandResult r;
  r.ok = true;
  r.status = "select layer: \"" + doc.document.layers[index].name + "\"";
  return r;
}

CommandResult doSetLayerBlend(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  const std::string modeName = params.stringOr("mode", "");
  // `Layer::blend` stores the wire NAME and not the enum (core/Blend.hpp), so
  // it would accept this string verbatim -- which is exactly why it goes
  // through `blendModeFromName()` anyway. A mode nothing knows must be a
  // refusal here, not a name written into the document and silently ignored by
  // the compositor on every future open.
  const std::optional<BlendMode> mode = blendModeFromName(modeName);
  if (!mode) return commandRefused("refused: no blend mode is named \"" + modeName + "\".");
  return fromDocumentOpResult(recordLayerEdit(doc, setLayerBlend(doc.document, index, *mode)),
                              "set blend mode");
}

CommandResult doSetLayerOpacity(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  double opacity = 0.0;
  const std::string bad = requireNumber(params, "opacity", &opacity);
  if (!bad.empty()) return commandRefused(bad);
  // Not clamped here: core/LayerOps refuses a value outside [0,1] and refuses
  // NaN, by name, precisely so that a caller cannot put a value on screen that
  // the file would then reject (PRD I11). Clamping in the adapter would turn
  // an action that says 1.4 into one that quietly means 1.0.
  return fromDocumentOpResult(
      recordLayerEdit(doc, setLayerOpacity(doc.document, index, static_cast<float>(opacity))),
      "set opacity");
}

CommandResult doSetLayerVisible(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  bool visible = false;
  const std::string bad = requireBool(params, "visible", &visible);
  if (!bad.empty()) return commandRefused(bad);
  return fromDocumentOpResult(
      recordLayerEdit(doc, setLayerVisible(doc.document, index, visible)), "set visibility");
}

CommandResult doSetLayerLocked(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  bool locked = false;
  const std::string bad = requireBool(params, "locked", &locked);
  if (!bad.empty()) return commandRefused(bad);
  return fromDocumentOpResult(recordLayerEdit(doc, setLayerLocked(doc.document, index, locked)),
                              "set lock");
}

CommandResult doSetLayerAlphaLocked(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  bool alphaLocked = false;
  const std::string bad = requireBool(params, "alpha_locked", &alphaLocked);
  if (!bad.empty()) return commandRefused(bad);
  return fromDocumentOpResult(
      recordLayerEdit(doc, setLayerAlphaLocked(doc.document, index, alphaLocked)),
      "set alpha lock");
}

CommandResult doSetLayerClipped(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  bool clipped = false;
  const std::string bad = requireBool(params, "clipped", &clipped);
  if (!bad.empty()) return commandRefused(bad);
  return fromDocumentOpResult(recordLayerEdit(doc, setLayerClipped(doc.document, index, clipped)),
                              "set clipping");
}

CommandResult doSetLayerFlatsReference(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  bool reference = false;
  const std::string bad = requireBool(params, "reference", &reference);
  if (!bad.empty()) return commandRefused(bad);
  return fromDocumentOpResult(
      recordLayerEdit(doc, setLayerFlatsReference(doc.document, index, reference)),
      "set flats reference");
}

CommandResult doSetLayerName(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  std::string name;
  const std::string bad = requireString(params, "name", &name);
  if (!bad.empty()) return commandRefused(bad);
  // An empty name is accepted, because core/LayerOps accepts one and it means
  // "unnamed" (core/Layer.hpp). Note what this command does to the step AFTER
  // it in an action: targeting is by name, so a rename moves the target of
  // every later step that addressed the old one. That is the user's edit, not
  // a defect -- the recorder writes what happened -- and it is why a rename
  // is worth seeing in a diffable file (PRD P5).
  return fromDocumentOpResult(
      recordLayerEdit(doc, setLayerName(doc.document, index, std::move(name))), "rename layer");
}

CommandResult doSetLayerColorLabel(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  std::string label;
  const std::string bad = requireString(params, "label", &label);
  if (!bad.empty()) return commandRefused(bad);
  // Any string, including one this build has no swatch for. core/LayerOps is
  // explicit that the member is a NAME for `np:blend`'s reason, so a label a
  // newer build invented round-trips instead of being normalised away (PRD
  // I10) -- and an adapter that validated it against this build's swatch list
  // would be the exact normalisation that rule forbids.
  return fromDocumentOpResult(
      recordLayerEdit(doc, setLayerColorLabel(doc.document, index, std::move(label))),
      "set colour label");
}

CommandResult doSetLayerLinkGroup(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  double group = 0.0;
  const std::string bad = requireNumber(params, "group", &group);
  if (!bad.empty()) return commandRefused(bad);
  if (!(group >= 0.0) || group != std::floor(group))
    return commandRefused(
        "refused: a link group is a whole number of zero or more; zero unlinks the layer.");
  return fromDocumentOpResult(
      recordLayerEdit(doc,
                      setLayerLinkGroup(doc.document, index, static_cast<uint64_t>(group))),
      "set link group");
}

}  // namespace

const char* layerCommandId(LayerCommand command) noexcept {
  for (const LayerCommandRow& row : layerCommandRows())
    if (row.command == command) return row.id;
  return nullptr;
}

const char* layerSetCommandId(LayerSetCommand command) noexcept {
  for (const LayerSetCommandRow& row : layerSetCommandRows())
    if (row.command == command) return row.id;
  return nullptr;
}

// --- the value setters' encoders (app/CommandsLayers.hpp) ------------------
//
// Directly beneath the `requireBool()` / `requireNumber()` / `requireString()`
// readers they feed, which is the whole of that header's argument for where
// they live. Each writes exactly the one key its adapter requires, and no
// `"layer"` -- see the header on why the active-layer fallback is the only
// safe target for a panel control.
namespace {

Command activeLayerCommand(const char* id, const char* key, JsonValue value) {
  Command c;
  c.id = id;
  c.params = JsonValue::object();
  c.params.set(key, std::move(value));
  return c;
}

}  // namespace

Command setLayerBlendCommand(BlendMode mode) {
  // By NAME, through `blendModeName()`. `Layer::blend` is itself a string for
  // `np:blend`'s reason (core/LayerOps.hpp), so this is not a conversion an
  // encoder invented -- it is the same canonical spelling the document stores.
  return activeLayerCommand("set_layer_blend", "mode", JsonValue::string(blendModeName(mode)));
}

Command setLayerOpacityCommand(float opacity) {
  // Not clamped, matching `doSetLayerOpacity()`'s own note: core/LayerOps
  // refuses a value outside [0,1] by name, and clamping on the way in would
  // turn a control that asked for 1.4 into one that quietly meant 1.0.
  return activeLayerCommand("set_layer_opacity", "opacity", JsonValue::number(opacity));
}

Command setLayerVisibleCommand(bool visible) {
  return activeLayerCommand("set_layer_visible", "visible", JsonValue::boolean(visible));
}

Command setLayerLockedCommand(bool locked) {
  return activeLayerCommand("set_layer_locked", "locked", JsonValue::boolean(locked));
}

Command setLayerClippedCommand(bool clipped) {
  return activeLayerCommand("set_layer_clipped", "clipped", JsonValue::boolean(clipped));
}

Command setLayerNameCommand(std::string name) {
  // An empty name is passed through, not refused: core/LayerOps accepts one
  // and it means "unnamed" (core/Layer.hpp), and `doSetLayerName()` says so
  // explicitly.
  return activeLayerCommand("set_layer_name", "name", JsonValue::string(std::move(name)));
}

Command setLayerColorLabelCommand(std::string label) {
  // Any string, `kNoLayerColorLabel` included. `doSetLayerColorLabel()` refuses
  // to validate it against this build's swatch list, for PRD I10's reason, and
  // an encoder that normalised here would defeat that from the other side.
  return activeLayerCommand("set_layer_color_label", "label",
                            JsonValue::string(std::move(label)));
}

void registerLayerCommandRows(std::vector<CommandSpec>* out) {
  // `select_layer` first, because it is the step docs/automation-plan.md §5
  // says the recorder emits whenever the active layer changes -- the one that
  // makes "blur the active layer" deterministic on a replay. It belongs to no
  // enum: it changes session state and no document data, which is why it is
  // the one row here that is neither a `LayerCommand` nor a setter.
  out->push_back({"select_layer", "Select Layer", {"layer"}, layerTargetUnavailable, doSelectLayer});

  // The two walked vocabularies, in the order their own tables give -- menu
  // order, which is the order the ACTIONS panel will list them in. Walking
  // rather than listing each `push_back()` by hand is what makes a new
  // enumerator a missing row that `--selftest` names, rather than a row
  // somebody forgot to add twice.
  for (const LayerCommandRow& row : layerCommandRows())
    out->push_back({row.id, layerCommandLabel(row.command), {"layer"}, row.unavailableReason,
                    row.apply});
  for (const LayerSetCommandRow& row : layerSetCommandRows())
    out->push_back({row.id, layerSetCommandLabel(row.command), {"layers"}, row.unavailableReason,
                    row.apply});

  // core/LayerOps' value setters. `set_layer_blend` is the worked example
  // (docs/automation-plan.md's own sample action uses it); the rest follow it
  // exactly.
  out->push_back({"set_layer_blend", "Set Blend Mode", {"layer", "mode"}, layerTargetUnavailable,
                  doSetLayerBlend});
  out->push_back({"set_layer_opacity", "Set Opacity", {"layer", "opacity"},
                  layerTargetUnavailable, doSetLayerOpacity});
  out->push_back({"set_layer_visible", "Set Visibility", {"layer", "visible"},
                  layerTargetUnavailable, doSetLayerVisible});
  out->push_back({"set_layer_locked", "Set Lock", {"layer", "locked"}, layerTargetUnavailable,
                  doSetLayerLocked});
  out->push_back({"set_layer_alpha_locked", "Set Transparency Lock", {"layer", "alpha_locked"},
                  layerTargetUnavailable, doSetLayerAlphaLocked});
  out->push_back({"set_layer_clipped", "Set Clipping", {"layer", "clipped"},
                  layerTargetUnavailable, doSetLayerClipped});
  out->push_back({"set_layer_flats_reference", "Set Flats Reference", {"layer", "reference"},
                  layerTargetUnavailable, doSetLayerFlatsReference});
  out->push_back({"set_layer_name", "Rename Layer", {"layer", "name"}, layerTargetUnavailable,
                  doSetLayerName});
  out->push_back({"set_layer_color_label", "Set Colour Label", {"layer", "label"},
                  layerTargetUnavailable, doSetLayerColorLabel});
  out->push_back({"set_layer_link_group", "Set Link Group", {"layer", "group"},
                  layerTargetUnavailable, doSetLayerLinkGroup});
}

}  // namespace np
