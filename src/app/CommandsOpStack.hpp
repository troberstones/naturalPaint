#pragma once

#include <string>

#include "core/OpStack.hpp"
#include "io/Json.hpp"

// app/CommandsOpStack -- the op codec, published for the one caller outside
// this family that needs it.
//
// `app/CommandsOpStack.cpp` gives a `core::Op` its text form: keyed by kind
// NAME, refusing anything that is not `OpClass::PointA` rather than carrying
// it inert. That codec was written for the `add_layer_op` / `set_layer_op`
// command rows, and PRD P6's converter -- `actionFromLayerOps()`, which turns
// a graded layer into an action -- needs the identical encoding.
//
// **A header rather than a second encoder, and the reason is the drift trap
// this project has already been bitten by.** `io/OpSerial` and this codec are
// two encodings of one op list (hex for an EXR header attribute, text for a
// file a human edits), and docs/automation-plan.md §7 names keeping them in
// step as a standing risk. A *third* -- a converter that wrote its own
// approximation of an op -- would not merely risk drifting: it would drift
// silently, because the converter's output is only ever read back by the same
// build that wrote it, so no round trip would ever disagree.
//
// The two functions were already at file scope with external linkage; this
// header only declares them.
namespace np {

// One `core::Op` as the JSON an action file carries. Returns a null JsonValue
// and fills `*errorOut` for anything that is not a point op this build can
// name.
JsonValue opToJson(const Op& op, std::string* errorOut);

// The inverse. False, with `*errorOut` naming what it could not read, for an
// unknown kind id or a class this build cannot evaluate.
bool opFromJson(const JsonValue& value, Op* out, std::string* errorOut);

}  // namespace np
