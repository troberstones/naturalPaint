#pragma once

#include <cstddef>

#include "imgui.h"

// ui/LabelledControl -- the label-left layout for the right-hand controls
// column (UI detour step 3, problem 1b). Moved out of ui/MacPaintUI.cpp
// verbatim, in the "move code out, never in" convention that file's size
// keeps forcing: this cluster was already self-contained (it depends on
// nothing in MacPaintUI.cpp but app/ControlsLayout.hpp's
// `layoutLabelledControl()`), so it gained nothing from staying there.
//
// Dear ImGui draws a widget's label to the *right* of the widget, and the
// controls column is a fixed-width docked panel, so before this existed four
// of its sliders read "Granulatio", "Edge darke", "Paper slop" and "Working
// ti" -- clipped by the window edge, mid-word.
//
// Every labelled control in that column goes through `ctlSlider()` /
// `ctlSliderInt()` / `ctlBeginCombo()` / `ctlInputText()` below, which draw
// the label at the left and give the widget what is left.
// app/ControlsLayout.hpp owns the arithmetic and its one invariant (the
// widget never starts before the label ends); this module owns the
// measurement, because only ImGui knows how wide a string is in the loaded
// font at the current scale.
//
// The label column, the widest label seen, and the "have I reported this
// width yet" flag are this module's own file-scope state now (not exposed
// here) -- see LabelledControl.cpp's comment above them for why they are
// shared mutable state across every call in a frame rather than parameters.
// `reportLabelColumnIfChanged()` is the one seam MacPaintUI.cpp's `drawUI()`
// still needs into that state, for the "[controls] label column ..." log
// line -- everything else about the column is read only by the functions
// below.

namespace np {

// Draws `label`, positions the cursor for the widget and sizes it. Returns
// the `##`-prefixed id the widget must be given, in a caller-owned buffer, so
// the label is never drawn twice.
void beginLabelled(const char* label, char* idOut, size_t idCap);

bool ctlSlider(const char* label, float* v, float lo, float hi, const char* fmt = "%.3f");

bool ctlSliderInt(const char* label, int* v, int lo, int hi);

// BeginCombo, laid out the same way. The caller ends it with EndCombo() as
// usual -- this only replaces the label and the width.
bool ctlBeginCombo(const char* label, const char* preview);

// `ImGui::TextDisabled()` that wraps at the panel edge. The same failure as
// the labels above, in a different widget: the layers panel's own status
// lines carry a document name and a counter triple, neither of which is
// bounded, and an unwrapped one is cut off by the window rather than
// continued.
void textDisabledWrapped(const char* fmt, ...) IM_FMTARGS(1);

bool ctlInputText(const char* label, char* buf, size_t cap, ImGuiInputTextFlags flags);

// Prints the "[controls] label column ..." diagnostic line the first time the
// column's width has changed since the last call -- once at startup, and
// again only if a wider label ever appears. `panelWidthPx` is the column's
// own docked width (`kControlsW` in MacPaintUI.cpp) and `availWidthPx` is
// `ImGui::GetContentRegionAvail().x` measured at the call site; both are
// needed only to format the message, not to decide whether to print it.
//
// This exists only because the label-column state moved into
// LabelledControl.cpp as file-static: `drawUI()` used to read
// `g_labelColumn`/`g_reportedColumn`/`g_widestLabel`/`g_widestLabelPx`
// directly for this one log line, which is the sole place outside this
// cluster's own functions that touched them.
void reportLabelColumnIfChanged(float panelWidthPx, float availWidthPx);

}  // namespace np
