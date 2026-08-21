// naturalPaint — real-time watercolour on WebGPU.
//
//   Curtis et al. 1997   shallow-water + pigment transport + capillary layer
//   Stam 1999            semi-Lagrangian advection, Jacobi projection
//   Sochorova & Jamriska 2021  Mixbox latent-space pigment mixing
//
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "app/AppState.hpp"
#include "app/FixedStep.hpp"
#include "app/Keymap.hpp"
#include "app/Latency.hpp"
#include "app/Memory.hpp"
#include "app/Screenshot.hpp"
#include "app/SelfTest.hpp"
#include "core/Blend.hpp"
#include "app/LayerEditor.hpp"
#include "app/LayerPanel.hpp"
#include "core/LayerOps.hpp"
#include "core/Tile.hpp"
#include "core/LayerCompOps.hpp"
#include "gfx/Context.hpp"
#include "paint/Palette.hpp"
#include "sim/PaintSim.hpp"
#include "ui/MacPaintUI.hpp"
#include "ui/Theme.hpp"

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
  np::setLayersPanelSelection(adjustment);

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
  np::setLayersPanelSelection(selected);
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
  np::setLayersPanelSelection(selected);
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
      if (e.paxis.axis == SDL_PEN_AXIS_PRESSURE) {
        st.penSeen = true;
        st.penPressure = std::clamp(e.paxis.value, 0.0f, 1.0f);
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

}  // namespace

int main(int argc, char** argv) {
  // --selftest [out.png] runs the solver headless and checks that latent-space
  // pigment mixing actually produces green where blue crosses yellow.
  const char* selfTestOut = nullptr;
  bool selfTest = false;
  float diagSeconds = 0.0f;
  bool modeTest = false;
  bool latencyVerbose = false;
  const char* screenshotPath = nullptr;
  int screenshotFrames = 30;
  bool demoDocument = false;
  bool compsDemo = false;
  size_t compsDemoRestore = 0;
  bool compsDemoDrop = false;
  bool uiLayerDemo = false;
  bool uiLayerDemoClip = true;
  const char* uiMergeDemo = nullptr;
  bool controlsAllOpen = false;
  const char* controlsScrollTo = nullptr;
  bool openLayerMenu = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a(argv[i]);
    if (a == "--selftest") {
      selfTest = true;
      if (i + 1 < argc && argv[i + 1][0] != '-') selfTestOut = argv[++i];
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
    } else if (a == "--ui-layer-demo") {
      // UI detour step 3: build a stack through the layer editor's own
      // commands. See runUiLayerDemo(). `noclip` runs the same script without
      // the clip, which is the comparison shot for PRD C9 on screen.
      uiLayerDemo = true;
      if (i + 1 < argc && std::string_view(argv[i + 1]) == "noclip") {
        uiLayerDemoClip = false;
        ++i;
      }
    } else if (a == "--ui-merge-demo") {
      // Phase 5 step 10 / PRD C10: press one merge button. See runUiMergeDemo().
      if (i + 1 < argc && argv[i + 1][0] != '-') uiMergeDemo = argv[++i];
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
    }
  }

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

  np::GpuContext gpu;
  if (!gpu.init(window)) return 1;

  np::MixboxLut lut;
  if (!lut.load(NP_MIXBOX_LUT)) {
    std::fprintf(stderr, "Could not load the Mixbox LUT. Expected it at:\n  %s\n",
                 NP_MIXBOX_LUT);
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
    // Phase 2 step 8: ui/NaturalPaintUI -- Document -> per-tile GPU texture
    // -> screen, a read-only proof of the tile pipeline independent of the
    // interactive painting canvas. Needs `gpu` for a real device/queue, but
    // no PaintSim -- this module never touches the solver.
    const bool tiledViewportOk = np::runTiledViewportTest(gpu);
    // Phase 2 step 9: ui/NaturalPaintUI's mip pyramid -- CPU-side box-filter
    // downsample correctness, the zoom->level formula, and an end-to-end
    // GPU proof that draw()'s level pick actually changes which texels
    // render. Needs `gpu` for the end-to-end part only -- see SelfTest.hpp.
    const bool mipPyramidOk = np::runMipPyramidTest(gpu);
    // Phase 2 step 10 (narrow, Document-level slice; PRD Q10): core/Probe's
    // probePixel() -- linear + display readout, NxN sample-size averaging,
    // sample-all-layers as a parameter of the sample rather than a separate
    // tool, and premultiply-aware un-premultiplication on read. Also
    // headless and GPU-free -- pure CPU, no PaintSim involvement.
    const bool probeOk = np::runProbeTest();
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
    const bool histogramOk = np::runHistogramTest();
    // Phase 3 steps 2+3 (ops/PointOps; docs/operations.md §1.1; PRD B4):
    // Levels, Curves (through color/Shaper), Exposure, Saturation,
    // RGB->grayscale, channel mixer -- each a plain rgb->rgb function --
    // plus the un-premultiply/re-premultiply wrapper bracketing them.
    // Also headless and GPU-free -- pure CPU math, no PaintSim involvement.
    const bool pointOpsOk = np::runPointOpsTest();
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
    // Phase 3 step 8 ("Op-stack UI... and a curve widget operating in the
    // shaper domain"): app/CurveEdit.hpp's pure screen<->curve-space
    // geometry and list-mutation math -- everything the interactive curve
    // widget (ui/MacPaintUI.cpp) calls into. Also headless and GPU-free --
    // pure CPU, no PaintSim involvement.
    const bool curveEditOk = np::runCurveEditTest();
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
    // 1.3 / ADR-0003: deposited mass must match regardless of stroke speed.
    const bool strokeSpeedOk = np::runStrokeSpeedTest(gpu, *s, lut);
    // 1.4 / ADR-0001 bullet 5: idle RSS, measured before this branch (or
    // any other) ever constructed a PaintSim.
    const bool idleMemOk = np::runIdleMemoryTest(idleRssBytes);
    const bool ok = pigmentOk && accumulatorOk && colorSpaceOk && shaperOk && keymapOk &&
                    tileStoreOk && imageDecodeOk && documentOk && baseLayerAlphaOk &&
                    createBlankOk && imageIOOk && placeImageAsLayerOk && probeOk &&
                    tiledViewportOk && mipPyramidOk && viewTransformOk && guidesGridSnapOk &&
                    histogramOk && pointOpsOk && opStackOk && lutBakeOk && applyPassOk &&
                    curveEditOk && exportOk && formatSupportOk && npaintOk && tileResidencyOk &&
                    exportAsOk && documentLifecycleOk && recoveryJournalOk && layerStackOk &&
                    blendOk && pigmentLayerOk && layerMaskOk && adjustmentLayerOk &&
                    cowTileOk && historyOk && historyPanelOk && clippingMaskOk &&
                    documentTextureOk && layerEditorOk && controlsLayoutOk && incrementalCompositeOk && mergeFamilyOk && layerCompOk &&
                    strokeSpeedOk && idleMemOk && fieldAllocOk && fontsOk;
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
  np::applyMacPaintDarkTheme();

  ImGui_ImplSDL3_InitForOther(window);
  ImGui_ImplWGPU_InitInfo wgpuInit;
  wgpuInit.Device = gpu.device;
  wgpuInit.NumFramesInFlight = 3;
  wgpuInit.RenderTargetFormat = gpu.surfaceFormat;
  wgpuInit.DepthStencilFormat = WGPUTextureFormat_Undefined;
  ImGui_ImplWGPU_Init(&wgpuInit);

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
                 NP_KEYMAP_DIR);
  }

  np::AppState st;

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
  // After --demo-document deliberately: the script builds on whatever the
  // document already holds, and the layer it clips to is the one that was on
  // top when it started -- which is a layer with pixels in it exactly when the
  // fixture above has run.
  st.controlsAllOpen = controlsAllOpen;
  st.openLayerMenu = openLayerMenu;
  if (controlsScrollTo != nullptr) st.controlsScrollTo = controlsScrollTo;
  if (uiLayerDemo) {
    if (np::OpenDocument* od = st.documents.active()) runUiLayerDemo(*od, uiLayerDemoClip);
  }
  // After both fixtures, deliberately: a merge is applied to whatever stack
  // the flags before it built, which is what makes the before/after pair a
  // pair.
  if (uiMergeDemo != nullptr) {
    if (np::OpenDocument* od = st.documents.active()) runUiMergeDemo(*od, uiMergeDemo);
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

  // Frame counter, used only by --screenshot: the first frames are not
  // representative (ImGui lays out docked panels on frame 1).
  uint64_t frameIndex = 0;
  while (!st.quit) {
    st.lastInputEventNs = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      ImGui_ImplSDL3_ProcessEvent(&e);
      handlePenEvent(st, e);
      // e.common.timestamp is when SDL generated the event, not when we
      // happened to drain the queue for it — using our own SDL_GetTicksNS()
      // here would understate latency by however long the event sat queued.
      if (isPointerSampleEvent(e)) st.lastInputEventNs = e.common.timestamp;

      if (e.type == SDL_EVENT_QUIT) st.quit = true;
      if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
          e.window.windowID == SDL_GetWindowID(window))
        st.quit = true;
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
        else if (action == "quit") st.quit = true;
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
        // selection" (PRD Q1) is also deliberately absent -- there is no
        // selection/mask concept anywhere in this codebase yet (that's
        // phase 7, PRD E1); binding it now would mean inventing fake
        // selection state just to give the key something to do.
        else if (action == "fit_window") st.requestFitWindow = true;
        else if (action == "zoom_100") st.requestZoom100 = true;
        else if (action == "zoom_in") st.requestZoomIn = true;
        else if (action == "zoom_out") st.requestZoomOut = true;
        else if (action == "mirror_x") st.view.mirrorX = !st.view.mirrorX;
        else if (action == "mirror_y") st.view.mirrorY = !st.view.mirrorY;
        else if (action == "reset_rotation") st.view.rotation = 0.0f;
        else if (action == "toggle_grayscale") st.view.grayscale = !st.view.grayscale;
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

    const auto now = std::chrono::steady_clock::now();
    st.frameMs = std::chrono::duration<float, std::milli>(now - prev).count();
    prev = now;

    gpu.tick();

    ImGui_ImplWGPU_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    np::drawUI(st, sim, gpu, lut, kCanvasW, kCanvasH);

    ImGui::Render();

    // ---- simulation ----
    const auto& pig = np::defaultPalette()[st.brush.pigment];
    const np::Latent z = lut.rgbToLatent(pig.rgb[0], pig.rgb[1], pig.rgb[2]);
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

    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(gpu.device, nullptr);
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(enc, &rp);
    ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);

    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, nullptr);
    wgpuQueueSubmit(gpu.queue, 1, &cmd);
    wgpuCommandBufferRelease(cmd);
    wgpuCommandEncoderRelease(enc);

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
    const np::DocumentTexture& docTex = np::canvasDocumentTexture();
    const uint64_t served = docTex.uploads() + docTex.cacheHits();
    std::printf("[document-texture] %llu frame(s) served: %llu composite+upload totalling "
                "%.1f ms, %llu from cache\n",
                static_cast<unsigned long long>(served),
                static_cast<unsigned long long>(docTex.uploads()), docTex.totalUploadMs(),
                static_cast<unsigned long long>(docTex.cacheHits()));
  }

  ImGui_ImplWGPU_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  if (sim) sim->shutdown();
  gpu.shutdown();
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
