// naturalPaint — real-time watercolour on WebGPU.
//
//   Curtis et al. 1997   shallow-water + pigment transport + capillary layer
//   Stam 1999            semi-Lagrangian advection, Jacobi projection
//   Sochorova & Jamriska 2021  Mixbox latent-space pigment mixing
//
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cfloat>
#include <cstdlib>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/AbrReport.hpp"
#include "app/ProfileToggle.hpp"
#include "app/PsdReport.hpp"
#include "app/BrushSheet.hpp"
#include "app/StrokePreview.hpp"
#include "app/AppState.hpp"
#include "app/FixedStep.hpp"
#include "app/Keymap.hpp"
#include "app/Latency.hpp"
#include "app/Memory.hpp"
#include "app/OpenAnyFile.hpp"
#include "app/PenAxes.hpp"
#include "app/Screenshot.hpp"
#include "app/SelfTest.hpp"
#include "app/StrokeBake.hpp"
#include "app/StrokeSession.hpp"
#include "app/ZoomAndSize.hpp"
#include "brush/Deposit.hpp"
#include "color/Space.hpp"
#include "core/Composite.hpp"
#include "core/Blend.hpp"
#include "app/LayerEditor.hpp"
#include "app/LayerPanel.hpp"
#include "core/LayerOps.hpp"
#include "core/LayerSetOps.hpp"
#include "core/Tile.hpp"
#include "core/LayerCompOps.hpp"
#include "core/ResourcePaths.hpp"
#include "gfx/Context.hpp"
#include "io/ImageDecode.hpp"
#include "paint/Palette.hpp"
#include "sim/PaintSim.hpp"
#include "ui/Fonts.hpp"
#include "ui/CanvasQuad.hpp"
#include "ui/FileDialog.hpp"
#include "ui/MacNativeMenu.hpp"
#include "ui/MacTrackpadTouch.hpp"
#include "ui/MacPaintUI.hpp"
#include "ui/AtelierChrome.hpp"
#include "ui/AtelierLayout.hpp"
#include "ui/AtelierTheme.hpp"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_wgpu.h"

namespace {

constexpr uint32_t kCanvasW = 1024;
constexpr uint32_t kCanvasH = 1024;

// --demo-document (UI detour step 2, Part D): fills the session's document
// with content, so that a `--screenshot` shows the composite *doing* something
// rather than showing that a transparent document changes nothing.
//
// Here, in main.cpp's anonymous namespace, rather than in a production module:
// nothing in the application builds a document from literals, this exists to
// make a verification claim photographable, and a `ui/` or `core/` module that
// carried demo fixtures would be carrying test data in production code.
//
// The three layers are chosen so that a single picture shows three *different*
// mechanisms, each of which would look identical to the others if it were
// silently not working:
//
//   0. a plain opaque rectangle          -- the composite reaches the screen
//   1. Multiply at 60% opacity           -- blend and opacity are honoured
//   2. an opaque rectangle under a mask  -- coverage is per texel
//
// Every rectangle is inset well inside the canvas, so the transparent
// remainder proves the paper is still visible through alpha 0 -- the same
// regression boundary the no-flag screenshot makes at full canvas size.
void buildDemoDocument(np::OpenDocument& od) {
  using np::PixelCoord;
  np::Document& doc = od.document;

  auto fillRect = [&](size_t layerIndex, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                      const std::array<float, 4>& straight) {
    // Stored premultiplied (DESIGN-imaging.md §2), which is what makes
    // ui/DocumentTexture's un-premultiply on the way out a real operation
    // rather than a copy.
    const std::array<float, 4> premultiplied{straight[0] * straight[3], straight[1] * straight[3],
                                             straight[2] * straight[3], straight[3]};
    for (int32_t y = y0; y < y1; ++y) {
      for (int32_t x = x0; x < x1; ++x) {
        const PixelCoord at{x, y};
        doc.layers[layerIndex].rgbTiles->getOrCreate(np::tileCoordAt(at))
            .writePixel(np::tileLocalOffset(at), premultiplied);
      }
    }
  };

  fillRect(0, 96, 96, 544, 544, {0.10f, 0.52f, 0.74f, 1.0f});
  np::setLayerName(doc, 0, "Cyan block");

  np::addLayer(doc, doc.layers.size(), np::makeRgbLayer("Magenta, Multiply 60%"));
  fillRect(1, 352, 288, 800, 736, {0.86f, 0.16f, 0.42f, 1.0f});
  np::setLayerBlend(doc, 1, np::BlendMode::Multiply);
  np::setLayerOpacity(doc, 1, 0.6f);

  np::addLayer(doc, doc.layers.size(), np::makeRgbLayer("Yellow, masked ramp"));
  fillRect(2, 480, 512, 928, 928, {0.96f, 0.78f, 0.16f, 1.0f});
  np::addLayerMask(doc, 2);
  for (int32_t y = 512; y < 928; ++y) {
    for (int32_t x = 480; x < 928; ++x) {
      const PixelCoord at{x, y};
      doc.layers[2].mask->getOrCreate(np::tileCoordAt(at))
          .writeCoverage(np::tileLocalOffset(at), static_cast<float>(x - 480) / 448.0f);
    }
  }

  // One recordEdit at the end, not one per write: tile writes go straight to
  // the store and bump nothing, so the revision the texture cache keys on has
  // to be moved deliberately here. That is the caching property
  // ui/DocumentTexture.hpp names -- demonstrated by the one code path in this
  // repository that writes tiles without going through core/LayerOps.
  od.recordEdit("demo document", np::EditKind::Content);
}

// --pigment-stroke-demo [normal] (Phase 5, the CPU Pigment deposit): **the
// first hand-made stroke in this project's life that reaches a `Layer`.**
//
// Two Pigment layers, each painted by a real `app::StrokeSession` -- the same
// class the pen will drive -- with the upper one blended `Mix` (the default)
// or `Normal` (the `normal` argument). The two screenshots differ in exactly
// one property of one layer, so the difference between the pictures *is*
// PRD C3, which is what makes the pair worth capturing rather than either one
// alone.
//
// A broad yellow stroke crossed by a broad blue one, and the crossing square
// is the whole point: under `Mix` it is green, because half a mass of blue
// pigment mixed into a mass of yellow pigment IS green; under `Normal` it is
// the blue-over-yellow average, which is a pale wash. The arms of each stroke
// show the two pigments unmixed in both pictures, so the crossing is the only
// thing that moved.
//
// Here in main.cpp's anonymous namespace for the same reason
// buildDemoDocument() is: this exists to make a verification claim
// photographable, and a `ui/` or `core/` module carrying demo fixtures would
// be carrying test data in production code.
void buildPigmentStrokeDemo(np::OpenDocument& od, bool mix) {
  np::Document& doc = od.document;
  np::recordLayerEdit(
      od, np::addLayer(doc, doc.layers.size(), np::makePigmentLayer("Yellow ground")));
  np::recordLayerEdit(od, np::addLayer(doc, doc.layers.size(),
                                       np::makePigmentLayer(mix ? "Blue, Mix" : "Blue, Normal")));

  // Mixbox's own primaries, straight from core/Pigment.cpp's polynomial: c1 is
  // its yellow, c0 its blue, and the derived fourth weight is white. No LUT is
  // loaded for this, because latent -> RGB needs none.
  np::Latent yellow;
  yellow.c = {0.0f, 1.0f, 0.0f};
  np::Latent blue;
  blue.c = {0.625f, 0.0f, 0.0f};

  auto paint = [&](size_t layerIndex, const np::Latent& pigment, float flow, float x0, float y0,
                   float x1, float y1) {
    np::BrushTip tip;
    tip.radius = 110.0f;
    tip.hardness = 0.55f;
    tip.flow = flow;
    tip.pigment = pigment;

    np::StrokeSession session;
    std::string error;
    if (!session.begin(od, layerIndex, tip, np::Tool::Brush, &error)) {
      std::fprintf(stderr, "[pigment-stroke-demo] %s\n", error.c_str());
      return;
    }
    constexpr int kSamples = 48;  // one per render frame, as the pen would
    for (int i = 0; i <= kSamples; ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(kSamples);
      session.addPoint(x0 + (x1 - x0) * u, y0 + (y1 - y0) * u);
    }
    session.end();
    std::printf("[pigment-stroke-demo] layer %zu '%s': %zu dabs, %zu texels, %zu tiles, "
                "1 history entry ('%s')\n",
                layerIndex, doc.layers[layerIndex].name.c_str(), session.dabCount(),
                session.texelsWritten(), session.strokeTiles().size(),
                session.label().c_str());
  };

  // Full flow on the ground so its core saturates; a low flow on the blue so
  // its core lands near half a mass, which is PRD C3's own worked example.
  paint(1, yellow, 0.5f, 150.0f, 512.0f, 880.0f, 512.0f);
  paint(2, blue, 0.075f, 512.0f, 150.0f, 512.0f, 880.0f);

  np::setLayerBlend(doc, 2, mix ? np::BlendMode::Mix : np::BlendMode::Normal);

  // The numbers behind the picture, so the screenshot is checkable from the
  // log rather than only by eye.
  auto texelAt = [&](size_t layerIndex, int32_t x, int32_t y) {
    const np::PixelCoord at{x, y};
    const np::PigmentTile* t = doc.layers[layerIndex].pigmentTiles->find(np::tileCoordAt(at));
    return t ? t->readTexel(np::tileLocalOffset(at)) : np::PigmentTexel{};
  };
  const np::PigmentTexel low = texelAt(1, 512, 512);
  const np::PigmentTexel up = texelAt(2, 512, 512);
  const std::vector<float> comp = np::compositeDocumentPremultiplied(doc);
  const size_t i = (static_cast<size_t>(512) * static_cast<size_t>(doc.width) + 512) * 4;
  const float a = comp[i + 3];
  std::printf("[pigment-stroke-demo] crossing: yellow mass %.3f, blue mass %.3f -- %s gives "
              "(%.3f %.3f %.3f) at alpha %.3f\n",
              static_cast<double>(low.mass), static_cast<double>(up.mass),
              mix ? "Mix" : "Normal", static_cast<double>(comp[i + 0] / a),
              static_cast<double>(comp[i + 1] / a), static_cast<double>(comp[i + 2] / a),
              static_cast<double>(a));
  std::printf("[pigment-stroke-demo] %zu layers, revision %llu, %zu history entries\n",
              doc.layers.size(), static_cast<unsigned long long>(od.revision),
              od.history.entries().size());
}

// --ui-layer-demo [noclip] (UI detour step 3): builds a layer stack using
// **only the layer editor's own entry points** -- the same `applyLayerCommand()`
// the `Layer` menu and the LAYERS panel buttons call, and the same recorded
// op-stack operations the per-layer op editor calls.
//
// This is deliberately not a second `buildDemoDocument()`. That one writes
// tiles directly and calls `recordEdit()` by hand, which is honest for a
// pixel fixture and is exactly what a UI can never do; this one presses
// buttons. Every line it prints is one gesture and the document's revision
// after it, so the claim "this document was built through the UI's own
// operations" is checkable from the log rather than from a screenshot.
//
// The stack it leaves, on top of whatever the document already had:
//
//   T+2  Pigment, with a mask                 -- steps 4 and 5 of Phase 5
//   T+1  Adjustment, one Exposure op, clipped -- steps 5, 9 and this step
//   T    whatever was on top before
//
// The adjustment is clipped to the layer that was on top when the script
// started, which is a layer with pixels in it under `--demo-document`. That is
// the pair that makes clipping visible on screen: the grade lands only where
// that layer has alpha. `noclip` runs the identical script without the clip,
// for the comparison shot.
// --pen-demo's fixture: give the pointer something it is allowed to paint.
//
// The brush routes to the CPU deposit only on an unlocked Pigment layer
// (app/StrokeSession section 1), and neither a blank document nor
// `--demo-document` has one -- so without this the flag would exercise the
// PaintSim branch and prove nothing about the new route. Added on top and made
// **active**, which is the state a user reaches by pressing `+ Pigment` in
// LAYERS.
void preparePenDemo(np::OpenDocument& od) {
  const np::LayerEditResult r =
      np::applyLayerCommand(od, np::LayerCommand::NewPigmentLayer, od.activeLayer);
  if (!r.ok) {
    std::fprintf(stderr, "[pen-demo] %s\n", r.error.c_str());
    return;
  }
  np::setActiveLayer(od, r.selected);
  const np::Layer* target = np::activeLayerOf(od);
  std::printf("[pen-demo] active layer %zu of %zu: '%s', route %s\n", od.activeLayer,
              od.document.layers.size(), target != nullptr ? target->name.c_str() : "(none)",
              np::strokeRouteName(np::strokeRouteFor(np::Tool::Brush, target)));
}

void runUiLayerDemo(np::OpenDocument& od, bool clip) {
  size_t selected = od.document.layers.empty() ? 0 : od.document.layers.size() - 1;
  int step = 0;
  auto report = [&](const char* gesture, bool ok, const std::string& error) {
    std::printf("[ui-layer-demo] %2d. %-24s %s%s  (revision %llu, %zu layers)\n", ++step, gesture,
                ok ? "ok" : "REFUSED -- ", ok ? "" : error.c_str(),
                static_cast<unsigned long long>(od.revision), od.document.layers.size());
  };
  auto press = [&](np::LayerCommand command) {
    const np::LayerEditResult r = np::applyLayerCommand(od, command, selected);
    report(np::layerCommandLabel(command), r.ok, r.error);
    selected = r.selected;
    return r.ok;
  };
  // The op-stack half. `recordLayerEdit` is the same funnel `applyLayerCommand`
  // uses internally, and the same one ui/MacPaintUI's op editor binds its
  // buttons to -- a layer op written around it would not move the revision, and
  // ui/DocumentTexture would not recomposite.
  auto opEdit = [&](const char* gesture, np::LayerOpResult result) {
    const np::DocumentOpResult out = np::recordLayerEdit(od, std::move(result));
    report(gesture, out.ok, out.error);
    return out.ok;
  };

  std::printf("[ui-layer-demo] starting from %zu layer(s), selection on layer %zu\n",
              od.document.layers.size(), selected);

  press(np::LayerCommand::NewAdjustmentLayer);
  const size_t adjustment = selected;
  // "+ Add" with the kind combo on Exposure: `makeNewOp()` is what the button
  // adds, disabled, so nothing on screen changes yet.
  opEdit("+ Add op (Exposure)",
         np::addLayerOp(od.document, adjustment, np::makeNewOp(np::PointOpKind::Exposure)));
  // The Stops slider, which writes the whole op back through setLayerOp().
  //
  // **Negative**, so the clip is legible in a single frame rather than only in
  // the difference between two: a darkening grade confined to the base layer's
  // alpha draws its own boundary against the layers it is not allowed to
  // touch. A brightening one on this fixture lands mostly on a region that is
  // already near white.
  {
    np::Op op = od.document.layers[adjustment].ops.at(0);
    op.exposure.stops = -1.5f;
    opEdit("Stops slider -> -1.5", np::setLayerOp(od.document, adjustment, 0, op));
  }
  opEdit("enable op", np::setLayerOpEnabled(od.document, adjustment, 0, true));
  if (clip) press(np::LayerCommand::ToggleClipped);

  press(np::LayerCommand::NewPigmentLayer);
  press(np::LayerCommand::AddMask);

  // Leave the panel showing the adjustment layer, so its op stack is on screen
  // in the screenshot rather than collapsed behind another row's selection.
  np::setLayersPanelSelection(od, adjustment);

  // The finished stack, in the layers panel's own words -- app/LayerPanel's row
  // text, top first, which is what the panel draws. Printed so the picture and
  // the log can be checked against each other.
  std::printf("[ui-layer-demo] final stack, top first:\n");
  const size_t count = od.document.layers.size();
  for (size_t row = 0; row < count; ++row) {
    const size_t i = np::layerIndexForPanelRow(row, count);
    std::printf("[ui-layer-demo]   %s %-24s %s\n", np::layerKindGlyph(od.document.layers[i].kind),
                np::layerRowTitle(od.document.layers[i], i).c_str(),
                np::layerRowSubLine(od.document.layers[i]).c_str());
  }
}

// --ui-merge-demo <command> (PLAN.md Phase 5 step 10): press exactly one of
// core/Merge's five buttons on the session's document, through the same
// `applyLayerCommand()` the `Layer` menu and the LAYERS panel call.
//
// It exists for one reason: PRD C10 is a P0 whose whole deliverable is *a
// picture that changed*, and there was no way to make a merge happen from
// outside the window. Running the app twice -- once with the flag and once
// without -- and photographing both is the before/after pair. Every line it
// prints is the result and the warnings, so what the picture shows and what
// the model says can be checked against each other.
//
// The argument is a comma-separated list because two of the five need a
// stack this build cannot otherwise reach from the command line: `stamp,down`
// is the shortest route to a merge down that *succeeds* on the demo document,
// whose middle layer is `multiply` and whose top layer is masked -- so a bare
// `down` on it is refused, correctly and by design (core/Merge.hpp section 4).
// Both are worth photographing and the flag can produce either.
// --ui-multiselect-demo <script> (PLAN.md Phase 5 step 11; PRD C12, C13, C15):
// presses the multi-selection's own set commands through
// `app::applyLayerSetCommand()` -- the identical entry point the LAYERS panel's
// Multi-selection buttons and the `Layer` > Selection menu items call, so a
// screenshot of the result is a screenshot of what a click does.
//
// `runUiLayerDemo()`'s reason for existing, one level up: a fixture that wrote
// `Layer::colorLabel` directly would photograph a struct field, not a feature.
//
// The script is comma-separated. `select:0.2.4` sets the selection (dot-
// separated indices, so a comma stays the script's own separator); every other
// token is a set command by short name.
void runUiMultiSelectDemo(np::OpenDocument& od, std::string_view script) {
  struct Entry {
    const char* name;
    np::LayerSetCommand command;
  };
  static constexpr Entry kEntries[] = {
      {"delete", np::LayerSetCommand::DeleteLayers},
      {"duplicate", np::LayerSetCommand::DuplicateLayers},
      {"up", np::LayerSetCommand::MoveLayersUp},
      {"down", np::LayerSetCommand::MoveLayersDown},
      // PRD C7's UI half: the two tokens this table was missing when
      // `GroupLayers`/`UngroupLayers` joined `core::allLayerSetCommands()`.
      // Nothing else about this demo needed to change -- both commands
      // already ran through the identical `applyLayerSetCommand()` funnel
      // every other entry here does; the panel's own "Multi-selection"
      // section and the `Layer` > Selection menu already offered them too,
      // by walking that same list generically. This table was simply
      // incomplete.
      {"group", np::LayerSetCommand::GroupLayers},
      {"ungroup", np::LayerSetCommand::UngroupLayers},
      {"show", np::LayerSetCommand::ShowLayers},
      {"hide", np::LayerSetCommand::HideLayers},
      {"lock", np::LayerSetCommand::LockLayers},
      {"unlock", np::LayerSetCommand::UnlockLayers},
      {"link", np::LayerSetCommand::LinkLayers},
      {"unlink", np::LayerSetCommand::UnlinkLayers},
      {"red", np::LayerSetCommand::LabelRed},
      {"orange", np::LayerSetCommand::LabelOrange},
      {"yellow", np::LayerSetCommand::LabelYellow},
      {"green", np::LayerSetCommand::LabelGreen},
      {"blue", np::LayerSetCommand::LabelBlue},
      {"violet", np::LayerSetCommand::LabelViolet},
      {"grey", np::LayerSetCommand::LabelGrey},
      {"nolabel", np::LayerSetCommand::LabelNone},
      {"alignleft", np::LayerSetCommand::AlignSelectionLeft},
      {"alignright", np::LayerSetCommand::AlignSelectionRight},
      {"aligncx", np::LayerSetCommand::AlignSelectionCenterX},
      {"aligncy", np::LayerSetCommand::AlignSelectionCenterY},
      {"canvascx", np::LayerSetCommand::AlignCanvasCenterX},
      {"canvascy", np::LayerSetCommand::AlignCanvasCenterY},
      {"canvasleft", np::LayerSetCommand::AlignCanvasLeft},
      {"distx", np::LayerSetCommand::DistributeHorizontally},
      {"disty", np::LayerSetCommand::DistributeVertically},
  };
  np::LayerSelection selection =
      np::singleLayerSelection(od.document.layers.empty() ? 0 : od.document.layers.size() - 1);
  np::setLayersPanelSelectionSet(od, selection);
  std::printf("[ui-multiselect-demo] before: %zu layer(s), revision %llu\n",
              od.document.layers.size(), static_cast<unsigned long long>(od.revision));
  size_t at = 0;
  while (at <= script.size()) {
    const size_t comma = script.find(',', at);
    const std::string_view token =
        script.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
    at = comma == std::string_view::npos ? script.size() + 1 : comma + 1;
    if (token.empty()) continue;
    if (token.rfind("select:", 0) == 0) {
      std::vector<size_t> indices;
      const std::string list(token.substr(7));
      size_t p = 0;
      while (p <= list.size()) {
        const size_t dot = list.find('.', p);
        const std::string one =
            list.substr(p, dot == std::string::npos ? std::string::npos : dot - p);
        p = dot == std::string::npos ? list.size() + 1 : dot + 1;
        if (!one.empty()) indices.push_back(static_cast<size_t>(std::atoi(one.c_str())));
      }
      selection = np::makeLayerSelection(std::move(indices));
      np::setLayersPanelSelectionSet(od, selection);
      std::printf("[ui-multiselect-demo]     select %zu layer(s)\n", selection.size());
      continue;
    }
    const np::LayerSetCommand* command = nullptr;
    for (const Entry& e : kEntries)
      if (token == e.name) command = &e.command;
    if (command == nullptr) {
      std::printf("[ui-multiselect-demo] unknown command \"%.*s\"\n",
                  static_cast<int>(token.size()), token.data());
      continue;
    }
    const np::LayerSetEditResult r = np::applyLayerSetCommand(od, *command, selection);
    std::printf("[ui-multiselect-demo] %-34s %s%s  (revision %llu, %zu layers)\n",
                np::layerSetCommandLabel(*command), r.ok ? "ok" : "REFUSED -- ",
                r.ok ? "" : r.error.c_str(), static_cast<unsigned long long>(od.revision),
                od.document.layers.size());
    for (const std::string& w : r.warnings)
      std::printf("[ui-multiselect-demo]     warning: %s\n", w.c_str());
    if (r.ok) selection = r.selection;
    np::setLayersPanelSelectionSet(od, selection);
    np::setLayersPanelMessages(r.ok ? std::string() : r.error, r.warnings);
  }
}

void runUiMergeDemo(np::OpenDocument& od, std::string_view list) {
  struct Entry {
    const char* name;
    np::LayerCommand command;
  };
  static constexpr Entry kEntries[] = {
      {"down", np::LayerCommand::MergeDown},        {"visible", np::LayerCommand::MergeVisible},
      {"stamp", np::LayerCommand::StampVisible},    {"flatten", np::LayerCommand::FlattenImage},
      {"rasterise", np::LayerCommand::RasteriseLayer},
  };
  // The top layer, which is what a panel opens on and what "merge down" is
  // about. `--demo-document` leaves three layers with pixels in them.
  size_t selected = od.document.layers.empty() ? 0 : od.document.layers.size() - 1;
  np::setLayersPanelSelection(od, selected);
  std::printf("[ui-merge-demo] before: %zu layer(s), selection on layer %zu, revision %llu\n",
              od.document.layers.size(), selected,
              static_cast<unsigned long long>(od.revision));
  size_t at = 0;
  while (at <= list.size()) {
    const size_t comma = list.find(',', at);
    std::string_view which =
        list.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
    at = comma == std::string_view::npos ? list.size() + 1 : comma + 1;
    if (which.empty()) continue;
    // `name:index` picks the row the command acts on, because the default --
    // the top layer -- is the wrong one for exactly one of the five:
    // `--ui-layer-demo` leaves a Pigment layer above the Adjustment layer it
    // built, and Rasterise Layer is about the Adjustment one.
    if (const size_t colon = which.find(':'); colon != std::string_view::npos) {
      selected = static_cast<size_t>(std::atoi(std::string(which.substr(colon + 1)).c_str()));
      which = which.substr(0, colon);
    }
    const np::LayerCommand* command = nullptr;
    for (const Entry& e : kEntries)
      if (which == e.name) command = &e.command;
    if (command == nullptr) {
      std::printf("[ui-merge-demo] unknown command \"%.*s\" -- one of: down, visible, stamp, "
                  "flatten, rasterise\n",
                  static_cast<int>(which.size()), which.data());
      continue;
    }
    const np::LayerEditResult r = np::applyLayerCommand(od, *command, selected);
    std::printf("[ui-merge-demo] %-16s %s%s\n", np::layerCommandLabel(*command),
                r.ok ? "ok" : "REFUSED -- ", r.ok ? "" : r.error.c_str());
    for (const std::string& w : r.warnings)
      std::printf("[ui-merge-demo]   warning: %s\n", w.c_str());
    selected = r.selected;
    // Exactly what a button press would have left in the panel, so the
    // screenshot shows the same thing a user would see.
    np::setLayersPanelMessages(r.ok ? std::string() : r.error, r.warnings);
    std::printf("[ui-merge-demo] after:  %zu layer(s), selection on layer %zu, revision %llu\n",
                od.document.layers.size(), selected,
                static_cast<unsigned long long>(od.revision));
  }
  np::setLayersPanelSelection(od, selected);
  std::printf("[ui-merge-demo] final stack, top first:\n");
  const size_t count = od.document.layers.size();
  for (size_t row = 0; row < count; ++row) {
    const size_t i = np::layerIndexForPanelRow(row, count);
    std::printf("[ui-merge-demo]   %s %-24s %s\n", np::layerKindGlyph(od.document.layers[i].kind),
                np::layerRowTitle(od.document.layers[i], i).c_str(),
                np::layerRowSubLine(od.document.layers[i]).c_str());
  }
}

void handlePenEvent(np::AppState& st, const SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_PEN_DOWN:
      st.penSeen = true;
      st.penDown = true;
      break;
    case SDL_EVENT_PEN_UP:
      st.penDown = false;
      st.penPressure = 0.0f;
      break;
    case SDL_EVENT_PEN_AXIS:
      // Each axis arrives as its own event, which is why the two tilt angles
      // are stored raw and re-converted on either one: an x-tilt event on its
      // own cannot compute an azimuth, it needs the y that came before it.
      switch (e.paxis.axis) {
        case SDL_PEN_AXIS_PRESSURE:
          st.penSeen = true;
          st.penPressure = std::clamp(e.paxis.value, 0.0f, 1.0f);
          break;
        case SDL_PEN_AXIS_XTILT:
        case SDL_PEN_AXIS_YTILT:
          st.penSeen = true;
          if (e.paxis.axis == SDL_PEN_AXIS_XTILT) st.penTiltXDeg = e.paxis.value;
          else st.penTiltYDeg = e.paxis.value;
          st.penTilt = np::penTiltNormalised(st.penTiltXDeg, st.penTiltYDeg);
          st.penAzimuth = np::penAzimuthNormalised(st.penTiltXDeg, st.penTiltYDeg);
          break;
        case SDL_PEN_AXIS_ROTATION:
          st.penSeen = true;
          st.penBarrel = np::penBarrelNormalised(e.paxis.value);
          break;
        default:
          break;
      }
      break;
    default:
      break;
  }
}

// Events that carry a new pointer sample worth judging for responsiveness.
// Pen events cover tablets; mouse events make --latency measurable on the
// mouse/trackpad hardware most dev machines actually have.
bool isPointerSampleEvent(const SDL_Event& e) {
  switch (e.type) {
    case SDL_EVENT_PEN_MOTION:
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_AXIS:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
      return true;
    default:
      return false;
  }
}

// --comps-demo [index] (PLAN.md Phase 5 step 12): capture two layer comps of
// whatever the document currently holds, then restore comp `index`.
//
// Here rather than in a production module for `buildDemoDocument()`'s reason:
// nothing in the application builds a document from literals, and this exists
// only to make "two comps, switched between, visibly changing the canvas" a
// photographable claim. Like `runUiLayerDemo()` it presses the application's
// own buttons -- `captureLayerComp()` and `restoreLayerComp()` through
// `recordLayerEdit()`, which is exactly what the COMPS panel's + Capture and
// Restore do -- so the picture and the log can be checked against each other.
//
// The two comps, over `--demo-document`'s three layers:
//
//   0  "Everything"   every layer showing, as the fixture built it
//   1  "Cyan only"    the magenta and the masked yellow hidden, the cyan block
//                     dropped to 45% -- three of the four captured properties
//                     differing at once
//
// `drop` deletes the top layer between the second capture and the restore, so
// the restore is **partial** and the panel shows the sentence it says about
// that. That is the case worth photographing: a comp outliving a layer is the
// ordinary state of a comp, and what the panel does about it is the whole
// design of the step.
void runCompsDemo(np::OpenDocument& od, size_t restoreIndex, bool dropALayer) {
  np::Document& doc = od.document;
  auto report = [&](const char* gesture, const np::DocumentOpResult& r) {
    std::printf("[comps-demo] %-22s %s%s  (revision %llu, %zu comp(s))\n", gesture,
                r.ok ? "ok" : "REFUSED -- ", r.ok ? "" : r.error.c_str(),
                static_cast<unsigned long long>(od.revision), doc.comps.size());
  };

  report("capture \"Everything\"", np::recordLayerEdit(od, np::captureLayerComp(doc, "Everything")));
  for (size_t i = 1; i < doc.layers.size(); ++i)
    np::recordLayerEdit(od, np::setLayerVisible(doc, i, false));
  if (!doc.layers.empty()) np::recordLayerEdit(od, np::setLayerOpacity(doc, 0, 0.45f));
  report("capture \"Cyan only\"", np::recordLayerEdit(od, np::captureLayerComp(doc, "Cyan only")));
  if (dropALayer && !doc.layers.empty())
    report("delete the top layer",
           np::recordLayerEdit(od, np::removeLayer(doc, doc.layers.size() - 1)));

  np::LayerCompRestoreReport restore;
  const np::DocumentOpResult r =
      np::recordLayerEdit(od, np::restoreLayerComp(doc, restoreIndex, &restore));
  report("restore", r);
  const std::string summary = np::layerCompRestoreSummary(restore);
  // Put it where the panel would have put it had a user pressed Restore, so a
  // `--screenshot` photographs what the application really says about a partial
  // restore rather than only what this log says.
  np::setCompsPanelRestoreSummary(summary);
  std::printf("[comps-demo] restored comp %zu (\"%s\"): %zu of %zu layer state(s) applied, %zu "
              "changed. %s\n",
              restoreIndex,
              restoreIndex < doc.comps.size() ? doc.comps[restoreIndex].name.c_str() : "?",
              restore.entriesApplied, doc.comps.empty() ? 0 : doc.comps[0].layers.size(),
              restore.layersChanged, summary.empty() ? "Applied in full." : summary.c_str());
  for (size_t i = 0; i < doc.layers.size(); ++i)
    std::printf("[comps-demo]   layer %zu %-24s %s\n", i,
                np::layerRowTitle(doc.layers[i], i).c_str(),
                np::layerRowSubLine(doc.layers[i]).c_str());
}

// --split-demo [rows] (PLAN.md Phase 5 step 14): **open two documents and turn
// the split on**, which is the one thing no other flag can do.
//
// Step 14's own commit message stated the gap: "no CLI flag opens two
// documents, so the two-pane ImGui drawing is compile-verified only". Its pure
// halves -- the pane geometry, the pane-to-document rule and the two-slot
// residency pool -- are asserted headlessly in
// src/app/selftest/DocumentResidency.cpp, but `splitActive` in
// ui/MacPaintUI.cpp stayed unreachable from outside the GUI because
// `--screenshot` opened exactly one document. This flag is the missing route,
// and `verifySplitDemoScreenshot()` below is what makes it a *check* rather
// than a second picture to look at.
//
// Here in main.cpp's anonymous namespace for `buildDemoDocument()`'s reason,
// which applies unchanged: nothing in the application builds a document from
// literals, this exists to make a verification claim photographable, and a
// `ui/` or `core/` module carrying demo fixtures would be carrying test data
// in production code.
//
// **The two documents differ in every way the screenshot can be asked about.**
//
//   * Colour. A is green and B is blue-violet, far apart in every channel --
//     so "which document is in this pane" is answerable from one pixel, and a
//     *swap* is answerable from a bounding box.
//   * Size. A is the session's own canvas-sized 1024 x 1024 square; B is
//     768 x 432, a 16:9 landscape. The unfocused pane fits the whole document
//     it shows, so the two panes photograph a square and a letterbox.
//   * Extent. Each field leaves its top-left eighth unpainted, so the paper
//     shows through one corner. That keeps `buildDemoDocument()`'s "the
//     transparent remainder proves the paper is still visible" boundary, and
//     because it is a corner rather than a centred mark it would make a
//     mirrored or rotated draw visible instead of plausible.
//
// The colours are linear light, as everything in a `Layer` is. What they reach
// the screen as is `srgbEncode()` of them -- ui/CanvasQuad's fragment shader
// performs that encode, because gfx/Context deliberately takes a non-sRGB
// surface -- and the verifier expects exactly that function rather than a
// hand-rolled gamma.
constexpr int32_t kSplitDemoBW = 768;
constexpr int32_t kSplitDemoBH = 432;
constexpr std::array<float, 4> kSplitFieldA{0.05f, 0.40f, 0.10f, 1.0f};
constexpr std::array<float, 4> kSplitFieldB{0.10f, 0.06f, 0.55f, 1.0f};

// Neither field is any chrome token, and that is a requirement rather than an
// aesthetic: the verifier counts matching pixels over the *whole* framebuffer,
// so a field that collided with (say) the accent would count the focused
// pane's own focus rule and the active tab's underline as document pixels. The
// nearest token to either is `kAccent` #ff563c, strongly red where A is
// strongly green and B strongly blue.
static_assert(kSplitFieldA[1] > 3.0f * kSplitFieldA[0] && kSplitFieldA[1] > 3.0f * kSplitFieldA[2],
              "--split-demo: document A has to be unambiguously green");
static_assert(kSplitFieldB[2] > 3.0f * kSplitFieldB[0] && kSplitFieldB[2] > 3.0f * kSplitFieldB[1],
              "--split-demo: document B has to be unambiguously blue");

// A flat opaque field over the whole document except its top-left eighth.
void fillSplitDemoField(np::OpenDocument& od, const std::array<float, 4>& straight) {
  np::Document& doc = od.document;
  if (doc.layers.empty()) return;
  // Stored premultiplied (DESIGN-imaging.md §2), exactly as buildDemoDocument()
  // does and for its reason: it is what makes ui/DocumentTexture's
  // un-premultiply on the way out a real operation rather than a copy.
  const std::array<float, 4> premultiplied{straight[0] * straight[3], straight[1] * straight[3],
                                           straight[2] * straight[3], straight[3]};
  const int32_t notchW = doc.width / 8, notchH = doc.height / 8;
  for (int32_t y = 0; y < doc.height; ++y) {
    for (int32_t x = 0; x < doc.width; ++x) {
      if (x < notchW && y < notchH) continue;  // the unpainted corner
      const np::PixelCoord at{x, y};
      doc.layers[0].rgbTiles->getOrCreate(np::tileCoordAt(at))
          .writePixel(np::tileLocalOffset(at), premultiplied);
    }
  }
  // One recordEdit for the whole fill, not one per write: tile writes go
  // straight to the store and bump nothing, so the revision
  // ui/DocumentTexture's cache keys on has to be moved deliberately here.
  od.recordEdit("split demo field", np::EditKind::Content);
}

// Paint the session's document, add a second one, and put the active document
// back on the first.
//
// That last part is the one that needs saying. `DocumentSession::add()` makes
// what it adds active and `atelierPaneDocuments()` puts the active document in
// the focused pane, whose index starts at 0 -- so without the `setActive(0)`
// the picture would still be right and the *contract this demo asserts*, "pane
// 0 shows A", would be inverted. Clicking back onto the first tab is what a
// user does to reach the same state, so nothing here is a state a click cannot
// produce.
void buildSplitDemo(np::AppState& st, np::AtelierSplit mode) {
  np::OpenDocument* a = st.documents.active();
  if (a == nullptr) return;
  a->title = "Split A (green)";
  fillSplitDemoField(*a, kSplitFieldA);

  np::OpenDocument* b = st.documents.add(np::makeBlankOpenDocument(
      kSplitDemoBW, kSplitDemoBH, np::WorkingSpace{}, "Split B (blue)"));
  if (b != nullptr) fillSplitDemoField(*b, kSplitFieldB);
  st.documents.setActive(0);

  np::setSplitArrangement(mode);

  // Zoom out so the whole of A fits either arrangement's pane at the default
  // window size. The focused pane is the *real* canvas, with the session's one
  // shared `CanvasView`, and at 100% a 1024 px document overflows a half-sized
  // pane and shows only its top-left corner -- which is precisely the eighth
  // the fixture leaves unpainted.
  //
  // Cosmetic only, and deliberately so: every assertion below is a bounding
  // box over matched pixels, which holds whether the document fits its pane or
  // is clipped by it. A verification that needed a particular zoom would be a
  // verification that broke when the window was resized.
  st.view.zoom = 0.25f;

  std::printf("[split-demo] %s: A \"%s\" %d x %d, B \"%s\" %d x %d\n",
              mode == np::AtelierSplit::Columns ? "columns-2 (side by side)"
                                                : "layout-grid (top and bottom)",
              np::documentDisplayName(*a).c_str(), a->document.width, a->document.height,
              b != nullptr ? np::documentDisplayName(*b).c_str() : "?",
              b != nullptr ? b->document.width : 0, b != nullptr ? b->document.height : 0);
}

// --- reading the screenshot back -------------------------------------------
//
// What `--split-demo --screenshot` is *for*. Without this the flag would be a
// second picture to eyeball, and PLAN.md section 1.5 ("an unexercised build
// option is not a seam") applies just as much to a flag whose effect nothing
// checks.
//
// **The strategy, and why it is this one.** The tempting assertion -- sample
// the middle of pane 0, compare it to A -- needs the pane rectangle, and pane
// rectangles are in *logical* pixels while the screenshot is framebuffer
// pixels at 2x on this machine. Getting that backwards samples the wrong pixel
// and is the likeliest way to write an assertion that passes on the wrong
// picture. So the assertions that decide the result use **no geometry at
// all**: they find every pixel matching each field colour and compare the two
// bounding boxes.
//
//   * `columns-2`: every A pixel is left of every B pixel, and the two boxes
//     overlap vertically. That is what "side by side, A on the left" means,
//     stated without one layout constant.
//   * `layout-grid`: every A pixel is above every B pixel, and the two boxes
//     overlap horizontally.
//
// The two runs are each other's control. A build that ignored the arrangement
// and always drew columns passes the first and fails the second's vertical
// separation; a build that drew the panes *swapped* fails the ordering in
// both. Colour is what identifies a document and the box is what places it, so
// "the companion pane drew the focused document" cannot pass either.
//
// The icon check is the one thing that does need geometry, because a colour on
// its own cannot say *where* in the tab strip it was found. It calls the same
// pure `atelierLayout()` the chrome calls, at the same logical size, and
// scales by the ratio the decoded image itself reports -- so the logical-to-
// framebuffer conversion happens once, explicitly, in the one place needing it.
struct SplitDemoBox {
  size_t count = 0;
  float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;

  void add(float x, float y) {
    if (count == 0) {
      minX = maxX = x;
      minY = maxY = y;
    } else {
      minX = std::min(minX, x);
      maxX = std::max(maxX, x);
      minY = std::min(minY, y);
      maxY = std::max(maxY, y);
    }
    ++count;
  }
};

// io/ImageDecode hands back linear light -- it applies `srgbDecode()` to an
// 8-bit PNG's channels -- so the byte that was really in the file is
// `srgbEncode()` of what comes out. Comparing bytes rather than linear values
// keeps the tolerance in the unit it is really in: the screenshot is 8-bit, and
// one byte of rounding is one byte everywhere, where the same error expressed
// in linear light is some thirty times wider at the top of the range than at
// the bottom.
int splitDemoByte(float linear) {
  return static_cast<int>(std::lround(std::clamp(np::srgbEncode(linear), 0.0f, 1.0f) * 255.0f));
}

// Whether a decoded pixel is this field colour, within `tol` bytes on every
// channel. Tight on purpose: the fields are flat and opaque, so an exact match
// is the expected case and the tolerance pays only for the shader's
// float-to-unorm rounding.
bool splitDemoMatches(const float* px, const std::array<float, 4>& field, int tol) {
  for (int c = 0; c < 3; ++c)
    if (std::abs(splitDemoByte(px[c]) - splitDemoByte(field[c])) > tol) return false;
  return true;
}

// The brightest grey and the reddest pixel in a framebuffer-space rect. Both
// are how the split icons are read; see the icon check for what each says.
void splitDemoIconStats(const np::DecodedImage& img, const np::AtelierRect& r, int* maxGrey,
                        int* maxRedness) {
  *maxGrey = 0;
  *maxRedness = -255;
  const int x0 = std::max(0, static_cast<int>(r.x));
  const int y0 = std::max(0, static_cast<int>(r.y));
  const int x1 = std::min(static_cast<int>(img.width), static_cast<int>(r.right()));
  const int y1 = std::min(static_cast<int>(img.height), static_cast<int>(r.bottom()));
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const float* px = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
      const int rr = splitDemoByte(px[0]), gg = splitDemoByte(px[1]), bb = splitDemoByte(px[2]);
      // The *darkest* channel of the pixel, so that a saturated colour cannot
      // pass a brightness threshold on one channel alone.
      *maxGrey = std::max(*maxGrey, std::min(rr, std::min(gg, bb)));
      *maxRedness = std::max(*maxRedness, rr - std::max(gg, bb));
    }
  }
}

// Reads back the PNG `--screenshot` just wrote and says whether the split
// really drew. Every claim prints its own line in `--selftest`'s shape, so a
// failure names which one broke rather than only that one did.
bool verifySplitDemoScreenshot(const std::string& path, np::AtelierSplit mode, float logicalW,
                               float logicalH) {
  std::vector<uint8_t> bytes;
  {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
      std::fprintf(stderr, "[split-demo] cannot read back %s\n", path.c_str());
      return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    bytes.resize(size > 0 ? static_cast<size_t>(size) : 0u);
    const size_t got = bytes.empty() ? 0u : std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    bytes.resize(got);
  }
  std::string decodeError;
  const np::DecodedImage img = np::decodeImageLinear(bytes.data(), bytes.size(), &decodeError);
  if (!img.valid()) {
    std::fprintf(stderr, "[split-demo] %s: %s\n", path.c_str(), decodeError.c_str());
    return false;
  }

  bool ok = true;
  const auto check = [&ok](bool condition, const char* what) {
    std::printf("  %-76s %s\n", what, condition ? "pass" : "FAIL");
    if (!condition) ok = false;
  };

  SplitDemoBox a, b;
  for (uint32_t y = 0; y < img.height; ++y) {
    for (uint32_t x = 0; x < img.width; ++x) {
      const float* px = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
      if (splitDemoMatches(px, kSplitFieldA, 2))
        a.add(static_cast<float>(x), static_cast<float>(y));
      else if (splitDemoMatches(px, kSplitFieldB, 2))
        b.add(static_cast<float>(x), static_cast<float>(y));
    }
  }

  std::printf("[split-demo] %u x %u framebuffer -- A #%02x%02x%02x, %zu px, box "
              "(%.0f,%.0f)-(%.0f,%.0f); B #%02x%02x%02x, %zu px, box (%.0f,%.0f)-(%.0f,%.0f)\n",
              img.width, img.height, splitDemoByte(kSplitFieldA[0]),
              splitDemoByte(kSplitFieldA[1]), splitDemoByte(kSplitFieldA[2]), a.count, a.minX,
              a.minY, a.maxX, a.maxY, splitDemoByte(kSplitFieldB[0]),
              splitDemoByte(kSplitFieldB[1]), splitDemoByte(kSplitFieldB[2]), b.count, b.minX,
              b.minY, b.maxX, b.maxY);

  // A pane that drew nothing and a pane that drew the *other* document are
  // indistinguishable from one colour's count, so both counts are required
  // before anything is claimed about where they are. The floor is 10000
  // framebuffer pixels -- 100 x 100 -- which no anti-aliased chrome edge can
  // reach by accident and which the smaller pane clears by two orders of
  // magnitude at the default window size.
  constexpr size_t kFloor = 10000;
  check(a.count >= kFloor, "document A's field is on screen: the focused pane drew it");
  check(b.count >= kFloor, "document B's field is on screen: the companion pane drew it");

  if (a.count >= kFloor && b.count >= kFloor) {
    if (mode == np::AtelierSplit::Columns) {
      check(a.maxX < b.minX,
            "columns-2: every A pixel is left of every B pixel -- side by side, not swapped");
      check(a.minY < b.maxY && b.minY < a.maxY,
            "columns-2: the fields overlap vertically -- side by side, not stacked");
    } else {
      check(a.maxY < b.minY,
            "layout-grid: every A pixel is above every B pixel -- stacked, not swapped");
      check(a.minX < b.maxX && b.minX < a.maxX,
            "layout-grid: the fields overlap horizontally -- stacked, not side by side");
    }
  }

  // --- and the two icons are no longer drawn disabled ---------------------
  //
  // The one check that needs geometry. `drawAtelierTabStrip()` puts the icons
  // in the last two cells of the tab strip, each as wide as the band is tall;
  // the rectangles below are that same arithmetic, over the bands
  // `atelierLayout()` produces at the viewport ui/MacPaintUI hands it -- origin
  // (0,0) and the *full* logical size, not the ImGui work area, for the reason
  // that call site gives.
  //
  // What the pixels say, measured rather than reasoned from the tokens alone.
  // Disabled draws the outline in `kChromeBase` #2d2b2b over the band's own
  // `kChromeMid` #444141, and the icon cells of a *one-document* screenshot
  // read 65 with a redness of 3. Enabled and idle draws it in `kTextSecondary`
  // #9b9797 and a two-document one reads 151; the arrangement that is *on*
  // draws in `kAccent` #ff563c and reads a redness of 169, which is 255 - 86
  // exactly. Enabled-and-hovered is `kTextPrimary`, brighter still, so a mouse
  // that happens to rest on an icon cannot turn a pass into a failure. The
  // thresholds are 120 and 80, each in the middle of its own gap.
  const np::AtelierBands bands =
      np::atelierLayout(0.0f, 0.0f, logicalW, logicalH, /*showTabStrip=*/true);
  // No width means no scale factor, and no honest answer to give: say so
  // rather than dividing by zero and sampling wherever that lands.
  if (logicalW <= 0.0f) {
    std::fprintf(stderr, "[split-demo] the window reports %.0f logical pixels of width\n",
                 logicalW);
    return false;
  }
  const float scale = static_cast<float>(img.width) / logicalW;
  const float iconH = bands.tabStrip.h;
  const int onCell = mode == np::AtelierSplit::Columns ? 0 : 1;
  for (int i = 0; i < 2; ++i) {
    const float ix = bands.tabStrip.right() - 2.0f * iconH + static_cast<float>(i) * iconH;
    const np::AtelierRect cell{ix * scale, bands.tabStrip.y * scale, iconH * scale, iconH * scale};
    int maxGrey = 0, maxRedness = 0;
    splitDemoIconStats(img, cell, &maxGrey, &maxRedness);
    const char* name = i == 0 ? "columns-2" : "layout-grid";
    char what[128];
    if (i == onCell) {
      std::snprintf(what, sizeof(what), "%s is drawn pressed, in the accent (redness %d)", name,
                    maxRedness);
      check(maxRedness >= 80, what);
    } else {
      std::snprintf(what, sizeof(what),
                    "%s is enabled, not the disabled grey (brightest channel %d)", name, maxGrey);
      check(maxGrey >= 120, what);
    }
  }

  std::printf("[split-demo] %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  // --selftest [out.png] runs the solver headless and checks that latent-space
  // pigment mixing actually produces green where blue crosses yellow.
  const char* selfTestOut = nullptr;
  bool selfTest = false;
  float diagSeconds = 0.0f;
  bool modeTest = false;
  bool latencyVerbose = false;
  // --frame-trace: per-phase timing for every frame (poll/newframe/drawUI/
  // render/present) plus the active document's revision, printed to stderr,
  // and an on-screen frame counter + a marker circle at the pointer for a
  // couple of seconds after every mouse-button-down -- so a screen recording
  // of a real click can be lined up frame-for-frame against the log. Added to
  // chase a user report of ~2s between clicking a layer's eye icon and the
  // canvas visibly updating, which --profile-toggle's headless numbers
  // (~14-17ms for this exact PSD post-fix) do not explain; this measures the
  // real interactive path instead of guessing which part of it is slow.
  bool frameTrace = false;
  const char* screenshotPath = nullptr;
  int screenshotFrames = 30;
  bool penDemo = false;
  // Frame 4 so the UI has settled (the first frames are not representative --
  // see the screenshot counter's own comment), and 24 steps because that is a
  // stroke long enough to cross several tiles at the default brush radius.
  constexpr int kPenDemoFirstFrame = 4;
  constexpr int kPenDemoSteps = 24;
  // --transform-demo : raises the SAME request flag Cmd+T and Edit > Free
  // Transform both raise, so `--screenshot` photographs the real gizmo
  // arriving through the real entry path rather than a hand-built session
  // that could look right while the path to it was broken. Combines with
  // --demo-document, which gives it a layer to put a box around.
  bool transformDemo = false;
  // Optional layer index for --transform-demo. -1 means "whatever layer is
  // active", which is what a bare Cmd+T does. A number SELECTS that layer
  // first and then raises the same request -- so the flag stays one code
  // path with the keystroke rather than a second way in, and
  // `--transform-demo 0 --pen-demo` can put a transform on a layer that
  // HAS content while the pen tries to paint. That combination is a golden
  // case: it pins the rule that a live gizmo suppresses every paint route.
  int transformDemoLayer = -1;
  bool demoDocument = false;
  bool pigmentStrokeDemo = false;
  bool pigmentStrokeDemoMix = true;
  bool compsDemo = false;
  size_t compsDemoRestore = 0;
  bool compsDemoDrop = false;
  bool uiLayerDemo = false;
  bool marqueeDemo = false;
  bool flyoutDemo = false;
  bool panelStackDemo = false;
  bool uiLayerDemoClip = true;
  bool splitDemo = false;
  np::AtelierSplit splitDemoMode = np::AtelierSplit::Columns;
  const char* uiMergeDemo = nullptr;
  const char* uiMultiSelectDemo = nullptr;
  bool controlsAllOpen = false;
  const char* controlsScrollTo = nullptr;
  bool openLayerMenu = false;
  bool openExportStates = false;
  const char* exportStatesFolder = nullptr;
  bool openLayerProperties = false;
  // D4 (docs/reachability-audit.md): `naturalPaint foo.npaint` used to open
  // nothing, because this loop matched only `--flag` strings and fell
  // through every positional argument with no `else` to catch it -- silent,
  // like everything else this audit is about. Collected here rather than
  // opened on the spot: `openAnyFileAsDocument()` (app/OpenAnyFile.hpp) needs
  // `st.documents` and `st.recentDocuments`, neither of which exists yet this
  // early in main() -- the identical reason `--demo-document` and its
  // siblings above are flags collected here and applied later, next to where
  // the session's first document is created.
  //
  // **Multiple arguments each become their own document/tab**, opened in the
  // order given. That is the same reading `File > Open...`'s own multi-select
  // gives, and it is the one that cannot silently discard the second file the
  // way "use the first, ignore the rest" would -- a user who typed two names
  // on a command line meant both.
  //
  // **A name that collides with a flag is read as the flag.** `naturalPaint
  // --quit` opens no document named `--quit`; every branch above already
  // claims its own exact spelling first, and this only ever sees what none of
  // them matched. A flag string is not a legal path component on any
  // platform this project ships for without deliberately naming a file that
  // way, so the existing flags keep first claim on their own spellings rather
  // than this step inventing a `--` end-of-flags escape hatch nothing has
  // asked for yet.
  // --abr-report <file.abr> : import an .abr headlessly and print exactly what
  // survived the import, then exit. No SDL, no GPU, no window. This exists
  // because "the brushes do not feel right" has two very different causes --
  // the dynamics graph being misread, or the TIP never arriving at all -- and
  // guessing between them from a painted stroke is hopeless.
  const char* abrReportPath = nullptr;
  // --psd-report <file.psd> : import a .psd headlessly and print the layer
  // stack it produced, then exit. Same shape and same argument as
  // --abr-report, for the same reason: io/PsdImport was written with no
  // Photoshop-authored file to test against, so its own header records that
  // gap explicitly, and the only thing that closes it is comparing a real
  // document's panel against what the reader built from its bytes. The
  // failures that matter here are silent ones -- a stack read upside down,
  // every hidden layer shown, a blend key quietly downgraded -- and none of
  // them is visible from a canvas.
  const char* psdReportPath = nullptr;
  // --profile-toggle <file.psd> <layer-index> <iterations> : headless
  // benchmarking scaffold, see app/ProfileToggle.hpp. Temporary.
  const char* profileTogglePath = nullptr;
  int profileToggleLayer = -1;
  int profileToggleIterations = 50;
  // --brush-sheet <file.abr> <out.png> : paint every imported preset with
  // Photoshop's own preview stroke and write one contact sheet. Also headless.
  const char* brushSheetAbr = nullptr;
  const char* brushSheetOut = nullptr;
  const char* brushSheetExperiment = nullptr;
  // --stroke-preview <out.png> [radius] [spacing] : write the BRUSH EDITOR's
  // own TEST STROKE strip for the DEFAULT brush, at the exact size the panel
  // draws it. Headless, like the two above.
  //
  // **This exists because the panel's copy cannot be looked at from a test.**
  // `app/StrokePreview`'s whole claim is that it paints through the real
  // `StrokeSession` rather than drawing an impression of a stroke, and
  // `--selftest` asserts that numerically -- but "does this actually read as a
  // brush stroke at 288x96" is a question only an eye answers, and without
  // this flag answering it means launching the GUI, opening a panel and
  // photographing it. Same argument `--brush-sheet` already makes for the
  // sheet it writes.
  const char* strokePreviewOut = nullptr;
  float strokePreviewRadius = -1.0f;
  float strokePreviewSpacing = -1.0f;
  std::vector<std::string> positionalPaths;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a(argv[i]);
    if (a == "--selftest") {
      selfTest = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') selfTestOut = argv[++i];
    } else if (a == "--abr-report") {
      if (i + 1 < argc) abrReportPath = argv[++i];
    } else if (a == "--psd-report") {
      if (i + 1 < argc) psdReportPath = argv[++i];
    } else if (a == "--profile-toggle") {
      if (i + 1 < argc) profileTogglePath = argv[++i];
      if (i + 1 < argc) profileToggleLayer = std::atoi(argv[++i]);
      if (i + 1 < argc && argv[i + 1][0] != '-') profileToggleIterations = std::atoi(argv[++i]);
    } else if (a == "--brush-sheet") {
      if (i + 1 < argc) brushSheetAbr = argv[++i];
      if (i + 1 < argc) brushSheetOut = argv[++i];
      if (i + 1 < argc && argv[i + 1][0] != '-') brushSheetExperiment = argv[++i];
    } else if (a == "--stroke-preview") {
      if (i + 1 < argc) strokePreviewOut = argv[++i];
      if (i + 1 < argc && argv[i + 1][0] != '-') strokePreviewRadius = std::atof(argv[++i]);
      if (i + 1 < argc && argv[i + 1][0] != '-') strokePreviewSpacing = std::atof(argv[++i]);
    } else if (a == "--modes") {
      modeTest = true;
    } else if (a == "--diag") {
      // --diag [seconds] : run the solver headless and report where the
      // pigment goes over time.
      diagSeconds = 20.0f;
      if (i + 1 < argc && argv[i + 1][0] != '-') diagSeconds = std::atof(argv[++i]);
    } else if (a == "--latency") {
      // Prints a line per recorded sample, not just the per-stroke summary.
      latencyVerbose = true;
    } else if (a == "--frame-trace") {
      frameTrace = true;
    } else if (a == "--screenshot") {
      // --screenshot <path> [frames] : render, photograph the window into
      // <path>, and exit. See app/Screenshot.hpp for why the app captures
      // itself rather than being captured -- in short, every macOS route to
      // another process's window pixels is behind a permission that fails
      // *silently*, returning the desktop with all windows stripped out.
      //
      // The frame count exists because the first frames are not
      // representative: ImGui needs a frame to lay out docked panels, and the
      // GPU context reports its adapter on frame 0. The default settles both
      // without being slow enough to be annoying in a loop.
      if (i + 1 < argc && argv[i + 1][0] != '-') screenshotPath = argv[++i];
      if (i + 1 < argc && argv[i + 1][0] != '-') screenshotFrames = std::atoi(argv[++i]);
    } else if (a == "--demo-document") {
      // UI detour step 2, Part D: put content in the session's document, so a
      // --screenshot photographs the composite rather than photographing the
      // fact that a transparent one is invisible. See buildDemoDocument().
      demoDocument = true;
    } else if (a == "--pen-demo") {
      // --pen-demo : drag a synthetic pointer across the canvas through the
      // real UI, so the *interactive* stroke path is exercised rather than
      // `app::StrokeSession` being called directly. See the injection block in
      // the frame loop, and `preparePenDemo()` for the Pigment layer it needs
      // to have something to paint into.
      penDemo = true;
    } else if (a == "--pigment-stroke-demo") {
      // Phase 5, the CPU Pigment deposit: paint two Pigment layers with real
      // strokes so --screenshot photographs `Mix` against `Normal`. See
      // buildPigmentStrokeDemo(). `normal` is the comparison shot.
      pigmentStrokeDemo = true;
      if (i + 1 < argc && std::string_view(argv[i + 1]) == "normal") {
        pigmentStrokeDemoMix = false;
        ++i;
      }
    } else if (a == "--comps-demo") {
      // PLAN.md Phase 5 step 12: capture two comps of the current document and
      // restore the one named, so --screenshot can photograph the same
      // document in two comp states. See runCompsDemo().
      compsDemo = true;
      if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
        compsDemoRestore = static_cast<size_t>(std::atoi(argv[++i]));
      // `drop` deletes a layer between the capture and the restore, so the
      // restore is partial and the panel's report is on screen.
      if (i + 1 < argc && std::string_view(argv[i + 1]) == "drop") {
        compsDemoDrop = true;
        ++i;
      }
    } else if (a == "--marquee-demo") {
      // Phase 7: installs a rectangular selection so the marching ants and
      // the selected region can be photographed (app/Screenshot). There is no
      // other way to get a selection on screen without a human dragging one,
      // and a feature that can only be verified by a human is one that
      // silently rots -- the same argument app/Screenshot.hpp makes for
      // photographing the swapchain at all.
      marqueeDemo = true;
    } else if (a == "--flyout-demo") {
      // sidequest/lucide-toolbox, the nested-flyout revision: holds the
      // Brush group's flyout open (four members, Brush/Pencil implemented-
      // and-not-implemented mixed with Water/DryBrush, the best subject the
      // user's own instruction names) so --screenshot can photograph it --
      // a flyout opens on right-click or a ~350ms press-and-hold, and the
      // screenshot path has neither. See AppState::openToolFlyoutDemo.
      flyoutDemo = true;
    } else if (a == "--panel-stack-demo") {
      // The dockable-panel revision's tab stacks: puts HISTOGRAM and GRADE
      // into COLOR's slot as tabs, and COMPS into the collapsed HISTORY's, so
      // --screenshot can photograph both an OPEN tab strip and a COLLAPSED
      // one. A stack is made by dragging a panel onto another, and the
      // screenshot path has no drag -- the same gap --flyout-demo exists to
      // cover for a press-and-hold. See `st.panelStackDemo`.
      panelStackDemo = true;
    } else if (a == "--ui-layer-demo") {
      // UI detour step 3: build a stack through the layer editor's own
      // commands. See runUiLayerDemo(). `noclip` runs the same script without
      // the clip, which is the comparison shot for PRD C9 on screen.
      uiLayerDemo = true;
      if (i + 1 < argc && std::string_view(argv[i + 1]) == "noclip") {
        uiLayerDemoClip = false;
        ++i;
      }
    } else if (a == "--split-demo") {
      // PLAN.md Phase 5 step 14 / PRD A5: open a second document and press one
      // of the tab strip's two split icons, so --screenshot can photograph the
      // two-pane canvas -- the half of step 14 that shipped compile-verified
      // only. See buildSplitDemo(), and verifySplitDemoScreenshot() for what is
      // then asserted about the picture.
      //
      // `rows` selects `layout-grid` (stacked) instead of `columns-2` (side by
      // side). Spelled `rows` rather than `layout-grid` because that is what
      // ui/AtelierLayout's enumerator is called and the design's own two icon
      // names are, by that header's admission, an interpretation.
      splitDemo = true;
      if (i + 1 < argc && std::string_view(argv[i + 1]) == "rows") {
        splitDemoMode = np::AtelierSplit::Rows;
        ++i;
      }
    } else if (a == "--ui-merge-demo") {
      // Phase 5 step 10 / PRD C10: press one merge button. See runUiMergeDemo().
      if (i + 1 < argc && argv[i + 1][0] != '-') uiMergeDemo = argv[++i];
    } else if (a == "--transform-demo") {
      transformDemo = true;
      if (i + 1 < argc && argv[i + 1][0] != '-')
        transformDemoLayer = std::atoi(argv[++i]);
    } else if (a == "--ui-multiselect-demo") {
      // PLAN.md Phase 5 step 11 / PRD C12, C13, C15: press the multi-selection's
      // own set commands. See runUiMultiSelectDemo().
      if (i + 1 < argc && argv[i + 1][0] != '-') uiMultiSelectDemo = argv[++i];
    } else if (a == "--controls-all-open") {
      // UI detour step 3: open every collapsing header in the controls column
      // on the first frame, so --screenshot can photograph a section the
      // default state closes. An optional section title (LAYERS, PIGMENT,
      // MEDIUM, ...) additionally pins that header to the top of the column,
      // which is the only way to photograph a section that is below the fold
      // once every one of them is open. See AppState::controlsAllOpen.
      controlsAllOpen = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') controlsScrollTo = argv[++i];
    } else if (a == "--open-layer-menu") {
      // UI detour step 3: hold the `Layer` menu open so --screenshot can
      // photograph it. See AppState::openLayerMenu.
      openLayerMenu = true;
    } else if (a == "--open-export-states") {
      // Phase 5 step 13: hold File > Export Comps / Layers To Files... open so
      // --screenshot can photograph it. See AppState::openExportStatesDialog.
      openExportStates = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') exportStatesFolder = argv[++i];
    } else if (a == "--open-layer-properties") {
      // The LAYERS panel's own gear-button modal, same justification as
      // --open-export-states one dialog over: it too is opened by a click and
      // --screenshot has no input. See AppState::openLayerProperties.
      openLayerProperties = true;
    } else if (np::looksLikePositionalArgument(a)) {
      // D4: the one case none of the branches above matched -- a bare
      // filename. See this loop's own comment, just above it, and
      // `looksLikePositionalArgument()`'s (app/OpenAnyFile.hpp) for why it is
      // collected here and why a name that collides with a flag is read as
      // the flag: this predicate is only ever consulted for what none of the
      // exact-spelling branches above it matched.
      positionalPaths.emplace_back(a);
    }
    // An argument that starts with '-' and matches none of the flags above
    // falls through here unchanged -- pre-existing behaviour this step does
    // not touch, and out of scope for D4, which is about a bare filename
    // opening nothing, not about diagnosing an unrecognised flag.
  }

  // Before SDL, deliberately: an .abr is a file and a parser, and nothing in
  // the report needs a device, a window or a surface. Putting it here means it
  // runs in milliseconds on a headless box and cannot be perturbed by anything
  // the GPU path does.
  if (abrReportPath != nullptr) return np::runAbrReport(abrReportPath);
  if (psdReportPath != nullptr) return np::runPsdReport(psdReportPath);
  if (profileTogglePath != nullptr)
    return np::runProfileToggle(profileTogglePath, profileToggleLayer, profileToggleIterations);
  if (brushSheetAbr != nullptr && brushSheetOut != nullptr)
    return np::runBrushSheet(brushSheetAbr, brushSheetOut, brushSheetExperiment);
  if (strokePreviewOut != nullptr)
    return np::runStrokePreviewDump(strokePreviewOut, strokePreviewRadius, strokePreviewSpacing);

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window* window = SDL_CreateWindow(
      "naturalPaint", 1480, 940,
      SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_METAL);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return 1;
  }

  // ---- the OS file panel's parent (ui/FileDialog) -------------------------
  //
  // Not cosmetic, and ui/FileDialog.hpp says why: with a window, SDL's cocoa
  // backend presents the panel as a *sheet* and returns immediately; without
  // one it falls back to `[dialog runModal]`, which is application-modal and
  // blocks the calling thread -- i.e. it would stop the render loop below
  // dead for as long as the panel is up.
  np::setFileDialogParentWindow(window);

  // ---- the native menu bar (ui/MacNativeMenu, ui/MenuModel) ---------------
  //
  // A no-op off Apple, where the ImGui menu bar in the title band is the whole
  // story and stays exactly as it was.
  //
  // **Here, and not one line earlier.** SDL builds the standard macOS
  // application menu inside `SDL_Init(SDL_INIT_VIDEO)` above, and it does so in
  // the same branch where it installs its own `NSApplication` subclass -- the
  // subclass whose `terminate:` override is the only reason ⌘Q becomes an
  // `SDL_EVENT_QUIT`, and therefore the only reason ⌘Q reaches
  // `AppState::requestQuit` and the unsaved-work guard below rather than
  // killing the process outright. Touching `NSApp` before SDL does would
  // realise a plain `NSApplication` instead, SDL's branch would never run, and
  // ⌘Q would silently discard every open document. ui/MacNativeMenu.mm's header
  // comment has the citation.
  //
  // It is also after `SDL_CreateWindow()`, which is not required by anything
  // but keeps the whole of the platform's window setup in one place and means
  // a failed window is never behind a menu bar advertising commands for it.
  np::installNativeMenuBar();

  // ---- raw two-finger trackpad touch capture (ui/MacTrackpadTouch) --------
  //
  // item 4: bypasses AppKit's own magnify/rotate gesture classifier for the
  // canvas's two-finger zoom/rotate/pan. Same placement reasoning as
  // `setFileDialogParentWindow()` above: needs the real `SDL_Window*`,
  // which only exists once `SDL_CreateWindow()` has returned. A no-op off
  // Apple; a failure here (see ui/MacTrackpadTouch.mm's own stderr
  // message) degrades gracefully to the existing AppKit-classified
  // pinch/rotate path in ui/MacPaintUI.cpp, so nothing worse than "the old
  // behaviour" can result.
  np::installTrackpadTouchCapture(window);

  np::GpuContext gpu;
  if (!gpu.init(window)) return 1;

  np::MixboxLut lut;
  const std::string mixboxLutPath = np::mixboxLutPath();
  if (!lut.load(mixboxLutPath)) {
    std::fprintf(stderr, "Could not load the Mixbox LUT. Expected it at:\n  %s\n",
                 mixboxLutPath.c_str());
    return 1;
  }

  // 1.4 / ADR-0001: the true "idle" measurement -- SDL, the window and the
  // WebGPU device/surface all exist, but nothing sim-shaped does yet.
  // Captured here, once, before any of the branches below (including
  // --selftest itself) construct a PaintSim, so --selftest's idle-RSS
  // assertion checks a real "before any heavy subsystem exists" number
  // rather than one taken after a sim it constructs eagerly for its own
  // purposes.
  const size_t idleRssBytes = np::currentResidentBytes();

  // Null until something actually needs the solver. --selftest/--diag/
  // --modes exist specifically to exercise it, so they construct it via
  // ensurePaintSim() immediately below, same as before this change; the
  // interactive path leaves it null and defers construction to MacPaintUI's
  // canvas (see drawUI's doc comment) so idle RSS with nothing painted stays
  // near zero rather than paying for the sim on every launch.
  std::unique_ptr<np::PaintSim> sim;

  if (modeTest) {
    np::PaintSim* s = np::ensurePaintSim(sim, gpu, kCanvasW, kCanvasH, lut);
    if (!s) return 1;
    np::runModeTest(gpu, *s, lut, "mode");
    s->shutdown(); gpu.shutdown();
    SDL_DestroyWindow(window); SDL_Quit();
    return 0;
  }

  if (diagSeconds > 0.0f) {
    np::PaintSim* s = np::ensurePaintSim(sim, gpu, kCanvasW, kCanvasH, lut);
    if (!s) return 1;
    np::runDiagnostic(gpu, *s, lut, diagSeconds, "np");
    s->shutdown();
    gpu.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
  }

  if (selfTest) {
    np::PaintSim* s = np::ensurePaintSim(sim, gpu, kCanvasW, kCanvasH, lut);
    if (!s) return 1;
    // 1.4 / ADR-0001 bullets 2 and 3: right after init(), still in the
    // default Watercolour mode, confirms the ink lattice / oil brush grid
    // are genuinely absent -- then cycles setMode() through all three media
    // and confirms each outgoing medium's fields actually get freed, not
    // just the incoming one's allocated.
    const bool fieldAllocOk = np::runFieldAllocationTest(gpu, *s);
    const bool pigmentOk = np::runSelfTest(gpu, *s, lut,
                                    selfTestOut ? selfTestOut : "selftest.png");
    // Headless, GPU-free — doesn't need sim/gpu at all, but runs from the
    // same --selftest entry point since it's the same "does the solver
    // still behave" gate a CI run would check.
    const bool accumulatorOk = np::runAccumulatorTest();
    // 2.3: color/Space's sRGB/Rec.709 transfer function round trip. Also
    // headless and GPU-free -- pure CPU math, no PaintSim involvement.
    const bool colorSpaceOk = np::runColorSpaceTest();
    // Phase 3 step 1 (ADR-0004): color/Shaper's ACEScct log encode/decode --
    // breakpoint continuity, round trip, a hand-computed known-value check,
    // and monotonicity. Also headless and GPU-free -- pure CPU math, no
    // PaintSim involvement.
    const bool shaperOk = np::runShaperTest();
    // Phase 2 step 15: app/Keymap load, conflict detection and resolve().
    // Headless, GPU-free -- pure CPU/file-IO, no PaintSim involvement.
    const bool keymapOk = np::runKeymapTest();
    // UI detour: ui/Fonts -- ImGui's built-in ProggyClean holds no glyph above
    // U+00FF, so six of docs/ui.md 3.2's seven layer-kind glyphs could not be
    // drawn at all. Headless and GPU-free: it bakes a real font atlas on the
    // CPU and asks it, per codepoint, rather than asking what a function
    // returns -- which is the question nine other sections already ask.
    const bool fontsOk = np::runFontsTest();
    const bool atelierOk = np::runAtelierChromeTest();
    const bool activeLayerOk = np::runActiveLayerTest();
    // The solver-to-document mass mapping, the arithmetic half of the
    // stroke bridge. Pure CPU -- it is a claim about numbers.
    const bool pigmentBakeOk = np::runPigmentBakeTest();
    // docs/reachability-audit.md's B1: does a baked Pigment layer survive
    // saveNpaint()/loadNpaint()? Pure CPU -- it bakes from hand-built solver
    // output, same as pigmentBakeOk above, and needs no GPU either.
    const bool solverPersistenceOk = np::runSolverPersistenceTest();
    const bool strokeBridgeOk = np::runStrokeBridgeTest(gpu);
    // The presentation transfer function, from a linear value in a layer to a
    // byte in a screenshot -- the one edge no section covered, which is why the
    // chrome's measured darkening had no explanation. Needs the GPU.
    const bool presentTransferOk = np::runPresentTransferTest(gpu);
    // Phase 2 step 2: core/Half's shared half<->float codec and
    // core/TileStore's allocate-on-write / query-without-allocating /
    // iterate-occupied sparse map. Also headless and GPU-free -- pure CPU,
    // no PaintSim involvement.
    const bool tileStoreOk = np::runTileStoreTest();
    // Phase 2 step 6 (decode half): io/ImageDecode's PNG/JPEG/TGA/BMP -> linear
    // float RGBA path. Also headless and GPU-free -- pure CPU decode, no
    // PaintSim involvement.
    const bool imageDecodeOk = np::runImageDecodeTest();
    // Phase 2 step 4: core/Document + core/Layer -- one-entry layer list,
    // LayerKind's seven CONTEXT.md values, and the RGB layer's tile storage
    // round-trip. Also headless and GPU-free -- pure CPU, no PaintSim
    // involvement.
    const bool documentOk = np::runDocumentTest();
    // Phase 2 step 14 (PRD C16): the base layer is an ordinary layer with
    // alpha, no locked Background -- core/Layer.hpp has no such concept at
    // all, so this proves the property rather than leaving it assumed. Also
    // headless and GPU-free -- pure CPU, no PaintSim involvement.
    const bool baseLayerAlphaOk = np::runBaseLayerAlphaTest();
    // Phase 2 step 5: Document::createBlank() -- given size/working-space,
    // exactly one RGB-kind layer, and zero tiles allocated even for a large
    // canvas. Also headless and GPU-free -- pure CPU, no PaintSim
    // involvement.
    const bool createBlankOk = np::runCreateBlankTest();
    // Phase 2 step 6 (the remaining half): io/ImageIO -- premultiply + pack
    // a decoded image into a Document's tiles, on top of createBlank() and
    // ImageDecode. Also headless and GPU-free -- pure CPU, no PaintSim
    // involvement.
    const bool imageIOOk = np::runImageIOTest();
    // Phase 2 step 13 (narrow, Document-level slice; PRD I14): io/ImageIO's
    // placeImageAsLayer() -- append an image as a new top layer onto an
    // already-open Document, distinct from openImageAsDocument() creating a
    // brand-new one. Also headless and GPU-free -- pure CPU, no PaintSim
    // involvement.
    const bool placeImageAsLayerOk = np::runPlaceImageAsLayerTest();
    // Phase 2 step 9: ui/NaturalPaintUI's mip pyramid -- CPU-side box-filter
    // downsample correctness, the zoom->level formula, a pure-geometry
    // check on tileScreenRect(), and an end-to-end GPU proof that mip-level
    // selection actually changes which texels render. Needs `gpu` for the
    // end-to-end part only -- see SelfTest.hpp.
    const bool mipPyramidOk = np::runMipPyramidTest(gpu);
    // Phase 2 step 10 (narrow, Document-level slice; PRD Q10): core/Probe's
    // probePixel() -- linear + display readout, NxN sample-size averaging,
    // sample-all-layers as a parameter of the sample rather than a separate
    // tool, and premultiply-aware un-premultiplication on read. Also
    // headless and GPU-free -- pure CPU, no PaintSim involvement.
    const bool probeOk = np::runProbeTest();
    // Phase 2 step 10's other half (PRD Q10 and L4, both P0): the eyedropper
    // tool itself -- three sample sources over a stack built so they must
    // disagree, box clipping at the document edge, the foreground colour a
    // pick writes into and whether that colour reaches a stroke, and the
    // implemented-vs-has-a-handler tripwire that would have caught the two
    // phases in which `Tool::Eyedropper` claimed to be built and was not.
    // Headless and GPU-free -- pure CPU, no PaintSim involvement.
    const bool eyedropperOk = np::runEyedropperTest();
    // Phase 2 step 11 ("View controls", PRD Q1-Q4): the unified view
    // transform's round-trip identity, one hand-worked known-point check,
    // and the view-only proof that mirror/rotation/grayscale never mutate
    // PaintSim's own canvas texture. Needs `gpu`/`*s` only for that last
    // part -- see SelfTest.hpp for the full breakdown.
    const bool viewTransformOk = np::runViewTransformTest(gpu, *s);
    // Phase 2 step 12 ("Rulers, guides, grid and snapping", PRD Q5-Q7):
    // app/Snapping.hpp's pure math -- grid spacing/subdivision line
    // positions, the numeric/percentage guide-position parser, and the
    // snapping resolution function against hand-computable cases. Also
    // headless and GPU-free -- rulers/drag-to-create/the popup/the grid
    // overlay itself are UI with no headless driver; see SelfTest.hpp.
    const bool guidesGridSnapOk = np::runGuidesGridSnapTest();
    // Phase 3 step 7 ("Histogram over the visible region"): core/Histogram's
    // computeHistogram() -- display-domain R/G/B/Luma bin placement,
    // alpha<=0 texels excluded, region clipping against the tile store's
    // own allocated-tile iteration, and un-premultiply-before-bin on a
    // translucent pixel. Also headless and GPU-free -- pure CPU, no
    // PaintSim involvement.
    // docs/architecture-review.md P0-1: the hardware half convert against the
    // software routine it replaced, swept exhaustively in one direction and
    // structurally in the other. Headless and GPU-free.
    const bool halfOk = np::runHalfTest();
    const bool histogramOk = np::runHistogramTest();
    // Phase 3 steps 2+3 (ops/PointOps; docs/operations.md §1.1; PRD B4):
    // Levels, Curves (through color/Shaper), Exposure, Saturation,
    // RGB->grayscale, channel mixer -- each a plain rgb->rgb function --
    // plus the un-premultiply/re-premultiply wrapper bracketing them.
    // Also headless and GPU-free -- pure CPU math, no PaintSim involvement.
    const bool pointOpsOk = np::runPointOpsTest();
    // ops/ToneOps (docs/operations.md §1.2 "Committed additions"): four more
    // pure rgb->rgb point ops -- gain/offset/gamma, invert (both domains,
    // each an involution), posterize and threshold (both shaper-domain).
    // Also headless and GPU-free -- pure CPU math, no PaintSim involvement.
    const bool toneOpsOk = np::runToneOpsTest();
    // ops/ColorOps (docs/operations.md §1.2): hue/saturation/lightness (a
    // Rodrigues rotation about the normalised Rec.709 luma axis, so luma is
    // preserved exactly rather than approximately), vibrance, colour balance
    // by tonal range, and photo filter. Also headless and GPU-free.
    const bool colorOpsOk = np::runColorOpsTest();
    // ops/MonoOps (docs/operations.md §1.2): Black & white's six hue-wheel
    // weights and Gradient map's luma->ramp lookup, both reusing
    // ops/PointOps.hpp's `rgb -> rgb` contract and ops/Gradient.hpp's stop
    // model rather than inventing either afresh. Also headless and
    // GPU-free -- pure CPU math, no PaintSim involvement.
    const bool monoOpsOk = np::runMonoOpsTest();
    // ops/AutoLevels (docs/operations.md §1.2): auto-tone, auto-contrast,
    // auto-white-balance and equalize -- "a parameter solver, not an op", so
    // each inspects a core::HistogramResult and returns parameters for an
    // ordinary editable Levels or Curves op rather than touching a pixel.
    // Headless and GPU-free -- hand-built histogram fixtures, no Document.
    const bool autoLevelsOk = np::runAutoLevelsTest();
    // Phase 6 (ops/Gradient; PRD D24 and the gradient half of D26): linear,
    // radial and angular geometries, the independently-positioned colour and
    // opacity stop lists, straight-colour interpolation in linear light, and
    // the coverage-weighted render through an optional selection -- including
    // the null-selection-means-everywhere case through the op's own hoisted
    // loop. The op only; the editor and presets are not built. Also headless
    // and GPU-free -- pure CPU tile arithmetic, no PaintSim involvement.
    const bool gradientOk = np::runGradientTest();
    // PLAN.md "Phase 6 -- Filter and transform it" (DESIGN-imaging.md class B;
    // PRD E4): ops/Roi's backwards ROI propagation, ops/Blur's separable
    // Gaussian and box over a TileStore -- including the tile-seam property,
    // asserted bit-for-bit and then proved sensitive against the tile-local
    // blur it rejects -- and ops/Feather. Also headless and GPU-free.
    const bool blurOk = np::runBlurTest();
    // ops/Blur.cpp's blurPlane() CPU cost: channel-vectorised convolveLine4/
    // boxLine4 and the threaded row/column loop above blurTiles()'s own
    // already-threaded tile loops. Headless and GPU-free.
    const bool blurSimdOk = np::runBlurSimdTest();
    // PLAN.md "Phase 6 -- Filter and transform it": ops/Filters -- the filter
    // set that hangs off the blur spine. Highpass as `src - blur(src)`,
    // unsharp with amount/radius/threshold, sharpen, offset with wrap, add
    // noise and local contrast. The seam property is re-asserted for all six
    // (add noise earns it with a counter-based PRNG rather than inheriting it
    // from an apron), the premultiplied-alpha rule at a soft edge is asserted
    // against the RGB-only form it rejects, and both of PLAN.md's domain traps
    // for this phase are measured: shaper-domain noise is constant across six
    // stops where linear-light noise varies by 234x, and a log-domain local
    // contrast cannot produce the negative light a linear unsharp does. Also
    // headless and GPU-free.
    const bool filtersOk = np::runFiltersTest();
    // ops/Filters sections 7-9: emboss, median/despeckle, motion blur -- three
    // filters extending the set above, each earning the tile-seam property a
    // different way. See app/SelfTest.hpp's own comment on
    // runFiltersExtTest() for the shape of the argument. Also headless and
    // GPU-free.
    const bool filtersExtOk = np::runFiltersExtTest();
    // PLAN.md "Phase 7 -- Select and paste" (PRD E1, E2, M1): core/SelectionMask's
    // uint8 coverage store, its antialiased rectangle constructor, and the
    // coverage-weighted clear. Also headless and GPU-free -- pure CPU tile
    // arithmetic, no PaintSim involvement.
    const bool selectionOk = np::runSelectionTest();
    // PLAN.md "Phase 7 -- Select and paste" (PRD E11, E12, E13): core/Channels'
    // named coverage channels in the document, the exact selection<->channel
    // round trip, saved selections, quick mask, and io/NpaintFile's `S####`
    // part -- including that a document written with no channels still loads.
    // Mostly headless; the format sections write a real `.npaint` to disk.
    const bool channelsOk = np::runChannelsTest();
    // ops/FloodFill (PLAN.md "Phase 6" paint bucket + "Phase 7" magic wand;
    // PRD D25, D26, E2, E3): the display-encoded tolerance metric, the derived
    // antialiased coverage ramp, the scanline traversal that pages through the
    // tile store, and the fill that goes through the selection the wand
    // produces. Also headless and GPU-free.
    const bool floodFillOk = np::runFloodFillTest();
    // core/SelectionShapes (PRD E3): the ellipse, lasso and polygon lasso, and
    // the exact-area claim behind all three. Headless, pure CPU.
    const bool selectionShapesOk = np::runSelectionShapesTest();
    // core/SelectionRefine (PRD E8, E9): grow and shrink through a signed
    // distance field seeded at sub-texel accuracy from the coverage itself,
    // and the two range selections, which reuse ops/FloodFill's tolerance
    // metric rather than inventing a second one. Headless, pure CPU.
    const bool selectionRefineOk = np::runSelectionRefineTest();
    // ui/MacPaintUI's commitDrawnSelection(): the intent rules all five of
    // PRD E3's selection tools funnel through. Headless, pure CPU.
    const bool selectionToolsOk = np::runSelectionToolsTest();
    // docs/testing-issues.md T10: app/SelectionDrag.hpp's pure geometry
    // behind Shift-constrain, Option-from-centre and Space-move on the
    // rectangle/ellipse marquee drag. Headless, pure CPU.
    const bool selectionDragOk = np::runSelectionDragTest();
    // docs/testing-issues.md T13: app/SelectionDrag.hpp's
    // ellipseMarqueePreviewPoints() -- the ellipse marquee's live preview
    // agrees with what the commit path actually builds, for the same box.
    // Headless, pure CPU.
    const bool ellipseMarqueePreviewOk = np::runEllipseMarqueePreviewTest();
    // core/SelectionBoundary (PRD E6): the true outline the marching ants draw
    // -- islands, holes and concave corners -- which replaced the bounding box
    // that made every lasso, wand and Shift-add selection render as a
    // rectangle. Includes the 0.5 coverage threshold, the revision-keyed cache
    // and its invalidation, and the extraction's cost against PRD F3's 20 ms.
    // Headless, pure CPU.
    const bool selectionBoundaryOk = np::runSelectionBoundaryTest();
    // PLAN.md "Phase 6 -- Filter and transform it" (PRD D14-D17): ops/Transform's
    // 3x3 matrix stack composed BEFORE resampling, the exact no-resample paths
    // for flips and quarter turns, the five reconstruction kernels, the
    // area-average prefilter a downscale must run first, and crop/canvas
    // size/image size. Also headless and GPU-free -- pure CPU resampling.
    const bool transformOk = np::runTransformTest();
    // PLAN.md "Phase 6" again, one level up (PRD D14, D16, D17, E10):
    // ops/DocumentTransform, the Document/Layer entry point to that resampler.
    // A crop moving masks and selections with the pixels (a Layer has no offset
    // field, so its tile keying IS its offset); pigment layers transformed
    // mass-weighted through a kernel with no negative lobes, enforced by type;
    // and D16 asserted at document level -- a stack of any depth resamples once.
    // Headless and GPU-free.
    const bool documentTransformOk = np::runDocumentTransformTest();
    // docs/reachability-audit.md C1: app/TransformSession, the pure session
    // behind an interactive layer/selection transform -- handles, hit-testing,
    // drag semantics, and commit through ops/DocumentTransform's own resample
    // and app/DocumentLifecycle's recordEdit() undo funnel. Headless and
    // GPU-free.
    const bool transformSessionOk = np::runTransformSessionTest();
    // docs/testing-issues.md T14: the CPU half of the Free Transform live
    // pixel preview -- ui/TransformPreviewTexture's crop-and-pack, headless
    // and GPU-free (the GPU upload wrapper itself is untested, matching this
    // suite's own precedent for DabPreviewTexture/StrokePreviewTexture).
    const bool transformPreviewTextureOk = np::runTransformPreviewTextureTest();
    // PLAN.md "Phase 7 -- Select and paste" (PRD M1, M3, M4, M5, M8): the
    // internal clipboard's copy/cut/paste, its copy-on-write sharing, and the
    // two different coverage-weighting rules RGB and Pigment tiles take. Also
    // headless and GPU-free.
    const bool clipboardOk = np::runClipboardTest();
    // Phase 3 step 5 ("core/OpStack -- ordered ops, dirty tracking, run
    // detection for the collapse"): OpStack's add/remove/reorder/setEnabled/
    // setOp mutators and version() bumping, plus detectRuns()'s maximal-run
    // grouping over docs/operations.md's four op classes -- including the
    // "a disabled PointA entry does not split a run" rule. Also headless and
    // GPU-free -- pure CPU bookkeeping plus calls into the already-tested
    // ops/PointOps functions, no PaintSim involvement.
    const bool opStackOk = np::runOpStackTest();
    // Phase 3 step 4 (color/LutBake, ADR-0004): bakes a maximal run of
    // adjacent point ops onto a 32^3 rgba16float 3-D LUT via a seed compute
    // dispatch plus one dispatch per op, and checks the baked result
    // against the CPU ops/PointOps reference at hand-picked grid cells.
    // Needs `gpu` for a real device/queue -- genuine compute-shader work,
    // no PaintSim involvement.
    const bool lutBakeOk = np::runLutBakeTest(gpu);
    // Phase 3 step 6 ("Apply pass -- shaper -> 3-D LUT fetch -> un-
    // shape"): sim::PaintSim::updateGradePreview()'s bake-gate/blit
    // pipeline against the live simulation canvas, checked against an
    // independent CPU trilinear-interpolation reference at hand-picked
    // canvas pixels, plus the version-gating rebake proof. Needs `*s` for
    // a real PaintSim -- the same shared instance every other PaintSim-
    // backed --selftest case in this chain already uses.
    const bool applyPassOk = np::runApplyPassTest(gpu, *s);
    // docs/architecture-review.md P0-5: core/Composite.cpp's adjustment-
    // layer walk no longer calls a std::function per op per pixel -- it
    // calls core::applyOpDirect()'s switch (core/OpStack.hpp) instead.
    // Regression-checks the switch against the untouched closure path,
    // measures a from-scratch CPU-only LUT-accuracy question the review's
    // own suggested fix raised, and prints before/after timing. Headless
    // and GPU-free.
    const bool gradeDispatchOk = np::runGradeDispatchTest();
    // Phase 3 step 8 ("Op-stack UI... and a curve widget operating in the
    // shaper domain"): app/CurveEdit.hpp's pure screen<->curve-space
    // geometry and list-mutation math -- everything the interactive curve
    // widget (ui/MacPaintUI.cpp) calls into. Also headless and GPU-free --
    // pure CPU, no PaintSim involvement.
    const bool curveEditOk = np::runCurveEditTest();
    // The brush dynamics link model (design "naturalPaint Panels" turn 4a):
    // range semantics, invert, curve clamping, and the commutative fold that
    // lets three sources drive one target. Headless and GPU-free.
    const bool brushDynamicsOk = np::runBrushDynamicsTest();
    // A6 (docs/reachability-audit.md): the four sources that used to be
    // hard 0.0 (VELOCITY, FADE, NOISE, RANDOM), the six targets nothing
    // read, and the determinism the two stochastic sources must hold under
    // a replayed stroke. Headless and GPU-free.
    const bool dynamicsSourcesOk = np::runDynamicsSourcesTest();
    // The BRUSH EDITOR's tip preview (app/DabPreview): the rasterised dab
    // checked against a real depositDab(), plus the elliptical tip that
    // building it found missing. Headless and GPU-free.
    const bool dabPreviewOk = np::runDabPreviewTest();
    // io/AbrBrushes: Photoshop `.abr` libraries into brush/Library presets --
    // the container framing and the parameter mapping, including what an
    // import could NOT bring across. Headless and GPU-free.
    const bool abrBrushesOk = np::runAbrBrushesTest();
    // docs/architecture-review.md P2-2 item 1: io/AbrBrushes.hpp's
    // `checkedAdd()`, driven at SIZE_MAX/SIZE_MAX-1 -- a boundary none of the
    // parser's own call sites reach. Headless and GPU-free.
    const bool checkedAddOk = np::runCheckedAddTest();
    // docs/reachability-audit.md B6 and B7: a Multiply target's floor applied
    // exactly once, downstream of both halves of app/StrokeSession's
    // hardware/stroke-local split, rather than once per contributing link or
    // once per half. Headless and GPU-free.
    const bool multiplyFloorOk = np::runMultiplyFloorTest();
    // app/StrokeSession's applyPerDabScatter(): docs/reachability-audit.md
    // B5's axis defect. Checks the geometry directly -- tangent component
    // ~0, perpendicular component real -- rather than a flag. Headless and
    // GPU-free.
    const bool scatterOk = np::runScatterTest();
    // io/AbrBrushes' `samp` block: sampled bitmap tips decoded and stamped by
    // brush/Deposit.hpp §2c in place of the procedural tip. Headless and
    // GPU-free.
    const bool abrSampledTipsOk = np::runAbrSampledTipsTest();
    // track10/angle: an independent geometric pin -- BrushTip::angle is
    // clockwise-positive on screen, and DIRECTION->Angle actually faces the
    // tip along the stroke's travel vector. Headless and GPU-free.
    const bool angleConventionOk = np::runAngleConventionTest();
    // io/AbrBrushes' Dual Brush support: a second tip composited into
    // brush/Deposit.hpp §2d's dabCoverage() by Multiply/Overlay, and what an
    // import still cannot honour (an unsupported blend mode, the second tip's
    // own spacing/scatter/count). Headless and GPU-free.
    const bool abrDualBrushOk = np::runAbrDualBrushTest();
    // app/BrushLibraryFile: the preferences file for imported `.abr` libraries,
    // the row cache that makes launch pay nothing for them, and unload.
    // Headless and GPU-free.
    const bool brushLibraryFileOk = np::runBrushLibraryFileTest();
    // app/UserBrushLibrary: the presets a user made, persisted to their own
    // file (PRD G6, A7) -- the round trip of a full BrushLinkSet, the fork
    // Save performs on a built-in, and a simulated mid-write crash. Headless
    // and GPU-free.
    const bool userBrushLibraryOk = np::runUserBrushLibraryTest();
    // Phase 4 step 1 ("Export path -- encode from working space to a chosen
    // target space and bit depth, explicitly, never silently"; PRD B6, I5,
    // I1): io/Export's flatten -> un-premultiply -> encode -> quantize ->
    // write pipeline -- a real 16-bit PNG round trip back through
    // decodeImageLinear(), the 8-vs-16-bit precision difference demonstrated
    // rather than assumed, unsatisfiable depth requests failing with their
    // error strings actually inspected, the three target spaces proven
    // distinct, and the primaries-mismatch refusal. Also headless and
    // GPU-free -- pure CPU, no PaintSim involvement.
    const bool exportOk = np::runExportTest();
    // Phase 4 steps 2+3 ("io/OiioBackend behind NP_USE_OIIO -- EXR, TIFF,
    // HDR, DPX, flattened PSD, camera raw"; "Capability query -- format
    // support is discovered at runtime; the core builds and runs without
    // OIIO"; PRD I3, I1, B6): io/Capabilities' runtime query answered for
    // every format, EXR/TIFF/DPX/HDR round trips and a hand-built flattened
    // PSD read through the OpenImageIO backend, camera raw reported
    // unsupported even in the OIIO build, and the loud named refusals in
    // the build that has no backend at all. Runs -- and asserts the correct
    // answers -- in BOTH NP_USE_OIIO configurations; see SelfTest.hpp on
    // why compiling it out of the OFF build would defeat its purpose. Also
    // headless and GPU-free -- pure CPU, no PaintSim involvement.
    const bool formatSupportOk = np::runFormatSupportTest();
    // Phase 4 step 4 ("Native `.npaint` save and load -- multi-part tiled
    // EXR via OIIO"; docs/document-format.md; PRD I4, I5b, I6, I7, I8, I10,
    // I11, I12): io/NpaintFile's document round trip -- every layer's tiles
    // bit-identical at zero tolerance, all seven per-layer np:* attributes
    // at their exact values, part 0 proven regenerated rather than stale,
    // unrecognised attributes and a whole foreign part surviving two
    // generations verbatim, and the lossy-compression and lost-data
    // refusals. Runs -- and asserts the correct answers -- in BOTH
    // NP_USE_OIIO configurations. Headless and GPU-free, but the one
    // section that writes real scratch files, since a document format is a
    // file format; it removes every one of them.
    const bool npaintOk = np::runNpaintFormatTest();
    // Phase 4 step 5 ("Wire OIIO's `ImageCache` as the residency layer for
    // unmodified source tiles. This is the main reason the dependency earns
    // its cost"; ADR-0001; docs/document-format.md §1): io/TileResidency's
    // two strategies behind one interface -- a 2048x2048 .npaint served both
    // eagerly and from the cache with all 256 tiles compared bit for bit at
    // zero tolerance, resident bytes and fetch cost printed for both,
    // eviction proven by a re-read incrementing the cache's tiles_created,
    // copy-on-first-write surviving that eviction, and every
    // missing/truncated/changed-on-disk path refusing rather than serving
    // plausible pixels. Runs -- and asserts the same answers, not merely the
    // correct ones -- in BOTH NP_USE_OIIO configurations, because Eager is a
    // complete residency strategy and not a degraded mode. Headless and
    // GPU-free; writes and removes selftest_residency_* scratch files.
    const bool tileResidencyOk = np::runTileResidencyTest();
    // Phase 4 step 7 ("Export As -- format, space, depth *and resize*, with
    // saveable presets (PRD I15). Downscale prefilters; see the phase 6
    // warning"; PRD I5, I11, B6): io/ExportAs' request model, offerable-set
    // resolution, validation and preset file, plus ops/Resample's
    // prefiltered area-average downscale -- everything the File > Export
    // As... dialog calls into, with the dialog itself holding only widgets.
    // The prefilter's benefit is measured against a naive point-sampled
    // downscale rather than asserted, the resize is proven to run in linear
    // light by the 8-bit code the file actually carries, and the dialog's
    // refusal strings are asserted *equal* to io/Export's own so a second
    // set cannot drift into existence. Runs -- and asserts the correct
    // answers -- in BOTH NP_USE_OIIO configurations, because the behaviour
    // that matters most here is what an ON-build preset does in an OFF
    // build. Headless and GPU-free; writes and removes selftest_exportas_*
    // scratch files.
    const bool exportAsOk = np::runExportAsTest();
    // Phase 4 step 8 ("Document lifecycle -- revert, duplicate document, save
    // a copy, save incremental, open recent"; PRD I18, and I10/I11/I12 which
    // every save path here must keep honouring): app/DocumentLifecycle's
    // open-document record, the session that owns it, and the five
    // operations. The duplicate is proven not to inherit the original's path,
    // save-a-copy is proven to leave the document's path alone, the
    // incremental naming rule is checked across gaps, collisions and
    // already-versioned names, the recent list keeps a missing entry and says
    // why instead of dropping it, and the PRD I10 carry -- unknown attributes
    // and a whole foreign part -- is asserted intact after every operation
    // that writes. Runs -- and asserts the correct answers -- in BOTH
    // NP_USE_OIIO configurations; the record, the session, duplication, the
    // naming rule and the recent list are identical in each, and the
    // file-backed operations forward io/NpaintFile's own named refusal in the
    // build that has no writer. Headless and GPU-free; writes and removes a
    // selftest_lifecycle/ scratch directory.
    const bool documentLifecycleOk = np::runDocumentLifecycleTest();
    // Phase 4 step 9 ("`core/Journal` -- the recovery journal from ADR-0008";
    // PRD O5-O10): app/Journal's scratch directory, its timer, and the
    // recovery path. The timer rule is asserted as a pure function so both
    // builds check it; the round trip goes through the real writer, with the
    // crash simulated in-process by letting the session's destructor run
    // rather than by killing anything. A truncated journal is proven to be
    // refused on its own integrity record *before* the format reader sees
    // it -- provably so in the build that has no reader. Runs, and asserts the
    // correct answers, in BOTH NP_USE_OIIO configurations; the answer in the
    // OFF build is that there is no journal at all, because saveNpaint() is
    // the only writer and PRD O7 forbids a second one. Headless and GPU-free;
    // writes and removes a selftest_journal/ scratch directory, with
    // $NP_JOURNAL_DIR pointed at it so no real user state is touched.
    const bool recoveryJournalOk = np::runRecoveryJournalTest();
    // Phase 5 step 1 ("Multiple layers in `Document`, with reorder,
    // visibility, lock, opacity"; PRD C4, C16): core/Composite's `over`
    // against a hand-computed reference, opacity proven distinct from alpha,
    // a hidden layer contributing exactly nothing at zero tolerance,
    // core/LayerOps' operations and what each does to the dirty/structural
    // revision, `locked` refusing by name, app/LayerPanel's single top-first
    // reversal, and the round trip of a reordered stack. It also asserts the
    // regression boundary this step's semantics change makes necessary: a
    // single-layer document and a non-overlapping multi-layer one still
    // composite BYTE-identically to the plain sum `over` replaced, checked
    // against a second implementation of that sum written inside the test.
    // Runs, and asserts the correct answers, in BOTH NP_USE_OIIO
    // configurations. Headless and GPU-free; writes and removes one
    // selftest_layerstack.npaint.
    const bool layerStackOk = np::runLayerStackTest();
    // Phase 5 step 2 ("`core/Blend` -- the linear-safe set (over, plus,
    // multiply, screen, min, max) and `Mix`, the KM latent lerp.
    // Display-referred modes labelled as such"; PRD B7, C3, L5): every mode
    // against hand-computed references at opaque and at partial alpha, alpha
    // proven to be `over`'s under every mode, a transparent source proven a
    // bit-exact identity under every mode, `over` proven bit-identical to the
    // formula step 1 shipped, PRD B7's display-referred label proven to be a
    // property of the data (and screen's non-monotonicity above 1.0 proven
    // numerically, which is why it carries the label), PRD L5's Pigment-pair
    // restriction enforced in the model as well as the dropdown, and `Mix`'s
    // latent lerp checked against the real Mixbox LUT. It also re-makes step
    // 1's regression boundary with a DIFFERENT blend on each layer. Runs, and
    // asserts the same answers, in BOTH NP_USE_OIIO configurations. Headless
    // and GPU-free; writes no files.
    const bool blendOk = np::runBlendTest();
    // Phase 5 step 3 ("Pigment layers -- latent x mass tile storage at f16.
    // Per-layer op stack applies *after* the latent->RGB projection, so
    // grading never bakes the latents"; PRD C1, C3, C8, F10, L5): the
    // 7-channel f16 pigment tile and its exact round trip, the LUT-free
    // latent->RGB projection, PLAN.md's own Phase 5 verify sentence (blue over
    // yellow: green under `Mix`, the naive RGB lerp under `Normal`), opacity
    // proven to be transparency and never mass, a grade proven to leave the
    // stored latents bit-identical, every other `Mix` combination warned by
    // name, and a `.npaint` round trip carrying a Pigment layer and an RGB
    // layer together. Runs, and asserts the correct answers, in BOTH
    // NP_USE_OIIO configurations. Headless and GPU-free; writes and removes
    // one selftest_pigment.npaint.
    const bool pigmentLayerOk = np::runPigmentLayerTest();
    // Phase 5 step 15 / PRD C8, I4: the pigment basis as a `core::Document`
    // field -- stamped from the document, read back onto it, a basis this build
    // cannot interpret round-tripped rather than relabelled, an ordinary
    // document's file proven byte-identical, and the export warnings that never
    // reached a caller.
    const bool pigmentBasisOk = np::runPigmentBasisTest();
    // Phase 5 step 4 ("Layer masks -- single-channel tile store, the same
    // machinery"; PRD C4 (P0), C3 (P0), C2, I4, I11): the 32 KiB
    // single-channel f16 mask tile whose default is REVEAL, its derived 2^-12
    // bound, out-of-range and NaN samples clamped at every boundary, a mask
    // proven to multiply coverage against hand-computed references, mask x
    // opacity proven byte-identical to their product, absent vs all-1.0 vs
    // all-0.0 proven three different things, the PRD C3 trap (a mask on a
    // Pigment layer is not mass -- printed side by side on a mixed pair),
    // core/LayerOps' add/remove and their refusals, and a `.npaint` round trip
    // that also proves a mask-free document's bytes are unchanged. Runs, and
    // asserts the correct answers, in BOTH NP_USE_OIIO configurations.
    // Headless and GPU-free; writes and removes three `.npaint` files.
    const bool layerMaskOk = np::runLayerMaskTest();
    // Phase 5 step 5 ("Adjustment layers -- op stack against the composite
    // below"; PRD C5, C1, C3, C4, D13, D18, I10, I11): the one layer kind that
    // transforms what is accumulated beneath it instead of contributing to it,
    // against exact references; alpha proven bit-identical across it; opacity
    // 0, a hidden layer and an empty stack each byte-identically the layer not
    // existing; a mask restricting the adjustment per texel; two adjustment
    // layers proven to compose in stack order with a deliberately
    // non-commuting pair; the scope proven to be everything below and not just
    // the layer beneath (which is PRD C9's clipping mask, a different
    // feature); and io/OpSerial -- the `np:ops` carrier this step had to build,
    // because an adjustment layer's whole content is its op stack -- proven
    // against a hand-built payload and proven to carry a newer build's op
    // through a `.npaint` verbatim. Runs, and asserts the correct answers, in
    // BOTH NP_USE_OIIO configurations. Headless and GPU-free; writes and
    // removes three `.npaint` files.
    const bool adjustmentLayerOk = np::runAdjustmentLayerTest();
    // Phase 5 step 6 ("COW tiles -- copy-on-write with reference-counted
    // history"; PRD A9, O1, O4, C2): a `TileStoreOf<T>` slot is now a
    // `std::shared_ptr<T>`, so copying a store shares its tiles and the first
    // write to a shared one copies it. Sharing proven by pointer identity
    // rather than by equal contents, a write to one copy proven invisible in
    // the other, the refcount reaching zero proven to free exactly once
    // through an instrumented tile type, all three tile shapes including the
    // mask's REVEAL default, io/TileResidency's file-side copy-on-first-write
    // proven to compose with this one, the composite proven bit-identical over
    // shared tiles, and the cost measurements the step is justified by. Runs,
    // and asserts the correct answers, in BOTH NP_USE_OIIO configurations.
    // Headless and GPU-free; writes and removes two `.npaint` files.
    const bool cowTileOk = np::runCowTileTest();
    // Phase 5 step 7 ("`core/History` -- a linear list with a cursor, not a
    // stack: undo moves it back, redo moves it forward, and a new edit at a
    // non-end cursor truncates the tail. Undo bounded in bytes"; PRD O1, A9,
    // O4): every entry is a whole `core::Document`, which step 6's
    // copy-on-write made nearly free, so undo and redo are one traversal
    // primitive over states rather than two mechanisms over deltas. PLAN.md's
    // own verify sentence is run in the only form that exists today -- ten
    // tile writes through the real `recordEdit()` funnel, said so in the
    // output -- with the redo proven bit-identical to before the undos; the
    // truncated tail's memory is proven released by accounting and by RSS;
    // and eviction is proven correct against step 6's NON-additive sharing by
    // running the naive per-entry-sum policy beside it and showing it
    // over-evicts. A snapshot is proven to survive both an eviction and a
    // truncation. Runs, and asserts the correct answers, in BOTH NP_USE_OIIO
    // configurations. Headless and GPU-free; writes and removes two `.npaint`
    // files.
    const bool historyOk = np::runHistoryTest();
    // Phase 5 step 8 ("History panel listing entries by originating tool or
    // op; clicking one moves the cursor there in a single replay, not N"; PRD
    // O2, O3, with O1's redo and O4's snapshots made visible): app/HistoryPanel
    // is the pure half -- rows, row text, the serial mapping and the click --
    // with the chrome in ui/MacPaintUI.cpp, the same split app/LayerPanel has.
    // The panel reads oldest-at-top and reverses nothing, which is the
    // OPPOSITE of the layers panel, and both orders are asserted in one place
    // so "fixing" either to match the other fails. A row is keyed by
    // HistoryEntry::serial and never by its index, with the trap demonstrated:
    // a budget that drops six states leaves row index 3 holding a different
    // picture, and a click carrying the pre-eviction serial is refused with
    // the numbers rather than redirected to it. PRD O3 is counted and timed --
    // one cursor move at distance 1 and at distance 40, against the per-step
    // walk run beside it on the same history and shown to take forty calls for
    // the same bytes. Every row carries PAST/CURRENT/REDOABLE in its text, so
    // the branch the next edit destroys is legible without a screenshot. Runs,
    // and asserts the correct answers, in BOTH NP_USE_OIIO configurations.
    // Headless and GPU-free; writes no files.
    const bool historyPanelOk = np::runHistoryPanelTest();
    // Phase 5 step 9 ("Clipping masks -- a layer or group clipped by the alpha
    // of the layer below"; PRD C9): one bool on core::Layer, one clipRuns()
    // pass, and a three-function bracket that folds a clipping group per texel
    // inside the base's own tile walk -- no offscreen buffer, which is the
    // prediction this step falsified. A run of clipped layers is proven to
    // clip to ONE base in pixels rather than in bookkeeping, with the
    // cumulative reading's answer printed beside it. The rejected reading of
    // "which alpha, and where the group lands" is implemented inside the test
    // and printed beside the built one on fixtures where they differ. A
    // clipped Adjustment layer is proven to grade its base and nothing else,
    // which is step 5's function re-pointed rather than rewritten; `Mix` and a
    // clip are proven mutually exclusive through the one PRD L5 predicate; a
    // layer's own mask and its clip are proven to be different operators, one
    // acting on colour and one on coverage; and the bottom layer is refused by
    // name with the numbers at both setters while the compositor still
    // composites and warns about one that arrived from a file. The regression
    // boundary is asserted at zero tolerance, and the lazy-open rule that
    // makes it hold is proven necessary rather than asserted. Runs, and
    // asserts the correct answers, in BOTH NP_USE_OIIO configurations.
    // Headless and GPU-free; writes and removes three `.npaint` files.
    const bool clippingMaskOk = np::runClippingMaskTest();
    // UI detour step 2 ("the document, on screen"): ui/DocumentTexture, the
    // edge that makes all nine of Phase 5's steps visible, plus core/Premultiply
    // -- the `a <= 0 -> {0,0,0,0}` guard promoted out of four retyped copies and
    // now asserted at all five call sites at once. Straight alpha and RGBA16Float
    // are each proven by running the rejected alternative beside them and
    // printing both answers, and the revision cache's saving is measured against
    // PRD F3's pen-to-photon budget. Needs the GPU: it uploads a document at a row
    // stride the readback direction refuses and reads it back through a padded
    // staging buffer. Runs, and asserts the correct answers, in BOTH NP_USE_OIIO
    // configurations. Writes no files.
    const bool documentTextureOk = np::runDocumentTextureTest(gpu);
    // Phase 5 step 14 / PRD A5 + A6: the two-tab split's geometry and pane
    // rule, and the residency cap -- twenty tabs, two textures, measured.
    const bool documentResidencyOk = np::runDocumentResidencyTest(gpu);
    // UI detour step 3 ("the layer editor, and making it reachable"):
    // app/LayerEditor is the one surface the `Layer` menu and the LAYERS panel
    // buttons share, so this covers what every one of those controls does --
    // the three layer kinds, the mask, the clip, the flags, the per-layer op
    // stack -- and the rule that keeps them honest: every successful command
    // moves the revision exactly once and appends one history entry, while a
    // refusal moves neither. Runs, and asserts the correct answers, in BOTH
    // NP_USE_OIIO configurations. Headless and GPU-free; writes no files.
    const bool layerEditorOk = np::runLayerEditorTest();
    // The same step's other half: app/ControlsLayout, the right-hand column's
    // order, its default-open set and the label column that stops a slider's
    // name being clipped by the panel edge, with ImGui's own
    // label-on-the-right run beside it and both clip counts printed. Headless
    // and GPU-free; writes no files.
    const bool controlsLayoutOk = np::runControlsLayoutTest();
    // app/PanelLayout: the headless model behind the dockable panel system --
    // where every panel is (one of four docks, a flyout, or put away), in
    // what order, at what size, and that it survives a relaunch. The
    // exactly-once invariant under every mutation, the round-trip repair
    // rules, a version 1 file from the single-column build still reading
    // correctly, and a real save/load round trip under $NP_PANEL_LAYOUT.
    // Headless, GPU-free and ImGui-free.
    const bool panelLayoutOk = np::runPanelLayoutTest();
    // ui/DockLayout: the geometric half of the same feature -- that the slots
    // and splitters tile a dock exactly on all four sides, that a collapsed
    // panel hands its space to its neighbours, that the minimum-extent floor
    // holds without the slots overrunning the dock, that the overflow case is
    // disclosed rather than hidden, and that a splitter drag moves exactly
    // the boundary it grabbed. Headless, GPU-free and ImGui-free.
    const bool dockLayoutOk = np::runDockLayoutTest();
    // The incremental composite (core/DirtyTiles + core/Composite's region
    // walk + ui/DocumentTexture's sub-rectangle upload): that the dirty set is
    // complete, that a non-tile-local change is classified as one, and that
    // ten kinds of edit composite bit-identically to a full recomposite.
    const bool incrementalCompositeOk = np::runIncrementalCompositeTest(gpu);
    // Phase 5 step 10 / PRD C10, C11: core/Merge's five operations, checked
    // against the composite they must preserve at a bound derived from f16.
    const bool mergeFamilyOk = np::runMergeFamilyTest();
    // Phase 5 step 12 ("Layer comps -- named sets of visibility, position and
    // properties, restorable in one click and persisted in the document"; PRD
    // C14): the comp model, the id-keyed restore and its refusals, io/CompSerial's
    // `np:comps` carrier (a hex string, because the format table's `<blob>` is
    // unwritable), and the `.npaint` round trip -- with position reported as
    // not applicable, because core::Layer has none. Runs, and asserts the
    // correct answers, in BOTH NP_USE_OIIO configurations. Headless and
    // GPU-free; writes and removes five `.npaint` files.
    const bool layerCompOk = np::runLayerCompTest();
    // PLAN.md Phase 5's C7/C12 follow-on (PRD C7, P0): layer groups. The
    // model, core/LayerSetOps' span-splice Group/Ungroup commands and their
    // order-preservation proof, core/Composite's pass-through coverage fold
    // (including nesting and a hand-built cycle proven not to hang), undo
    // through the existing command funnel, and the `.npaint` round trip with
    // its older-build degradation. Headless and GPU-free; writes and removes
    // six `.npaint` files.
    const bool layerGroupOk = np::runLayerGroupTest();
    // task/group-ui: the LAYERS panel's own half of PRD C7 -- the row-depth,
    // ancestry-ordering and collapsed-row predicate app/LayerPanel's pure
    // half adds, plus the CLI-provable finding that the command-issuing half
    // (a LAYERS gesture / a menu item) already existed before this step,
    // through ui/MacPaintUI.cpp's pre-existing generic command walk.
    const bool layerGroupPanelOk = np::runLayerGroupPanelTest();
    // Phase 5 step 13 ("Export comps to files, and layers to files -- one
    // shared loop"; PRD I16, I17): io/ExportStates' name template and its
    // path refusals, the pre-flight that decides collisions and overwrites
    // before the first byte, the four-file deliverable proven byte-identical
    // to clicking each comp and using Export As, layers alone on
    // transparency with the rejected reading run beside it, and the stop-and-
    // report behaviour when file 3 of 4 fails. Runs, and asserts the correct
    // answers, in BOTH NP_USE_OIIO configurations -- the EXR seam is refused
    // before the first byte in OFF rather than skipped. Headless and
    // GPU-free; writes and removes a selftest_exportstates/ directory.
    const bool exportStatesOk = np::runExportStatesTest();
    // Phase 5 -- the CPU Pigment deposit (brush/Deposit + app/StrokeSession):
    // what one dab does to one texel, that a stroke's tile set is complete and
    // tight, that N dabs are one undo step, and `Mix` witnessed from a stroke.
    const bool pigmentDepositOk = np::runPigmentDepositTest();
    // Phase 5 -- painting on a plain RGB layer (brush/RgbDeposit), and the
    // routing fix that made it reachable: the premultiplied and linear
    // conventions established rather than assumed, the per-stroke opacity
    // ceiling with the rejected per-dab model measured beside it, speed
    // independence at zero tolerance, the selection as a bound rather than a
    // speed limit, and paint landing on the active layer and on no other.
    const bool rgbDepositOk = np::runRgbDepositTest();
    // PRD F9/F10 (both P0), ADR-0007 -- the eraser on a plain RGB layer, which
    // until this step did NOTHING: Tool::Eraser sat in the not-built routing
    // list, so a drag with it reached no layer and said nothing about why.
    // Destination-out on all four channels (premultiplied, so no fringe), the
    // per-stroke FLOOR with the rejected per-dab model measured beside it, the
    // selection as a bound rather than a speed limit, and an erase that costs
    // nothing on blank canvas.
    const bool rgbEraseOk = np::runRgbEraseTest();
    // PRD E1 (P0) on the layer kind that never had it, and ADR-0007's Pigment
    // eraser row that gate unblocked. brush/Deposit.hpp did not contain the word
    // "Selection", so a natural-media stroke on a Pigment layer painted straight
    // through the marching ants while the RGB branch one line above it passed
    // doc_->selection. The gate, with the rate-only model measured beside it --
    // mass accumulates LINEARLY, so kMaxMass alone is not a bound and six dabs
    // saturate a half-selected texel -- and then the eraser whose refusal named
    // that gate as its single blocker: the floor at zero tolerance, the latent
    // bit-identical, and an emptied texel proven well-formed by the rule a
    // straight-latent storage actually has rather than by the premultiplied one.
    const bool pigmentSelectionOk = np::runPigmentSelectionTest();
    // PRD D25/D26 -- the paint bucket's refusals. ops/FloodFill was never
    // wrong; the gate in front of it was inside the click condition, so a
    // bucket click on the layer kind a new layer defaults to disappeared with
    // nothing said. A success first (so a silenced bucket cannot pass the
    // refusals), then three refusals that name the layer and move no texel,
    // locked told apart from no-RGB-store, the options bar's two tables read at
    // once, PRD E1's selection bound at exact zero -- and the live recomposite
    // the Layer Properties dialog's undimmed modal depends on.
    const bool bucketRefusalOk = np::runBucketRefusalTest();
    // ui/ToolCursor -- the pointer, which until now was the OS arrow over every
    // tool because the build made no cursor call at all. The table is total and
    // no tool answers the fallback; the brush is a real crosshair, which needed
    // the ImGui backend suppressed and SDL driven directly (ImGui's set has no
    // crosshair, so routing through it left the six most-used tools sharing one
    // arrow); ImGui's own eleven cursors are asserted too, since suppressing
    // the backend made the panels ours; and the half worth having is that the
    // cursor reads the SAME refusal predicates the options bar does, so a brush
    // over a locked layer and a bucket over a Pigment layer are slashed before
    // the gesture is spent -- with the successes asserted beside them so a
    // function that always answers "not allowed" cannot pass.
    const bool toolCursorOk = np::runToolCursorTest();
    // Phase 5 step 11 / PRD C12, C13, C15: the multi-selection's ordering and
    // all-or-nothing rules, the integer-pixel translate align is built on
    // (asserted bit-identical), links, colour labels and the panel filter.
    const bool layerMultiSelectOk = np::runLayerMultiSelectTest();
    // The LAYERS panel as design "naturalPaint Panels" turn 2 option 2a
    // specifies it: the kind rail, the NEW popup's seven kinds, the row
    // metadata line against the design's own examples -- and the three pieces
    // of 2a that are deliberately NOT drawn, each pinned so a later revision
    // cannot quietly invent the number behind it.
    const bool layerPanel2aOk = np::runLayerPanel2aTest();
    // Phase 12 / PRD G7, G9: io/Descriptor, the Action Descriptor reader, against
    // synthetic fixtures parsed out of guard-paged mappings.
    const bool descriptorOk = np::runDescriptorTest();
    // app/CloseDecision: closing a document that holds unsaved work. PRD I11's
    // refusal was correct and invisible -- it went to a line of dim grey beside
    // the menus, so the tab's close box read as a dead control. This is the
    // Save / Don't Save / Cancel question that replaced it, and above all the
    // assertion that the pending close is keyed on the document's identity
    // rather than on its index: the stale index is arranged to name a
    // *different* document, so an index-keyed version discards the wrong one
    // successfully instead of failing. Headless, GPU-free, writes no files, and
    // asserts the same answers in BOTH NP_USE_OIIO configurations.
    const bool closeDecisionOk = np::runCloseDecisionTest();
    // app/ImportImage and app/QuitSequence: the way an image gets into the open
    // document, and the way the application gets out without discarding it.
    // Both were features that existed and could not be reached -- an importer
    // with no caller outside this suite, and a quit that never asked a single
    // document whether it was dirty.
    const bool quitGuardOk = np::runQuitGuardTest();
    // docs/reachability-audit.md D1, D2, D4, A4: undo/redo reachable by
    // keyboard and menu (not mouse-only), the Edit menu's nine
    // clipboard/selection commands correctly wired and correctly
    // enabled/disabled, the Goodies menu obeying the same disabled-tool
    // guard the palette does, and a positional command-line argument opening
    // a document. Headless; deliberately does not call
    // menuContextFromState() itself, so it never touches the user's real
    // recent-documents file.
    const bool menuBasicsOk = np::runMenuBasicsTest();
    // ui/MenuModel: the menu bar as data, so that a native macOS menu and the
    // ImGui one can be two renderings of ONE set of actions rather than two
    // copies of them. Headless -- no window, no GPU, no ImGui context, no
    // NSApplication. Its sharpest assertion is a sibling of the one above:
    // performing the model's Quit sets `requestQuit` and leaves `quit` alone,
    // so a backend wired to Cocoa's `terminate:` -- which would route straight
    // past the guard runQuitGuardTest() covers -- cannot pass.
    const bool menuModelOk = np::runMenuModelTest();
    // The Select menu (docs/reachability-audit.md C5; PRD E4/E8/E9): the
    // dialog-to-engine wiring for grow, shrink, feather, colour range and
    // luminance range, plus the dedicated undo stack a pure-selection change
    // needs because core::History cannot hold one. Headless -- no window, no
    // GPU, no ImGui context.
    const bool selectMenuOk = np::runSelectMenuTest();
    // app/OpenAnyFile and io/FileKind: File > Open takes any file this build
    // reads, and which reader it goes to is decided from the file's **bytes**
    // rather than its extension -- a `.npaint` is an OpenEXR carrying
    // `np:version`, so one saved as `.exr` is still a document and a PNG named
    // `sketch.npaint` is still a picture. Plus the drag-and-drop routing rule,
    // asserted as a pure function so it needs no window, and what a drop of
    // twelve files at once resolves to.
    const bool openAnyFileOk = np::runOpenAnyFileTest();
    // io/PsdImport: a hand-written, dependency-free reader for LAYERED PSD
    // files, and the app/OpenAnyFile.cpp seam that section's own header
    // named before this module existed -- "a decoder returning a Document
    // with N layers replaces this one call and nothing else in this
    // function". No genuine Photoshop file was available to test against;
    // this section's own doc comment states exactly what that leaves
    // unverified.
    const bool psdImportOk = np::runPsdImportTest();
    // app/FilterOps, ui/MenuModel's Filter and Image menus (docs/
    // reachability-audit.md C1): six of ops/Blur's, ops/Filters' and ops/
    // DocumentTransform's ~93 tested-but-unreachable entry points, wired to
    // a menu, a dialog and a history entry. Proves the WIRING, not the
    // engines runBlurTest()/runFiltersTest() already cover: each item
    // reaches its own engine call with its own dialog parameters, the active
    // selection bounds a pixel op over the whole excluded region, one
    // history entry per confirmed dialog with an exact undo, and a layer
    // PixelOpRefusal cannot touch refuses by the SAME message the paint
    // bucket already uses.
    const bool filterMenuOk = np::runFilterMenuTest();
    // Image > Adjustments (app/AdjustmentOps, ops/PointOpTiles): the tile-level
    // runner for ops/PointOps' pure rgb->rgb functions, the bridge that aims
    // one at the active layer through the selection, and the five menu items.
    // See SelfTest.hpp for why this section deliberately re-tests neither the
    // maths nor the selection blend. Headless and GPU-free.
    const bool adjustmentMenuOk = np::runAdjustmentMenuTest();
    // Reachability audit A5/B2/B3: the BRUSH panel's shared-field ranges, the
    // WET slider's route-dependent disabled state, and the loaded pigment's
    // ownership of Density/Staining/Granulation. Headless -- no ImGui frame,
    // no window; see the section's own doc comment for what that leaves
    // unverified.
    const bool chromeConsistencyOk = np::runChromeConsistencyTest();
    // Reachability audit D5 / PRD I13: a save is now read back through the
    // same reader File > Open uses and structurally verified before the
    // original file is replaced, closing the in-place-write hazard the step
    // found alongside the missing readback.
    const bool saveReadbackOk = np::runSaveReadbackTest();
    // 1.3 / ADR-0003: deposited mass must match regardless of stroke speed.
    const bool strokeSpeedOk = np::runStrokeSpeedTest(gpu, *s, lut);
    // 1.4 / ADR-0001 bullet 5: idle RSS, measured before this branch (or
    // any other) ever constructed a PaintSim.
    const bool idleMemOk = np::runIdleMemoryTest(idleRssBytes);
    // track8/zoom (PRD Q1, R5): the Zoom tool's click/Alt-click/scrubby-drag
    // anchor math and the brush-size gesture/bracket-key range, both as pure
    // functions -- app/ZoomAndSize.hpp. Headless and GPU-free.
    const bool zoomAndSizeOk = np::runZoomAndSizeTest();
    // naturalPaint canvasdim bug fix: `canvasDimensionsFor()` (app/
    // ZoomAndSize.hpp section 4) -- the active document's own size is now
    // `ui/MacPaintUI.cpp`'s canvas block's one source of truth for its
    // on-screen `texW`/`texH`, closing "a non-square document displays
    // square". Headless and GPU-free.
    const bool canvasDimensionsOk = np::runCanvasDimensionsTest();
    // track10/input ("make Mac trackpad input feel right"): the notch-vs-
    // precise wheel classifier, the panel scroll's discount/smoothing, and
    // the pinch-to-zoom path's arithmetic -- app/WheelInput.hpp. Headless;
    // the SDL/ImGui dispatch sites this feeds are unreachable from here (F4).
    const bool wheelInputOk = np::runWheelInputTest();
    // item 4 ("trackpad interactions feel unnatural"): app/TouchGesture.hpp's
    // pure two-touch pan+zoom+rotate geometry, the half of the raw-touch
    // capture --selftest can reach (the NSTouch/AppKit dispatch itself is
    // unreachable from here, F4, the same as every other native trackpad path).
    const bool touchGestureOk = np::runTouchGestureTest();
    // item 4 continued: app/TouchGestureSession's gesture-start-baseline
    // half, the part computeTwoTouchDelta() alone (just tested above)
    // cannot exercise since it is stateless.
    const bool touchGestureSessionOk = np::runTouchGestureSessionTest();
    // track10/feel (PaintCopilot §3.2, arXiv:2605.20941): the log/power
    // pressure-response curves and the pressure EMA's per-stroke reset.
    const bool pressureFeelOk = np::runPressureFeelTest();
  const bool strokePreviewOk = np::runStrokePreviewTest();
    // Paper tooth (brush/Deposit.hpp §2e, brush/Grain.hpp), US 5,347,620:
    // the tiled grain field, `F = clamp(P*S*O1 - G, 0, 1)`, grain OFF as a
    // bit-exact no-op, `app/DabPreview` agreeing with a real `depositDab()`
    // on a grained dab, and the field keyed on absolute document position
    // rather than dab-local offset -- the assertion a grain that moved with
    // the brush would fail.
    const bool grainOk = np::runGrainTest();
    // The OS file panel (ui/FileDialog.hpp): SDL's own validator on every
    // filter pattern this build ships, the filter lists derived from
    // io/Capabilities rather than hard-coded, and the cross-thread mailbox
    // that carries the chosen path back from SDL's callback.
    const bool fileDialogOk = np::runFileDialogTest();
    // app/DocumentPresets (docs/testing-issues.md T9, piece 1): the sizes
    // File > New offers, the built-in set, and user-defined presets --
    // add/rename/remove, persisted, atomically. Headless and GPU-free.
    const bool documentPresetsOk = np::runDocumentPresetsTest();
    // io/ClipboardImage (docs/testing-issues.md T9, piece 2): the system-
    // pasteboard bridge the "New from Clipboard" preset needs. Runs after
    // SDL_Init(SDL_INIT_VIDEO) above, so the live pasteboard queries this
    // section makes are real, not stubbed.
    const bool clipboardImageOk = np::runClipboardImageTest();
    // docs/architecture-review.md P0-3: core/Parallel (the threading layer)
    // and its two consumers, ops/Blur.cpp's blurTiles() and ops/Filters.cpp's
    // scatterAligned()/gatherBlurredPlane(). Headless and GPU-free -- pure
    // CPU tile arithmetic, no PaintSim involvement.
    const bool parallelOk = np::runParallelTest();
    // docs/architecture-review.md P0-4: the premise check for "the full
    // composite is layer-major over a 256 MB accumulator" -- composite cost
    // vs. layer count at 2048x2048, and the zero-fill's own share of it,
    // measured before any loop-order change is made on the strength of the
    // finding alone. Headless and GPU-free.
    const bool compositeCostOk = np::runCompositeCostTest();
    // Step 0 of the ops/Resample + ops/Transform performance task: whether
    // those two per-texel walks are worth threading/vectorising, measured
    // rather than assumed. Headless and GPU-free -- pure CPU resampling,
    // same as runTransformTest().
    const bool resamplePerfOk = np::runResamplePerfTest();
    // docs/architecture-review.md P1-2: core/ResourcePaths -- the resolver
    // this task added so the binary can leave the machine that built it.
    // Headless and GPU-free -- pure filesystem, no PaintSim involvement.
    const bool resourcePathsOk = np::runResourcePathsTest();
    // core/Composite.cpp's opaque-floor early exit: a layer, clip base, or
    // Mix pair whose own effective alpha is exactly 1.0 everywhere in a
    // tile makes everything strictly below it in that tile provably
    // irrelevant, so the walk can skip it there. Bit-identical on/off for
    // every qualifying and disqualifying case, a document-surgery sabotage
    // proof, a stale-cache invalidation proof, and a printed (not asserted)
    // performance sanity check. Headless and GPU-free.
    const bool opaqueFloorOk = np::runOpaqueFloorTest();
    const bool ok = pigmentOk && accumulatorOk && colorSpaceOk && shaperOk && keymapOk &&
                    tileStoreOk && imageDecodeOk && documentOk && baseLayerAlphaOk &&
                    createBlankOk && imageIOOk && placeImageAsLayerOk && probeOk &&
                    eyedropperOk &&
                    mipPyramidOk && viewTransformOk && guidesGridSnapOk &&
                    halfOk && histogramOk && pointOpsOk && toneOpsOk && colorOpsOk && monoOpsOk &&
                    autoLevelsOk &&
                    gradientOk && selectionOk && channelsOk &&
                    selectionShapesOk && selectionRefineOk && selectionToolsOk && selectionDragOk &&
                    ellipseMarqueePreviewOk &&
                    selectionBoundaryOk && floodFillOk &&
                    clipboardOk && opStackOk &&
                    lutBakeOk && applyPassOk && gradeDispatchOk && transformOk && resamplePerfOk && documentTransformOk &&
                    transformSessionOk && transformPreviewTextureOk && blurOk &&
                    blurSimdOk &&
                    filtersOk && filtersExtOk &&
                    curveEditOk && brushDynamicsOk && dynamicsSourcesOk && dabPreviewOk && abrBrushesOk && checkedAddOk && multiplyFloorOk && scatterOk && abrSampledTipsOk && abrDualBrushOk && brushLibraryFileOk && userBrushLibraryOk && exportOk && formatSupportOk && npaintOk && tileResidencyOk &&
                    exportAsOk && documentLifecycleOk && recoveryJournalOk && layerStackOk &&
                    blendOk && pigmentLayerOk && pigmentBasisOk && layerMaskOk && adjustmentLayerOk &&
                    cowTileOk && historyOk && historyPanelOk && clippingMaskOk &&
                    documentTextureOk && documentResidencyOk && layerEditorOk &&
                    controlsLayoutOk && panelLayoutOk && dockLayoutOk &&
                    incrementalCompositeOk && mergeFamilyOk && layerCompOk && layerGroupOk &&
                    layerGroupPanelOk &&
                    exportStatesOk && pigmentDepositOk && rgbDepositOk && rgbEraseOk &&
                    pigmentSelectionOk && bucketRefusalOk &&
                    layerMultiSelectOk && layerPanel2aOk && toolCursorOk &&
                    strokeSpeedOk && idleMemOk && fieldAllocOk && fontsOk &&
                    atelierOk && activeLayerOk && presentTransferOk &&
                    pigmentBakeOk && solverPersistenceOk && strokeBridgeOk && descriptorOk &&
                    closeDecisionOk && quitGuardOk && menuBasicsOk && menuModelOk &&
                    openAnyFileOk && psdImportOk && filterMenuOk && adjustmentMenuOk &&
                    selectMenuOk &&
                    chromeConsistencyOk && saveReadbackOk && zoomAndSizeOk &&
                    canvasDimensionsOk && angleConventionOk && wheelInputOk && touchGestureOk &&
                    touchGestureSessionOk && pressureFeelOk &&
                    grainOk && strokePreviewOk && fileDialogOk && documentPresetsOk &&
                    clipboardImageOk && parallelOk && compositeCostOk && resourcePathsOk &&
                    opaqueFloorOk;
    s->shutdown();
    gpu.shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok ? 0 : 1;
  }

  // ---- ImGui ----
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // the layout is fixed; don't persist window state
  // ui/ToolCursor §§1 and 6: **this build owns the mouse cursor outright.**
  //
  // `ImGui_ImplSDL3_UpdateMouseCursor()` tests this flag first and early-returns,
  // so the backend stops calling `SDL_SetCursor()` entirely. That is what makes
  // a crosshair over the canvas possible at all -- ImGui's cursor set contains
  // no crosshair, and its backend only ever builds one SDL cursor per ImGui
  // value, so `SDL_SYSTEM_CURSOR_CROSSHAIR` is unreachable through it.
  //
  // **The cost is that the panels are now ours too.** With the backend
  // suppressed, nothing else will set the I-beam in a text box or the resize
  // arrows on a window border; `cursors.apply()` below has to, by falling
  // back to `ImGui::GetMouseCursor()` whenever the canvas has not asked for
  // something. Removing this line without also removing that call leaves two
  // writers and a cursor that sticks; removing the call without this line
  // leaves the tool cursors dead.
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  np::applyAtelierTheme();

  // ui/Fonts: six of the seven layer-kind glyphs docs/ui.md 3.2 assigns are
  // above U+00FF and ImGui's built-in ProggyClean holds nothing above U+00FF,
  // so none of them could be drawn -- ui/MacPaintUI substituted `[R]`/`[P]`/
  // `[A]` instead. Merged into the text face, which the same call loads.
  // Reported either way: a silent failure here is a panel of stand-ins with
  // no explanation, which is how this survived nine passing --selftest
  // sections that assert the glyph values. 13.0f is ProggyClean's own native
  // size (imgui_draw.cpp's AddFontDefault); the merge source has to be asked
  // for the same size or the merged glyphs sit off the baseline beside it.
  const np::FontLoadResult fontResult = np::installUiFonts(13.0f);
  // Both halves reported, because both can silently fall back: the ramp to
  // ImGui's bitmap ProggyClean, the glyph merge to boxes. docs/ui.md section 1
  // names Archivo, which nothing ships -- so the line says what actually
  // loaded rather than what was asked for.
  std::printf("[fonts] text %s, mono %s\n",
              fontResult.textPath.empty() ? "(ImGui ProggyClean fallback)"
                                          : fontResult.textPath.c_str(),
              fontResult.monoPath.empty() ? "(none -- numerics share the text face)"
                                          : fontResult.monoPath.c_str());
  if (fontResult.ok)
    std::printf("[fonts] merged %s for %zu layer-kind glyphs\n", fontResult.path.c_str(),
                np::requiredUiCodepoints().size());
  else
    std::printf("[fonts] %s\n", fontResult.error.c_str());

  // The tool palette's Lucide icons (docs/ui.md section 2: "Toolbox uses
  // Lucide icons at 15px, one per tool"), unioned with the Multi-selection
  // panel's alignment/distribute icons (`core::layerSetCommandIconCodepoints()`)
  // -- both draw from the same vendored Lucide font, so one merge covers
  // both rather than fighting over `g_fonts.text`. A second, independent
  // merge from the layer-kind one above -- see ui/Fonts.hpp's
  // installToolIconFont() for why it cannot just reuse that call -- so it
  // gets its own report line rather than being folded into fontResult's.
  std::vector<uint32_t> toolIconPoints = np::toolIconCodepoints();
  for (const uint32_t cp : np::layerSetCommandIconCodepoints())
    if (std::find(toolIconPoints.begin(), toolIconPoints.end(), cp) == toolIconPoints.end())
      toolIconPoints.push_back(cp);
  std::sort(toolIconPoints.begin(), toolIconPoints.end());
  const np::ToolIconLoadResult iconResult = np::installToolIconFont(toolIconPoints);
  if (iconResult.ok)
    std::printf("[fonts] merged %s for %zu tool-palette icons\n", iconResult.path.c_str(),
                toolIconPoints.size());
  else
    std::printf("[fonts] %s\n", iconResult.error.c_str());

  ImGui_ImplSDL3_InitForOther(window);
  ImGui_ImplWGPU_InitInfo wgpuInit;
  wgpuInit.Device = gpu.device;
  wgpuInit.NumFramesInFlight = 3;
  wgpuInit.RenderTargetFormat = gpu.surfaceFormat;
  wgpuInit.DepthStencilFormat = WGPUTextureFormat_Undefined;
  ImGui_ImplWGPU_Init(&wgpuInit);
  np::initCanvasQuad(gpu);

  // Every cursor this application will ever show (ui/ToolCursor §6), built once
  // here because `SDL_CreateSystemCursor()` needs SDL's video subsystem up --
  // which it has been since `SDL_Init()` far above -- and destroyed at the
  // bottom of `main()` **before `SDL_Quit()`**, which tears the subsystem down
  // underneath any cursor still alive.
  //
  // A local rather than a file-scope object so that lifetime is exactly this
  // function's: there is no static destructor racing `SDL_Quit()`, and the
  // create/destroy pair can be read as one by scrolling between them.
  np::SystemCursorTable cursors;
  cursors.create();
  // ui/ToolCursor.hpp §7: whether the per-tool bitmaps or the system shapes
  // are what this run will show. One line, because "the cursor looks wrong"
  // and "the bitmaps silently fell back" are indistinguishable from outside.
  std::printf("[cursor] per-tool bitmap cursors %s\n",
              cursors.bitmapCursorsEnabled() ? "on" : "off");

  // app/Keymap (Phase 2 step 15, PRD R7/R8): bindings loaded from a data
  // file rather than the `if (e.key.key == SDLK_...)` checks this used to
  // be. Conflicts are detected at load time, not silently resolved by load
  // order -- report them now, once, rather than only when a bad key is
  // actually pressed.
  np::Keymap keymap;
  if (keymap.loadFromFile("default.json")) {
    if (keymap.hasConflicts()) {
      std::fprintf(stderr, "[keymap] %zu conflict(s) in the default keymap:\n",
                   keymap.conflicts().size());
      keymap.reportConflicts();
    }
  } else {
    std::fprintf(stderr,
                 "[keymap] failed to load %s/default.json -- keyboard shortcuts "
                 "will not resolve to any action this session\n",
                 np::keymapDir().c_str());
  }

  np::AppState st;
  // docs/reachability-audit.md F2: set once, for the whole run -- see
  // AppState::screenshotCliActive's own comment for why the title bar's fps
  // readout needs to know this.
  st.screenshotCliActive = (screenshotPath != nullptr);

  // PLAN.md Phase 4 step 9 (app/Journal, ADR-0008, PRD O5-O10).
  //
  // Discovery runs *before* begin(), so this session's own directory can
  // never appear in its own recovery offer -- the flock probe would exclude
  // it anyway, but ordering makes that unconditional rather than dependent on
  // a lock succeeding.
  st.recovery = np::discoverRecoverySessions();
  st.recoveryOfferPending = !st.recovery.empty();
  {
    std::string journalError;
    const bool journalling = st.journal.begin({}, &journalError);
    if (journalling)
      std::fprintf(stderr, "[journal] recovery scratch: %s (every %.0f s)\n",
                   st.journal.directory().c_str(), np::kJournalIntervalSeconds);
    // Printed on both paths, because begin() also reports a *non-fatal*
    // problem this way (a scratch directory it could not lock, which costs
    // nothing but is worth knowing). Named once, at the point the journal
    // would otherwise have started -- including the whole NP_USE_OIIO=OFF
    // refusal, which is the default build's answer. A painting application
    // that refused to start without a journal would be worse than one that
    // says it has none.
    if (!journalError.empty()) std::fprintf(stderr, "[journal] %s\n", journalError.c_str());
  }

  // --- A session always has a document (UI detour step 2, Part C) ---------
  //
  // Before this, `st.documents` started empty and the LAYERS and HISTORY
  // panels were dead on launch: "No document open", with File > New Document
  // the only way to make either mean anything. That was defensible while the
  // document was invisible -- there was nothing to look at either way -- and
  // it stops being defensible the moment the canvas draws the composite.
  //
  // Sized to kCanvasW/kCanvasH so the document quad and the paper quad are the
  // same rectangle at the same resolution (ui/MacPaintUI's canvas block draws
  // the document on the canvas quad). `createBlank()` allocates **no tiles**
  // (PRD C2), so this costs one empty TileStore and one baseline history
  // entry, and the canvas looks exactly as it did before this step until a
  // layer holds content.
  //
  // Deliberately after the `--selftest` / `--diag` / `--modes` branches have
  // already returned, so none of them sees a document they did not ask for --
  // in particular `--selftest`'s idle-RSS assertion, whose whole point is a
  // measurement taken before any subsystem exists.
  st.documents.add(np::makeBlankOpenDocument(static_cast<int32_t>(kCanvasW),
                                             static_cast<int32_t>(kCanvasH), np::WorkingSpace{}));
  if (demoDocument) {
    if (np::OpenDocument* od = st.documents.active()) {
      buildDemoDocument(*od);
      std::printf("[demo-document] %d x %d, %zu layers, revision %llu\n", od->document.width,
                  od->document.height, od->document.layers.size(),
                  static_cast<unsigned long long>(od->revision));
    }
  }
  if (pigmentStrokeDemo) {
    if (np::OpenDocument* od = st.documents.active())
      buildPigmentStrokeDemo(*od, pigmentStrokeDemoMix);
  }
  if (penDemo) {
    if (np::OpenDocument* od = st.documents.active()) preparePenDemo(*od);
  }
  // After --demo-document deliberately: the script builds on whatever the
  // document already holds, and the layer it clips to is the one that was on
  // top when it started -- which is a layer with pixels in it exactly when the
  // fixture above has run.
  st.controlsAllOpen = controlsAllOpen;
  st.openLayerMenu = openLayerMenu;
  st.openToolFlyoutDemo = flyoutDemo;
  st.panelStackDemo = panelStackDemo;
  st.openExportStatesDialog = openExportStates;
  st.openLayerProperties = openLayerProperties;
  if (exportStatesFolder != nullptr) st.exportStatesFolder = exportStatesFolder;
  if (controlsScrollTo != nullptr) st.controlsScrollTo = controlsScrollTo;
  if (uiLayerDemo) {
    if (np::OpenDocument* od = st.documents.active()) runUiLayerDemo(*od, uiLayerDemoClip);
  }
  if (marqueeDemo) {
    if (np::OpenDocument* od = st.documents.active()) {
      // Inset from the canvas edges on all four sides so every side of the
      // boundary is visible against paper, and deliberately NOT tile-aligned
      // (the canvas is 1024 and the tiles are 128) so the drawn rectangle is
      // the selection's true bounds rather than a tile grid.
      od->selection = np::selectRectangle(180.0f, 150.0f, 700.0f, 560.0f);
      ++od->selectionRevision;
      st.brush.tool = np::Tool::Marquee;
      std::printf("[marquee-demo] selection installed: 180,150 -> 700,560\n");
    }
  }
  if (flyoutDemo)
    std::printf("[flyout-demo] Brush group's flyout held open (right-click/press-hold demo)\n");
  if (panelStackDemo)
    std::printf("[panel-stack-demo] HISTOGRAM+GRADE tabbed into COLOR's slot, COMPS into the "
                "collapsed HISTORY's -- in memory only, never saved\n");
  // After both fixtures, deliberately: a merge is applied to whatever stack
  // the flags before it built, which is what makes the before/after pair a
  // pair.
  if (uiMergeDemo != nullptr) {
    if (np::OpenDocument* od = st.documents.active()) runUiMergeDemo(*od, uiMergeDemo);
  }
  // **Pre-existing defect from Phase 5 step 12, repaired here rather than
  // worked around.** `--comps-demo` parsed its arguments and `runCompsDemo()`
  // was defined, but nothing ever called it, so the flag silently did nothing.
  // Found while trying to photograph step 13's dialog against a document that
  // actually has comps. It is precisely the failure `.claude/AGENT-BRIEF.md`
  // §6.3 names, one level out from --selftest, and its fix belongs where the
  // other fixture flags are applied.
  if (compsDemo) {
    if (np::OpenDocument* od = st.documents.active())
      runCompsDemo(*od, compsDemoRestore, compsDemoDrop);
  }

  // Last, deliberately: a set gesture acts on whatever stack the flags before
  // it built, so `--demo-document --ui-multiselect-demo ...` photographs the
  // three-layer fixture acted on as a set.
  if (uiMultiSelectDemo != nullptr) {
    if (np::OpenDocument* od = st.documents.active())
      runUiMultiSelectDemo(*od, uiMultiSelectDemo);
  }

  // After the set gestures, deliberately: a transform photographs whatever
  // stack the flags before it built. Serviced by ui/MacPaintUI.cpp's canvas
  // block on the first frame, exactly as a keystroke would be.
  if (transformDemo) {
    if (transformDemoLayer >= 0) {
      if (np::OpenDocument* od = st.documents.active())
        np::setActiveLayer(*od, static_cast<size_t>(transformDemoLayer));
    }
    st.requestFreeTransform = true;
  }

  // After all of them, and the only fixture that is not meant to be combined
  // with the others: it paints a flat field over layer 0 and adds a second
  // document, so `--demo-document --split-demo` would photograph this field
  // under that fixture's upper two layers. Said here rather than enforced --
  // the flags are a developer's tool and a refusal would be a rule to
  // remember where a sentence is enough.
  if (splitDemo) buildSplitDemo(st, splitDemoMode);

  // D4 (docs/reachability-audit.md): `naturalPaint foo.npaint` on the command
  // line. After every `--*-demo` fixture rather than interleaved with them,
  // deliberately -- those flags all act on "the" active document assuming
  // there is exactly one, and a positional argument opening its own document
  // partway through that sequence would silently redirect a later fixture at
  // a file it was never meant to touch. Positional files are the true last
  // word instead: `DocumentSession::add()` makes the last one opened active,
  // so a real file given alongside a demo flag wins the tab a human would
  // expect it to.
  //
  // Routed through `openAnyFileAsDocument()` (app/OpenAnyFile.hpp) -- the
  // SAME function `File > Open...` calls (ui/MacPaintUI.cpp's
  // `applyDocumentPathAction()`, the `DocPathAction::Open` case) -- so a
  // `.npaint` or any decodable image opens exactly as it would from the
  // menu, sniffed from its bytes rather than its extension. There is no
  // second dispatch path here: this loop is the same three lines that
  // function's Open case is, because that function itself is file-local to
  // ui/MacPaintUI.cpp and this is a different translation unit.
  //
  // `st.recentDocuments` is loaded explicitly first -- it would otherwise
  // still be the empty, never-read default `AppState` starts with (the lazy
  // load normally happens on the first UI frame, in
  // `menuContextFromState()`, which has not run yet at startup), and saving
  // an empty-plus-one-entry list over the real one on disk would be a
  // command-line launch quietly truncating the user's actual recent-file
  // history to a single line.
  //
  // A refusal -- missing file, unreadable, a format this build cannot read --
  // names the file on stderr and changes nothing else. It must not stop the
  // application from starting: a typo in a shell script should open an empty
  // canvas and say why, not make naturalPaint impossible to launch.
  if (!positionalPaths.empty()) {
    if (!st.recentDocumentsLoaded) {
      st.recentDocumentsLoaded = true;
      st.recentDocuments.loadFromFile(np::defaultRecentDocumentsPath());
    }
    for (const std::string& path : positionalPaths) {
      np::OpenAnyResult opened = np::openAnyFileAsDocument(path, &st.recentDocuments);
      if (!opened.status.empty()) std::fprintf(stderr, "[open] %s\n", opened.status.c_str());
      for (const std::string& w : opened.warnings)
        std::fprintf(stderr, "[open] ! %s\n", w.c_str());
      if (opened.ok) {
        st.documents.add(std::move(opened.document));
        std::string recentSaveError;
        st.recentDocuments.saveToFile(np::defaultRecentDocumentsPath(), &recentSaveError);
      }
    }
  }

  // st.opStack starts empty -- PLAN.md Phase 3 step 8's real op-authoring
  // UI (ui/MacPaintUI.cpp's GRADE section: add/reorder/toggle/delete, plus
  // the curve widget) is how a user populates it now. Earlier, before this
  // step existed, this constructor seeded two fixed debug ops here so the
  // Apply pass (step 6) had something to show in the running app; that
  // scaffolding is gone now that real UI exists.
  st.sim.brushRadius = st.brush.radius;
  // Fixed timestep (PRD H7): the look of a wash should not depend on the
  // frame rate. `st.sim.dt` is set once, here, to the constant physics tick
  // — never recomputed per frame — so PaintSim::frame()'s existing
  // `params.dt = paramsIn.dt / activeSubsteps` division is unchanged in
  // form. What varies per frame is how many times frame() gets called; see
  // the accumulator loop below.
  st.sim.dt = np::kFixedDt;

  auto prev = std::chrono::steady_clock::now();
  uint32_t frame = 0;
  // Leftover simulated time, in ms, banked between render frames.
  float fixedStepAcc = 0.0f;

  np::Latency latency;
  latency.setVerbose(latencyVerbose);
  bool strokeWasActive = false;

  // The stroke bridge's frame sequence (app/StrokeBake.hpp section 1) now
  // lives in `st.bakeCycle`, not as a local here.
  //
  // It was a local, on the argument that AppState holds document and session
  // state and this is neither. That argument was right about what the cycle
  // *is* and wrong about who needs it: ui/MacPaintUI has to force the same
  // cycle to settle before it moves the history cursor (app/StrokeBake.hpp
  // section 4), and a local here is unreachable from there. Giving MacPaintUI
  // its own would be worse than untidy -- each would keep a separate drying
  // episode, so one stroke would produce two history entries, and both would
  // contend for the solver's single readback slot.

  // ------------------------------------------------------------ drag and drop
  //
  // PLAN.md step 13's other half. io/Export.hpp described `placeImageAsLayer()`
  // as written "for step 13's drag-and-drop" and there was **no
  // `SDL_EVENT_DROP_FILE` handler anywhere in `src/`** -- the same
  // finished-but-unreachable shape app/ImportImage and app/OpenAnyFile each
  // closed one level further in.
  //
  // **Enabling.** SDL3 posts drop events only while they are enabled, and only
  // registers the window with the platform's drag-and-drop machinery when they
  // are: on macOS `Cocoa_AcceptDragAndDrop()` is what calls
  // `-registerForDraggedTypes:`, and it is driven by
  // `SDL_EventEnabled(SDL_EVENT_DROP_FILE)` at window creation and again from
  // `SDL_ToggleDragAndDropSupport()` whenever that flag changes. Read against
  // this SDL, every event type starts *enabled* (`SDL_disabled_events` is
  // zero-initialised), so this call changes nothing today and the window is
  // already registered. It is here anyway, and this is the argument for it: it
  // is the line that would have to exist if that default ever flipped, it costs
  // one no-op call once, and without it the requirement is invisible -- a
  // feature that works by default is a feature nobody knows is conditional.
  // Ordering is not load-bearing either way, because a change to the flag
  // re-registers every window that already exists.
  SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);

  // One drop gesture's files, accumulated across the burst of
  // `SDL_EVENT_DROP_FILE` events between `SDL_EVENT_DROP_BEGIN` and
  // `SDL_EVENT_DROP_COMPLETE`.
  //
  // **SDL delivers one event per file**, so acting on each as it arrives is how
  // a drop of twelve pictures becomes twelve separate decisions -- twelve tabs,
  // or twelve modals, or (the version that reads as a bug) one file used and
  // eleven ignored. Batching turns the gesture back into one act, which is what
  // the user performed; app/OpenAnyFile's `applyDroppedFiles()` owns what that
  // act means and is asserted headlessly.
  //
  // SDL synthesises `DROP_BEGIN` itself before the first file of a gesture
  // (`SDL_SendDrop()` tracks `window->is_dropping`), so it is always seen even
  // though the Cocoa backend never sends one explicitly -- but it also fires on
  // `DROP_POSITION`, i.e. when a drag merely *enters* the window. A drag that
  // enters and leaves therefore begins a gesture that carries no files, which
  // is why the list is cleared at both ends and why `applyDroppedFiles()` has a
  // no-files case that says nothing happened.
  std::vector<std::string> droppedFiles;

  // Where the gesture's files were released, window-relative -- the space
  // `SDL_DropEvent::x/y` and `ui/AtelierLayout`'s `AtelierBands` both live in
  // (app/OpenAnyFile.cpp's `dropDestinationForPoint()` comment has the full
  // coordinate-space argument for why no scaling belongs between them).
  // Updated on every `SDL_EVENT_DROP_FILE` of the gesture rather than read
  // once, because `SDL_EVENT_DROP_BEGIN` never carries a position
  // (`SDL_DropEvent::x/y`'s own doc comment says so) and this is the first
  // event of a gesture that reliably does.
  float dropX = 0.0f, dropY = 0.0f;

  // Frame counter, used only by --screenshot: the first frames are not
  // representative (ImGui lays out docked panels on frame 1).
  uint64_t frameIndex = 0;

  // --frame-trace's marker-circle state: when the last mouse-button-down
  // landed and where, so the circle can keep drawing at that screen position
  // for a couple of seconds even across however many frames a stall spans.
  // Wall-clock (SDL_GetTicksNS()), not a frame count, precisely because a
  // frame-count TTL would itself be stretched by the very stall this exists
  // to make visible.
  uint64_t clickMarkerUntilNs = 0;
  ImVec2 clickMarkerPos{};

  while (!st.quit) {
    const uint64_t frameStartNs = SDL_GetTicksNS();
    st.lastInputEventNs = 0;
    bool clickedThisFrame = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL3_ProcessEvent(&e);
      handlePenEvent(st, e);
      // e.common.timestamp is when SDL generated the event, not when we
      // happened to drain the queue for it — using our own SDL_GetTicksNS()
      // here would understate latency by however long the event sat queued.
      if (isPointerSampleEvent(e)) st.lastInputEventNs = e.common.timestamp;
      if (frameTrace && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        clickedThisFrame = true;
        clickMarkerPos = ImVec2(e.button.x, e.button.y);
        clickMarkerUntilNs = frameStartNs + 2'000'000'000ull;  // 2s, plenty over any real frame
      }

      // `requestQuit`, not `quit` — a user asking to leave is a request that
      // has to be answered against the open documents first (app/QuitSequence).
      // The one flag that still stops the loop outright is `--screenshot`'s,
      // below; keeping the two apart is what makes the guard structurally
      // unable to hang the golden harness.
      if (e.type == SDL_EVENT_QUIT) st.requestQuit = true;
      if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
          e.window.windowID == SDL_GetWindowID(window))
        st.requestQuit = true;
      // --- a drop ---------------------------------------------------------
      //
      // The window filter accepts `windowID == 0` as well as ours: SDL's
      // `SDL_SendDrop()` posts that for a drop delivered to the application
      // rather than to a specific window, and this build has exactly one window
      // for such a drop to have meant.
      if (e.type == SDL_EVENT_DROP_BEGIN || e.type == SDL_EVENT_DROP_FILE ||
          e.type == SDL_EVENT_DROP_COMPLETE) {
        const bool forUs =
            e.drop.windowID == 0 || e.drop.windowID == SDL_GetWindowID(window);
        if (forUs) {
          if (e.type == SDL_EVENT_DROP_BEGIN) {
            droppedFiles.clear();
          } else if (e.type == SDL_EVENT_DROP_FILE) {
            // Copied immediately and deliberately. `e.drop.data` is SDL
            // temporary memory (`SDL_CreateTemporaryString()`), valid only
            // until the next `SDL_PumpEvents()` -- which the very next
            // `SDL_PollEvent()` in this loop performs. Holding the pointer
            // until DROP_COMPLETE would read freed memory for every file but
            // the last, and would do it only for multi-file drops.
            if (e.drop.data != nullptr) droppedFiles.emplace_back(e.drop.data);
            // SDL3's `SDL_SendDrop()` (src/events/SDL_dropevents.c) stamps
            // every `DROP_FILE` event of a gesture with the position last
            // reported by `DROP_POSITION` -- which macOS's Cocoa backend
            // refreshes immediately before delivering the files
            // (`-performDragOperation:`) -- so this is the real release
            // point, not a stale one.
            dropX = e.drop.x;
            dropY = e.drop.y;
          } else {
            // Classify the release point into a destination *here*, from the
            // live chrome geometry, and hand the caller an enum rather than a
            // coordinate. `showTabStrip` mirrors ui/MacPaintUI.cpp's own call
            // exactly (`!st.documents.empty()`) -- if it disagreed, a drop
            // could be classified against a tab strip band the chrome never
            // actually drew this frame. The hit test itself is a pure
            // function in app/OpenAnyFile, driven by `--selftest` without a
            // window; nothing here does more than measure the window and call
            // it.
            int winW = 0, winH = 0;
            SDL_GetWindowSize(window, &winW, &winH);
            const np::AtelierBands dropBands =
                np::atelierLayout(0.0f, 0.0f, static_cast<float>(winW),
                                  static_cast<float>(winH), !st.documents.empty());
            // The two bands the routing rule cares about, copied into
            // app/OpenAnyFile's own struct -- see DropBandRect's comment for
            // why that module takes rectangles rather than AtelierBands.
            np::DropBands dropRects;
            dropRects.tabStrip = np::DropBandRect{dropBands.tabStrip.x, dropBands.tabStrip.y,
                                                  dropBands.tabStrip.w, dropBands.tabStrip.h};
            dropRects.canvas = np::DropBandRect{dropBands.canvas.x, dropBands.canvas.y,
                                                dropBands.canvas.w, dropBands.canvas.h};
            const np::DropDestination dest =
                np::dropDestinationForPoint(dropX, dropY, dropRects);

            // The whole gesture, resolved at once. Every decision -- open or
            // import, in what order, what a refusal says -- belongs to
            // app/OpenAnyFile, which `--selftest` exercises without a window;
            // nothing here does more than hand over the list, the
            // destination, and show the sentence that comes back.
            const np::DropOutcome dropped = np::applyDroppedFiles(
                st.documents, &st.recentDocuments, droppedFiles, dest);
            droppedFiles.clear();
            np::setDocumentStatusLine(dropped.status);
            // **A dropped picture is transformable the instant it lands.**
            // Dragging a photo in and then hunting for Edit > Free Transform
            // to place it is the step nobody wants; the layer arrives already
            // in a session, so the next drag moves it.
            //
            // Only for the unambiguous gesture -- app/OpenAnyFile.cpp
            // withdraws `transformableLayer` for a multi-file import and for
            // a mixed drop, so this cannot start a session on an arbitrary
            // one of eleven layers or on an index into a stack the user is no
            // longer looking at. A refusal from `beginLayer()` is left silent
            // ON PURPOSE and it is the one silence in this block: the drop
            // itself succeeded and `dropped.status` already says so, and
            // overwriting that sentence with a transform-shaped complaint
            // would report the wrong operation as having failed.
            if (dropped.transformableLayer) {
              if (np::OpenDocument* od = st.documents.active()) {
                // Seeds the session at a proportional scale-to-fit, centred
                // on the canvas, instead of identity, when the dropped
                // image's native size overflows it -- `computeDropFitTransform()`
                // itself is the no-op-for-content-that-already-fits guard
                // (app/TransformSession.hpp), so this is unconditional here:
                // an image that already fit gets identity back, matching
                // today's behaviour exactly. The layer's own pixels are
                // still written at native size (io/ImageIO.cpp), preserving
                // every source pixel; only the PENDING gizmo starts
                // pre-scaled, so the user sees the whole picture immediately
                // and can commit as-is or adjust further.
                const np::Layer& droppedLayer =
                    od->document.layers[*dropped.transformableLayer];
                const np::Mat3 initialPending = np::computeDropFitTransform(
                    np::regionFromBounds(np::layerContentBounds(droppedLayer)),
                    np::documentCanvasRegion(od->document));
                st.transform.beginLayer(od->document, *dropped.transformableLayer,
                                        initialPending);
                // T14: the SAME live-pixel-preview upload drawUI()'s own
                // Free Transform handler makes -- this is the session's
                // other begin*() call site (docs/testing-issues.md T14's own
                // brief is about the gizmo in general, not one entry point
                // into it), and without this call a dropped picture would
                // start its session showing the wireframe box only, while
                // Cmd+T on an existing layer showed pixels -- the same
                // inconsistency a second, drifting copy of the upload logic
                // would eventually reproduce anyway.
                np::beginTransformPreview(st, gpu);
              }
            }
            // Only a `.npaint` open adds a recent entry (app/OpenAnyFile.hpp
            // says why a picture cannot yet), so this is a no-op for a drop of
            // pictures -- but a dropped document has to reach the same list
            // File > Open... puts one in, or "open recent" would depend on
            // which gesture opened the file.
            if (dropped.opened > 0) {
              std::string recentSaveError;
              st.recentDocuments.saveToFile(np::defaultRecentDocumentsPath(),
                                            &recentSaveError);
            }
          }
        }
      }
      if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        gpu.configureSurface(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        ImGui_ImplWGPU_InvalidateDeviceObjects();
        ImGui_ImplWGPU_CreateDeviceObjects();
      }
      if (e.type == SDL_EVENT_KEY_DOWN) {
        // Resolve the raw key event through the keymap rather than testing
        // SDL keycodes here. `activeScope` is std::nullopt because no
        // document/layer model exists yet (core/Document + core/Layer are a
        // later Phase 2 step) -- every binding that exists today is global,
        // so this is "no active layer kind," not a stand-in for a real
        // value being dropped.
        const np::KeyChord chord{e.key.key, np::keyModsFromSDL(e.key.mod)};
        const std::optional<std::string> action = keymap.resolve(chord, std::nullopt);
        if (action == "toggle_pause") st.paused = !st.paused;
        else if (action == "clear_canvas") st.requestClear = true;
        else if (action == "reload_shaders") st.requestReload = true;
        else if (action == "quit") st.requestQuit = true;
        // F12: app/Screenshot -- the app photographs its own window, because
        // every macOS route to another process's pixels is behind a
        // permission that fails silently. Serviced in the present block.
        else if (action == "screenshot") st.requestScreenshot = true;
        // PLAN.md Phase 2 step 11 ("View controls", PRD Q1-Q4). Fit/100%/
        // zoom-in/zoom-out are request flags because they need the canvas
        // window's actual on-screen size, which only exists inside
        // MacPaintUI's canvas Begin()/End() block -- consumed there, same
        // as requestClear/requestReload are consumed in drawUI itself.
        // Mirror/rotation-reset/grayscale are plain view-state flips with
        // no layout dependency, so they're applied directly, right here,
        // the same way toggle_pause is above. `R` (unmodified) itself is
        // deliberately *not* a binding in keymaps/default.json -- it is a
        // held-plus-drag gesture (rotate view), which app/Keymap's
        // resolve() has no way to express (it only ever fires once per
        // discrete key-down); MacPaintUI.cpp's canvas block reads that key's
        // live held-state directly instead. See that file's comment at its
        // `rotateHeld` local for the full reasoning. `⌘⌥0` "zoom to
        // selection" (PRD Q1) is still absent, but the reason has changed
        // and is restated rather than left standing untrue: selection state
        // now EXISTS (core/SelectionMask; ⌘A/⌘D/⌘C/⌘X/⌘V are bound just
        // below). What is missing is the view maths to frame an arbitrary
        // rectangle, which belongs with the other view commands.
        // Cmd+T. Sets the same flag `MenuAction::FreeTransform` does, so the
        // chord and Edit > Free Transform are one path from here on --
        // ui/MacPaintUI.cpp's canvas block services it, because choosing
        // between a whole-layer and a selection-pixels transform needs the
        // live document.
        else if (action == "free_transform") st.requestFreeTransform = true;
        // Image > Adjustments' three chorded commands (app/AdjustmentOps).
        // These set the SAME `st.requestAdjustment` field the menu items set
        // -- see that field's comment in app/AppState.hpp for why the request
        // lives there rather than as a ui/MacPaintUI file-static like the
        // Filter menu's. ui/MacPaintUI.cpp's adjustment block services it,
        // opening the modal for the two that have one and performing
        // Desaturate outright.
        else if (action == "adjust_levels")
          st.requestAdjustment = np::AdjustmentRequest::Levels;
        else if (action == "adjust_curves")
          st.requestAdjustment = np::AdjustmentRequest::Curves;
        else if (action == "adjust_desaturate")
          st.requestAdjustment = np::AdjustmentRequest::Desaturate;
        else if (action == "adjust_hue_saturation")
          st.requestAdjustment = np::AdjustmentRequest::HueSaturation;
        else if (action == "adjust_color_balance")
          st.requestAdjustment = np::AdjustmentRequest::ColorBalance;
        else if (action == "adjust_black_and_white")
          st.requestAdjustment = np::AdjustmentRequest::BlackAndWhite;
        else if (action == "adjust_invert")
          st.requestAdjustment = np::AdjustmentRequest::Invert;
        else if (action == "adjust_auto_tone")
          st.requestAdjustment = np::AdjustmentRequest::AutoTone;
        else if (action == "adjust_auto_contrast")
          st.requestAdjustment = np::AdjustmentRequest::AutoContrast;
        else if (action == "adjust_auto_color")
          st.requestAdjustment = np::AdjustmentRequest::AutoColor;
        else if (action == "fit_window") st.requestFitWindow = true;
        else if (action == "zoom_100") st.requestZoom100 = true;
        else if (action == "zoom_in") st.requestZoomIn = true;
        else if (action == "zoom_out") st.requestZoomOut = true;
        // PRD R5 / D3: `[`/`]`, the alternate path to the ⌃⌥-drag gesture
        // (ui/MacPaintUI.cpp's canvas block). Applied directly, right here,
        // the same as mirror/reset_rotation/toggle_grayscale just below --
        // no layout dependency, so no request-flag round trip through the
        // canvas Begin()/End() block is needed. `bracketStepForRadius()`
        // reads the CURRENT radius so the step scales with it (PRD's own
        // words: "a constant is wrong across a 1..200 range"), and
        // `clampBrushRadius()` is the exact same clamp the gesture uses --
        // one range, not two.
        else if (action == "size_down")
          st.brush.radius = np::clampBrushRadius(
              st.brush.radius - np::bracketStepForRadius(st.brush.radius));
        else if (action == "size_up")
          st.brush.radius = np::clampBrushRadius(
              st.brush.radius + np::bracketStepForRadius(st.brush.radius));
        else if (action == "mirror_x") st.view.mirrorX = !st.view.mirrorX;
        else if (action == "mirror_y") st.view.mirrorY = !st.view.mirrorY;
        else if (action == "reset_rotation") st.view.rotation = 0.0f;
        // track11/pan-rotate-reset: the whole-view reset (Shift+Cmd+0) --
        // app/ZoomAndSize.hpp's `resetCanvasView()` has the field-by-field
        // reasoning for which six of CanvasView's eight fields this touches
        // (not grayscale/grade, which are preview toggles, not navigation).
        else if (action == "reset_view") st.view = np::resetCanvasView(st.view);
        else if (action == "toggle_grayscale") st.view.grayscale = !st.view.grayscale;
        // PLAN.md Phase 7 (PRD E1-E3, M1-M5). Request flags for the same
        // reason the zoom commands are: acting on a selection needs the
        // active OpenDocument and the sim, which are drawUI()'s to reach.
        else if (action == "select_all") st.requestSelectAll = true;
        else if (action == "deselect") st.requestDeselect = true;
        else if (action == "reselect") st.requestReselect = true;
        else if (action == "invert_selection") st.requestInvertSelection = true;
        else if (action == "copy") st.requestCopy = true;
        else if (action == "copy_merged") st.requestCopyMerged = true;
        else if (action == "cut") st.requestCut = true;
        else if (action == "paste") st.requestPaste = true;
        else if (action == "delete_selection") st.requestDeleteSelection = true;
        // D1 (reachability audit): ⌘Z/⇧⌘Z did not exist as keymap actions at
        // all -- there was no "undo"/"redo" name for `resolve()` to return,
        // so the chord had nowhere to go even before asking whether anything
        // consumed it. Same request-flag shape as the selection commands
        // just above and for the identical reason: undoing has to settle
        // wet paint first (app/StrokeBake.hpp section 4), which needs the
        // sim, which main.cpp's key handler does not have a document to aim
        // it at yet. ui/MacPaintUI.cpp's `moveHistoryCursor()` is the one
        // place that happens, and the title-bar buttons and the Edit menu's
        // Undo/Redo call it too -- see AppState::requestUndo's comment.
        else if (action == "undo") st.requestUndo = true;
        else if (action == "redo") st.requestRedo = true;
        // PLAN.md Phase 2 step 12 ("Rulers, guides, grid and snapping",
        // PRD Q5-Q7): guides/snapping/grid toggles, matching
        // docs/shortcuts.md section 3's Cmd+; / Cmd+Shift+; / Cmd+'.
        // Rulers deliberately has NO entry here and no keymaps/default.json
        // binding at all -- docs/shortcuts.md assigns rulers to Cmd+R, but
        // that chord is already bound to reload_shaders above (Phase 1,
        // predating both this step and step 11). Adding a second Cmd+R
        // binding for rulers would either trip Keymap's own load-time
        // conflict detector or silently make one of the two actions
        // unreachable depending on resolution order -- neither acceptable.
        // This is a known, pre-existing inconsistency between
        // docs/shortcuts.md and the already-shipped keymap, not something
        // to silently paper over by picking a different key; rulers are
        // menu-only (View > Rulers in MacPaintUI.cpp) until a product
        // decision resolves the conflict.
        else if (action == "toggle_guides") st.showGuides = !st.showGuides;
        else if (action == "toggle_snapping") st.snappingEnabled = !st.snappingEnabled;
        else if (action == "toggle_grid") st.showGrid = !st.showGrid;
      }
    }

    // ---- the quit guard (app/QuitSequence) ---------------------------------
    //
    // Serviced once per frame, right after the events that can raise it. Before
    // this, every one of the four ways out of this application set `st.quit`
    // directly and none of them looked at a document, so quitting with three
    // painted, unsaved documents open discarded all three and said nothing --
    // and, because a clean shutdown removes the recovery scratch directory
    // (PRD O8), it deleted the journal's copy of them on the way past.
    //
    // **Why this cannot hang `--screenshot`, and therefore the golden
    // harness.** Two independent reasons, either of which is sufficient:
    //
    //  1. The guard reads `st.requestQuit` and never `st.quit`. The capture
    //     block further down this loop writes `st.quit` and never
    //     `st.requestQuit`. There is no expression in either direction, so a
    //     `--screenshot` run's exit cannot pass through a document question at
    //     all -- it is not a check that happens to pass, it is a check that is
    //     not on that path.
    //  2. The branch below exits at once whenever `--screenshot` was passed.
    //     That is not belt-and-braces: SDL turns SIGINT and SIGTERM into
    //     `SDL_EVENT_QUIT`, so a harness that times out and signals the process
    //     really can set `requestQuit` in a capture run, and a modal raised in
    //     response would make the process unkillable by signal.
    if (st.requestQuit) {
      st.requestQuit = false;
      if (screenshotPath != nullptr) {
        st.quit = true;
      } else {
        const np::QuitStep step = np::beginQuit(st.documents, st.quitSequence, st.pendingClose);
        if (step.exitNow) st.quit = true;
        // Only ever a refusal ("something is already waiting for an answer"),
        // and the modal that refused it is on screen saying so. stderr rather
        // than the status line beside the menus because that string is
        // ui/MacPaintUI.cpp's own file-local state, and because this is the
        // same channel the journal's own problems already use.
        if (!step.status.empty()) std::fprintf(stderr, "[quit] %s\n", step.status.c_str());
      }
    }

    const auto now = std::chrono::steady_clock::now();
    st.frameMs = std::chrono::duration<float, std::milli>(now - prev).count();
    prev = now;

    gpu.tick();

    ImGui_ImplWGPU_NewFrame();
    ImGui_ImplSDL3_NewFrame();

    // --pen-demo: drive the *interactive* stroke path with synthetic pointer
    // input, so the thing under test is `ui/MacPaintUI`'s canvas block and its
    // routing branch rather than `app/StrokeSession` called directly.
    //
    // This is the difference from `--pigment-stroke-demo`, which constructs a
    // session itself: that one proves the deposit works, this one proves
    // something *calls* it. The distinction is exactly the one that made
    // commit bd30c2c ship a font module nothing invoked -- a test that
    // constructs its own subject proves the subject works, not that anything
    // reaches it.
    //
    // Injected after `ImGui_ImplSDL3_NewFrame()` and before `NewFrame()`,
    // which is the window in which ImGui accepts queued input events for the
    // frame about to be built.
    if (penDemo) {
      const int step = static_cast<int>(frameIndex) - kPenDemoFirstFrame;
      if (step >= 0 && step <= kPenDemoSteps) {
        const float u = static_cast<float>(step) / static_cast<float>(kPenDemoSteps);
        ImGuiIO& io = ImGui::GetIO();
        // A diagonal across the middle half of the canvas band, found the same
        // way the chrome finds it -- `ui/AtelierLayout` -- rather than from
        // hard-coded screen coordinates that would stop being over the canvas
        // the moment a band's height changed.
        const np::AtelierBands bands =
            np::atelierLayout(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y,
                              /*showTabStrip=*/true);
        const ImVec2 from(bands.canvas.x + bands.canvas.w * 0.25f,
                          bands.canvas.y + bands.canvas.h * 0.25f);
        const ImVec2 to(bands.canvas.x + bands.canvas.w * 0.75f,
                        bands.canvas.y + bands.canvas.h * 0.75f);
        io.AddMousePosEvent(from.x + (to.x - from.x) * u, from.y + (to.y - from.y) * u);
        // Down on the first step and held; released one step past the end, so
        // the pen-up branch runs on a frame the pointer is still over the
        // canvas rather than on the frame the demo happens to stop moving.
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, step < kPenDemoSteps);
      }
    }

    // --- the screenshot path takes no mouse ---------------------------------
    //
    // **`--screenshot` photographs a real window, and a real window is under
    // wherever the operator's physical pointer happens to be sitting.** The
    // SDL backend feeds that position to ImGui every frame, so hover states --
    // a tab's close button lighting up, a row tinting, the brush cursor ring
    // following the pointer across the canvas -- land in the capture or not
    // depending on something no test controls.
    //
    // Measured, not theorised: with this suppression absent, the `toolbar`
    // golden view matched exactly in five of six launches and differed in the
    // sixth by 4 px at max channel diff 23, the pixels reading *lighter*
    // (rgb(54,52,51) against rgb(38,36,35)) -- an ImGui hover tint on the tab
    // strip, roughly a one-in-six failure on a view whose threshold is exact
    // equality. The same cause put a brush cursor ring in the `canvas` view's
    // frame, which cost that view its exact-zero threshold until the ring was
    // cropped out.
    //
    // `(-FLT_MAX, -FLT_MAX)` is ImGui's own documented "no mouse anywhere"
    // sentinel (imgui.h, `AddMousePosEvent`), so this is not a hack position
    // off the edge of a particular window size -- it is the value that means
    // the pointer is nowhere.
    //
    // Deliberately NOT applied when a demo is driving the pointer itself:
    // `--pen-demo` exists precisely to inject positions, and neutralising the
    // mouse after it queued one would erase the input under test. The guard is
    // the flag rather than "did anything call AddMousePosEvent this frame",
    // because the former is checkable and the latter is not.
    if (screenshotPath != nullptr && !penDemo) {
      ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }

    ImGui::NewFrame();
    const uint64_t newFrameNs = frameTrace ? SDL_GetTicksNS() : 0;

    // ---- the stroke bridge: dried paint moves into the document -----------
    //
    // **This call must stay above drawUI(), and the reason is not obvious.**
    // ui/MacPaintUI draws the solver canvas and the document as two stacked
    // quads, and ui/DocumentTexture caches its upload on
    // OpenDocument::revision, which drawUI() reads. Baking here bumps that
    // revision before the read, so the document uploads this frame, and the
    // solver clear that goes with it is submitted long before present -- both
    // pictures change in the same presented image.
    //
    // Move this below drawUI() and it still compiles, still passes every
    // headless assertion, and puts a one-frame dropout on every stroke that
    // dries: the clear lands (a texture view samples the latest GPU write)
    // while the document upload waits for next frame's cache key. The full
    // argument, including the double-render failure in the other direction,
    // is app/StrokeBake.hpp section 1.
    if (sim) {
      st.bakeCycle.step(gpu, *sim, st.documents.active(), sim->mode(), frameIndex);
    }

    // --frame-trace: revision of the active document immediately before and
    // after drawUI() -- drawUI() is where a layer edit (e.g. the eye icon)
    // both gets recorded (bumping this) and where ui/DocumentTexture reads
    // that bump to decide whether/how to recompose, so a revision change here
    // is "this frame is the one that processed the click."
    const uint64_t revisionBeforeUI =
        frameTrace && st.documents.active() ? st.documents.active()->revision : 0;

    np::drawUI(st, sim, gpu, lut, kCanvasW, kCanvasH);

    const uint64_t drawUiNs = frameTrace ? SDL_GetTicksNS() : 0;
    const uint64_t revisionAfterUI =
        frameTrace && st.documents.active() ? st.documents.active()->revision : 0;

    if (frameTrace) {
      ImDrawList* fg = ImGui::GetForegroundDrawList();
      char buf[64];
      std::snprintf(buf, sizeof(buf), "frame %llu", (unsigned long long)frameIndex);
      fg->AddText(ImVec2(12.0f, 12.0f), IM_COL32(0, 255, 0, 255), buf);
      if (revisionAfterUI != revisionBeforeUI) {
        fg->AddText(ImVec2(12.0f, 30.0f), IM_COL32(255, 220, 0, 255), "REVISION CHANGED");
      }
      if (frameStartNs < clickMarkerUntilNs) {
        fg->AddCircle(clickMarkerPos, 24.0f, IM_COL32(255, 32, 32, 255), 32, 3.0f);
        fg->AddCircle(clickMarkerPos, 8.0f, IM_COL32(255, 32, 32, 255), 16, 2.0f);
      }
    }

    // **The one place the mouse cursor is set** (ui/ToolCursor §6). After
    // `drawUI()`, because the canvas's request is produced inside it and every
    // panel has by now told ImGui what it wants; before `Render()`, because
    // both inputs -- `np::canvasCursorRequest()` and `ImGui::GetMouseCursor()`
    // -- are this frame's answers and `NewFrame()` resets the second one.
    //
    // Unconditional. A frame in which the pointer is over a panel passes
    // `nullopt` and `apply()` honours ImGui's own request, which is what stands
    // in for the backend call suppressed by
    // `ImGuiConfigFlags_NoMouseCursorChange` above. Making this call
    // conditional on the canvas having asked for something is exactly how the
    // panels would lose their cursors.
    //
    // The second argument is ui/ToolCursor.hpp §7's addition: the same
    // frame's request as a `ToolCursor` intent, so `apply()` can pick the
    // per-tool bitmap over the system shape above. That is now the normal
    // path -- `bitmapCursorsEnabled()` defaults to true -- and the system
    // shape is the fallback for a bitmap that failed to rasterise.
    cursors.apply(np::canvasCursorRequest(), np::canvasCursorToolRequest());

    ImGui::Render();
    const uint64_t renderNs = frameTrace ? SDL_GetTicksNS() : 0;

    // ---- simulation ----
    //
    // **Colour from the foreground, constants from the pigment**, and the
    // split is the honest half of PRD Q10 rather than an inconsistency. This
    // used to read `defaultPalette()[st.brush.pigment]` for both, which was the
    // same thing until `BrushState` gained a second way to say a colour; had it
    // been left alone, an eyedropper pick would have changed what the brush and
    // the bucket lay down while the solver went on injecting the old pigment --
    // three tools disagreeing about the foreground.
    //
    // The constants genuinely cannot follow a picked colour: density, staining
    // and granulation are measurements of a real paint (paint/Palette.cpp,
    // Curtis et al. 1997 Table 1) and there is no function from three floats to
    // "how does this settle out of suspension". They therefore keep following
    // `st.brush.pigment` in both modes, which is exactly what the COLOR panel
    // tells the user in RGB mode rather than leaving it to be discovered by a
    // wash that granulates unexpectedly. `foregroundPhysicalConstants()` is
    // also the bounds-checked read `defaultPalette()[st.brush.pigment]` was not.
    const std::array<float, 3> fg = np::foregroundSrgb(st.brush);
    const auto& pig = np::foregroundPhysicalConstants(st.brush);
    const np::Latent z = lut.rgbToLatent(fg[0], fg[1], fg[2]);
    for (int i = 0; i < 3; ++i) {
      st.sim.brushLatentC[i] = z.c[i];
      st.sim.brushLatentR[i] = z.res[i];
    }
    // Physical constants follow the selected paint, not a global slider, so
    // switching from Phthalo Blue to Ultramarine actually changes behaviour.
    st.sim.density = pig.density;
    st.sim.staining = pig.staining;
    st.sim.granulation = pig.granulation;
    st.sim.frame = frame++;

    // Arc-length dab emission (1.3, ADR-0003): deposit whatever dabs
    // MacPaintUI's emitter produced this render frame *before* running
    // physics below, so the freshly-laid pigment gets to participate in
    // this frame's advection/diffusion immediately, same as everything
    // already on the canvas. Each dab is its own small, self-contained
    // dispatch (PaintSim::depositDab()) — a fast stroke emitting ten dabs
    // this frame means ten cheap splat dispatches, not ten trips through
    // frame()'s Jacobi solve. Oil doesn't deposit this way (see
    // depositDab()'s doc comment and the loop below), and nothing here
    // should run while paused. Guarded on `sim` existing at all (1.4 /
    // ADR-0001): st.pendingDabs can only be non-empty once MacPaintUI has
    // already constructed the sim (see drawUI's stroke-start block), so
    // this check is defensive rather than load-bearing -- but frame()
    // below genuinely has nothing to step before that first construction.
    if (sim) {
      if (!st.paused && sim->mode() != np::PaintMode::Oil) {
        for (const auto& d : st.pendingDabs) sim->depositDab(gpu, st.sim, d.x, d.y);
      }

      if (st.paused) st.sim.brushActive = 0;
      if (!st.paused || st.sim.brushActive) {
        // Fixed timestep (PRD H7): run however many kFixedDt ticks the
        // accumulator has banked, not exactly one. `steps` can be 0 (a very
        // fast frame hasn't banked a full tick yet) up to kMaxStepsPerFrame
        // (catching up after a stall) — see app/FixedStep.hpp for the two
        // independent caps involved.
        const int steps = np::consumeFixedSteps(fixedStepAcc, st.frameMs, np::kFixedDtMs,
                                                 np::kMaxCatchUpMs, np::kMaxStepsPerFrame);

        // Post-1.3, this guard exists purely for OIL. Watercolour and ink no
        // longer read brushActive/brushA/B inside frame() at all — their
        // deposition happens above, once per emitted dab, entirely outside
        // this loop — so for those two modes the zeroing below is inert
        // bookkeeping, not a gate. Oil's contact -> velocity -> transfer
        // pipeline (kOilSplat/kOilTransfer/kOilBrush) is still driven
        // through frame() itself, reading whatever (lastDab -> newestDab)
        // segment MacPaintUI set in st.sim this render frame. Replaying
        // that same segment on every one of `steps` substeps would
        // multiply oil's contact-driven transfer by however many physics
        // ticks this frame happened to run, so — same convention this loop
        // has used since 1.2 — only the first substep carries the real
        // flags; the rest run with both cleared, advancing oil's
        // levelling/advection only.
        const uint32_t brushActiveThisFrame = st.sim.brushActive;
        const uint32_t brushReloadThisFrame = st.sim.brushReload;
        for (int i = 0; i < steps; ++i) {
          st.sim.brushActive = (i == 0) ? brushActiveThisFrame : 0;
          st.sim.brushReload = (i == 0) ? brushReloadThisFrame : 0;
          sim->frame(gpu, st.sim);
        }
        st.sim.brushActive = brushActiveThisFrame;
        st.sim.brushReload = brushReloadThisFrame;
      }

      // Grayscale preview (PRD Q3): must run *after* the frame()/depositDab
      // calls above, not from inside drawUI (which ran earlier this same
      // iteration, before any of this frame's physics/composite work was
      // even submitted). ImGui's AddImageQuad call in MacPaintUI.cpp only
      // records a texture *view* handle into the draw list -- what it
      // actually samples at present time is whatever the GPU last wrote
      // into that view, which depends on GPU *submission* order, not CPU
      // recording order. Submitting this blit here, after frame()'s own
      // submission, guarantees it reads this frame's fresh composite rather
      // than the previous frame's. Skipped whenever the toggle is off, so
      // it costs nothing then.
      if (st.view.grayscale) sim->updateGrayscalePreview(gpu);
      // Apply pass (PLAN.md Phase 3 step 6): same ordering requirement as
      // the grayscale preview immediately above -- must run after this
      // frame's frame()/depositDab() submissions, not from inside drawUI
      // (which ran earlier this same iteration). Skipped whenever the
      // toggle is off, so it costs nothing then.
      if (st.view.grade) sim->updateGradePreview(gpu, st.opStack);
    }

    // ---- present ----
    WGPUSurfaceTexture surfaceTex = {};
    wgpuSurfaceGetCurrentTexture(gpu.surface, &surfaceTex);
    if (!surfaceTex.texture) continue;

    WGPUTextureView backbuffer = wgpuTextureCreateView(surfaceTex.texture, nullptr);

    WGPURenderPassColorAttachment att = {};
    att.view = backbuffer;
    att.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    att.loadOp = WGPULoadOp_Clear;
    att.storeOp = WGPUStoreOp_Store;
    att.clearValue = {0.07, 0.07, 0.075, 1.0};

    WGPURenderPassDescriptor rp = {};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;

    // The document quads' vertices and bind groups, after the UI has been built
    // (so every quad is queued) and before it is rendered (so the callbacks
    // have something to bind). See ui/CanvasQuad.hpp on ordering.
    np::flushCanvasQuads(gpu);

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(gpu.queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);
    // The submit holds its own reference to everything it consumed, so the
    // per-frame bind groups can go now rather than being cached.
    np::endCanvasQuadFrame();

    // --screenshot / F12: between the UI's submission and the present, which
    // is the only window where the backbuffer both holds this frame's UI and
    // is still readable. app/Screenshot.hpp explains why the app photographs
    // itself instead of being photographed.
    if (st.requestScreenshot || (screenshotPath != nullptr && frameIndex == screenshotFrames)) {
      const std::string shotPath =
          st.requestScreenshot && screenshotPath == nullptr ? st.screenshotPath : screenshotPath;
      std::string shotError;
      if (np::captureSurfaceToPng(gpu, surfaceTex.texture, gpu.width, gpu.height, shotPath,
                                  &shotError))
        std::printf("[screenshot] wrote %s (%ux%u)\n", shotPath.c_str(), gpu.width, gpu.height);
      else
        std::fprintf(stderr, "[screenshot] %s\n", shotError.c_str());
      st.requestScreenshot = false;
      if (screenshotPath != nullptr) st.quit = true;  // --screenshot is capture-and-exit
    }
    ++frameIndex;

    wgpuSurfacePresent(gpu.surface);

    if (frameTrace) {
      const uint64_t presentNs = SDL_GetTicksNS();
      // Which pool slot (if any) holds the active document, so the trace can
      // report *why* this frame's composite was full or narrow -- not just
      // how long it took. `--frame-trace`'s whole point is telling "the
      // toggled layer's own footprint legitimately covers the canvas" apart
      // from "the narrowing logic missed a case it should have caught".
      const char* fullReasonName = "n/a";
      size_t dirtyTiles = 0;
      uint64_t uploadedTexels = 0;
      double compositeMs = 0.0, packMs = 0.0, lastUploadMs = 0.0;
      if (const np::OpenDocument* od = st.documents.active()) {
        const np::DocumentTexturePool& pool = np::canvasDocumentTexture();
        for (size_t s = 0; s < np::kVisibleDocumentCap; ++s) {
          if (pool.slotDocument(s) == od->id) {
            fullReasonName = np::fullRecompositeReasonName(pool.slot(s).lastFullRecompositeReason());
            dirtyTiles = pool.slot(s).lastDirtyTiles();
            uploadedTexels = pool.slot(s).lastUploadedTexels();
            compositeMs = pool.slot(s).lastCompositeMs();
            packMs = pool.slot(s).lastPackMs();
            lastUploadMs = pool.slot(s).lastUploadMs();
            break;
          }
        }
      }
      std::fprintf(stderr,
                    "[frame-trace] frame=%llu%s rev=%llu->%llu poll_to_newframe_ms=%.2f "
                    "drawui_ms=%.2f render_ms=%.2f present_ms=%.2f total_ms=%.2f "
                    "reason=%s dirty_tiles=%zu uploaded_texels=%llu upload_total_ms=%.2f "
                    "composite_ms=%.2f pack_ms=%.2f upload_calls_ms=%.2f\n",
                    static_cast<unsigned long long>(frameIndex - 1), clickedThisFrame ? " CLICK" : "",
                    static_cast<unsigned long long>(revisionBeforeUI),
                    static_cast<unsigned long long>(revisionAfterUI),
                    static_cast<double>(newFrameNs - frameStartNs) / 1e6,
                    static_cast<double>(drawUiNs - newFrameNs) / 1e6,
                    static_cast<double>(renderNs - drawUiNs) / 1e6,
                    static_cast<double>(presentNs - renderNs) / 1e6,
                    static_cast<double>(presentNs - frameStartNs) / 1e6, fullReasonName, dirtyTiles,
                    static_cast<unsigned long long>(uploadedTexels), lastUploadMs, compositeMs, packMs,
                    lastUploadMs - compositeMs - packMs);
    }
    // st.paintingThisFrame (not st.sim.brushActive): post-1.3 brushActive
    // means "oil has a fresh dab-sourced segment," true on only a fraction
    // of painting frames — the wrong thing to correlate pen-to-photon
    // latency against. paintingThisFrame is the direct "was the user
    // painting, hovered, in bounds this frame" signal the old brushActive
    // used to carry.
    latency.recordFrame(st.paintingThisFrame, st.lastInputEventNs, SDL_GetTicksNS());
    if (strokeWasActive && !st.strokeActive) latency.endStroke();
    strokeWasActive = st.strokeActive;

    // The recovery journal's timer (PLAN.md Phase 4 step 9, PRD O5, O6, O10).
    //
    // Here, at the very end of the frame, rather than beside the simulation:
    // a journal write is synchronous on this thread today (see
    // app/Journal.hpp's interval arithmetic), so it must land *after*
    // latency.recordFrame() has already judged this frame -- otherwise the
    // one frame per minute that carries a journal write would be recorded as
    // a pen-to-photon outlier that has nothing to do with the pen.
    //
    // st.strokeActive is PRD O10's deferral: a write that would collide with
    // a stroke in progress is held back to the first frame after it ends.
    // Almost every call returns having done nothing but compare two integers
    // per open document.
    {
      const double nowSeconds =
          std::chrono::duration<double>(now.time_since_epoch()).count();
      const np::JournalTickResult journalled =
          st.journal.tick(st.documents, {nowSeconds, st.strokeActive});
      for (const std::string& e : journalled.errors)
        std::fprintf(stderr, "[journal] %s\n", e.c_str());
    }

    wgpuTextureViewRelease(backbuffer);
    wgpuTextureRelease(surfaceTex.texture);
  }

  // A clean shutdown removes the scratch directory, so this run leaves
  // nothing to be offered for recovery next launch (PRD O8 -- the offer must
  // mean something, which requires that a normal exit never produces one).
  // Only this path removes it: JournalSession's destructor deliberately does
  // not, so any exit that does not reach here still leaves the work.
  {
    std::string journalError;
    if (!st.journal.finishClean(&journalError))
      std::fprintf(stderr, "[journal] %s\n", journalError.c_str());
  }

  // UI detour step 2: what the revision cache saved over this session's real
  // frames. `--selftest` benchmarks the same code, which shows the composite is
  // expensive; this shows how seldom it was actually paid. Printed on every
  // interactive exit, including the one --screenshot takes, so the picture and
  // the number come out of the same command.
  {
    const np::DocumentTexturePool& docTex = np::canvasDocumentTexture();
    const uint64_t served = docTex.uploads() + docTex.cacheHits();
    std::printf("[document-texture] %llu frame(s) served: %llu composite+upload totalling "
                "%.1f ms, %llu from cache\n",
                static_cast<unsigned long long>(served),
                static_cast<unsigned long long>(docTex.uploads()), docTex.totalUploadMs(),
                static_cast<unsigned long long>(docTex.cacheHits()));

    // The canvas pipeline's own tally beside it (ui/CanvasQuad): a dropped
    // quad is a document that silently did not draw, so it is reported even
    // when it is zero rather than only when something has already gone wrong.
    std::printf("[canvas-quad] %zu document quad(s) drawn, %zu dropped\n", np::canvasQuadsDrawn(),
                np::canvasQuadsDropped());
  }

  // --- what `--split-demo` is worth ---------------------------------------
  //
  // Here, after the loop, rather than beside the capture inside it: the file
  // has to be closed before it can be read back, `--screenshot` sets `st.quit`
  // on the frame it captures, and the frame loop is no place for a full image
  // decode. The window is still alive, which is what makes its *logical* size
  // available -- ImGui's `DisplaySize` is exactly `SDL_GetWindowSize()`, and
  // the layout constants the verifier reconstructs are logical (the decoded
  // PNG is 2x that on this display; ui/AtelierLayout's rects are not).
  //
  // The exit code is the point. `--selftest` is the project's assertion
  // harness and this is not one of its sections -- it cannot be, because it
  // needs a window, a swapchain, thirty rendered frames and a file on disk,
  // which is the opposite of what every section in src/app/selftest is. So the
  // flag carries its own verdict out through the process's status instead, and
  // `--split-demo --screenshot out.png` is a command a script can fail on.
  int exitCode = 0;
  if (splitDemo) {
    if (screenshotPath != nullptr) {
      int logicalW = 0, logicalH = 0;
      SDL_GetWindowSize(window, &logicalW, &logicalH);
      if (!verifySplitDemoScreenshot(screenshotPath, splitDemoMode,
                                     static_cast<float>(logicalW), static_cast<float>(logicalH)))
        exitCode = 1;
    } else {
      // Not silent: a --split-demo run with no --screenshot has photographed
      // nothing and therefore asserted nothing, and a flag that reported
      // nothing would read exactly like one that had passed.
      std::printf("[split-demo] no --screenshot, so nothing was verified\n");
    }
  }

  np::shutdownCanvasQuad();
  // Before `SDL_Quit()` below, which tears down the video subsystem that owns
  // these handles -- destroying them afterwards is a use-after-free. Placed
  // with the other shutdowns rather than at the end so the whole teardown
  // sequence reads in one block, and paired with the `cursors.create()` beside
  // `ImGui_ImplWGPU_Init()`.
  cursors.destroy();
  ImGui_ImplWGPU_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  if (sim) sim->shutdown();
  gpu.shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  // Non-zero only when a verification above actually ran and failed; every
  // other path leaves it 0, so no existing invocation's exit code changes.
  return exitCode;
}
