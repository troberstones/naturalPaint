#include "app/ProfileToggle.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "core/Composite.hpp"
#include "core/Document.hpp"
#include "core/Half.hpp"
#include "core/Layer.hpp"
#include "core/Premultiply.hpp"
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

  // --- The same toggle, through the buffer-reuse path a live app actually
  // takes -------------------------------------------------------------------
  //
  // The loop above calls `compositeDocumentStraightHalf()` fresh every
  // iteration -- a brand-new, zero-filled accumulator allocated and thrown
  // away each time -- because that is what this harness has always done
  // (see this file's header) and the loop is left exactly as it was so the
  // published baseline stays comparable against it.
  //
  // A running `ui::DocumentTexture`, though, is one persistent object across
  // every toggle in the app: its full-recomposite branch now calls
  // `compositeDocumentPremultipliedInto()` with a buffer it owns and keeps
  // between calls (`premultScratch_`), reallocating only on a canvas-size
  // change -- never on a repeated toggle of the same open document. This
  // second loop reproduces exactly that, with the identical composite +
  // unpremultiply + `floatToHalf()` pack `compositeDocumentStraightHalf()`
  // performs, so the only thing that differs from the loop above is whether
  // the accumulator is reused.
  std::vector<float> premult;
  std::vector<uint16_t> halves;
  compositeDocumentPremultipliedInto(doc, premult, nullptr);  // untimed warm-up, as above
  halves.resize(premult.size());

  std::vector<double> reusedMs(static_cast<size_t>(iterations));
  for (int i = 0; i < iterations; ++i) {
    Layer& layer = doc.layers[static_cast<size_t>(layerIndex)];
    layer.visible = !layer.visible;
    const auto t0 = std::chrono::steady_clock::now();
    compositeDocumentPremultipliedInto(doc, premult, nullptr);
    for (size_t t = 0; t * 4 + 3 < premult.size(); ++t) {
      const std::array<float, 4> straight = unpremultiply(std::array<float, 4>{
          premult[t * 4 + 0], premult[t * 4 + 1], premult[t * 4 + 2], premult[t * 4 + 3]});
      for (size_t c = 0; c < 4; ++c) halves[t * 4 + c] = floatToHalf(straight[c]);
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (halves.empty() && doc.width > 0 && doc.height > 0) {
      std::fprintf(stderr, "profile-toggle: empty composite result (buffer-reuse path), aborting\n");
      return 1;
    }
    reusedMs[static_cast<size_t>(i)] = std::chrono::duration<double, std::milli>(t1 - t0).count();
  }

  std::vector<double> reusedSorted = reusedMs;
  std::sort(reusedSorted.begin(), reusedSorted.end());
  double reusedSum = 0.0;
  for (double v : reusedMs) reusedSum += v;
  std::printf("profile-toggle: [buffer reuse path] min %.2f ms, median %.2f ms, mean %.2f ms, "
              "max %.2f ms (n=%d)\n",
              reusedSorted.front(), reusedSorted[reusedSorted.size() / 2],
              reusedSum / static_cast<double>(reusedMs.size()), reusedSorted.back(), iterations);
  return 0;
}

}  // namespace np
