#pragma once

#include "app/LayerEditor.hpp"
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

}  // namespace np
