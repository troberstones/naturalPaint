#include "app/OpenAnyFile.hpp"

#include <cmath>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <utility>

#include "app/ImportImage.hpp"
#include "core/CanvasLimits.hpp"
#include "core/Document.hpp"
#include "io/Capabilities.hpp"
#include "io/ImageIO.hpp"
#include "core/LayerOps.hpp"
#include "io/PsdImport.hpp"
#include "io/SvgImport.hpp"

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

// core/CanvasLimits' refusal, in this file's message vocabulary, or the empty
// string when the document is renderable.
//
// **Placed after the decode, not before it**, and that is the deliberate half.
// Refusing on the sniffed header would be cheaper -- a PSD and a PNG both
// carry their extent in the first few dozen bytes -- but it would mean a
// second place that has to know how to read every format's dimensions, and
// getting *that* wrong fails in the direction this whole check exists to
// prevent. Decoding a file that is about to be refused costs the user a
// moment; a second extent parser costs a correctness surface. The abort this
// guards against happens at the first `wgpuDeviceCreateTexture`, which is
// several steps after the decode either way, so nothing is lost by being late.
std::string unrenderableRefusal(const std::string& path, const Document& doc, const char* what) {
  const std::string why = canvasDimensionRefusal(doc.width, doc.height);
  if (why.empty()) return {};
  return "Open refused: '" + fileNameOf(path) + "' is a " + what + " this build cannot draw: " + why;
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
    // Before `r.ok`, so a refused document is never half-opened -- the
    // header's "on every refusal `out.document` is left default-constructed"
    // contract, which `opened` going out of scope here honours.
    if (std::string why = unrenderableRefusal(path, opened.document, "naturalPaint document");
        !why.empty()) {
      r.status = std::move(why);
      return r;
    }
    r.ok = true;
    r.document = std::move(opened);
    r.status = "Opened '" + fileNameOf(path) + "' (naturalPaint document, " +
               std::to_string(r.document.document.layers.size()) + " layer" +
               (r.document.document.layers.size() == 1 ? "" : "s") + ").";
    return r;
  }

  // --- SVG ------------------------------------------------------------------
  //
  // Its own branch rather than a fall-through to the picture path below,
  // because `sniff.format` is `std::nullopt` for a `Vector` file
  // (io/FileKind.hpp): an SVG reaching the image decoder would come back
  // "matches no image format this build reads", which is false -- the file was
  // recognised and there IS a reader for it.
  //
  // The result is a **one-layer Vector document**, not a rasterised picture.
  // That is the point of the whole subsystem: the geometry stays editable, and
  // the raster is a cache the compositor derives (core/VectorRaster). So this
  // does not go through `openImageAsDocument()` at all -- there is nothing to
  // decode -- and builds the document here, which is a dozen lines because
  // every piece already exists.
  if (sniff.kind == FileKind::Vector) {
    SvgImportResult svg = importSvg(bytes.data(), bytes.size());
    if (!svg.ok) {
      return refuse("Open refused: '" + fileNameOf(path) + "' is an SVG, but it could not "
                        "be read -- " + svg.error,
                    FileKind::Vector);
    }

    // The viewport, rounded OUT: a 100.5px-wide drawing needs 101 columns, and
    // rounding down would clip the last one. io/SvgImport guarantees these are
    // non-zero when `ok`, and SVG's own UA default (300x150) covers a document
    // that states no size at all, so the zero test below is a belt on a brace.
    const double wD = std::ceil(static_cast<double>(svg.widthPx));
    const double hD = std::ceil(static_cast<double>(svg.heightPx));
    if (!(wD >= 1.0) || !(hD >= 1.0)) {
      return refuse("Open refused: '" + fileNameOf(path) +
                        "' is an SVG whose viewport is empty (" +
                        std::to_string(svg.widthPx) + "x" + std::to_string(svg.heightPx) +
                        "), so there is no canvas to open it into.",
                    FileKind::Vector);
    }
    // Refused by name rather than clamped. A clamp would open the file and
    // silently crop the artwork, and an SVG is resolution-independent -- the
    // honest answer is that this build's canvas is not, and to say the limit.
    //
    // **The ceiling is the renderable one (core/CanvasLimits), not
    // app/DocumentPresets' `kMaxDocumentPresetDimension`.** This branch
    // originally reached for the preset bound because it was the only canvas
    // limit that existed when it was written. That bound is 32768 -- twice
    // what a typical adapter will create a texture for -- so an SVG declaring
    // a 20000px viewport passed it and then aborted the process at the first
    // `wgpuDeviceCreateTexture`, which is the exact failure the render
    // ceiling exists to prevent.
    //
    // Worth recording how that survived: this branch and the commit adding
    // the render ceiling were developed in parallel and **text-merged with no
    // conflict**, because they touch different lines of the same function.
    // Neither side's tests covered the other's path. `--selftest`'s
    // `openAnyFileAsDocument` section now drives an oversize SVG for exactly
    // that reason.
    //
    // Compared as `double` rather than by casting to `int32_t` first: a
    // hostile or generated SVG can declare a viewport far outside int32's
    // range, and the cast would wrap before the comparison ever ran.
    const double kRenderMax = static_cast<double>(maxCanvasDimension());
    if (wD > kRenderMax || hD > kRenderMax) {
      return refuse("Open refused: '" + fileNameOf(path) + "' is an SVG whose viewport is " +
                        std::to_string(static_cast<long long>(wD)) + "x" +
                        std::to_string(static_cast<long long>(hD)) +
                        " px, and this display adapter cannot draw a canvas wider or taller "
                        "than " + std::to_string(maxCanvasDimension()) +
                        ". Scale it down in the exporting application first.",
                    FileKind::Vector);
    }

    Document built = Document::createBlank(static_cast<int32_t>(wD),
                                           static_cast<int32_t>(hD), WorkingSpace{});
    built.layers.clear();
    Layer vec = makeVectorLayer(fileNameOf(path));
    vec.shapes = std::move(svg.shapes);
    // **io/SvgImport leaves every `id` at zero, and this is where they are
    // assigned.** `core/VectorShape.hpp` says zero means "not yet assigned",
    // and app/PenTool keys its selection on the id -- so importing without
    // this step gives a layer whose shapes are individually unselectable, all
    // of them answering to shape 0. Nothing in the importer could do it
    // instead: ids are unique *within a layer*, and the importer does not know
    // which layer its shapes are about to land in.
    uint64_t nextId = 1;
    for (VectorShape& shape : vec.shapes) shape.id = nextId++;
    vec.nextShapeId = nextId;
    const size_t shapeCount = vec.shapes.size();
    addLayer(built, 0, std::move(vec));

    OpenAnyResult r;
    r.ok = true;
    r.kind = FileKind::Vector;
    r.document.id = allocateDocumentId();
    r.document.document = std::move(built);
    // Bound to nothing, for the same reason the picture path below is: a
    // Cmd-S against `logo.svg` would write EXR bytes over the user's SVG.
    r.document.path.clear();
    r.document.title = fileNameOf(path);
    r.document.residencyMode = TileResidencyMode::Eager;
    r.document.revision = 0;
    r.document.savedRevision = 0;
    r.document.recordEdit("open SVG as document (" + fileNameOf(path) + ")",
                          EditKind::Structural);

    r.status = "Opened '" + fileNameOf(path) + "' (SVG, " +
               std::to_string(static_cast<long long>(wD)) + "x" +
               std::to_string(static_cast<long long>(hD)) + ") as a vector document with " +
               std::to_string(shapeCount) +
               (shapeCount == 1 ? " shape." : " shapes.");
    // Every refusal io/SvgImport recorded, verbatim -- the same "say what was
    // dropped" contract app/PsdReport established, and the reason the importer
    // returns them as data rather than printing them.
    for (std::string& why : svg.refusals) r.warnings.push_back("SVG: " + std::move(why));
    if (shapeCount == 0) {
      r.warnings.push_back(
          "'" + fileNameOf(path) +
          "' produced no shapes: the document opened, but it is empty. Any reason is "
          "listed above.");
    }
    r.warnings.push_back("'" + fileNameOf(path) +
                         "' was opened as a document but is not bound to a file: Save is "
                         "unavailable until Save As gives it a .npaint of its own.");
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
  // **This was the seam for a layered PSD** (OpenAnyFile.hpp's last section
  // named it before io/PsdImport existed) **and this is that widening,
  // landed.** Exactly as that section predicted: a decoder returning a
  // `Document` with N layers replaces the one `openImageAsDocument()` call
  // below for this one format, and nothing else in this function changed --
  // every line from "Wrap it in a lifecycle record" on is still per-document,
  // not per-layer, so it did not need to know the layer count was no longer
  // always one.
  //
  // `sniff.format` (not the generic decode-and-see-what-comes-back path
  // every other format takes) is what selects io/PsdImport specifically,
  // because PSD is the one format this build reads through two entirely
  // different decoders for two different reasons: io/PsdImport's own layered
  // reader, tried first, and the flattened fallback below for the one case
  // io/PsdImport declines on purpose rather than by failure --
  // `PsdImportResult::noLayerData`, a PSD saved with Maximize Compatibility
  // off (or otherwise carrying no Layer and Mask Information at all), which
  // has no layers for a layered reader to build a Document out of and is
  // exactly what `openImageAsDocument()`'s existing OpenImageIO-backed path
  // already opens correctly. **Every other io/PsdImport refusal skips that
  // fallback and refuses the whole open with io/PsdImport's own reason**: a
  // corrupt file, or one using a compression/colour mode/depth this build's
  // PSD reader does not support, has real layer content that the fallback
  // would silently flatten and report as success -- the confidently-wrong
  // outcome this function's three-refusals design (this file's own header)
  // exists to keep out.
  std::string decodeError;
  std::vector<std::string> psdWarnings;
  std::optional<Document> decoded;
  bool psdDecided = false;

  if (sniff.format == ImageFormat::Psd) {
    PsdImportResult psd = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
    if (psd.ok) {
      decoded = std::move(psd.document);
      psdWarnings = std::move(psd.warnings);
      psdDecided = true;
    } else if (!psd.noLayerData) {
      decodeError = psd.error;
      psdDecided = true;  // refuse outright -- see this comment's own argument
    }
    // else: `noLayerData` -- fall through to the flattened path below,
    // unchanged, exactly as if io/PsdImport did not exist for this file.
  }

  if (!psdDecided) decoded = openImageAsDocument(bytes.data(), bytes.size(), &decodeError);

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

  // The one canvas this build could not draw. Checked here, on the decoded
  // document, because this is the branch every raster format reaches --
  // PNG/JPEG through stb, EXR/TIFF/DPX through OpenImageIO, and a layered PSD
  // through io/PsdImport all arrive as a `Document` at this line, so one check
  // covers formats that share no decoder.
  if (std::string why = unrenderableRefusal(path, *decoded, "picture"); !why.empty())
    return refuse(std::move(why), FileKind::Image);

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
  // A layer count past one is worth saying in the same sentence the npaint
  // branch above already says it in -- this is the one place an *image*
  // open can have more than the single layer every other format here still
  // produces, and it is exactly io/PsdImport landing that makes it possible.
  if (doc.layers.size() != 1)
    r.status += " (" + std::to_string(doc.layers.size()) + " layers.)";
  // io/PsdImport's own notes -- today, an unmapped blend mode naming the PSD
  // key and the layer -- carried through unchanged, the same "say what was
  // dropped" shape `warnings` already carries for every other non-fatal
  // note this function forwards.
  for (std::string& w : psdWarnings) r.warnings.push_back(std::move(w));
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

// **The coordinate-space argument**, so the next reader does not have to
// re-derive it from scratch or, worse, trust that it was checked.
//
// `SDL_DropEvent::x/y` and `SDL_MouseMotionEvent::x/y` carry the identical
// doc comment in SDL3's own `SDL_events.h` -- "relative to window" -- and
// this project already depends on the mouse half of that pairing: main.cpp's
// `--pen-demo` writes synthetic coordinates straight into `ImGuiIO::MousePos`
// at values it reads out of `atelierLayout()`, with no scale factor anywhere
// between the two, and main.cpp's own `--split-demo` comment states outright
// that "ImGui's `DisplaySize` is exactly `SDL_GetWindowSize()`" -- the
// *logical* size, not `SDL_GetWindowSizeInPixels()`'s framebuffer one, which
// this window's `SDL_WINDOW_HIGH_PIXEL_DENSITY` flag makes a real, non-1:1
// distinction on a Retina display.
//
// That is one working half of the pairing, not a proof for the other, so the
// other was read rather than inferred: SDL3's Cocoa backend
// (`src/video/cocoa/SDL_cocoawindow.m`, `-performDragOperation:` and
// `-draggingUpdated:`) computes the drop position as
// `x = point.x; y = sdlwindow->h - point.y;` from `[sender draggingLocation]`
// -- an `NSPoint` in the content view's own coordinate system, i.e. Cocoa
// points, never backing-store pixels -- flipped against `sdlwindow->h`, which
// is `SDL_Window::h` (`src/video/SDL_sysvideo.h`), the same logical field
// `SDL_GetWindowSize()` reads. (`SDL_Window` keeps the pixel size in a
// separate field, `last_pixel_w`/`last_pixel_h`, that this path never
// touches.) `SDL_SendDrop()` (`src/events/SDL_dropevents.c`) then caches
// that position in a static and stamps it onto every `SDL_EVENT_DROP_FILE`
// event of the gesture, not just `SDL_EVENT_DROP_POSITION` -- which is what
// makes reading `e.drop.x/y` on `DROP_FILE` (main.cpp does, below) sound: the
// value is not "wherever the pointer happened to be", it is the same location
// `draggingUpdated:` last reported, in the same units `atelierLayout()` uses.
//
// So both halves resolve to the same space -- SDL's logical window
// coordinates, which is also `AtelierBands`' own space by that header's
// comment -- and **no scale conversion is applied anywhere in this path,
// because none is needed.** A build where that ever stopped being true would
// show it as every drop landing in the band above or left of the one the
// user actually released over, on a Retina display only -- exactly the
// silent, hard-to-notice failure this comment exists to keep out from the
// start rather than debug once shipped.
DropDestination dropDestinationForPoint(float x, float y, const DropBands& bands) noexcept {
  if (bands.tabStrip.contains(x, y)) return DropDestination::TabStrip;
  if (bands.canvas.contains(x, y)) return DropDestination::ActiveDocument;
  return DropDestination::Unspecified;
}

DropAction dropActionFor(FileKind kind, bool documentIsOpen, DropDestination destination) {
  switch (kind) {
    // Never a layer, wherever it lands: importing one would flatten a whole
    // document into a single RGB layer via its composite, and would look like
    // it worked. See OpenAnyFile.hpp.
    case FileKind::NpaintDocument: return DropAction::OpenAsDocument;
    case FileKind::Image:
      // The tab strip means "a new document" even when one is already open --
      // that is the one place `destination` changes the answer. Every other
      // destination, including `Unspecified`, computes the rule this function
      // has always had: import when there is somewhere to import into, else
      // open. See OpenAnyFile.hpp's "Unspecified and ActiveDocument compute
      // the same thing" for why that is one branch and not two.
      if (destination == DropDestination::TabStrip) return DropAction::OpenAsDocument;
      return documentIsOpen ? DropAction::ImportAsLayer : DropAction::OpenAsDocument;
    // An SVG opens as a document wherever it lands, exactly like a `.npaint`
    // above and unlike a picture -- and the asymmetry is deliberate rather
    // than unfinished.
    //
    // **Importing one as a layer is not a missing branch here, it is an
    // unmade decision.** io/SvgImport returns shapes in the coordinates of
    // the SVG's own viewport; dropping those into a document of a different
    // size has at least three defensible answers -- place at 1:1 and let the
    // artwork fall off the canvas, scale to fit, or centre -- and picking one
    // silently is how an importer acquires a behaviour nobody chose. Until
    // that is decided, opening the file is the answer that loses nothing:
    // every shape arrives, at its own coordinates, on a canvas sized to hold
    // them.
    case FileKind::Vector: return DropAction::OpenAsDocument;
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
                              const std::vector<std::string>& paths,
                              DropDestination destination) {
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

  // Index into `out.warnings` of the oversize-canvas warning
  // `importImageAsLayer()` appends for whichever import most recently set
  // `out.transformableLayer` -- reset by every subsequent import so it never
  // outlives the one call it describes. Withdrawn below alongside
  // `transformableLayer` itself, for the identical reason: main.cpp's drop
  // handler seeds exactly that layer's TransformSession with a fit-to-canvas
  // scale (`computeDropFitTransform()`) the instant this function returns, so
  // the overflow this warning describes is about to stop being true before
  // the user even reads it. See the withdrawal below for the case where it
  // stays.
  std::optional<size_t> transformableLayerOversizeWarningIndex;

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

    switch (dropActionFor(kind, documentIsOpen, destination)) {
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
          // Remembered for every import, then withdrawn below unless this
          // gesture turns out to have been the unambiguous one-picture case.
          // Recorded here rather than recomputed afterwards because
          // `ImportImageResult` is the thing that actually knows where the
          // layer landed -- app/ImportImage.hpp returns it precisely so a
          // caller does not re-derive `layers.size() - 1` and drift.
          out.transformableLayer = imported.layerIndex;
          // Reset every time `transformableLayer` is (re)set, so a warning
          // from an EARLIER import in a multi-file drop is never withdrawn
          // by a LATER one's own bookkeeping -- see the withdrawal below.
          transformableLayerOversizeWarningIndex.reset();
          for (const std::string& w : imported.warnings) {
            out.warnings.push_back(w);
            transformableLayerOversizeWarningIndex = out.warnings.size() - 1;
          }
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
        // One reason reaches this case now: the bytes matched no format this
        // build knows. `Vector` used to arrive here too, when there was no SVG
        // importer to route it to; it now routes to OpenAsDocument above, so
        // an SVG that still fails does so inside openAnyFileAsDocument() with
        // that function's own specific reason (a malformed document, an empty
        // viewport, a canvas past this build's maximum) rather than under this
        // generic sentence.
        note("'" + fileNameOf(path) +
             "' was not opened: its contents match no format this build reads.");
        break;
    }
  }

  // --- the summary line ----------------------------------------------------
  //
  // Built from the counts rather than from the loop, so that the sentence and
  // what actually happened cannot disagree.
  // **Withdraw the transform offer unless this gesture was the unambiguous
  // one.** The loop above set it on every successful import; only a drop that
  // imported exactly one picture and opened nothing keeps it. See
  // OpenAnyFile.hpp's `transformableLayer` for why both halves are required --
  // eleven layers have no non-arbitrary "the one you meant", and a gesture
  // that also opened a document has moved the active document out from under
  // the index.
  if (out.imported != 1 || out.opened != 0) {
    out.transformableLayer.reset();
  } else if (transformableLayerOversizeWarningIndex &&
             *transformableLayerOversizeWarningIndex < out.warnings.size()) {
    // The unambiguous case: main.cpp is about to seed THIS layer's
    // TransformSession with a fit-to-canvas scale the moment this function
    // returns (see app/TransformSession.hpp's `computeDropFitTransform()`),
    // so the oversize warning importImageAsLayer() appended -- "the part
    // past the canvas edge is ... never composited" -- describes a problem
    // that stops being true before the user can read it. Withdrawn here,
    // and only here: a multi-file or mixed drop (the branch above) gets no
    // seeded transform for anything, so its oversize warnings, if any, stay.
    out.warnings.erase(out.warnings.begin() +
                       static_cast<std::ptrdiff_t>(*transformableLayerOversizeWarningIndex));
  }

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
