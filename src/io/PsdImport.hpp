#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/Document.hpp"

// io/PsdImport -- a hand-written, dependency-free reader for **layered**
// `.psd` files, producing a `Document` with one `Layer` per PSD layer.
//
// --- The gap this closes ---------------------------------------------------
//
// io/OiioBackend.hpp has said since PLAN.md Phase 4 step 2 that its PSD
// support is "flattened PSD read: yes" and that layered PSD was out of
// scope -- OpenImageIO's `psd` plugin decodes only the file's composite
// image data, the same bytes any other application would show as a
// thumbnail, and never touches the Layer and Mask Information section at
// all. That is confirmed by reading OiioBackend.cpp itself, not assumed: it
// asks OpenImageIO to open the file and read its default subimage, which is
// the merged/composite the "Image Data Section" of the format carries, and
// nothing in that call graph ever looks at the fourth section of the file.
// So today, and unchanged by this module's own existence, a layered PSD
// dropped on `File > Open` decodes to a `Document` with exactly one RGB
// layer holding the flattened picture -- correct pixels, wrong layer count,
// and every layer name, blend mode and visibility flag silently discarded.
// That is this module's measured baseline, not a guess: OiioBackend.hpp's
// own header states it, and nothing between here and there disputes it.
//
// app/OpenAnyFile.hpp names the seam this module fills, in its own words,
// months before this module existed: "A decoder returning a `Document` with
// N layers replaces this one call and nothing else in this function:
// everything below is per-document." `importPsd()` below is that decoder;
// app/OpenAnyFile.cpp is the one call site that changed to reach it.
//
// --- What this module is built from, and what it is not --------------------
//
// Implemented from Adobe's published "Photoshop File Formats Specification"
// (the public developer document Adobe itself distributes, most recently
// mirrored at https://www.adobe.com/devnet-apps/photoshop/fileformatashtml/
// -- fetched for this work from a byte-identical GitHub mirror,
// github.com/prcaen/psd-reader, after Adobe's own host would not respond to
// this environment) and from expired US patents 5,347,620 (compositing) and
// 6,373,490 (layer effects), neither of which this module needed: nothing
// here implements a blend formula (core/Blend already owns that; this module
// only *names* a PSD blend key against core/Blend's own enum) or a layer
// effect (drop shadow, glow, ... -- out of scope, named as such below).
//
// **Two framing facts this module depends on were cross-checked against an
// independent, non-Adobe, non-Krita source**, because the published spec's
// own prose is ambiguous or silent on both and getting either wrong would
// produce a document that opens without error and is confidently backwards:
//
//  1. **Layer stacking order.** The spec never states whether the first
//     layer record in the file is the top or the bottom of the stack.
//     psd-tools (github.com/psd-tools/psd-tools, MIT, independently
//     maintained, no relation to this codebase and not vendored into it)
//     settles it in its compositor: `composite/composite.py` reads
//     `bottom = psd[0]` when it needs the backdrop-most layer for a
//     knockout calculation -- i.e. index 0, the FIRST layer record read
//     from the file, IS the bottom of the stack. That is exactly
//     core/Document.hpp's own convention for `Document::layers` ("index 0
//     is the bottom of the stack"), so `importPsd()` appends layer records
//     to `document.layers` in the order it reads them, with **no reversal
//     anywhere in this module** -- and says so here because a reversal is
//     the natural first guess and would be wrong.
//  2. **The layer record `flags` byte's "visible" bit is inverted from its
//     own name in the spec table.** Adobe's own text reads "bit 1 =
//     visible", which a literal reading takes as "bit set means visible".
//     psd-tools' `LayerFlags.read()` computes
//     `visible = not bool(flags & 2)`, carrying the comment "# why 'not'?"
//     at the exact line that inverts it -- an independent implementer
//     hitting the identical surprise and choosing to leave the question
//     mark in rather than smooth it over. This module follows the same
//     inversion (see `kFlagHidden` in the .cpp): bit 1 SET means the layer
//     is **hidden**. Getting this backwards would show every hidden layer
//     and hide every visible one, silently and consistently, which is
//     precisely the kind of "confidently wrong" result no fixture the
//     author writes for their own reader can be trusted to catch -- so it
//     is recorded here as a fact that was checked, not assumed, and the
//     source that settled it is named.
//
// **Krita's source was not read for this module** (its GPL license forbids
// it here, per this project's own licensing rule) **and neither were the two
// in-force patents named in this project's brief.** Nothing above depended
// on either: stacking order and the flags inversion are both facts about a
// public file layout, corroborated from an independently-licensed reader's
// *behaviour*, not copied from anyone's implementation.
//
// --- Scope: what a first landing handles, and what it refuses by name ------
//
// **Version.** PSD (`8BPS`, version 1) only. **PSB, the Large Document
// Format (version 2), is refused by name, not misparsed.** PSB widens
// several fields from 4 to 8 bytes -- the layer-and-mask-info section
// length, the layer-info section length, each channel's own compressed-data
// length, and (only for a specific 13-key whitelist: `LMsk`, `Lr16`,
// `Lr32`, `Layr`, `Mt16`, `Mt32`, `Mtrn`, `Alph`, `FMsk`, `lnk2`, `FEid`,
// `FXid`, `PxSD`) an Additional Layer Information block's own length --
// while every *other* field keeps its PSD width, including the file
// header's own width/height. Implementing that correctly needs the whole
// whitelist wired through the tagged-block walk, and there is no way to
// validate the result against a real PSB (this environment has none, and
// PSB exists specifically for documents too large for this module's author
// to hand-author a convincing one). Given PRD-equivalent guidance to
// "support it or reject it by name" explicitly either way, refusing is the
// honest choice here: a wrong guess at 8-byte field widths would silently
// misread every subsequent byte in the section, which is exactly the
// PSD-parsed-as-PSB failure mode this file's own name warns against turned
// inside out. The version word is read first and checked before anything
// else in the file is trusted, so a PSB is refused **by name, immediately**
// -- "this build reads PSD (version 1) only" -- never partially parsed.
//
// **Compression.** Raw (0) and RLE/PackBits (1) are read. **ZIP (2) and
// ZIP-with-prediction (3) are refused by name, for the whole file, rather
// than attempted.** This project has no zlib dependency anywhere in its
// tree (`grep -r zlib` finds nothing), and the brief for this module says
// explicitly to keep it dependency-free -- vendoring zlib (or a from-scratch
// DEFLATE decoder, a much larger and much easier to get subtly wrong
// undertaking than PackBits) to read a compression mode most real-world PSD
// exporters do not even default to was judged not worth the weight for a
// first landing. Refused **for the whole file**, not silently skipped for
// just the ZIP-compressed layer: a document that opened missing one layer
// with no indication would look like it worked, and this codebase's
// existing refusal discipline (io/Descriptor.hpp: "a refusal is total";
// io/NpaintFile: "no half-built document") is followed here rather than
// invented fresh.
//
// **Colour mode.** RGB (mode 3) only. Bitmap, Grayscale, Indexed, CMYK,
// Multichannel, Duotone and Lab are refused by name, naming the mode
// Photoshop wrote. None is silently coerced into RGB -- a CMYK file's four
// channels are not an RGB file's three with an extra one ignored, and
// guessing a conversion matrix here would be exactly the kind of
// "plausible rather than true" decomposition core/Blend.hpp's own `Mix`
// comment already rejected once, for a different feature.
//
// **Depth.** 8- and 16-bit integer only, per this brief's minimum. 1-bit
// (Bitmap) and 32-bit (float) are refused by name.
//
// **Colour space.** PSD's integer samples are sRGB-encoded; this
// application is linear rgba16float end to end (PRD B6). Every RGB sample
// this module decodes is linearised through `srgbDecode()`
// (color/Space.hpp) -- the same function io/ImageDecode.cpp's
// `decodeChannelToLinear()` calls for every other untagged 8-/16-bit format
// this codebase reads, so PSD gets the identical, already-argued assumption
// rather than a second, independently-invented one. Alpha is never
// gamma-encoded and passes straight through as linear opacity, matching
// io/ImageDecode.cpp's own convention.
//
// **What is read from each layer.** The rectangle, the channel data (R, G,
// B and the transparency-mask alpha channel, id -1 -- a user-supplied layer
// mask, ids -2/-3, is walked past correctly to stay aligned with the file
// but its pixels are not imported into `Layer::mask`; this landing produces
// pixels and stacking, not masks), the blend mode key, the opacity byte,
// the visibility bit, the clipping byte (mapped to `Layer::clipped`, PRD C9
// -- PSD's "non-base" clipping is the identical relationship
// core/Layer.hpp's own `clipped` member documents: a run of non-base layers
// clips to the one base beneath the run, not to each other progressively),
// and the name -- preferring the Unicode `luni` Additional Layer
// Information block over the legacy Pascal-string name when both are
// present, exactly as Photoshop itself has since version 5.0. Layer
// effects (`lrFX`), adjustment-layer parameters, smart-object links, vector
// masks, and the group/folder structure (`lsct`) are none of them
// interpreted: a group's own boundary marker layers import as ordinary
// (typically near-empty) layers rather than as `LayerKind::Group` with real
// membership, which is a stated scope limit rather than a silent gap --
// group reconstruction is real, separate work this landing does not
// attempt.
//
// **Blend mode mapping.** PSD's blend-mode key maps onto this codebase's
// `core::BlendMode` (core/Blend.hpp) where an equivalent exists --
// `norm`->Normal, `mul `->Multiply, `scrn`->Screen, `dark`->Min (Photoshop's
// Darken is an exact per-channel minimum, the same arithmetic `BlendMode::
// Min` implements), `lite`->Max (Lighten is the per-channel maximum,
// symmetrically) -- and is reported by name and imported as Normal,
// **explicitly, with a warning naming the PSD key**, for every key with no
// equivalent (`over`/Overlay, `sLit`/Soft Light, `hLit`/Hard Light, and the
// rest of Photoshop's non-separable and second-generation modes). Never a
// silent substitution: `PsdImportResult::warnings` names every layer whose
// blend mode was not carried across, the same "say what was dropped rather
// than approximate it" discipline io/AbrBrushes.hpp already holds itself to
// for a different Photoshop artifact.
//
// --- What has NOT been verified, stated plainly -----------------------------
//
// **No genuine Photoshop-authored `.psd` or `.psb` file was available on the
// machine this was written on, and none was available to test against.**
// Every fixture `--selftest` parses through this module is written by a
// matching writer in the test code itself (see app/selftest/PsdImport.cpp),
// so passing proves this reader agrees with this author's reading of the
// published specification -- corroborated, for the two facts named above,
// against an independent third-party reader's *behaviour* -- and does
// **not** prove agreement with what Photoshop itself actually emits. That
// gap is real, it is the same gap io/Descriptor.hpp and io/AbrBrushes.hpp
// already carry for the identical reason, and it closes only when a real
// file is opened and the reviewer can compare the panel of a genuine
// Photoshop document against what this module built from its bytes.
namespace np {

// One PSD import's outcome.
struct PsdImportResult {
  bool ok = false;

  // Set only when `!ok`, and only for one specific reason: the file's Layer
  // and Mask Information section named zero layers (either the section
  // itself is empty/absent, or its own layer count is zero) -- a legitimate
  // state for a PSD saved with "Maximize Compatibility" off, which carries
  // only the flattened composite. app/OpenAnyFile.cpp reads this flag to
  // decide whether to fall back to the pre-existing flattened
  // `decodeImageLinear()` path (correct: there is no layer data to have
  // lost) or to refuse the file outright (every other `!ok`, where layer
  // data existed and this module could not read it -- falling back there
  // would silently discard real content and call it success).
  bool noLayerData = false;

  // Non-empty exactly when `!ok`. Names the byte offset and the reason, in
  // the style io/Descriptor.hpp and io/AbrBrushes.hpp already use for a
  // foreign binary format this project did not write.
  std::string error;

  // Valid only when `ok`. One `Layer` per PSD layer record, `document.width`
  // / `document.height` from the file header, `document.workingSpace`
  // default-constructed (kRec709Primaries -- the same choice
  // io/ImageIO.cpp's `openImageAsDocument()` makes for every other format,
  // since PSD's sRGB-encoded samples are linearised against the identical
  // primaries).
  Document document;

  // Non-fatal notes: an unmapped blend mode (naming the layer and the PSD
  // key), and nothing else in this landing -- there is no adjustment-layer,
  // effect, group or vector-mask drop to report because none of those are
  // attempted at all (a stated scope limit is not the same thing as a
  // per-file surprise, so it lives in this header's comment rather than in
  // every result's `warnings`).
  std::vector<std::string> warnings;
};

// Parses `bytes` as a PSD file and builds a layered `Document` from its
// Layer and Mask Information section.
//
// Reads no byte outside `bytes`, for any content of `bytes` whatsoever --
// the same contract io/Descriptor.hpp and io/AbrBrushes.hpp hold themselves
// to, and for the same reason: this parses a file format from outside the
// project, and the only property that matters more than getting the pixels
// right is not reading past the end of a buffer that happens to hold them.
PsdImportResult importPsd(std::span<const uint8_t> bytes);

}  // namespace np
