#include "app/ProfileToggle.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "core/Layer.hpp"
#include "io/PsdImport.hpp"
#include "ui/DocumentTexture.hpp"

namespace np {

int runProfileToggle(const char* path, int layerIndex, int iterations) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "profile-toggle: cannot open %s\n", path);
    return 1;
  }
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  const PsdImportResult r = importPsd(std::span<const uint8_t>(bytes.data(), bytes.size()));
  if (!r.ok) {
    std::fprintf(stderr, "profile-toggle: import failed: %s\n", r.error.c_str());
    return 1;
  }
  Document doc = r.document;
  if (layerIndex < 0 || static_cast<size_t>(layerIndex) >= doc.layers.size()) {
    std::fprintf(stderr, "profile-toggle: layer index %d out of range (0..%zu)\n", layerIndex,
                 doc.layers.size() - 1);
    return 1;
  }
  if (iterations < 1) iterations = 1;

  std::printf("profile-toggle: %s\n", path);
  std::printf("profile-toggle: layer %d = \"%s\" (%dx%d canvas, %zu layers), %d iterations\n",
              layerIndex, doc.layers[static_cast<size_t>(layerIndex)].name.c_str(), doc.width,
              doc.height, doc.layers.size(), iterations);

  // One untimed composite first: the timed loop below should measure steady-
  // state cost, not the allocator growth and page faults the very first
  // composite of a freshly-imported document always pays.
  compositeDocumentStraightHalf(doc, nullptr);

  std::vector<double> ms(static_cast<size_t>(iterations));
  for (int i = 0; i < iterations; ++i) {
    Layer& layer = doc.layers[static_cast<size_t>(layerIndex)];
    layer.visible = !layer.visible;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<uint16_t> halves = compositeDocumentStraightHalf(doc, nullptr);
    const auto t1 = std::chrono::steady_clock::now();
    ms[static_cast<size_t>(i)] = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (halves.empty() && doc.width > 0 && doc.height > 0) {
      std::fprintf(stderr, "profile-toggle: empty composite result, aborting\n");
      return 1;
    }
  }

  std::vector<double> sorted = ms;
  std::sort(sorted.begin(), sorted.end());
  double sum = 0.0;
  for (double v : ms) sum += v;
  std::printf("profile-toggle: min %.2f ms, median %.2f ms, mean %.2f ms, max %.2f ms (n=%d)\n",
              sorted.front(), sorted[sorted.size() / 2], sum / static_cast<double>(ms.size()),
              sorted.back(), iterations);
  return 0;
}

}  // namespace np
