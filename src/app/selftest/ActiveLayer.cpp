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

  // --- Part D: brushTipFor, rewritten for the model migration --------------
  //
  // **Inverted from what this section proved before.** Radius/flow used to
  // vary with pressure through `defaultBrushLinks()`'s two PRESSURE links
  // (SIZE, FLOW). `brushTipFor()` no longer reads `BrushState::links` in any
  // form, and no longer reads its own `inputs` parameter at all
  // (`(void)inputs;`, its own comment) -- Size/Angle/Roundness are base-only
  // there, resolved per dab only inside a real `StrokeSession`, and Flow is
  // `brush.load` unscaled, deliberately. So what this section proves now is
  // the opposite claim: the tip `brushTipFor()` returns is the model's own
  // base values, AT EVERY PRESSURE, full stop.
  {
    MixboxLut noLut;  // never loaded: the fallback path, and the test's default
    BrushState brush;
    brush.model.tip.diameterPx = 80.0f;  // radius 40
    brush.model.tip.hardness = 0.5f;
    brush.load = 0.8f;
    brush.model.tip.spacingPercent = 30.0f;
    brush.pigment = 6;  // Ultramarine Blue, BrushState's own default

    const BrushTip full = brushTipFor(brush, noLut, 1.0f);
    check(full.radius == brush.model.tip.diameterPx / 2.0f && full.flow == brush.load,
          "at full pressure the tip is the model's base values, unmodified");
    // `full.spacing` is RADII (`brush/Deposit.hpp`'s own comment);
    // `spacingPercent` is a percentage OF THE DIAMETER, so `/ 100 * 2` is the
    // conversion `brushTipFor()` itself applies (its own comment on
    // `tip.spacing` names the same factor of two), not a bare `/ 100`.
    check(full.hardness == brush.model.tip.hardness &&
              full.spacing == brush.model.tip.spacingPercent / 100.0f * 2.0f,
          "hardness and spacing do not vary with pressure at all");

    // At EVERY pressure, not merely full: `brushTipFor()` never reads its
    // `inputs` argument (or `brush.links`) at all any more, so a sweep
    // across the whole [0,1] range must return the IDENTICAL tip every
    // time -- the direct replacement for the old "0.25+0.75p and
    // 0.15+0.85p at every pressure" claim, restated as "constant at every
    // pressure" instead.
    bool constantEverywhere = true;
    for (int i = 0; i <= 20; ++i) {
      const float p = static_cast<float>(i) / 20.0f;
      const BrushTip t = brushTipFor(brush, noLut, p);
      if (t.radius != full.radius || t.flow != full.flow) constantEverywhere = false;
    }
    check(constantEverywhere,
          "the tip is now the SAME at every pressure from 0 to 1 -- pressure no longer "
          "reaches brushTipFor() at all, so there is no curve left to be the old formula");

    // What used to be "the two [links] are independent" (removing one
    // leaves the other's own behaviour unchanged) is now the stronger claim
    // that removing EITHER (or both) changes nothing at all, because
    // neither was ever read.
    removeLink(brush.links, DynamicSource::Pressure, DynamicTarget::Flow);
    const BrushTip oneLinkRemoved = brushTipFor(brush, noLut, 0.5f);
    check(oneLinkRemoved.flow == full.flow && oneLinkRemoved.radius == full.radius,
          "removing the Pressure -> Flow link (still present in `brush.links`, just unread) "
          "changes neither flow nor radius");
    brush.links = defaultBrushLinks();
    removeLink(brush.links, DynamicSource::Pressure, DynamicTarget::Size);
    const BrushTip otherLinkRemoved = brushTipFor(brush, noLut, 0.5f);
    check(otherLinkRemoved.radius == full.radius && otherLinkRemoved.flow == full.flow,
          "and removing Pressure -> Size instead changes neither either");
    brush.links = defaultBrushLinks();

    // Out-of-range pressure used to be clamped rather than producing a
    // negative radius; now it is simply never read, so an out-of-range
    // value changes nothing at all -- the stronger claim subsumes the old
    // clamp behaviour (a radius that never varies with pressure cannot go
    // negative from pressure either).
    check(brushTipFor(brush, noLut, -3.0f).radius == full.radius,
          "negative pressure changes nothing -- not clamped, simply unread");
    check(brushTipFor(brush, noLut, 9.0f).radius == full.radius,
          "and pressure above 1 changes nothing either");

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
      for (int k = 0; k < 3; ++k)
        if (std::fabs(t.pigment.c[k] - palette[i].rgb[k]) > 1e-6f) colourOnly = false;
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
    brush.model.tip.diameterPx = 60.0f;  // radius 30
    brush.load = 1.0f;
    // **`brushTipFor()`'s own pressure argument no longer widens anything**
    // (Part D above, just proved) -- pressure now reaches a per-dab radius
    // only through `model.shape.size`'s own `Variance`, resolved inside
    // `StrokeSession`'s per-dab loop off the `hardwareInputs` `setTip()`
    // latches. Rewritten to exercise THAT mechanism: a PenPressure-
    // controlled Size Variance, with `hardwareInputs.pressure` supplied
    // fresh at each `setTip()` call -- still mid-stroke, still reaching the
    // deposit, just through the architecture that replaced the old one.
    brush.model.shape.size.control = VarianceControl::PenPressure;

    const auto strokeTexels = [&](bool grow) {
      OpenDocument d = makeBlankOpenDocument(256, 256, WorkingSpace{}, "S");
      applyLayerCommand(d, LayerCommand::NewPigmentLayer, d.activeLayer);
      setActiveLayer(d, 1);
      StrokeSession session;
      std::string error;
      DynamicInputs startIn;
      startIn.hasPressure = true;
      startIn.pressure = 0.2f;
      if (!session.begin(d, d.activeLayer, brushTipFor(brush, noLut, 0.2f), Tool::Brush, &error,
                         &brush.model, startIn))
        return size_t{0};
      constexpr int kSamples = 24;
      for (int i = 0; i <= kSamples; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(kSamples);
        DynamicInputs in;
        in.hasPressure = true;
        in.pressure = grow ? 0.2f + 0.8f * u : 0.2f;
        session.setTip(brushTipFor(brush, noLut, in.pressure), in);
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
          "setTip's hardwareInputs mid-stroke reaches the per-dab Variance resolution, so "
          "pressure widens the stroke -- through PenPressure-controlled Size now, not the "
          "retired PRESSURE -> SIZE link");
  }

  std::printf("[selftest] active layer %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
