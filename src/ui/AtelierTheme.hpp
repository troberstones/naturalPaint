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
// | rule -- 2px between major regions            | #f3f2f2 |
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
constexpr uint32_t kRule          = 0xf3f2f2;
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
// (1px internal separators) that currently resolves to `chrome mid`, and
// `text primary` / `text secondary` resolve to `rule` / `hairline`. Asserting
// the equalities pins the intent -- if a later revision wants a divider that
// is not the tab-strip fill, it has to change the token and the assertion
// together rather than discovering that half the UI moved.
static_assert(kDivider == kChromeMid, "divider is the chrome-mid value in a separator role");
static_assert(kTextPrimary == kRule, "text primary is the rule value in a type role");
static_assert(kTextSecondary == kHairline, "text secondary is the hairline value in a type role");
static_assert(kOnAccent == kChromeDeep, "on-accent foreground is the chrome-deep value");

// docs/ui.md section 1: "Rules: 2px #f3f2f2 between major regions, 1px
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

}  // namespace np
