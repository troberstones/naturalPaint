#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/Document.hpp"
#include "core/LayerOps.hpp"
#include "core/Region.hpp"

// core/RegionOps -- the operations over `Document::regions`: add, delete,
// move, resize, rename. `app/RegionTool` is the gesture that calls these;
// this file owns none of the gesture, only the document edit each gesture
// commits.
//
// Returns `core::LayerOpResult`, reused rather than a new result type
// invented -- `core::LayerCompOps.hpp`'s own reason: `recordLayerEdit()`
// takes a `LayerOpResult` and is the one funnel that bumps
// `OpenDocument::revision`, appends a `core::History` entry and tells
// app/Journal a structural change happened. A caller writes
// `recordLayerEdit(od, addRegion(od.document, ...))` and undo, the journal
// and the History panel all come for free.
namespace np {

// A default name for a new region of `kind`: "Frame N" or "Slice N", one
// above the highest number already used by an existing name with that kind's
// own stem -- `core::defaultNewLayerName()`'s scan, over this list, per kind.
// Always routed through `uniqueRegionName()` before being handed back, so a
// user who has renamed some other region to "Frame 3" cannot collide with it.
std::string defaultNewRegionName(const Document& doc, RegionKind kind);

// `desired` if no region in the document (of either kind) already has that
// name; otherwise `desired` with the smallest free " N" suffix from 2 up.
// An empty `desired` is treated as the empty string itself -- callers that
// want a generated default call `defaultNewRegionName()` first, matching
// `core::uniqueChannelName()`'s own split between "give me a name" and
// "make this name usable".
//
// `excludeIndex`, when it names a real region, is skipped while scanning for
// a collision -- so renaming a region to the name it already has is a no-op
// rather than growing a " 2" suffix onto itself. The default,
// `size_t(-1)`, never matches a real index.
std::string uniqueRegionName(const Document& doc, std::string_view desired,
                             size_t excludeIndex = static_cast<size_t>(-1));

// Appends a region of `kind` at `(x, y, width, height)`, in document pixels.
// `name` is uniquified via `uniqueRegionName()`; an empty `name` gets
// `defaultNewRegionName()`.
//
// Refused, by name, on an empty rectangle (`width == 0 || height == 0`): a
// region a user could not see, drag or export is not a region a gesture
// should be able to create, and `app/RegionTool`'s own "a click without a
// drag creates nothing" rule depends on this refusal existing rather than on
// the caller remembering to check first.
LayerOpResult addRegion(Document& doc, RegionKind kind, int32_t x, int32_t y, uint32_t width,
                        uint32_t height, std::string name = std::string());

// Deletes region `index`. Never touches a layer or a pixel -- a region is a
// document-level bookmark, not content.
LayerOpResult deleteRegion(Document& doc, size_t index);

// Renames region `index`. Uniquified against every other region
// (`excludeIndex = index`), so renaming to a name already in use lands on
// the same disambiguated name `addRegion()` would have chosen.
LayerOpResult renameRegion(Document& doc, size_t index, std::string name);

// Moves region `index` to a new origin, keeping its size. The gesture's
// corner-drag resize calls `resizeRegion()` instead; this is the whole-shape
// drag.
LayerOpResult moveRegion(Document& doc, size_t index, int32_t x, int32_t y);

// Sets region `index`'s whole rectangle -- the corner-drag resize. Refused,
// by name, on an empty rectangle, `addRegion()`'s own reason: a resize that
// collapsed a region to nothing would leave a region no consumer could draw,
// select or export, silently.
LayerOpResult resizeRegion(Document& doc, size_t index, int32_t x, int32_t y, uint32_t width,
                           uint32_t height);

}  // namespace np
