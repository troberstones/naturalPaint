#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "io/Capabilities.hpp"
#include "io/ExportAs.hpp"

// io/ExportStates (PLAN.md "Phase 5 -- Stack it", step 13: "**Export comps to
// files, and layers to files** -- one shared loop: set a document state,
// composite, write through phase 4's Export As presets with a name template.
// Both are the same mechanism, so building them apart would mean building it
// twice"). PRD I16 and I17.
//
// PRD I16 (P1): "Export layers to individual files."
// PRD I17 (P1): "**Export layer comps to files** -- one file per comp, with a
// name template, a choice of which comps, and I15's format/space/depth/resize
// options."
//
// PRD's own note under those two rows is the whole design brief: "**I16 and
// I17 are one feature, not two.** Both are the same loop: set a document
// state, composite, write with a naming rule."
//
// (The brief this step was handed cited I18 for the name template. I18 is
// "Revert, duplicate document, save a copy, save incremental, open recent" --
// phase 4 step 8's work, already landed. The name template is **I17**, which
// is the row that contains the words "with a name template". Recorded here
// rather than silently corrected, because a wrong citation is how a
// requirement gets built twice or not at all.)
//
// ==========================================================================
// (1) The loop, and what makes it one loop rather than two
// ==========================================================================
//
// Everything this header exports runs through `exportDocumentStates()`, whose
// body is a single `for` over a plan. Comps and layers differ in exactly two
// places inside it, both one line long: which list the plan enumerates, and
// which mutation `applyState()` performs on the scratch document. Every other
// concern -- the name template, collision detection, the pre-flight refusals,
// the composite, the encode, the write, the per-file report, the restoration
// of the caller's document -- is written once and shared.
//
// That unification reaches the vocabulary as well, and it is worth naming
// because it is the load-bearing consequence: **there is one `{name}` token,
// not a `{comp}` and a `{layer}`.** A template written for comps works
// unchanged on layers. Two tokens would have been two mechanisms wearing one
// function's name.
//
// **A "document state" is a set of the four properties `core::LayerComp`
// captures** -- visible, opacity, blend, clipped -- and nothing else. A comp
// is a stored one; a layer isolation is a computed one. Both are applied to
// the same scratch, both are undone by the same reset.
//
// ==========================================================================
// (2) The caller's document is `const`, and that is the whole answer to
//     "is it restored?"
// ==========================================================================
//
// Exporting four comps has to put the document into four different states.
// The obvious implementation mutates the live document four times and puts it
// back, and then owes the reader a proof that "puts it back" is exact --
// including that four junk entries did not land in the undo history, that the
// revision counter did not move, and that a failure part-way through did not
// leave the user looking at comp 3.
//
// None of that proof is needed here, because **nothing in this header can
// mutate the caller's document**: every entry point takes `const Document&`
// and the loop works on a copy. `core/LayerComp.hpp` §3 predicted exactly
// this shape before comps existed -- "an exporter loops comps, restores each
// into a scratch copy of the document" -- and it is right for a reason
// stronger than convenience:
//
//   * **A batch export is not an edit.** It produces files. `recordLayerEdit()`
//     bumps `OpenDocument::revision`, appends a `core::History` entry and
//     tells app/Journal a structural change happened; all three are wrong for
//     an operation whose entire output is on disk. Routing the loop through
//     it would make "export four comps" cost four undo steps to get back from
//     and mark a clean document dirty. So **this module never calls
//     `recordLayerEdit()`, and could not: it has no `OpenDocument`.**
//   * **Restoration becomes structural rather than careful.** There is no
//     "put it back" step that can be wrong, no failure path that can skip it,
//     and no ordering hazard between the restore and an early `return`.
//
// The copy is cheap and it is cheap for a documented reason rather than by
// luck: `core::TileStoreOf` holds `std::unordered_map<TileCoord,
// std::shared_ptr<T>>` (core/TileStore.hpp), so copying a `Document` copies
// map nodes and bumps refcounts -- it does not copy a pixel. And the loop
// writes *only* the four appearance properties, never a tile, so the
// copy-on-write those shared pointers exist for never fires either.
// `--selftest` measures the copy against the composite it feeds.
//
// One copy is made, before the loop, and reset to the original's four
// properties before each item. The reset is a direct member assignment rather
// than a call through `core/LayerOps`' setters, and the asymmetry with the
// apply below is deliberate: putting a throwaway scratch back to values it
// already held is not an edit that a lock has any opinion about.
//
// ==========================================================================
// (3) What a comp file contains: exactly what clicking that comp would show
// ==========================================================================
//
// The state-set for a comp is `restoreLayerComp()` -- the same call
// `app/CompPanel`'s row makes, not a second implementation of it. That has
// one consequence that had to be chosen rather than inherited: **a restore
// honours the layer lock** (core/LayerCompOps.hpp), so a locked layer keeps
// its current opacity, blend and clip even when the comp names different
// ones.
//
// Clearing the locks on the scratch first was available and is rejected. It
// would produce a file matching what the comp *says*, which is a picture the
// user cannot get on screen -- clicking that comp in the panel leaves the
// locked layer alone. The property worth having is the one a user can check:
//
//   **The file exported for comp *i* is byte-identical to what File > Export
//   As produces after clicking comp *i* in the comps panel.**
//
// `--selftest` asserts that equality directly, on bytes, rather than trusting
// it. The locked layers are named in the item's `warnings` through
// `layerCompRestoreSummary()`, so the divergence from the comp's stored
// values is reported rather than silent.
//
// A restore that is refused outright -- an unreadable comp carried from a
// newer build, two layers sharing an id, a comp none of whose layers are
// still present -- makes that item a `Skipped` with `restoreLayerComp()`'s own
// error as its reason. Nothing was written for it, so it is not a `Failed`.
//
// ==========================================================================
// (4) What a layer file contains: the layer alone, on transparency
// ==========================================================================
//
// PRD I16 says "Export layers to individual files", and two readings of that
// are defensible. Photoshop ships both.
//
//   * **The layer alone on transparency** -- built. The state is "this layer
//     visible, every other hidden".
//   * **The layer composited over everything beneath it** -- not built. The
//     state would be "layers 0..i visible", which in this loop is *one line*:
//     `l.visible = (j <= index)` in place of `l.visible = (j == index)`. That
//     is the honest cost of the alternative, stated so it is a decision
//     rather than an omission.
//
// Alone-on-transparency wins on two grounds. It is the only reading under
// which the *set* of files is a decomposition of the document rather than N
// nested copies of it -- file 7 of 7 in the other reading is the whole
// picture, and files 1..6 are prefixes of it, so six of the seven files are
// something no layer panel row corresponds to. And it is the reading that
// matches what "individual" means in I16's own sentence.
//
// Opacity, blend and clip are **left exactly as authored**, not reset:
//
//   * opacity and blend, because the file is then what that layer contributes,
//     and resetting them would be a second policy invented here about what a
//     layer "really" looks like.
//   * clip is the exception, and it has to be. `core/Composite`'s clip runs
//     key off the `clipped` flag and clip a run to the alpha of the layer
//     below -- which this state has just hidden, so its alpha is 0 and the
//     isolated layer composites to **nothing at all**. A silently empty file
//     is the worst available outcome, so an isolated clipped layer is
//     **un-clipped on the scratch and the item warns by name**. The warning
//     is what keeps it from being the silent rewrite it would otherwise be.
//
// **An Adjustment layer is refused, by name.** It holds no pixels -- its op
// stack transforms what is accumulated beneath it (core/Composite.hpp §§8-11)
// -- so isolating one composites to a fully transparent canvas. Writing that
// file would be reporting success for an image with nothing in it. The same
// test refuses any layer kind with no pixel storage in this build (Media,
// Strokes, Text, Flats have no `rgbTiles` and no `pigmentTiles`), so the rule
// is "a kind that cannot hold a pixel here", checked, rather than one enum
// value special-cased.
//
// ==========================================================================
// (5) The name template (PRD I17)
// ==========================================================================
//
// Three tokens, and the exclusions matter as much as the inclusions:
//
//   `{name}`   the comp's or the layer's name. **One token, not two** -- see
//              §1. This is the token a template is written for.
//   `{doc}`    the document's name, supplied by the caller
//              (`ExportStatesRequest::documentName`). io/ does not reach into
//              app/DocumentLifecycle for it; a string argument keeps this
//              module callable from `--selftest` with no session at all.
//   `{index}`  the item's 1-based ordinal **within the selection**, zero-
//              padded to two digits. Assigned at plan time, before any skip
//              decision, so a comp that turns out to be unexportable does not
//              renumber the ones after it -- re-running a batch whose third
//              item now fails must not rename the fourth item's file.
//
// **No date token**, and this is a decision rather than an oversight. A date
// makes the same batch produce different filenames on two runs: the user
// cannot predict the output, a second run silently doubles the file count
// instead of overwriting, and neither `--selftest` nor the dialog's own
// preview can state what will be written. PRD I17 asks for "a name template",
// not for a timestamped one. When a date is genuinely wanted it belongs in the
// output *directory*, which the user already chooses.
//
// **The extension is not a token, it is derived** from the request's format
// (`imageFormatExtension()`). A template able to write `.png` while the
// preset says JPEG is a template able to produce a lying filename, and this
// codebase refuses to be the place a file's name and its contents disagree.
//
// **A token whose value is absent renders as nothing**, and the refusal is
// deferred to §6's check on the resolved name. So `{doc}` on a document that
// has never been saved contributes an empty string, and `{doc}_{name}` gives
// `_Sky.png` -- visible in the plan, which the dialog shows before anything is
// written. The two alternatives are worse in a way that is not visible:
// refusing an absent `{doc}` outright stops an unsaved document using a
// template where the document name does not matter, and substituting
// "untitled" makes two different unsaved documents resolve to the same
// filenames -- a collision the user cannot see the cause of.
//
// ~~**One adjacent gap, stated rather than left to be discovered.**~~
// **Closed at PLAN.md Phase 5 step 15.** `exportDocumentWithRequest()` (phase
// 4 step 7) called the one-argument `flattenDocumentToLinear()`, so
// `ExportResult::warnings` from it was always empty and core/Composite's blend
// approximations never reached an item's `warnings` -- this module's own item
// warnings, the comp restore summary and the un-clip note, were all a batch
// ever reported. It now calls the two-argument overload and carries what it
// produces, on a refusal as well as a success, so an item exported from a
// document holding a blend this build approximates says so.
//
// ==========================================================================
// (6) A template names a FILE. Every path hazard follows from that
// ==========================================================================
//
// The output *directory* is a separate field, and the template resolves to a
// single filename component. So `../../etc/passwd` is not "sanitised" -- it is
// unreachable, because `/` is refused wherever it appears:
//
//   in the template's literal text  -> the whole batch is refused, once, with
//     one clear message. The user typed it; the directory field is where a
//     directory goes.
//   in a substituted value          -> that item is skipped and named. A layer
//     called "sky/clouds" is an ordinary layer name, and refusing an entire
//     four-file batch over one of them would be disproportionate.
//
// Refused in a resolved filename, each with the reason it is refused for:
//
//   `/` and `\`   path separators. Never rewritten to `_`: two layers named
//                 "a/b" and "a_b" would then collide, and the user would be
//                 told nothing about either.
//   `:`           legal in a POSIX filename and **displayed by the macOS
//                 Finder as `/`**, a swap inherited from HFS. A file the user
//                 cannot read the name of in their own file browser is a file
//                 they cannot find. This project's primary platform is macOS.
//   control characters and NUL   `ExportPresetStore`'s rule and reasoning,
//                 one level down: a name a user cannot reliably pick out of a
//                 list again.
//   a leading `.` a hidden file on every platform this ships on. The user
//                 asked for a file, and got one they will not see.
//   `.` and `..`  reserved directory entries; there is no file with either
//                 name to create.
//   empty         a substitution that leaves the stem empty produces a file
//                 called `.png`, which is both hidden and a collision magnet
//                 (every empty-named comp resolves to the same one). Comp
//                 names are explicitly allowed to be empty
//                 (core/LayerCompOps.hpp), so this is a state a user can
//                 reach, and it is skipped and named rather than refused
//                 globally.
//   over 255 bytes  `NAME_MAX` on APFS, HFS+, ext4 and NTFS alike. The
//                 refusal quotes the resolved length and the limit, so a
//                 300-character layer name produces a number, not "too long".
//
// ==========================================================================
// (7) Collisions are refused before the first byte, case-insensitively
// ==========================================================================
//
// Two comps may share a name -- core/LayerCompOps.hpp says so explicitly, and
// two layers may as well. So two items resolving to one filename is an
// ordinary state, not a corrupt one, and it has to be answered.
//
// **Refused, with both items named, before anything is written.** The
// alternative -- disambiguating with a suffix -- was rejected because it makes
// the mapping from comp to file depend on enumeration order: insert a comp and
// every "-2" moves to a different picture, silently, in a folder the user has
// already handed to someone else. A refusal costs one rename and is decided in
// full before a byte is committed, which is much easier to reason about than a
// half-finished batch (and is what PRD P4's "never partially overwrites an
// input" asks for in the automation phase).
//
// **The comparison is case-insensitive**, which is not fussiness: APFS and
// NTFS are case-insensitive by default, so "Sky.png" and "sky.png" are one
// file on this project's own primary platform and the second write would
// silently destroy the first. `ExportPresetStore::savePreset()` already
// compares preset names case-insensitively for the same class of reason. Both
// spellings are quoted in the refusal, because "Sky and sky collide" is
// otherwise an unreadable message.
//
// **An output path that already exists is refused** in the same pre-flight,
// unless `overwriteExisting` is set. That is PRD P4's "never partially
// overwrites an input" delivered without this module needing to know which
// file the input is: the document's own `.npaint` exists, so a template
// resolving onto it is refused by the general rule.
//
// ==========================================================================
// (8) Partial failure: stop, and report per file
// ==========================================================================
//
// PRD's own acceptance row settles this one rather than leaving it to taste:
//
//   | batch run failing on file 12 of 40 | files 13-40 untouched, failure
//     reported | P4 |
//
// So the loop **stops at the first write failure**. Items after it are
// reported as `NotAttempted` with the failing item named as the reason, and
// they are genuinely untouched. Continuing would usually mean N more failures
// with the same message -- a write failure at item 3 is nearly always the disk
// or the directory, not item 3 -- and the user waits longer for the same news.
//
// Every item carries its own outcome and its own reason, and the four
// outcomes are kept distinct because collapsing them is exactly how "exported
// 7 files" gets printed when 6 landed:
//
//   Written       the bytes are on disk, and `bytesWritten` says how many.
//   Skipped       nothing reached the filesystem for it, and `reason` says
//                 why. Three things land here: an item the plan already ruled
//                 out (an Adjustment layer, an unresolvable name), a comp
//                 whose restore was refused, and a composite this format
//                 refuses (JPEG names a translucent one). All three are
//                 properties of *this item*, so the batch carries on.
//   Failed        a write was attempted and did not land. At most one per run,
//                 because a write failure is nearly always the disk or the
//                 directory rather than the file, so the run stops.
//   NotAttempted  an earlier item failed first. Genuinely untouched.
//
// `ExportStatesReport::ok` means one thing in both a plan and a run: **the
// pre-flight passed and nothing failed to write.** Skips do not make a run
// fail -- they are the reported, expected outcome for an Adjustment layer --
// but `skipped()` is there for a caller that wants to insist on none.
namespace np {

// Which list the loop enumerates. The only enum in this header, and the only
// thing about a run that differs between PRD I16 and I17.
enum class ExportStateSource {
  // `Document::comps`, in panel order (PRD I17).
  Comps,
  // `Document::layers`, in the document's own bottom-to-top order (PRD I16).
  Layers,
};

// "comp"/"layer" and "comps"/"layers" -- used to build refusal strings that
// read as the thing the user asked for rather than as "state".
const char* exportStateSourceNoun(ExportStateSource source);
const char* exportStateSourcePlural(ExportStateSource source);

// One batch. `format` is phase 4 step 7's `ExportRequest` verbatim -- the
// exact value an `ExportPreset` stores -- so "write through phase 4's Export
// As presets" is a field assignment, not an adapter.
struct ExportStatesRequest {
  ExportStateSource source = ExportStateSource::Comps;

  // Format, target space, bit depth and resize (PRD I15, I17's "I15's
  // format/space/depth/resize options"). Assign `preset.request` here.
  ExportRequest format;

  // Where the files go. Must already exist: creating directories on a user's
  // behalf during an export is a side effect they did not ask for, and the
  // refusal names the path.
  std::string outputDirectory;

  // §5's tokens plus literal text. The extension is appended from
  // `format.format`; do not put one here.
  std::string nameTemplate = "{name}";

  // Fills `{doc}`. Empty is legal -- a document that has never been saved has
  // no name -- and a template that then resolves to an empty stem is skipped
  // by name (§6), which is why the default template does not use this token.
  std::string documentName;

  // PRD I17's "a choice of which comps": indices into `doc.comps` or
  // `doc.layers`. **Empty means all of them**, which is the common case and
  // keeps a caller with no picker from having to build the identity vector.
  // An out-of-range index refuses the batch, naming the index and the count.
  // Duplicates are refused by the ordinary collision rule.
  std::vector<size_t> selection;

  // Off by default (§7). On, an existing output path is overwritten -- still
  // never partially: `exportDocumentWithRequestToFile()` encodes to memory in
  // full before it opens anything.
  bool overwriteExisting = false;
};

// What happened to one file. §8 defines all four.
enum class ExportItemOutcome {
  Written,
  Skipped,
  Failed,
  NotAttempted,
};

const char* exportItemOutcomeName(ExportItemOutcome outcome);

// One row of the per-file report. Also one row of the plan: `planStateExport()`
// fills everything except `bytesWritten`, and the outcomes it can already know
// (`Skipped`, or `NotAttempted` standing for "would be attempted").
struct ExportStateItem {
  // Index into `doc.comps` or `doc.layers`.
  size_t sourceIndex = 0;
  // 1-based position within the selection; what `{index}` rendered.
  size_t ordinal = 0;
  // The comp's or layer's name at plan time.
  std::string stateName;
  // Resolved filename including the extension. Empty when the name could not
  // be resolved, in which case `reason` says why and the item is `Skipped`.
  std::string filename;
  // `outputDirectory` joined to `filename`. Empty exactly when `filename` is.
  std::string path;

  ExportItemOutcome outcome = ExportItemOutcome::NotAttempted;
  // Why, for `Skipped`, `Failed` and `NotAttempted`. Empty for `Written`.
  std::string reason;
  // Bytes on disk. Nonzero only for `Written`.
  size_t bytesWritten = 0;
  // Non-fatal notes about *this file*: the composite's blend warnings, a
  // partial comp restore, an un-clipped isolated layer. Per item and not per
  // report because they genuinely differ per item -- a layer hidden by one
  // comp warns about nothing.
  std::vector<std::string> warnings;
};

// The result of a run, and (from `planStateExport()`) of a dry run.
struct ExportStatesReport {
  // True when `error` is empty and no item is `Failed`. See §8.
  bool ok = false;

  // A refusal that applies to the whole batch. **Non-empty means nothing was
  // written**, and `items` is empty or holds the plan as far as it got.
  std::string error;

  // One per selected item, in selection order.
  std::vector<ExportStateItem> items;

  size_t count(ExportItemOutcome outcome) const noexcept;
  size_t written() const noexcept { return count(ExportItemOutcome::Written); }
  size_t skipped() const noexcept { return count(ExportItemOutcome::Skipped); }
  size_t failed() const noexcept { return count(ExportItemOutcome::Failed); }
  size_t notAttempted() const noexcept { return count(ExportItemOutcome::NotAttempted); }
};

// The tokens §5 defines, each with its braces, in the order a help line should
// list them.
std::vector<std::string> exportNameTemplateTokens();

// One line naming every token, for a dialog's hint text and for the refusal an
// unknown token produces. There is one list, so the two cannot disagree.
std::string exportNameTemplateHelp();

// Renders one filename, extension included.
//
// Public because three callers need the identical answer: the plan, the
// dialog's preview, and `--selftest`. `ordinal` is 1-based.
//
// Returns false with `*errorOut` for anything §6 refuses. The distinction
// between a template-literal refusal and a substitution refusal is made by
// `planStateExport()`, not here -- this function answers "is this resolved
// name usable", which is the question both cases reduce to.
bool resolveExportStateName(const std::string& nameTemplate, const std::string& documentName,
                            const std::string& stateName, size_t ordinal, ImageFormat format,
                            std::string* outFilename, std::string* errorOut);

// Checks the template itself, once per batch: balanced braces, no unknown
// token, no path separator or colon in the *literal* text. Returns false with
// `*errorOut`. §6 explains why this is separate from the per-item check.
bool validateExportNameTemplate(const std::string& nameTemplate, std::string* errorOut);

// Everything decided before the first byte: the selection, the availability of
// the request in this build, the output directory, every resolved filename,
// every collision, and every existing path.
//
// **Writes nothing and touches no pixel.** A dialog calls this to show the
// user the exact list of files a click would produce, and `--selftest` calls
// it to assert filenames without a filesystem write.
//
// Items that will be exported come back as `NotAttempted`; items already known
// to be unexportable come back as `Skipped` with their reason.
ExportStatesReport planStateExport(const Document& doc, const ExportStatesRequest& request);

// `planStateExport()`, then the loop of §1: reset, apply, composite, encode,
// write. Stops at the first `Failed` (§8).
//
// Never mutates `doc`; see §2.
ExportStatesReport exportDocumentStates(const Document& doc, const ExportStatesRequest& request);

// The one sentence a status line shows, in io/Export's refusal style -- name
// the numbers, say what did not happen. Never "exported 7 files" when 6
// landed.
std::string exportStatesSummary(const ExportStatesReport& report);

}  // namespace np
