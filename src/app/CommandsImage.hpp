#pragma once

#include <array>
#include <cstdint>

#include "app/Command.hpp"
#include "ops/AutoLevels.hpp"
#include "ops/Blur.hpp"
#include "ops/ColorOps.hpp"
#include "ops/Filters.hpp"
#include "ops/MonoOps.hpp"
#include "ops/PointOps.hpp"
#include "ops/ToneOps.hpp"
#include "ops/Transform.hpp"

// app/CommandsImage -- the ENCODERS, the direction app/CommandsImage.cpp did
// not need until the UI started calling `applyCommand()` instead of the
// appliers (docs/automation-plan.md step 2).
//
// ==========================================================================
// (1) Why the encoder lives beside the decoder and not at the call site
// ==========================================================================
//
// A dialog holds an `UnsharpParams`, not a `JsonValue`. Something has to turn
// one into the other before `applyCommand()` can be called, and the obvious
// place -- ui/MacPaintUI.cpp, three lines above the button -- is the wrong
// one: the reader that has to understand what it wrote is in
// `app/CommandsImage.cpp`, and two files that must agree about the spelling
// of `"colorize_hue_degrees"` will eventually not.
//
// This is app/CommandsOpStack.hpp's own argument, which published `opToJson()`
// for exactly this reason ("a THIRD would not merely risk drifting: it would
// drift silently, because the converter's output is only ever read back by the
// same build that wrote it"). The encoders below are in the same translation
// unit as the readers they feed, immediately under them, so a key renamed in
// one is a key visibly not renamed in the other -- and `--selftest`
// (app/selftest/CommandCallsites.cpp) round-trips every one of them through
// its own adapter, which is the assertion that makes the adjacency mean
// something.
//
// ==========================================================================
// (2) They emit every key the adapter reads, including the identity ones
// ==========================================================================
//
// `requireAnyOf()` refuses a step that names none of the keys that would make
// it mean something, and it tests **presence**, not value. A dialog's
// Brightness/Contrast is a real request even when the user moved only the
// gamma, so each encoder writes the whole struct rather than the fields that
// differ from a default. That also makes the recorded step self-describing:
// a `.npaction` a user reads says what every control was set to, not which
// ones were touched.
//
// ==========================================================================
// (3) What they do NOT do is validate
// ==========================================================================
//
// An encoder writes what it was given. The magnitude gates (`requireAbove()`),
// the range checks and the engine's own predicates all live in the adapter and
// stay there: a second copy here would be the drift this header exists to
// prevent, and -- worse -- a UI that pre-validated would refuse in a different
// sentence from the one a replayed action gets for the identical parameters.
namespace np {

// --- the Filter menu's seven ---------------------------------------------
Command gaussianBlurCommand(float sigma);
Command sharpenCommand(float strength);
Command unsharpMaskCommand(const UnsharpParams& p);
Command addNoiseCommand(const NoiseParams& p);
Command embossCommand(const EmbossParams& p);
Command medianCommand(const MedianParams& p);
Command motionBlurCommand(const MotionBlurParams& p);

// --- Image > Adjustments' nineteen ---------------------------------------
Command levelsCommand(const std::array<LevelsParams, 3>& channels);
Command curvesCommand(const std::array<Curve, 3>& channels);
Command exposureCommand(const ExposureParams& p);
Command channelMixerCommand(const ChannelMixerParams& p);
Command desaturateCommand();
Command brightnessContrastCommand(const GainOffsetGammaParams& p);
Command hueSaturationCommand(const HueSaturationParams& p);
Command vibranceCommand(const VibranceParams& p);
Command colorBalanceCommand(const ColorBalanceParams& p);
Command blackAndWhiteCommand(const BlackAndWhiteParams& p);
Command photoFilterCommand(const PhotoFilterParams& p);
Command posterizeCommand(const PosterizeParams& p);
Command thresholdCommand(const ThresholdParams& p);
Command gradientMapCommand(const GradientMapParams& p);

// The five with no dialog and no parameters of their own. Each takes nothing
// because the applier the menu reaches takes nothing: `applyInvert(doc)` is
// `applyInvert(doc, InvertParams{})`, and `InvertParams{}` is what
// `doInvert()` starts from, so an empty params object is not an approximation
// of the menu's request -- it is exactly it. Writing `"amount": 1` here would
// be this header inventing a default the adapter already owns.
Command invertCommand();
Command autoToneCommand();
Command autoContrastCommand();
Command autoColorCommand();
Command equalizeCommand();

// --- the document's own geometry ------------------------------------------
Command imageSizeCommand(uint32_t width, uint32_t height, ResampleKernel kernel);
Command canvasSizeCommand(uint32_t width, uint32_t height, CanvasAnchor anchor);
Command cropToSelectionCommand();
Command trimToContentCommand();

}  // namespace np
