#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/Document.hpp"
#include "io/Capabilities.hpp"
#include "io/Export.hpp"
#include "io/ImageDecode.hpp"

// io/ExportAs (PLAN.md "Phase 4 -- Write it out", step 7: "Export As --
// format, space, depth **and resize**, with saveable presets (PRD I15).
// Downscale prefilters; see the phase 6 warning").
//
// PRD I15 (P1), in full: "Export As: target format, colour space, bit depth
// **and resize**, with saveable presets." Four settings and a way to name a
// combination of them. This header is those four settings as one value, the
// rules for what makes a combination legal, the named file that stores them,
// and the composed operation that carries one out. It is deliberately
// **ImGui-free** -- the dialog in ui/MacPaintUI.cpp calls into everything
// here and adds nothing of its own except widgets.
//
// That split is app/CurveEdit.hpp's precedent, restated: the piece of a
// widget that can be wrong in a way a screenshot will not show -- the
// geometry, the list mutation, and here the validation, the resize and the
// preset file -- is the piece that belongs in a testable module, and
// --selftest exercises this header headlessly (runExportAsTest()). What is
// left in ui/ is genuinely only ImGui.
//
// --- The gap this step does not close ------------------------------------
//
// Stated plainly here because every prior UI-facing step's Findings row
// records the same thing and papering over it would be worse than the gap:
// **there is no bridge from the live painting canvas to a core::Document.**
// main.cpp holds no Document at all; sim::PaintSim owns a single dense
// canvas texture with no layer or document awareness, and app/AppState holds
// view state next to it. So the File > Export As... dialog this step adds is
// real, working code -- every combo is populated from the live capability
// query, every keystroke is validated through the same refusal strings the
// encoder itself would produce, presets save and load to a real file -- and
// its Export button is nonetheless **disabled**, because there is nothing
// for it to export from. It says so in the dialog rather than failing
// mysteriously at the moment of use.
//
// That bridge is PLAN.md Phase 4 step 8's decision to make (document
// lifecycle: revert, duplicate, save a copy, save incremental, open recent
// -- all of which need the same Document ownership question answered), and
// inventing it here would be pre-empting it with an ownership model chosen
// by whoever happened to need one first. Everything below takes a
// `const Document&` and is completely ready for the day that argument
// exists: exportDocumentWithRequest() is already the whole operation, and
// --selftest already runs it end to end on documents it builds itself.
//
// --- What a preset is, and what happens to one this build cannot honour ---
//
// A preset is a *name* plus an ExportRequest. Nothing else -- no output path,
// no filename template (that is PRD I17's per-comp naming rule, phase 5), no
// per-format quality knob yet.
//
// The interesting case is not saving one, it is loading one that this binary
// cannot satisfy. It is not hypothetical: this project ships two build
// configurations, and an "EXR half" preset saved from an NP_USE_OIIO=ON
// build is a preset an OFF build has no writer for. Three behaviours were
// available and two of them are wrong:
//
//   * **Silently substitute a format that does work.** Absolutely not. The
//     entire discipline of io/Export ("explicitly, never silently", PRD B6,
//     I5, I11) exists to stop exactly this, and a preset is a promise about
//     what file you are going to get.
//   * **Drop it at load.** Also wrong, and destructive: the preset file is
//     shared between configurations, so an OFF build opening it and saving
//     anything would delete every OIIO preset the user has. That is PRD
//     I10's principle ("an older build cannot destroy a newer document's
//     data") applied to settings rather than documents.
//   * **Load it, keep it, mark it unavailable, and say why.** What this
//     module does. The preset survives load and re-save byte for byte;
//     exportPresetAvailability() reports it unavailable with
//     io/Export's own refusal string -- which names the build option, or the
//     missing OpenImageIO plugin, or the format's real depth limit -- and
//     the dialog shows it greyed with that reason as its tooltip. Applying
//     it fills the dialog's controls in and leaves the request invalid and
//     the Export button disabled, so the user sees precisely what the
//     preset asked for and precisely why this build will not do it.
//
// --selftest asserts this in **both** configurations: the same preset file
// round-trips unchanged in each, and is available in one and refused-with-a-
// named-reason in the other.
//
// A preset naming a token this build does not recognise at all (a format
// added by some future build) is a different case and is handled less well:
// that one preset is skipped, recorded in problems(), and **is not preserved
// on re-save**. Preserving it would mean keeping the raw JSON object for
// every row, which is io/NpaintFile's carry mechanism rebuilt for a settings
// file; it is not built here and the cost is recorded in PLAN.md's Findings
// rather than discovered later.
//
// --- Where presets live ---------------------------------------------------
//
// This project had no user settings file before this step, so choosing one
// is a real decision rather than a lookup. app/Keymap is the nearest
// precedent and is deliberately *not* followed: it loads from
// `${NP_KEYMAP_DIR}`, a compile-time path into the source tree, because a
// keymap is shipped data that the developer hand-edits and hot-reloads.
// Export presets are the opposite -- user-authored at run time, written by
// the application, and meaningless to ship. Putting them in the source tree
// would mean the app writes into its own checkout, and that a release build
// installed read-only could not save a preset at all.
//
// So: `~/Library/Application Support/naturalPaint/export-presets.json` on
// Apple platforms (the platform's own convention for application-managed
// user data), `${XDG_CONFIG_HOME:-~/.config}/naturalPaint/export-presets.json`
// elsewhere, and `$NP_EXPORT_PRESETS` overrides both -- which is what makes
// the location testable without a test writing into the developer's real
// settings. Nothing here creates or reads that path until something asks for
// a preset: PRD A2's cold-start budget and ADR-0001's idle-memory rule both
// say a file nobody opened costs nothing, and --selftest's idle-RSS
// measurement would notice if that stopped being true.
//
// The file is JSON, matching keymaps/default.json rather than introducing a
// second configuration syntax, and is parsed by a small reader private to
// ExportAs.cpp. That reader is deliberately *not* shared with Keymap.cpp's:
// that one is private to its own translation unit and parses a different
// schema, and promoting it would mean editing app/Keymap, which is not this
// step's business -- the same reasoning io/Export.cpp's duplicated
// unpremultiply() carries, and the same note applies: a third consumer is
// when it should be hoisted.
namespace np {

// How a resize is *expressed*. Not "what size" -- what size is derived from
// this plus the source's own dimensions by resolveExportSize(), which is what
// makes a preset applicable to a document of any size (PRD P3 already
// promises exactly that: "Batch output supports format, colour space, bit
// depth and resize, reusing I15's presets", and a batch over a folder is a
// batch over documents of different sizes).
enum class ExportResizeMode {
  // Export at the document's own pixel dimensions. The default.
  None,
  // Scale both axes by `percent`. 100 is a no-op; above 100 is refused --
  // see ops/Resample.hpp on why upscaling is out of scope here.
  Percent,
  // Fit inside a `maxWidth` x `maxHeight` box, preserving aspect ratio, and
  // **never enlarging**: a document already smaller than the box exports at
  // 1:1 rather than being blown up to fill it. This is the mode that makes a
  // preset like "web preview, 2048 long edge" mean the same useful thing for
  // every document it is applied to.
  FitWithin,
};

inline constexpr std::size_t kExportResizeModeCount = 3;

inline const char* exportResizeModeName(ExportResizeMode m) {
  switch (m) {
    case ExportResizeMode::None: return "No resize (document size)";
    case ExportResizeMode::Percent: return "Percentage";
    case ExportResizeMode::FitWithin: return "Fit within (preserves aspect, never enlarges)";
  }
  return "?";
}

// The resize half of a request. Only the fields its `mode` uses are read, so
// switching modes in a dialog never silently loses the other mode's numbers.
struct ExportResize {
  ExportResizeMode mode = ExportResizeMode::None;
  // Percent mode: (0, 100]. Above 100 is an upscale and is refused by name.
  float percent = 100.0f;
  // FitWithin mode: both must be >= 1.
  uint32_t maxWidth = 2048;
  uint32_t maxHeight = 2048;
};

// The four settings PRD I15 names, as one value: format, colour space, bit
// depth, resize. This is what a preset stores and what the dialog edits.
//
// The defaults are the ones PRD I1 guarantees work in every build with no
// optional dependency: 8-bit sRGB PNG at document size. A default that
// depended on NP_USE_OIIO would be a dialog that opens in an invalid state
// half the time.
struct ExportRequest {
  ImageFormat format = ImageFormat::Png;
  ExportTargetSpace targetSpace = ExportTargetSpace::Rec709Srgb;
  ExportBitDepth bitDepth = ExportBitDepth::UInt8;
  ExportResize resize;
};

// Turns a resize plus a source size into the destination size in pixels.
//
// Returns false, with `*errorOut` naming what was refused, for a request that
// cannot produce a size: a non-positive or above-100 percentage, a
// FitWithin box with a zero side, or a zero-sized source. Never returns a
// zero dimension on success -- a scale factor that would round a dimension
// to 0 clamps it to 1, because "80x1" is a real (if odd) image and "80x0" is
// not an image at all.
//
// Never returns a size larger than the source. FitWithin clamps its scale
// factor at 1, and Percent above 100 is refused rather than clamped: those
// two are deliberately different, because "fit within a box bigger than the
// image" is an ordinary thing to ask that has an obvious correct answer,
// while "scale to 150%" has no answer this build will give (ops/Resample.hpp).
bool resolveExportSize(const ExportResize& resize, uint32_t srcWidth, uint32_t srcHeight,
                       uint32_t* outWidth, uint32_t* outHeight, std::string* errorOut);

// Whether this build can write `request`'s (format, depth) combination at
// all, independent of any document. Empty string means yes; otherwise it is
// io/Export's own refusal string, verbatim -- see exportRefusalReason().
//
// This is what a preset list is greyed out by, and it is the answer that
// differs between the two build configurations for the same preset file.
std::string exportRequestAvailability(const ExportRequest& request);

// The formats this build can actually write, in ImageFormat declaration
// order -- the list a format combo box is built from. Never contains a format
// the encoder would refuse, which is how "the dialog cannot offer a
// combination io/Export will reject" is a property of the code rather than a
// promise. Read-only formats (PSD, camera raw) are absent by construction:
// they report canWrite == false.
std::vector<ImageFormat> offerableExportFormats();

// The depths `format` can be written at in this build, in ExportBitDepth
// declaration order. Empty for a format this build cannot write at all.
std::vector<ExportBitDepth> offerableExportDepths(ImageFormat format);

// The outcome of checking a whole request against a document (or against
// nothing at all -- both data arguments are optional, exactly as
// exportRefusalReason()'s are).
//
// `ok` and `error` behave like ExportResult's: `error` is non-empty exactly
// when `ok` is false, and it is always io/Export's or ops/Resample's own
// message rather than a reworded one.
//
// `warnings` is PRD I11 ("A save that would lose data names exactly what,
// rather than degrading silently") applied to the cases that are **legal**.
// A refusal covers the combinations that cannot be honoured; a warning
// covers the ones that can, at a cost the user should be told the size of
// before they click Export -- how many pixels a downscale discards, that
// 8-bit quantisation is 256 levels from a 16-bit half-float working space,
// which highlight value an integer depth will clip, that 8-bit *linear* bands
// ~13x worse than 8-bit sRGB in the shadows, and that JPEG is lossy. Each
// warning names a number, not an adjective.
struct ExportValidation {
  bool ok = false;
  std::string error;
  std::vector<std::string> warnings;
  // The resolved destination size, valid when ok. Equal to the source size
  // when no resize was asked for.
  uint32_t outWidth = 0;
  uint32_t outHeight = 0;
};

// Checks `request` against a source of `srcWidth` x `srcHeight`.
//
// `sourceSpace` (nullptr to skip the primaries check) and `img` (nullptr to
// skip both the translucency check and the highlight-clipping warning) are
// optional for the same reason exportRefusalReason()'s are: the dialog can
// validate a preset with no document open, and validates far more precisely
// once one exists. Nothing is checked *differently* when they are supplied --
// only additionally.
ExportValidation validateExportRequest(const ExportRequest& request, uint32_t srcWidth,
                                       uint32_t srcHeight, const WorkingSpace* sourceSpace,
                                       const DecodedImage* img);

// A named request. `name` is what the preset menu shows and what save/delete
// address it by.
struct ExportPreset {
  std::string name;
  ExportRequest request;
};

// Loads, edits, serialises and stores the named presets PRD I15 asks for.
//
// Deliberately shaped like app/Keymap: a string form and a file form, with
// the string form being the whole implementation and the file form a thin
// wrapper, so --selftest exercises every parse and serialise path without
// touching a filesystem (the same reason io/Export encodes to memory first).
class ExportPresetStore {
 public:
  // Parses the JSON document described at the top of ExportAs.cpp. Replaces
  // whatever this store held. Returns false only for a *structurally* broken
  // file -- bad JSON, a missing "presets" array, a duplicate name -- in which
  // case error() says what and where, and the store is left empty rather than
  // half-loaded.
  //
  // A preset whose format/space/depth/resize token this build does not
  // recognise does NOT fail the load: it is skipped, described in problems(),
  // and the rest load. See this header's cross-build section for why the two
  // cases differ, and for the cost of the skip.
  bool loadFromString(std::string_view json, std::string_view sourceLabel);

  // The JSON form of the current contents, ready to write. Stable field
  // order, one preset per line-group, so the file is diffable by hand.
  std::string serialize() const;

  // Reads `path`. **A file that does not exist is not an error** -- it is a
  // user who has not saved a preset yet -- and yields an empty store with a
  // true return. A file that exists and cannot be read or parsed returns
  // false with error() set.
  bool loadFromFile(const std::string& path);

  // Writes serialize() to `path`, creating the parent directory if needed.
  // Returns false with `*errorOut` naming the path on any filesystem failure.
  bool saveToFile(const std::string& path, std::string* errorOut) const;

  // Adds `preset`, or replaces the existing one with the same name. Name
  // comparison is **case-insensitive**: a user who has "Web" does not want a
  // separate "web", and a list containing both is a support question rather
  // than a feature. The stored name keeps the caller's own capitalisation.
  //
  // Returns false with `*errorOut` for a name that is empty or whitespace
  // only, longer than kMaxPresetNameLength, or contains a control character
  // -- all three of which produce a preset a user cannot reliably pick out of
  // a menu again.
  bool savePreset(const ExportPreset& preset, std::string* errorOut);

  // Removes the preset with this name (case-insensitively). Returns false if
  // there was none.
  bool removePreset(std::string_view name);

  // The preset with this name (case-insensitively), or nullptr.
  const ExportPreset* find(std::string_view name) const;

  const std::vector<ExportPreset>& presets() const { return presets_; }
  // Human-readable descriptions of presets that were skipped at load, one
  // per skipped preset, each naming the preset and the token that was not
  // recognised. Empty for a clean load.
  const std::vector<std::string>& problems() const { return problems_; }
  // Why the last loadFromString()/loadFromFile() returned false. Empty
  // otherwise.
  const std::string& error() const { return error_; }

  static constexpr std::size_t kMaxPresetNameLength = 64;

 private:
  std::vector<ExportPreset> presets_;
  std::vector<std::string> problems_;
  std::string error_;
};

// Where ExportPresetStore's file lives for this user. See this header's
// "where presets live" section for the reasoning and the
// `$NP_EXPORT_PRESETS` override. Pure string construction -- touches no
// filesystem and creates nothing.
std::string defaultExportPresetsPath();

// The stable ASCII tokens the preset file stores, and their inverses. Kept
// separate from the human-readable *Name() functions in io/Capabilities.hpp
// and io/Export.hpp on purpose: those exist to be read by a person in a
// dialog and are free to be reworded, while these are a file format and
// changing one silently invalidates every saved preset. Two vocabularies
// because they have two different compatibility rules, not by oversight.
const char* exportFormatToken(ImageFormat format);
const char* exportTargetSpaceToken(ExportTargetSpace space);
const char* exportBitDepthToken(ExportBitDepth depth);
const char* exportResizeModeToken(ExportResizeMode mode);
bool exportFormatFromToken(std::string_view token, ImageFormat* out);
bool exportTargetSpaceFromToken(std::string_view token, ExportTargetSpace* out);
bool exportBitDepthFromToken(std::string_view token, ExportBitDepth* out);
bool exportResizeModeFromToken(std::string_view token, ExportResizeMode* out);

// The whole Export As operation, composed from parts that already existed:
//
//   flattenDocumentToLinear (io/Export) -> resolveExportSize ->
//   resampleAreaAverage in linear light (ops/Resample) -> encodeLinearImage
//   (io/Export)
//
// The resize sits between the flatten and the encode, which is what puts it
// in linear light structurally rather than by convention -- see
// ops/Resample.hpp. Returns io/Export's own ExportResult, and any refusal
// (from the resize or the encode) arrives as that type's `error` verbatim.
ExportResult exportDocumentWithRequest(const Document& doc, const ExportRequest& request);

// exportDocumentWithRequest() plus an fwrite, with exportDocumentToFile()'s
// exact guarantee: nothing is opened until the bytes exist in full, so a
// refused request never leaves a truncated file behind.
bool exportDocumentWithRequestToFile(const Document& doc, const std::string& path,
                                     const ExportRequest& request, std::string* errorOut);

}  // namespace np
