#pragma once
#include <cstdint>

// ui/AtelierTheme -- the design tokens of docs/ui.md section 1, as data.
//
// This replaces ui/Theme's `applyMacPaintDarkTheme()`, which was a 1984
// bitmap-UI pastiche: near-black panels (#121213-ish) and a wet-ink cyan
// accent, neither of which appears anywhere in the design. docs/ui.md is
// explicit that `src/ui/MacPaintUI.*` and `src/ui/Theme.*` are "replaced, not
// extended"; this is the first half of that, and the panel *bodies* are still
// MacPaintUI's -- see ui/AtelierChrome.hpp for what has moved and what
// has not.
//
// The tokens are `constexpr uint32_t` 0xRRGGBB literals rather than ImVec4s so
// that `--selftest` can compare them against the hex in the document without
// a float tolerance and without an ImGui context. A token that drifts from
// docs/ui.md fails the suite by exact equality, which is the only kind of
// check worth having for a table of hex values.
namespace np {

// docs/ui.md section 1, "Tokens". Dark chrome, light paper.
//
// | role                                        | value   |
// |---------------------------------------------|---------|
// | chrome base -- panels, palette, status bar   | #2d2b2b |
// | chrome deep -- tool options, active tab, nav | #201e1d |
// | chrome mid -- tab strip, internal fills      | #444141 |
// | rule -- 2px between major regions            | #201e1d |
// | divider -- 1px internal                      | #444141 |
// | hairline, rulers                             | #9b9797 |
// | text primary                                 | #f3f2f2 |
// | text secondary                               | #9b9797 |
// | accent -- active tool, dirty marker, select  | #ff563c |
// | row selected                                 | #7c1405 |
// | canvas paper                                 | #f8f4f4 |
// | on-accent foreground                         | #201e1d |
constexpr uint32_t kChromeBase    = 0x2d2b2b;
constexpr uint32_t kChromeDeep    = 0x201e1d;
constexpr uint32_t kChromeMid     = 0x444141;
constexpr uint32_t kRule          = 0x201e1d;
constexpr uint32_t kDivider       = 0x444141;
constexpr uint32_t kHairline      = 0x9b9797;
constexpr uint32_t kTextPrimary   = 0xf3f2f2;
constexpr uint32_t kTextSecondary = 0x9b9797;
constexpr uint32_t kAccent        = 0xff563c;
constexpr uint32_t kRowSelected   = 0x7c1405;
constexpr uint32_t kCanvasPaper   = 0xf8f4f4;
constexpr uint32_t kOnAccent      = 0x201e1d;

// Four of the twelve rows above are the same value under two names, and that
// is deliberate rather than a table that wants tidying: `divider` is a role
// (1px internal separators) that currently resolves to `chrome mid`,
// `text secondary` resolves to `hairline`, and `rule` / `on-accent
// foreground` both resolve to `chrome deep`. Asserting the equalities pins
// the intent -- if a later revision wants a divider that is not the tab-strip
// fill, it has to change the token and the assertion together rather than
// discovering that half the UI moved.
//
// **`rule` used to be `#f3f2f2`, the same near-white as `text primary`, and
// the pair was asserted here.** Changing it to `chrome deep` was a design
// decision taken from four rendered candidates (near-white, chrome deep,
// chrome mid, and no rule at all): the chrome is built from tonal steps, and
// a near-white 2px line was the only element in it drawn as a *line* rather
// than as a change of ground. It read as a seam laid over the layout instead
// of a boundary belonging to it.
//
// The consequence, stated because it is a real one and not a side effect
// worth discovering later: where a rule borders a `chrome deep` surface --
// the tool options bar is one -- the rule is now invisible against that side
// and the boundary is carried entirely by the other. That is the intended
// reading (the bar simply ends) and is what the chosen candidate actually
// showed, but a future revision wanting a rule visible against dark chrome
// needs a *third* value here, not a nudge to this one.
static_assert(kDivider == kChromeMid, "divider is the chrome-mid value in a separator role");
static_assert(kRule == kChromeDeep, "rule is the chrome-deep value in a separator role");
static_assert(kTextSecondary == kHairline, "text secondary is the hairline value in a type role");
static_assert(kOnAccent == kChromeDeep, "on-accent foreground is the chrome-deep value");
static_assert(kTextPrimary != kRule,
              "text primary and rule were the same near-white until the rule became a seam; if "
              "they are equal again the divider change has been reverted by halves");

// docs/ui.md section 1: "Rules: 2px #201e1d between major regions, 1px
// #444141 internally."
constexpr float kRuleThickness    = 2.0f;
constexpr float kDividerThickness = 1.0f;

// `ImGuiStyle::WindowPadding` and `::ScrollbarSize`, named and shared here
// rather than left as literals inside `applyAtelierTheme()`'s body and
// duplicated as a second literal wherever a window's *content* width has to
// be computed ahead of time. That duplication is a real defect this project
// shipped once already: `ui/AtelierLayout.hpp`'s tool-palette width was
// sized as `kToolCellSize + 8`, which is `kToolCellSize` plus *half* of
// `WindowPadding` (8px is per side, 16px total) and none of `ScrollbarSize`
// -- a 36px icon drawn into a palette whose real content region was 16px
// wide, sliced in half, caught by a screenshot rather than by anything that
// runs in `--selftest`. `applyAtelierTheme()` now assigns
// `ImGuiStyle::WindowPadding`/`::ScrollbarSize` *from* these two constants
// rather than from separate literals, so the two cannot drift apart again by
// construction; `ui/AtelierLayout.hpp`'s `kToolPaletteW` is checked against
// them by a `static_assert`, and `app/selftest/AtelierChrome.cpp` checks the
// same thing again against a live `ImGuiStyle` and a real
// `BeginChild()`/`GetContentRegionAvail()`, because a hand-derived formula
// agreeing with itself is not evidence that Dear ImGui's own layout code
// agrees with it too.
constexpr float kWindowPaddingX = 8.0f;
// The other half of the same pair. It had no name until docked panels needed
// one: `applyAtelierTheme()` wrote `ImVec2(kWindowPaddingX, 8)` with the second
// number as a bare literal, and a panel body that has to inset its content by
// exactly what the windows it replaced did cannot read a literal. Same rule as
// kWindowPaddingX's own: the style is assigned FROM this constant, so the two
// cannot drift.
constexpr float kWindowPaddingY = 8.0f;
constexpr float kScrollbarSize  = 12.0f;

// `ui/AtelierLayout.hpp`'s `kToolPaletteW` no longer subtracts
// `kScrollbarSize` -- the tool grid's `BeginChild()` carries
// `ImGuiWindowFlags_NoScrollbar` (see that file's kToolPaletteW comment for
// the fuller account), so there is no scrollbar reserving width inside the
// palette to account for any more. `kScrollbarSize` is still real and still
// assigned to `ImGuiStyle::ScrollbarSize` below: other scrolling regions in
// this chrome (the controls column) still draw one.

// The canvas surround -- and the one token that is deliberately NOT taken
// from the design.
//
// docs/ui.md's own warning callout, in full: the wireframe painted the canvas
// surround at `#2d2b2b`, near black, and "simultaneous contrast makes paint
// read lighter and more saturated against a near-black surround than it truly
// is, which is precisely the judgement a painting application must not
// distort." The instruction that follows is not "use a lighter grey" -- it is
// "make the surround a separate, user-adjustable value defaulting to
// mid-grey, not to the chrome colour," which is PRD **L6** (P2).
//
// So this is a default, not a constant, and `atelierSurround()` below is
// mutable state. The two facts worth keeping true are that it starts at
// mid-grey and that it is not tied to `kChromeBase`; both are asserted.
constexpr uint32_t kCanvasSurroundDefault = 0xbab6b6;
static_assert(kCanvasSurroundDefault != kChromeBase,
              "PRD L6 / docs/ui.md: the surround is a separate value from the chrome, "
              "because paint judged against near-black reads wrong");

// The current canvas surround (PRD L6). Session state, like zoom and the
// mirror axes: it never touches the document, and no file records it.
uint32_t atelierSurround() noexcept;
void setAtelierSurround(uint32_t rgb) noexcept;

// Unpack a 0xRRGGBB token to three [0,1] floats, in the order R, G, B.
// sRGB-encoded values -- these are chrome, drawn by ImGui straight into the
// swapchain, not paint.
void unpackRgb(uint32_t rgb, float out[3]) noexcept;

// Push docs/ui.md section 1's design language into the live ImGuiStyle:
// "flat fills, hard rules, no radius, no gradients outside the colour
// picker, one shadow (on the canvas)."
void applyAtelierTheme();

// ---------------------------------------------------------------------------
// The modal dim, restricted to the chrome.
//
// The user's instruction, in two parts. First: *"dont' gray out the UI when any
// modal panels are opened."* Then, on seeing that: *"what if the image stayed
// un-grayed, but the toolbox and control panels were grayed out"* -- which is
// the better answer, because the two halves of the screen want opposite things
// from a modal.
//
// A modal dialog here is usually an **editor whose output is the canvas**:
// thirteen of the Image > Adjustments dialogs carry a live preview, and a wash
// over the image defeats the only feedback their sliders have. The chrome is
// the opposite case -- a toolbox that is not clickable should not look
// clickable, and the dim was the only thing ever saying so. So: image at full
// strength, chrome greyed.
//
// ImGui cannot express that. `ImGuiCol_ModalWindowDimBg` is one rect over the
// whole viewport, captured per-modal in `Begin()` and drawn at end of frame; it
// is all or nothing. That token is therefore held at alpha 0 in
// `applyAtelierTheme()` (which records the reasoning at length) and the
// selective wash is these two functions instead.
//
// The wash is drawn two ways, and which one a window gets is decided by one
// ImGui rule that is easy to get backwards. **imgui.cpp:7077**, in
// `CreateNewWindow()`: a window flagged `NoBringToFrontOnFocus` is
// `push_front`ed into `g.Windows` and every other window is `push_back`ed, into
// a list that is render order. So that flag does not mean "keeps its place", it
// means **"goes to the very back, permanently"** -- `FocusWindow()` then
// refuses to lift it (imgui.cpp:13938). A window's slot is chosen once, when it
// is created, and nothing but a focus event moves it again.
//
//   1. **`ui/MacPaintUI.cpp`'s chrome scrim** covers every window that carries
//      that flag: the title band, the tab strip, all four docks and the status
//      bar (`beginBand()` in ui/AtelierChrome.cpp sets it) plus the canvas. One
//      window, geometry `viewport minus bands.canvas`, and being an ordinary
//      `push_back` window is exactly what puts it above all of them -- for good,
//      whenever each was created. It has to be a separate window rather than a
//      wash inside each: **a child window owns its own draw list and renders
//      after its parent's**, so a dock washing itself greys its background and
//      leaves every swatch, layer row and tool button at full brightness. That
//      was built, measured, and is why the scrim exists.
//   2. **`washCurrentWindowForModal()`** is for the chrome that the scrim
//      cannot reach: the flyout rail and an open flyout panel. Both sit inside
//      the canvas rect on purpose -- that is what makes them cost the docks
//      nothing -- and neither carries the flag, so both are `push_back` windows
//      created after the scrim and therefore above it. They grey themselves,
//      which is order-free: the rect is over that window's content because it
//      was submitted after it, and under every other window because it belongs
//      to that window's list.
//
// The scrim's other ordering claim -- that it stays *below* the dialog -- rests
// on the same one-slot-per-window rule, and cost a second measured mistake to
// get right. Dialogs are submitted early in the frame and the chrome late, so a
// modal already open on frame 1 (`--open-layer-properties`, or a crash-recovery
// prompt at launch) would create its popup window *before* a scrim first
// submitted after the chrome, leaving the scrim on top of the dialog. A
// sabotage that widened the scrim to the whole viewport greyed the dialog and
// showed it. The scrim is therefore `Begin()`/`End()`ed once, empty, before the
// dialog pass purely to claim its slot, and appended to later with the actual
// rects -- ImGui allows a window to be begun more than once per frame, and the
// second pass draws into the same list without moving it.
bool modalDimActive();

// Grey the current ImGui window. Call last, inside the window, before `End()`.
void washCurrentWindowForModal();

// Grey one rect into the current window's draw list, so both routes above use
// one colour.
void washRectForModal(float x0, float y0, float x1, float y1);

}  // namespace np
