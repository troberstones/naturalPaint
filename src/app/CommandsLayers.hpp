#pragma once

#include <string>

#include "app/Command.hpp"
#include "app/LayerEditor.hpp"
#include "core/Blend.hpp"
#include "core/LayerSetOps.hpp"

// app/CommandsLayers -- the two enumerator-to-command-id maps the layer family
// registers with, exposed for one reader and one reason.
//
// **This header exists so that `--selftest` can prove the registration is
// exhaustive without keeping a second copy of the id list.** The plan's step 1
// gate (docs/automation-plan.md §6) asks that "a command added later without a
// registration fails this test rather than being silently unrecordable", and
// the only honest way to check that for a walked enum is to walk
// `allLayerCommands()` / `allLayerSetCommands()` and ask, per enumerator, which
// id it registered under. A test that instead held its own table of
// enumerator-to-id pairs would pass for a new enumerator the moment somebody
// added it to the *test's* table, which is the failure mode the gate is for.
//
// Both return `nullptr` for an enumerator the family has no row for, which is
// exactly the state the suite goes red on. They are not a general lookup
// service: nothing outside the registration and the test should care, because
// **every other route addresses a command by its string id** (app/Command.hpp
// §3), and a caller that mapped an enum to an id here would be re-introducing
// the ordinal coupling that section rules out.
namespace np {

// The stable `.npaction` id `applyCommand()` reaches this gesture by, or
// nullptr if it is unregistered.
const char* layerCommandId(LayerCommand command) noexcept;

// The same, for the multi-selection gestures.
const char* layerSetCommandId(LayerSetCommand command) noexcept;

// --- the value setters' encoders (docs/automation-plan.md step 2) ---------
//
// The direction this file did not need until the LAYERS panel started calling
// `applyCommand()` instead of `core/LayerOps` directly. Same argument as
// app/CommandsImage.hpp's encoders: the reader that has to understand what was
// written is `doSetLayerBlend()` and its siblings in
// app/CommandsLayers.cpp, so the writer belongs beside them and not three
// lines above a combo box in ui/MacPaintUI.cpp.
//
// **None of them writes a `"layer"` key**, and that is the whole targeting
// story for these call sites. An action file addresses a layer by name
// (app/CommandSupport.hpp's `resolveTarget()`), but a *panel row* is an index,
// and layer names are explicitly not unique (core/LayerOps.hpp) -- so a name
// derived from a row would silently address the FIRST layer sharing it.
// Every call site these encoders serve acts on the active layer, which
// `resolveTarget()` falls back to when the key is absent, so the ambiguity
// never arises. A control that acts on an arbitrary row is deliberately NOT
// migrated; see this commit's message and ui/MacPaintUI.cpp's own note at the
// eye and padlock icons.
//
// Seven, not ten: `set_layer_alpha_locked`, `set_layer_flats_reference` and
// `set_layer_link_group` have no direct UI call site to encode for -- the
// first two are reached as `LayerCommand` toggles and the third only through
// `LayerSetCommand::LinkLayers`. An encoder with no caller would be a
// spelling nothing checks.
Command setLayerBlendCommand(BlendMode mode);
Command setLayerOpacityCommand(float opacity);
Command setLayerVisibleCommand(bool visible);
Command setLayerLockedCommand(bool locked);
Command setLayerClippedCommand(bool clipped);
Command setLayerNameCommand(std::string name);
Command setLayerColorLabelCommand(std::string label);

}  // namespace np
