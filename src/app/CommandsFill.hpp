#pragma once

#include <string>

#include "app/Command.hpp"
#include "core/TileStore.hpp"
#include "ops/Fill.hpp"

// app/CommandsFill -- the command rows for PRD D26 (fill and stroke a
// selection or layer with colour, pattern or gradient) and, riding beside
// them, `define_pattern`'s first encoder.
//
// **Why `define_pattern` gets an encoder here rather than in a header of its
// own.** `app/CommandsPatterns.cpp` (`define_pattern`, `fill_with_pattern`)
// predates this track and has no header: nothing in the built application
// called either command yet, so nothing needed one (docs/automation.md §2.2's
// "beside the reader" rule has no reader-file to sit beside when the reader
// has never been reached). This track is what gives Define Pattern its first
// menu item and dialog, so its encoder is written here, once, rather than
// leaving a second header stub for the one function that needs it. Adding a
// second command to that family later is the moment a `CommandsPatterns.hpp`
// earns its own existence; one function does not.
//
// Everything else follows `app/CommandsImage.hpp`'s own shape: the applier
// reads a `JsonValue` (app/CommandsFill.cpp), the encoder beside it builds
// one from the same params struct the dialog's controls hold, and nothing
// outside this pair constructs a `Command{"fill", ...}` or `Command{"stroke",
// ...}` literal.
namespace np {

// PRD D26's "Location" control for Edit > Stroke. Not part of `ops/Fill.hpp`:
// it names how `app/CommandsFill.cpp` builds the BAND selection stroke draws
// into (via `core/SelectionRefine.hpp`'s grow/shrink), which is a command-layer
// decision, not something the fill engine itself needs to know -- `fillTiles()`
// paints a rectangle it is given and has never heard of a stroke.
enum class StrokeLocation {
  Inside,
  Center,
  Outside,
};

// `ops/Fill::FillParams` plus the two controls Stroke adds beyond Fill's own
// dialog. Held as one struct, rather than passed as loose arguments, for the
// same reason `UnsharpParams` is: the dialog's controls and the encoder's
// argument are the same object, so a field added to one is visible from the
// other.
struct StrokeParams {
  FillParams fill;
  // Document texels, split across the band per `location` (`Center` puts
  // `width / 2` on each side of the selection's own edge). Refused at zero or
  // below -- a zero-width stroke is the identity, and accepting it would be
  // exactly the silent no-op docs/automation.md §3's rule 4 refuses.
  float width = 1.0f;
  StrokeLocation location = StrokeLocation::Center;
};

// Appends `fill`, `stroke` to the command table (app/Command.cpp's `table()`).
void registerFillCommands(std::vector<CommandSpec>* out);

// --- Encoders, beside app/CommandsFill.cpp's readers ----------------------

Command fillCommand(const FillParams& p);
Command strokeCommand(const StrokeParams& p);

// What `fill` or `stroke` would leave on the active layer, computed from a
// const document by the code the command itself runs -- the Fill and Stroke
// dialogs' live preview. Empty on success, with `*changedOut` texels changed
// and, when any did, `*layerOut` the whole composed layer; else the refusal.
std::string previewFillCommand(const OpenDocument& doc, const Command& command, TileStore* layerOut,
                               size_t* changedOut);

// `define_pattern`'s encoder -- see this header's own note above for why it
// lives here. Beside `app/CommandsPatterns.cpp`'s `doDefinePattern()`, which
// reads exactly the one key this writes.
Command definePatternCommand(const std::string& name);

}  // namespace np
