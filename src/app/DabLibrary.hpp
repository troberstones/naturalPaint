#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "brush/Deposit.hpp"
#include "brush/Library.hpp"

// app/DabLibrary -- **a folder of brush tips, where dropping a file in is the
// whole import step.**
//
// ==========================================================================
// 1. Why a watched folder and not an Import button
// ==========================================================================
//
// A brush tip is a picture. Every other picture in this application arrives
// through a file dialog and becomes a document, but a tip is not a document --
// it is an ingredient, and an artist collecting fifty of them does not want to
// answer fifty dialogs. So the model here is the one Photoshop, GIMP and Krita
// all landed on independently: a directory the application reads, and putting
// a file in it is the import.
//
// **Two roots, not one, and the reason is not tidiness.**
//
//   * `dabs/` is the USER'S. The application reads it and never writes to it.
//   * `dabs-imported/` is the application's own, where extracted `.abr` tips
//     land.
//
// A single watched folder the application also wrote into could not tell "the
// user put this here" from "I put this here", which matters the moment
// anything wants to clean up after itself: deleting an orphaned extracted tip
// out of a folder the user has also been filling is how you delete someone's
// work. Two roots makes that distinction structural instead of a naming
// convention nobody can enforce.
//
// ==========================================================================
// 2. Rescanning without a file watcher
// ==========================================================================
//
// No `FSEvents`, no `inotify`, no background thread. A scan is a
// `directory_iterator` plus one `stat()` per entry, compared against the
// index; an entry whose `(size, mtime)` is unchanged **decodes nothing at
// all**, which is what makes the scan cheap enough to run on a window focus
// event. `decodeCount()` exists so `--selftest` can assert that rather than
// assume it -- a rescan that quietly re-decoded a 500-file folder would still
// pass every correctness test and would make the feature unusable.
//
// The moments a scan runs are chosen around one observation: the user was
// just in Finder. First picker open, `SDL_EVENT_WINDOW_FOCUS_GAINED`
// (debounced), an explicit button, and `--dab-scan`.
//
// **What a file watcher would buy, stated so the choice is visible:** a dab
// appearing while the application already has focus, without the user
// touching anything. That is a real difference, and it is small next to a
// platform-specific watcher thread in an application that has none.
//
// ==========================================================================
// 3. Ids, and what a rename costs
// ==========================================================================
//
//   `abr:<uuid>`         an extracted Photoshop tip. The uuid is the `samp`
//                        block's own, already the `sampledData` join key --
//                        no invention, and it survives re-importing the pack.
//   `file:<relpath>`     an image in `dabs/`.
//   `gbr:<relpath>`      a GIMP `.gbr`.
//   `gih:<relpath>#<n>`  cell `n` of a GIMP `.gih` image hose.
//
// A `file:` id names a path, so a rename breaks it, and a preset that
// referenced it would lose its tip. The index therefore stores an FNV-1a
// fingerprint of the decoded coverage, and a rescan that finds a NEW path
// whose fingerprint matches a path that just disappeared treats it as a
// rename: the entry keeps its old id and gains the new path.
//
// **That is a mitigation and not a fix**, and it is worth being plain about
// where it stops. Two identical files under different names are one
// fingerprint, so which one "wins" a repair is whichever the directory
// iteration reached first; and renaming a file AND editing it in the same
// interval between scans defeats it entirely. It is the same bargain
// app/BrushLibraryFile already strikes for a relocated `.abr`, and the
// alternative -- a content-addressed id -- would mean a preset's tip
// reference changing every time the user touched up the image.
//
// ==========================================================================
// 4. What becomes coverage
// ==========================================================================
//
// `BrushTipBitmap::alpha`, 255 = full coverage, is what everything downstream
// wants (brush/Deposit.hpp §2c). Getting there from an arbitrary picture needs
// exactly one rule, and here it is, stated once:
//
//   **If the image has a non-trivial alpha channel, that is the coverage.
//   Otherwise coverage is `1 - luminance`.**
//
// "Non-trivial" means some texel's alpha is below 1. A source with no alpha
// channel decodes as fully opaque (io/ImageDecode.hpp says so), so the two
// cases -- "no alpha channel" and "an alpha channel that is opaque
// everywhere" -- are indistinguishable in the decoded buffer AND want the
// same answer: a fully opaque RGBA image used by its alpha would be a solid
// rectangle, which is not a brush tip anyone drew on purpose.
//
// The `1 - luminance` branch is what makes a scanned black-on-white tip work,
// which is how most of them are drawn. Luminance is `ops/PointOps`'
// `computeLuma()` on the linear RGB, then one `srgbEncode()` of the
// scalar -- **the display-encoded luminance, not the linear one**, which is
// `core/SelectionRefine`'s own order and for its reason: the artist drew a
// mid-grey they saw as half-dark, and linear luminance would call that 0.22
// and make it three-quarters opaque.
//
// ==========================================================================
// 5. What is deliberately not here
// ==========================================================================
//
// **No UI, no ImGui, no GPU, no atlas.** This resolves files to coverage
// bitmaps; drawing a grid of them is `ui/DabPicker`'s job, and keeping the two
// apart is what lets `--selftest` exercise every rule above with a temporary
// directory and no window (audit F4: `--selftest` cannot reach an ImGui
// dispatch site).
//
// **No writing to `dabs/`.** §1. The application writes only to the imported
// root, and only files it named.
//
// **No `.gih` animation.** An image hose carries a placement/order/pressure
// ranking across its cells; this reads the cells as N independent tips and
// says so, because the ranking drives a stroke-time selection policy this
// build has nowhere to put yet. A user who wants cell 3 picks cell 3.
namespace np {

// Which root an entry came from -- §1's distinction, made structural.
enum class DabRoot : uint8_t {
  User,      // `dabs/`, never written by this application
  Imported,  // `dabs-imported/`, written by the `.abr` importer
};

// What the file was, which is also what the id prefix says.
enum class DabSource : uint8_t {
  Image,  // `file:`  -- anything io/ImageDecode opens
  Gimp,   // `gbr:` / `gih:`
  Abr,    // `abr:`   -- an extracted Photoshop `samp` tip
};

struct DabEntry {
  std::string id;       // §3
  std::string name;     // what the picker shows
  std::string relPath;  // relative to its root; the key a rescan stats
  DabRoot root = DabRoot::User;
  DabSource source = DabSource::Image;
  // `.gih` cell index, 0 for everything else. A hose contributes one entry
  // per cell and they share a `relPath`, which is why the index is part of
  // the id and not only of the name.
  int32_t frame = 0;

  int32_t width = 0;
  int32_t height = 0;

  // FNV-1a over the decoded coverage bytes (§3). Zero means "not yet
  // computed", which only happens for an entry read from an index written by
  // an older build.
  uint64_t fingerprint = 0;

  // The file's own default spacing, when the format carries one -- `.gbr`
  // version 2 and `.gih` do, an image does not. Never applied automatically:
  // it is the file's opinion, offered to the picker, not a setting that
  // changes a brush behind the user's back.
  bool haveSpacing = false;
  float spacingPercent = 0.0f;

  // The stat pair a rescan compares. Not the file's contents: §2.
  uint64_t sizeBytes = 0;
  int64_t mtimeNs = 0;

  // Resolved lazily by `resolve()`. Null in an entry that came from the index
  // and has not been asked for yet, which is the common case and the whole
  // point of storing width/height in the index.
  std::shared_ptr<const BrushTipBitmap> bitmap;
};

// A file that is under a root and is not a tip this build can read.
//
// **Remembered, and for the same reason the entries are.** A folder of fifty
// holiday photos next to three brushes would otherwise cost fifty decode
// attempts on every window-focus scan -- the exact cost §2 exists to avoid,
// arriving through the one path that was not indexed. Keyed by the same
// `(size, mtime)` pair, so editing the file gets it a fresh hearing and
// leaving it alone costs one `stat()`.
struct DabRefusal {
  std::string relPath;
  DabRoot root = DabRoot::User;
  uint64_t sizeBytes = 0;
  int64_t mtimeNs = 0;
  std::string note;  // replayed verbatim, so a cached refusal reads the same
};

struct DabScanResult {
  int32_t added = 0;
  int32_t removed = 0;
  int32_t unchanged = 0;
  int32_t repaired = 0;  // a rename recognised by fingerprint (§3)
  int32_t rejected = 0;
  // One line per rejected file, naming the file and why. A folder is a user
  // interface: a file that silently does not appear is indistinguishable from
  // one the application never noticed.
  std::vector<std::string> notes;
};

// Where the two roots and the index live -- beside the other preference files
// (`brush-libraries.txt`, `panel-layout.txt`), through the same resolver and
// with the same environment override, so `--selftest` never touches the real
// `~/Library/Application Support/naturalPaint`.
//
// `$NP_DAB_DIR` overrides the PARENT of both roots and the index, so one
// variable relocates the whole library rather than three that could disagree.
std::string defaultDabRootPath();
std::string dabUserRootPath();
std::string dabImportedRootPath();
std::string dabIndexPath();

class DabLibrary {
 public:
  // Both roots and the index path. Called with the defaults above by the
  // application and with a temporary directory by `--selftest`.
  void setRoots(std::string userRoot, std::string importedRoot, std::string indexPath);

  const std::string& userRoot() const noexcept { return userRoot_; }
  const std::string& importedRoot() const noexcept { return importedRoot_; }

  // Reads the index from disk if it has not been read yet. Cheap and
  // idempotent; a missing index is not an error, it is a first run.
  void loadIndex();

  // §2. Stats every file under both roots, decodes only what changed, and
  // writes the index back. **Creates neither root** -- a scan of a folder
  // that does not exist yet finds nothing, which is the correct answer and
  // avoids making directories in the user's Application Support just because
  // something opened a picker.
  DabScanResult rescan();

  const std::vector<DabEntry>& entries() const noexcept { return entries_; }
  const std::vector<DabRefusal>& refusals() const noexcept { return refusals_; }
  const DabEntry* find(const std::string& id) const noexcept;

  // The coverage bitmap for `id`, decoding the file if it is not already in
  // hand. Null if the id is unknown or the file no longer decodes.
  std::shared_ptr<const BrushTipBitmap> resolve(const std::string& id);

  // How many files this object has decoded since it was constructed.
  // **Exists for `--selftest`**, which asserts that an unchanged rescan does
  // not increase it -- §2's claim, checked rather than described.
  size_t decodeCount() const noexcept { return decodeCount_; }

  // Bumped by every `rescan()` that changed the entry list, and by every
  // `resolve()` that decoded a bitmap which was not in hand before.
  //
  // **Exists so a thumbnail atlas knows when it is stale** without comparing
  // the whole list every frame. Two different things move it, deliberately:
  // the set of dabs changing (a cell appears or goes) and a cell that was a
  // placeholder acquiring its picture (ui/DabPicker draws only what is on
  // screen, so most bitmaps arrive long after the scan that found them).
  uint64_t generation() const noexcept { return generation_; }

  // The index, serialised. Exposed so `--selftest` can round-trip it without
  // a filesystem, the way app/BrushLibraryFile exposes its own text form.
  std::string indexText() const;
  void parseIndex(const std::string& text);

 private:
  std::string userRoot_;
  std::string importedRoot_;
  std::string indexPath_;
  std::vector<DabEntry> entries_;
  std::vector<DabRefusal> refusals_;
  bool indexLoaded_ = false;
  size_t decodeCount_ = 0;
  uint64_t generation_ = 1;

  // Decodes one file into however many entries it yields (one for an image or
  // a `.gbr`, N for a `.gih`). Appends to `out`; returns false and fills
  // `note` when the file is not a tip this build can read.
  bool decodeFile(const std::string& root, DabRoot which, const std::string& relPath,
                  std::vector<DabEntry>& out, std::string& note);
};

// §4's rule, exposed on its own so `--selftest` can exercise it directly and
// so the `.abr` extractor can share it rather than reimplementing the
// polarity. `pixels` is io/ImageDecode's linear straight-alpha RGBA.
BrushTipBitmap coverageFromDecodedImage(uint32_t width, uint32_t height,
                                        const std::vector<float>& pixels);

// FNV-1a over a coverage bitmap's bytes, with the dimensions mixed in first
// so that two different-shaped tips with the same byte sequence cannot
// collide (§3).
uint64_t dabFingerprint(const BrushTipBitmap& bitmap) noexcept;

// **Extraction: a Photoshop sampled tip, written out so it survives the pack.**
//
// A `.abr`'s `samp` block holds real scanned tips, and until now the only
// place a preset could keep one was a `shared_ptr` that lived exactly as long
// as the library stayed loaded. brush/Library.hpp is blunt about what that
// cost: Duplicate on a sampled-tip preset copies the pointer, `Save` writes
// seven scalars and no bitmap, "so a saved duplicate of a sampled-tip brush
// reloads next launch as the round procedural tip". That header proposes
// closing it by naming the source `.abr` and re-resolving on load, with its
// own failure mode when the file moves or is edited.
//
// **This closes it differently, and better: the tip stops depending on the
// pack at all.** Each imported tip is written once to
// `dabs-imported/<uuid>.png`, the uuid being the `samp` block's own -- already
// the `sampledData` join key, so nothing is invented -- and the preset stores
// the id `abr:<uuid>`. Unloading the source library, moving it, or deleting it
// no longer takes the bitmap with it, and re-importing the same pack finds the
// file already there and writes nothing.
//
// **The mask goes in the ALPHA channel, over black RGB**, which is a decision
// and not a formatting detail. §4's rule reads a picture's alpha when it has a
// non-trivial one and falls back to `1 - luminance` when it does not, so a
// greyscale PNG of a tip would round-trip INVERTED. Alpha-over-black is
// correct in both branches: the normal case takes the alpha, and a tip that
// happens to be opaque everywhere (a solid sample) falls through to
// `1 - luminance(black)` = 1, which is the full coverage it should be. Writing
// white RGB instead would make that second case come out empty.
//
// Returns the ids written or already present, in the order given. Existing
// files are never overwritten -- the uuid identifies the tip, so a file
// already at that name IS this tip, and rewriting it would throw away any
// touch-up the user made in an image editor.
std::vector<std::string> extractAbrTips(
    const std::string& importedRoot,
    const std::vector<std::pair<std::string, BrushTipBitmap>>& tips,
    std::vector<std::string>* notesOut = nullptr);

// Fills every preset's `tipBitmap` from its `dabId`, for a library just read
// off disk.
//
// **This is the load half of the persistence.** `user-presets.txt` stores the
// id and not the bitmap (brush/Library.hpp's `dabId`), so a freshly parsed
// preset has a name for its tip and no tip; this is what turns the one into
// the other. Returns how many were resolved, and appends one note per id that
// no longer names anything -- a preset whose PNG the user deleted paints with
// its procedural tip, visibly a different brush, which is better than a
// silent one.
//
// Takes the library by reference and mutates it, rather than returning a copy,
// because it runs once at launch over a vector that is already the live one.
size_t resolveDabIds(BrushLibrary& lib, DabLibrary& dabs,
                     std::vector<std::string>* notesOut = nullptr);

// `--dab-scan` -- scan both roots and print what is there, what changed and
// what was refused. Headless, GPU-free, and the answer to "why is the file I
// dropped in not showing up", which a picker with no diagnostics cannot
// answer. Reads and writes only the index; never creates either root.
//
// Returns a process exit code: 0 always, including on an empty library, which
// is a legitimate state and not a failure.
int runDabScan();

// `--dab-import <file.abr>` -- extract one pack's sampled tips into the
// imported root and report what landed. Headless and GPU-free.
//
// **The same two calls app/BrushLibraryFile makes**, in the same order, with
// nothing between them: `importAbrBrushes()` then `extractAbrTips()`. It
// exists so the extraction can be measured against real packs without a
// window -- the plan's own verification is "import all four and count the
// `abr:` dabs" -- and because "why did my tip not survive a relaunch" needs
// an answer that does not begin with "open the application".
//
// Returns a process exit code: 0 on a successful import, 1 if the file cannot
// be opened or the parser refuses it.
int runDabImport(const char* path);

}  // namespace np
