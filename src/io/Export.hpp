#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "color/Space.hpp"
#include "core/Document.hpp"
#include "io/ImageDecode.hpp"

// io/Export (PLAN.md "Phase 4 -- Write it out", step 1: "Export path --
// encode from working space to a chosen target space and bit depth,
// explicitly, never silently (PRD B6, I5)").
//
// This is the first write path anywhere in io/. io/ImageDecode and
// io/ImageIO between them cover the inbound direction only -- file bytes ->
// linear float RGBA -> premultiplied half-float tiles. This header is the
// exact mirror of that journey, run backwards:
//
//   tiles -> flatten -> un-premultiply -> encode -> quantize -> file bytes
//
// and every one of those five stages is a place the decode side already
// made a decision that has to be undone in the same terms, not
// re-improvised. Where that's the case the relevant decode-side comment is
// named below rather than paraphrased.
//
// --- What "explicitly, never silently" actually means here ---------------
//
// PRD I5 (P0) reads "Export offers target colour space and bit depth
// explicitly." That is a statement about the *API*, not just the UI: every
// entry point below takes `ExportTargetSpace` and `ExportBitDepth` as
// ordinary, non-defaulted parameters. There is deliberately no overload,
// no default argument and no "sensible fallback" that lets a caller omit
// either one and receive a guess -- omitting one is a compile error, which
// is the only enforcement that actually holds. The enum values name both
// halves of what a colour space is (primaries *and* transfer function), so
// "which curve got applied" is never something a call site leaves implicit.
//
// PRD B6 (P0) reads "Bit depth is preserved end to end; 16- and 32-bit
// files never silently truncate to 8." A request this module cannot honour
// -- 16-bit into JPEG, say, which is 8-bit-per-channel by format
// definition -- returns `ExportResult{ok = false}` with an error string
// that names the format, the depth, and what to do instead. It never
// quietly writes 8 bits and reports success. Today PNG is the only format
// here that carries 16 bits (see kSupported table in Export.cpp); the
// remaining three refuse it loudly rather than degrading.
//
// --- Scope decision: transfer function only, primaries rejected ----------
//
// A "target colour space" can differ from the working space in two
// independent ways: its transfer function and its primaries. This module
// implements the first and *refuses* the second.
//
// color/Space.hpp's `WorkingSpace` carries `Primaries` -- chromaticity
// coordinates -- and that header says outright that it is "*not* an
// RGB<->XYZ matrix: deriving one is a job for whichever later step actually
// needs to adapt primaries". No such matrix exists anywhere in this
// codebase yet, and building one here would mean landing a
// Bradford/CAT02-or-not chromatic-adaptation decision, an XYZ intermediate,
// and a gamut-mapping-vs-clipping policy for out-of-gamut colours -- three
// real design questions, none of which PLAN.md step 1 asks for and none of
// which the two transfer functions that actually exist need. So:
//
//   Every `ExportTargetSpace` below states its primaries, and
//   encodeLinearImage() compares them against the document's working-space
//   primaries. A mismatch is a hard, named failure -- never silently
//   ignored, and never silently "converted" by a matrix that does not
//   exist.
//
// This is the deliberate, documented alternative to implementing the matrix
// now, and it matches step 1's own "never silently" wording: refusing to
// answer a question this build cannot answer correctly is honest; writing
// the pixels out under a primaries label they were never converted to is
// exactly the silent misreporting the step exists to prevent. When a later
// step (ACEScg support, or an OIIO-backed export) needs real gamut
// conversion, the check in Export.cpp is the single place that turns from
// "reject" into "convert" -- there is no second, scattered assumption of
// matching primaries anywhere else in this module.
//
// --- Alpha ---------------------------------------------------------------
//
// Alpha is opacity, not light. io/ImageDecode.hpp's header states the
// policy for the inbound direction ("Alpha... passes through unencoded --
// alpha is opacity, not light -- it is never gamma-encoded") and
// core/Probe.hpp restates it for the readout direction. This module follows
// it in reverse: the transfer function is applied to R, G and B only; alpha
// is quantized straight from its linear opacity value with no curve on it
// at all, for every target space including the two encoded ones.
//
// Separately: JPEG has no alpha channel -- stb_image_write's JPEG encoder
// reads only offsets 0/1/2 of each pixel and drops the fourth outright. A
// document with any partially transparent pixel therefore cannot be
// exported to JPEG without losing data, so this module refuses that
// combination by name (PRD I11's "A save that would lose data names exactly
// what, rather than degrading silently", applied to the same principle
// PLAN.md step 1 already demands for bit depth). Fully opaque documents
// export to JPEG normally. Compositing a translucent document onto a chosen
// matte is a real feature with a real UI decision behind it (what colour?)
// -- that belongs to PLAN.md step 7's "Export As", not here.
//
// --- Entry points, deliberately kept separate ---------------------------
//
// Same structuring philosophy io/ImageIO.hpp already documents: separate,
// individually callable stages rather than one function that does
// everything, so each is testable on its own and later steps can reuse the
// middle of the pipeline without re-deriving it.
//
//   flattenDocumentToLinear -- Document -> straight, linear float RGBA. No
//     colour encoding, no quantization, no format. The stage that undoes
//     io/ImageIO.cpp's premultiply-on-import.
//
//   encodeLinearImage -- straight linear float RGBA + the space it is
//     defined in -> encoded file bytes. Everything colour and format
//     related lives here: the primaries check, the depth/format
//     compatibility check, the transfer function, the quantization, and the
//     actual byte writer.
//
//   exportDocument -- the two above, composed. The Document-level operation
//     PLAN.md step 1 asks for.
//
//   exportDocumentToFile -- exportDocument() plus an fwrite. Kept separate
//     so nothing about the encode path depends on a filesystem, which is
//     what lets --selftest exercise all of it in memory.
//
//   encodePng16 -- the 16-bit PNG writer itself, exposed because it is the
//     only reason PRD B6's 16-bit requirement is satisfiable against a
//     vendored-stb-only build (PRD I1: "no optional dependency").
//
// Explicitly NOT here: any UI. PLAN.md step 7 ("Export As -- format, space,
// depth *and resize*, with saveable presets") is where a File > Export
// dialog and its presets belong, and this codebase still has no
// live-painting-canvas-to-core::Document bridge for such a menu item to
// export from -- the same gap every prior UI-facing step's Findings row
// records. This is the Document-level operation underneath that step, the
// same way placeImageAsLayer() was for step 13's drag-and-drop.
namespace np {

// The four formats PRD I1 (P0) requires with no optional dependency:
// "Read and write PNG, JPEG, TGA, BMP with no optional dependency." All
// four are written through the already-vendored third_party/stb_image_write.h
// (plus encodePng16() below for the one thing stb's public API cannot do).
// EXR/TIFF/HDR/DPX are PLAN.md step 2's OIIO-backed territory and are
// deliberately absent -- this enum is exactly I1's list, not a superset
// that implies support this build does not have.
enum class ExportFormat {
  Png,
  Jpeg,
  Tga,
  Bmp,
};

// A target colour space, named as what a colour space actually is: a
// primaries set *and* a transfer function. Both halves are in every name on
// purpose -- `Rec709Srgb` cannot be mistaken for `Rec709Bt709` at a call
// site the way a bare "sRGB"/"709" pair could, and `Rec709Linear` says
// out loud that no curve is applied rather than leaving "linear" to be
// inferred from the absence of a parameter.
//
// All three share Rec.709/sRGB primaries (D65) because those are the only
// primaries this codebase has a constant for (color/Space.hpp's
// kRec709Primaries, also `WorkingSpace`'s default) -- see this header's
// scope-decision section for why a target with *different* primaries is a
// rejected request rather than a supported conversion. Adding e.g.
// `AcesCgLinear` here is the natural extension point once a real
// primaries-conversion matrix exists; nothing else in this module would
// need to move.
enum class ExportTargetSpace {
  // Rec.709/sRGB primaries (D65), NO transfer function: the linear-light
  // working values are quantized as-is. This is the "no-encode" option --
  // useful for handing linear data to something that will apply its own
  // curve, and the one option where a bug that accidentally applied sRGB
  // anyway is trivially visible (mid-grey lands at 0.5 of full scale, not
  // at ~0.735).
  Rec709Linear,
  // Rec.709/sRGB primaries (D65), sRGB transfer function (IEC 61966-2-1) --
  // color/Space.hpp's srgbEncode(). The right choice for a file that will
  // be opened by anything that assumes untagged 8-bit images are sRGB,
  // which is what io/ImageDecode.cpp's own decode-side assumption already
  // commits this pipeline to on the way in.
  Rec709Srgb,
  // Rec.709 primaries (D65), the ITU-R BT.709 OETF -- color/Space.hpp's
  // rec709Encode(). Numerically close to sRGB but a genuinely different
  // curve (different toe breakpoint and slope); color/Space.hpp's own
  // comment spells out why conflating the two "because the primaries
  // match" would be wrong.
  Rec709Bt709,
};

// Bits per channel in the written file. Not an int, so "8" and "16" are the
// only expressible requests and a caller cannot ask for something (10? 12?)
// no format here writes and get a silent nearest-fit. 32-bit float export
// is PLAN.md step 2's OIIO/EXR territory and is deliberately not an option
// this build pretends to offer.
enum class ExportBitDepth {
  Eight,
  Sixteen,
};

// Human-readable names, used to build the error strings below and available
// to any future UI. Header-inline for the same reason core/Layer.hpp's
// layerKindName() is -- nothing here is non-trivial.
inline const char* exportFormatName(ExportFormat f) {
  switch (f) {
    case ExportFormat::Png: return "PNG";
    case ExportFormat::Jpeg: return "JPEG";
    case ExportFormat::Tga: return "TGA";
    case ExportFormat::Bmp: return "BMP";
  }
  return "?";
}

inline const char* exportTargetSpaceName(ExportTargetSpace s) {
  switch (s) {
    case ExportTargetSpace::Rec709Linear: return "Rec709Linear (Rec.709 primaries, linear)";
    case ExportTargetSpace::Rec709Srgb: return "Rec709Srgb (Rec.709 primaries, sRGB transfer)";
    case ExportTargetSpace::Rec709Bt709: return "Rec709Bt709 (Rec.709 primaries, BT.709 OETF)";
  }
  return "?";
}

inline int exportBitDepthBits(ExportBitDepth d) {
  return d == ExportBitDepth::Sixteen ? 16 : 8;
}

// The primaries each target space is defined against -- the value
// encodeLinearImage() compares with the document's own working space. All
// three are kRec709Primaries today; this function exists so that stays a
// single lookup rather than an assumption spread through the encoder.
inline Primaries exportTargetPrimaries(ExportTargetSpace) { return kRec709Primaries; }

// The outcome of an encode. `ok` and `error` are mutually exclusive by
// construction: `error` is non-empty exactly when `ok` is false, and it
// always names the specific thing that was refused (which format, which
// depth, which mismatch) rather than a generic "export failed" -- that
// specificity is PRD B6's actual requirement, not a nicety.
struct ExportResult {
  bool ok = false;
  // Encoded file bytes, ready to write verbatim. Empty when !ok.
  std::vector<uint8_t> bytes;
  // Non-empty exactly when !ok.
  std::string error;
};

// Flattens `doc` to one straight-alpha, linear-light float RGBA image the
// size of the document's canvas.
//
// Returns a DecodedImage -- io/ImageDecode.hpp's type, reused rather than
// duplicated, because its documented contract ("linear-light float RGBA,
// straight (non-premultiplied) alpha, row-major top-to-bottom, no row
// padding") is exactly, to the letter, what this stage produces. Import and
// export therefore meet in the middle on one shared representation, which
// is also what makes a decode(export(doc)) round-trip test a direct
// comparison rather than a conversion exercise.
//
// Two things this does, in this order:
//
//  1. Sums every RGB-kind layer's premultiplied tile contents into the
//     canvas. This is a plain sum, NOT Porter-Duff "over" compositing:
//     there is no blend-mode/compositing implementation anywhere in this
//     codebase yet (Phase 5 step 2, core/Blend), so there is nothing
//     correct to fake here. It is the right sum for *today's* invariant of
//     at most one layer with actually-painted content at a given point --
//     summing zero or one contribution needs no blending math at all. This
//     is the identical decision core/Probe.cpp's sampleAllLayers path
//     already made and documented, deliberately mirrored rather than
//     re-litigated; the moment a second layer can hold content at the same
//     pixel, both places need real per-layer alpha-under compositing, and
//     both say so.
//
//  2. Un-premultiplies the summed result, with the same `a <= 0 ->
//     {0,0,0,0}` guard core/Probe.cpp's unpremultiply() uses. Summing in
//     premultiplied space and un-premultiplying once at the end is the
//     correct order for the same reason core/Probe.cpp gives at length: an
//     alpha-0 texel must contribute "no colour", not "black at full
//     weight".
//
// Only iterates the tiles that actually exist (TileStore's own
// begin()/end() over occupied tiles), never a grid across the canvas, and
// never allocates -- an unpainted region simply stays transparent black,
// exactly the implicit value core::Tile gives an unwritten texel. Tile
// content outside the canvas rectangle (a layer placed partly off-canvas,
// or a tile that overhangs the right/bottom edge) is clipped away here:
// export writes the document's canvas, not its content's bounding box.
//
// Returns a DecodedImage with width == 0 (valid() == false) for a document
// with a non-positive width or height. A document with no layers, or with
// no RGB-kind layer, is NOT an error -- it flattens to a fully transparent
// canvas, which is a legitimate thing to export.
DecodedImage flattenDocumentToLinear(const Document& doc);

// Encodes straight-alpha, linear-light `img` -- defined against
// `sourceSpace`'s primaries -- into `format` file bytes, applying
// `targetSpace`'s transfer function and quantizing to `bitDepth`.
//
// `targetSpace` and `bitDepth` are required parameters with no defaults;
// see this header's "explicitly, never silently" section for why that is
// the requirement and not a style preference.
//
// Per channel: RGB gets the target space's transfer function applied, alpha
// does not (alpha is opacity, not light). Then every channel is clamped to
// [0, 1] and quantized to `bitDepth` bits by round-half-away-from-zero
// (`floor(v * max + 0.5)`), the same rounding io/ImageDecode's inverse
// (`sample / max`) reads back.
//
// On the [0,1] clamp: linear working values can legitimately exceed 1.0
// (color/Space.hpp's transfer functions deliberately do not clamp, since
// "whether to clamp is a display/export policy decision"). This is that
// policy decision, made here where it belongs: an integer file format has
// no representation for a value above full scale, so highlights above 1.0
// clip. That is a property of asking for an integer format, not a silent
// bit-depth truncation -- PRD B6 is about 16- and 32-bit files being
// written at their requested depth, which is exactly what the depth checks
// below enforce. Exporting >1.0 values losslessly needs a float format
// (EXR), which is PLAN.md step 2's OIIO territory.
//
// Fails, with a specific error naming what was refused, when:
//  - `img` is not valid() (nothing to encode);
//  - `sourceSpace`'s primaries differ from `targetSpace`'s (see this
//    header's scope-decision section -- this build converts transfer
//    functions, never primaries, and says so rather than ignoring it);
//  - `bitDepth` is Sixteen and `format` is not PNG (JPEG/TGA/BMP are
//    8-bit-per-channel as written here -- PRD B6);
//  - `format` is JPEG and `img` contains any pixel with alpha < 1 (JPEG has
//    no alpha channel; see this header's Alpha section);
//  - the underlying stb_image_write encoder itself refuses the buffer.
ExportResult encodeLinearImage(const DecodedImage& img, const WorkingSpace& sourceSpace,
                               ExportFormat format, ExportTargetSpace targetSpace,
                               ExportBitDepth bitDepth);

// The Document-level export PLAN.md Phase 4 step 1 asks for:
// flattenDocumentToLinear() followed by encodeLinearImage(), with the
// document's own `workingSpace` supplied as the source space. All three of
// format, target space and bit depth are required.
ExportResult exportDocument(const Document& doc, ExportFormat format,
                            ExportTargetSpace targetSpace, ExportBitDepth bitDepth);

// exportDocument() plus writing the resulting bytes to `path`. Returns
// false and, if `errorOut` is non-null, the specific reason -- either the
// encode's own error string verbatim (so a refused depth/primaries request
// reads identically whether the caller asked for bytes or a file) or an
// fopen/fwrite failure naming the path.
//
// Nothing is written to `path` unless the encode succeeded in full: the
// bytes are produced in memory first, so a refused request never leaves a
// truncated or partially written file behind.
bool exportDocumentToFile(const Document& doc, const std::string& path, ExportFormat format,
                          ExportTargetSpace targetSpace, ExportBitDepth bitDepth,
                          std::string* errorOut = nullptr);

// Builds a minimal, valid 16-bit RGBA PNG in memory from a top-to-bottom
// RGBA16 pixel array (`rgba` holds width*height*4 samples in native host
// byte order; they are written out big-endian, PNG's on-the-wire order for
// bit depths above 8).
//
// This is the only way PRD B6's 16-bit requirement and PRD I1's "no
// optional dependency" can both hold at once. stb_image_write's PNG writer
// is 8-bit-per-channel only -- there is no public 16-bit write entry point
// in the vendored third_party/stb_image_write.h -- so a 16-bit export
// otherwise needs a second image library, which I1 forbids. It reuses
// stb_image_write's own public stbi_zlib_compress() for the IDAT payload
// (Export.cpp is the translation unit that defines
// STB_IMAGE_WRITE_IMPLEMENTATION) but needs its own CRC32: stb's crc32
// helper is `static`, internal to its own translation unit, so it isn't
// reachable from here.
//
// This writer began life inside app/SelfTest.cpp as buildMinimal16BitPng(),
// where it existed only to generate a 16-bit *decode* fixture. B6 makes
// 16-bit export a P0 production requirement, so it lives here now and
// --selftest calls this one -- there is exactly one 16-bit PNG writer in
// the binary, not a production copy and a test copy that can drift apart.
//
// No filtering (every scanline uses filter type 0, None) and no interlace:
// this is a correctness-first writer, not a size-optimised one. Returns an
// empty vector for a zero-sized image or a null pixel pointer.
std::vector<uint8_t> encodePng16(uint32_t width, uint32_t height, const uint16_t* rgba);

}  // namespace np
