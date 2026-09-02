#include "app/selftest/Support.hpp"

#include "app/StrokeSession.hpp"
#include "brush/ToolOptionsBlend.hpp"

namespace np {

// ---------------------------------------------------------------------------
// Phase C Part 3 (bounded): `blendModeFromPsToolOptions()`
// (brush/ToolOptionsBlend.hpp) -- the edge mapping from Photoshop's own `Md `
// tool-options blend id onto `core::BlendMode`. Groundwork only: this build
// found two independent obstacles to wiring `BrushTip::blend` into any of the
// four deposit routes (a Pigment texel has no premultiplied RGBA to blend,
// and Photoshop's own Eraser tool does not consult a brush's blend mode at
// all -- see brush/Deposit.hpp's own comment on `BrushTip::blend` for both),
// so this section asserts the mapping and its one call site
// (`brushTipFor()`) rather than any painted pixel -- there is no painted
// pixel this feature affects yet.
bool runToolOptionsBlendTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ==========================================================================
  // A. The three ids this build honours
  // ==========================================================================
  {
    BlendMode out = BlendMode::Screen;  // a deliberately wrong starting
                                        // value, so a `true` return that
                                        // left `out` untouched would show
    std::string reason = "untouched";
    check(blendModeFromPsToolOptions("Nrml", out, &reason) && out == BlendMode::Normal,
          "toolopts: \"Nrml\" maps to BlendMode::Normal");
    check(blendModeFromPsToolOptions("", out, &reason) && out == BlendMode::Normal,
          "toolopts: the empty string (no toolOptions block, or one predating this key) also "
          "maps to BlendMode::Normal -- the same fallback an unrecognised id gets");
    check(blendModeFromPsToolOptions("Mltp", out, &reason) && out == BlendMode::Multiply,
          "toolopts: \"Mltp\" maps to BlendMode::Multiply");
    check(blendModeFromPsToolOptions("Drkn", out, &reason) && out == BlendMode::Min,
          "toolopts: \"Drkn\" (Darken) maps to BlendMode::Min -- brush/CoverageBlend.cpp's own "
          "Darken-is-min equivalence (`applyCoverageBlend()`'s Darken case, `std::min(a, b)`), "
          "extended componentwise to premultiplied RGBA rather than re-derived");
  }

  // ==========================================================================
  // B. The two named refusals, and the general one -- each with ITS OWN
  //    reason, matching this project's "the three reasons produce three
  //    visibly different sentences" idiom (app/StrokeSession.hpp's own
  //    `pixelOpRefusalMessage()` comment states the same requirement for the
  //    pixel-op refusals)
  // ==========================================================================
  {
    BlendMode out = BlendMode::Normal;
    std::string reasonBurn, reasonDissolve, reasonUnknown;
    const bool okBurn = blendModeFromPsToolOptions("linearBurn", out, &reasonBurn);
    const bool okDissolve = blendModeFromPsToolOptions("Dslv", out, &reasonDissolve);
    const bool okUnknown = blendModeFromPsToolOptions("CDdg", out, &reasonUnknown);

    check(!okBurn && !okDissolve && !okUnknown,
          "toolopts: \"linearBurn\", \"Dslv\" and an unrecognised id (\"CDdg\", Color Dodge) all "
          "refuse -- naturalPaint has no per-stroke blend implementation for any of them");
    check(!reasonBurn.empty() && !reasonDissolve.empty() && !reasonUnknown.empty(),
          "toolopts: every refusal carries a human-readable reason, never a bare false");
    check(reasonBurn != reasonDissolve && reasonBurn != reasonUnknown &&
              reasonDissolve != reasonUnknown,
          "toolopts: the three refusals read as three DIFFERENT sentences, not one generic "
          "\"unsupported\" repeated three times -- Linear Burn's reason names the missing "
          "core::BlendMode enumerator, Dissolve's names why it could never be a two-pixel blend "
          "at all, and the unknown id's names the id itself");
    check(reasonUnknown.find("CDdg") != std::string::npos,
          "toolopts: the general refusal names the UNRECOGNISED id itself, not just \"unknown\"");

    // The out-param is genuinely optional -- a caller that only wants the
    // bool (brushTipFor()'s own use) must not crash passing nullptr.
    std::string reasonUnused;
    check(!blendModeFromPsToolOptions("Dslv", out, nullptr),
          "toolopts: a null reasonOut is accepted -- the refusal still returns false, it just "
          "does not explain itself");
  }

  // ==========================================================================
  // C. brushTipFor()'s own call site: BrushTip::blend is set from the
  //    model's toolOptions.blendMode, falling back to Normal on refusal
  // ==========================================================================
  {
    const MixboxLut noLut;  // colour is not this section's subject
    BrushState brush;

    brush.model.options.blendMode = "Mltp";
    const BrushTip multiplyTip = brushTipFor(brush, noLut, 1.0f);
    check(multiplyTip.blend == BlendMode::Multiply,
          "toolopts: brushTipFor() sets BrushTip::blend from a model whose toolOptions carry "
          "\"Mltp\"");

    brush.model.options.blendMode = "Dslv";  // a refusal
    const BrushTip dissolveTip = brushTipFor(brush, noLut, 1.0f);
    check(dissolveTip.blend == BlendMode::Normal,
          "toolopts: brushTipFor() falls back to BlendMode::Normal for a blend id this build "
          "refuses -- the same fallback every brush painted before this field existed had, "
          "implicitly");

    brush.model.options.blendMode.clear();
    const BrushTip defaultTip = brushTipFor(brush, noLut, 1.0f);
    check(defaultTip.blend == BlendMode::Normal,
          "toolopts: and for the default BrushState -- every built-in preset, and every brush "
          "this codebase painted before Part 3 -- BrushTip::blend is BlendMode::Normal, its own "
          "default member initializer's value, unchanged");
  }

  std::printf("[selftest] tool options blend %s\n", ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace np
