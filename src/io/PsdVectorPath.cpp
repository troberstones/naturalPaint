#include "io/PsdVectorPath.hpp"

#include <algorithm>
#include <utility>

// io/PsdVectorPath -- decodePsdPathRecords(). The wire format is argued at
// length in io/PsdVectorPath.hpp and docs/psd-vector-shapes.md; this file is
// the mechanics, kept small on purpose (step 1 of that doc's plan -- see
// "composePsdSubPaths()", a separate translation unit, for step 2).
namespace np {
namespace {

// Big-endian field reads. Every call site below reads inside a record slot
// whose full `kPsdPathRecordBytes` have already been proven present -- the
// record count is a floor division of the bytes actually in `block`, never
// the file's own claim about itself -- so these take no bounds argument.
uint16_t readU16(std::span<const uint8_t> b, size_t at) noexcept {
  return static_cast<uint16_t>((static_cast<uint16_t>(b[at]) << 8) | b[at + 1]);
}

uint32_t readU32(std::span<const uint8_t> b, size_t at) noexcept {
  return (static_cast<uint32_t>(b[at]) << 24) | (static_cast<uint32_t>(b[at + 1]) << 16) |
         (static_cast<uint32_t>(b[at + 2]) << 8) | static_cast<uint32_t>(b[at + 3]);
}

int16_t readI16(std::span<const uint8_t> b, size_t at) noexcept {
  return static_cast<int16_t>(readU16(b, at));
}

int32_t readI32(std::span<const uint8_t> b, size_t at) noexcept {
  return static_cast<int32_t>(readU32(b, at));
}

// A knot coordinate: signed 8.24 fixed point, `raw / 2^24` a fraction of
// `dimension`. io/PsdVectorPath.hpp names the file that proved both the
// sign and the vertical-by-height/horizontal-by-width split.
float fixedToCoord(int32_t raw, uint32_t dimension) noexcept {
  constexpr double kInv2Pow24 = 1.0 / static_cast<double>(1u << 24);
  return static_cast<float>(static_cast<double>(raw) * kInv2Pow24 * static_cast<double>(dimension));
}

// PSD's own record-selector numbering (Adobe's Path resource layout,
// mirrored by psd-tools -- docs/psd-vector-shapes.md's "Where the geometry
// is").
enum PsdPathSelector : uint16_t {
  kSelClosedLength = 0,
  kSelClosedLinked = 1,
  kSelClosedUnlinked = 2,
  kSelOpenLength = 3,
  kSelOpenLinked = 4,
  kSelOpenUnlinked = 5,
  kSelPathFillRule = 6,
  kSelClipboard = 7,
  kSelInitialFillRule = 8,
};

// Maps a subpath-length record's raw int16 operation to `PsdPathOp`.
// **A named switch, not `static_cast<PsdPathOp>(raw)`**, even though the
// enumerators now carry PSD's own values: a cast cannot tell a recognised
// operation from an unknown one, and `opKnown` is the whole point.
PsdPathOp mapPathOp(int16_t raw, bool& known) noexcept {
  switch (raw) {
    case 0: known = true; return PsdPathOp::Exclude;
    case 1: known = true; return PsdPathOp::Union;
    case 2: known = true; return PsdPathOp::Subtract;
    case 3: known = true; return PsdPathOp::Intersect;
    case -1: known = true; return PsdPathOp::MergeWithPrevious;
    default: known = false; return PsdPathOp::Union;
  }
}

}  // namespace

bool decodePsdPathRecords(std::span<const uint8_t> block, uint32_t docWidth, uint32_t docHeight,
                          PsdPathStream& out, std::string& error) {
  out = PsdPathStream{};
  error.clear();

  if (block.size() < kPsdPathBlockHeaderBytes) {
    error = "PSD path block is shorter than its 8-byte header";
    return false;
  }

  // version/flags: read past, not validated. Every block measured so far is
  // version 3, flags 0 (io/PsdVectorPath.hpp), and neither value changes how
  // the record stream that follows is parsed.
  (void)readU32(block, 0);
  (void)readU32(block, 4);

  const size_t payload = block.size() - kPsdPathBlockHeaderBytes;
  const size_t recordCount = payload / kPsdPathRecordBytes;  // pad remainder discarded

  // The subpath currently being assembled. `hasPending` is false both
  // before the first subpath-length record and right after one is flushed.
  bool hasPending = false;
  SubPath pendingSub;
  PsdPathOp pendingOp = PsdPathOp::Union;
  bool pendingOpKnown = true;
  int16_t pendingRawOp = 1;
  uint16_t pendingDeclaredKnots = 0;

  auto flushPending = [&]() {
    if (!hasPending) return;
    if (pendingSub.anchors.size() != pendingDeclaredKnots) {
      // Not fatal, and not rare under a hostile or truncated block: the
      // subpath-length record's knot count is the file's CLAIM, and this
      // module never allocates from it -- it only ever pushes an anchor for
      // a knot record it actually saw. A mismatch means the file lied or
      // ran out; either way the geometry decoded is exactly what was
      // present, never a padded or truncated guess.
      out.warnings.push_back("subpath declared " + std::to_string(pendingDeclaredKnots) +
                              " knot(s) but " + std::to_string(pendingSub.anchors.size()) +
                              " were present; the ones present were decoded");
    }
    PsdSubPath ps;
    ps.sub = std::move(pendingSub);
    ps.op = pendingOp;
    ps.opKnown = pendingOpKnown;
    ps.rawOp = pendingRawOp;
    out.subpaths.push_back(std::move(ps));
    pendingSub = SubPath{};
    hasPending = false;
  };

  for (size_t i = 0; i < recordCount; ++i) {
    const size_t at = kPsdPathBlockHeaderBytes + i * kPsdPathRecordBytes;
    const uint16_t selector = readU16(block, at);

    switch (selector) {
      case kSelPathFillRule:
      case kSelInitialFillRule:
        // All-zero in every file examined, and neither carries the
        // even-odd/nonzero choice (io/PsdVectorPath.hpp) -- not a warning.
        flushPending();
        break;

      case kSelClipboard:
        // Never seen in a file this module has been checked against
        // (docs/psd-vector-shapes.md), and carries no geometry this module
        // reads. Named so a file that does carry one is visible rather than
        // silently dropped, unlike 6/8 above.
        flushPending();
        out.warnings.push_back("record " + std::to_string(i) +
                                ": clipboard record (selector 7) skipped, not read");
        break;

      case kSelClosedLength:
      case kSelOpenLength: {
        flushPending();
        const uint16_t knotCount = readU16(block, at + 2);
        const int16_t rawOp = readI16(block, at + 4);
        bool known = false;
        const PsdPathOp op = mapPathOp(rawOp, known);
        hasPending = true;
        pendingSub = SubPath{};
        pendingSub.closed = (selector == kSelClosedLength);
        // A hint, not an allocation from an attacker's number: capped by
        // the records actually left in the block.
        pendingSub.anchors.reserve(std::min<size_t>(knotCount, recordCount - i));
        pendingOp = op;
        pendingOpKnown = known;
        pendingRawOp = rawOp;
        pendingDeclaredKnots = knotCount;
        if (selector == kSelOpenLength) out.sawOpenSubPath = true;
        if (!known) {
          out.warnings.push_back("record " + std::to_string(i) +
                                  ": subpath path-operation " + std::to_string(rawOp) +
                                  " is not one of the five known values; geometry kept, "
                                  "operation unknown");
        }
        break;
      }

      case kSelClosedLinked:
      case kSelClosedUnlinked:
      case kSelOpenLinked:
      case kSelOpenUnlinked: {
        const bool isClosedKnot = (selector == kSelClosedLinked || selector == kSelClosedUnlinked);
        const bool smooth = (selector == kSelClosedLinked || selector == kSelOpenLinked);
        if (!hasPending) {
          // A knot record with no subpath-length record ahead of it: the
          // file is malformed, but the six coordinates that follow are
          // still well-formed, so this starts an implicit subpath rather
          // than discarding them.
          hasPending = true;
          pendingSub = SubPath{};
          pendingSub.closed = isClosedKnot;
          pendingOp = PsdPathOp::Union;
          pendingOpKnown = false;
          pendingRawOp = 0;
          pendingDeclaredKnots = 0;
          out.warnings.push_back("record " + std::to_string(i) +
                                  ": knot record with no preceding subpath-length record; "
                                  "started an implicit subpath");
        }
        const int32_t inY = readI32(block, at + 2);
        const int32_t inX = readI32(block, at + 6);
        const int32_t ptY = readI32(block, at + 10);
        const int32_t ptX = readI32(block, at + 14);
        const int32_t outY = readI32(block, at + 18);
        const int32_t outX = readI32(block, at + 22);
        Anchor a;
        a.in = PathPoint{fixedToCoord(inX, docWidth), fixedToCoord(inY, docHeight)};
        a.pt = PathPoint{fixedToCoord(ptX, docWidth), fixedToCoord(ptY, docHeight)};
        a.out = PathPoint{fixedToCoord(outX, docWidth), fixedToCoord(outY, docHeight)};
        a.smooth = smooth;
        pendingSub.anchors.push_back(a);
        break;
      }

      default:
        flushPending();
        out.warnings.push_back("record " + std::to_string(i) +
                                ": unrecognised path record selector " +
                                std::to_string(selector) + ", skipped");
        break;
    }
  }
  flushPending();

  // pathIsFinite() is core/Path.hpp's documented precondition for anything
  // that walks this geometry downstream (the rasteriser, PathFlatten). It
  // can only fail here if `fixedToCoord()` produced a NaN or an Inf, and it
  // provably cannot: every input is a finite int32 read from `block` and a
  // finite uint32 dimension, combined with one multiply and one divide by a
  // compile-time nonzero constant -- no step here can manufacture 0*inf,
  // inf-inf, or a divide by a runtime zero. The check is kept anyway
  // because that proof is about *this* function's arithmetic today, not a
  // promise that survives every future edit to it, and core/Path.hpp asks
  // every producer of untrusted geometry to make it rather than trust its
  // own reasoning.
  Path probe;
  probe.subpaths.reserve(out.subpaths.size());
  for (const PsdSubPath& ps : out.subpaths) probe.subpaths.push_back(ps.sub);
  if (!pathIsFinite(probe)) {
    error = "decoded path geometry is not finite";
    out = PsdPathStream{};
    return false;
  }

  return true;
}

}  // namespace np
