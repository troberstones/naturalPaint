#include "app/selftest/Support.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "brush/BrushModelDiff.hpp"

namespace np {
namespace {

// A "make this differ" step per leaf type, used by section C below to prove
// every one of BrushModel's leaves is actually wired into the visitor,
// without 151 hand-written assignments to do it. Overloaded rather than one
// function with a big switch, because that is what lets the mutable-visitor
// call site below stay generic (`bumpValue(av, counter)` resolves by the
// leaf's own type, whatever it is).
//
// `counter` only exists so a run through many leaves of the same type (there
// are dozens of floats) doesn't produce the exact same "after" value for
// each -- not for correctness of any single leaf, since every overload
// already guarantees `bumped != v`.
float bumpValue(float v, int counter) {
  return v + 3.5f + static_cast<float>(counter % 11);
}

bool bumpValue(bool v, int /*counter*/) { return !v; }

int32_t bumpValue(int32_t v, int counter) { return v + 4 + (counter % 6); }

std::string bumpValue(const std::string& v, int counter) {
  return v + "#" + std::to_string(counter);
}

VarianceControl bumpValue(VarianceControl v, int counter) {
  static constexpr VarianceControl kAll[] = {
      VarianceControl::Off,          VarianceControl::Fade,
      VarianceControl::PenPressure,  VarianceControl::PenTilt,
      VarianceControl::StylusWheel,  VarianceControl::Rotation,
      VarianceControl::Direction,    VarianceControl::InitialDirection,
  };
  const VarianceControl candidate = kAll[counter % 8];
  return candidate == v ? kAll[(counter + 1) % 8] : candidate;
}

CoverageBlend bumpValue(CoverageBlend v, int counter) {
  static constexpr CoverageBlend kAll[] = {
      CoverageBlend::Multiply,   CoverageBlend::Overlay,     CoverageBlend::ColorBurn,
      CoverageBlend::HardMix,    CoverageBlend::LinearBurn,  CoverageBlend::ColorDodge,
      CoverageBlend::Darken,     CoverageBlend::Subtract,    CoverageBlend::Height,
      CoverageBlend::LinearHeight,
  };
  const CoverageBlend candidate = kAll[counter % 10];
  return candidate == v ? kAll[(counter + 1) % 10] : candidate;
}

}  // namespace

// ---------------------------------------------------------------------------
// brush/BrushModelDiff: brushModelDiff()/brushModelEqual(), built on ONE
// templated visitor (brush/BrushModelDiff.hpp's detail::visitBrushModel)
// walked over BrushModel's ~151 leaves rather than the 14 scalars
// presetMatches() checks today.
//
// Section B is the one that matters most and is the least automatable: it
// hand-mutates eight fields DIRECTLY (`m.shape.size.jitter = ...`, never
// through the visitor), so a bug where the visitor's path STRING for a leaf
// does not match the variable it actually reads -- e.g. the line for
// "shape.size" copy-pasted from "shape.angle" and left reading `a.angle` --
// is caught. Section C's reachability sweep mutates through the SAME visitor
// it then reads through, which proves every leaf is *some* distinct,
// nameable, round-trippable thing, but a self-consistent mislabeling would
// slip past it; that is what section B is for.
// ---------------------------------------------------------------------------
bool runBrushModelDiffTest() {
  bool ok = true;
  auto check = [&](bool cond, const char* what) {
    std::printf("  %-70s %s\n", what, cond ? "pass" : "FAIL");
    if (!cond) ok = false;
  };

  // ==========================================================================
  std::printf("  -- A. two default models are equal, and their diff is empty --\n");
  // ==========================================================================
  {
    BrushModel d1, d2;
    check(brushModelDiff(d1, d2).empty(),
          "diff/default: two default-constructed models differ nowhere");
    check(brushModelEqual(d1, d2), "equal/default: agrees -- default models are equal");
  }

  // ==========================================================================
  std::printf(
      "  -- B. one field changed, one path reported -- eight fields, eight sub-structs --\n");
  // ==========================================================================
  {
    // Deliberately NOT the default model: sibling fields of the same type
    // (shape.size.jitter vs shape.angle.jitter, tip.diameterPx vs
    // dual.tip.diameterPx, scatter.count vs dual.scatter.count, ...) start at
    // DIFFERENT values here. A diff that read the wrong sibling -- the
    // specific bug this section exists to catch -- would then report either
    // the wrong path or a value that does not match what was actually
    // assigned, not just silently pass by coincidence of both being 0.
    BrushModel base;
    base.shape.size.jitter = 0.10f;
    base.shape.angle.jitter = 0.20f;
    base.shape.angle.control = VarianceControl::PenTilt;
    base.transfer.opacity.control = VarianceControl::PenPressure;
    base.transfer.flow.control = VarianceControl::Rotation;
    base.color.foregroundBackground.present = false;
    base.texture.blend = CoverageBlend::Height;
    base.dual.blend = CoverageBlend::Multiply;
    base.scatter.count = 2;
    base.dual.scatter.count = 5;
    base.tip.diameterPx = 40.0f;
    base.dual.tip.diameterPx = 55.0f;
    base.options.blendMode = "Nrml";

    auto expectOne = [&](const BrushModel& mutated, const char* expectedPath, const char* label) {
      const auto diff = brushModelDiff(base, mutated);
      char buf[160];
      std::snprintf(buf, sizeof(buf), "diff/%s: reports exactly {\"%s\"}", label, expectedPath);
      check(diff.size() == 1 && diff[0] == expectedPath, buf);
      std::snprintf(buf, sizeof(buf), "equal/%s: brushModelEqual() agrees it changed", label);
      check(!brushModelEqual(base, mutated), buf);
    };

    {
      BrushModel m = base;
      m.tip.diameterPx = 77.5f;
      expectOne(m, "tip.diameterPx", "float");
    }
    {
      BrushModel m = base;
      m.dual.flip = true;
      expectOne(m, "dual.flip", "bool");
    }
    {
      BrushModel m = base;
      m.scatter.count = 9;
      expectOne(m, "scatter.count", "int");
    }
    {
      BrushModel m = base;
      m.texture.blend = CoverageBlend::Subtract;
      expectOne(m, "texture.blend", "enum");
    }
    {
      BrushModel m = base;
      m.options.blendMode = "Mltp";
      expectOne(m, "options.blendMode", "string");
    }
    {
      BrushModel m = base;
      m.shape.size.jitter = 0.55f;
      expectOne(m, "shape.size.jitter", "Variance::jitter");
    }
    {
      BrushModel m = base;
      m.transfer.opacity.control = VarianceControl::Direction;
      expectOne(m, "transfer.opacity.control", "Variance::control");
    }
    {
      BrushModel m = base;
      m.color.foregroundBackground.present = true;
      expectOne(m, "color.foregroundBackground.present", "Variance::present");
    }
  }

  // ==========================================================================
  std::printf("  -- C. every leaf is reachable, and the count is pinned --\n");
  // ==========================================================================
  const auto paths = brushModelDiffPaths();
  {
    // 151, not the model header's own "roughly 117": that estimate charges
    // Variance four leaves (it has five -- control, jitter, minimum,
    // fadeSteps, present) and counts PsTipShape and PsScatter once each even
    // though PsDualBrush embeds a second copy of both. This literal is the
    // real number, and it is what catches a field added to BrushModel with
    // no matching visitor line: the count moves and this line does not.
    check(paths.size() == 151, "brushModelDiffPaths(): total leaf count pinned at 151");

    std::set<std::string> distinctPaths(paths.begin(), paths.end());
    check(distinctPaths.size() == paths.size(),
          "brushModelDiffPaths(): no two leaves share a path string");
  }

  {
    const BrushModel low;  // defaults; each leaf's own bumpValue() guarantees a change
    bool allReachable = true;
    bool allEqualAgrees = true;
    size_t firstBadIndex = paths.size();

    for (size_t target = 0; target < paths.size(); ++target) {
      BrushModel mutated = low;
      int counter = 0;
      // Walked in MUTABLE mode (A = BrushModel&): detail::visitBrushModel is
      // the exact same template brushModelDiff() below reads through, just
      // instantiated with a writable first argument. `mutated` stands in for
      // both model parameters -- the second is never read here -- so this is
      // one traversal, not a construction pass followed by a separate query.
      auto mutate = [&](const std::string&, auto& leaf, const auto&) -> bool {
        if (static_cast<size_t>(counter) == target) leaf = bumpValue(leaf, counter);
        ++counter;
        return true;
      };
      detail::visitBrushModel(mutated, mutated, mutate);

      const auto diff = brushModelDiff(low, mutated);
      const bool reachedCorrectly = diff.size() == 1 && diff[0] == paths[target];
      if (!reachedCorrectly && allReachable) firstBadIndex = target;
      allReachable = allReachable && reachedCorrectly;

      // Same mutation, both questions: brushModelEqual() must say "not
      // equal" exactly when brushModelDiff() found something to report.
      if (brushModelEqual(low, mutated) != diff.empty()) allEqualAgrees = false;
    }

    check(allReachable,
          "diff/reachability: mutating leaf k alone (all 151, one at a time) names exactly "
          "leaf k");
    if (!allReachable) {
      std::printf("    first mismatch at index %zu (%s)\n", firstBadIndex,
                  firstBadIndex < paths.size() ? paths[firstBadIndex].c_str() : "?");
    }
    check(allEqualAgrees,
          "equal/reachability: brushModelEqual() agrees with brushModelDiff().empty() on "
          "all 151 single-field mutations");
  }

  // ==========================================================================
  std::printf("  -- D. DabRef::bitmap is not compared -- id is the whole fact --\n");
  // ==========================================================================
  {
    // The resolved bitmap is derived FROM the id at load time and never
    // persisted (BrushModel.hpp's own comment on DabRef::bitmap); two models
    // naming the same id are the same brush whether or not one of them
    // happens to have a live bitmap loaded, e.g. mid-import versus after a
    // picker click. If this compared the pointer, "not loaded yet" would
    // light the EDITED badge for a preset nobody touched.
    BrushModel a, b;
    a.tip.dab.id = "abr:11111111-1111-1111-1111-111111111111";
    b.tip.dab.id = a.tip.dab.id;
    a.tip.dab.bitmap = nullptr;
    b.tip.dab.bitmap =
        std::make_shared<const BrushTipBitmap>(BrushTipBitmap{2, 2, std::vector<uint8_t>(4, 255)});
    check(brushModelDiff(a, b).empty(),
          "diff/DabRef: same id, one resolved bitmap and one not -- no diff");
    check(brushModelEqual(a, b), "equal/DabRef: agrees");

    // The id itself is very much compared -- this is the fact `bitmap` is
    // merely a cache of.
    BrushModel c = a;
    c.tip.dab.id = "abr:22222222-2222-2222-2222-222222222222";
    const auto diff = brushModelDiff(a, c);
    check(diff.size() == 1 && diff[0] == "tip.dab.id",
          "diff/DabRef: a different id IS reported, at \"tip.dab.id\"");
  }

  return ok;
}

}  // namespace np
