#include "app/selftest/Support.hpp"

#include "app/DocumentLifecycle.hpp"
#include "app/LayerEditor.hpp"
#include "app/StrokeSession.hpp"
#include "paint/Palette.hpp"

namespace np {

// **The active layer, and the pen reaching it.**
//
// `app/StrokeSession.hpp` section 4 has said since it was written that "the pen
// is not wired to this yet, and that is a missing decision rather than missing
// plumbing. A deposit needs a target layer, and this application has **no
// concept of an active layer**." This section covers the two things that
// decision turned into: `OpenDocument::activeLayer` and `brushTipFor()`.
//
// What it deliberately does NOT cover is the wiring itself -- the branch in
// `ui/MacPaintUI.cpp` that picks a route and calls `begin`/`addPoint`/`end`.
// That branch needs a window, a pointer and a frame loop, and a headless test
// that constructed its own `StrokeSession` would prove exactly what
// `--pigment-stroke-demo` already proves and what commit bd30c2c showed is not
// enough: that the subject works, not that anything calls it. `--pen-demo`
// covers that, by dragging a synthetic pointer through the real UI.
bool runActiveLayerTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-58s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // --- Part A: the active layer ------------------------------------------
  {
    OpenDocument doc = makeBlankOpenDocument(256, 256, WorkingSpace{}, "A");
    check(doc.document.layers.size() == 1, "a blank document has one layer");
    check(activeLayerIndex(doc).value_or(999) == 0, "and it is the active one");
    check(activeLayerOf(doc) == &doc.document.layers[0], "which the pointer accessor agrees on");

    // Three more layers, then the clamp that is the reason the accessor exists.
    for (int i = 0; i < 3; ++i) applyLayerCommand(doc, LayerCommand::NewPigmentLayer, doc.activeLayer);
    check(doc.document.layers.size() == 4, "four layers");
    setActiveLayer(doc, 3);
    check(doc.activeLayer == 3, "the top one is active");

    // A shrink under a stored index. This is what a layer delete, an undo past
    // a layer add, and a comp restore all do, and the stored index is stale in
    // every one of them -- so the clamp is not a defensive nicety, it is the
    // normal case.
    doc.document.layers.resize(2);
    check(doc.activeLayer == 3, "the stored index is untouched by a shrink -- nothing watches it");
    check(activeLayerIndex(doc).value_or(999) == 1,
          "but the accessor clamps it into the stack, so a stroke cannot miss");
    check(activeLayerOf(doc) == &doc.document.layers[1], "and names the layer that is really there");

    // Setting past the end clamps rather than storing a value that would need
    // clamping again on every read.
    setActiveLayer(doc, 99);
    check(doc.activeLayer == 1, "setActiveLayer clamps on the way in");

    // The empty stack. `activeLayerIndex()` reports nothing rather than 0,
    // because row 0 of a stack with no rows is not a layer -- and a caller
    // that got 0 would index it.
    doc.document.layers.clear();
    check(!activeLayerIndex(doc).has_value(), "an empty stack has no active layer, not layer 0");
    check(activeLayerOf(doc) == nullptr, "and no layer to point at");
    setActiveLayer(doc, 0);
    check(!activeLayerIndex(doc).has_value(), "setting one on an empty stack is a no-op");
  }

  // --- Part B: it is per document, which is why it moved ------------------
  {
    // The panel used to hold one index for the whole application. Two
    // documents with different stacks is the case that makes that wrong, and
    // it is wrong *silently*: the second document just paints on a different
    // layer than its panel highlighted.
    OpenDocument a = makeBlankOpenDocument(64, 64, WorkingSpace{}, "A");
    OpenDocument b = makeBlankOpenDocument(64, 64, WorkingSpace{}, "B");
    for (int i = 0; i < 3; ++i) applyLayerCommand(a, LayerCommand::NewPigmentLayer, a.activeLayer);
    setActiveLayer(a, 3);
    setActiveLayer(b, 0);
    check(a.activeLayer == 3 && b.activeLayer == 0, "two documents hold two active layers");
    check(activeLayerIndex(a).value_or(999) == 3 && activeLayerIndex(b).value_or(999) == 0,
          "and switching between them cannot carry one's row into the other");
  }

  // --- Part C: it is NOT undone -------------------------------------------
  {
    // `core::History` entries hold a whole `Document`, so anything stored
    // there is restored by undo. The active layer is on the *session* record
    // for exactly this reason: undoing a paint stroke must not move the user's
    // selection as a side effect.
    OpenDocument doc = makeBlankOpenDocument(64, 64, WorkingSpace{}, "C");
    applyLayerCommand(doc, LayerCommand::NewPigmentLayer, doc.activeLayer);
    setActiveLayer(doc, 1);
    doc.document.layers[1].name = "painted";
    doc.recordEdit("brush stroke", EditKind::Content);
    setActiveLayer(doc, 0);
    check(doc.history.canUndo(), "there is a state to undo to");
    if (const Document* previous = doc.history.undo()) {
      doc.document = *previous;
      check(doc.document.layers[1].name != "painted", "undo really did restore the document");
      check(doc.activeLayer == 0,
            "and left the active layer where the user put it, not where the entry had it");
    }
  }

  // --- Part D: brushTipFor, the other half of the missing decision ---------
  {
    MixboxLut noLut;  // never loaded: the fallback path, and the test's default
    BrushState brush;
    brush.radius = 40.0f;
    brush.hardness = 0.5f;
    brush.load = 0.8f;
    brush.spacing = 0.3f;
    brush.pigment = 6;  // Ultramarine Blue, BrushState's own default

    // Full pressure is the identity, which is what makes a mouse (pressure
    // pinned to 1) behave as the sliders say.
    const BrushTip full = brushTipFor(brush, noLut, 1.0f);
    check(full.radius == brush.radius && full.flow == brush.load,
          "at full pressure the tip is the sliders, unmodified");
    check(full.hardness == brush.hardness && full.spacing == brush.spacing,
          "hardness and spacing do not vary with pressure at all");

    // The two curves, which used to be `bool pressureSize` / `pressureFlow`
    // with their ranges written in as literals, and are now two links in
    // `defaultBrushLinks()`. **Still asserted as the literal expressions**, at
    // every pressure rather than at one -- that is what makes the migration
    // off the booleans checkable rather than asserted in a comment. If the
    // default link set is not exactly the old formula, a pen that has felt one
    // way for the whole project's life quietly starts feeling another.
    bool exactEverywhere = true;
    for (int i = 0; i <= 20; ++i) {
      const float p = static_cast<float>(i) / 20.0f;
      const BrushTip t = brushTipFor(brush, noLut, p);
      if (std::fabs(t.radius - brush.radius * (0.25f + 0.75f * p)) > 1e-5f)
        exactEverywhere = false;
      if (std::fabs(t.flow - brush.load * (0.15f + 0.85f * p)) > 1e-5f) exactEverywhere = false;
    }
    check(exactEverywhere,
          "the default links ARE the old pressure curves, at every pressure -- 0.25+0.75p "
          "and 0.15+0.85p, so replacing the two booleans changed no feel");

    // The two are independent, because a pen configured for size-only must
    // feel the same on both routes. Expressed by removing a link rather than
    // by clearing a boolean; the rule under test is unchanged.
    removeLink(brush.links, DynamicSource::Pressure, DynamicTarget::Flow);
    const BrushTip sizeOnly = brushTipFor(brush, noLut, 0.5f);
    check(sizeOnly.flow == brush.load && sizeOnly.radius < brush.radius,
          "pressure -> flow removed leaves flow alone and still sizes");
    brush.links = defaultBrushLinks();
    removeLink(brush.links, DynamicSource::Pressure, DynamicTarget::Size);
    const BrushTip flowOnly = brushTipFor(brush, noLut, 0.5f);
    check(flowOnly.radius == brush.radius && flowOnly.flow < brush.load,
          "and the other way round");
    brush.links = defaultBrushLinks();

    // Out-of-range pressure is clamped rather than producing a negative
    // radius, which `dabCoverage()` would read as "deposits nothing".
    check(brushTipFor(brush, noLut, -3.0f).radius > 0.0f, "negative pressure clamps to zero, not to a negative tip");
    check(brushTipFor(brush, noLut, 9.0f).radius == brush.radius, "and pressure above 1 clamps to 1");

    // A palette index that is out of range picks entry 0 rather than reading
    // off the end. `BrushState::pigment` is a plain int on a public aggregate.
    brush.pigment = 9999;
    const BrushTip clamped = brushTipFor(brush, noLut, 1.0f);
    brush.pigment = 0;
    const BrushTip first = brushTipFor(brush, noLut, 1.0f);
    check(clamped.pigment.c[0] == first.pigment.c[0] &&
              clamped.pigment.c[1] == first.pigment.c[1] &&
              clamped.pigment.c[2] == first.pigment.c[2],
          "an out-of-range pigment index falls back to the first, not past the end");

    // The three physical constants deliberately do not travel into the tip --
    // see brushTipFor()'s own comment. Two pigments with the same RGB and
    // different constants therefore give the same tip, which is the honest
    // consequence and is asserted rather than left as a surprise.
    const std::vector<Pigment>& palette = defaultPalette();
    bool constantsVary = false;
    for (const Pigment& p : palette)
      if (p.density != palette[0].density || p.granulation != palette[0].granulation)
        constantsVary = true;
    check(constantsVary,
          "the palette's pigments really do differ in their physical constants");

    // And the tip is a function of the colour alone. Walked over the whole
    // palette rather than asserted on one entry, so a future mapping that
    // reached for `density` on some pigments and not others would fail here
    // rather than on whichever one this test happened to pick.
    bool colourOnly = true;
    for (size_t i = 0; i < palette.size(); ++i) {
      BrushState b;
      b.pigment = static_cast<int>(i);
      const BrushTip t = brushTipFor(b, noLut, 1.0f);
#if defined(NP_USE_MIXBOX)
      for (int k = 0; k < 3; ++k)
        if (std::fabs(t.pigment.c[k] - palette[i].rgb[k]) > 1e-6f) colourOnly = false;
#else
      // KM2 basis: `noLut` is unconditionally "loaded" under this basis (see
      // paint/Palette.cpp), so `brushTipFor()` takes the real
      // `rgbToLatent()` branch rather than the ON-build's "no LUT, copy the
      // sRGB triple into `c` verbatim" fallback -- `t.pigment.c` is Kubelka's
      // K per channel, not a colour, and comparing it to `palette[i].rgb`
      // directly no longer means anything. The claim this test exists for --
      // the tip is a function of the pigment's colour ALONE, not its
      // physical constants -- is checked instead by round-tripping the tip's
      // latent back to RGB and comparing THAT to the pigment's colour, at
      // the same tolerance app/selftest/PigmentLayer.cpp derives for the
      // identical KM2 round trip.
      const std::array<float, 3> back = latentToRgb(t.pigment);
      for (int k = 0; k < 3; ++k)
        if (std::fabs(back[k] - palette[i].rgb[k]) > MixboxLut::kKm2ReflectanceFloor + 1.0e-6f)
          colourOnly = false;
#endif
    }
    check(colourOnly,
          "the tip carries the pigment's colour and nothing else -- see brushTipFor()");
  }

  // --- Part E: routing, and the tip changing mid-stroke --------------------
  {
    OpenDocument doc = makeBlankOpenDocument(256, 256, WorkingSpace{}, "E");
    applyLayerCommand(doc, LayerCommand::NewPigmentLayer, doc.activeLayer);
    setActiveLayer(doc, 1);
    Layer* target = activeLayerOf(doc);
    check(target != nullptr && target->kind == LayerKind::Pigment, "the active layer is Pigment");
    check(strokeRouteFor(Tool::Brush, target) == StrokeRoute::CpuDeposit,
          "so a Brush stroke routes to the CPU deposit");

    // The refusal the options bar reports. Locked is a route of None, not a
    // fall-through to the solver -- putting paint on the canvas when the user
    // aimed at a layer is the failure a painter cannot see.
    target->locked = true;
    check(strokeRouteFor(Tool::Brush, target) == StrokeRoute::None,
          "a locked target refuses rather than falling through to PaintSim");
    target->locked = false;

    // `setTip()` mid-stroke, which is how pressure reaches a CPU deposit at
    // the same frame granularity the solver route gets. Two identical strokes,
    // one at a fixed small tip and one that grows: the growing one must write
    // more texels, or pressure is not reaching the deposit at all.
    MixboxLut noLut;
    BrushState brush;
    brush.radius = 30.0f;
    brush.load = 1.0f;

    const auto strokeTexels = [&](bool grow) {
      OpenDocument d = makeBlankOpenDocument(256, 256, WorkingSpace{}, "S");
      applyLayerCommand(d, LayerCommand::NewPigmentLayer, d.activeLayer);
      setActiveLayer(d, 1);
      StrokeSession session;
      std::string error;
      if (!session.begin(d, d.activeLayer, brushTipFor(brush, noLut, 0.2f), Tool::Brush, &error))
        return size_t{0};
      constexpr int kSamples = 24;
      for (int i = 0; i <= kSamples; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(kSamples);
        if (grow) session.setTip(brushTipFor(brush, noLut, 0.2f + 0.8f * u));
        session.addPoint(40.0f + 170.0f * u, 128.0f);
      }
      session.end();
      return session.texelsWritten();
    };

    const size_t flat = strokeTexels(false);
    const size_t grown = strokeTexels(true);
    std::printf("  [measured] a fixed light-pressure stroke wrote %zu texels; the same stroke\n"
                "             with pressure ramping 0.2 -> 1.0 wrote %zu\n", flat, grown);
    check(flat > 0 && grown > flat,
          "setTip mid-stroke reaches the deposit, so pressure widens the stroke");
  }

  std::printf("[selftest] active layer %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
