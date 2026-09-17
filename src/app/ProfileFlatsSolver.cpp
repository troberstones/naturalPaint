#include "app/ProfileFlatsSolver.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "flats/Field.hpp"
#include "flats/Membrane.hpp"
#include "flats/MembraneGpu.hpp"

namespace np {

namespace {

void printStats(const char* label, std::vector<double>& ms) {
  std::sort(ms.begin(), ms.end());
  double sum = 0.0;
  for (double v : ms) sum += v;
  std::printf("profile-flats-solver: %s: min %.2f ms, median %.2f ms, mean %.2f ms, max %.2f ms (n=%zu)\n",
              label, ms.front(), ms[ms.size() / 2], sum / static_cast<double>(ms.size()), ms.back(),
              ms.size());
}

}  // namespace

int runProfileFlatsSolver(GpuContext& gpu, int width, int height, int iterations) {
  std::printf("profile-flats-solver: canvas %dx%d, %d iterations, cycles=30 tol=1e-2 (production defaults)\n",
              width, height, iterations);

  // A grid of leaky boxes covers the canvas with the case the solver actually
  // has to work for: many separate ink-bounded basins plus narrow gaps the
  // line search has to survive, rather than one trivial empty rectangle.
  FlatArt a = flatBlankArt(width, height);
  const int cell = 128;
  for (int y0 = 8; y0 + cell < height; y0 += cell)
    for (int x0 = 8; x0 + cell < width; x0 += cell)
      flatArtRect(a, x0, y0, x0 + cell - 8, y0 + cell - 8, /*gap=*/6);

  double cpuMean = 0.0, gpuMean = 0.0;

  {
    std::vector<double> ms(static_cast<size_t>(iterations));
    (void)flatMembraneSag(a.line, a.w, a.h);  // untimed warmup, same reasoning as ProfileVectorWarp
    for (int i = 0; i < iterations; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      const std::vector<float> sag = flatMembraneSag(a.line, a.w, a.h);
      const auto t1 = std::chrono::steady_clock::now();
      ms[static_cast<size_t>(i)] = std::chrono::duration<double, std::milli>(t1 - t0).count();
      (void)sag;
    }
    double sum = 0.0;
    for (double v : ms) sum += v;
    cpuMean = sum / static_cast<double>(ms.size());
    printStats("CPU flatMembraneSag", ms);
  }

  {
    std::vector<double> ms(static_cast<size_t>(iterations));
    (void)membraneSagGpu(gpu, a.line, a.w, a.h);  // untimed warmup: pipeline/shader compile, not the loop
    for (int i = 0; i < iterations; ++i) {
      const auto t0 = std::chrono::steady_clock::now();
      const std::vector<float> sag = membraneSagGpu(gpu, a.line, a.w, a.h);
      const auto t1 = std::chrono::steady_clock::now();
      ms[static_cast<size_t>(i)] = std::chrono::duration<double, std::milli>(t1 - t0).count();
      (void)sag;
    }
    double sum = 0.0;
    for (double v : ms) sum += v;
    gpuMean = sum / static_cast<double>(ms.size());
    printStats("GPU membraneSagGpu", ms);
  }

  std::printf("profile-flats-solver: speedup (CPU mean / GPU mean) = %.2fx\n",
              gpuMean > 0.0 ? cpuMean / gpuMean : 0.0);
  return 0;
}

}  // namespace np
