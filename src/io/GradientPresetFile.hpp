#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "app/GradientTool.hpp"

// io/GradientPresetFile -- `.npgradient`, the text form of a
// `GradientPresetStops` (app/GradientTool.hpp § 2a), and the library
// directory it lives in. PRD D24's "saveable presets".
//
// Shaped like `io/ActionFile` on purpose, one directory over: a named library
// item, one human-readable JSON file per item, resolved through its own
// `NP_*_DIR` override so `--selftest` and the golden harness never touch the
// real one. Copied rather than shared for the reason `io/ActionFile.cpp`
// gives for its own copy of `actionsDirectoryPath()` -- eight of these
// resolvers exist already (app/DabLibrary.cpp, app/PanelLayout.cpp, and the
// rest) and none of them has ever been the thing that broke.
//
// ==========================================================================
// A colour stop's "foreground" bit is the one field this format has that
// `io/ActionFile` does not need an analogue for
// ==========================================================================
//
// `GradientColorStopSpec::foreground` (app/GradientTool.hpp § 2a) means
// "ignore `color`, use whatever the swatch holds when this gradient is
// drawn" -- so a saved preset's Foreground stops must round-trip as
// Foreground, not as whatever colour happened to be on the swatch the moment
// Save was pressed. The written form is `{"position":.., "foreground":true,
// "midpoint":..}` with NO `"color"` key at all: a reader that saw both would
// have to pick a winner, and either choice is a preset that silently
// disagrees with what Save actually wrote down.
//
// ==========================================================================
// Versioned, flat-per-stop, ordered -- the same three calls io/ActionFile
// makes, for the same reasons (see that header)
// ==========================================================================
namespace np {

// The version this build writes, and the only one it reads.
inline constexpr int kGradientPresetFileVersion = 1;
inline constexpr const char* kGradientPresetFileVersionKey = "npgradientpreset";
inline constexpr const char* kGradientPresetFileExtension = ".npgradient";

// --- text form -----------------------------------------------------------

// Serialises `name` and `stops` into `*out`. Cannot fail: unlike an Action's
// step parameters (io/ActionFile.hpp § 3), a gradient preset has no
// caller-supplied key namespace for a reserved word to collide with.
void writeGradientPreset(const std::string& name, const GradientPresetStops& stops,
                         std::string* out);

// Parses `text`, which `label` names in every refusal. Returns false and
// leaves `*nameOut`/`*out` untouched on any refusal:
//
//   * no version key, or one this build does not read;
//   * a colour stop that is neither `"foreground": true` nor a `"color"`
//     array of three numbers;
//   * a position, midpoint, opacity or colour component that is not a
//     number.
bool readGradientPreset(std::string_view text, std::string_view label, std::string* nameOut,
                        GradientPresetStops* out, std::string* errorOut = nullptr);

// --- files -----------------------------------------------------------------

bool saveGradientPresetToFile(const std::string& path, const std::string& name,
                              const GradientPresetStops& stops, std::string* errorOut = nullptr);

// A missing file is a refusal here, exactly as `loadActionFromFile()` -- a
// caller naming one preset file has already been told it exists, by a
// listing or a picker row.
bool loadGradientPresetFromFile(const std::string& path, std::string* nameOut,
                                GradientPresetStops* out, std::string* errorOut = nullptr);

// Removes the file at `path`. Refuses, naming the reason, if it does not
// exist or cannot be removed -- a delete that reports success without
// deleting is the one silent failure a "delete" button must never have.
bool deleteGradientPresetFile(const std::string& path, std::string* errorOut = nullptr);

// `~/Library/Application Support/naturalPaint/gradients/`, or the XDG and
// no-HOME cases -- `actionsDirectoryPath()`'s resolver exactly, one leaf over,
// with its own override: `NP_GRADIENT_DIR`. No trailing slash.
std::string gradientPresetsDirectoryPath();

// The file name for a preset called `name`, extension included, or empty
// when `name` has nothing usable in it. `actionFileNameFor()`'s sanitiser,
// unchanged.
std::string gradientPresetFileNameFor(std::string_view name);

// Every `.npgradient` file directly in `dir`, full paths, sorted by file
// name. A directory that does not exist is an empty list, not a refusal --
// `listActionFiles()`'s rule, for the same reason.
std::vector<std::string> listGradientPresetFiles(const std::string& dir);

// --- the picker's library --------------------------------------------------

// One row of the options-bar picker's library section.
struct GradientPresetLibraryRow {
  std::string path;
  std::string name;
};

// Every `.npgradient` file in `dir`, sorted by file name, each with its own
// saved NAME read out of it -- unlike `io/ActionFile`'s `ActionLibraryRow`,
// which shows a file's STEM to avoid opening every file in what can be a
// large action library, a gradient library is small in practice (a handful of
// named ramps, not hundreds of recordings) and the picker's whole point is
// showing the name a user actually gave it, which only the file's own
// contents holds. A file that fails to parse (`readGradientPreset()`
// refuses it) is skipped rather than shown with a blank or placeholder name;
// its path is still on disk for `--selftest` or a human to find, just not in
// this list.
std::vector<GradientPresetLibraryRow> gradientPresetLibrary(const std::string& dir);

}  // namespace np
