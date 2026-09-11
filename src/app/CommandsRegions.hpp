#pragma once

#include <cstdint>
#include <string>

#include "app/Command.hpp"
#include "core/Region.hpp"

// app/CommandsRegions -- the ENCODERS for the five `core::Region` rows
// app/CommandsRegions.cpp registers, beside the readers they feed for
// app/CommandsImage.hpp §1's reason: two files that must agree about the
// spelling of `"rect_width"` will eventually not, unless they are one file.
//
// **Every UI route that edits a region builds its command here.** The canvas
// gesture (app/RegionTool's `regionCommit*()`) and the options row's name
// field and delete button all do, and all reach `applyCommand()` -- the one
// place the recorder taps (docs/automation.md §2.3). An encoder built inline
// at a call site is a second encoder, and a call site that reached
// `core::RegionOps` directly would run correctly and record nothing, which is
// the failure docs/automation.md §7 says no assertion can see.
//
// A region is addressed by NAME (app/CommandsRegions.cpp's own argument:
// `Document::regions` names are unique, and an id is not portable to another
// document). Rectangles are half-open document pixels, `core::Region`'s own
// convention.
namespace np {

Command addRegionCommand(RegionKind kind, int32_t x, int32_t y, uint32_t width, uint32_t height,
                         const std::string& name = std::string());
Command deleteRegionCommand(const std::string& region);
Command renameRegionCommand(const std::string& region, const std::string& newName);
Command moveRegionCommand(const std::string& region, int32_t x, int32_t y);
Command resizeRegionCommand(const std::string& region, int32_t x, int32_t y, uint32_t width,
                            uint32_t height);

}  // namespace np
