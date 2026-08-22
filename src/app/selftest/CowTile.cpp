#include "app/selftest/Support.hpp"

namespace np {

namespace {

// An instrumented tile type, for the one property that cannot be proven from
// outside the store: that the reference count reaching zero frees each tile
// **exactly once**. Identity comparisons prove sharing and isolation, and RSS
// proves the bytes come back, but neither can tell "freed once" from "freed
// twice" or from "leaked and the allocator reused the address". A counting
// type can, and `TileStoreOf<T>` is a template precisely so a test can supply
// one -- no production API had to be opened up for this.
//
// It is not a tile in any other sense: no texels, no size assertion, and
// nothing outside this section instantiates the store with it.
struct CowProbeStats {
  long constructed = 0;  // fresh tiles: TileStoreOf::getOrCreate on a miss
  long copied = 0;       // copy-on-write clones: the barrier firing
  long destroyed = 0;
  long live = 0;
  long doubleDestroyed = 0;  // best-effort tripwire; see the destructor
};

CowProbeStats& cowProbeStats() {
  static CowProbeStats stats;
  return stats;
}

class CowProbeTile {
 public:
  CowProbeTile() {
    CowProbeStats& s = cowProbeStats();
    ++s.constructed;
    ++s.live;
  }
  CowProbeTile(const CowProbeTile& other) : value_(other.value_), magic_(kMagic) {
    CowProbeStats& s = cowProbeStats();
    ++s.copied;
    ++s.live;
  }
  CowProbeTile& operator=(const CowProbeTile&) = delete;
  // The magic word is a tripwire, not a proof: reading it after the object's
  // storage has been released is itself undefined, so a double destruction is
  // only *likely* to be caught here. The counters are the real assertion --
  // `destroyed == constructed + copied` and `live == 0` with no other
  // bookkeeping in between is what "exactly once" reduces to.
  ~CowProbeTile() {
    CowProbeStats& s = cowProbeStats();
    if (magic_ != kMagic) {
      ++s.doubleDestroyed;
      return;
    }
    magic_ = 0;
    ++s.destroyed;
    --s.live;
  }

  int value() const noexcept { return value_; }
  void setValue(int v) noexcept { value_ = v; }

 private:
  static constexpr uint32_t kMagic = 0xC0FFEEu;
  int value_ = 0;
  uint32_t magic_ = kMagic;
};

}  // namespace

bool runCowTileTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
  };

#if defined(NP_USE_OIIO)
  constexpr bool kOiioBuild = true;
#else
  constexpr bool kOiioBuild = false;
#endif

  using Clock = std::chrono::steady_clock;
  auto seconds = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
  };
  constexpr double kMiB = 1024.0 * 1024.0;

  // --- Tolerances: there are none on any correctness claim ---------------
  //
  // Every assertion in this section except the four timing/RSS *reports* is
  // at **exactly zero tolerance**, and that is a property of what is being
  // tested rather than of a carefully chosen fixture. Copy-on-write is not
  // arithmetic: a shared tile is proven shared by **pointer identity**
  // (`a.find(c) == b.find(c)`, the same object, not equal contents -- which
  // is the distinction PLAN.md's step asks for by name), a clone is proven by
  // the same pointers becoming different, and isolation is proven by
  // `memcmp` over raw half words. No value is converted, interpolated or
  // rounded anywhere in this mechanism, so a tolerance here would be hiding
  // something rather than allowing for something.
  //
  // The timings are machine numbers and are **printed, not asserted**, with
  // two exceptions that are ratios rather than absolutes and are given a 10x
  // margin over what is measured on this machine -- see them at the point of
  // use.

  // --- Part A: the write barrier is the only door, checked by the compiler-
  //
  // The soundness argument in core/TileStore.hpp is an *enumeration* of the
  // ways to obtain a mutable tile, so the enumeration being closed is the
  // load-bearing claim. Two of the three closures are compile-time facts, and
  // a `static_assert`-style constant is the honest way to assert a
  // compile-time fact -- these are `false` only if a non-const overload is
  // put back, in which case this file stops compiling as written or the line
  // below says FAIL.
  {
    constexpr bool findIsConstOnly =
        std::is_same_v<decltype(std::declval<TileStore&>().find(TileCoord{})), const Tile*> &&
        std::is_same_v<decltype(std::declval<PigmentTileStore&>().find(TileCoord{})),
                       const PigmentTile*> &&
        std::is_same_v<decltype(std::declval<MaskTileStore&>().find(TileCoord{})),
                       const MaskTile*>;
    check(findIsConstOnly,
          "barrier: find() is const-only on all three stores, even on a non-const store -- "
          "the T* overload that used to sit there was an unbarriered write handle");

    constexpr bool iterationIsConstOnly =
        std::is_same_v<decltype(*std::declval<TileStore&>().begin()),
                       std::pair<const TileCoord&, const Tile&>> &&
        std::is_same_v<decltype(*std::declval<MaskTileStore&>().begin()),
                       std::pair<const TileCoord&, const MaskTile&>>;
    check(iterationIsConstOnly,
          "barrier: iterating a NON-const store still yields `const T&`, so a range-for is "
          "not a third way to write a tile without copying it first");

    // And the third closure, which is a runtime fact: the two barriers agree.
    TileStore a;
    a.getOrCreate(TileCoord{0, 0}).writePixel(PixelCoord{1, 1}, {0.25f, 0.5f, 0.75f, 1.0f});
    TileStore b = a;
    const Tile* beforeA = a.find(TileCoord{0, 0});
    Tile* viaFindForWrite = a.findForWrite(TileCoord{0, 0});
    check(viaFindForWrite != nullptr && viaFindForWrite != beforeA &&
              b.find(TileCoord{0, 0}) == beforeA,
          "barrier: findForWrite() copies a shared tile exactly as getOrCreate() does, and "
          "leaves the other holder pointing at the original");
    check(a.findForWrite(TileCoord{9, 9}) == nullptr,
          "barrier: findForWrite() on a coordinate that does not exist allocates nothing and "
          "returns null -- it is find()'s write half, not getOrCreate()");
  }

  // --- Part B: sharing, proven by identity rather than by equality -------

  {
    Document src = Document::createBlank(256, 256, WorkingSpace{});
    src.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0})
        .writePixel(PixelCoord{3, 4}, {0.25f, 0.5f, 0.75f, 1.0f});
    src.layers[0].rgbTiles->getOrCreate(TileCoord{1, 0})
        .writePixel(PixelCoord{5, 6}, {0.125f, 0.25f, 0.375f, 0.5f});

    Document copy = src;

    const Tile* srcT0 = src.layers[0].rgbTiles->find(TileCoord{0, 0});
    const Tile* copyT0 = copy.layers[0].rgbTiles->find(TileCoord{0, 0});
    const Tile* srcT1 = src.layers[0].rgbTiles->find(TileCoord{1, 0});
    const Tile* copyT1 = copy.layers[0].rgbTiles->find(TileCoord{1, 0});

    check(srcT0 != nullptr && srcT0 == copyT0 && srcT1 != nullptr && srcT1 == copyT1,
          "share: after `Document copy = src;` both documents' tiles are the SAME OBJECTS -- "
          "identical addresses, not merely identical contents");
    check(src.layers[0].rgbTiles->tileUseCount(TileCoord{0, 0}) == 2 &&
              copy.layers[0].rgbTiles->tileUseCount(TileCoord{0, 0}) == 2,
          "share: and both agree the tile has exactly two holders");
    check(src.layers[0].rgbTiles->occupiedTileCount() == 2 &&
              copy.layers[0].rgbTiles->occupiedTileCount() == 2 &&
              documentSharedTileCount(src) == 2,
          "share: the copy sees every tile the source does, and all of them are shared");
    check(documentTileBytes(copy) == 2 * sizeof(Tile) && documentExclusiveTileBytes(copy) == 0,
          "share: the copy SHOWS 256 KiB and would give back 0 -- the two numbers step 7's "
          "byte budget must not confuse (core/TileShare.hpp)");

    // The property the whole step turns on.
    copy.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0})
        .writePixel(PixelCoord{3, 4}, {1.0f, 1.0f, 1.0f, 1.0f});

    const Tile* srcT0After = src.layers[0].rgbTiles->find(TileCoord{0, 0});
    const Tile* copyT0After = copy.layers[0].rgbTiles->find(TileCoord{0, 0});
    const auto srcPixel = srcT0After->readPixel(PixelCoord{3, 4});
    const auto copyPixel = copyT0After->readPixel(PixelCoord{3, 4});

    check(srcT0After == srcT0 && copyT0After != srcT0,
          "write: the writer got a NEW tile and the other holder kept the original object -- "
          "the source's own pointer did not move");
    check(srcPixel[0] == 0.25f && srcPixel[1] == 0.5f && srcPixel[2] == 0.75f &&
              copyPixel[0] == 1.0f && copyPixel[1] == 1.0f && copyPixel[2] == 1.0f,
          "write: a write to one copy is NOT visible in the other -- the correctness "
          "property the entire step rests on, at exactly zero tolerance");
    check(src.layers[0].rgbTiles->tileUseCount(TileCoord{0, 0}) == 1 &&
              copy.layers[0].rgbTiles->tileUseCount(TileCoord{0, 0}) == 1,
          "write: the copy dropped the count back to one holder each");
    check(src.layers[0].rgbTiles->find(TileCoord{1, 0}) ==
              copy.layers[0].rgbTiles->find(TileCoord{1, 0}),
          "write: the tile that was NOT written is still shared -- the copy is per tile, not "
          "per store");
    check(documentExclusiveTileBytes(src) == sizeof(Tile) &&
              documentExclusiveTileBytes(copy) == sizeof(Tile),
          "write: and each document now exclusively holds exactly the one tile it wrote");

    // A second write to the same, now-unique tile must not copy again.
    const Tile* beforeSecond = copy.layers[0].rgbTiles->find(TileCoord{0, 0});
    copy.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0})
        .writePixel(PixelCoord{7, 7}, {0.5f, 0.5f, 0.5f, 1.0f});
    check(copy.layers[0].rgbTiles->find(TileCoord{0, 0}) == beforeSecond,
          "write: the SECOND write to the same tile is in place -- copy-on-FIRST-write, not "
          "copy-on-every-write");

    // The reference-taken-before-the-copy hazard core/TileStore.hpp names, in
    // the order that is correct: write, then copy.
    Document ordered = Document::createBlank(64, 64, WorkingSpace{});
    Tile& handle = ordered.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    handle.writePixel(PixelCoord{0, 0}, {0.75f, 0.0f, 0.0f, 1.0f});
    Document orderedCopy = ordered;
    ordered.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0})
        .writePixel(PixelCoord{0, 0}, {0.0f, 0.75f, 0.0f, 1.0f});
    const auto keptPixel =
        orderedCopy.layers[0].rgbTiles->find(TileCoord{0, 0})->readPixel(PixelCoord{0, 0});
    check(keptPixel[0] == 0.75f && keptPixel[1] == 0.0f,
          "write: taking a reference, writing, THEN copying is the supported order, and the "
          "copy holds the pre-copy content");
  }

  // --- Part C: all three tile types, and the mask's 1.0 default ----------

  {
    // RGB is covered above. Pigment: 224 KiB, latent x mass, zero default.
    Document pig = Document::createBlank(256, 256, WorkingSpace{});
    pig.layers[0] = makePigmentLayer("wash");
    PigmentTexel t;
    t.latent.c = {0.2f, 0.4f, 0.6f};
    t.latent.res = {0.1f, 0.1f, 0.1f};
    t.mass = 0.5f;
    pig.layers[0].pigmentTiles->getOrCreate(TileCoord{0, 0}).writeTexel(PixelCoord{2, 2}, t);
    Document pigCopy = pig;
    check(pig.layers[0].pigmentTiles->find(TileCoord{0, 0}) ==
              pigCopy.layers[0].pigmentTiles->find(TileCoord{0, 0}),
          "types: a PigmentTileStore shares by identity too -- one template, three "
          "instantiations, one mechanism");
    PigmentTexel other = t;
    other.mass = 0.125f;
    pigCopy.layers[0].pigmentTiles->getOrCreate(TileCoord{0, 0}).writeTexel(PixelCoord{2, 2},
                                                                           other);
    check(pig.layers[0].pigmentTiles->find(TileCoord{0, 0})
                  ->readTexel(PixelCoord{2, 2})
                  .mass == 0.5f &&
              pigCopy.layers[0].pigmentTiles->find(TileCoord{0, 0})
                      ->readTexel(PixelCoord{2, 2})
                      .mass == 0.125f,
          "types: and a pigment write to one copy leaves the other's mass untouched");
    check(pig.layers[0].pigmentTiles->tileBytes() == 224 * 1024,
          "types: a pigment store's tileBytes() is 224 KiB per tile, not 128 -- "
          "core/TileShare sums the three stores rather than multiplying one size");

    // Mask: 32 KiB, and the one tile type whose "nothing here" value is 1.0.
    // The trap this guards is a clone implemented as a *fresh default* rather
    // than a copy: for core::Tile and core::PigmentTile a fresh default is
    // zero and the bug shows up as lost paint, but for a MaskTile a fresh
    // default is all-reveal, which composites plausibly and hides the bug.
    Document masked = Document::createBlank(256, 256, WorkingSpace{});
    check(addLayerMask(masked, 0).ok, "types: a mask can be added to the layer under test");
    MaskTile& mt = masked.layers[0].mask->getOrCreate(TileCoord{0, 0});
    mt.writeCoverage(PixelCoord{1, 1}, 0.25f);
    Document maskedCopy = masked;
    check(masked.layers[0].mask->find(TileCoord{0, 0}) ==
              maskedCopy.layers[0].mask->find(TileCoord{0, 0}),
          "types: a MaskTileStore shares by identity");
    maskedCopy.layers[0].mask->getOrCreate(TileCoord{0, 0}).writeCoverage(PixelCoord{2, 2}, 0.0f);
    const MaskTile* origMask = masked.layers[0].mask->find(TileCoord{0, 0});
    const MaskTile* copyMask = maskedCopy.layers[0].mask->find(TileCoord{0, 0});
    check(copyMask->readCoverage(PixelCoord{1, 1}) == 0.25f,
          "mask: the CLONE carries the pre-share 0.25 sample -- proof the copy-on-write copy "
          "is a copy of the tile and not a freshly defaulted one (which for a MaskTile would "
          "be all-1.0 and would look plausible)");
    check(copyMask->readCoverage(PixelCoord{3, 3}) == 1.0f &&
              origMask->readCoverage(PixelCoord{3, 3}) == 1.0f,
          "mask: every untouched sample in both tiles is still exactly 1.0 -- core/Mask.hpp's "
          "REVEAL default survives being shared and then cloned");
    check(origMask->readCoverage(PixelCoord{2, 2}) == 1.0f &&
              copyMask->readCoverage(PixelCoord{2, 2}) == 0.0f,
          "mask: and the 0.0 written into the copy is invisible in the original");
    check(masked.layers[0].mask->tileBytes() == 32 * 1024,
          "mask: a mask store's tileBytes() is 32 KiB per tile");

    // An engaged-but-empty mask store -- core/Mask.hpp's canonical "reveal
    // all" -- shares as nothing, because there is nothing to share.
    Document emptyMask = Document::createBlank(64, 64, WorkingSpace{});
    check(addLayerMask(emptyMask, 0).ok, "mask: reveal-all mask added");
    Document emptyMaskCopy = emptyMask;
    check(emptyMaskCopy.layers[0].mask.has_value() &&
              emptyMaskCopy.layers[0].mask->occupiedTileCount() == 0 &&
              layerTileBytes(emptyMaskCopy.layers[0]) == 0,
          "mask: copying a layer whose mask is the canonical zero-tile reveal-all costs zero "
          "bytes and still arrives as a mask");
  }

  // --- Part D: the refcount reaching zero frees exactly once -------------

  {
    CowProbeStats& s = cowProbeStats();
    s = CowProbeStats{};

    {
      TileStoreOf<CowProbeTile> a;
      a.getOrCreate(TileCoord{0, 0}).setValue(11);
      a.getOrCreate(TileCoord{1, 0}).setValue(22);
      check(s.constructed == 2 && s.copied == 0 && s.live == 2,
            "lifetime: two fresh tiles cost two constructions and zero copies");

      TileStoreOf<CowProbeTile> b = a;
      TileStoreOf<CowProbeTile> c = a;
      check(s.constructed == 2 && s.copied == 0 && s.live == 2 &&
                a.tileUseCount(TileCoord{0, 0}) == 3,
            "lifetime: two more stores sharing them construct and copy NOTHING, and the "
            "count reads three");

      b.getOrCreate(TileCoord{0, 0}).setValue(33);
      check(s.copied == 1 && s.live == 3 && a.tileUseCount(TileCoord{0, 0}) == 2 &&
                b.tileUseCount(TileCoord{0, 0}) == 1,
            "lifetime: one write makes exactly ONE copy, and the remaining sharers drop to "
            "two");
      check(a.find(TileCoord{0, 0})->value() == 11 && b.find(TileCoord{0, 0})->value() == 33 &&
                c.find(TileCoord{0, 0})->value() == 11,
            "lifetime: and the value moved in exactly one of the three stores");

      // Destroying a sharer must free nothing.
      {
        TileStoreOf<CowProbeTile> temp = c;
        check(a.tileUseCount(TileCoord{1, 0}) == 4, "lifetime: a fourth sharer reads as four");
      }
      check(s.destroyed == 0 && s.live == 3 && a.tileUseCount(TileCoord{1, 0}) == 3,
            "lifetime: destroying a sharer frees NOTHING and returns the count to three");

      c.unshareAll();
      check(s.copied == 3 && s.live == 5,
            "lifetime: unshareAll() copies exactly the tiles that were shared -- two here, "
            "for a running total of three copies");
    }

    check(s.destroyed == s.constructed + s.copied && s.destroyed == 5 && s.live == 0,
          "lifetime: every tile ever made -- 2 fresh + 3 copy-on-write clones -- is destroyed "
          "EXACTLY once when the last reference goes, and none is leaked");
    check(s.doubleDestroyed == 0,
          "lifetime: and the double-destruction tripwire never fired");
  }

  // --- Part E: io/TileResidency composes rather than competes ------------

  {
    // io/TileResidency has its own copy-on-first-write, against a *file*.
    // This one is against another in-memory holder. The claim is that they
    // stack: a residency's owned store is an ordinary TileStore, so sharing
    // it and then promoting a tile through `tileForWrite()` unshares first.
    TileStore owned;
    owned.getOrCreate(TileCoord{2, 2}).writePixel(PixelCoord{0, 0}, {0.5f, 0.0f, 0.0f, 1.0f});
    LayerResidency eager = LayerResidency::adoptEager(std::move(owned));

    TileStore snapshot = eager.ownedTiles();  // a share of what the residency owns
    check(snapshot.find(TileCoord{2, 2}) == eager.ownedTiles().find(TileCoord{2, 2}) &&
              snapshot.tileUseCount(TileCoord{2, 2}) == 2,
          "residency: a residency's owned tiles share with an ordinary TileStore -- the two "
          "copy-on-write mechanisms sit on the same storage");

    Tile* writable = eager.tileForWrite(TileCoord{2, 2});
    check(writable != nullptr && writable != snapshot.find(TileCoord{2, 2}),
          "residency: tileForWrite() on a SHARED owned tile goes through findForWrite() and "
          "copies before returning a writable pointer");
    writable->writePixel(PixelCoord{0, 0}, {0.0f, 0.5f, 0.0f, 1.0f});
    const auto kept = snapshot.find(TileCoord{2, 2})->readPixel(PixelCoord{0, 0});
    check(kept[0] == 0.5f && kept[1] == 0.0f,
          "residency: and the snapshot still reads what was there before the promotion");
    check(eager.residentBytes() == sizeof(Tile),
          "residency: residentBytes() still counts one tile -- it measures what the residency "
          "shows, which is the same number it measured before this step");
  }

  // --- Part F: the composite is byte-identical over shared tiles ---------

  {
    // Steps 1-5's regression boundary, restated for this step: sharing must be
    // invisible to the walk. Deep and shared copies of the same document must
    // composite to the same bits.
    Document doc = Document::createBlank(192, 128, WorkingSpace{});
    doc.layers[0].name = "under";
    for (int32_t i = 0; i < 40; ++i) {
      const PixelCoord at{i * 3, i};
      const float a = 0.25f + static_cast<float>(i % 3) * 0.25f;
      doc.layers[0].rgbTiles->getOrCreate(tileCoordAt(at))
          .writePixel(tileLocalOffset(at), {0.5f * a, 0.25f * a, 0.125f * a, a});
    }
    Layer top = makeRgbLayer("over");
    top.opacity = 0.5f;
    for (int32_t i = 0; i < 40; ++i) {
      const PixelCoord at{i * 3 + 1, i};
      top.rgbTiles->getOrCreate(tileCoordAt(at))
          .writePixel(tileLocalOffset(at), {0.75f, 0.5f, 0.25f, 1.0f});
    }
    check(addLayer(doc, 1, std::move(top)).ok, "composite: two-layer fixture built");

    Document shared = doc;
    Document deep = doc;
    unshareDocumentTiles(deep);
    check(documentSharedTileCount(deep) == 0 && documentTileCount(deep) == documentTileCount(doc),
          "composite: unshareDocumentTiles() leaves an independent document with the same "
          "tiles -- the pre-step-6 deep copy, still available on demand");

    const std::vector<float> a = compositeDocumentPremultiplied(doc);
    const std::vector<float> b = compositeDocumentPremultiplied(shared);
    const std::vector<float> c = compositeDocumentPremultiplied(deep);
    check(!a.empty() && a.size() == b.size() && a.size() == c.size() &&
              std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0 &&
              std::memcmp(a.data(), c.data(), a.size() * sizeof(float)) == 0,
          "composite: the shared copy and the deep copy composite BIT-IDENTICALLY to the "
          "original -- steps 1-5's boundary holds to the ulp");

    // And a write through one of them moves only that one's composite.
    shared.layers[1].rgbTiles->getOrCreate(TileCoord{0, 0})
        .writePixel(PixelCoord{1, 1}, {1.0f, 1.0f, 1.0f, 1.0f});
    const std::vector<float> afterWrite = compositeDocumentPremultiplied(doc);
    check(std::memcmp(a.data(), afterWrite.data(), a.size() * sizeof(float)) == 0,
          "composite: writing into the shared copy leaves the original's composite unchanged, "
          "bit for bit");
  }

  // --- Part G: the measurements this step is justified by ----------------
  //
  // The whole case for copy-on-write is cost, so these are printed with the
  // fixture that produced them. The fixture is io/TileResidency's own
  // "realistic document": 2048x2048, one RGB layer, all 256 tiles occupied,
  // 32.0 MiB of half data -- the same one app/Journal.hpp's 0.080-0.085 s
  // write cost and its "deep copy = 0.002-0.003 s" figure were measured
  // against, so the baseline below is directly comparable to the number this
  // step is asked to beat. It is re-measured here rather than quoted.

  constexpr int32_t kTilesPerSide = 16;
  auto buildRealisticDocument = [&]() {
    Document doc = Document::createBlank(kTilesPerSide * kTileSize, kTilesPerSide * kTileSize,
                                         WorkingSpace{});
    doc.layers[0].name = "Source";
    TileStore& tiles = *doc.layers[0].rgbTiles;
    std::array<uint16_t, Tile::kTexelCount> base{};
    for (size_t i = 0; i < Tile::kTexelCount; i += 4) {
      const float t = static_cast<float>(i % 8192) / 8192.0f;
      base[i + 0] = floatToHalf(t);
      base[i + 1] = floatToHalf(1.0f - t);
      base[i + 2] = floatToHalf(t * t);
      base[i + 3] = floatToHalf(1.0f);
    }
    for (int32_t ty = 0; ty < kTilesPerSide; ++ty) {
      for (int32_t tx = 0; tx < kTilesPerSide; ++tx) {
        Tile& tile = tiles.getOrCreate(TileCoord{tx, ty});
        std::memcpy(tile.data(), base.data(), Tile::kTexelCount * sizeof(uint16_t));
        for (int32_t k = 0; k < 8; ++k) {
          const float a = static_cast<float>(tx * 37 + ty * 11 + k) / 512.0f;
          tile.writePixel(PixelCoord{k, k}, {a, 1.0f - a, a * 0.5f, 1.0f});
        }
      }
    }
    return doc;
  };

  const Document fixture = buildRealisticDocument();
  const size_t fixtureTiles = documentTileCount(fixture);
  const size_t fixtureBytes = documentTileBytes(fixture);
  check(fixtureTiles == 256 && fixtureBytes == 256 * sizeof(Tile),
        "measure: the fixture is io/TileResidency's realistic document -- 2048x2048, 256 "
        "tiles, 32.0 MiB");

  // G1 -- a document copy, before and after. Both timed in THIS binary, so
  // the comparison does not depend on a number recorded at a previous commit.
  {
    double cowBest = 1e9, deepBest = 1e9;
    for (int rep = 0; rep < 5; ++rep) {
      {
        const auto t0 = Clock::now();
        Document shared = fixture;
        const auto t1 = Clock::now();
        cowBest = std::min(cowBest, seconds(t0, t1));
        // Touch it so the copy cannot be optimised away.
        if (documentTileCount(shared) != fixtureTiles) cowBest = 1e9;
      }
      {
        const auto t0 = Clock::now();
        Document deep = fixture;
        unshareDocumentTiles(deep);
        const auto t1 = Clock::now();
        deepBest = std::min(deepBest, seconds(t0, t1));
        if (documentTileCount(deep) != fixtureTiles) deepBest = 1e9;
      }
    }
    std::printf(
        "[selftest] cow: document copy, %zu tiles / %.1f MiB "
        "[measured] deep %.4f s  shared %.6f s  (%.0fx)\n",
        fixtureTiles, static_cast<double>(fixtureBytes) / kMiB, deepBest, cowBest,
        cowBest > 0.0 ? deepBest / cowBest : 0.0);
    std::printf(
        "[selftest] cow: per tile [measured] deep %.3f us  shared %.3f us  "
        "(one atomic increment + one unordered_map node)\n",
        deepBest * 1e6 / static_cast<double>(fixtureTiles),
        cowBest * 1e6 / static_cast<double>(fixtureTiles));

    // The atomic half of that, isolated -- core/TileStore.hpp claims the
    // refcount is the small part of a shared copy and this is the number
    // behind the claim. `std::shared_ptr`'s count is atomic, which is what
    // makes step 7's "evict a history tail on a background thread" sound, so
    // this is the price of that soundness. Measured on ONE hot control block,
    // deliberately: this is the cost of the instruction pair, with the cache
    // misses of walking 256 cold control blocks kept out of it, because those
    // are the map's cost and not the counter's.
    {
      std::shared_ptr<Tile> one = std::make_shared<Tile>();
      constexpr long kIters = 4000000;
      volatile long sink = 0;
      double best = 1e9;
      for (int rep = 0; rep < 3; ++rep) {
        const auto t0 = Clock::now();
        for (long i = 0; i < kIters; ++i) {
          std::shared_ptr<Tile> held = one;  // one atomic increment
          sink = sink + held.use_count();    // and one decrement at scope exit
        }
        const auto t1 = Clock::now();
        best = std::min(best, seconds(t0, t1));
      }
      const double perPair = best * 1e9 / static_cast<double>(kIters);
      const double perTile = cowBest * 1e9 / static_cast<double>(fixtureTiles);
      std::printf(
          "[selftest] cow: refcount, isolated [measured] one atomic increment+decrement pair "
          "%.2f ns on a hot control block -- %.1f%% of the %.0f ns a shared copy spends per "
          "tile; the rest is the unordered_map node and the cold control block\n",
          perPair, 100.0 * perPair / perTile, perTile);
    }

    // A ratio, not an absolute, and with a 10x margin over what this machine
    // measures (~30-60x). If a shared copy of a 32 MiB document ever stops
    // being at least three times cheaper than the deep copy it replaced, the
    // mechanism has stopped paying for itself and this should fail rather
    // than be discovered in a profile.
    check(cowBest > 0.0 && deepBest / cowBest >= 3.0,
          "measure: a shared document copy is at least 3x cheaper than the deep copy it "
          "replaced (a 10x margin under what this machine measures)");
  }

  // G2 -- the cost of the write barrier itself: the first write to a shared
  // tile pays for a 128 KiB copy; the second pays nothing.
  {
    double firstBest = 1e9, secondBest = 1e9;
    for (int rep = 0; rep < 5; ++rep) {
      Document shared = fixture;
      TileStore& tiles = *shared.layers[0].rgbTiles;
      const auto t0 = Clock::now();
      for (int32_t ty = 0; ty < kTilesPerSide; ++ty) {
        for (int32_t tx = 0; tx < kTilesPerSide; ++tx) {
          tiles.getOrCreate(TileCoord{tx, ty}).writePixel(PixelCoord{0, 0},
                                                          {1.0f, 0.0f, 0.0f, 1.0f});
        }
      }
      const auto t1 = Clock::now();
      for (int32_t ty = 0; ty < kTilesPerSide; ++ty) {
        for (int32_t tx = 0; tx < kTilesPerSide; ++tx) {
          tiles.getOrCreate(TileCoord{tx, ty}).writePixel(PixelCoord{0, 1},
                                                          {0.0f, 1.0f, 0.0f, 1.0f});
        }
      }
      const auto t2 = Clock::now();
      firstBest = std::min(firstBest, seconds(t0, t1));
      secondBest = std::min(secondBest, seconds(t1, t2));
    }
    const double perFirst = firstBest * 1e6 / static_cast<double>(fixtureTiles);
    const double perSecond = secondBest * 1e6 / static_cast<double>(fixtureTiles);
    std::printf(
        "[selftest] cow: one texel write [measured] first-touch of a SHARED tile %.3f us  "
        "already-unique %.3f us  (128 KiB copy = %.3f us)\n",
        perFirst, perSecond, perFirst - perSecond);
    check(perFirst > perSecond,
          "measure: the first write to a shared tile costs measurably more than a write to a "
          "unique one -- the copy is real and is paid once");
  }

  // G3 -- memory for N shared copies against N deep copies, at the realistic
  // tile count. Measured as process RSS (app/Memory's task_info, the same
  // source the idle-RSS gate uses), not as an estimate.
  {
    constexpr int kCopies = 16;
    const size_t rssBase = currentResidentBytes();
    double sharedDeltaMiB = 0.0, deepDeltaMiB = 0.0;
    size_t sharedShown = 0, sharedExclusive = 0;

    {
      std::vector<Document> copies;
      copies.reserve(kCopies);
      for (int i = 0; i < kCopies; ++i) copies.push_back(fixture);
      const size_t rssShared = currentResidentBytes();
      sharedDeltaMiB = static_cast<double>(rssShared - std::min(rssShared, rssBase)) / kMiB;
      for (const Document& d : copies) {
        sharedShown += documentTileBytes(d);
        sharedExclusive += documentExclusiveTileBytes(d);
      }
    }

    const size_t rssMid = currentResidentBytes();
    {
      std::vector<Document> copies;
      copies.reserve(kCopies);
      for (int i = 0; i < kCopies; ++i) {
        copies.push_back(fixture);
        unshareDocumentTiles(copies.back());
      }
      const size_t rssDeep = currentResidentBytes();
      deepDeltaMiB = static_cast<double>(rssDeep - std::min(rssDeep, rssMid)) / kMiB;
    }

    std::printf(
        "[selftest] cow: %d copies of a %.1f MiB document [measured] shared +%.1f MiB RSS  "
        "deep +%.1f MiB RSS\n",
        kCopies, static_cast<double>(fixtureBytes) / kMiB, sharedDeltaMiB, deepDeltaMiB);
    std::printf(
        "[selftest] cow: the same %d copies account as %.1f MiB SHOWN, %.1f MiB EXCLUSIVE -- "
        "the second is what a byte-bounded history may spend\n",
        kCopies, static_cast<double>(sharedShown) / kMiB,
        static_cast<double>(sharedExclusive) / kMiB);

    check(sharedShown == static_cast<size_t>(kCopies) * fixtureBytes && sharedExclusive == 0,
          "measure: 16 untouched shared copies SHOW 512 MiB and hold 0 MiB exclusively -- "
          "every tile still belongs to the original too");
    // A 5x margin: 16 shared copies cost 16 unordered_maps of 256 pointers,
    // i.e. tens of KiB, against 480 MiB of tiles they did not copy. Anything
    // above a fifth of the deep figure means sharing has silently stopped.
    check(deepDeltaMiB > 5.0 * sharedDeltaMiB,
          "measure: and they cost more than 5x less resident memory than 16 deep copies of "
          "the same document");
  }

  // G4 -- per-read overhead on the composite path, which is the hot path
  // steps 1-5 all extended. Two numbers, because they bound it from opposite
  // ends: the real walk over the real fixture, and a deliberately
  // pathological lookup-per-texel sweep against a plain
  // `unordered_map<TileCoord, Tile>` -- the exact shape the store had before
  // this step -- so the extra indirection is measured on its own.
  {
    double compositeBest = 1e9;
    for (int rep = 0; rep < 3; ++rep) {
      const auto t0 = Clock::now();
      const std::vector<float> out = compositeDocumentPremultiplied(fixture);
      const auto t1 = Clock::now();
      if (!out.empty()) compositeBest = std::min(compositeBest, seconds(t0, t1));
    }
    std::printf(
        "[selftest] cow: composite of the same document [measured] %.4f s "
        "(%.1f Mtexel/s over %d x %d)\n",
        compositeBest,
        static_cast<double>(fixture.width) * fixture.height / compositeBest / 1e6, fixture.width,
        fixture.height);

    // The isolation microbenchmark. One lookup per texel is ~16 384x what the
    // composite actually does (it hoists the lookup out of its texel loop),
    // so whatever this shows, the walk pays 1/16384 of it.
    std::unordered_map<TileCoord, Tile> plain;
    for (const auto& [coord, tile] : *fixture.layers[0].rgbTiles) plain[coord] = tile;
    const TileStore& cow = *fixture.layers[0].rgbTiles;

    constexpr int32_t kStride = 4;  // 1024 lookups per tile, 262 144 in total
    double plainBest = 1e9, cowBest = 1e9;
    for (int rep = 0; rep < 3; ++rep) {
      double acc = 0.0;
      auto t0 = Clock::now();
      for (int32_t ty = 0; ty < kTilesPerSide; ++ty) {
        for (int32_t tx = 0; tx < kTilesPerSide; ++tx) {
          for (int32_t y = 0; y < kTileSize; y += kStride) {
            for (int32_t x = 0; x < kTileSize; x += kStride) {
              const auto it = plain.find(TileCoord{tx, ty});
              acc += it == plain.end() ? 0.0 : it->second.readPixel(PixelCoord{x, y})[0];
            }
          }
        }
      }
      auto t1 = Clock::now();
      plainBest = std::min(plainBest, seconds(t0, t1));

      double acc2 = 0.0;
      t0 = Clock::now();
      for (int32_t ty = 0; ty < kTilesPerSide; ++ty) {
        for (int32_t tx = 0; tx < kTilesPerSide; ++tx) {
          for (int32_t y = 0; y < kTileSize; y += kStride) {
            for (int32_t x = 0; x < kTileSize; x += kStride) {
              const Tile* t = cow.find(TileCoord{tx, ty});
              acc2 += t == nullptr ? 0.0 : t->readPixel(PixelCoord{x, y})[0];
            }
          }
        }
      }
      t1 = Clock::now();
      cowBest = std::min(cowBest, seconds(t0, t1));
      if (acc != acc2) {
        plainBest = 1e9;  // the two sweeps must read the same pixels
      }
    }
    const double lookups = static_cast<double>(fixtureTiles) * (kTileSize / kStride) *
                           (kTileSize / kStride);
    std::printf(
        "[selftest] cow: %.0f find()+readPixel [measured] plain map %.2f ns each  "
        "shared_ptr slot %.2f ns each  (delta %+.2f ns)\n",
        lookups, plainBest * 1e9 / lookups, cowBest * 1e9 / lookups,
        (cowBest - plainBest) * 1e9 / lookups);
  }

  // G5 -- what step 7 will actually build, costed. Ten history entries over
  // the realistic document, one tile edited between each. core/TileShare.hpp
  // works this example through; this is it, measured.
  {
    constexpr int kEntries = 10;
    // A document of its own, not a share of `fixture` -- otherwise every
    // original tile has a holder outside the history and no entry could ever
    // hold anything exclusively. This is the pre-step-6 deep copy, used once,
    // to make the history's arithmetic the history's own.
    Document live = fixture;
    unshareDocumentTiles(live);

    std::vector<Document> entries;
    entries.reserve(kEntries);
    for (int i = 0; i < kEntries; ++i) {
      entries.push_back(live);  // the history entry: a shared copy
      live.layers[0].rgbTiles->getOrCreate(TileCoord{i, 0})
          .writePixel(PixelCoord{100, 100}, {1.0f, 1.0f, 1.0f, 1.0f});
    }

    // How many DISTINCT tiles the whole history actually holds. Counted by
    // address, which is the only honest way to count a shared thing: summing
    // per-document numbers would count every shared tile once per holder.
    auto distinctTiles = [&](const std::vector<Document>& list, const Document& also) {
      std::vector<const Tile*> seen;
      auto add = [&](const Document& d) {
        for (const Layer& layer : d.layers) {
          if (!layer.rgbTiles) continue;
          for (const auto& [coord, tile] : *layer.rgbTiles) {
            (void)coord;
            seen.push_back(&tile);
          }
        }
      };
      add(also);
      for (const Document& d : list) add(d);
      std::sort(seen.begin(), seen.end());
      seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
      return seen.size();
    };

    const size_t held = distinctTiles(entries, live);
    const size_t deepCost = static_cast<size_t>(kEntries + 1) * fixtureBytes;
    size_t shown = 0, exclusive = 0;
    for (const Document& e : entries) {
      shown += documentTileBytes(e);
      exclusive += documentExclusiveTileBytes(e);
    }
    std::printf(
        "[selftest] cow: %d history entries + the live document, one tile edited between "
        "each -- [measured] %.1f MiB if deep-copied, %.2f MiB actually held (%zu distinct "
        "tiles, %.0fx)\n",
        kEntries, static_cast<double>(deepCost) / kMiB,
        static_cast<double>(held * sizeof(Tile)) / kMiB, held,
        static_cast<double>(deepCost) / static_cast<double>(held * sizeof(Tile)));

    check(shown == static_cast<size_t>(kEntries) * fixtureBytes,
          "history: each of the ten entries SHOWS the whole 32.0 MiB document -- what a "
          "pre-copy-on-write history would have paid, ten times over");
    check(held == 256 + static_cast<size_t>(kEntries),
          "history: what is actually held is 256 + 10 tiles -- the document, plus one extra "
          "version per edit, and nothing else");

    // Content, not just addresses: an entry must still read what it was
    // copied at, and the live document must not.
    bool entriesFrozen = true, liveMoved = true;
    for (int i = 0; i < kEntries; ++i) {
      const TileCoord c{i, 0};
      const Tile* asEntered = entries[static_cast<size_t>(i)].layers[0].rgbTiles->find(c);
      const Tile* asLive = live.layers[0].rgbTiles->find(c);
      const Tile* asOriginal = fixture.layers[0].rgbTiles->find(c);
      const size_t bytes = Tile::kTexelCount * sizeof(uint16_t);
      if (asEntered == nullptr || asOriginal == nullptr || asLive == nullptr ||
          std::memcmp(asEntered->data(), asOriginal->data(), bytes) != 0) {
        entriesFrozen = false;
      }
      if (asLive == nullptr || std::memcmp(asLive->data(), asOriginal->data(), bytes) == 0) {
        liveMoved = false;
      }
    }
    check(entriesFrozen,
          "history: every entry still reads the tile exactly as it was when the entry was "
          "taken -- bit for bit against the unedited original");
    check(liveMoved,
          "history: and the live document reads all ten edits -- so the test could have "
          "failed");
    check(entries[3].layers[0].rgbTiles->find(TileCoord{9, 0}) ==
              entries[7].layers[0].rgbTiles->find(TileCoord{9, 0}),
          "history: two entries taken before the same edit still share that tile -- the "
          "sharing is between entries, not only with the live document");

    // The per-entry byte numbers, and the caveat that makes them usable.
    // Dropping ONE entry returns its exclusive bytes; dropping all ten
    // returns ten tiles, which is not the sum. core/TileShare.hpp states this
    // as a lower bound rather than an equality for exactly this reason, and
    // here is the case that separates them.
    check(documentExclusiveTileBytes(entries[0]) == sizeof(Tile) &&
              documentExclusiveTileBytes(entries[5]) == 0,
          "history: the OLDEST entry alone holds 128 KiB exclusively; a middle entry holds 0, "
          "because the entry on each side of it shares everything it has");
    std::printf(
        "[selftest] cow: sum of per-entry exclusive bytes %.2f MiB, but dropping all %d "
        "entries frees %.2f MiB -- exclusive bytes are a LOWER bound on a multi-entry "
        "eviction, not an additive one\n",
        static_cast<double>(exclusive) / kMiB, kEntries,
        static_cast<double>((held - 256) * sizeof(Tile)) / kMiB);
    entries.clear();
    check(distinctTiles(entries, live) == 256,
          "history: and dropping every entry really does return the whole history to the "
          "256 tiles the live document holds");
  }

  // --- Part H: `.npaint` ---------------------------------------------------

  {
    const char* kShared = "selftest_cow_shared.npaint";
    const char* kDeep = "selftest_cow_deep.npaint";
    // The two files the mask's non-vacuity check below writes. Declared here so
    // the "every scratch file this section wrote is removed" check at the end of
    // the block covers them in both build configurations -- in the OFF build
    // they are never created, and `std::remove()` on a file that is not there is
    // exactly as harmless as it needs to be.
    const char* kNameA = "selftest_cow_name_a.npaint";
    const char* kNameB = "selftest_cow_name_b.npaint";

    Document doc = Document::createBlank(256, 256, WorkingSpace{});
    doc.layers[0].name = "base";
    doc.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0})
        .writePixel(PixelCoord{4, 4}, {0.25f, 0.5f, 0.75f, 1.0f});
    doc.layers[0].rgbTiles->getOrCreate(TileCoord{1, 1})
        .writePixel(PixelCoord{9, 9}, {0.125f, 0.25f, 0.5f, 1.0f});
    // A duplicate: core/LayerOps says `Layer copy = doc.layers[index]`, which
    // as of this step is a share rather than a deep copy. Nothing in
    // core/LayerOps changed to make that true.
    const LayerOpResult dup = duplicateLayer(doc, 0);
    check(dup.ok && doc.layers.size() == 2,
          "npaint: duplicateLayer() built the two-layer fixture");
    check(doc.layers[0].rgbTiles->find(TileCoord{0, 0}) ==
              doc.layers[1].rgbTiles->find(TileCoord{0, 0}),
          "npaint: duplicateLayer() now SHARES the source's tiles -- it got copy-on-write "
          "with no edit to core/LayerOps at all, because the deep copy it did was already "
          "spelled as a copy");

    Document deep = doc;
    unshareDocumentTiles(deep);

    const NpaintSaveResult sharedSave = saveNpaint(doc, kShared);
    const NpaintSaveResult deepSave = saveNpaint(deep, kDeep);

    if (kOiioBuild) {
      check(sharedSave.ok && deepSave.ok, "npaint: both the shared and the deep document save");

      // --- Comparing two `.npaint` files, and why it has to mask -----------
      //
      // Sharing is invisible to the format, which is the property that keeps
      // step 4's byte-identity claim true, and the only way to show it is to
      // compare the two files byte for byte. So, stated where the next reader
      // of this block will hit it:
      //
      //   **A `.npaint` is not byte-reproducible across saves, and that is a
      //   property of OpenEXR rather than a bug here.** OpenEXR stamps a
      //   `capDate` header attribute -- "YYYY:MM:DD HH:MM:SS", read off the
      //   wall clock, one per part -- so two saves of the identical document
      //   that land either side of a second boundary differ in those digits.
      //   io/NpaintFile could not make them agree without writing the EXR
      //   header itself.
      //
      // The version of this block that shipped counted raw differing bytes with
      // no masking at all and asserted `differing <= 19`. The assertion always
      // held, but **the number it printed moved**. Measured 2026-08-21 on an
      // M4 Max, 240 runs of the OIIO build: 232 printed 0 and **8 printed 3**,
      // a 3.3% flake rate. The 3 is simply how many digit positions happened to
      // change at those particular second boundaries; a crossing that rolls a
      // ten, a minute or an hour over moves more of them, and 9 has been seen
      // in the wild. Every change in this project is verified by diffing
      // `--selftest` against a baseline and requiring additions only, so a line
      // that rewrites itself with no edit behind it is indistinguishable from a
      // regression -- and cost a real investigation before anyone worked out it
      // was a clock.
      //
      // The fix is to **mask** the capDate fields and then demand *zero*
      // differing bytes. It is deliberately not "keep counting and raise the
      // tolerance until the clock fits": a tolerance wide enough to swallow the
      // stamp is also wide enough to swallow any other nineteen-byte
      // difference, which is the exact failure this suite exists to catch. The
      // masked comparison is proven able to still see one, two checks below.
      //
      // app/selftest/PigmentBasis.cpp needs the same comparison for its own
      // byte-identity assertions and shares Support.hpp's maskCapDates() for
      // it rather than defining a second one: match the pattern, not an
      // offset, because the offsets move with the header. The narrowness
      // both share and neither hides: a *layer name* shaped exactly like a
      // timestamp would be masked too. Nothing in this suite is named that,
      // and the alternative -- parsing the EXR header to find the real
      // attribute offsets -- is a second reader of the format inside a test.
      auto readAll = [](const char* path) {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
      };
      // Counts rather than returns a bool, because the count is what the
      // printed line reports and what the non-vacuity check below asserts on.
      auto maskedDifference = [&](const std::string& a, const std::string& b) {
        const std::string ma = maskCapDates(a), mb = maskCapDates(b);
        size_t differing = 0;
        for (size_t i = 0; i < ma.size() && i < mb.size(); ++i) {
          if (ma[i] != mb[i]) ++differing;
        }
        return differing;
      };

      const std::string bytesA = readAll(kShared);
      const std::string bytesB = readAll(kDeep);
      check(!bytesA.empty() && bytesA.size() == bytesB.size(),
            "npaint: the shared and deep saves are the same size");
      const size_t differing = maskedDifference(bytesA, bytesB);
      std::printf(
          "[selftest] cow: shared vs deep `.npaint`, %zu bytes: %zu differ once OpenEXR's "
          "per-part capDate is masked (a `.npaint` is not byte-reproducible across saves; "
          "that is OpenEXR, not this loader)\n",
          bytesA.size(), differing);
      check(differing == 0,
            "npaint: the shared save and the deep save are byte-identical once OpenEXR's "
            "capDate is masked -- tile sharing is invisible to the file format, so step 4's "
            "byte-identity claim is untouched");

      // --- The mask is not vacuous -----------------------------------------
      //
      // A masking comparator can be made to pass unconditionally by masking too
      // much, so it has to be shown still detecting a real difference *of the
      // width it hides*. Two saves of one document whose only difference is a
      // nineteen-character layer name -- same length, so the file size cannot
      // move and nothing but the value changes, PigmentBasis.cpp's own
      // "mixbox-v2" trick -- must come out nineteen bytes apart, not zero.
      // Neither name is digits-and-colons, so the mask leaves both alone; EXR
      // stores header attributes uncompressed, so one changed character in the
      // name is one changed byte in the file.
      Document named = doc;
      named.layers[0].name = "AAAAAAAAAAAAAAAAAAA";  // 19 characters
      const NpaintSaveResult saveA = saveNpaint(named, kNameA);
      named.layers[0].name = "BBBBBBBBBBBBBBBBBBB";  // 19, and no byte in common
      const NpaintSaveResult saveB = saveNpaint(named, kNameB);
      const std::string namedA = readAll(kNameA), namedB = readAll(kNameB);
      check(saveA.ok && saveB.ok && !namedA.empty() && namedA.size() == namedB.size(),
            "npaint: the two 19-character-name saves are the same size");
      check(maskedDifference(namedA, namedB) == 19,
            "npaint: and they differ in exactly 19 bytes AFTER masking -- the comparator that "
            "reports 0 above can still see a difference the width of the capDate stamp, so "
            "that 0 is a measurement and not an artefact of the mask");

      NpaintLoadResult back = loadNpaint(kShared);
      check(back.ok && back.document.layers.size() == 2,
            "npaint: the shared document loads back with both layers");
      const Tile* l0 = back.document.layers[0].rgbTiles->find(TileCoord{0, 0});
      const Tile* l1 = back.document.layers[1].rgbTiles->find(TileCoord{0, 0});
      check(l0 != nullptr && l1 != nullptr &&
                std::memcmp(l0->data(), l1->data(), Tile::kTexelCount * sizeof(uint16_t)) == 0,
            "npaint: both layers come back with bit-identical pixels -- the content survives "
            "a save and load unchanged");
      // Stated as a limitation rather than implied away.
      check(l0 != l1 && back.document.layers[0].rgbTiles->tileUseCount(TileCoord{0, 0}) == 1,
            "npaint: but the sharing does NOT survive -- the format stores each part's pixels "
            "in full and the loader allocates per part, so a reopened document costs what its "
            "tiles weigh (there is no np: attribute for a shared tile, and inventing one is "
            "not this step)");
      // Writing into one loaded layer must therefore still be safe.
      back.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0})
          .writePixel(PixelCoord{4, 4}, {1.0f, 1.0f, 1.0f, 1.0f});
      const auto other =
          back.document.layers[1].rgbTiles->find(TileCoord{0, 0})->readPixel(PixelCoord{4, 4});
      check(other[0] != 1.0f,
            "npaint: and writing into one reopened layer leaves the other alone");
    } else {
      check(!sharedSave.ok && contains(sharedSave.error, "OpenImageIO") && !deepSave.ok,
            "npaint: refused by name in the build with no OpenImageIO backend, exactly as "
            "every other section's `.npaint` block is");
    }

    for (const char* p : {kShared, kDeep, kNameA, kNameB}) std::remove(p);
    check(std::fopen(kShared, "rb") == nullptr && std::fopen(kDeep, "rb") == nullptr &&
              std::fopen(kNameA, "rb") == nullptr && std::fopen(kNameB, "rb") == nullptr,
          "cow: every scratch file this section wrote is removed");
  }

  std::printf("[selftest] cow tiles %s\n", ok ? "PASS" : "FAIL");
  return ok;
}


// ==========================================================================
// PLAN.md Phase 5 step 7 -- core/History
// ==========================================================================


}  // namespace np
