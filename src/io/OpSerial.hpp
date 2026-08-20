#pragma once

#include <string>
#include <string_view>

#include "core/OpStack.hpp"

// io/OpSerial (PLAN.md "Phase 5 -- Stack it", step 5: "Adjustment layers -- op
// stack against the composite below"). docs/document-format.md's `np:ops`;
// PRD I4, I10, I11, D18.
//
// **Why this module exists at all, stated first because it is a file-format
// decision and not a helper.** An Adjustment layer's entire content *is* its
// op stack: it holds no pixels, no latents and no mask samples of its own. A
// format that cannot carry an op stack therefore cannot carry an Adjustment
// layer at all -- not "carries it approximately", but loses everything the
// user authored on every save. PRD I11 forbids losing data silently and
// docs/document-format.md's whole stated purpose is round-trip fidelity, so
// step 5 could not do what step 3 could afford to do (warn by name and drop
// the grade, because a Pigment layer still had latents worth writing).
//
// --- The carrier, and why it is a hex string -----------------------------
//
// docs/document-format.md names `np:ops` a `UINT8[n]` blob and then corrects
// itself, measured: array-typed EXR header attributes written through this
// OpenImageIO are simply **absent** when the file is read back. Its own
// prescribed cheap fix is "a base64 or hex `string` attribute", and that is
// what this module produces.
//
// Hex rather than base64, deliberately, on three grounds:
//
//  * **A test fixture must not share the encoder's assumptions.** io/NpaintFile
//    already sets this precedent with its hand-built 52-byte PSD fixture, and
//    `--selftest` here decodes a hex string typed out by hand, byte by byte,
//    rather than only decoding what this module encoded. Hex is the encoding a
//    human can write correctly at a keyboard; base64's 3-byte grouping and
//    padding rules are exactly the kind of thing a hand-built fixture would
//    get wrong in a way that made the test agree with a wrong decoder.
//  * **Size does not matter here.** Hex is 2x and base64 is 4/3x, over a
//    payload that is tens to a few hundred bytes for a realistic stack (the
//    largest single op this build can produce is a three-channel Curves at
//    `kMaxCurvePointsPerChannel` = 16 points, 390 bytes of body). The measured
//    ceiling is not close: a 200 000-character `string` attribute survives a
//    write/read cycle through this OpenImageIO intact (measured 2026-08-20,
//    same experiment that produced the zero-channel result io/NpaintFile
//    cites).
//  * **`exrheader` prints it.** A string attribute is legible in any EXR
//    header dump; docs/document-format.md §3.5's argument for this format is
//    precisely that a recoverer meets named channels and readable headers
//    rather than an opaque private block.
//
// --- The format ----------------------------------------------------------
//
// The attribute's value is
//
//     "npops1:" <hex>
//
// where `<hex>` is the payload below, two lowercase hex digits per byte.
//
// **The version lives in the prefix, not in the payload**, so a reader decides
// whether it understands the encoding before it decodes a single byte -- and a
// build that does not understand `npops2:` says so by name instead of
// misreading a payload whose framing changed. Bumping the format means
// changing the `1`.
//
// The payload is little-endian, and floats are IEEE-754 binary32 bit patterns.
// Little-endian because EXR itself is (the container this lives in), and bit
// patterns because a decimal round trip is a *nearly* exact one and this
// module's whole claim is that reopening a document gives back the grade that
// was authored.
//
//     u16  opCount
//     opCount x record:
//       u32  bodyLength           bytes of `body` that follow
//       body:
//         u16  opClassCode        0 PointA, 1 SpatialB, 2 StrokeC, 3 BakedD
//         u16  pointKindCode      0 Levels, 1 Curves, 2 Exposure,
//                                 3 Saturation, 4 Grayscale, 5 ChannelMixer
//         u8   enabled            0 or 1
//         u8   reserved           written 0; a non-zero value makes the whole
//                                 record unrecognised (see below)
//         params                  present only for opClassCode 0
//
// The class and kind codes are **explicit numbers, not the C++ enumerators'
// ordinals**, so appending a value to `core::OpClass` or `core::PointOpKind`
// cannot silently move the meaning of a file that already exists.
//
// Params, by kind, all f32:
//
//     Levels        3 x 5   (blackIn whiteIn gamma blackOut whiteOut)   60 B
//     Curves        3 x (u16 pointCount, pointCount x (x, y))     variable
//     Exposure      1       (stops)                                      4 B
//     Saturation    4       (scale, lumaWeights[3])                     16 B
//     Grayscale     3       (lumaWeights[3])                            12 B
//     ChannelMixer  12      (matrix, row-major)                         48 B
//
// --- The rule that makes PRD I10 true at the *entry* level ----------------
//
// `bodyLength` exists so that a record this build cannot interpret can be
// stepped over **and kept**. Such a record becomes an `Op` with
// `opClass == OpClass::Unknown` whose `unrecognised` member holds the body
// verbatim, sitting in its original position in the stack; writing the stack
// back emits those bytes unchanged. It is inert -- `OpStack::detectRuns()`
// treats every non-PointA entry as a run boundary, so it is never evaluated
// and never baked -- which is the correct behaviour for an operation whose
// meaning this build does not know.
//
// **A record is unrecognised when any of these holds**, and the rule is
// deliberately strict in the reader's own disfavour, exactly as
// io/NpaintFile's part-recognition test is:
//
//   * the class code is not 0..3;
//   * the class code is 0 and the kind code is not 0..5;
//   * `reserved` is not 0 (a newer build used the byte for something);
//   * the body's length is not **exactly** what this build's parse of that
//     class and kind consumes.
//
// That last one is the one worth arguing. A newer build that adds a field to
// `LevelsParams` produces a 64-byte Levels body where this build expects 66
// bytes' worth of framing; parsing the leading 60 and discarding the rest
// would be *guessing* that the leading fields kept their meaning, and would
// silently drop the new field on the next save. Refusing to interpret it, and
// carrying it whole, loses nothing and claims nothing.
//
// --- What is deliberately not here ---------------------------------------
//
// **`np:docOps`.** The carrier now exists for it, but `core::Document` still
// has no document-level op stack to put in one (`app::AppState::opStack` is
// the global GPU-previewed grade and stays there), so there is nothing to
// serialise. io/NpaintFile.hpp's deferral list says so.
//
// **A human-readable, diffable action file.** PLAN.md Phase 19 step 1
// (`ops/Action`) writes an op stack out as a named, diffable artefact for
// batch replay. That is a different format with a different audience -- it is
// read and edited by people, this one is read by a machine and has to be
// bit-exact -- and building one to serve both would compromise both. This
// module is not that module and does not pre-empt it.
//
// **Compression.** A stack is hundreds of bytes.
namespace np {

// The version tag every value produced here begins with, including its colon.
// Exposed so io/NpaintFile and `--selftest` can name it rather than spelling
// the literal a second time.
inline constexpr const char* kOpStackSerialPrefix = "npops1:";

// `ops` as an `np:ops` attribute value: `kOpStackSerialPrefix` followed by the
// payload in lowercase hex. Never fails and never returns an empty string --
// an empty stack serialises to a well-formed zero-count payload
// (`npops1:0000`). io/NpaintFile still does not *write* the attribute for an
// empty stack, because an empty `string` attribute is not the same thing and
// because a document with no grades must produce the bytes it produced before
// this step existed.
std::string serializeOpStack(const OpStack& ops);

// The inverse. Returns false, leaving `*out` untouched, only when the value is
// malformed *as a container*: a prefix this build does not recognise, an odd
// or non-hex payload, a truncated record, or trailing bytes after the declared
// op count. An op whose class or kind this build does not know is **not** a
// failure -- it decodes to an `OpClass::Unknown` entry carrying its bytes, per
// this header's PRD I10 rule.
//
// `errorOut`, when non-null, receives a sentence naming what was wrong and
// where, in the io/Export refusal style every other refusal in this codebase
// follows.
bool deserializeOpStack(std::string_view value, OpStack* out, std::string* errorOut = nullptr);

}  // namespace np
