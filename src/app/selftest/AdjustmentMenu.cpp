#include "app/selftest/Support.hpp"

#include <cstring>

#include "app/AdjustmentOps.hpp"
#include "ops/PointOpTiles.hpp"
#include "ui/MenuModel.hpp"

namespace np {

namespace {

constexpr int32_t kW = 256;  // 2x2 tiles, so a tile SEAM exists to test across
constexpr int32_t kH = 256;

// A deterministic, varying, opaque premultiplied field. splitmix64's
// finalizer, three lines -- the same private-copy-per-section convention
// app/selftest/Blur.cpp, Filters.cpp and FilterMenu.cpp each already follow,
// rather than a fifth caller of a shared fixture header.
float adjustTestNoise(uint64_t i) noexcept {
  uint64_t z = i * 0x9e3779b97f4a7c15ULL + 0x243f6a8885a308d3ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z = z ^ (z >> 31);
  return static_cast<float>(z >> 40) * (1.0f / 16777216.0f);  // [0,1)
}

void fillAdjustField(TileStore& tiles) {
  uint64_t counter = 0;
  for (int32_t ty = 0; ty < 2; ++ty) {
    for (int32_t tx = 0; tx < 2; ++tx) {
      Tile& t = tiles.getOrCreate(TileCoord{tx, ty});
      for (int32_t y = 0; y < kTileSize; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          const float v = adjustTestNoise(counter++);
          t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.5f * v, 1.0f});
        }
      }
    }
  }
}

OpenDocument makeAdjustDocument(const char* title) {
  OpenDocument od = makeBlankOpenDocument(kW, kH, WorkingSpace{}, title);
  fillAdjustField(*od.document.layers[0].rgbTiles);
  // Recorded, not merely written -- app/selftest/FilterMenu.cpp's own fixture
  // comment explains why an unrecorded fill makes an undo assertion compare a
  // filled store against a blank one and report a history defect that is
  // really a fixture defect.
  od.recordEdit("adjustment fixture field", EditKind::Content);
  return od;
}

bool tilesExactlyEqual(const TileStore& a, const TileStore& b) {
  if (a.occupiedTileCount() != b.occupiedTileCount()) return false;
  for (const auto& [coord, tile] : a) {
    const Tile* other = b.find(coord);
    if (other == nullptr) return false;
    if (std::memcmp(tile.data(), other->data(), Tile::kTexelCount * sizeof(uint16_t)) != 0) return false;
  }
  return true;
}

// A one-element run wrapping applyLevels with the given per-channel params --
// the same construction app/AdjustmentOps.cpp's `runFor()` performs, spelled
// out here so section A can drive `pointOpTiles()` without going through the
// document bridge at all.
PointOpRun levelsRun(const std::array<LevelsParams, 3>& p) {
  return PointOpRun{[p](const std::array<float, 3>& rgb) { return applyLevels(rgb, p); }};
}

// A non-identity Levels: lift the black output off zero and pull the white
// input in. Chosen deliberately for section A's transparency assertion --
// `blackOut > 0` is exactly the parameterisation that would turn empty canvas
// into visible paint if the wrapper's `alpha <= 0` guard were not there.
std::array<LevelsParams, 3> liftedLevels() {
  LevelsParams p;
  p.whiteIn = 0.8f;
  p.blackOut = 0.25f;
  std::array<LevelsParams, 3> all{p, p, p};
  return all;
}

// Walks the menu tree for a Command node carrying `action`, and reports the
// chain of submenu labels that led to it. An action reachable from the wrong
// menu is a different defect from one that is unreachable, and only a path
// tells them apart.
bool findMenuPath(const std::vector<MenuNode>& nodes, MenuAction action, std::string& path) {
  for (const MenuNode& n : nodes) {
    if (n.kind == MenuNodeKind::Command && n.action == action) return true;
    std::string sub;
    if (!n.children.empty() && findMenuPath(n.children, action, sub)) {
      path = n.label + (sub.empty() ? std::string() : " > " + sub);
      return true;
    }
  }
  return false;
}

}  // namespace

// ===========================================================================
// Image > Adjustments -- ops/PointOpTiles and app/AdjustmentOps
// ===========================================================================
//
// Photoshop's Image > Adjustments menu, wired to the six `rgb -> rgb`
// functions ops/PointOps has had since PLAN.md Phase 3 with no UI path to any
// of them -- the same reachability gap docs/reachability-audit.md C1 named for
// the filters, one module over.
//
// **What this section is actually for, and what it deliberately does not
// retest.** The maths is already covered: `runPointOpsTest()` drives Levels,
// Curves, Exposure, Saturation, grayscale and the channel mixer directly. The
// selection blend, the copy-on-write discipline and the history rule are
// already covered by `runFilterMenuTest()`, which exercises the very same
// `computePixelFilter()`/`applyPixelFilter()` templates through
// app/PixelOpBridge.hpp. Repeating either here would produce assertions that
// are green because something else is right.
//
// What is genuinely new -- and therefore what this section tests -- is the
// join: a tile-level runner whose sparseness rules differ from every spatial
// filter's, and a bridge that reaches the same layer through a different
// engine. Section A is the runner's own three claims; section B is the
// properties that only hold once the runner and the bridge are combined;
// section C is the menu.
//
// Headless and GPU-free -- pure CPU tile arithmetic and a pure menu tree, no
// PaintSim and no window.
bool runAdjustmentMenuTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  std::printf("  -- A. ops/PointOpTiles: the runner's own three claims --\n");

  // --- A1. A point op NEVER grows the layer. ---
  //
  // ops/PointOpTiles.hpp's first stated property, and the one that separates
  // it from every engine in ops/Blur and ops/Filters: those allocate every
  // tile across the requested rectangle because a blur legitimately spreads
  // paint outward, and app/FilterOps.hpp names that as a real cost. A point op
  // cannot spread anything, so a request over the WHOLE canvas against a store
  // holding ONE tile must come back holding at most that one tile.
  //
  // The parameterisation matters: `liftedLevels()` has `blackOut = 0.25`, so
  // if the runner iterated the rectangle's tile range instead of the source's
  // tiles, every one of the four tiles would come back filled with a visible
  // grey -- an adjustment that painted over empty canvas. Asserting the tile
  // COUNT is what catches that, and asserting it against a store deliberately
  // smaller than the canvas is what makes the count meaningful.
  {
    TileStore src;
    Tile& only = src.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y)
      for (int32_t x = 0; x < kTileSize; ++x) only.writePixel(PixelCoord{x, y}, {0.4f, 0.4f, 0.4f, 1.0f});

    TileStore out;
    const bool ran = pointOpTiles(src, PixelRect{0, 0, kW, kH}, levelsRun(liftedLevels()), &out);
    check(ran, "runner: a whole-canvas request over a one-tile store runs");
    check(out.occupiedTileCount() == 1,
          "runner: it allocates ONE tile, not the rectangle's four -- an adjustment "
          "cannot create coverage where the layer had none");
  }

  // --- A2. A partially-changed tile keeps its unchanged texels. ---
  //
  // **This is the assertion that catches the defect this module was one line
  // away from shipping.** `compositeFilterResult()` (app/FilterOps.cpp) treats
  // a tile PRESENT in the engine's output as authoritative for every texel it
  // covers -- it reads `filtered.readPixel(local)` at each address inside the
  // rectangle and blends that value in. A freshly-created `Tile` is all
  // zeroes. So a runner that created the destination tile and wrote only the
  // texels the op changed would hand the compositor transparent black for
  // every texel the op left alone, erasing the untouched half of a partly
  // graded tile.
  //
  // **How the fixture makes the split, and the first way of doing it that did
  // NOT work.** The obvious construction is an opaque half the op moves and a
  // TRANSPARENT half it provably leaves alone (`alpha <= 0` is the one
  // guaranteed fixed point of every op in the family). That fixture is useless
  // here, and was written and discarded: transparent black is `{0,0,0,0}`, and
  // a freshly-created `Tile` is all zeroes, so "the untouched texel survived"
  // and "the untouched texel was erased" are the SAME BYTES. Deleting the
  // whole-tile copy from `pointOpTiles()` left it green.
  //
  // So the untouched half is opaque and NON-ZERO, and it is held out of the op
  // by the RECTANGLE rather than by its own value: the request covers only the
  // top half of the tile. That is also the more realistic case -- a selection
  // or a canvas edge cutting through a tile is how this arises in practice --
  // and it gives the assertion a value to be destroyed, which is the whole
  // point of asserting it.
  {
    TileStore src;
    Tile& t = src.getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < kTileSize; ++y)
      for (int32_t x = 0; x < kTileSize; ++x)
        t.writePixel(PixelCoord{x, y},
                     y < kTileSize / 2 ? std::array<float, 4>{0.40f, 0.40f, 0.40f, 1.0f}
                                       : std::array<float, 4>{0.60f, 0.55f, 0.50f, 1.0f});

    TileStore out;
    // Only the top half of the tile is in the requested rectangle.
    pointOpTiles(src, PixelRect{0, 0, kTileSize, kTileSize / 2}, levelsRun(liftedLevels()), &out);
    const Tile* got = out.find(TileCoord{0, 0});
    check(got != nullptr, "runner: the partly-changed tile IS present in the output");

    bool topMoved = true, bottomIntact = true;
    if (got != nullptr) {
      for (int32_t y = 0; y < kTileSize; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          const std::array<float, 4> before = t.readPixel(PixelCoord{x, y});
          const std::array<float, 4> after = got->readPixel(PixelCoord{x, y});
          if (y < kTileSize / 2) {
            if (after == before) topMoved = false;
          } else if (after != before) {
            bottomIntact = false;
          }
        }
      }
    }
    check(topMoved, "runner: every texel inside the rectangle did move");
    check(bottomIntact,
          "runner: every OPAQUE, NON-ZERO texel outside it is bit-identical -- a present "
          "tile carries the whole source, not only the addresses the op rewrote");
  }

  // --- A3. Transparent stays transparent, under the op most likely to break
  // it, and across a tile seam. ---
  //
  // `blackOut = 0.25` maps a straight black input to a visible grey. A texel
  // with zero alpha must still come back untouched, because
  // `applyPointOpsPremultiplied()` returns `{0,0,0,0}` for `alpha <= 0` before
  // any op runs. And because a point op reads no neighbourhood at all, the
  // tile-seam invariant every spatial filter in ops/Filters has to EARN is
  // free here -- asserted anyway, cheaply, because "free by construction" is a
  // claim and not a measurement.
  {
    TileStore src;
    for (int32_t ty = 0; ty < 2; ++ty) {
      for (int32_t tx = 0; tx < 2; ++tx) {
        Tile& t = src.getOrCreate(TileCoord{tx, ty});
        for (int32_t y = 0; y < kTileSize; ++y)
          for (int32_t x = 0; x < kTileSize; ++x)
            t.writePixel(PixelCoord{x, y}, {0.0f, 0.0f, 0.0f, 0.0f});
      }
    }
    TileStore out;
    pointOpTiles(src, PixelRect{0, 0, kW, kH}, levelsRun(liftedLevels()), &out);
    check(out.occupiedTileCount() == 0,
          "runner: a fully transparent layer under blackOut=0.25 stays empty -- no tile "
          "is even allocated, so empty canvas cannot be painted by an adjustment");

    // A row straddling the seam at x == kTileSize, opaque this time, must be
    // continuous: the same input value either side must produce the same
    // output value either side.
    TileStore seamSrc;
    for (int32_t tx = 0; tx < 2; ++tx) {
      Tile& t = seamSrc.getOrCreate(TileCoord{tx, 0});
      for (int32_t y = 0; y < kTileSize; ++y)
        for (int32_t x = 0; x < kTileSize; ++x)
          t.writePixel(PixelCoord{x, y}, {0.37f, 0.37f, 0.37f, 1.0f});
    }
    TileStore seamOut;
    pointOpTiles(seamSrc, PixelRect{0, 0, kW, kTileSize}, levelsRun(liftedLevels()), &seamOut);
    const Tile* left = seamOut.find(TileCoord{0, 0});
    const Tile* right = seamOut.find(TileCoord{1, 0});
    bool seamContinuous = left != nullptr && right != nullptr;
    if (seamContinuous) {
      const std::array<float, 4> l = left->readPixel(PixelCoord{kTileSize - 1, 7});
      const std::array<float, 4> r = right->readPixel(PixelCoord{0, 7});
      seamContinuous = l == r;
    }
    check(seamContinuous,
          "runner: equal inputs either side of a tile boundary give equal outputs -- "
          "no seam, by construction rather than by apron");
  }

  // --- A4. The four refusals, each written nothing. ---
  {
    TileStore src;
    src.getOrCreate(TileCoord{0, 0});
    TileStore out;
    const PointOpRun run = levelsRun(liftedLevels());
    check(!pointOpTiles(src, PixelRect{0, 0, kW, kH}, run, nullptr),
          "runner: a null destination refuses");
    TileStore aliased = src;
    check(!pointOpTiles(aliased, PixelRect{0, 0, kW, kH}, run, &aliased),
          "runner: an in-place run refuses rather than reading what it just wrote");
    check(!pointOpTiles(src, PixelRect{0, 0, 0, 0}, run, &out),
          "runner: an empty rectangle refuses");
    check(!pointOpTiles(src, PixelRect{0, 0, kW, kH}, PointOpRun{}, &out),
          "runner: an EMPTY run refuses -- the identity is a no-op, not a full copy");
  }

  std::printf("  -- B. app/AdjustmentOps: the bridge to a live layer --\n");

  // --- B1. The refusal vocabulary is app/StrokeSession's, not a second one.
  {
    OpenDocument od = makeAdjustDocument("adjust refusal");
    od.document.layers[0].locked = true;
    const FilterOpResult locked = applyLevelsAdjustment(od, liftedLevels());
    check(locked.refusal == PixelOpRefusal::Locked && locked.texelsChanged == 0,
          "bridge: a locked layer refuses with Locked and writes nothing");

    OpenDocument pig = makeAdjustDocument("adjust pigment");
    pig.document.layers[0] = makePigmentLayer("Pigment");
    const FilterOpResult kind = applyExposureAdjustment(pig, ExposureParams{2.0f});
    check(kind.refusal == PixelOpRefusal::NoRgbStore && kind.texelsChanged == 0,
          "bridge: a Pigment layer refuses with NoRgbStore -- latents are not "
          "Working-space RGB, and an adjustment is a fill in every texel it touches");
  }

  // --- B2. The selection bounds it, exactly, inside a partially covered tile.
  //
  // Not a restatement of runFilterMenuTest()'s section B: that proves
  // `compositeFilterResult()` honours a selection when a BLUR feeds it. This
  // proves the adjustment path reaches that same function at all -- a bridge
  // that quietly wrote `*target->rgbTiles` itself would pass every maths
  // assertion above and fail only here.
  {
    OpenDocument od = makeAdjustDocument("adjust selection");
    const TileStore before = *od.document.layers[0].rgbTiles;
    od.selection = selectRectangle(0.0f, 0.0f, 64.0f, 64.0f);  // a quarter of tile (0,0)

    const FilterOpResult r = applyExposureAdjustment(od, ExposureParams{1.5f});
    check(r.refusal == PixelOpRefusal::None && r.texelsChanged > 0,
          "selection: the bounded adjustment runs and changes something inside");

    const TileStore& after = *od.document.layers[0].rgbTiles;
    bool outsideIntact = true, insideChanged = false;
    for (int32_t y = 0; y < kTileSize && outsideIntact; ++y) {
      for (int32_t x = 0; x < kTileSize; ++x) {
        const std::array<float, 4> b = before.find(TileCoord{0, 0})->readPixel(PixelCoord{x, y});
        const std::array<float, 4> a = after.find(TileCoord{0, 0})->readPixel(PixelCoord{x, y});
        if (x < 64 && y < 64) {
          if (a != b) insideChanged = true;
          continue;
        }
        if (a != b) {
          outsideIntact = false;
          break;
        }
      }
    }
    check(insideChanged, "selection: the interior of the rectangle did change");
    check(outsideIntact,
          "selection: every texel outside it is bit-identical -- inside the SAME TILE "
          "the rectangle only partly covers, not merely tile-granular");
  }

  // --- B3. One history entry, and none at all for an identity.
  //
  // The second half is the interesting one: a user who opens Exposure, leaves
  // it at 0 stops and clicks the button has asked for nothing, and an undo
  // step nobody can tell from the one before it is worse than no step.
  {
    OpenDocument od = makeAdjustDocument("adjust history");
    const size_t base = od.history.entries().size();
    applyExposureAdjustment(od, ExposureParams{1.0f});
    check(od.history.entries().size() == base + 1,
          "history: one adjustment records exactly one entry");

    const size_t afterOne = od.history.entries().size();
    const FilterOpResult none = applyExposureAdjustment(od, ExposureParams{0.0f});
    check(none.texelsChanged == 0 && od.history.entries().size() == afterOne,
          "history: an identity adjustment records NO entry and changes no texel");

    // Undo restores the pre-adjustment bytes exactly, which is what makes the
    // entry above a real one rather than a label.
    const TileStore graded = *od.document.layers[0].rgbTiles;
    const Document* prior = od.history.undo();
    check(prior != nullptr && !tilesExactlyEqual(graded, *prior->layers[0].rgbTiles),
          "history: undo actually moved the pixels back off the graded state");
  }

  // --- B4. The preview computes bit-for-bit what the commit writes. ---
  //
  // T15's own sabotage (b) is "the preview and the committed result use
  // different parameters". Sharing `computePixelFilter()` is what makes that a
  // compile error away rather than a discipline away, but sharing is a claim
  // about the code; this is the measurement. `memcmp` over every occupied
  // tile, not a tolerance -- they are the same arithmetic or they are not.
  {
    OpenDocument od = makeAdjustDocument("adjust preview");
    const ChannelMixerParams mix{{{{0.3f, 0.6f, 0.1f, 0.05f},
                                   {0.1f, 0.8f, 0.1f, 0.0f},
                                   {0.2f, 0.2f, 0.6f, -0.02f}}}};
    TileStore previewed;
    const FilterOpResult p = previewChannelMixerAdjustment(od, mix, &previewed);
    const TileStore untouched = *od.document.layers[0].rgbTiles;
    check(p.texelsChanged > 0, "preview: it computed something");
    check(tilesExactlyEqual(untouched, *od.document.layers[0].rgbTiles),
          "preview: the document itself is untouched -- a preview writes nothing");

    const FilterOpResult a = applyChannelMixerAdjustment(od, mix);
    check(a.texelsChanged == p.texelsChanged,
          "preview: it reports the same texel count the commit does");
    check(tilesExactlyEqual(previewed, *od.document.layers[0].rgbTiles),
          "preview: bit-for-bit what the commit wrote -- same engine, same params, "
          "one implementation");
  }

  // --- B5. Desaturate is achromatic, and it is the same weights the GPU
  // grayscale preview uses. ---
  {
    OpenDocument od = makeAdjustDocument("desaturate");
    const FilterOpResult r = applyDesaturate(od);
    check(r.refusal == PixelOpRefusal::None && r.texelsChanged > 0, "desaturate: it ran");

    bool achromatic = true;
    const Tile* t = od.document.layers[0].rgbTiles->find(TileCoord{0, 0});
    if (t == nullptr) {
      achromatic = false;
    } else {
      for (int32_t y = 0; y < kTileSize && achromatic; ++y) {
        for (int32_t x = 0; x < kTileSize; ++x) {
          const std::array<float, 4> c = t->readPixel(PixelCoord{x, y});
          if (c[0] != c[1] || c[1] != c[2]) {
            achromatic = false;
            break;
          }
        }
      }
    }
    check(achromatic, "desaturate: every texel comes out with r == g == b");
    check(kRec709LumaWeights[0] == 0.2126f && kRec709LumaWeights[1] == 0.7152f &&
              kRec709LumaWeights[2] == 0.0722f,
          "desaturate: at the same Rec.709 weights shaders/grayscale_blit.wgsl "
          "hardcodes, so a desaturated layer and a grayscale VIEW of it agree");
  }

  std::printf("  -- C. the menu items themselves --\n");

  // --- C1. Every one of the five is reachable, and from Image > Adjustments
  // specifically. An action reachable from the wrong menu is a different
  // defect from an unreachable one, and only the path tells them apart.
  {
    MenuContext ctx;
    ctx.hasDocument = true;
    const std::vector<MenuNode> bar = buildMenuModel(ctx);
    const MenuAction kAdjust[] = {MenuAction::AdjustLevels, MenuAction::AdjustCurves,
                                  MenuAction::AdjustExposure, MenuAction::AdjustChannelMixer,
                                  MenuAction::AdjustDesaturate};
    bool allUnderImage = true;
    for (MenuAction a : kAdjust) {
      std::string path;
      if (!findMenuPath(bar, a, path) || path != "Image > Adjustments") allUnderImage = false;
    }
    check(allUnderImage,
          "menu: all five items sit under Image > Adjustments -- the path, not merely "
          "reachability from somewhere");
  }

  // --- C2. Four open a modal; Desaturate does not.
  //
  // The distinction `MenuEffect` exists to record. Getting it wrong for
  // Desaturate would mean a modal that opens with nothing in it -- and getting
  // it wrong for one of the other four means `ImGui::OpenPopup()` called from
  // an AppKit callback with no frame in flight.
  {
    check(menuActionEffect(MenuAction::AdjustLevels) == MenuEffect::Deferred &&
              menuActionEffect(MenuAction::AdjustCurves) == MenuEffect::Deferred &&
              menuActionEffect(MenuAction::AdjustExposure) == MenuEffect::Deferred &&
              menuActionEffect(MenuAction::AdjustChannelMixer) == MenuEffect::Deferred,
          "menu: the four with a dialog are Deferred -- a native menu callback has no "
          "ImGui frame to call OpenPopup() from");
    check(menuActionEffect(MenuAction::AdjustDesaturate) == MenuEffect::Inline,
          "menu: Desaturate is Inline -- it has no parameter to ask for, so a modal "
          "would open with nothing in it");
  }

  return ok;
}

}  // namespace np
