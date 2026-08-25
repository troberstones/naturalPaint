#include "app/OpenAnyFile.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>

#include "app/ImportImage.hpp"
#include "core/Document.hpp"
#include "io/Capabilities.hpp"
#include "io/ImageIO.hpp"

// app/OpenAnyFile -- implementation. Every design decision is argued in
// OpenAnyFile.hpp; this file holds the mechanics, and comments only where the
// mechanics are not obvious from the header's contract.

namespace np {

namespace {

// The file's own name, for messages. Deliberately not `documentDisplayName()`:
// this is a file that may never become a document, and on the failure paths
// there is no document to ask.
std::string fileNameOf(const std::string& path) {
  const std::string name = std::filesystem::path(path).filename().string();
  // A path ending in a separator has no filename component. Falling back to the
  // whole path keeps every message non-empty, which is the one property every
  // sentence here depends on.
  return name.empty() ? path : name;
}

OpenAnyResult refuse(std::string status, FileKind kind = FileKind::Unknown) {
  OpenAnyResult r;
  r.ok = false;
  r.kind = kind;
  r.status = std::move(status);
  return r;
}

// The formats this **binary** can read, asked of io/Capabilities' live runtime
// query rather than written down here.
//
// PRD I3's whole point is that the answer differs between builds -- and between
// two builds that both have OpenImageIO, because this project links a
// deliberately stripped-down one with no camera-raw plugin. A hardcoded list in
// a refusal message would be a guess that is wrong for the very build printing
// it, which is worse than saying nothing.
std::string readableFormatList() {
  std::string out;
  for (const FormatCapability& c : allFormatCapabilities()) {
    if (!c.canRead) continue;
    if (!out.empty()) out += ", ";
    out += imageFormatName(c.format);
  }
  return out;
}

}  // namespace

OpenAnyResult openAnyFileAsDocument(const std::string& path, RecentDocuments* recent) {
  if (path.empty()) return refuse("Open refused: no file name was given.");

  // `is_regular_file` before opening, for app/ImportImage's own reason: on
  // macOS opening a directory for reading succeeds and the first *read* fails,
  // so without this a dropped folder would be reported as an unreadable file.
  // A user who dropped a folder has made a different mistake from one whose
  // file is corrupt, and the message should say which.
  std::error_code ec;
  const std::filesystem::file_status st = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::exists(st))
    return refuse("Open refused: '" + path + "' does not exist.");
  if (std::filesystem::is_directory(st))
    return refuse("Open refused: '" + path + "' is a folder, not a file.");
  if (!std::filesystem::is_regular_file(st))
    return refuse("Open refused: '" + path + "' is not a regular file.");

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    // `strerror(errno)` rather than a sentence invented here: "Permission
    // denied" and "Too many open files" are different problems with different
    // fixes, and the C library already knows which one happened.
    return refuse("Open refused: '" + path + "' could not be opened (" +
                  std::strerror(errno) + ").");
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  if (in.bad())
    return refuse("Open refused: '" + path + "' could not be read to the end.");
  if (bytes.empty())
    return refuse("Open refused: '" + path + "' is empty (0 bytes).");

  const FileSniff sniff = sniffFileKind(bytes.data(), bytes.size());

  // --- One of ours ---------------------------------------------------------
  //
  // `loadNpaint()` takes a path, not bytes, so the file is read a second time
  // here. That is a deliberate trade rather than an oversight: giving
  // io/NpaintFile a from-memory entry point means handing OpenImageIO an
  // `IOProxy` through the whole multi-part reader, which is io/NpaintFile's
  // decision to make and a larger change than this dispatch is worth. The cost
  // is one extra read of a file that is about to be fully decoded anyway.
  if (sniff.kind == FileKind::NpaintDocument) {
    OpenAnyResult r;
    r.kind = sniff.kind;
    OpenDocument opened;
    const DocumentOpResult loaded = openNpaintDocument(path, &opened, recent);
    r.warnings = loaded.warnings;
    if (!loaded.ok) {
      // io/NpaintFile's own error, forwarded rather than paraphrased -- in an
      // NP_USE_OIIO=OFF build that reason is the whole account of why the file
      // was declined, and rewording it would lose the sentence that says how to
      // fix it. The prefix names the file *and* says what we thought it was, so
      // "it looked like one of ours and would not load" is distinguishable from
      // "we did not recognise it" without reading the rest.
      r.status = "Open refused: '" + fileNameOf(path) +
                 "' is a naturalPaint document, and it could not be read -- " + loaded.error;
      return r;
    }
    r.ok = true;
    r.document = std::move(opened);
    r.status = "Opened '" + fileNameOf(path) + "' (naturalPaint document, " +
               std::to_string(r.document.document.layers.size()) + " layer" +
               (r.document.document.layers.size() == 1 ? "" : "s") + ").";
    return r;
  }

  // --- A picture -----------------------------------------------------------
  //
  // Routed through io/ImageIO's `openImageAsDocument()` -- the function whose
  // own header has said since Phase 2 that it is "the function a future File >
  // Open would call" -- and not through a second decode-and-build path written
  // here. It sniffs content itself (stb first, then OpenImageIO), so the
  // signature match above gates only the *message*, never the attempt: a TGA
  // 1.0 file with no recognisable signature still gets decoded, and still
  // opens.
  //
  // **This is the seam for a layered PSD** (OpenAnyFile.hpp's last section). A
  // decoder returning a `Document` with N layers replaces this one call and
  // nothing else in this function: everything below is per-document.
  std::string decodeError;
  const std::optional<Document> decoded =
      openImageAsDocument(bytes.data(), bytes.size(), &decodeError);

  if (!decoded) {
    // The three refusals OpenAnyFile.hpp promises, in the order that makes each
    // one reachable: unknown signature, known signature this build has no
    // reader for, known signature this build reads and the file still declined.
    const std::string named = "Open refused: '" + fileNameOf(path) + "'";
    const std::string because =
        decodeError.empty() ? std::string() : std::string(" -- ") + decodeError;

    if (sniff.kind == FileKind::Unknown) {
      const std::string readable = readableFormatList();
      return refuse(named + " is not a naturalPaint document and its contents match no image "
                            "format this build reads" +
                        because + ". This build reads: " +
                        (readable.empty() ? std::string("nothing") : readable) + ".",
                    FileKind::Unknown);
    }

    if (sniff.format && !formatCapability(*sniff.format).canRead) {
      // The build-configuration case, named as itself. `unavailableReason` is
      // io/Capabilities' own sentence and already says whether the cause is the
      // NP_USE_OIIO build option or a plugin this OpenImageIO does not have --
      // which is the difference between "rebuild it" and "you cannot".
      return refuse(named + " is a " + sniff.signature +
                        " file, and this build has no " + imageFormatName(*sniff.format) +
                        " reader: " + formatCapability(*sniff.format).unavailableReason,
                    FileKind::Image);
    }

    return refuse(named + " is a " + sniff.signature +
                      " file this build does read, so the file itself is damaged or "
                      "truncated" +
                      because + ".",
                  FileKind::Image);
  }

  // --- Wrap it in a lifecycle record ---------------------------------------
  //
  // Every line here is per-document, which is what makes N layers a widening
  // rather than a rewrite (OpenAnyFile.hpp's seam section).
  OpenAnyResult r;
  r.ok = true;
  r.kind = FileKind::Image;
  r.document.id = allocateDocumentId();
  r.document.document = *decoded;
  // **Bound to nothing.** OpenAnyFile.hpp argues this at length; the short
  // version is that `saveNpaint()` writes EXR bytes whatever the path is
  // called, so a document bound to `photo.png` would have its next Cmd-S
  // destroy the user's photograph.
  r.document.path.clear();
  r.document.title = fileNameOf(path);
  r.document.residencyMode = TileResidencyMode::Eager;
  r.document.revision = 0;
  r.document.savedRevision = 0;
  // Dirty from birth, seeded exactly as `duplicateDocument()` seeds its copy:
  // `recordEdit()` on an empty history *is* the baseline (core/History::record),
  // so there is no earlier state -- correct, because there was no earlier state
  // of this document. The label is the noun form app/DocumentLifecycle.hpp asks
  // for, and it is what the close question and the History panel will show.
  r.document.recordEdit("open image as document (" + fileNameOf(path) + ")",
                        EditKind::Structural);

  const Document& doc = r.document.document;
  r.status = "Opened '" + fileNameOf(path) + "' (" + sniff.signature + ", " +
             std::to_string(doc.width) + "x" + std::to_string(doc.height) +
             ") as a new document.";
  // Said every time rather than once, because it is the surprising half of the
  // decision and the moment it matters is the moment the user reaches for Cmd-S.
  r.warnings.push_back("'" + fileNameOf(path) +
                       "' was opened as a document but is not bound to a file: Save is "
                       "unavailable until Save As gives it a .npaint of its own. That is "
                       "deliberate -- saving would write naturalPaint's own multi-layer "
                       "format, which would destroy the original picture if it were written "
                       "back over it.");
  return r;
}

// --- Drag and drop ---------------------------------------------------------

DropAction dropActionFor(FileKind kind, bool documentIsOpen) {
  switch (kind) {
    // Never a layer: importing one would flatten a whole document into a single
    // RGB layer via its composite, and would look like it worked. See
    // OpenAnyFile.hpp.
    case FileKind::NpaintDocument: return DropAction::OpenAsDocument;
    case FileKind::Image:
      return documentIsOpen ? DropAction::ImportAsLayer : DropAction::OpenAsDocument;
    case FileKind::Unknown: return DropAction::Refuse;
  }
  return DropAction::Refuse;
}

namespace {

// How many failing files get named in the status before it turns into a count.
//
// Eight, because the case this batching exists for is a drop of a dozen or so
// files, and naming eight of twelve is what tells a user whether the problem is
// "all of them" (a wrong folder) or "one odd one out" (one corrupt file) --
// which is the only question the list answers. Past that a list stops being
// readable in a tooltip and a count carries more. `DropOutcome::refused` is
// never capped, so the number is always exact.
constexpr size_t kMaxNamedProblems = 8;

}  // namespace

DropOutcome applyDroppedFiles(DocumentSession& session, RecentDocuments* recent,
                              const std::vector<std::string>& paths) {
  DropOutcome out;
  if (paths.empty()) {
    // Reachable: SDL raises SDL_EVENT_DROP_BEGIN when a drag merely enters the
    // window, so a drag that leaves again without dropping ends a gesture that
    // carried no files. Nothing happened and nothing is said about it.
    out.status = "Nothing was dropped.";
    return out;
  }

  auto note = [&out](std::string problem) {
    ++out.refused;
    if (out.problems.size() < kMaxNamedProblems) out.problems.push_back(std::move(problem));
  };

  for (const std::string& path : paths) {
    // Re-asked per file, which is the whole multi-file rule: the first picture
    // of a batch dropped onto an empty session opens a document, and by the
    // time the second is looked at there is one, so it imports. See
    // OpenAnyFile.hpp.
    const bool documentIsOpen = session.active() != nullptr;

    // Sniffed by opening and reading the head of the file, which is what
    // `openAnyFileAsDocument()` does anyway. Rather than read every file twice
    // -- once to route and once to act -- the routing question is asked with
    // the *cheap* half: for the import branch we only need to know it is not a
    // `.npaint`, and app/ImportImage re-reads the file itself.
    FileKind kind = FileKind::Unknown;
    {
      std::ifstream probe(path, std::ios::binary);
      if (probe) {
        // 64 bytes is more than every leading signature io/FileKind matches
        // (the longest is OpenEXR's ten-byte `#?RADIANCE` rival at ten) -- but
        // **not** enough for the `np:version` walk, which has to cross part 0's
        // whole header, nor for TGA's trailing footer. So the probe reads the
        // file whole rather than a window: these are files a human just
        // dragged, the very next step reads them entirely, and a window would
        // make the routing wrong for exactly the file (a large `.npaint` with a
        // fat PRD I10 carry) where being wrong costs the most.
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(probe)),
                                         std::istreambuf_iterator<char>());
        if (!bytes.empty()) kind = sniffFileKind(bytes.data(), bytes.size()).kind;
      }
    }

    switch (dropActionFor(kind, documentIsOpen)) {
      case DropAction::ImportAsLayer: {
        OpenDocument* target = session.active();
        // `documentIsOpen` was true a few lines ago and nothing between here
        // and there can close a document, so this is belt-and-braces rather
        // than a real branch -- but a null dereference on a drag is a crash the
        // user cannot even describe afterwards.
        if (target == nullptr) {
          note("'" + fileNameOf(path) + "' was not imported: the document it was meant for "
                                        "is no longer open.");
          break;
        }
        const ImportImageResult imported = importImageAsLayer(*target, path);
        if (imported.ok) {
          ++out.imported;
          for (const std::string& w : imported.warnings) out.warnings.push_back(w);
        } else {
          // app/ImportImage's own sentence, which already names the file and
          // forwards the decoder's reason. Not reworded here -- two modules
          // wording the same refusal differently is how they drift.
          note(imported.status);
        }
        break;
      }
      case DropAction::OpenAsDocument: {
        OpenAnyResult opened = openAnyFileAsDocument(path, recent);
        for (const std::string& w : opened.warnings) out.warnings.push_back(w);
        if (opened.ok) {
          ++out.opened;
          session.add(std::move(opened.document));
        } else {
          note(opened.status);
        }
        break;
      }
      case DropAction::Refuse:
        // The bytes matched nothing. Said here rather than by letting the file
        // fall through to a decoder that will also decline, so the sentence can
        // name the gesture ("dropped") and the file together.
        note("'" + fileNameOf(path) +
             "' was not opened: its contents match no format this build reads.");
        break;
    }
  }

  // --- the summary line ----------------------------------------------------
  //
  // Built from the counts rather than from the loop, so that the sentence and
  // what actually happened cannot disagree.
  const size_t total = paths.size();
  out.status = std::to_string(total) + (total == 1 ? " file dropped: " : " files dropped: ");
  std::string parts;
  auto addPart = [&parts](size_t n, const char* one, const char* many) {
    if (n == 0) return;
    if (!parts.empty()) parts += ", ";
    parts += std::to_string(n) + " " + (n == 1 ? one : many);
  };
  addPart(out.opened, "opened as a document", "opened as documents");
  addPart(out.imported, "imported as a layer", "imported as layers");
  addPart(out.refused, "refused", "refused");
  out.status += parts.empty() ? "nothing happened." : parts + ".";

  for (const std::string& p : out.problems) out.status += "\n" + p;
  // `problems` holds refusals and nothing else (warnings go to `warnings`), so
  // this subtraction is exactly "how many failures were not named".
  if (out.refused > out.problems.size())
    out.status +=
        "\n(and " + std::to_string(out.refused - out.problems.size()) + " more not listed.)";
  for (const std::string& w : out.warnings) out.status += "\n! " + w;
  return out;
}

// D4: see this function's declaration in the header for the full argument.
// The rule is deliberately this simple -- one character, no lookahead into
// the rest of `argv` -- because every flag that needs more than that (a
// following value, an optional trailing word) is matched by its own exact
// spelling in main()'s loop BEFORE this predicate is ever consulted for that
// argument.
bool looksLikePositionalArgument(std::string_view arg) noexcept {
  return !arg.empty() && arg[0] != '-';
}

}  // namespace np
