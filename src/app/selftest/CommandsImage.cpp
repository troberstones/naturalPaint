#include "app/selftest/Support.hpp"

#include "app/AdjustmentOps.hpp"
#include "app/Action.hpp"
#include "app/Command.hpp"
#include "app/CropTool.hpp"
#include "app/FilterOps.hpp"
#include "app/Recorder.hpp"
#include "app/Replay.hpp"
#include "app/TransformSession.hpp"
#include "core/SelectionMask.hpp"
#include "ops/Blur.hpp"
#include "ops/Filters.hpp"
#include "ops/ToneOps.hpp"
#include "ops/Transform.hpp"

namespace np {
namespace {

// A 64x64 RGB document whose content is deliberately awkward in four ways at
// once, because one fixture has to satisfy thirty commands:
//
//  * **High-frequency, non-monotonic content.** A smooth ramp would make the
//    median filter an identity over its whole interior (the median of a
//    symmetric window on a linear ramp IS the centre sample), so a
//    "texelsChanged > 0" assertion would have been measuring the content, not
//    the filter. The hash pattern below has no such structure.
//  * **A wide histogram**, so the four auto solvers have a distribution to
//    solve from rather than a single spike. A flat fill makes an auto-levels
//    black and white point coincide, and every one of the four then reports a
//    perfectly honest zero.
//  * **Content that does NOT fill the canvas** (texels 8..48 of 0..64), so
//    `trim_to_content` has something to trim to. On a full-bleed fixture it
//    succeeds and changes nothing, which is a green assertion measuring
//    nothing.
//  * **An engaged selection smaller than the canvas** (4..52), so
//    `crop_to_selection` has a region. Every pixel op below is
//    selection-bounded through app/PixelOpBridge, and the content sits well
//    inside the marquee so a filter's apron still has room to spread.
OpenDocument makeImageCommandDocument() {
  OpenDocument od = makeBlankOpenDocument(64, 64, WorkingSpace{}, "image commands");
  od.document.layers[0].name = "Base";
  Tile& t = od.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
  for (int32_t y = 8; y < 48; ++y) {
    for (int32_t x = 8; x < 48; ++x) {
      const float v = 0.05f + 0.9f * (static_cast<float>((x * 7 + y * 13) % 17) / 16.0f);
      // Alpha 1 everywhere in the band, so premultiplied and straight agree
      // and none of the adjustments is silently working on a texel that
      // un-premultiplies to {0,0,0,0} and comes back unchanged.
      t.writePixel(PixelCoord{x, y}, {v, 0.2f + 0.5f * v, 1.0f - v, 1.0f});
    }
  }
  od.selection = selectRectangle(4.0f, 4.0f, 52.0f, 52.0f);
  od.recordEdit("image command fixture", EditKind::Content);
  return od;
}

// Every texel of a layer folded into one number, so two runs can be compared
// without printing 4096 of them. Not a cryptographic hash and does not need to
// be: it is used only to assert that two requests differing in ONE named enum
// value produce different pictures, and a collision there would have to be
// engineered.
double layerSignature(const Document& doc, size_t layerIndex) {
  if (layerIndex >= doc.layers.size() || !doc.layers[layerIndex].rgbTiles) return 0.0;
  const TileStore& tiles = *doc.layers[layerIndex].rgbTiles;
  double acc = 0.0;
  for (int32_t y = 0; y < static_cast<int32_t>(doc.height); ++y) {
    for (int32_t x = 0; x < static_cast<int32_t>(doc.width); ++x) {
      const TileCoord tc = tileCoordAt(PixelCoord{x, y});
      const Tile* tile = tiles.find(tc);
      if (tile == nullptr) continue;
      const std::array<float, 4> px = tile->readPixel(tileLocalOffset(PixelCoord{x, y}));
      // The coordinate is folded in so that a picture and the SAME picture
      // translated do not hash alike -- which is exactly the difference a
      // canvas anchor makes, and the whole reason this function exists.
      const double weight = 1.0 + static_cast<double>(x) * 0.001 + static_cast<double>(y) * 0.007;
      for (size_t c = 0; c < 4; ++c) acc += weight * static_cast<double>(px[c]) * (1.0 + c);
    }
  }
  return acc;
}

// Bit-exact, unlike `layerSignature()` above: for section H's three rows this
// is comparing two DIFFERENT code paths that are each supposed to have done
// the identical thing (a command against its applier, or the command against
// `app/TransformSession`'s own interactive commit) rather than checking that
// one control changed *something*, so a tolerance -- or a weighted sum that a
// pair of compensating errors could still hit -- would let a reroute that ran
// a slightly different op pass. `sameLayerPixels()` in
// app/selftest/CommandCallsites.cpp makes the identical argument; this is a
// second, smaller copy rather than a shared header because both are
// file-local test helpers over a `Document`/`Tile` shape neither module
// exports for the purpose.
bool sameRgbLayerPixels(const Document& a, const Document& b, size_t layerIndex) {
  if (layerIndex >= a.layers.size() || layerIndex >= b.layers.size()) return false;
  const Layer& la = a.layers[layerIndex];
  const Layer& lb = b.layers[layerIndex];
  if (la.rgbTiles.has_value() != lb.rgbTiles.has_value()) return false;
  if (!la.rgbTiles.has_value()) return true;
  if (a.width != b.width || a.height != b.height) return false;
  for (int32_t y = 0; y < static_cast<int32_t>(a.height); ++y) {
    for (int32_t x = 0; x < static_cast<int32_t>(a.width); ++x) {
      const PixelCoord pc{x, y};
      const TileCoord tc = tileCoordAt(pc);
      const Tile* ta = la.rgbTiles->find(tc);
      const Tile* tb = lb.rgbTiles->find(tc);
      if ((ta == nullptr) != (tb == nullptr)) return false;
      if (ta == nullptr) continue;
      if (ta->readPixel(tileLocalOffset(pc)) != tb->readPixel(tileLocalOffset(pc))) return false;
    }
  }
  return true;
}

JsonValue num(double v) { return JsonValue::number(v); }

JsonValue arrayOf(std::initializer_list<double> values) {
  JsonValue a = JsonValue::array();
  for (double v : values) a.push(num(v));
  return a;
}

JsonValue levelsChannel(float blackIn, float whiteIn, float gamma) {
  JsonValue o = JsonValue::object();
  o.set("black_in", num(blackIn));
  o.set("white_in", num(whiteIn));
  o.set("gamma", num(gamma));
  o.set("black_out", num(0.0));
  o.set("white_out", num(1.0));
  return o;
}

JsonValue curvePoint(double x, double y) {
  JsonValue o = JsonValue::object();
  o.set("x", num(x));
  o.set("y", num(y));
  return o;
}

JsonValue gradientStop(double position, std::initializer_list<double> color) {
  JsonValue o = JsonValue::object();
  o.set("position", num(position));
  o.set("color", arrayOf(color));
  o.set("midpoint", num(0.5));
  return o;
}

// One valid, complete request per registered id.
//
// **Every fixture sets EVERY key its row advertises**, and section A asserts
// that in both directions. That is what makes section C's removal loop
// exhaustive: a key the table advertises but no fixture carries would never
// have its absence tested, and a key an adapter reads but the table does not
// advertise is a control the ACTIONS panel cannot offer.
struct ImageCommandFixture {
  const char* id;
  JsonValue params;
  // Whether a successful run must report a non-zero texel count. Every row
  // registered today does; the flag exists so a future row that legitimately
  // changes nothing (app/Command.hpp's `select_layer` is the shape) can be
  // added here without weakening the claim for the rest.
  bool changesTexels = true;
};

std::vector<ImageCommandFixture> imageCommandFixtures() {
  std::vector<ImageCommandFixture> f;
  auto add = [&](const char* id, JsonValue params) {
    f.push_back(ImageCommandFixture{id, std::move(params), true});
  };

  // ---- document geometry ----
  {
    JsonValue p = JsonValue::object();
    p.set("width", num(32));
    p.set("height", num(32));
    p.set("kernel", JsonValue::string("Catmull-Rom"));
    add("image_size", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("width", num(32));
    p.set("height", num(32));
    p.set("anchor", JsonValue::string("center"));
    add("canvas_size", p);
  }
  add("crop_to_selection", JsonValue::object());
  add("trim_to_content", JsonValue::object());

  // ---- the Filter menu's seven ----
  {
    JsonValue p = JsonValue::object();
    p.set("sigma", num(2.0));
    add("filter_gaussian_blur", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("strength", num(1.5));
    add("filter_sharpen", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("amount", num(1.0));
    p.set("radius", num(2.0));
    p.set("blur_kind", JsonValue::string("gaussian"));
    p.set("threshold", num(0.0));
    add("filter_unsharp_mask", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("amount", num(0.25));
    p.set("distribution", JsonValue::string("gaussian"));
    p.set("monochrome", JsonValue::boolean(false));
    p.set("seed", num(7));
    add("filter_add_noise", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("amount", num(1.0));
    p.set("dx", num(1));
    p.set("dy", num(-1));
    p.set("depth", num(1.0));
    add("filter_emboss", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("radius", num(2));
    add("filter_median", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("radius", num(3));
    p.set("angle_radians", num(0.5));
    add("filter_motion_blur", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("radius", num(4));
    add("filter_inpaint", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("sigma", num(8.0));
    add("filter_remove_lighting_gradient", p);
  }
  // `filter_offset` is NOT here -- see section A's comment on why it cannot
  // share this file's one fixture document (a live selection is standing on
  // every other row's behalf, and `filter_offset` refuses under any selection
  // at all). It has its own section further down.

  // ---- Image > Adjustments ----
  {
    JsonValue channels = JsonValue::array();
    channels.push(levelsChannel(0.05f, 0.9f, 0.6f));
    channels.push(levelsChannel(0.0f, 1.0f, 1.4f));
    channels.push(levelsChannel(0.1f, 0.95f, 1.0f));
    JsonValue p = JsonValue::object();
    p.set("channels", channels);
    add("adjust_levels", p);
  }
  {
    JsonValue channels = JsonValue::array();
    for (int c = 0; c < 3; ++c) {
      JsonValue curve = JsonValue::array();
      curve.push(curvePoint(0.0, 0.1));
      curve.push(curvePoint(0.5, 0.4));
      curve.push(curvePoint(1.0, 0.9));
      channels.push(curve);
    }
    JsonValue p = JsonValue::object();
    p.set("channels", channels);
    add("adjust_curves", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("stops", num(1.0));
    add("adjust_exposure", p);
  }
  {
    JsonValue matrix = JsonValue::array();
    matrix.push(arrayOf({0.2, 0.3, 0.5, 0.02}));
    matrix.push(arrayOf({0.5, 0.2, 0.3, 0.0}));
    matrix.push(arrayOf({0.3, 0.5, 0.2, -0.01}));
    JsonValue p = JsonValue::object();
    p.set("matrix", matrix);
    add("adjust_channel_mixer", p);
  }
  add("adjust_desaturate", JsonValue::object());
  {
    JsonValue p = JsonValue::object();
    p.set("gain", num(1.4));
    p.set("offset", num(0.05));
    p.set("gamma", num(0.9));
    add("adjust_brightness_contrast", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("hue_degrees", num(40.0));
    p.set("saturation", num(1.3));
    p.set("lightness", num(0.05));
    p.set("colorize", JsonValue::boolean(false));
    p.set("colorize_hue_degrees", num(210.0));
    p.set("colorize_saturation", num(0.5));
    add("adjust_hue_saturation", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("amount", num(0.6));
    p.set("luma_weights", arrayOf({0.2126, 0.7152, 0.0722}));
    add("adjust_vibrance", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("shadows", arrayOf({0.1, -0.05, 0.0}));
    p.set("midtones", arrayOf({0.0, 0.1, -0.1}));
    p.set("highlights", arrayOf({-0.05, 0.0, 0.1}));
    p.set("preserve_luminosity", JsonValue::boolean(true));
    add("adjust_color_balance", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("reds", num(0.4));
    p.set("yellows", num(0.6));
    p.set("greens", num(0.5));
    p.set("cyans", num(0.4));
    p.set("blues", num(0.2));
    p.set("magentas", num(0.3));
    add("adjust_black_and_white", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("density", num(0.6));
    p.set("color", arrayOf({1.0, 0.5, 0.2}));
    p.set("preserve_luminosity", JsonValue::boolean(true));
    add("adjust_photo_filter", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("levels", num(4));
    add("adjust_posterize", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("threshold", num(0.5));
    p.set("amount", num(1.0));
    add("adjust_threshold", p);
  }
  {
    JsonValue stops = JsonValue::array();
    stops.push(gradientStop(0.0, {0.05, 0.0, 0.2}));
    stops.push(gradientStop(1.0, {1.0, 0.9, 0.6}));
    JsonValue p = JsonValue::object();
    p.set("stops", stops);
    p.set("luma_weights", arrayOf({0.2126, 0.7152, 0.0722}));
    add("adjust_gradient_map", p);
  }
  {
    JsonValue p = JsonValue::object();
    p.set("amount", num(1.0));
    p.set("domain", JsonValue::string("linear"));
    add("adjust_invert", p);
  }
  for (const char* id :
       {"adjust_auto_tone", "adjust_auto_contrast", "adjust_auto_color", "adjust_equalize"}) {
    JsonValue p = JsonValue::object();
    p.set("clip_fraction", num(0.001));
    add(id, p);
  }
  return f;
}

bool hasPrefix(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// A copy of `params` with one key dropped, for section C.
JsonValue withoutKey(const JsonValue& params, const std::string& key) {
  JsonValue out = JsonValue::object();
  for (const auto& [k, v] : params.members())
    if (k != key) out.set(k, v);
  return out;
}

}  // namespace

// app/CommandsImage (docs/automation-plan.md step 1) -- the command rows for
// everything that changes pixels or the document's own geometry.
//
// **What this section is for, and what it deliberately is not.** Every applier
// these rows dispatch to is already asserted by its own section
// (app/selftest/FilterMenu.cpp, AdjustmentMenu.cpp, FiltersExt.cpp,
// CropTool.cpp). Nothing here re-tests a blur, a curve or a crop. What is new
// is the ADAPTER between a JSON object and those appliers, and the specific
// ways an adapter can be wrong while looking right:
//
//  * **A parameter of the wrong type reads as its default.**
//    `JsonValue::numberOr()` answers its fallback for `"sigma": "four"`, so a
//    filter can run with a default parameter and report success. Section D
//    drives exactly that.
//  * **An enum-valued parameter can be parsed and then dropped.** A row that
//    reads `"anchor"` into a local and passes `CanvasAnchor::Center` anyway
//    still round-trips its names perfectly. Section E therefore asserts that
//    two different names produce two different PICTURES, not merely two
//    different parsed values.
//  * **A no-op reported as a success is this feature's designed failure mode**
//    (docs/automation-plan.md §7): in a batch it is thirty files written
//    unmodified. Section F asserts the zero that lets the replayer warn --
//    `trim_to_content` on an already-tight document -- and section C asserts
//    that a step naming none of an adjustment's controls refuses rather than
//    running an identity.
//
// Sections A, B and C are LOOPS over `allCommands()` and over one fixture per
// row, so a row added to app/CommandsImage.cpp later without a test of its own
// is still covered: it fails section A for having no fixture before it can
// silently pass anything else.
//
// Headless, GPU-free and filesystem-free.
bool runCommandsImageTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };
  auto contains = [](const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
  };

  const std::vector<ImageCommandFixture> fixtures = imageCommandFixtures();

  std::printf("  -- A. every registered row is in the table, and has a fixture --\n");
  {
    bool allFound = true;
    bool allIdentical = true;
    for (const ImageCommandFixture& f : fixtures) {
      const CommandSpec* spec = findCommand(f.id);
      if (spec == nullptr) {
        allFound = false;
        std::printf("     no row registered for id \"%s\"\n", f.id);
        continue;
      }
      // `findCommand()` must return the row inside `allCommands()` itself, not
      // a copy: a replayer holds the pointer across an apply.
      bool inTable = false;
      for (const CommandSpec& row : allCommands())
        if (&row == spec) inTable = true;
      if (!inTable) allIdentical = false;
    }
    check(allFound, "table: every fixture's id resolves to a registered row");
    check(allIdentical, "table: findCommand() returns the row inside allCommands()");

    // The reverse direction, and the one that catches a row added later with
    // no test. Fully general for the two prefixed families; the four
    // document-geometry ids carry no prefix (app/CommandsImage.cpp §1 says why
    // and states that they are a closed set of four), so they are named -- a
    // FIFTH unprefixed document command would need a line here, which is the
    // one gap in this loop and is stated rather than hidden.
    //
    // **`filter_offset` is named OUT, deliberately, despite the prefix.**
    // `makeImageCommandDocument()` below carries a live selection so
    // `crop_to_selection` has a region to crop to, and every other row in this
    // section is bounded BY a selection -- it runs fine under one. `filter_offset`
    // is the opposite: `offsetRefusalFor()` refuses OUTRIGHT under any live
    // selection (app/CommandsImage.cpp's `doOffset()` and its row comment
    // explain why), so it cannot share this section's one fixture document
    // with the other thirty-two rows and is exercised on its own instead
    // (this file's own dedicated Offset/Inpaint/DeleteSelection/
    // NumericTransform section, further down).
    bool everyRowCovered = true;
    size_t covered = 0;
    for (const CommandSpec& row : allCommands()) {
      const std::string id = row.id;
      const bool mine = (hasPrefix(id, "filter_") || hasPrefix(id, "adjust_") ||
                         id == "image_size" || id == "canvas_size" ||
                         id == "crop_to_selection" || id == "trim_to_content") &&
                        id != "filter_offset";
      if (!mine) continue;
      ++covered;
      bool found = false;
      for (const ImageCommandFixture& f : fixtures)
        if (id == f.id) found = true;
      if (!found) {
        everyRowCovered = false;
        std::printf("     registered row \"%s\" has no fixture in this section\n", row.id);
      }
    }
    check(everyRowCovered && covered == fixtures.size(),
          "table: every image-side row has a fixture, and no fixture is orphaned");
    std::printf("     %zu image-side rows registered\n", covered);

    // The claim app/Command.hpp makes about `paramNames`: it is the list of
    // keys the applier reads, in the order it writes them. Asserted in both
    // directions, because each failure is its own bug -- a key read but not
    // advertised is a control the ACTIONS panel cannot offer, and a key
    // advertised but never read is a control that silently does nothing.
    bool paramsAgree = true;
    for (const ImageCommandFixture& f : fixtures) {
      const CommandSpec* spec = findCommand(f.id);
      if (spec == nullptr) continue;
      for (const auto& [key, unused] : f.params.members()) {
        (void)unused;
        if (std::find(spec->paramNames.begin(), spec->paramNames.end(), key) ==
            spec->paramNames.end()) {
          paramsAgree = false;
          std::printf("     \"%s\" sends \"%s\", which its row does not advertise\n", f.id,
                      key.c_str());
        }
      }
      for (const std::string& advertised : spec->paramNames) {
        if (f.params.find(advertised) == nullptr) {
          paramsAgree = false;
          std::printf("     \"%s\" advertises \"%s\", which no fixture exercises\n", f.id,
                      advertised.c_str());
        }
      }
    }
    check(paramsAgree, "table: each row's paramNames is exactly the keys its fixture sends");
  }

  std::printf("  -- B. every row runs, and reports what it moved --\n");
  {
    bool allRan = true;
    bool allCounted = true;
    bool allSpoke = true;
    for (const ImageCommandFixture& f : fixtures) {
      // A FRESH document per row, deliberately: four of these change the
      // document's extent, so a shared fixture would make every later row's
      // result depend on the order this list happens to be in.
      OpenDocument od = makeImageCommandDocument();
      const CommandResult r = applyCommand(od, Command{f.id, f.params});
      if (!r.ok) {
        allRan = false;
        std::printf("     \"%s\" refused: %s\n", f.id, r.status.c_str());
        continue;
      }
      if (f.changesTexels && !(r.changesPixels && r.texelsChanged > 0)) {
        allCounted = false;
        std::printf("     \"%s\" succeeded but reported %zu texels changed\n", f.id,
                    r.texelsChanged);
      }
      if (r.status.empty()) allSpoke = false;
    }
    check(allRan, "apply: every row succeeds on a filled RGB fixture");
    check(allCounted, "apply: every row that changes pixels reports a non-zero texel count");
    check(allSpoke, "apply: no row succeeds with an empty status sentence");
  }

  std::printf("  -- C. a missing parameter refuses, and the sentence names it --\n");
  {
    // Two loops, and they catch different bugs.
    //
    // The first drops ONE key at a time from a complete request. A key whose
    // absence is a refusal must have the refusal name it -- "refused: not
    // enough parameters" is useless in a batch report of forty files. A key
    // whose absence is fine is an optional one and simply succeeds; that is
    // asserted too, because a required flag added by accident to an optional
    // key would otherwise pass unnoticed.
    bool namesTheKey = true;
    for (const ImageCommandFixture& f : fixtures) {
      for (const auto& [key, unused] : f.params.members()) {
        (void)unused;
        OpenDocument od = makeImageCommandDocument();
        const CommandResult r = applyCommand(od, Command{f.id, withoutKey(f.params, key)});
        if (r.ok) continue;  // optional key
        if (!contains(r.status, key)) {
          namesTheKey = false;
          std::printf("     \"%s\" without \"%s\" refused without naming it: %s\n", f.id,
                      key.c_str(), r.status.c_str());
        }
        if (r.texelsChanged != 0) {
          namesTheKey = false;
          std::printf("     \"%s\" refused but still reported %zu texels\n", f.id,
                      r.texelsChanged);
        }
      }
    }
    check(namesTheKey, "params: dropping any one key either succeeds or refuses naming that key");

    // The second drives every row with NO parameters at all. A row that
    // refuses must name one of the keys that would have made it work; a row
    // that succeeds must be one whose applier documents a
    // default-constructed params struct as doing something (Invert, Threshold,
    // Desaturate, the four solvers and the two crops), never one whose
    // defaults are the identity.
    bool emptyHandled = true;
    for (const ImageCommandFixture& f : fixtures) {
      const CommandSpec* spec = findCommand(f.id);
      if (spec == nullptr) continue;
      OpenDocument od = makeImageCommandDocument();
      const CommandResult r = applyCommand(od, Command{f.id, JsonValue::object()});
      if (r.ok) {
        // Succeeding on an empty request is only defensible if it actually
        // did something. This is the assertion that would catch an adapter
        // whose "required" checks were all deleted: the row would run its
        // params struct's defaults and report a clean, useless success.
        if (!(r.texelsChanged > 0)) {
          emptyHandled = false;
          std::printf("     \"%s\" accepted an empty request and changed nothing\n", f.id);
        }
        continue;
      }
      bool named = false;
      for (const std::string& key : spec->paramNames)
        if (contains(r.status, key)) named = true;
      if (!named) {
        emptyHandled = false;
        std::printf("     \"%s\" refused an empty request without naming a parameter: %s\n", f.id,
                    r.status.c_str());
      }
    }
    check(emptyHandled,
          "params: an empty request either refuses by name or genuinely changes pixels");
  }

  std::printf("  -- D. a parameter of the wrong type is refused, not defaulted --\n");
  {
    // `JsonValue::numberOr()` answers its fallback for a string, so a reader
    // built on it would run `filter_gaussian_blur` with sigma 0 -- an identity
    // -- and report success. This is the single cheapest way for an action
    // file to be silently wrong, so it is asserted per JSON type rather than
    // once.
    OpenDocument od = makeImageCommandDocument();

    // **This assertion is phrased against the SENTENCE, not merely against the
    // refusal, and a sabotage is why.** Written first as `!ok && names the
    // key`, it stayed green with the type check deleted outright: with no type
    // check, `"sigma": "four"` leaves sigma at 0, and the range gate then
    // refuses -- naming "sigma" -- for an entirely different reason. The
    // assertion was measuring `requireAbove()` while claiming to measure the
    // type check. Requiring the sentence to say what was wrong with the VALUE
    // separates the two, because the range refusal does not contain that
    // phrase.
    JsonValue stringSigma = JsonValue::object();
    stringSigma.set("sigma", JsonValue::string("four"));
    const CommandResult r1 = applyCommand(od, Command{"filter_gaussian_blur", stringSigma});
    check(!r1.ok && contains(r1.status, "sigma") && contains(r1.status, "must be a number"),
          "types: a sigma written as a string refuses AS A TYPE ERROR, naming the key");

    // The case nothing else in this suite can catch: an OPTIONAL key of the
    // wrong type. `depth` defaults to a perfectly valid 1.0, so a reader that
    // let a wrong type fall through to the default would run a real emboss and
    // report a clean success -- no range gate downstream would ever fire.
    JsonValue stringDepth = JsonValue::object();
    stringDepth.set("amount", num(1.0));
    stringDepth.set("depth", JsonValue::string("deep"));
    const CommandResult r1b = applyCommand(od, Command{"filter_emboss", stringDepth});
    check(!r1b.ok && contains(r1b.status, "depth"),
          "types: an OPTIONAL key of the wrong type refuses rather than falling back");

    JsonValue boolAmount = JsonValue::object();
    boolAmount.set("amount", JsonValue::boolean(true));
    boolAmount.set("radius", num(2.0));
    const CommandResult r2 = applyCommand(od, Command{"filter_unsharp_mask", boolAmount});
    check(!r2.ok && contains(r2.status, "amount"),
          "types: an amount written as true refuses, naming the key");

    JsonValue fractionalRadius = JsonValue::object();
    fractionalRadius.set("radius", num(2.5));
    const CommandResult r3 = applyCommand(od, Command{"filter_median", fractionalRadius});
    check(!r3.ok && contains(r3.status, "radius"),
          "types: a fractional texel radius refuses rather than being floored");

    // The seed is the one value JSON cannot carry losslessly: a double is
    // exact to 2^53 and ops/Filters' seed is a uint64. A rounded seed is a
    // different grain that still reports success.
    JsonValue hugeSeed = JsonValue::object();
    hugeSeed.set("amount", num(0.25));
    hugeSeed.set("seed", num(1.8446744073709552e19));
    const CommandResult r4 = applyCommand(od, Command{"filter_add_noise", hugeSeed});
    check(!r4.ok && contains(r4.status, "seed"),
          "types: a seed past 2^53 refuses rather than rounding to a different grain");

    JsonValue shortColor = JsonValue::object();
    shortColor.set("density", num(0.5));
    shortColor.set("color", arrayOf({1.0, 0.5}));
    const CommandResult r5 = applyCommand(od, Command{"adjust_photo_filter", shortColor});
    check(!r5.ok && contains(r5.status, "color"),
          "types: a two-element colour refuses rather than leaving blue defaulted");

    JsonValue twoChannels = JsonValue::object();
    JsonValue channels = JsonValue::array();
    channels.push(levelsChannel(0.0f, 1.0f, 0.5f));
    channels.push(levelsChannel(0.0f, 1.0f, 0.5f));
    twoChannels.set("channels", channels);
    const CommandResult r6 = applyCommand(od, Command{"adjust_levels", twoChannels});
    check(!r6.ok && contains(r6.status, "channels"),
          "types: a two-entry channels array refuses, naming the key");

    // The identity-magnitude refusal §2 of app/CommandsImage.cpp argues for.
    JsonValue zeroStrength = JsonValue::object();
    zeroStrength.set("strength", num(0.0));
    const CommandResult r7 = applyCommand(od, Command{"filter_sharpen", zeroStrength});
    check(!r7.ok && contains(r7.status, "strength"),
          "range: a strength of zero refuses rather than running the identity");

    // Out of order, and NOT silently sorted: ops/Gradient makes ascending
    // order the caller's contract.
    JsonValue unsorted = JsonValue::object();
    JsonValue stops = JsonValue::array();
    stops.push(gradientStop(1.0, {1.0, 1.0, 1.0}));
    stops.push(gradientStop(0.0, {0.0, 0.0, 0.0}));
    unsorted.set("stops", stops);
    const CommandResult r8 = applyCommand(od, Command{"adjust_gradient_map", unsorted});
    check(!r8.ok && contains(r8.status, "stops"),
          "range: gradient stops out of ascending order refuse, naming the key");
  }

  std::printf("  -- E. every enum parameter round-trips AND reaches the applier --\n");
  {
    // The round trip first: name -> value -> name, over the enum's own values,
    // plus a name nothing knows.
    bool blurKinds = true;
    for (const BlurKind k : {BlurKind::Gaussian, BlurKind::Box})
      if (blurKindFromName(blurKindName(k)) != k) blurKinds = false;
    check(blurKinds && !blurKindFromName("triangle").has_value(),
          "enums: every blur kind name round-trips; an unknown one is nothing");

    bool distributions = true;
    for (const NoiseDistribution d : {NoiseDistribution::Uniform, NoiseDistribution::Gaussian})
      if (noiseDistributionFromName(noiseDistributionName(d)) != d) distributions = false;
    check(distributions && !noiseDistributionFromName("poisson").has_value(),
          "enums: every noise distribution name round-trips; an unknown one is nothing");

    bool anchors = true;
    for (const CanvasAnchor a : allCanvasAnchors())
      if (canvasAnchorFromName(canvasAnchorName(a)) != a) anchors = false;
    check(anchors && allCanvasAnchors().size() == 9 &&
              !canvasAnchorFromName("middle").has_value(),
          "enums: all nine canvas anchor names round-trip; an unknown one is nothing");

    bool domains = true;
    for (const InvertParams::Domain d :
         {InvertParams::Domain::Linear, InvertParams::Domain::Display})
      if (invertDomainFromName(invertDomainName(d)) != d) domains = false;
    check(domains && !invertDomainFromName("srgb").has_value(),
          "enums: both invert domain names round-trip; an unknown one is nothing");

    // **And now the half a round trip cannot see.** An adapter that parses the
    // name into a local and then passes the struct's default anyway passes
    // every assertion above. So: two requests differing in nothing but the
    // name must produce two different pictures.
    auto runAndSign = [&](const char* id, const JsonValue& params, double* out) {
      OpenDocument od = makeImageCommandDocument();
      const CommandResult r = applyCommand(od, Command{id, params});
      *out = layerSignature(od.document, 0);
      return r.ok;
    };

    JsonValue topLeft = JsonValue::object();
    topLeft.set("width", num(32));
    topLeft.set("height", num(32));
    topLeft.set("anchor", JsonValue::string("top_left"));
    JsonValue bottomRight = topLeft;
    bottomRight.set("anchor", JsonValue::string("bottom_right"));
    double sigTopLeft = 0.0;
    double sigBottomRight = 0.0;
    const bool anchorsRan = runAndSign("canvas_size", topLeft, &sigTopLeft) &&
                            runAndSign("canvas_size", bottomRight, &sigBottomRight);
    check(anchorsRan && sigTopLeft != sigBottomRight,
          "enums: the canvas anchor reaches the applier -- two names, two pictures");

    JsonValue linear = JsonValue::object();
    linear.set("domain", JsonValue::string("linear"));
    JsonValue display = JsonValue::object();
    display.set("domain", JsonValue::string("display"));
    double sigLinear = 0.0;
    double sigDisplay = 0.0;
    const bool domainsRan = runAndSign("adjust_invert", linear, &sigLinear) &&
                            runAndSign("adjust_invert", display, &sigDisplay);
    check(domainsRan && sigLinear != sigDisplay,
          "enums: the invert domain reaches the applier -- two names, two pictures");

    JsonValue uniform = JsonValue::object();
    uniform.set("amount", num(0.3));
    uniform.set("seed", num(11));
    uniform.set("distribution", JsonValue::string("uniform"));
    JsonValue gaussian = uniform;
    gaussian.set("distribution", JsonValue::string("gaussian"));
    double sigUniform = 0.0;
    double sigGaussian = 0.0;
    const bool noiseRan = runAndSign("filter_add_noise", uniform, &sigUniform) &&
                          runAndSign("filter_add_noise", gaussian, &sigGaussian);
    // Same seed, same coordinates: the ONLY difference is the distribution, so
    // an ignored name would give two identical pictures.
    check(noiseRan && sigUniform != sigGaussian,
          "enums: the noise distribution reaches the applier -- same seed, two pictures");

    JsonValue boxBlur = JsonValue::object();
    boxBlur.set("amount", num(1.0));
    boxBlur.set("radius", num(3));
    boxBlur.set("blur_kind", JsonValue::string("box"));
    JsonValue gaussianBlur = boxBlur;
    gaussianBlur.set("blur_kind", JsonValue::string("gaussian"));
    double sigBox = 0.0;
    double sigGauss = 0.0;
    const bool kindsRan = runAndSign("filter_unsharp_mask", boxBlur, &sigBox) &&
                          runAndSign("filter_unsharp_mask", gaussianBlur, &sigGauss);
    check(kindsRan && sigBox != sigGauss,
          "enums: the unsharp blur kind reaches the applier -- two names, two pictures");

    // A name nothing knows is a refusal that quotes the name, not a silent
    // fallback to the default -- the difference between a batch that stops and
    // forty files anchored somewhere the file did not ask for.
    JsonValue badAnchor = topLeft;
    badAnchor.set("anchor", JsonValue::string("middle"));
    OpenDocument od = makeImageCommandDocument();
    const CommandResult r = applyCommand(od, Command{"canvas_size", badAnchor});
    check(!r.ok && contains(r.status, "middle"),
          "enums: an anchor name nothing knows refuses, quoting the name");
  }

  std::printf("  -- F. a no-op the user asked for is reported as one --\n");
  {
    // docs/automation-plan.md §7's designed failure mode. `applyCropRegion()`
    // records nothing when the region is already the whole canvas, and
    // `fromDocumentTransform()` is the only helper that can see that, because
    // `DocumentTransformResult` carries the previous extent.
    //
    // The fixture is built full-bleed on purpose, so trim has nothing to trim.
    OpenDocument tight = makeBlankOpenDocument(32, 32, WorkingSpace{}, "already tight");
    Tile& t = tight.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
    for (int32_t y = 0; y < 32; ++y)
      for (int32_t x = 0; x < 32; ++x) t.writePixel(PixelCoord{x, y}, {0.4f, 0.3f, 0.2f, 1.0f});
    tight.recordEdit("full bleed", EditKind::Content);
    const uint32_t widthBefore = tight.document.width;
    const CommandResult noop =
        applyCommand(tight, Command{"trim_to_content", JsonValue::object()});
    check(noop.ok && noop.texelsChanged == 0 && tight.document.width == widthBefore,
          "no-op: a trim with nothing to trim succeeds and reports ZERO texels changed");

    // And the other side of the same helper, so the zero above is not simply
    // "this helper always says zero".
    OpenDocument loose = makeImageCommandDocument();
    const CommandResult real = applyCommand(loose, Command{"trim_to_content", JsonValue::object()});
    check(real.ok && real.texelsChanged > 0 && loose.document.width < 64,
          "no-op: a trim that really shrinks the document reports a change");

    // A crop with no selection at all is a refusal naming the selection, not a
    // zero-texel success -- the two outcomes must not be confused, because one
    // is a batch that stops and the other is a batch that continues.
    OpenDocument unselected = makeImageCommandDocument();
    unselected.selection.reset();
    const CommandResult noSelection =
        applyCommand(unselected, Command{"crop_to_selection", JsonValue::object()});
    check(!noSelection.ok && contains(noSelection.status, "selection"),
          "no-op: crop_to_selection with no selection refuses, naming the selection");
  }

  std::printf("  -- G. the precondition still answers for every image row --\n");
  {
    // app/selftest/Command.cpp already proves the hook mechanism on one row.
    // What is new here is that all thirty rows are wired to a hook that gives
    // the right answer -- a row registered with `documentAlwaysAvailable` by
    // copy-paste would let a batch drive a filter at a document with no layer.
    OpenDocument empty = makeImageCommandDocument();
    empty.document.layers.clear();
    bool allRefuse = true;
    for (const ImageCommandFixture& f : fixtures) {
      const CommandSpec* spec = findCommand(f.id);
      if (spec == nullptr) continue;
      if (spec->unavailableReason(empty, f.params).empty()) {
        allRefuse = false;
        std::printf("     \"%s\" claims to be available on a document with no layers\n", f.id);
      }
      const CommandResult r = applyCommand(empty, Command{f.id, f.params});
      if (r.ok || r.status.empty()) {
        allRefuse = false;
        std::printf("     \"%s\" ran on a document with no layers\n", f.id);
      }
    }
    check(allRefuse, "precondition: no image row is available on a document with no layers");

    // And the layer-kind half: a Pigment layer holds Latents, not Working-space
    // RGBA, so every pixel op here refuses it by app/FilterOps.hpp's stated
    // structural limit. Asserted over the loop rather than on one row, because
    // the refusal comes from the shared hook and a row that forgot it would be
    // the only one that let a filter at a pigment layer.
    OpenDocument pigment = makeImageCommandDocument();
    const LayerEditResult added =
        applyLayerCommand(pigment, LayerCommand::NewPigmentLayer, pigment.activeLayer);
    bool pigmentHandled = added.ok;
    if (added.ok) {
      setActiveLayer(pigment, added.selected);
      for (const ImageCommandFixture& f : fixtures) {
        const CommandSpec* spec = findCommand(f.id);
        if (spec == nullptr) continue;
        // The four document-geometry rows are NOT pixel ops and must still be
        // available: ops/DocumentTransform moves every layer including ones
        // with no RGB store, and that header's §5 argues why refusing there
        // would be wrong. So this loop asserts a split, not a blanket refusal.
        const std::string id = f.id;
        const bool isPixelOp = hasPrefix(id, "filter_") || hasPrefix(id, "adjust_");
        const bool unavailable = !spec->unavailableReason(pigment, f.params).empty();
        if (isPixelOp != unavailable) {
          pigmentHandled = false;
          std::printf("     \"%s\" is %s on a pigment layer and should not be\n", f.id,
                      unavailable ? "unavailable" : "available");
        }
      }
    }
    check(pigmentHandled,
          "precondition: pixel ops refuse a pigment layer; document ops still run");
  }

  // ==========================================================================
  // H. Offset, Delete Selection and Numeric Transform -- the three section A
  //    named out of its shared-fixture loop, each for its own reason.
  // ==========================================================================
  std::printf("  -- H. Offset, Delete Selection, Numeric Transform --\n");
  {
    std::printf("     -- offset --\n");
    // A document with no selection (offset refuses under any live one), sized
    // so `offsetByHalf()` lands on an EXACT half in both axes -- the case the
    // by-half button exists for. A pattern that is not flat, so an offset
    // that landed on the wrong texels would show up as different pixels
    // rather than the same colour moved.
    OpenDocument origin = makeBlankOpenDocument(64, 64, WorkingSpace{}, "offset origin");
    {
      Tile& t = origin.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
      for (int32_t y = 0; y < kTileSize; ++y)
        for (int32_t x = 0; x < kTileSize; ++x) {
          const float v = static_cast<float>((x * 3 + y * 5) % 11) / 10.0f;
          t.writePixel(PixelCoord{x, y}, {v, 1.0f - v, 0.25f, 1.0f});
        }
      origin.recordEdit("offset fixture", EditKind::Content);
    }
    const PixelCoord half = offsetByHalf(origin);
    check(half.x == 32 && half.y == 32,
          "offset: fixture's by-half is an exact half -- the case this section needs");

    JsonValue offsetParams = JsonValue::object();
    offsetParams.set("dx_fraction", num(0.5));
    offsetParams.set("dy_fraction", num(0.5));
    offsetParams.set("edge", JsonValue::string("wrap"));

    // (b) command vs applier, at the resolution it was "recorded" at: the
    // fraction this row carries and the texel delta the old dialog computed
    // must land on the identical picture.
    {
      OpenDocument viaCommand = origin;
      OpenDocument viaApplier = origin;
      const CommandResult r = applyCommand(viaCommand, Command{"filter_offset", offsetParams});
      const FilterOpResult a = applyOffset(viaApplier, OffsetRequest{32, 32, OffsetEdge::Wrap});
      check(r.ok && a.refusal == PixelOpRefusal::None && r.texelsChanged > 0,
            "offset: both the command and the applier ran and moved texels");
      check(sameRgbLayerPixels(viaCommand.document, viaApplier.document, 0),
            "offset: filter_offset(0.5, 0.5) and applyOffset(32, 32) agree bit-for-bit");
    }

    // (c) record -> replay at a DIFFERENT resolution: by-half stays by-half.
    // The stored step is the SAME JSON (0.5, 0.5, wrap) recorded against the
    // 64x64 fixture above; replayed against a 96x160 document it must land on
    // THAT document's own half, not on 32x32 texels of a 96x160 canvas.
    {
      OpenDocument resized = makeBlankOpenDocument(96, 160, WorkingSpace{}, "offset resized");
      for (int32_t ty = 0; ty < 3; ++ty) {
        for (int32_t tx = 0; tx < 2; ++tx) {
          Tile& t = resized.document.layers[0].rgbTiles->getOrCreate(TileCoord{tx, ty});
          for (int32_t y = 0; y < kTileSize; ++y)
            for (int32_t x = 0; x < kTileSize; ++x) {
              const float v = static_cast<float>((x * 7 + y * 3 + tx * 13 + ty * 17) % 13) / 12.0f;
              t.writePixel(PixelCoord{x, y}, {v, 0.4f, 1.0f - v, 1.0f});
            }
        }
      }
      resized.recordEdit("offset resized fixture", EditKind::Content);
      const PixelCoord halfAtNewSize = offsetByHalf(resized);
      check(halfAtNewSize.x != 32 || halfAtNewSize.y != 32,
            "offset: the resized fixture's own half is NOT the 64x64 fixture's half -- a "
            "literal texel replay would visibly disagree with it");

      OpenDocument replayed = resized;
      Action a;
      a.name = "Offset by half";
      a.steps.push_back(Command{"filter_offset", offsetParams});
      const ReplayResult rr = replayAction(replayed, a);
      check(rr.ok, "offset: replay of the recorded fraction succeeds on the resized document");

      OpenDocument direct = resized;
      applyOffset(direct, OffsetRequest{halfAtNewSize.x, halfAtNewSize.y, OffsetEdge::Wrap});
      check(sameRgbLayerPixels(replayed.document, direct.document, 0),
            "offset: replaying the recorded fraction at a new resolution reproduces THAT "
            "document's own offsetByHalf(), not the original texel count");
    }

    // Refusals: outright under any live selection, and the (0,0) identity.
    {
      OpenDocument withSelection = makeImageCommandDocument();
      const CommandResult r =
          applyCommand(withSelection, Command{"filter_offset", offsetParams});
      check(!r.ok && contains(r.status, "selection"),
            "offset: refuses outright under a live selection, unlike every bounded row above");
    }
    {
      OpenDocument flat = origin;
      JsonValue zero = JsonValue::object();
      zero.set("dx_fraction", num(0.0));
      zero.set("dy_fraction", num(0.0));
      zero.set("edge", JsonValue::string("wrap"));
      const CommandResult r = applyCommand(flat, Command{"filter_offset", zero});
      check(!r.ok && contains(r.status, "identity"),
            "offset: a fraction that resolves to (0, 0) texels is refused as the identity");
    }

    std::printf("     -- delete_selection --\n");
    // (b) command vs the direct clear, both bounded and unbounded.
    {
      OpenDocument viaCommand = makeImageCommandDocument();  // carries a selection
      OpenDocument viaApplier = makeImageCommandDocument();
      const CommandResult r = applyCommand(viaCommand, Command{"delete_selection", JsonValue::object()});
      const Selection* sel = viaApplier.selection ? &*viaApplier.selection : nullptr;
      clearThroughSelection(*viaApplier.document.layers[viaApplier.activeLayer].rgbTiles, sel);
      check(r.ok && r.texelsChanged > 0,
            "delete_selection: the command ran and cleared something under the fixture's "
            "selection");
      check(sameRgbLayerPixels(viaCommand.document, viaApplier.document, 0),
            "delete_selection: command and direct clearThroughSelection() agree, bounded");
    }
    {
      OpenDocument viaCommand = makeImageCommandDocument();
      viaCommand.selection.reset();
      OpenDocument viaApplier = makeImageCommandDocument();
      viaApplier.selection.reset();
      applyCommand(viaCommand, Command{"delete_selection", JsonValue::object()});
      clearThroughSelection(*viaApplier.document.layers[viaApplier.activeLayer].rgbTiles, nullptr);
      check(sameRgbLayerPixels(viaCommand.document, viaApplier.document, 0),
            "delete_selection: an absent selection clears the WHOLE layer, matching the "
            "applier's own documented default");
    }
    // Accepts a Pigment layer, unlike the shared pixel-op bridge.
    {
      OpenDocument pig = makeImageCommandDocument();
      const LayerEditResult added =
          applyLayerCommand(pig, LayerCommand::NewPigmentLayer, pig.activeLayer);
      check(added.ok, "delete_selection: fixture can add a Pigment layer");
      if (added.ok) {
        setActiveLayer(pig, added.selected);
        const CommandResult r = applyCommand(pig, Command{"delete_selection", JsonValue::object()});
        check(r.ok,
              "delete_selection: available on a Pigment layer, unlike the shared pixel-op "
              "bridge that would refuse it");
      }
    }
    // A locked layer is refused, by name.
    {
      OpenDocument locked = makeImageCommandDocument();
      locked.document.layers[locked.activeLayer].locked = true;
      const CommandResult r = applyCommand(locked, Command{"delete_selection", JsonValue::object()});
      check(!r.ok && contains(r.status, "locked"), "delete_selection: refuses a locked layer, by name");
    }
    // selectionBounded's channel-match protection (app/Recorder.hpp §4):
    // exactly the same rule app/selftest/Recorder.cpp section E proves on
    // filter_gaussian_blur, exercised here on the two rows this track added
    // that reuse it for a reason other than "absent means whole canvas".
    {
      Recorder& session = sessionRecorder();
      OpenDocument marquee = makeImageCommandDocument();  // a live, unsaved selection
      session.arm(marquee);
      const CommandResult r = applyCommand(marquee, Command{"delete_selection", JsonValue::object()});
      const bool refusedAsStep = session.steps().empty() && session.refusals().size() == 1;
      const std::string why = session.refusals().empty() ? std::string() : session.refusals()[0];
      session.stop();
      check(r.ok, "delete_selection: the command itself still ran under the live marquee");
      check(refusedAsStep && contains(why, "delete_selection") && contains(why, "channel"),
            "delete_selection: but the RECORDER refuses it as a step, naming the fix");
    }
    {
      Recorder& session = sessionRecorder();
      OpenDocument marquee = makeImageCommandDocument();
      session.arm(marquee);
      // radius is inpaintCommand's own required key; large enough that the
      // small live selection is comfortably inside its reach.
      JsonValue p = JsonValue::object();
      p.set("radius", num(6));
      const CommandResult r = applyCommand(marquee, Command{"filter_inpaint", p});
      const bool refusedAsStep = session.steps().empty() && session.refusals().size() == 1;
      const std::string why = session.refusals().empty() ? std::string() : session.refusals()[0];
      session.stop();
      check(r.ok, "filter_inpaint: the command itself still ran under the live marquee");
      check(refusedAsStep && contains(why, "filter_inpaint") && contains(why, "channel"),
            "filter_inpaint: but the RECORDER refuses it as a step too, the same rule");
    }
    // Inpaint's own point: an ABSENT selection is a hard refusal, never "the
    // whole canvas" -- `inpaintRefusal()`'s `NoSelection`, not a silent
    // whole-layer fill nobody asked for.
    {
      OpenDocument noSel = makeBlankOpenDocument(64, 64, WorkingSpace{}, "inpaint no selection");
      Tile& t = noSel.document.layers[0].rgbTiles->getOrCreate(TileCoord{0, 0});
      for (int32_t y = 0; y < kTileSize; ++y)
        for (int32_t x = 0; x < kTileSize; ++x) t.writePixel(PixelCoord{x, y}, {0.5f, 0.5f, 0.5f, 1.0f});
      noSel.recordEdit("inpaint no-selection fixture", EditKind::Content);
      JsonValue p = JsonValue::object();
      p.set("radius", num(6));
      const CommandResult r = applyCommand(noSel, Command{"filter_inpaint", p});
      check(!r.ok && contains(r.status, "selection"),
            "filter_inpaint: an absent selection is refused, not read as 'the whole canvas'");
    }

    std::printf("     -- numeric_transform --\n");
    // (b) command vs the interactive path: `app/TransformSession` built and
    // committed exactly as `drawNumericTransformDialog()`'s Apply button
    // does, compared against `numeric_transform` given the same five fields.
    // Neither side recomputes the other's pivot -- each asks its OWN code for
    // it (`TransformSession::sourceBounds()` for the session,
    // `layerContentBounds()` inside `doNumericTransform()` for the command),
    // so agreement here is the two independent implementations landing on the
    // same answer, not one checked against a copy of itself.
    {
      OpenDocument viaSession = makeImageCommandDocument();
      viaSession.selection.reset();
      OpenDocument viaCommand = makeImageCommandDocument();
      viaCommand.selection.reset();

      TransformSession ts;
      const TransformBeginResult began = ts.beginLayer(viaSession, viaSession.activeLayer);
      check(began.ok, "numeric_transform: the session begins on the same fixture");
      const DocumentRegion& b = ts.sourceBounds();
      const Point2 pivot{static_cast<float>(b.x) + static_cast<float>(b.width) * 0.5f,
                         static_cast<float>(b.y) + static_cast<float>(b.height) * 0.5f};
      const Mat3 m = composeNumericTransform(15.0f, 1.2f, 0.85f, 6.0f, -4.0f, pivot);
      ts.setPending(m);
      const TransformCommitResult committed = ts.commit(viaSession);
      check(committed.ok, "numeric_transform: the interactive session commits");

      JsonValue p = JsonValue::object();
      p.set("rotate_degrees", num(15.0));
      p.set("scale_x_percent", num(120.0));
      p.set("scale_y_percent", num(85.0));
      p.set("translate_x", num(6.0));
      p.set("translate_y", num(-4.0));
      const CommandResult r = applyCommand(viaCommand, Command{"numeric_transform", p});
      check(r.ok, "numeric_transform: the command runs the same request");
      check(sameRgbLayerPixels(viaSession.document, viaCommand.document, viaSession.activeLayer),
            "numeric_transform: the interactive session and the command agree bit-for-bit");
    }
    // An identity request is a no-op, matching TransformSession::commit()'s
    // own rule exactly -- not a refusal, and not read as "nothing was asked".
    {
      OpenDocument idOp = makeImageCommandDocument();
      idOp.selection.reset();
      JsonValue p = JsonValue::object();
      p.set("rotate_degrees", num(0.0));
      p.set("scale_x_percent", num(100.0));
      p.set("scale_y_percent", num(100.0));
      p.set("translate_x", num(0.0));
      p.set("translate_y", num(0.0));
      const CommandResult r = applyCommand(idOp, Command{"numeric_transform", p});
      check(r.ok && contains(r.status, "identity"),
            "numeric_transform: the identity request succeeds and says so, matching the "
            "interactive session's own rule");
    }
    // Refused by name: a live selection (the case left to the interactive
    // dialog and Free Transform), a locked layer.
    {
      OpenDocument withSelection = makeImageCommandDocument();  // carries a selection
      JsonValue p = JsonValue::object();
      p.set("rotate_degrees", num(10.0));
      const CommandResult r = applyCommand(withSelection, Command{"numeric_transform", p});
      check(!r.ok && contains(r.status, "selection"),
            "numeric_transform: refuses a live selection by name -- that stays the "
            "interactive dialog's and Free Transform's job");
    }
    {
      OpenDocument locked = makeImageCommandDocument();
      locked.selection.reset();
      locked.document.layers[locked.activeLayer].locked = true;
      JsonValue p = JsonValue::object();
      p.set("rotate_degrees", num(10.0));
      const CommandResult r = applyCommand(locked, Command{"numeric_transform", p});
      check(!r.ok && contains(r.status, "locked"),
            "numeric_transform: refuses a locked layer, by name");
    }
  }

  return ok;
}

}  // namespace np
