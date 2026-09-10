#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "app/Action.hpp"
#include "io/ExportAs.hpp"
#include "io/ExportStates.hpp"

// app/Batch -- one action over many files, and the pre-flight that makes PRD
// P4 a property of this module rather than a promise about it
// (docs/automation-plan.md step 6; PLAN.md "Phase 19 -- Automate it").
//
// The loop itself is three calls, and every one of them already existed:
//
//     openAnyFileAsDocument()  ->  replayAction()  ->  exportDocumentWithRequest()
//
// so almost nothing below is about processing a file. It is about the two
// answers a batch has to get right that an interactive edit never has to give:
// **which files may be written**, and **what a run does when one of them goes
// wrong**. Both are argued at length here because both are silent when they
// are wrong.
//
// ==========================================================================
// (1) The pre-flight, which is the only reason this module exists
// ==========================================================================
//
// `std::fopen(path, "wb")` **truncates before the first byte is written.**
// io/Export encodes a whole file to memory before it opens anything, so a
// failed *encode* leaves nothing behind -- but that guarantee says nothing
// about an output path that happens to name an input. The moment such a path
// is opened the input is gone, encode success or not, and the action that was
// meant to grade thirty plates has destroyed one of them.
//
// So this module refuses **the whole run**, before a single file is opened,
// when any output path resolves to any input path. Not "skips that file": if
// the output rule can reach the input set at all, the rule is wrong, and the
// user is one rename away from a run that works. That refusal is
// docs/automation-plan.md §2's entire argument against letting a user wire
// their own save node -- "that pre-flight is unprovable" once open and save
// are somebody else's nodes -- so it is the check that has to be true.
//
// **Spelling is not identity, and this is where the check earns its keep.**
// `./a.exr` and `a.exr` are one file; so are `out/../a.exr` and `a.exr`,
// `out/` and `out`, a symlink and its target, and -- on this project's own
// primary filesystem -- `A.exr` and `a.exr`. String equality agrees with none
// of them. `pathsNameTheSameFile()` below therefore has two branches, and
// they are disjoint by construction so that neither can quietly cover for the
// other going wrong:
//
//   * **Both paths exist** -> `std::filesystem::equivalent()`, which asks the
//     filesystem rather than guessing. It settles case-insensitivity, Unicode
//     normalisation (APFS compares NFC and NFD as one name), symlinks, hard
//     links -- two names, one inode, and no symlink to follow -- and every
//     lexical difference at once, because it compares the file rather than
//     the name. This is the branch that fires for the case that matters: an
//     output path naming an input, where the input necessarily exists.
//   * **Otherwise** -> `std::filesystem::weakly_canonical()` on both sides
//     and an ASCII-case-insensitive compare of the results.
//
// **What `weakly_canonical()` does here, measured rather than assumed.** It
// makes the path absolute against the current directory, removes `.` and `..`
// segments lexically, drops a trailing slash, and resolves symlinks over the
// prefix that exists. It does **not** fold case, and it does not normalise
// Unicode. So the second branch covers: relative-vs-absolute, `.`, `..`, a
// trailing slash, a symlinked directory that exists, and (through the added
// fold) an ASCII case difference.
//
// **What is not covered, stated as a limitation rather than papered over.**
// The second branch cannot see a hard link, a non-ASCII case difference
// (`É` / `é`), or an NFC/NFD difference -- all three are one file on APFS and
// two different canonical strings. That branch is only reached when at least
// one of the two paths does not exist, and a path that does not exist is not
// an input anyone can lose, so the gap costs a *spurious* run rather than a
// destroyed file. The branch that guards real data is the first one, and it
// has none of those gaps because it does not compare names at all.
//
// Two outputs resolving to one path are refused by the same pre-flight, for
// io/ExportStates §7's reason exactly: disambiguating with a suffix makes the
// mapping from source to file depend on enumeration order.
//
// ==========================================================================
// (2) Resolution-dependent parameters, and why v1 compares sources to each
//     other rather than to the action
// ==========================================================================
//
// `filter_gaussian_blur`'s sigma is in document texels. A sigma authored on a
// 2k plate is a different filter on an 8k one, and the batch that applies it
// to both reports thirty successes and writes fifteen wrong files.
//
// The fix docs/automation-plan.md §5 asks for is that a step store the unit
// **and the resolution it was authored at**. The first half exists implicitly
// -- the parameter names are documented in texels -- and the second half does
// not: `Action` records no size, so there is nothing here to compare an input
// against. Inventing one now would mean either re-recording every action or
// guessing.
//
// What is available is the source set, and it answers the useful half of the
// question: **if any step carries a pixel-unit parameter and the sources do
// not all share a size, the run is refused.** A batch over one resolution is
// the case this rule lets through, and it is also the case where the sigma is
// either right for all of them or wrong for all of them -- which is a
// mistake the user can see in the first output file. A mixed-resolution batch
// is the case where the output is wrong for *some* files, which is the one
// nobody notices.
//
// **Is that too strict? It is, and deliberately.** A mixed batch of a
// pixel-unit action is sometimes exactly what someone means -- "blur
// everything by two texels, whatever size it is" is a legitimate instruction,
// and this rule refuses it. The alternative was to warn and continue, and it
// was rejected because a warning on a thirty-file run is a line in a report
// nobody reads next to twenty-nine successes. A refusal costs one flag the
// user does not have yet; a warning costs files that look right. When the
// authored resolution lands in the file format, this rule should become
// "scale the parameter", and this comment is the note saying so.
//
// **`image_size` and `canvas_size` are not pixel-unit, and the distinction is
// the whole rule.** Their width and height are absolute *destinations* --
// "resize to 512x512" means the same thing whatever the source was, and
// docs/automation-plan.md §8 names replaying it across three differently-sized
// documents as an acceptance case. A pixel-unit parameter is one whose
// meaning is relative to the source's resolution, not one whose value happens
// to be counted in pixels.
//
// **The cost, stated because it is paid at run time.** Knowing the sources'
// sizes means decoding them, and the pre-flight has to finish before the
// first byte is written, so an action carrying a pixel-unit parameter decodes
// every source **twice**: once in the sizing pass, once in the loop. Nothing
// else in a batch does. An action with no such parameter -- which is most of
// them -- pays nothing, because the pass does not run.
//
// The pixel-unit table lives in the .cpp, keyed by command id and parameter
// name, because putting a `unit` field on `CommandSpec` would mean editing
// the four `app/Commands*.cpp` files that several branches hold at once
// (app/Command.hpp says so in its own "where the rows come from" section).
// `--selftest` holds the table to the registry in both directions: every
// entry must name a command that exists and a parameter that command
// advertises, and every command advertising one of the pixel-unit *names*
// must be in the table. So a renamed parameter and a new command reusing
// `radius` both fail the suite rather than quietly widening the batch.
//
// ==========================================================================
// (3) Four outcomes, and which one an input that will not open gets
// ==========================================================================
//
// The outcomes are io/ExportStates' four, reused rather than restated --
// `ExportItemOutcome` and `exportItemOutcomeName()` are that header's, and a
// second four-valued enum meaning the same four things is how a report grows
// two vocabularies for one idea.
//
// Where a batch has to make its own decision is which failures stop the run:
//
//   Written       the bytes are on disk.
//   Skipped       nothing reached the filesystem, and the reason is a
//                 property of **this file**: it could not be opened or
//                 decoded, or the encoder refused this particular composite
//                 (JPEG names a translucent one). The run carries on -- one
//                 SVG among thirty EXRs is not a reason to abandon the other
//                 twenty-nine, and io/ExportStates §8 makes the same call for
//                 the same reason.
//   Failed        the run cannot be trusted to continue. Two things land
//                 here: a write that did not land, and a replay that refused.
//   NotAttempted  an earlier item failed first. Genuinely untouched.
//
// **A replay refusal stops the run, and that is the one judgement call in
// this file.** A refusal is a statement about the *action* -- a layer it
// names is missing, a precondition it needs is not met -- and an action is
// one artefact applied to a set the user chose as a set. Twenty-nine more
// attempts produce twenty-nine more identical refusals, no files, and the
// same news later; io/ExportStates §8's own words for the write case ("the
// user waits longer for the same news") transfer exactly. The alternative --
// skip the file, carry on -- is defensible and costs one line (the
// `haltReason` assignment in the replay branch of `runBatch()`); it is not
// taken because a batch that reports "27 written, 3 refused" reads as a
// success with footnotes, and an action that does not fit its source set is
// not a success.
//
// **An input that will not open is a Skip and never a Failure**, which is the
// other half of the same argument: that refusal is a statement about the
// file, has already been proven to be about the file, and says nothing about
// the twenty-nine after it.
//
// ==========================================================================
// (4) A silent no-op is the failure mode this feature is built to have
// ==========================================================================
//
// docs/automation-plan.md §7, in its own words. `applyImageSize()` records no
// history for a resize to the size the document already is; a filter on a
// non-RGB layer is refused by `pixelOpRefusalFor()`. Interactively those are
// a greyed menu item and a nothing-happens. Here they are thirty files
// written **unmodified** and counted as successes.
//
// `replayAction()` already produces a warning per zero-texel step (app/Replay
// §3), and those warnings are carried into `BatchItem::warnings` verbatim.
// This module adds the summary half, because a per-file warning in a
// thirty-row report is not conspicuous: `BatchItem::texelsChanged` is the sum
// over the run's steps, `BatchItem::unchangedByAction` is true when steps ran
// and that sum is zero, and `batchSummary()` **names the count of unchanged
// files in the headline sentence** -- so "30 files written" can never be
// printed over thirty files the action did nothing to.
//
// A file with `unchangedByAction` set is still `Written`, and deliberately: a
// format conversion is a legitimate batch with no action at all, and an
// action whose steps are all structural changes no texel records is a real
// thing too. The number is reported, not enforced.
//
// ==========================================================================
// (5) Import warnings reach the report, because that is where they are read
// ==========================================================================
//
// `OpenAnyResult::warnings` carries a `.npaint` written by a newer build, a
// part that could not become a layer, a PSD's unmapped blend modes. In the
// application those reach a status line. In a batch there is no status line,
// and a file that imported lossily and then exported perfectly is a
// success -- of the wrong picture. Every one of them becomes a
// `BatchItem::warnings` entry prefixed `import:`, beside the replay's
// `action:` entries and the encoder's `export:` ones, so the three sources
// stay distinguishable in one list.
//
// ==========================================================================
// (6) What this module does not own
// ==========================================================================
//
// No UI. `BatchRequest` and `planBatch()` are what the BATCH dialog is built
// against (docs/automation-plan.md step 7), which is why they are in a header
// that pulls in `app/Action`, `io/ExportAs` and `io/ExportStates` and nothing
// from `main.cpp`.
//
// And one thing deliberately half-done, recorded rather than left to be
// found: io/ExportAs now exposes `writeEncodedExportToFile()`, the "open
// nothing until the bytes exist in full" writer, because this is the third
// consumer and io/ExportStates.cpp's inline copy already says in as many
// words that "a third is when the writer should be hoisted".
// `exportDocumentWithRequestToFile()` was ported to it in the same change.
// io/ExportStates.cpp's copy was **not**, and only because that file is held
// by other branches in this wave; porting it is one four-line edit and its
// own comment is the note that it is owed.
namespace np {

// One batch run.
//
// Deliberately holds the `Action` by value rather than a path: the dialog
// edits an action in memory and `--batch` loads one from a file, and a
// request that named a file would make the first case impossible and the
// second untestable without a filesystem.
struct BatchRequest {
  Action action;

  // The input files, in the order they will be processed. Empty is refused --
  // a batch over nothing is a click that reports success and does nothing.
  std::vector<std::string> sources;

  // Where the outputs go. Must already exist, for io/ExportStates' reason:
  // an export that silently makes folders puts files somewhere nobody looked.
  std::string outputDirectory;

  // Format, target space, bit depth and resize -- PRD I15's `ExportRequest`
  // verbatim, so a preset is a field assignment here exactly as it is in
  // io/ExportStates.
  ExportRequest format;

  // io/ExportStates' template and its three tokens, reused whole. `{name}`
  // and `{doc}` both render **the source file's stem**, and that is not a
  // duplication that slipped through: in a batch the item and the document
  // are the same thing, so the two tokens have one honest value. `{name}` is
  // the one to write; `{doc}` is kept working so a template copied from the
  // export-states dialog does not silently resolve to nothing.
  //
  // There is no `{action}` token. Adding one would mean editing
  // `exportNameTemplateTokens()` and `validateExportNameTemplate()`, which
  // refuse an unknown token -- a shared file, for a token nobody has asked
  // for. The extension is appended from `format.format`; do not put one here.
  std::string nameTemplate = "{name}";
};

// One row of the per-file report, and one row of the plan.
struct BatchItem {
  // The source path exactly as the request spelled it -- not canonicalised,
  // because a report the user reads should quote what they typed.
  std::string sourcePath;
  // 1-based position in the source set; what `{index}` rendered.
  size_t ordinal = 0;
  // The source file's stem; what `{name}` and `{doc}` rendered.
  std::string sourceName;
  // Resolved output filename, extension included. Empty when the name could
  // not be resolved, in which case `reason` says why and this is a `Skipped`.
  std::string filename;
  // `outputDirectory` joined to `filename`. Empty exactly when `filename` is.
  std::string outputPath;

  ExportItemOutcome outcome = ExportItemOutcome::NotAttempted;
  // Why, for `Skipped`, `Failed` and `NotAttempted`. Empty for `Written`.
  std::string reason;
  size_t bytesWritten = 0;

  // Every non-fatal note about this file, prefixed by where it came from --
  // `import:`, `action:` or `export:`. See §5.
  std::vector<std::string> warnings;

  // How far the replay got, and what it did. `stepsRun` is the number of
  // steps *attempted*, so it is less than the action's length exactly when
  // the replay refused.
  size_t stepsRun = 0;
  size_t texelsChanged = 0;
  // §4: steps ran and none of them changed a texel. The file is still
  // written; this is what stops it being counted as a plain success.
  bool unchangedByAction = false;
};

struct BatchReport {
  // True when `error` is empty and no item is `Failed`. A `Skipped` item does
  // not make a run fail -- see §3 -- but `skipped()` is there for a caller
  // that wants to insist on none.
  bool ok = false;

  // A refusal that applies to the **whole run**. Non-empty means nothing was
  // opened and nothing was written, and `items` holds the plan as far as it
  // got. Every §1 and §2 refusal arrives here.
  std::string error;

  std::vector<BatchItem> items;

  size_t count(ExportItemOutcome outcome) const noexcept;
  size_t written() const noexcept { return count(ExportItemOutcome::Written); }
  size_t skipped() const noexcept { return count(ExportItemOutcome::Skipped); }
  size_t failed() const noexcept { return count(ExportItemOutcome::Failed); }
  size_t notAttempted() const noexcept { return count(ExportItemOutcome::NotAttempted); }
  // §4. Counted over `Written` items only -- an item that was never written
  // did not write an unmodified file.
  size_t unchanged() const noexcept;
};

// Whether `a` and `b` name one file on this filesystem. §1 defines both
// branches and states exactly what each covers.
//
// Public because it is the single load-bearing predicate in this module and
// `--selftest` drives it directly with hand-built pairs -- a run that refuses
// proves the pre-flight consulted *something*, and only calling this proves
// it consulted the right thing.
bool pathsNameTheSameFile(const std::string& a, const std::string& b);

// The parameter names this build treats as resolution-dependent (§2), sorted
// and unique. Exposed so `--selftest` can hold the .cpp's table to the command
// registry rather than to a copy of itself.
std::vector<std::string> pixelUnitParameterNames();

// The command ids this build knows to carry `paramName` as a pixel-unit
// parameter, sorted. Empty for a name that is not a pixel unit.
std::vector<std::string> commandsWithPixelUnitParameter(const std::string& paramName);

// Everything decided before the first byte: the action's steps, the template,
// the format's availability in this build, the output directory, every
// resolved filename, every output/output collision, every output/input
// collision (§1), and -- when the action carries a pixel-unit parameter --
// every source's size (§2).
//
// **Writes nothing.** It does decode every source when §2's pass runs, which
// is the one thing about it that is not free; see §2's cost note.
//
// Items that would be processed come back as `NotAttempted`; items already
// known to be unprocessable come back as `Skipped` with their reason.
BatchReport planBatch(const BatchRequest& request);

// `planBatch()`, then the loop: open, replay, encode, write -- one file at a
// time, stopping at the first `Failed` (§3).
//
// No input file is opened for writing anywhere in this translation unit, and
// no input is opened at all until `planBatch()` has returned without an
// error. That is PRD P4 as a property of the control flow rather than as a
// claim about it; `--selftest` hashes every input's bytes on both sides of a
// thirty-file run.
BatchReport runBatch(const BatchRequest& request);

// The one sentence a status line or a terminal shows. Names the numbers,
// including §4's unchanged count -- never "30 files written" over thirty
// files the action did nothing to.
std::string batchSummary(const BatchReport& report);

// --- the command line ------------------------------------------------------

// `--batch <action.npaction> <output-dir> <file...>`, plus the two optional
// flags `main.cpp` collects for it. Headless: nothing here touches SDL, a
// window or a GPU, which is what lets it sit before `SDL_Init()` and be
// driven from `--selftest`.
//
// Returns a process exit code: 0 when the report is `ok`, 1 otherwise. A run
// with skips is still 0 -- a skip is a reported, expected outcome (§3) -- and
// the per-file lines it prints name every one of them.
//
// `formatToken` and `nameTemplate` may be null, meaning "8-bit sRGB PNG at
// document size" (`ExportRequest`'s own defaults, the combination PRD I1
// guarantees every build can write) and `{name}`.
int runBatchCli(const char* actionPath, const char* outputDirectory,
                const std::vector<std::string>& sources, const char* formatToken,
                const char* nameTemplate);

}  // namespace np
