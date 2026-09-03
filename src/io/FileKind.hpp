#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "io/Capabilities.hpp"

// io/FileKind -- "what is this file?", answered from the file's own bytes.
//
// --- Why this exists at all ------------------------------------------------
//
// `File > Open...` used to call `openNpaintDocument()` and nothing else, so it
// opened `.npaint` and refused everything a user would actually double-click.
// Widening it needs one question answered first, and answered *before* either
// reader is handed the file: is this one of our documents, or is it a picture?
//
// **The answer must come from the content, never from the extension.** Three
// reasons, in the order they bite:
//
//  1. A `.npaint` **is an OpenEXR file** (docs/document-format.md; io/NpaintFile
//     even says "`.exr` is the same container under a different name", PRD I8),
//     so a user who saved one as `.exr` has a document, not a picture, and an
//     extension test would flatten it to a single RGB layer and throw the other
//     nine away without a word.
//  2. Files arrive renamed. A `.npaint` mailed as `foo.png` to get past a
//     filter, a PNG someone called `sketch.npaint` because that is the app they
//     use -- both are ordinary, and both come out wrong from a name test.
//  3. `io/ImageDecode` already sniffs content rather than trusting a name
//     (stb_image, then OpenImageIO, both on the bytes), so a name-based
//     dispatch one level up would be the only part of the read path that
//     believed the extension. Two rules for one question is how they diverge.
//
// Nothing here opens a file, allocates a decoder, or touches OpenImageIO. It
// looks at a byte buffer and says what the buffer claims to be.
//
// --- What identifies a `.npaint`, found rather than assumed -----------------
//
// There is no `np`-specific magic number, and searching io/NpaintFile.hpp for
// one finds nothing -- because the container is not ours. A `.npaint` is a
// multi-part tiled **OpenEXR**, so its first eight bytes are OpenEXR's:
//
//     76 2F 31 01   little-endian 20000630, OpenEXR's magic number
//     vv ff ff ff   version byte, then three flag bytes
//
// What makes it *ours* is an attribute in part 0's header: `np:version`
// (io/NpaintFile.cpp's `kAttrVersion`), which `saveNpaint()` stamps on every
// file it writes and `loadNpaint()` reads back. So the test is: OpenEXR magic,
// **and** an attribute named `np:version` in the first part's header. An EXR
// without it is somebody else's EXR and opens as a picture, which is exactly
// right -- it has no layer parts to reconstruct a document from.
//
// That test reads the EXR header structure directly (see FileKind.cpp), which
// is nine lines of walking null-terminated strings. It is deliberately not
// "search the first N kilobytes for the string `np:version`": a hex-encoded
// `np:comps` payload or an embedded file name could contain those ten bytes,
// and a false positive here sends a picture to the document reader.
//
// **On-disk format unchanged.** Nothing in this track writes a byte of
// `.npaint` differently from before; the marker used is one `saveNpaint()`
// has always written. An existing `.npaint` that opened before opens now.
//
// --- What identifies an SVG, and why a substring search is not enough ------
//
// Every format above has a magic number: fixed bytes at a fixed offset, a
// single `memcmp()`. SVG has none -- it is text, and valid SVG text can start
// with a byte-order mark, whitespace, an XML declaration, a comment, or a
// doctype before the first byte that is actually part of the picture. So
// "does the first 4 KB contain `<svg`" is the tempting shortcut, and it is
// wrong the same way this header's `np:version` shortcut above would have
// been wrong: it false-positives on an HTML page with an inline SVG, on an
// XSL stylesheet, and on a plain-text file that merely discusses SVG.
//
// The fix is the same shape as the OpenEXR one: read the format's own
// grammar instead of searching for a string that grammar happens to contain.
// `sniffFileKind()` (FileKind.cpp) runs a bounded **structural** scan:
//
//  1. Skip an optional UTF-8 BOM (`EF BB BF`).
//  2. Skip XML whitespace.
//  3. Repeatedly skip whole prologue constructs -- a `<?...?>` processing
//     instruction (the XML declaration is one), a `<!--...-->` comment, or a
//     `<!DOCTYPE ...>` -- and the whitespace between them.
//  4. Whatever comes next must be an element start tag. It is SVG when that
//     element's name is `svg` or ends in `:svg` (a namespace prefix, e.g.
//     `<svg:svg>`). Anything else -- a different root element, or the buffer
//     running out before step 4 is reached -- is not SVG.
//
// This is why an HTML file with an inline `<svg>` sniffs correctly as
// `Unknown` (or as HTML, if this module ever learns HTML): its root element
// is `html`, and the scan never looks inside it. A file that only *mentions*
// `<svg` in running text fails at step 4 the same way, because its first
// non-whitespace byte after the BOM is not `<` at all.
//
// The scan is bounded twice over, so a hostile or merely malformed file
// cannot make it run long or read out of bounds: it examines at most
// `kSvgMaxScanBytes` (FileKind.cpp), a few kilobytes from the front of the
// buffer, and it gives up after `kSvgMaxPrologueConstructs` prologue
// constructs even if every one of them was well-formed. Both are generous
// for a real file -- the same kind of two-orders-of-magnitude headroom the
// `.npaint` header walk above gives itself (a real part-0 header has under
// twenty attributes, capped at 4096) -- and neither is a quality setting: an
// SVG whose root tag sits further into the file than that has an
// unreasonable prologue, and this module says `Unknown` about it rather than
// searching indefinitely.
namespace np {

// What a file's own bytes say it is.
enum class FileKind {
  // An OpenEXR container carrying `np:version` -- one of this application's
  // own documents, whatever it is called on disk. Read by io/NpaintFile.
  NpaintDocument,
  // A container this application recognises as a picture. **Recognised is not
  // the same as decodable**: a TIFF is recognised in every build, and only
  // decodes when OpenImageIO is linked in. Keeping the two apart is the whole
  // point -- see `FileSniff::format` below.
  Image,
  // A vector image -- today, exclusively SVG. Recognised from a **structural**
  // scan of the text, not a magic number: see `sniffFileKind()`'s comment
  // below for why a substring search is not good enough and what the scan
  // actually does. io/SvgImport, which would read one, does not exist yet
  // (see FileKind.cpp) -- see app/OpenAnyFile.cpp and .hpp for how a `Vector`
  // file is refused, by name, until it does.
  Vector,
  // Nothing recognised. Not a refusal on its own; it is what turns a decode
  // failure into "this is not a format we read" rather than "your file is
  // damaged".
  Unknown,
};

// The bytes' own account of themselves.
struct FileSniff {
  FileKind kind = FileKind::Unknown;

  // The container recognised, for messages: "PNG", "JPEG", "OpenEXR",
  // "Photoshop PSD"... Empty exactly when `kind == Unknown`.
  //
  // A human-readable name rather than an enum, because several recognised
  // containers (GIF, WebP, PNM, Cineon) have no `ImageFormat` -- io/Capabilities'
  // enum is the set of formats this application *names in its UI*, which is
  // deliberately not the set OpenImageIO happens to decode. A signature with no
  // `ImageFormat` still tells a refusal what the file was.
  std::string signature;

  // The io/Capabilities format, when the signature names one. `std::nullopt`
  // for a recognised container outside that enum, and always for `Unknown`.
  //
  // This is what lets a refusal say "this build has no TIFF reader" (ask
  // `formatCapability()`) instead of the much less useful "could not be
  // decoded".
  std::optional<ImageFormat> format;
};

// Reads at most a few hundred bytes from the front of `data` (plus, for TGA,
// eighteen from the back) and says what it found. Never reads past
// `data + size`, on any input, including a deliberately malformed one -- the
// EXR header walk is bounded at every step, the same discipline io/Descriptor
// states for Action Descriptors.
//
// A null `data`, a zero `size`, or a buffer too short to hold any signature
// answers `Unknown` rather than guessing.
//
// **Four known limits, stated rather than left to be discovered:**
//
//  * **TGA has no leading magic.** The format begins with a raw header whose
//    every byte is a plausible value, so there is nothing to match. TGA 2.0
//    files carry `TRUEVISION-XFILE.` in the last eighteen bytes and those are
//    recognised; a TGA 1.0 file sniffs as `Unknown` and, if it then decodes,
//    decodes anyway -- the sniff gates the *message*, never the attempt.
//  * **Most camera raws are TIFF containers.** A CR2, NEF, ARW or DNG matches
//    the TIFF signature and is reported as TIFF. When this build cannot decode
//    it the refusal carries the decoder's own reason, which is the honest
//    account; it does not claim the file is a damaged TIFF.
//  * **UTF-16 SVG sniffs as `Unknown`.** A `FF FE` or `FE FF` byte-order mark
//    means every ASCII character of `<svg` is followed by a `00` byte, so the
//    structural scan above -- which reads UTF-8/ASCII bytes one at a time --
//    never finds a `<`. Handling it means transcoding the buffer before the
//    scan runs at all, which is a decoder's job, not a sniffer's: this module
//    answers from the bytes it was given, and does not allocate a converted
//    copy of them to answer a different question.
//  * **An SVG whose `<!DOCTYPE` internal subset hides a `>` inside a quoted
//    string is handled, not a gap** -- worth saying explicitly, because it is
//    exactly the kind of edge a naive "scan for the next `>`" would get
//    wrong. The doctype skip tracks bracket depth and quote state, the same
//    two things a real XML parser tracks there, so a quoted `>` (or a `[`/`]`
//    inside a quote) does not end the construct early.
FileSniff sniffFileKind(const uint8_t* data, size_t size);

// One word for `kind`, for messages and for --selftest output.
const char* fileKindName(FileKind kind);

}  // namespace np
