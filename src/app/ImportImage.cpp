#include "app/ImportImage.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>

#include "io/ImageDecode.hpp"
#include "io/ImageIO.hpp"

// app/ImportImage -- implementation. Every design decision is argued in
// ImportImage.hpp; this file holds the mechanics, and comments only where the
// mechanics are not obvious from the header's contract.

namespace np {

namespace {

// The file's own name, for messages. `documentDisplayName()` does the same
// thing for a document, but this is a file that is not a document and may
// never become one, so it does not go through that function.
std::string fileNameOf(const std::string& path) {
  const std::string name = std::filesystem::path(path).filename().string();
  // A path ending in a separator has no filename component. Falling back to the
  // whole path keeps every message non-empty, which is the one property the
  // failure sentences all depend on.
  return name.empty() ? path : name;
}

ImportImageResult refuse(std::string status) {
  ImportImageResult r;
  r.ok = false;
  r.status = std::move(status);
  return r;
}

}  // namespace

std::string importImageEditLabel(const std::string& path) {
  return "import image as layer (" + fileNameOf(path) + ")";
}

ImportImageResult importImageAsLayer(OpenDocument& doc, const std::string& path) {
  if (path.empty()) return refuse("Import refused: no file name was given.");

  // `is_regular_file` before opening, not because `ifstream` would crash on a
  // directory but because it would not: on macOS opening a directory for
  // reading succeeds and the first read fails, which would surface as
  // "unreadable" rather than "that is a folder". The two are worth telling
  // apart -- a user who typed a directory path has made a different mistake
  // from one whose file is corrupt.
  std::error_code ec;
  const std::filesystem::file_status st = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::exists(st))
    return refuse("Import refused: '" + path + "' does not exist.");
  if (std::filesystem::is_directory(st))
    return refuse("Import refused: '" + path + "' is a folder, not an image file.");
  if (!std::filesystem::is_regular_file(st))
    return refuse("Import refused: '" + path + "' is not a regular file.");

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    // `strerror(errno)` rather than a sentence invented here: "Permission
    // denied" and "Too many open files" are different problems with different
    // fixes, and the C library already knows which one happened.
    return refuse("Import refused: '" + path + "' could not be opened (" +
                  std::strerror(errno) + ").");
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  if (in.bad())
    return refuse("Import refused: '" + path + "' could not be read to the end.");
  if (bytes.empty())
    return refuse("Import refused: '" + path + "' is empty (0 bytes).");

  std::string decodeError;
  const DecodedImage img = decodeImageLinear(bytes.data(), bytes.size(), &decodeError);
  if (!img.valid()) {
    // io/ImageDecode's own reason, forwarded rather than paraphrased -- in an
    // NP_USE_OIIO=OFF build that reason is the whole account of why an EXR was
    // declined, and rewording it here would lose the one sentence that says
    // how to fix it. The file is named first because the message may arrive
    // long after the path was typed.
    return refuse("Import refused: '" + path + "' could not be decoded" +
                  (decodeError.empty() ? std::string(".")
                                       : std::string(" -- ") + decodeError + "."));
  }

  // Nothing above this line has touched the document, and nothing below it can
  // fail: `placeImageAsLayer()` refuses only an invalid `DecodedImage`, which
  // `img.valid()` has already ruled out. That ordering is what makes "no
  // partial import" a property of the code rather than a promise.
  if (!placeImageAsLayer(doc.document, img)) {
    return refuse("Import refused: '" + path +
                  "' decoded but could not be placed as a layer.");
  }

  ImportImageResult r;
  r.ok = true;
  r.imageWidth = img.width;
  r.imageHeight = img.height;
  r.layerIndex = doc.document.layers.size() - 1;

  // The new layer becomes the active one. See ImportImage.hpp: an import that
  // does not move the layers panel's selection reads as an import that did not
  // happen.
  setActiveLayer(doc, r.layerIndex);

  // Exactly one entry, after the mutation, which is what `recordEdit()`
  // requires (the entry holds the post-edit state).
  doc.recordEdit(importImageEditLabel(path), EditKind::Structural);

  r.status = "Imported '" + fileNameOf(path) + "' (" + std::to_string(img.width) + "x" +
             std::to_string(img.height) + ") as layer " + std::to_string(r.layerIndex + 1) +
             " of " + std::to_string(doc.document.layers.size()) + ".";

  // The oversize note. Both sizes, and what it costs, because the pixels past
  // the canvas edge are real, are paid for, and are invisible -- see
  // ImportImage.hpp for why they are neither cropped nor resampled.
  const bool widerThanCanvas =
      doc.document.width > 0 && img.width > static_cast<uint32_t>(doc.document.width);
  const bool tallerThanCanvas =
      doc.document.height > 0 && img.height > static_cast<uint32_t>(doc.document.height);
  if (widerThanCanvas || tallerThanCanvas) {
    r.warnings.push_back("'" + fileNameOf(path) + "' is " + std::to_string(img.width) + "x" +
                         std::to_string(img.height) + ", larger than this document's " +
                         std::to_string(doc.document.width) + "x" +
                         std::to_string(doc.document.height) +
                         " canvas. Nothing was cropped or resized, so the part past the "
                         "canvas edge is stored in the layer but is never composited, "
                         "drawn or exported.");
  }
  return r;
}

}  // namespace np
