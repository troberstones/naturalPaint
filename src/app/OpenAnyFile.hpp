#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "app/DocumentLifecycle.hpp"
#include "io/FileKind.hpp"

// app/OpenAnyFile -- **one** entry point for "the user gave us a file; make it
// a document", and the rule that decides what a *dropped* file becomes.
//
// --- The gap this closes ---------------------------------------------------
//
// `File > Open...` called `openNpaintDocument()` and nothing else, so it opened
// `.npaint` and refused every picture on the user's disk -- while
// `openImageAsDocument()` sat in io/ImageIO, finished and asserted, with no
// caller in the binary outside `--selftest`. Its own header had said for two
// phases that it was "the function a future File > Open would call". This is
// that call, and the same shape of defect app/ImportImage closed one level in:
// a feature that was complete and unreachable.
//
// And there was no `SDL_EVENT_DROP_FILE` handler anywhere in `src/`, despite
// io/Export.hpp describing `placeImageAsLayer()` as written "for step 13's
// drag-and-drop". The routing rule below is the missing half of that: the
// decision, written down once, next to the code that makes it and testable
// without a window.
//
// --- Content, never extension ----------------------------------------------
//
// Which reader a file goes to is decided by `io/FileKind`'s sniff of its bytes.
// io/FileKind.hpp argues that at length; the consequence here is that a
// `.npaint` named `foo.png` opens as a document and a PNG named
// `sketch.npaint` opens as a picture, both correctly, and neither this file nor
// io/FileKind ever looks at a filename except to put it in a message.
//
// --- What an opened image is bound to, and why a Save cannot go wrong -------
//
// This is the decision with teeth in it, so it is stated in full.
//
// **A picture opened as a document is bound to no file.** `OpenDocument::path`
// stays empty and `OpenDocument::title` becomes the picture's own file name, so
// the tab reads `photo.png` (`documentDisplayName()` falls through to `title`
// when there is no path) while the record is unbound.
//
// The alternative -- binding the document to `photo.png` -- is the trap, and it
// is a two-sided one:
//
//  * `saveDocument()` writes through `io/NpaintFile::saveNpaint()`, which
//    always writes a multi-part EXR whatever the path is called. A bound
//    picture would therefore make the next Cmd-S **overwrite the user's PNG
//    with `.npaint` bytes under a `.png` name** -- their original destroyed,
//    and replaced by a file no other application will open.
//  * Rebinding to `photo.npaint` instead would be a file name the user never
//    chose, silently claiming a path that may already hold something else.
//
// With no path, `saveDocument()` refuses -- by name, saying "has never been
// saved and is not bound to a file. Use Save As to choose one." (see
// app/DocumentLifecycle.cpp) -- and Save As is the one place a destination gets
// chosen, by the user, explicitly. So the wrong bytes cannot reach the wrong
// path without someone typing that path. That is the property, and
// app/selftest/OpenAnyFile.cpp section D asserts it directly rather than
// describing it.
//
// Two things fall out of the same choice, both correct:
//
//  * `revertDocument()` refuses too, and should: reverting means "what the file
//    says", and a PNG on disk is not this document's last saved state -- it is
//    where the pixels came from once.
//  * **The document is dirty from birth**, exactly as `duplicateDocument()`'s
//    is and for the same reason: it holds a document that exists nowhere on
//    disk *as a document*. Being dirty is what puts it in front of the
//    Save / Don't Save / Cancel question on close (PRD I11) and what gets it
//    journalled for crash recovery (PRD O5). A clean-from-birth image document
//    would be closed without a word.
//
// A `.npaint` opened here is bound to its path, unchanged from before -- it
// goes through `openNpaintDocument()`, which is still the only thing that reads
// one.
//
// --- The recent list -------------------------------------------------------
//
// Only `.npaint` opens are recorded, and that is a limit rather than a policy.
// `openRecentDocument()` (app/DocumentLifecycle.cpp) still calls
// `openNpaintDocument()` directly, so a picture in that list would be an entry
// that refuses when it is clicked. Widening it cannot be done from here:
// app/OpenAnyFile depends on app/DocumentLifecycle, so app/DocumentLifecycle
// cannot call back into this module without a cycle. The honest fix is to move
// this module's dispatch *into* DocumentLifecycle, or to give `openRecent` a
// callback; both are real work and neither is this track's. Stated here so the
// next reader finds the reason rather than the symptom.
//
// --- The seam a layered PSD would come through -----------------------------
//
// "A file becomes a document" means "a file becomes a document with **one**
// layer" today, because `openImageAsDocument()` returns exactly that. A layered
// PSD would mean N layers, and the thing worth noticing now is that **nothing
// in this module knows the number is one.**
//
// `openAnyFileAsDocument()` takes a whole `Document` from the decode step and
// wraps it: it sets the id, the title, the empty path, the dirty-from-birth
// edit, and the history baseline. Every one of those is per-*document*, not
// per-layer. So a decoder that returned a `Document` with ten layers would flow
// through this function, through the drop routing, and through the status line
// with no change at all -- widening, not redesign. The one line that would move
// is the `openImageAsDocument()` call in `openAnyFileAsDocument()`.
//
// The other half is already here too: `OpenAnyResult::warnings` is a vector
// that reaches the user's status line, which is where a PSD's unmapped blend
// modes, adjustment layers and layer effects would be named -- the same
// carry-a-note-rather-than-drop-it shape `AbrImportResult::notes` already uses.
// No parameter, no abstraction and no dead code has been added to chase this;
// the natural shape already accommodates it, which is the point of saying so.
namespace np {

// What an Open did, or refused to do.
struct OpenAnyResult {
  bool ok = false;

  // One sentence for the status line. On success it names the file and what
  // kind of thing was opened; on failure it names the file and the reason.
  // **Never empty**, in either direction.
  std::string status;

  // Non-fatal things the user still has to be told -- io/NpaintFile's load
  // warnings (a newer `np:version`, a foreign `np:basis`, a part that could not
  // be turned into a layer), verbatim. Separate from `status` because a warning
  // is not a failure and must not be coloured as one.
  std::vector<std::string> warnings;

  // What the file's bytes said it was, whether or not the open succeeded.
  // `Unknown` on a refusal that never got as far as sniffing (an empty path, a
  // folder, an unreadable file).
  FileKind kind = FileKind::Unknown;

  // The opened record, when `ok`. Move it into the session; it is not added to
  // one here, because this module has no session to add it to and the caller
  // may want to place it deliberately.
  OpenDocument document;
};

// Reads `path`, decides from its **bytes** whether it is one of this
// application's documents or a picture, and produces an `OpenDocument` either
// way.
//
// A `.npaint` (an OpenEXR carrying `np:version`, whatever it is called) goes to
// `openNpaintDocument()`; a picture goes to `io/ImageIO`'s
// `openImageAsDocument()`. There is no third path and no second copy of either.
//
// **Three distinguishable refusals**, because a user can act on the difference:
//
//  1. *"not a format naturalPaint reads"* -- the bytes match no signature this
//     build knows (`FileKind::Unknown`) and no decoder accepted them either.
//  2. *"this build has no <FORMAT> reader"* -- the signature was recognised and
//     `io/Capabilities` says this binary cannot read that format. The
//     NP_USE_OIIO=OFF case, and the missing-plugin case, named as themselves
//     rather than as a decode failure.
//  3. *"the file is damaged"* -- the signature was recognised, this build does
//     read that format, and the decoder still declined. The decoder's own
//     reason is forwarded rather than paraphrased.
//
// Plus the four that come before any of that and name the file: an empty path,
// a path that does not exist, a folder, and a file that cannot be read or is
// zero bytes. On every refusal `out.document` is left default-constructed --
// there is no half-opened document.
//
// `recent`, when non-null, records `path` **on a `.npaint` open only** -- see
// this header's recent-list section.
OpenAnyResult openAnyFileAsDocument(const std::string& path, RecentDocuments* recent = nullptr);

// --- The command line -------------------------------------------------------

// D4 (docs/reachability-audit.md): whether a bare `argv[i]` names a file to
// hand to `openAnyFileAsDocument()` above, rather than one of `main()`'s own
// `--flag` strings. `naturalPaint foo.npaint` used to open nothing -- the
// flag loop matched only exact `--flag` spellings and fell through every
// positional argument with no branch to catch it.
//
// Pulled out as a pure predicate rather than left inline in that loop:
// everything else in `main()`'s argument parsing needs a live `argc`/`argv`
// cursor to consume a flag's own value (`--screenshot <path> [frames]`), and
// this is the one decision that does not, so it is the one worth asserting
// from `--selftest` without constructing an argv. `main.cpp` still owns the
// collection itself -- it needs `argv[i]`, not a `std::string_view` copied
// out of one -- but the classification lives here so a later flag added to
// that loop cannot silently start being read as a filename, or vice versa,
// without a test noticing.
//
// **The rule: not empty, and does not start with `-`.** A name that happens
// to collide with a recognised flag's exact spelling is read as the flag --
// every `else if` above this one in `main()`'s loop already claims its own
// spelling first, and this predicate is only ever consulted for what none of
// them matched. See this header's own module comment for why a `--`
// end-of-flags escape hatch is not added here: nothing has asked for one.
bool looksLikePositionalArgument(std::string_view arg) noexcept;

// --- Drag and drop ---------------------------------------------------------

// What one dropped file should become.
enum class DropAction {
  // Make it a new document in its own tab.
  OpenAsDocument,
  // Add it to the active document as a new RGB layer on top
  // (app/ImportImage's `importImageAsLayer()`).
  ImportAsLayer,
  // Say why, and change nothing.
  Refuse,
};

// **The routing rule**, as a pure function of what the file is and whether
// there is a document to put it in -- so it is asserted directly rather than
// through a window, a pointer position or a modifier key that a test would have
// to fake.
//
//   .npaint            -> always OpenAsDocument
//   image, no document -> OpenAsDocument
//   image, document    -> ImportAsLayer
//   unrecognised       -> Refuse
//
// **Why a `.npaint` is never imported.** The two gestures are not symmetric for
// it. Importing a document would decode its *composite* -- part 0, a derived
// product -- into one flat RGB layer, throwing away every layer, every latent
// and every bit of pigment basis, and it would look like it worked. Opening is
// the only reading of "here is a document" that is not lossy.
//
// **Why an image imports when a document is open.** That is what the gesture
// means in every application that has it: dragging a photograph onto a canvas
// puts the photograph on the canvas. Opening it instead would leave the user's
// work behind a new tab they did not ask for. With nothing open there is no
// canvas to put it on, so the same drag can only sensibly mean "open this".
//
// **No modifier key**, deliberately. A held Option during a drag is not
// reliable input -- SDL learns modifier state from key events, and on macOS a
// drag is a system-level gesture that need not deliver them -- so a rule that
// depended on one would work on the machine it was written on and be a
// coin-flip elsewhere. **The other gesture is instead reached from the File
// menu**, which is guaranteed to work and which the user already has:
// `File > Open...` always opens a new document from any file, and
// `File > Import Image...` always adds any decodable file to the open one --
// including a `.npaint`, whose composite decodes like any other EXR. So both
// directions are available for every file; the drop just picks the likely one.
DropAction dropActionFor(FileKind kind, bool documentIsOpen);

// What a whole drop did. One of these per *gesture*, not per file.
struct DropOutcome {
  size_t opened = 0;
  size_t imported = 0;
  size_t refused = 0;

  // The line for the status bar: a first line summarising the gesture, then one
  // line per problem, each naming its file. **Never empty**, including for a
  // drop where everything failed -- the silent no-op is the defect this whole
  // module exists to stop shipping. Multi-line by design: ui/MacPaintUI's status
  // line shows the first line and the whole thing as its tooltip.
  std::string status;

  // One line per **refused** file, naming it. Separately from `status` so
  // `--selftest` can count them without parsing a sentence. Capped (see the
  // implementation); `refused` is not.
  std::vector<std::string> problems;

  // Non-fatal notes from files that *did* land -- an import's oversize warning,
  // an open's "this document is bound to no file" note. Kept apart from
  // `problems` rather than pooled with them for two reasons: a warning must not
  // be counted or coloured as a failure, and pooling them would let a batch of
  // successful-but-noisy imports consume the cap on named refusals, so the one
  // file that actually failed would be the one not named.
  std::vector<std::string> warnings;
};

// Applies `dropActionFor()` to `paths` **in order**, re-asking "is a document
// open?" after each one.
//
// That single sentence is the answer to "what happens when twelve files are
// dropped at once", and it is deliberately not a special case:
//
//  * Twelve pictures onto an empty session: the first opens as a document, the
//    other eleven land in it as layers. One document, twelve layers, one status
//    line -- rather than twelve tabs or, worse, one file used and eleven
//    silently dropped.
//  * Twelve pictures onto an open document: twelve layers.
//  * Three `.npaint`s: three tabs, because a document is never a layer.
//  * A mixture resolves file by file, in the order the system delivered them.
//
// **No modal is opened, ever.** The drop already carries the file name, which
// is the only thing the path modal exists to ask for, so putting one up would
// be asking a question that has been answered -- twelve times over for a
// twelve-file drop. Refusals go to the status line instead.
//
// Every failure names its file. The problem lines are capped (see the
// implementation) so that a drop of a hundred unreadable files produces a
// status a person can read rather than a hundred-line tooltip; the count in
// `refused` is never capped.
//
// `session` gains the opened documents; the last thing opened or imported into
// becomes active, which is `DocumentSession::add()`'s own behaviour and the
// same rule app/ImportImage follows for the layer it adds.
DropOutcome applyDroppedFiles(DocumentSession& session, RecentDocuments* recent,
                              const std::vector<std::string>& paths);

}  // namespace np
