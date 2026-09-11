#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/Document.hpp"
#include "core/Region.hpp"
#include "io/ExportAs.hpp"
#include "io/ExportStates.hpp"
#include "ops/DocumentTransform.hpp"  // DocumentRegion

// io/ExportRegions -- `File > Export Frames and Slices...`, PLAN.md
// gap-closing wave's payoff step. One file per region, each the visible
// composite cropped to that region's rectangle.
//
// ==========================================================================
// REUSE, AND WHERE IT STOPS
// ==========================================================================
//
// `io/ExportStates.hpp` is this codebase's "export N things to files" loop,
// and this file reuses the parts of it that are genuinely the same
// operation rather than re-deriving them:
//
//   * **The item and report shapes** -- `ExportStateItem`, `ExportStatesReport`
//     and `ExportItemOutcome` are used verbatim, unchanged. `sourceIndex`,
//     `stateName` (a region's name), `ordinal`, `filename`, `path`,
//     `outcome`, `reason`, `bytesWritten` and `warnings` all mean exactly
//     what they mean for a comp or a layer -- there is no region-specific
//     field this format needs that those five do not already have.
//   * **The name template** -- `resolveExportStateName()` and
//     `validateExportNameTemplate()` are the free functions
//     `io/ExportStates.cpp` already exposes for this reason, and are called
//     here unchanged: `{name}` is the region's own name, `{doc}` and
//     `{index}` behave exactly as they do for a comp or a layer.
//   * **The four settings and the writer** -- `io/ExportAs`'s
//     `ExportRequest` (format/space/depth/resize) and
//     `exportDocumentWithRequestToFile()` are the same ones every export
//     path in this build already goes through.
//
// **What is NOT reused: `ExportStateSource` and the state-toggle loop
// itself.** That enum's two members are "the four appearance properties of a
// comp" and "one layer isolated on transparency" -- both readings of *the
// same document, restyled*. A region export is a different operation
// entirely: **the same style, a different crop.** Extending
// `ExportStateSource` with a third member would not fit either existing
// switch arm (`request.source == Comps ? ... : ...` appears throughout
// io/ExportStates.cpp as a two-way branch, not a table), and every one of
// those two-way branches would need a third arm added for a case that shares
// no code with either -- which is not reuse, it is coupling two unrelated
// operations through one enum for the sake of appearing unified. So this
// file is its own (short) loop, calling the shared naming and writing
// primitives above, which is the actual overlap between the two features.
//
// ==========================================================================
// WHAT ONE ITEM'S FILE CONTAINS
// ==========================================================================
//
// The document's own composite -- every layer at its own current visibility,
// opacity, blend and clip, exactly as `File > Export As` would write it for
// the whole canvas -- **cropped to the region's rectangle**. Not an isolated
// layer, not a restored comp: a region is a rectangle over the picture as it
// stands, the same relationship a physical crop mark has to a printed page.
//
// The crop is `ops/DocumentTransform::cropDocument()` on a scratch copy of
// the document, reused rather than reimplemented for the reason every other
// geometry op in this codebase gives: it is bit-exact (no resample), it moves
// every store of every layer together, and it is already the function
// `core/Region.hpp` §4 states every other region-geometry rule in terms of.
//
// **A region partly outside the canvas exports the intersection.** The
// scratch is cropped to `intersect(region, documentCanvasRegion(doc))`
// rather than to the region's own (possibly out-of-canvas) rectangle --
// `cropDocument()` would happily extend the canvas into the margin a region
// hangs off the edge of, which would export a file with a border of
// transparency nobody asked to see. A region whose intersection with the
// canvas is empty exports nothing and is `Skipped`, named, rather than
// producing a `0x0` file no format can encode.
namespace np {

// Which regions a run considers. `All` is not "every kind" spelled a third
// way -- the export dialog's own two checkboxes, so `Frame`/`Slice` here have
// to be able to mean "only this kind" rather than "and also this kind".
enum class RegionExportScope {
  All,
  FramesOnly,
  SlicesOnly,
};

struct ExportRegionsRequest {
  RegionExportScope scope = RegionExportScope::All;

  // Format, target space, bit depth and resize (io/ExportAs's own four
  // settings, PRD I15).
  ExportRequest format;

  // Where the files go. Must already exist -- `io/ExportStates`'s own reason:
  // creating directories on a user's behalf is a side effect they did not
  // ask for.
  std::string outputDirectory;

  // `{name}` (the region's own name), `{doc}`, `{index}` -- `io/ExportStates`
  // §5's tokens, unchanged.
  std::string nameTemplate = "{name}";

  std::string documentName;

  // Indices into `doc.regions`, filtered by `scope`. Empty means every region
  // `scope` admits -- `ExportStatesRequest::selection`'s own convention.
  std::vector<size_t> selection;

  bool overwriteExisting = false;
};

// "region" / "regions" -- io/ExportStates' `exportStateSourceNoun()` has no
// row for this, and inventing a third `ExportStateSource` member just to get
// two words out of it would be exactly the coupling this header's own
// section argues against.
const char* regionExportNoun();
const char* regionExportPlural();

// The rectangle one region actually exports: its intersection with the
// canvas (this header's "a region partly outside exports the intersection").
// `false`, and `*out` untouched, when that intersection is empty. The plan,
// the export loop and the dialog's size readout all ask this one function,
// so the size the dialog promises is the size the loop crops to.
bool regionExportRect(const Document& doc, const Region& region, DocumentRegion* out);

// `validateExportRequest()` for ONE region, against the image that region
// actually exports -- its canvas intersection -- rather than the document.
// **Why this exists:** each file is cropped first and resized second
// (`exportDocumentRegions()` runs `exportDocumentWithRequest()` on the
// cropped scratch document), so validating against the document's own
// extent reported "1024 x 1024" for a 430x290 Frame and computed every
// resize warning for an image no file would ever be. `ok == false`, with a
// reason naming the region, when it lies wholly off the canvas.
ExportValidation validateRegionExport(const Document& doc, const Region& region,
                                      const ExportRequest& format);

// Everything decided before the first byte: the selection (after the scope
// filter), the output directory, every resolved filename, every collision,
// every existing path, and every region whose canvas intersection is empty.
// Writes nothing and touches no pixel -- `planStateExport()`'s own contract.
ExportStatesReport planRegionExport(const Document& doc, const ExportRegionsRequest& request);

// `planRegionExport()`, then one crop-and-export per surviving item. Stops at
// the first `Failed`, `io/ExportStates`'s own §8 rule. Never mutates `doc`.
ExportStatesReport exportDocumentRegions(const Document& doc, const ExportRegionsRequest& request);

}  // namespace np
