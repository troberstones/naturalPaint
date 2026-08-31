#include "io/PsdImport.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "color/Space.hpp"
#include "core/Blend.hpp"
#include "core/LayerOps.hpp"  // makeGroupLayer() -- the receiving model this
                               // module maps `lsct` onto, not reinvented here
#include "core/Mask.hpp"
#include "core/Tile.hpp"
#include "io/AbrBrushes.hpp"  // checkedAdd() -- shared overflow-safe addition

// io/PsdImport implementation. Every design decision is argued in
// io/PsdImport.hpp; this file holds the mechanics.
namespace np {
namespace {

// --- A bounds-checked cursor, in the AbrBrushes.cpp/Descriptor.cpp mould --
//
// AbrBrushes.cpp reads by passing an explicit offset to each free function
// (`readU16(bytes, at, out)`); io/Descriptor.hpp instead threads one cursor
// through the whole parse ("every read goes through one cursor that checks
// left() before it moves"). PSD's own nesting is deeper than either of
// those files' -- header, then four sections, then per-layer records, then
// per-channel data, then per-layer tagged-info blocks -- so this file takes
// Descriptor's shape: a `Cursor` that advances only on a successful,
// `checkedAdd()`-guarded read and turns permanently invalid the first time
// one fails, so a long chain of reads can be written as a chain of `&&`
// rather than a ladder of `if (!x) return false;`. Every single byte access
// underneath it still goes through `checkedAdd()` before touching memory,
// so the safety property is identical to the free-function style; this is
// only a readability choice for the deeper nesting here.
class Cursor {
 public:
  explicit Cursor(std::span<const uint8_t> data) : data_(data) {}

  bool ok() const noexcept { return ok_; }
  size_t pos() const noexcept { return pos_; }
  size_t remaining() const noexcept { return ok_ ? data_.size() - pos_ : 0; }
  size_t size() const noexcept { return data_.size(); }
  // The whole underlying buffer, for a caller that needs to carve out a
  // sub-cursor scoped to fewer bytes than "the rest of the file" -- see
  // `readLayerRecord()`'s extra-data field, which must not let a corrupt
  // inner length field read into the NEXT layer record's bytes even though
  // those bytes are themselves perfectly in-bounds of `bytes` as a whole.
  std::span<const uint8_t> data() const noexcept { return data_; }

  // Jumps to an absolute offset that has already been proven in-bounds by
  // the caller (every call site below computes `target` as `pos() +
  // alreadyCheckedLength`). Never advances past `data_.size()`; doing so
  // marks the cursor invalid rather than silently clamping, so a caller's
  // bookkeeping bug shows up as a refusal instead of a wrong resync point.
  [[nodiscard]] bool seek(size_t target) noexcept {
    if (!ok_ || target > data_.size()) {
      ok_ = false;
      return false;
    }
    pos_ = target;
    return true;
  }

  [[nodiscard]] bool skip(size_t n) noexcept {
    size_t end = 0;
    if (!ok_ || !checkedAdd(pos_, n, end) || end > data_.size()) {
      ok_ = false;
      return false;
    }
    pos_ = end;
    return true;
  }

  [[nodiscard]] bool u8(uint8_t& out) noexcept {
    size_t end = 0;
    if (!ok_ || !checkedAdd(pos_, 1, end) || end > data_.size()) {
      ok_ = false;
      return false;
    }
    out = data_[pos_];
    pos_ = end;
    return true;
  }

  [[nodiscard]] bool u16(uint16_t& out) noexcept {
    size_t end = 0;
    if (!ok_ || !checkedAdd(pos_, 2, end) || end > data_.size()) {
      ok_ = false;
      return false;
    }
    out = static_cast<uint16_t>((data_[pos_] << 8) | data_[pos_ + 1]);
    pos_ = end;
    return true;
  }

  [[nodiscard]] bool u32(uint32_t& out) noexcept {
    size_t end = 0;
    if (!ok_ || !checkedAdd(pos_, 4, end) || end > data_.size()) {
      ok_ = false;
      return false;
    }
    out = (static_cast<uint32_t>(data_[pos_]) << 24) |
          (static_cast<uint32_t>(data_[pos_ + 1]) << 16) |
          (static_cast<uint32_t>(data_[pos_ + 2]) << 8) |
          static_cast<uint32_t>(data_[pos_ + 3]);
    pos_ = end;
    return true;
  }

  [[nodiscard]] bool i16(int16_t& out) noexcept {
    uint16_t bits = 0;
    if (!u16(bits)) return false;
    out = static_cast<int16_t>(bits);
    return true;
  }

  [[nodiscard]] bool i32(int32_t& out) noexcept {
    uint32_t bits = 0;
    if (!u32(bits)) return false;
    out = static_cast<int32_t>(bits);
    return true;
  }

  // `n` raw bytes, as a subspan into the original buffer (no copy). The
  // returned span's lifetime is `data_`'s, which outlives this whole parse
  // (the caller of `importPsd()` owns `bytes` for the call's duration).
  [[nodiscard]] bool bytes(size_t n, std::span<const uint8_t>& out) noexcept {
    size_t end = 0;
    if (!ok_ || !checkedAdd(pos_, n, end) || end > data_.size()) {
      ok_ = false;
      return false;
    }
    out = data_.subspan(pos_, n);
    pos_ = end;
    return true;
  }

  // Four raw bytes, verbatim, for a signature or a blend-mode key -- never
  // treated as text (some blend keys are Latin-1 mush like `hue `, not
  // valid UTF-8), only ever compared byte-for-byte.
  [[nodiscard]] bool fourcc(std::array<char, 4>& out) noexcept {
    std::span<const uint8_t> s;
    if (!bytes(4, s)) return false;
    out = {static_cast<char>(s[0]), static_cast<char>(s[1]), static_cast<char>(s[2]),
           static_cast<char>(s[3])};
    return true;
  }

 private:
  std::span<const uint8_t> data_;
  size_t pos_ = 0;
  bool ok_ = true;
};

bool fourccEquals(const std::array<char, 4>& fourcc, const char* lit) noexcept {
  return std::memcmp(fourcc.data(), lit, 4) == 0;
}

std::string fourccToString(const std::array<char, 4>& fourcc) {
  return std::string(fourcc.data(), 4);
}

// --- PackBits (Photoshop's per-scanline RLE) --------------------------------
//
// io/AbrBrushes.cpp's own `decodePackBits()` documents the algorithm and the
// "decode as one continuous stream rather than resynchronising at each row"
// choice at length; this is the identical algorithm, rewritten here rather
// than shared. AbrBrushes.cpp's version is `static` to that translation
// unit (anonymous namespace) and is sized for a `samp` block's own u16
// row-length table, which happens to be the same width PSD's (non-PSB) RLE
// channel data uses -- but the two call sites read genuinely different file
// formats, and reaching across translation units for one ~25-line function
// was judged not worth the coupling. `rows` is the channel's own height
// (`bottom - top`), and `expected` is `width * height * bytesPerSample`.
bool decodePackBits(std::span<const uint8_t> body, uint32_t rows, size_t expected,
                    std::vector<uint8_t>& out) {
  Cursor c(body);

  // The row-length table: `rows` big-endian u16 compressed-byte-counts,
  // summed for the total compressed byte count -- decoding the whole
  // concatenated stream in one pass rather than row by row, exactly the
  // choice AbrBrushes.cpp's own `decodePackBits()` argues (a well-formed
  // PackBits run never straddles a Photoshop-written row boundary, so the
  // two approaches agree on well-formed input, and this form still cannot
  // read past `body` on malformed input because every access below is
  // bounds-checked first).
  uint64_t total = 0;
  for (uint32_t i = 0; i < rows; ++i) {
    uint16_t rowLen = 0;
    if (!c.u16(rowLen)) return false;
    total += rowLen;
  }
  if (total > c.remaining()) return false;
  std::span<const uint8_t> stream;
  if (!c.bytes(static_cast<size_t>(total), stream)) return false;

  out.clear();
  out.reserve(expected);
  size_t p = 0;
  while (p < stream.size() && out.size() < expected) {
    const int8_t n = static_cast<int8_t>(stream[p]);
    ++p;
    if (n == -128) {
      continue;  // PackBits' own no-op control byte
    } else if (n < 0) {
      // Run: repeat the next byte (-n + 1) times.
      if (p >= stream.size()) return false;
      const size_t count = static_cast<size_t>(-static_cast<int>(n) + 1);
      const uint8_t b = stream[p];
      ++p;
      for (size_t k = 0; k < count && out.size() < expected; ++k) out.push_back(b);
    } else {
      // Literal: the next (n + 1) bytes, verbatim.
      const size_t count = static_cast<size_t>(n) + 1;
      size_t next = 0;
      if (!checkedAdd(p, count, next) || next > stream.size()) return false;
      out.insert(out.end(), stream.data() + p, stream.data() + p + count);
      p = next;
    }
  }
  return out.size() == expected;
}

// --- Blend mode mapping ------------------------------------------------------
//
// io/PsdImport.hpp argues each mapping (and the deliberate absence of the
// rest) at length; this table is just the wire keys next to the
// core::BlendMode each one maps to. Every key not listed here is reported by
// name and left as Normal -- see `mapBlendKey()`. **Not every row here is an
// EXACT match** -- `dark`/`lite` are; `lddg` is not; see each row's own
// comment and io/PsdImport.hpp's "Blend mode mapping" section for the full
// argument.
struct BlendKeyMap {
  const char* psdKey;  // exactly 4 bytes, including a trailing space where
                       // Photoshop pads a short key with one
  BlendMode mode;
};
constexpr BlendKeyMap kBlendKeyMap[] = {
    {"norm", BlendMode::Normal},
    {"mul ", BlendMode::Multiply},
    {"scrn", BlendMode::Screen},
    // Darken is an exact per-channel minimum and Lighten an exact per-channel
    // maximum of source and backdrop -- io/PsdImport.hpp's own header
    // derives why these two, alone among Photoshop's non-`norm` keys with no
    // literal core::BlendMode counterpart, are still an EXACT match rather
    // than an approximation.
    {"dark", BlendMode::Min},
    {"lite", BlendMode::Max},
    // Linear Dodge (Add) is `Cs + Cb`, exactly core::Blend.cpp's
    // `BlendMode::Plus` (additive light). **Not exact, unlike the two rows
    // above**: min/max are order-preserving, so they commute with any
    // monotone transfer function and agree with Photoshop whether the
    // addition happens before or after gamma encoding. Addition does not
    // commute with a transfer function -- Photoshop adds in gamma space by
    // default ("Blend RGB Colors Using Gamma 1.0" off) and this codebase
    // adds in linear light. That is the identical compromise `mul `/`scrn`
    // already ship with above, not a new one.
    {"lddg", BlendMode::Plus},
    // Stage 1 (docs/blend-mode-gaps.md): these 7 are NOT exact matches, unlike
    // `dark`/`lite` above. This codebase composites them in premultiplied,
    // LINEAR-LIGHT RGBA (core/Blend.hpp), while Photoshop's default is to
    // blend in whichever (usually gamma-encoded) space the document works in
    // -- the same reason a `mul`/`scrn`/`lddg`-style key would only be an
    // approximation rather than an exact match. Listed anyway, and not
    // reported as a mismatch, because "approximate but visually close" is a
    // materially better outcome than falling back to Normal.
    {"diff", BlendMode::Difference},  // Difference
    {"smud", BlendMode::Exclusion},   // Exclusion
    {"fsub", BlendMode::Subtract},    // Subtract
    {"lbrn", BlendMode::LinearBurn},  // Linear Burn
    {"div ", BlendMode::ColorDodge},  // Color Dodge
    {"idiv", BlendMode::ColorBurn},   // Color Burn
    {"fdiv", BlendMode::Divide},      // Divide
    // Stage 2's seven "light family" modes (docs/blend-mode-gaps.md). Unlike
    // `dark`/`lite` above, these are NOT exact matches in substance, even
    // though `mapBlendKey()` reports them as one (there is no third bucket
    // between "exact" and "no equivalent, warn and fall back to Normal", and
    // a mode this build genuinely composites belongs on this side of that
    // line, not the other). The approximation: this codebase composites in
    // LINEAR light and Photoshop's default compositing is GAMMA-space, and
    // none of Hard Light/Overlay/Vivid Light/Linear Light/Pin Light/Soft
    // Light/Hard Mix is invariant to that choice of space (unlike Darken/
    // Lighten's per-channel min/max above, which are). A PSD written by
    // Photoshop with one of these blend keys will therefore round-trip
    // through this importer with the right blend *mode* but not bit-exact
    // pixels -- the same caveat this build already carries for any
    // gamma-space blend, stated here rather than left implicit.
    {"hLit", BlendMode::HardLight},
    {"over", BlendMode::Overlay},
    {"vLit", BlendMode::VividLight},
    {"lLit", BlendMode::LinearLight},
    {"pLit", BlendMode::PinLight},
    {"sLit", BlendMode::SoftLight},
    {"hMix", BlendMode::HardMix},
    // Stage 3's six non-separable modes. These, like `lddg` (Linear Dodge),
    // are approximations rather than exact matches for the same reason: this
    // codebase blends in premultiplied LINEAR light, while Photoshop's own
    // Hue/Saturation/Color/Luminosity/Darker-Color/Lighter-Color operate in
    // gamma (display-encoded) space, so the same PSD file can composite
    // visibly differently between the two. `colr` and `lum ` are the two
    // that matter most in practice -- they are the modes actually present in
    // the user's own real PSD file (three `colr` layers) -- so getting those
    // two exactly right (mode selection, not the linear-vs-gamma gap, which
    // is a known, stated approximation) is worth more than the rest of this
    // table.
    //
    // Three of the six keys carry a trailing space, matching Photoshop's own
    // 4-byte padding of a 3-character key -- get it exactly right or
    // fourccEquals() silently fails to match real files.
    {"hue ", BlendMode::Hue},
    {"sat ", BlendMode::Saturation},
    {"colr", BlendMode::Color},
    {"lum ", BlendMode::Luminosity},
    {"dkCl", BlendMode::DarkerColor},
    {"lgCl", BlendMode::LighterColor},
};

// `std::nullopt`-free by design: every key maps to a BlendMode, and the
// bool return says whether that mapping was an EXACT one (found in the
// table above) or the "no equivalent, reported and left as Normal" fallback
// io/PsdImport.hpp promises. The caller uses the bool to decide whether to
// warn; it never changes the returned mode, since Normal is the answer
// either way (the fallback's `BlendMode::Normal` and `norm`'s own entry
// happen to produce the same value, which is what makes "exact or
// Normal-and-warn" a total function rather than a partial one).
BlendMode mapBlendKey(const std::array<char, 4>& key, bool& exactMatch) noexcept {
  for (const BlendKeyMap& entry : kBlendKeyMap) {
    if (fourccEquals(key, entry.psdKey)) {
      exactMatch = true;
      return entry.mode;
    }
  }
  exactMatch = false;
  return BlendMode::Normal;
}

// --- One parsed layer, before its pixels are packed into a Layer's tiles ---

struct ParsedChannel {
  int16_t id = 0;      // 0/1/2 = R/G/B; -1 = alpha; -2/-3 = a layer mask
  uint32_t length = 0;  // this channel's own byte count, PSD (non-PSB) width
};

// A layer record's own `lsct` ("Section divider setting") tag, if it carries
// one -- docs/psd-import-gaps.md section 3's wire layout. `type` 0 ("other")
// is an ordinary layer that merely happens to carry an lsct block (every
// non-group layer in `Testforautoflats 2.psd` does, always type 0) and is
// treated identically to a layer with no lsct block at all: `kNone` covers
// both. Only `kDivider` (type 3, the bounding-section marker that OPENS a
// group reading forward through the bottom-first record list) and `kHeader`
// (type 1 "open folder" / 2 "closed folder", which CLOSES and names the
// group) change how this module treats the record -- see `importPsd()`'s
// group-stack walk.
enum class LsctRole { kNone, kDivider, kHeader };

struct ParsedLayer {
  int32_t top = 0, left = 0, bottom = 0, right = 0;
  std::vector<ParsedChannel> channels;
  std::array<char, 4> blendKey{'n', 'o', 'r', 'm'};
  uint8_t opacity = 255;
  uint8_t clipping = 0;
  bool hidden = false;
  std::string name;      // luni, if present; else the Pascal name
  bool alphaLocked = false;  // lspf bit 0 ("Lock transparent pixels"); see
                              // the `lspf` case in readLayerRecord() for why
                              // bits 1/2 have no field here to land in.

  // Set only when this record's `lsct` additional-layer-info block declared
  // type 1, 2 or 3 -- see `LsctRole`'s own comment.
  LsctRole lsctRole = LsctRole::kNone;
  // The group's own blend key ("pass" for Photoshop's Pass Through, or an
  // ordinary blend-mode key for an isolated group), read from the `lsct`
  // block's optional second field. Only meaningful when `lsctRole ==
  // kHeader` AND `lsctHasBlendKey` is true -- the field is itself optional
  // (present only when the block's declared length is >= 12), and no sample
  // file omits it, but a hand-built or older-Photoshop-written file might.
  std::array<char, 4> lsctBlendKey{'p', 'a', 's', 's'};
  bool lsctHasBlendKey = false;

  // The 20-byte layer mask record (docs/psd-import-gaps.md section 1), raw
  // and un-resolved: `maskTop`/`maskLeft`/`maskBottom`/`maskRight` are the
  // WIRE values, which are either absolute document coordinates or an
  // offset from this layer's own (top, left) depending on `maskFlags` bit 0
  // -- resolving that is `importPsd()`'s job, not this parse step's, because
  // resolution needs `top`/`left` above, which belong to the same struct and
  // are already in scope there. `hasMask` false means this layer carries no
  // mask block at all (the common case, and every layer before this step).
  bool hasMask = false;
  int32_t maskTop = 0, maskLeft = 0, maskBottom = 0, maskRight = 0;
  uint8_t maskDefaultColor = 255;  // 255 = reveal outside the mask rect, 0 = hide
  uint8_t maskFlags = 0;           // bit 0 = relative to layer, bit 1 = disabled
};

// Photoshop's own thirteen Additional-Layer-Information keys that widen to
// an 8-byte length in PSB -- unused by this build (PSB is refused, see
// io/PsdImport.hpp), kept here as the record of the research this module's
// PSB decision rests on, so a future implementer does not have to re-derive
// it: LMsk, Lr16, Lr32, Layr, Mt16, Mt32, Mtrn, Alph, FMsk, lnk2, FEid,
// FXid, PxSD (Adobe's published spec, "Additional Layer Information";
// cross-checked against psd-tools' `TaggedBlock._BIG_KEYS`, which names the
// identical thirteen keys under their own long constant names).

// Reads one layer record's header (everything up to and including its own
// tagged-information blocks) starting at `c`'s current position, which must
// be the start of the rectangle. Does NOT read this layer's channel image
// data -- that lives later in the file, after every layer record, in the
// same order (io/PsdImport.hpp's "Layer stacking order" section already
// established the order channel data appears in is irrelevant to this
// module's own stacking decision; it still has to be walked in file order
// to reach the next layer record).
bool readLayerRecord(Cursor& c, ParsedLayer& layer, std::string& error) {
  if (!(c.i32(layer.top) && c.i32(layer.left) && c.i32(layer.bottom) && c.i32(layer.right))) {
    error = "layer record: truncated rectangle at byte " + std::to_string(c.pos());
    return false;
  }
  // A well-formed rectangle never inverts. This is one of the deliberately
  // adversarial fixtures in --selftest, and refusing the whole file rather
  // than clamping is the same "no half-built document" discipline
  // io/Descriptor.hpp and io/NpaintFile hold themselves to: a clamped
  // rectangle would decode SOME pixels for a layer whose own bytes never
  // described a valid one.
  if (layer.bottom < layer.top || layer.right < layer.left) {
    error = "layer record: inverted rectangle (top=" + std::to_string(layer.top) +
            " left=" + std::to_string(layer.left) + " bottom=" + std::to_string(layer.bottom) +
            " right=" + std::to_string(layer.right) + ") at byte " + std::to_string(c.pos());
    return false;
  }
  // Widened to int64_t before subtracting: `right`/`left`/`bottom`/`top` are
  // each independently-read int32_t values from an untrusted file, and nothing
  // above bounds them against each other -- `right = 2000000000, left =
  // -2000000000` passes the "not inverted" check just above while `right -
  // left` in 32-bit arithmetic is signed overflow (undefined behaviour). No
  // legitimate PSD layer is anywhere near this large (30,000px is the whole
  // canvas's own ceiling), so this also doubles as the "implausible size"
  // refusal PSB's higher 300,000px ceiling would otherwise need a separate
  // check for.
  const int64_t layerWidth64 = static_cast<int64_t>(layer.right) - static_cast<int64_t>(layer.left);
  const int64_t layerHeight64 = static_cast<int64_t>(layer.bottom) - static_cast<int64_t>(layer.top);
  if (layerWidth64 > 30000 || layerHeight64 > 30000) {
    error = "layer record: implausible size " + std::to_string(layerWidth64) + "x" +
            std::to_string(layerHeight64) + " at byte " + std::to_string(c.pos());
    return false;
  }

  uint16_t numChannels = 0;
  if (!c.u16(numChannels)) {
    error = "layer record: truncated channel count at byte " + std::to_string(c.pos());
    return false;
  }
  // 56 is the file header's own documented ceiling on the TOTAL channel
  // count of the image; no single layer can plausibly declare more. A
  // hostile count here is exactly the "claims more than the buffer holds"
  // shape this module's fuzz-adjacent tests target -- refused before a
  // single `ParsedChannel` is reserved, not after an oversized allocation.
  constexpr uint16_t kMaxChannelsPerLayer = 56;
  if (numChannels > kMaxChannelsPerLayer) {
    error = "layer record: implausible channel count " + std::to_string(numChannels) +
            " (max " + std::to_string(kMaxChannelsPerLayer) + ") at byte " +
            std::to_string(c.pos());
    return false;
  }
  layer.channels.reserve(numChannels);
  for (uint16_t i = 0; i < numChannels; ++i) {
    ParsedChannel ch;
    int16_t id16 = 0;
    uint32_t len = 0;
    if (!(c.i16(id16) && c.u32(len))) {
      error = "layer record: truncated channel info table at byte " + std::to_string(c.pos());
      return false;
    }
    ch.id = id16;
    ch.length = len;
    layer.channels.push_back(ch);
  }

  std::array<char, 4> sig{};
  if (!c.fourcc(sig) || !fourccEquals(sig, "8BIM")) {
    error = "layer record: blend mode signature is not '8BIM' at byte " + std::to_string(c.pos());
    return false;
  }
  if (!c.fourcc(layer.blendKey)) {
    error = "layer record: truncated blend mode key at byte " + std::to_string(c.pos());
    return false;
  }

  uint8_t flags = 0;
  uint8_t filler = 0;
  if (!(c.u8(layer.opacity) && c.u8(layer.clipping) && c.u8(flags) && c.u8(filler))) {
    error = "layer record: truncated opacity/clipping/flags at byte " + std::to_string(c.pos());
    return false;
  }
  // **Bit 1 of `flags` means HIDDEN, not visible**, despite the published
  // spec's own table calling it "visible" -- io/PsdImport.hpp's header
  // states the evidence (an independent reader's behaviour, not this
  // module's guess) and why it mattered enough to check rather than trust
  // the table's own English.
  constexpr uint8_t kFlagHidden = 0x02;
  layer.hidden = (flags & kFlagHidden) != 0;

  uint32_t extraLen = 0;
  if (!c.u32(extraLen)) {
    error = "layer record: truncated extra-data length at byte " + std::to_string(c.pos());
    return false;
  }
  const size_t extraStart = c.pos();
  size_t extraEnd = 0;
  if (!checkedAdd(extraStart, extraLen, extraEnd) || extraEnd > c.size()) {
    error = "layer record: extra-data length " + std::to_string(extraLen) +
            " runs past the end of the file (at byte " + std::to_string(extraStart) + ")";
    return false;
  }

  // Everything from here to `extraEnd` is parsed through a SUB-CURSOR
  // scoped to exactly `[extraStart, extraEnd)`, rather than continuing to
  // read through `c` against the whole file. That is not a style choice:
  // `c`'s own bounds check only ever proves a read stays inside the whole
  // file, and mask/blending-ranges/name are each fronted by their own
  // length field that this module does not otherwise validate against
  // anything narrower than that. A corrupt (but still in-file-bounds) mask
  // length would, read through `c` directly, walk straight past this
  // layer's own `extraEnd` and into the NEXT layer record's bytes -- no
  // out-of-bounds read (still safe), but silently wrong data read as this
  // layer's name or tagged blocks, and a parse that appears to succeed with
  // scrambled metadata rather than refusing. Scoping `ec` to exactly
  // `extraLen` bytes makes that structurally impossible: every read below
  // is bounds-checked against `extraEnd` for free, by being bounds-checked
  // against `ec`'s own size.
  Cursor ec(c.data().subspan(extraStart, extraLen));

  // Layer mask / adjustment layer data: a self-length-prefixed block.
  // docs/psd-import-gaps.md section 1 gives the 20-byte shape byte for byte,
  // verified against a real file: 0 means no mask, 20 means the shape below,
  // and anything larger means a second "real" mask (vector-mask-derived
  // render) follows, which this module refuses BY NAME rather than guess at
  // -- none of the three sample files this module was checked against ever
  // exercises it (all ten of their masks are exactly size 20).
  //
  // Scoped to a SUB-cursor over exactly `maskLen` bytes, the same discipline
  // `ec` itself is scoped to `extraLen` bytes for: a malformed `maskLen`
  // that undershoots must not let the 20-byte read below wander into the
  // blending-ranges field that follows it and misparse that as mask data.
  uint32_t maskLen = 0;
  if (!ec.u32(maskLen)) {
    error = "layer record: truncated layer mask data length at byte " +
            std::to_string(extraStart + ec.pos());
    return false;
  }
  std::span<const uint8_t> maskSpan;
  if (!ec.bytes(maskLen, maskSpan)) {
    error = "layer record: layer mask data length " + std::to_string(maskLen) +
            " runs past the extra-data field at byte " + std::to_string(extraStart + ec.pos());
    return false;
  }
  if (maskLen == 20) {
    Cursor mc(maskSpan);
    int32_t mTop = 0, mLeft = 0, mBottom = 0, mRight = 0;
    uint8_t mDefault = 0, mFlags = 0;
    if (!(mc.i32(mTop) && mc.i32(mLeft) && mc.i32(mBottom) && mc.i32(mRight) &&
          mc.u8(mDefault) && mc.u8(mFlags) && mc.skip(2))) {
      error = "layer record: internal: 20-byte mask record did not fit its own sub-cursor";
      return false;
    }
    // A well-formed mask rectangle never inverts either -- the identical
    // discipline the layer's own rectangle is held to above, and for the
    // same reason: a clamped rectangle would decode SOME mask samples for a
    // rect whose own bytes never described a valid one.
    if (mBottom < mTop || mRight < mLeft) {
      error = "layer record: inverted mask rectangle (top=" + std::to_string(mTop) +
              " left=" + std::to_string(mLeft) + " bottom=" + std::to_string(mBottom) +
              " right=" + std::to_string(mRight) + ") at byte " + std::to_string(extraStart);
      return false;
    }
    layer.hasMask = true;
    layer.maskTop = mTop;
    layer.maskLeft = mLeft;
    layer.maskBottom = mBottom;
    layer.maskRight = mRight;
    layer.maskDefaultColor = mDefault;
    layer.maskFlags = mFlags;
  } else if (maskLen != 0) {
    error = "layer record: layer mask data of size " + std::to_string(maskLen) +
            " at byte " + std::to_string(extraStart) +
            " is not the plain 20-byte shape this module reads -- a size other than 0 or 20 "
            "means a second 'real' (vector-mask-derived) mask follows, which is refused by "
            "name rather than guessed at (no sample file this module was checked against ever "
            "carries one).";
    return false;
  }
  // maskLen == 0: `layer.hasMask` stays false, its default -- no mask block
  // at all, the common case.

  // Layer blending ranges: same shape, same treatment.
  uint32_t blendRangesLen = 0;
  if (!ec.u32(blendRangesLen) || !ec.skip(blendRangesLen)) {
    error = "layer record: truncated layer blending ranges at byte " +
            std::to_string(extraStart + ec.pos());
    return false;
  }

  // Layer name: Pascal string (1-byte length, then that many bytes),
  // padded so the WHOLE field (length byte included) is a multiple of 4.
  uint8_t nameLen = 0;
  if (!ec.u8(nameLen)) {
    error = "layer record: truncated layer name at byte " + std::to_string(extraStart + ec.pos());
    return false;
  }
  std::span<const uint8_t> nameBytes;
  if (!ec.bytes(nameLen, nameBytes)) {
    error = "layer record: layer name runs past the extra-data field at byte " +
            std::to_string(extraStart + ec.pos());
    return false;
  }
  layer.name.assign(reinterpret_cast<const char*>(nameBytes.data()), nameBytes.size());
  const size_t consumedSoFar = 1 + static_cast<size_t>(nameLen);
  const size_t padded = (consumedSoFar + 3) & ~static_cast<size_t>(3);
  if (!ec.skip(padded - consumedSoFar)) {
    error = "layer record: layer name padding runs past the extra-data field at byte " +
            std::to_string(extraStart + ec.pos());
    return false;
  }

  // Additional Layer Information: a sequence of `8BIM`/`8B64` + 4-char key +
  // 4-byte length + data, filling the rest of this layer's extra-data
  // field. Walked for exactly one key, `luni` (the Unicode layer name,
  // preferred over the Pascal one per io/PsdImport.hpp) -- every other key
  // (`lrFX`, `lsct`, adjustment-layer parameters, ...) is skipped by its own
  // declared length without being interpreted, which is what lets this
  // module stay correct in the face of Additional Layer Information kinds
  // it has never heard of: it only ever needs to know how many bytes to
  // step over, never what is in them.
  while (ec.remaining() >= 12) {
    const size_t blockStart = extraStart + ec.pos();
    std::array<char, 4> blockSig{};
    std::array<char, 4> key{};
    uint32_t blockLen = 0;
    if (!(ec.fourcc(blockSig) && ec.fourcc(key) && ec.u32(blockLen))) {
      error = "layer record: truncated additional layer info block at byte " +
              std::to_string(blockStart);
      return false;
    }
    if (!fourccEquals(blockSig, "8BIM") && !fourccEquals(blockSig, "8B64")) {
      error = "layer record: additional layer info block has signature '" +
              fourccToString(blockSig) + "', not '8BIM'/'8B64', at byte " +
              std::to_string(blockStart);
      return false;
    }
    std::span<const uint8_t> blockData;
    if (!ec.bytes(blockLen, blockData)) {
      error = "layer record: additional layer info block '" + fourccToString(key) +
              "' length " + std::to_string(blockLen) + " runs past the extra-data field (at byte " +
              std::to_string(blockStart) + ")";
      return false;
    }
    if (fourccEquals(key, "luni")) {
      // A UnicodeString: 4-byte code-unit count, then that many UTF-16BE
      // code units -- io/PsdImport.hpp's own header on why this is decoded
      // directly rather than routed through io/Descriptor's Action
      // Descriptor reader: `luni` is not an Action Descriptor value at all,
      // it is the format's own plain UnicodeString primitive, so the
      // Descriptor module's framing (a Key, an osType, ...) does not apply
      // here.
      Cursor nc(blockData);
      uint32_t units = 0;
      if (nc.u32(units)) {
        // `units` is a uint32_t, so `units * 2` is at most ~8.6 billion --
        // well inside size_t's 64-bit range on every platform this project
        // targets (Darwin/macOS only; nothing elsewhere in this tree guards
        // a 32-bit size_t either). `nc.bytes()` bounds-checks the result
        // against `blockData` regardless, so an oversized `units` from a
        // hostile file is still refused, just by that check rather than by
        // an overflow guard on the multiplication.
        std::span<const uint8_t> codeUnitBytes;
        if (nc.bytes(static_cast<size_t>(units) * 2, codeUnitBytes)) {
          std::u16string utf16;
          utf16.reserve(units);
          for (uint32_t i = 0; i < units; ++i) {
            const uint16_t unit = static_cast<uint16_t>((codeUnitBytes[i * 2] << 8) |
                                                         codeUnitBytes[i * 2 + 1]);
            utf16.push_back(static_cast<char16_t>(unit));
          }
          // Photoshop writes a trailing NUL inside `luni`'s own counted
          // length (observed convention, not stated in the published spec
          // table -- io/PsdImport.hpp's own header names this class of gap).
          // Stripped here so a re-opened name does not grow a literal NUL
          // glyph on every round trip through an application that does not
          // also strip it.
          while (!utf16.empty() && utf16.back() == u'\0') utf16.pop_back();
          // UTF-16 -> UTF-8, by hand rather than a locale-dependent
          // conversion facet: only the BMP is needed for this landing (a
          // lone surrogate is replaced with U+FFFD, matching
          // io/Descriptor.cpp's own repair policy for the identical
          // failure mode in a different Adobe string encoding), and every
          // codepoint here is either ASCII, in the 2-byte UTF-8 range, or a
          // full BMP character in the 3-byte range -- astral characters
          // would need surrogate-pair reassembly this loop does not
          // attempt, which is a stated, narrow gap rather than a silent
          // truncation: an unpaired surrogate becomes U+FFFD rather than
          // being emitted as invalid UTF-8.
          std::string utf8;
          for (size_t i = 0; i < utf16.size(); ++i) {
            uint32_t cp = static_cast<uint16_t>(utf16[i]);
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < utf16.size()) {
              const uint32_t lo = static_cast<uint16_t>(utf16[i + 1]);
              if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
              } else {
                cp = 0xFFFD;
              }
            } else if (cp >= 0xD800 && cp <= 0xDFFF) {
              cp = 0xFFFD;
            }
            if (cp < 0x80) {
              utf8.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
              utf8.push_back(static_cast<char>(0xC0 | (cp >> 6)));
              utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
              utf8.push_back(static_cast<char>(0xE0 | (cp >> 12)));
              utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
              utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
              utf8.push_back(static_cast<char>(0xF0 | (cp >> 18)));
              utf8.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
              utf8.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
              utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
          }
          layer.name = std::move(utf8);
        }
      }
      // A `luni` block this module could not even read as a UnicodeString
      // (truncated inside its own bytes) is not a whole-file refusal --
      // the Pascal name already read above is a perfectly good fallback,
      // and one damaged optional block should not cost the rest of a
      // document that otherwise parses cleanly.
    } else if (fourccEquals(key, "lspf")) {
      // "Protected Setting" -- a plain u32 bitfield, no Descriptor framing
      // (same reasoning as `luni` above: this is one of PSD's own small
      // fixed-shape primitives, not an Action Descriptor value). Bit 0 =
      // protect transparency ("Lock transparent pixels"); bit 1 = protect
      // composite ("Lock image pixels", i.e. no painting); bit 2 = protect
      // position ("Lock position", i.e. no move).
      //
      // Bit 0 is wired: it is exactly the promise `Layer::alphaLocked`
      // already makes (Layer.hpp's own comment on the field, landed
      // 4931d6d) -- "freezes the layer's alpha while still letting colour
      // change underneath it" is a direct restatement of Photoshop's own
      // "Lock transparent pixels".
      //
      // Bits 1 and 2 are read but DELIBERATELY left unmapped to
      // `Layer::locked`, the "nearest thing" this codebase has. They are
      // not a match: `Layer::locked` is enforced by core/LayerOps.hpp and
      // app/StrokeSession.cpp's `strokeRouteFor()` against remove, move,
      // rename, re-opacity AND painting all at once ("locked before kind,
      // so a locked layer refuses for being locked whatever it is made
      // of" -- Layer.hpp's own comment), which is strictly broader than
      // either PSD flag alone claims. Setting `locked` from bit 2
      // (position-only) would also freeze painting, renaming and opacity
      // the source file never asked to freeze; setting it from bit 1
      // (composite-only) would also freeze move, rename and opacity.
      // Either mapping would report a protection state stronger than the
      // one actually recorded in the PSD -- which is worse than reporting
      // none, so both bits are read here (for a future, better-fitted
      // field) and go nowhere, rather than being forced onto a field whose
      // promise they do not keep.
      if (blockData.size() >= 4) {
        Cursor lc(blockData);
        uint32_t flags = 0;
        if (lc.u32(flags)) {
          layer.alphaLocked = (flags & 0x1u) != 0;
        }
      }
    } else if (fourccEquals(key, "lsct")) {
      // Section divider setting -- docs/psd-import-gaps.md section 3's wire
      // layout, verified against `Testforautoflats 2.psd` and `Peter_...
      // fire.psd`: a u32 `type`, then optionally (blockData.size() >= 12) an
      // `8BIM` signature plus a 4-byte blend key, then optionally
      // (blockData.size() >= 16) a u32 sub-type this module has no use for
      // and does not read. Exactly like `luni` above, a block too short to
      // even carry its own `type` field is not a whole-file refusal -- the
      // record simply keeps `lsctRole == kNone` and imports as an ordinary
      // (very likely empty) layer, the same graceful-degradation choice
      // `luni` makes for a truncated name.
      Cursor lc(blockData);
      uint32_t lsctType = 0;
      if (lc.u32(lsctType)) {
        if (lsctType == 3) {
          layer.lsctRole = LsctRole::kDivider;
        } else if (lsctType == 1 || lsctType == 2) {
          layer.lsctRole = LsctRole::kHeader;
          std::array<char, 4> blendSig{};
          std::array<char, 4> blendKey{};
          if (lc.fourcc(blendSig) && lc.fourcc(blendKey) && fourccEquals(blendSig, "8BIM")) {
            layer.lsctBlendKey = blendKey;
            layer.lsctHasBlendKey = true;
          }
        }
        // type 0 ("other"): leave `lsctRole` at `kNone` -- an ordinary
        // layer that happens to carry this tag, per `LsctRole`'s own
        // comment. The optional sub-type field past this point (present
        // when blockData.size() >= 16) is never read, on any type.
      }
    }
  }

  // Resync to the extra-data field's own declared end rather than trusting
  // that the tagged-block walk consumed it to the byte -- the same
  // "jump to the outer length's own end" discipline io/AbrBrushes.cpp's
  // 8BIM section walk uses, and for the identical reason: a field this
  // module does not fully interpret (an Additional Layer Information kind
  // with no case above) must not be able to desynchronise everything after
  // it.
  if (!c.seek(extraEnd)) {
    error = "layer record: internal: extra-data end is not reachable";
    return false;
  }
  return true;
}

// Decodes one channel's own byte span (already sliced to exactly its
// declared length by the caller) into `expected` raw samples (1 or 2 bytes
// each, per `bytesPerSample`), honouring only the two compressions this
// module reads.
bool decodeChannelData(std::span<const uint8_t> channelSpan, uint32_t width, uint32_t height,
                       int bytesPerSample, std::vector<uint8_t>& rawSamples,
                       std::string& error) {
  Cursor c(channelSpan);
  uint16_t compression = 0;
  if (!c.u16(compression)) {
    error = "channel data: truncated compression field";
    return false;
  }
  const size_t expected = static_cast<size_t>(width) * height * static_cast<size_t>(bytesPerSample);
  std::span<const uint8_t> body = channelSpan.subspan(c.pos());

  if (compression == 0) {  // Raw
    if (body.size() != expected) {
      error = "channel data: raw data is " + std::to_string(body.size()) + " bytes, expected " +
              std::to_string(expected);
      return false;
    }
    rawSamples.assign(body.begin(), body.end());
    return true;
  }
  if (compression == 1) {  // RLE / PackBits
    if (!decodePackBits(body, height, expected, rawSamples)) {
      error = "channel data: PackBits stream did not decode to the expected " +
              std::to_string(expected) + " bytes";
      return false;
    }
    return true;
  }
  // ZIP (2) or ZIP-with-prediction (3): refused by name, for the whole
  // file -- io/PsdImport.hpp's header states why (no zlib dependency in
  // this tree) and why this is total rather than per-layer.
  error = "channel data: compression mode " + std::to_string(compression) +
          " (ZIP" + std::string(compression == 3 ? " with prediction" : "") +
          ") is not supported -- this build has no zlib dependency, and a ZIP-compressed "
          "PSD layer is refused rather than decoded into garbage or silently dropped";
  return false;
}

// Packs one layer's decoded straight-alpha, linear-light RGBA samples into
// `layer`'s tile storage at document coordinates offset by (`left`, `top`).
//
// This is io/ImageIO.cpp's `writeDecodedImageIntoLayer()` with one added
// offset, deliberately kept as its own function rather than a change to
// that one: `writeDecodedImageIntoLayer()`'s own contract (io/ImageIO.hpp)
// is fixed at document coordinates [0, width) x [0, height) -- every other
// caller relies on that -- and a PSD layer's own rectangle can sit anywhere
// on the canvas, including partly or wholly off it (negative `left`/`top`,
// or a `right`/`bottom` past the canvas edge, both legal in the format and
// both exercised by --selftest's "offset layer bounds" fixture).
// core/Tile.hpp's `floorDiv`/`floorMod` already handle negative document
// coordinates correctly (see that header's own comment), so no clamping is
// needed here at all -- a tile is simply allocated wherever the layer's
// pixels land, on or off the nominal canvas.
//
// **Fully transparent samples are skipped rather than written**, and that is
// a memory decision, not a shortcut. Premultiplying a sample whose alpha is
// zero gives {0, 0, 0, 0} whatever its straight RGB was -- which is exactly
// what a tile that does not exist already reads as, since `TileStoreOf`'s
// slots are value-initialised and core/Composite treats an absent tile as
// transparent. So the two are indistinguishable downstream, and writing the
// zero costs a whole 128 KB tile for nothing.
//
// It costs a great deal for nothing on real files. A Photoshop document
// whose layers were each pasted at full canvas size -- which is what a
// composited illustration tends to look like, every layer's record rect the
// whole canvas even where its content is a hand-sized patch -- allocated the
// entire tile grid for every one of them: a measured 5000x2559 document with
// 53 layers took **6.2 GB** resident, 800 tiles per layer regardless of
// content, against this project's stated 512 MB budget (app/Memory). The
// same file after this skip holds only tiles that contain something.
//
// This is not a heuristic and there is no threshold: a tile is skipped
// exactly when every sample that would land in it has alpha == 0, so no
// visible pixel can be dropped by it. `--selftest`'s "a fully transparent
// layer allocates no tiles" assertion is the guard.
void writeLayerPixelsAt(std::span<const float> straightRgba, uint32_t width, uint32_t height,
                        int32_t left, int32_t top, Layer& layer) {
  if (width == 0 || height == 0 || !layer.rgbTiles.has_value()) return;
  TileStore& tiles = *layer.rgbTiles;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const float* src = &straightRgba[(static_cast<size_t>(y) * width + x) * 4];
      const float a = src[3];
      if (a <= 0.0f) continue;
      const std::array<float, 4> premultiplied{src[0] * a, src[1] * a, src[2] * a, a};
      const PixelCoord doc{left + static_cast<int32_t>(x), top + static_cast<int32_t>(y)};
      tiles.getOrCreate(tileCoordAt(doc)).writePixel(tileLocalOffset(doc), premultiplied);
    }
  }
}

// Writes one mask channel's decoded coverage samples into `maskTiles` at
// document coordinates offset by (`left`, `top`) -- the RESOLVED mask rect's
// origin (already adjusted for the "position relative to layer" flag by the
// caller, so this function itself never has to know about it).
//
// **Skips writing a texel whose coverage is exactly 1.0 when `skipReveal` is
// true**, the mask analogue of `writeLayerPixelsAt`'s alpha==0 skip, and
// deliberately the INVERSE of it: core/Mask.hpp is explicit that an
// unallocated `MaskTile` already means 1.0 (reveal), the identity of the
// multiply a mask feeds, so writing 1.0 explicitly would only spend a whole
// 32 KiB tile to store a value an absent tile already gives for free. Get
// the direction backwards -- skip on 0.0 instead -- and a mask that is
// mostly "reveal everything" with one small painted region allocates a tile
// for every 1.0 texel and none for the one that matters, which is precisely
// the "discovered by the user as a black layer" failure Mask.hpp's own
// header warns about, arriving from the opposite direction.
//
// `skipReveal` is false only for the "default colour 0" case
// (`fillMaskHiddenAt` below): there, an absent tile no longer means this
// layer's own default, so a 1.0 sample inside the mask rect must be written
// explicitly to overwrite the 0.0 the whole-layer fill already put there.
void writeMaskPixelsAt(std::span<const float> coverage, uint32_t width, uint32_t height,
                       int32_t left, int32_t top, bool skipReveal, MaskTileStore& maskTiles) {
  if (width == 0 || height == 0) return;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const float v = coverage[static_cast<size_t>(y) * width + x];
      if (skipReveal && v >= 1.0f) continue;
      const PixelCoord doc{left + static_cast<int32_t>(x), top + static_cast<int32_t>(y)};
      maskTiles.getOrCreate(tileCoordAt(doc)).writeCoverage(tileLocalOffset(doc), v);
    }
  }
}

// The "default colour 0" half of docs/psd-import-gaps.md section 1: fills
// `maskTiles` with explicit 0.0 (hidden) coverage across THIS LAYER's own
// [left, left+width) x [top, top+height) extent -- not the whole canvas,
// because a mask only ever multiplies THIS layer's own coverage, and this
// layer holds no pixels anywhere outside its own rectangle for a mask value
// there to affect.
//
// Unlike `writeMaskPixelsAt` above, every texel here is written, none
// skipped: 0.0 is the mask's real content (core/Mask.hpp: "all 0.0 ... is
// not free: it is real content, 32 KiB per tile, exactly as a painted
// stroke is"), the opposite of the 1.0 an absent tile already means, so
// there is no free case to skip. Called BEFORE the mask channel's own
// samples are written (`writeMaskPixelsAt` above, with `skipReveal = false`
// in this case), so the real samples inside the mask rect overwrite this
// fill rather than the other way around.
void fillMaskHiddenAt(uint32_t width, uint32_t height, int32_t left, int32_t top,
                      MaskTileStore& maskTiles) {
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const PixelCoord doc{left + static_cast<int32_t>(x), top + static_cast<int32_t>(y)};
      maskTiles.getOrCreate(tileCoordAt(doc)).writeCoverage(tileLocalOffset(doc), 0.0f);
    }
  }
}

// A profiled import of a real 50-layer, 5000x2559 PSD found `srgbDecode()`
// (via `std::pow`) dominating a meaningful share of import time, called once
// per RGB byte of every layer. An 8-bit channel sample only ever takes one
// of 256 distinct values, so this table precomputes `srgbDecode(i/255.0f)`
// for every `i` in [0,255] and is used ONLY at the 8-bit PSD-channel-decode
// call site below -- `srgbDecode()` itself is untouched and still called
// directly for the 16-bit path (65536 distinct values -- a table isn't a
// win there) and by every other caller in this codebase that may pass a
// float that isn't exactly `byte/255.0f`. Do not repurpose this table for
// those other callers; see io/PsdImport.hpp's "Colour space" section.
const std::array<float, 256>& srgb8DecodeTable() {
  static const std::array<float, 256> table = [] {
    std::array<float, 256> t{};
    for (int i = 0; i < 256; ++i)
      t[static_cast<size_t>(i)] = srgbDecode(static_cast<float>(i) / 255.0f);
    return t;
  }();
  return table;
}

}  // namespace

// Declared in PsdImport.hpp for app/selftest/PsdImport.cpp only -- forwards
// to the same table `importPsd()` below actually indexes, so the selftest
// checks the real thing rather than a hand-duplicated copy of it.
const std::array<float, 256>& srgb8DecodeTableForSelftest() { return srgb8DecodeTable(); }

PsdImportResult importPsd(std::span<const uint8_t> bytes) {
  PsdImportResult result;
  Cursor c(bytes);

  auto fail = [&](std::string message) {
    result.ok = false;
    result.error = std::move(message);
    result.document = Document{};
    return result;
  };

  // --- File Header Section ---------------------------------------------
  std::array<char, 4> signature{};
  if (!c.fourcc(signature) || !fourccEquals(signature, "8BPS"))
    return fail("not a PSD file: missing '8BPS' signature.");

  uint16_t version = 0;
  if (!c.u16(version)) return fail("PSD file header is truncated (no version word).");
  if (version == 2) {
    // PSB, the Large Document Format: refused by name -- io/PsdImport.hpp's
    // header states the field-width research this decision rests on and
    // why it is refused rather than attempted.
    return fail(
        "this is a PSB (Large Document Format, version 2) file. This build's PSD reader "
        "handles PSD (version 1) only -- PSB widens several length fields from 4 to 8 "
        "bytes and is refused by name rather than misparsed as a PSD.");
  }
  if (version != 1) return fail("unrecognised PSD version " + std::to_string(version) + ".");

  if (!c.skip(6)) return fail("PSD file header is truncated (reserved bytes).");

  uint16_t channelCount = 0;
  uint32_t height = 0, width = 0;
  uint16_t depth = 0, colorMode = 0;
  if (!(c.u16(channelCount) && c.u32(height) && c.u32(width) && c.u16(depth) &&
        c.u16(colorMode)))
    return fail("PSD file header is truncated.");

  if (channelCount < 1 || channelCount > 56)
    return fail("PSD file header declares " + std::to_string(channelCount) +
               " channels, outside the format's own 1-56 range.");
  if (width == 0 || height == 0)
    return fail("PSD file header declares a " + std::to_string(width) + "x" +
               std::to_string(height) + " canvas.");
  if (width > 30000 || height > 30000)
    return fail("PSD file header declares a " + std::to_string(width) + "x" +
               std::to_string(height) +
               " canvas, past PSD's own 30,000px limit (that limit is what PSB, which "
               "this build does not read, exists to raise).");
  if (depth != 8 && depth != 16)
    return fail("PSD file header declares " + std::to_string(depth) +
               "-bit channels; this build reads 8- and 16-bit only.");
  if (colorMode != 3) {
    const char* modeName = "unknown";
    switch (colorMode) {
      case 0: modeName = "Bitmap"; break;
      case 1: modeName = "Grayscale"; break;
      case 2: modeName = "Indexed"; break;
      case 4: modeName = "CMYK"; break;
      case 7: modeName = "Multichannel"; break;
      case 8: modeName = "Duotone"; break;
      case 9: modeName = "Lab"; break;
    }
    return fail(std::string("PSD file header declares ") + modeName + " colour mode (" +
               std::to_string(colorMode) +
               "); this build reads RGB (mode 3) only, and does not guess a conversion for "
               "the others.");
  }
  const int bytesPerSample = depth == 16 ? 2 : 1;

  // --- Color Mode Data Section (skipped: only indexed/duotone carry any) -
  uint32_t colorDataLen = 0;
  if (!c.u32(colorDataLen) || !c.skip(colorDataLen))
    return fail("PSD Color Mode Data section is truncated.");

  // --- Image Resources Section (skipped) ---------------------------------
  uint32_t imageResLen = 0;
  if (!c.u32(imageResLen) || !c.skip(imageResLen))
    return fail("PSD Image Resources section is truncated.");

  // --- Layer and Mask Information Section --------------------------------
  uint32_t layerMaskInfoLen = 0;
  if (!c.u32(layerMaskInfoLen))
    return fail("PSD Layer and Mask Information section length is truncated.");
  const size_t layerMaskInfoStart = c.pos();
  size_t layerMaskInfoEnd = 0;
  if (!checkedAdd(layerMaskInfoStart, layerMaskInfoLen, layerMaskInfoEnd) ||
      layerMaskInfoEnd > c.size())
    return fail("PSD Layer and Mask Information section length (" +
               std::to_string(layerMaskInfoLen) + ") runs past the end of the file.");

  if (layerMaskInfoLen == 0) {
    // A flat PSD (or one saved with Maximize Compatibility off): no layer
    // section at all. Not this module's file to open -- app/OpenAnyFile.cpp
    // falls back to the existing flattened decode path for exactly this
    // case. See `PsdImportResult::noLayerData`.
    result.ok = false;
    result.noLayerData = true;
    result.error = "this PSD carries no layer information (a flat composite only).";
    return result;
  }

  uint32_t layerInfoLen = 0;
  if (!c.u32(layerInfoLen)) return fail("PSD Layer info section length is truncated.");
  const size_t layerInfoStart = c.pos();
  size_t layerInfoEnd = 0;
  if (!checkedAdd(layerInfoStart, layerInfoLen, layerInfoEnd) || layerInfoEnd > layerMaskInfoEnd)
    return fail("PSD Layer info section length (" + std::to_string(layerInfoLen) +
               ") runs past its own Layer and Mask Information section.");

  if (layerInfoLen < 2) {
    result.ok = false;
    result.noLayerData = true;
    result.error = "this PSD's layer info section is empty.";
    return result;
  }

  int16_t layerCountRaw = 0;
  if (!c.i16(layerCountRaw)) return fail("PSD layer count is truncated.");
  // A negative count means "the first alpha channel of the composite holds
  // the merged result's own transparency" -- a fact about the composite
  // this module never reads, so only the absolute value (the actual number
  // of layer records that follow) matters here.
  const int32_t layerCount =
      layerCountRaw < 0 ? -static_cast<int32_t>(layerCountRaw) : layerCountRaw;

  if (layerCount == 0) {
    result.ok = false;
    result.noLayerData = true;
    result.error = "this PSD declares zero layers.";
    return result;
  }

  std::vector<ParsedLayer> layers;
  layers.reserve(static_cast<size_t>(layerCount));
  for (int32_t i = 0; i < layerCount; ++i) {
    ParsedLayer layer;
    std::string layerError;
    if (!readLayerRecord(c, layer, layerError))
      return fail("layer " + std::to_string(i) + ": " + layerError);
    layers.push_back(std::move(layer));
  }
  // Each layer record's own `extraLen` is only checked against the WHOLE
  // FILE by `readLayerRecord()` (see that function's own comment on why a
  // tighter per-field bound isn't practical field by field). This is the
  // aggregate check that catches what those individual checks cannot: a
  // record whose declared extra-data length is large enough to run past
  // the Layer info section's own declared end while still fitting inside
  // the file as a whole -- a real corruption this module would otherwise
  // read straight through, silently, rather than refuse.
  if (c.pos() > layerInfoEnd)
    return fail("layer records overran the declared Layer info section length (reached byte " +
               std::to_string(c.pos()) + ", section ends at " + std::to_string(layerInfoEnd) +
               ").");

  // --- Channel image data: one block per channel, per layer, same order -
  //
  // Decoded straight into a Document built fresh here (not
  // Document::createBlank(), which hands out exactly one layer -- this
  // module needs N, and building the vector directly is Document::
  // createBlank()'s own shape done N times rather than a second, competing
  // way to construct a Document).
  Document doc;
  doc.width = static_cast<int32_t>(width);
  doc.height = static_cast<int32_t>(height);
  doc.workingSpace = WorkingSpace{};
  doc.layers.reserve(layers.size());

  // --- Layer groups (`lsct`) -----------------------------------------------
  //
  // docs/psd-import-gaps.md section 3. The flat record list runs
  // bottom-first, and a group's members sit BETWEEN its divider (type 3,
  // read FIRST) and its header (type 1/2, read LAST): the divider opens the
  // group as this loop reads forward, the header closes and names it. A
  // stack of pending frames gives nesting for free -- each element is the
  // `doc.layers.size()` at the moment its divider was seen, i.e. "how many
  // output layers existed when this group started", so `[frameStart,
  // doc.layers.size())` at the matching header is exactly this frame's
  // direct children, whether ordinary layers or (for a nested group) that
  // inner group's own single entry -- **never** its inner group's members,
  // which were already stamped with the INNER group's tag when the inner
  // frame closed. That is what "only stamp `parent` where it is still
  // empty" below buys: a doubly-nested member is never overwritten by an
  // outer frame that closes later.
  //
  // Getting this backward -- treating the header as the opener and the
  // divider as the closer -- produces a document that still has the right
  // NUMBER of groups (every header still closes exactly one frame) but
  // silently inverts or empties every group's membership, with no crash and
  // no refusal anywhere to catch it. `app/selftest/PsdImport.cpp`'s section F
  // sabotage exercises exactly this and confirms the group-count assertion
  // stays green while the membership assertion reddens.
  std::vector<size_t> groupStack;

  for (size_t li = 0; li < layers.size(); ++li) {
    ParsedLayer& pl = layers[li];
    const uint32_t layerWidth = static_cast<uint32_t>(pl.right - pl.left);
    const uint32_t layerHeight = static_cast<uint32_t>(pl.bottom - pl.top);
    const size_t pixelCount = static_cast<size_t>(layerWidth) * layerHeight;

    // Straight (non-premultiplied), linear-light RGBA -- default fully
    // transparent black, so a channel this layer's own table does not list
    // (no R, no G, no B, or no alpha) reads as its correct implicit value:
    // 0 for a missing colour channel, and 1.0 for a missing alpha is set
    // explicitly below, matching io/ImageDecode.cpp's own "sources without
    // an alpha channel decode as fully opaque" convention.
    std::vector<float> pixels(pixelCount * 4, 0.0f);
    bool hasAlphaChannel = false;

    // --- Resolve this layer's mask, if it has one, before the channel walk
    // reaches its -2 channel -- docs/psd-import-gaps.md section 1.
    //
    // `top`/`left`/`bottom`/`right` below are ABSOLUTE document coordinates,
    // resolved from `pl.mask*`'s wire values according to `maskFlags` bit 0
    // ("position relative to layer"): when set, the wire values are an
    // offset from this layer's own (top, left) rather than already-absolute
    // coordinates -- widened to int64_t for the add so two independently
    // untrusted int32_t values (the layer's own origin, and the mask's) can
    // never signed-overflow, the same `layerWidth64`-style widening
    // `readLayerRecord()` already uses for this layer's own rectangle.
    bool maskPresent = false;
    bool maskDisabled = false;
    int32_t maskAbsTop = 0, maskAbsLeft = 0, maskAbsBottom = 0, maskAbsRight = 0;
    if (pl.hasMask) {
      constexpr uint8_t kMaskFlagRelative = 0x01;
      constexpr uint8_t kMaskFlagDisabled = 0x02;
      maskPresent = true;
      maskDisabled = (pl.maskFlags & kMaskFlagDisabled) != 0;
      if (pl.maskFlags & kMaskFlagRelative) {
        maskAbsTop = static_cast<int32_t>(static_cast<int64_t>(pl.top) + pl.maskTop);
        maskAbsLeft = static_cast<int32_t>(static_cast<int64_t>(pl.left) + pl.maskLeft);
        maskAbsBottom = static_cast<int32_t>(static_cast<int64_t>(pl.top) + pl.maskBottom);
        maskAbsRight = static_cast<int32_t>(static_cast<int64_t>(pl.left) + pl.maskRight);
      } else {
        maskAbsTop = pl.maskTop;
        maskAbsLeft = pl.maskLeft;
        maskAbsBottom = pl.maskBottom;
        maskAbsRight = pl.maskRight;
      }
      if (pl.maskDefaultColor != 0 && pl.maskDefaultColor != 255) {
        return fail("layer " + std::to_string(li) + " (" + pl.name +
                   "): mask default colour " + std::to_string(pl.maskDefaultColor) +
                   " is neither 0 (hide outside the mask rect) nor 255 (reveal outside it) -- "
                   "this module does not guess at an intermediate default and refuses rather "
                   "than silently treating it as one or the other.");
      }
    }
    // `maskTiles` is populated here, in document-coordinate space, and moved
    // onto the Layer once it exists below -- the `Layer` object itself isn't
    // constructed until after the channel walk (its RGBA `pixels` still
    // needs to be gathered first), so there is nowhere on it yet to write a
    // mask sample as the channel loop reads one.
    std::optional<MaskTileStore> maskTiles;
    if (maskPresent && !maskDisabled) {
      maskTiles.emplace();
      // "default colour 0" (docs/psd-import-gaps.md): everything outside the
      // mask rect, across this layer's own extent, is HIDDEN -- and unlike
      // the 255 case, that is not what an absent tile already means, so it
      // must be written explicitly, before the mask channel's own samples
      // (below) overwrite the rect itself with real content.
      if (pl.maskDefaultColor == 0) {
        fillMaskHiddenAt(layerWidth, layerHeight, pl.left, pl.top, *maskTiles);
      }
    }

    for (const ParsedChannel& ch : pl.channels) {
      std::span<const uint8_t> channelSpan;
      if (!c.bytes(ch.length, channelSpan))
        return fail("layer " + std::to_string(li) + ": channel data length " +
                   std::to_string(ch.length) + " runs past the end of the file.");

      // The "real" layer mask channel -- docs/psd-import-gaps.md section 1.
      // Decoded at the MASK's own rectangle, never the layer's: verified
      // against a real file, a masked layer's mask rectangle is almost
      // always smaller than (and offset from) its layer's rectangle, so
      // reusing `layerWidth`/`layerHeight` here would read the wrong number
      // of samples for this channel's declared length and desynchronise
      // nothing (each channel's length is self-declared) while decoding
      // garbage into the wrong-shaped store.
      if (ch.id == -2) {
        if (maskTiles.has_value()) {
          const uint32_t maskWidth = static_cast<uint32_t>(maskAbsRight - maskAbsLeft);
          const uint32_t maskHeight = static_cast<uint32_t>(maskAbsBottom - maskAbsTop);
          const size_t maskPixelCount = static_cast<size_t>(maskWidth) * maskHeight;
          if (maskPixelCount > 0) {
            std::vector<uint8_t> raw;
            std::string channelError;
            if (!decodeChannelData(channelSpan, maskWidth, maskHeight, bytesPerSample, raw,
                                   channelError))
              return fail("layer " + std::to_string(li) + " (" + pl.name + "): mask " +
                         channelError);
            const float maxValue = bytesPerSample == 2 ? 65535.0f : 255.0f;
            std::vector<float> coverage(maskPixelCount);
            for (size_t px = 0; px < maskPixelCount; ++px) {
              uint32_t sample;
              if (bytesPerSample == 2)
                sample = (static_cast<uint32_t>(raw[px * 2]) << 8) | raw[px * 2 + 1];
              else
                sample = raw[px];
              // A mask sample is a coverage, not a colour -- linear already,
              // matching alpha's own treatment just below: no `srgbDecode()`.
              coverage[px] = static_cast<float>(sample) / maxValue;
            }
            // `skipReveal` follows the default colour: with a 255 default,
            // an unwritten (1.0) texel already reads correctly as absent, so
            // it is skipped (the "inverted empty-tile rule" core/Mask.hpp
            // states); with a 0 default, `fillMaskHiddenAt()` above already
            // put an explicit 0.0 everywhere in this rect, so every sample
            // -- 1.0 included -- must be written to overwrite it.
            writeMaskPixelsAt(coverage, maskWidth, maskHeight, maskAbsLeft, maskAbsTop,
                              /*skipReveal=*/pl.maskDefaultColor != 0, *maskTiles);
          }
        }
        continue;
      }
      // The secondary "user-supplied" mask channel, present only alongside a
      // vector mask when both a vector-derived and a user-painted mask
      // exist on the same layer -- out of scope, the same stated limit the
      // 20-vs-more-than-20 mask-record refusal above already draws, and
      // unexercised by any of the three sample files (all ten of their
      // masks are plain -2 channels). Its bytes are already consumed by
      // `c.bytes()` above, so skipping it here costs this layer nothing but
      // the (unimplemented) second mask.
      if (ch.id == -3) continue;

      if (ch.id != 0 && ch.id != 1 && ch.id != 2 && ch.id != -1) continue;
      if (pixelCount == 0) continue;  // an empty layer: nothing to decode

      std::vector<uint8_t> raw;
      std::string channelError;
      if (!decodeChannelData(channelSpan, layerWidth, layerHeight, bytesPerSample, raw,
                             channelError))
        return fail("layer " + std::to_string(li) + " (" + pl.name + "): " + channelError);

      const float maxValue = bytesPerSample == 2 ? 65535.0f : 255.0f;
      const int channelIndex = ch.id < 0 ? 3 : static_cast<int>(ch.id);  // R=0,G=1,B=2,A=3
      if (ch.id == -1) hasAlphaChannel = true;

      // RGB is sRGB-encoded and is linearised; alpha is linear opacity and
      // passes through unchanged -- io/PsdImport.hpp's "Colour space"
      // section, the identical rule io/ImageDecode.cpp already applies to
      // every other 8-/16-bit format this codebase reads. The 8-bit branch
      // below is byte-value-identical to the general `srgbDecode(encoded01)`
      // call in the 16-bit branch -- see `srgb8DecodeTable()` above -- just
      // computed once per distinct byte value instead of once per pixel.
      if (bytesPerSample == 1) {
        const std::array<float, 256>& table = srgb8DecodeTable();
        for (size_t px = 0; px < pixelCount; ++px) {
          const uint8_t sample = raw[px];
          pixels[px * 4 + static_cast<size_t>(channelIndex)] =
              channelIndex == 3 ? static_cast<float>(sample) / maxValue : table[sample];
        }
      } else {
        for (size_t px = 0; px < pixelCount; ++px) {
          const uint32_t sample =
              (static_cast<uint32_t>(raw[px * 2]) << 8) | raw[px * 2 + 1];
          const float encoded01 = static_cast<float>(sample) / maxValue;
          pixels[px * 4 + static_cast<size_t>(channelIndex)] =
              channelIndex == 3 ? encoded01 : srgbDecode(encoded01);
        }
      }
    }
    if (!hasAlphaChannel) {
      for (size_t px = 0; px < pixelCount; ++px) pixels[px * 4 + 3] = 1.0f;
    }

    // A bounding-section divider (type 3): not a layer at all -- Photoshop
    // gives it a junk Pascal name (`</Layer group>` on every sample file)
    // and it is dropped here entirely rather than imported as an empty
    // layer, which is today's defect (docs/psd-import-gaps.md section 3).
    // Its own (typically zero-length) channel data was already consumed
    // above, so the shared cursor `c` stays aligned with the next record
    // regardless. Opens a new group frame: everything pushed to `doc.layers`
    // from here until the matching header belongs to it.
    if (pl.lsctRole == LsctRole::kDivider) {
      groupStack.push_back(doc.layers.size());
      continue;
    }

    // A group header (type 1 "open folder" / 2 "closed folder"): closes and
    // names the frame its matching divider opened.
    if (pl.lsctRole == LsctRole::kHeader) {
      if (groupStack.empty()) {
        return fail("layer " + std::to_string(li) + " (" + pl.name +
                   "): a layer group header ('lsct' type 1 or 2) appeared with no matching "
                   "bounding-section divider ('lsct' type 3) open before it -- an unbalanced "
                   "group stack is refused rather than repaired, the same posture "
                   "io/Descriptor.hpp's Action Descriptor reader takes ('a refusal is total').");
      }
      const size_t frameStart = groupStack.back();
      groupStack.pop_back();

      Layer group = makeGroupLayer(doc, pl.name.empty() ? std::string("Group") : pl.name);
      // The header record carries a full layer record's own opacity/hidden
      // flags, and core/Composite.hpp's group section reads exactly these
      // two off a Group layer ("its own visible/opacity scale every member
      // uniformly") -- so, unlike a Group created fresh in-app (which always
      // starts fully visible at 100%), an imported one should carry
      // whatever Photoshop's own file says.
      group.opacity = static_cast<float>(pl.opacity) / 255.0f;
      group.visible = !pl.hidden;

      // Only every DIRECT child -- an ordinary layer, or a nested group's
      // own single entry -- gets this frame's tag; anything already
      // carrying a (necessarily more deeply nested) parent is left alone.
      // See this loop's own header comment for why that is the whole of
      // "nesting for free".
      for (size_t idx = frameStart; idx < doc.layers.size(); ++idx) {
        if (doc.layers[idx].parent.empty()) doc.layers[idx].parent = group.groupTag;
      }

      // The group's own blend key names Photoshop's Pass Through vs an
      // isolated group; core/Composite.hpp:688 argues every group this
      // build composites is pass-through (there is no isolated-group
      // accumulator here at all), so a non-`pass` key is a real, if
      // harmless-to-this-build, semantic mismatch -- imported anyway (PRD
      // I10: a foreign feature round-trips rather than vanishing) and
      // warned by name, the identical discipline `mapBlendKey()`'s own
      // caller uses just below for an ordinary layer's unmapped blend mode.
      if (pl.lsctHasBlendKey && !fourccEquals(pl.lsctBlendKey, "pass")) {
        result.warnings.push_back(
            "group '" + (pl.name.empty() ? std::string("(unnamed)") : pl.name) +
            "': PSD group blend mode '" + fourccToString(pl.lsctBlendKey) +
            "' is not Pass Through ('pass'); this build always composites a group as "
            "pass-through and does not isolate it.");
      }

      doc.layers.push_back(std::move(group));
      continue;
    }

    Layer layer;
    layer.kind = LayerKind::RGB;
    layer.rgbTiles.emplace();
    writeLayerPixelsAt(pixels, layerWidth, layerHeight, pl.left, pl.top, layer);
    // flags bit 1 (mask disabled) imports as no mask at all -- `maskTiles`
    // was never engaged for a disabled mask (guarded above), so this is
    // simply "move it across when there was one to move".
    if (maskTiles.has_value()) layer.mask = std::move(*maskTiles);

    layer.name = pl.name;
    layer.opacity = static_cast<float>(pl.opacity) / 255.0f;
    layer.visible = !pl.hidden;
    // The bottom layer can never be clipped (core/Layer.hpp's own stated
    // invariant, enforced elsewhere by core::setLayerClipped() -- this
    // module builds `doc.layers` directly rather than through that
    // function, so it owes the same invariant by hand). PSD layer 0 in file
    // order is this document's bottom layer (io/PsdImport.hpp's "Layer
    // stacking order" section) -- checked here as "is `doc.layers` still
    // empty", NOT as "is `li` still 0": a group's bounding-section divider
    // (dropped above, just before this point) can occupy record 0 without
    // contributing a layer, which would make `li == 0` true for this
    // layer's record while it is still genuinely `doc.layers`' first entry.
    layer.clipped = (pl.clipping != 0) && !doc.layers.empty();
    // `lspf` bit 0 only -- see the "lspf" case in readLayerRecord() above
    // for why bits 1/2 have nothing to land in here.
    layer.alphaLocked = pl.alphaLocked;

    bool exactBlend = false;
    const BlendMode mode = mapBlendKey(pl.blendKey, exactBlend);
    layer.blend = blendModeName(mode);
    if (!exactBlend) {
      result.warnings.push_back("layer '" + (pl.name.empty() ? std::string("(unnamed)") : pl.name) +
                                "': PSD blend mode '" + fourccToString(pl.blendKey) +
                                "' has no equivalent in this build and was imported as Normal.");
    }

    doc.layers.push_back(std::move(layer));
  }

  if (!groupStack.empty()) {
    return fail("this PSD's layer groups are unbalanced: " + std::to_string(groupStack.size()) +
               " bounding-section divider(s) ('lsct' type 3) were never closed by a matching "
               "group header ('lsct' type 1 or 2) -- an unbalanced group stack is refused "
               "rather than repaired.");
  }
  // The symmetric check to the one after the layer-records loop: the
  // channel data this module just read must not have run past the Layer
  // info section's own declared end either.
  if (c.pos() > layerInfoEnd)
    return fail("channel image data overran the declared Layer info section length (reached "
               "byte " + std::to_string(c.pos()) + ", section ends at " +
               std::to_string(layerInfoEnd) + ").");

  result.ok = true;
  result.document = std::move(doc);
  return result;
}

}  // namespace np
