#include <cmath>
#include <string>

#include "app/CommandSupport.hpp"
#include "core/RegionOps.hpp"

// app/CommandsRegions -- the command rows for `core::Region` (PLAN.md
// gap-closing wave, track `region`): create, delete, rename, move and resize.
//
// docs/automation.md §1's rule decides this the same way it decides every
// other row in the table: each of these five is a pure function of an
// `OpenDocument` plus its own parameters, with nothing session-side in its
// signature, so each is recordable and the honest answer is a row here, not
// `NotRecordable` in app/CommandCoverage.cpp (that file audits `MenuAction`s,
// and none of these five has one -- a region is created and edited by an
// on-canvas drag, `app/RegionTool`'s own gesture, never a menu item).
//
// **The interactive gesture itself does not call through here, and that is
// not a gap this file leaves open.** `app/CropTool`'s own on-canvas drag
// commits through `applyCropSession()` directly, never `applyCommand()` --
// only `crop_to_selection` and `trim_to_content`, the two MENU-triggered
// geometry ops, are registered rows, and the interactive rectangle itself is
// treated as the same kind of live, session-owned gesture `MenuAction::
// FreeTransform` is (`NotRecordable`, "the session owns the live transform").
// A region drag is the identical shape of gesture over the identical
// non-destructive replacement: two numbers dragged into place, not a value a
// dialog could hold. What IS recordable, and what this file registers, is
// the EDIT that gesture commits -- the same split app/CropTool.hpp keeps
// between its own untouched interactive drag and its two menu rows.
//
// Every row is an adapter and nothing more: read the parameters, refuse what
// is missing or malformed **by name**, call `core::RegionOps` (which already
// owns every refusal an empty rectangle or a missing index deserves), and
// translate the result through `app/CommandSupport.hpp`'s
// `fromDocumentOpResult()` -- `core::recordLayerEdit()`'s own adapter, used
// unchanged because a region edit bumps `OpenDocument::revision` and appends
// a `core::History` entry exactly the way a layer edit does (core/Region.hpp
// §3's whole argument for living inside `Document` in the first place).
//
// **Addressed by NAME, never by index.** `doc.regions`' own uniqueness
// guarantee (core/Region.hpp) is what makes this possible without a second
// "active region" concept the way layers have an active layer: there is
// nothing session-side to fall back to, so the `"region"` parameter is
// REQUIRED on every row but `add_region` -- `resolveRegionTarget()` below,
// `app/CommandSupport.hpp`'s `resolveTarget()` for layers, minus the
// active-layer fallback that has no region-side equivalent.
//
// **Pixel-unit parameters.** `add_region`'s x/y/rect_width/rect_height,
// `move_region`'s x/y and `resize_region`'s x/y/rect_width/rect_height are
// all in document pixels and therefore belong in `app/Batch.cpp`'s
// `kPixelUnitParams` table -- see that file, updated alongside this one.
// **Not spelled `width`/`height`**: those two names are already claimed, by
// `image_size` and `canvas_size`, for the OPPOSITE classification --
// `app/Batch.cpp`'s own comment there is explicit that a destination extent
// ("resize to 512x512") means the same thing at every resolution, so those
// two rows are deliberately absent from the table. `kPixelUnitParams` is
// keyed by parameter NAME, not by (command, name) pair, so reusing `width`/
// `height` here would have forced `image_size`/`canvas_size` into the
// pixel-unit column too -- `app/selftest/Batch.cpp` section I's own
// both-directions check is what catches exactly this collision.
namespace np {
namespace {

// `resolveTarget()` (app/CommandSupport.hpp) for regions: no active-region
// fallback exists, so the `"region"` key is required on every row that reads
// one.
std::string resolveRegionTarget(const OpenDocument& doc, const JsonValue& params, size_t* out) {
  const JsonValue* named = params.find("region");
  if (named == nullptr || !named->isString()) {
    return "refused: this command needs a \"region\" parameter naming which region to act on -- "
           "there is no active region to fall back to.";
  }
  const std::string& name = named->asString();
  for (size_t i = 0; i < doc.document.regions.size(); ++i) {
    if (doc.document.regions[i].name == name) {
      *out = i;
      return {};
    }
  }
  return "refused: this document has no region named \"" + name +
         "\". An action addresses regions by name, so that replaying it on another document "
         "cannot silently act on a different one.";
}

std::string regionTargetUnavailable(const OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  return resolveRegionTarget(doc, params, &index);
}

// A whole-number pixel parameter, `app/CommandsImage.cpp`'s `readWhole()` own
// shape: refused BY NAME for a missing key, a non-number, or a fractional
// value -- a region rectangle has no sub-pixel meaning.
std::string readRegionWhole(const JsonValue& params, const char* commandId, const char* key,
                           int32_t* out) {
  const JsonValue* v = params.find(key);
  if (v == nullptr)
    return "refused: " + std::string(commandId) + " needs a \"" + key + "\" parameter.";
  if (!v->isNumber())
    return "refused: " + std::string(commandId) + "'s \"" + key + "\" must be a number.";
  const double d = v->asNumber();
  if (!std::isfinite(d) || d != std::floor(d))
    return "refused: " + std::string(commandId) + "'s \"" + key +
           "\" must be a whole number of pixels.";
  *out = static_cast<int32_t>(d);
  return {};
}

// --------------------------------------------------------------------------
// add_region
// --------------------------------------------------------------------------

CommandResult doAddRegion(OpenDocument& doc, const JsonValue& params) {
  const JsonValue* kindParam = params.find("kind");
  if (kindParam == nullptr || !kindParam->isString())
    return commandRefused("refused: add_region needs a \"kind\" parameter, \"Frame\" or "
                          "\"Slice\".");
  const std::optional<RegionKind> kind = regionKindFromName(kindParam->asString());
  if (!kind)
    return commandRefused("refused: add_region's \"kind\" is \"" + kindParam->asString() +
                          "\", which names neither \"Frame\" nor \"Slice\".");

  int32_t x = 0, y = 0, width = 0, height = 0;
  std::string why = readRegionWhole(params, "add_region", "x", &x);
  if (why.empty()) why = readRegionWhole(params, "add_region", "y", &y);
  if (why.empty()) why = readRegionWhole(params, "add_region", "rect_width", &width);
  if (why.empty()) why = readRegionWhole(params, "add_region", "rect_height", &height);
  if (!why.empty()) return commandRefused(why);
  if (width < 1) return commandRefused("refused: add_region's \"rect_width\" must be at least 1 pixel.");
  if (height < 1)
    return commandRefused("refused: add_region's \"rect_height\" must be at least 1 pixel.");

  const std::string name = params.stringOr("name", "");
  return fromDocumentOpResult(
      recordLayerEdit(doc, addRegion(doc.document, *kind, x, y, static_cast<uint32_t>(width),
                                     static_cast<uint32_t>(height), name)),
      "add region");
}

// --------------------------------------------------------------------------
// delete_region
// --------------------------------------------------------------------------

CommandResult doDeleteRegion(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveRegionTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  return fromDocumentOpResult(recordLayerEdit(doc, deleteRegion(doc.document, index)),
                              "delete region");
}

// --------------------------------------------------------------------------
// rename_region
// --------------------------------------------------------------------------

CommandResult doRenameRegion(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  const std::string why = resolveRegionTarget(doc, params, &index);
  if (!why.empty()) return commandRefused(why);
  const JsonValue* newName = params.find("new_name");
  if (newName == nullptr || !newName->isString())
    return commandRefused("refused: rename_region needs a \"new_name\" parameter.");
  return fromDocumentOpResult(
      recordLayerEdit(doc, renameRegion(doc.document, index, newName->asString())),
      "rename region");
}

// --------------------------------------------------------------------------
// move_region
// --------------------------------------------------------------------------

CommandResult doMoveRegion(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  std::string why = resolveRegionTarget(doc, params, &index);
  if (why.empty()) {
    int32_t x = 0, y = 0;
    why = readRegionWhole(params, "move_region", "x", &x);
    if (why.empty()) why = readRegionWhole(params, "move_region", "y", &y);
    if (why.empty())
      return fromDocumentOpResult(recordLayerEdit(doc, moveRegion(doc.document, index, x, y)),
                                  "move region");
  }
  return commandRefused(why);
}

// --------------------------------------------------------------------------
// resize_region
// --------------------------------------------------------------------------

CommandResult doResizeRegion(OpenDocument& doc, const JsonValue& params) {
  size_t index = 0;
  std::string why = resolveRegionTarget(doc, params, &index);
  int32_t x = 0, y = 0, width = 0, height = 0;
  if (why.empty()) why = readRegionWhole(params, "resize_region", "x", &x);
  if (why.empty()) why = readRegionWhole(params, "resize_region", "y", &y);
  if (why.empty()) why = readRegionWhole(params, "resize_region", "rect_width", &width);
  if (why.empty()) why = readRegionWhole(params, "resize_region", "rect_height", &height);
  if (!why.empty()) return commandRefused(why);
  if (width < 1)
    return commandRefused("refused: resize_region's \"rect_width\" must be at least 1 pixel.");
  if (height < 1)
    return commandRefused("refused: resize_region's \"rect_height\" must be at least 1 pixel.");
  return fromDocumentOpResult(
      recordLayerEdit(doc, resizeRegion(doc.document, index, x, y, static_cast<uint32_t>(width),
                                        static_cast<uint32_t>(height))),
      "resize region");
}

}  // namespace

void registerRegionCommands(std::vector<CommandSpec>* out) {
  out->push_back({"add_region",
                  "Add Region",
                  {"kind", "name", "x", "y", "rect_width", "rect_height"},
                  documentAlwaysAvailable,
                  doAddRegion});
  out->push_back({"delete_region", "Delete Region", {"region"}, regionTargetUnavailable,
                  doDeleteRegion});
  out->push_back({"rename_region",
                  "Rename Region",
                  {"region", "new_name"},
                  regionTargetUnavailable,
                  doRenameRegion});
  out->push_back({"move_region",
                  "Move Region",
                  {"region", "x", "y"},
                  regionTargetUnavailable,
                  doMoveRegion});
  out->push_back({"resize_region",
                  "Resize Region",
                  {"region", "x", "y", "rect_width", "rect_height"},
                  regionTargetUnavailable,
                  doResizeRegion});
}

}  // namespace np
