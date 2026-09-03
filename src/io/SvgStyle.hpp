#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// io/SvgStyle -- the CSS cascade for SVG presentation, and nothing else.
//
// ============================================================================
// What this file answers, and what it deliberately does not
// ============================================================================
//
// An SVG element's paint can be set three different ways -- a presentation
// attribute (`fill="red"`), a matching rule in a `<style>` sheet, or a
// `style=""` attribute -- and the same property can appear in more than one
// of them at once. This file answers exactly one question: **which
// declaration wins**. It does not know what a colour, a length or a paint
// server looks like; every value that comes out of `svgComputeStyle()` is the
// same text that went in on whichever side supplied it. Parsing `"red"` into
// RGB, `"2px"` into a device length, or `"url(#grad1)"` into a paint-server
// reference is somebody else's job -- the SVG importer, once one is vendored
// -- and keeping that boundary sharp is why this module can be built, and
// fully tested, before that importer exists.
//
// ============================================================================
// Why selectors match a plain struct, not an XML node
// ============================================================================
//
// A CSS engine ordinarily walks the document tree it is embedded in. This one
// instead matches against `SvgElementView`, a small plain struct the caller
// fills in per element (tag, id, classes, and a pointer to the parent's own
// view). That is a deliberate boundary, not a shortcut standing in for a
// missing piece:
//
//  * **No XML parser dependency.** This module has none, and testing it does
//    not require constructing a document -- a `--selftest` assertion builds
//    an `SvgElementView` chain by hand in three lines. A cascade wired to a
//    live DOM cannot be tested without one.
//  * **This track can exist before the XML parser does.** The importer that
//    will eventually walk real SVG files and fill in one `SvgElementView`
//    per node is separate, ongoing work; this module does not wait on it,
//    and the importer does not need to know how specificity is computed to
//    fill the struct in.
//  * **The alternative is a template or virtual `Node` interface** so this
//    file could walk "some tree" generically. That buys nothing here: the
//    only things a selector ever asks a node are its tag, its id, its
//    classes and its parent -- exactly `SvgElementView`'s four fields. A
//    generic node interface would cost an indirection per access to let this
//    file *not* know four field names it already has to know.
//
// ============================================================================
// The cascade order (SVG 1.1 section 6.4)
// ============================================================================
//
// Lowest priority to highest:
//
//   1. the inherited value from the parent
//   2. presentation attributes (`fill="red"` as an XML attribute)
//   3. matching `<style>` sheet rules, ordered by specificity then by
//      source order
//   4. `style=""` declarations on the element itself
//   5. `!important` declarations, in that same relative order among
//      themselves
//
// Step 2 sitting *below* step 3 is the one people get backwards, because
// every other styling language on the web treats "the attribute written on
// the tag" as at least as strong as "a rule from a separate sheet". SVG does
// not: section 6.4 defines presentation attributes as author-level rules of
// specificity zero that are conceptually the first rules in the document, so
// *any* matching sheet rule -- even a bare type selector, specificity
// (0,0,1) -- outranks them. `svgComputeStyle()` applies presentation
// attributes and sheet rules as two separate, ordered passes rather than
// folding presentation attributes into the specificity comparison, and
// `--selftest` asserts the outranking directly rather than leaving it to be
// discovered by a wrong rendering.
//
// The literal value `inherit` on any property means "take the parent's
// computed value", regardless of whether that property is normally one that
// inherits -- it forces step 1 for that one property, overriding whatever
// step 2-5 declaration produced the text `"inherit"`.
//
// ============================================================================
// Selectors: what is supported, and what is refused by name
// ============================================================================
//
// Supported: type (`rect`), class (`.foo`), id (`#bar`), universal (`*`),
// compound selectors (`rect.foo#bar`), comma selector lists (`a, b`), the
// descendant combinator (`g rect`) and the child combinator (`g > rect`).
// CSS comments (`/* ... */`) are stripped anywhere, including between a
// selector and its `{`.
//
// Refused, one entry per skipped selector or rule in `*refusals`, naming
// what was refused: attribute selectors (`[x]`), pseudo-classes (`:hover`),
// pseudo-elements (`::before`), sibling combinators (`+`, `~`), and at-rules
// (`@media`, `@import`, `@font-face`, ...). A refused construct contributes
// nothing -- it is never kept as a rule that matches nothing, which would
// let an importer silently render the wrong thing while looking like it
// parsed the sheet correctly. Parsing continues after a refusal: one bad
// alternative in a comma list drops only that alternative, and one
// unparsable rule does not stop the rest of the sheet.
//
// Specificity is the standard `(ids, classes, types)` triple; `*` and an
// unspecified type both contribute nothing to it.
//
// ============================================================================
// Which properties inherit, and the default for one this file has never
// heard of
// ============================================================================
//
// The inheriting properties are SVG 1.1's own list (see `svgPropertyInherits()`
// in the .cpp for the exact set, rather than duplicating it here where it
// could drift out of sync): the paint and text properties -- `fill`,
// `stroke` and their siblings, `color`, `font-*`, `text-anchor` and so on.
// Structural and effect properties -- `opacity`, `clip-path`, `mask`,
// `filter`, `transform`, `display`, `overflow`, the gradient-stop
// properties, `style`, `class`, `id` -- do not.
//
// A property in neither list -- a future SVG addition, a vendor extension,
// or a typo -- defaults to **not inheriting**. That matches ordinary CSS:
// the Cascading and Inheritance spec's default for any property that does
// not say otherwise is "inherited: no", so a name this module does not
// recognise behaves the way an unrecognised property behaves in a real
// browser, not the way this module's own inheriting list happens to lean.
// The alternative -- default to inheriting, on the reasoning that most of
// the properties SVG presentation deals with (`fill`, `stroke`, `font-*`) do
// inherit -- has a larger blast radius on the case it is wrong about: an
// unrecognised property silently propagating down an entire subtree is a
// bigger mistake than confining it to the one element that declared it.
// `--selftest` pins this choice on a made-up property name.
//
// ============================================================================
// Robustness: this eats text from files the build did not write
// ============================================================================
//
// Nothing below may crash, hang or allocate unboundedly on malformed input:
// a declaration with no colon, an unterminated `/* comment`, an unterminated
// quoted string inside a value, a `{` with no matching `}`, a rule with an
// empty selector, braces nested inside a declaration block, or a sheet of
// 10 000 rules. Parsing is a single forward scan with no recursion, so an
// adversarial nesting depth in the *text* cannot blow the stack; the one
// explicit cap on the sheet itself is `kMaxSvgStyleRules` below, past which
// further rules are refused by name rather than parsed -- three orders of
// magnitude above any sheet a real document ships. (Matching a parsed
// selector against a real element chain does recurse, but its depth is
// bounded by that selector's own compound count, itself capped in the .cpp,
// not by anything an attacker controls independently of the sheet-size cap
// above.) A malformed *declaration* is simply not produced -- no colon means
// nothing to resolve into a property and a value, and there is no rule for
// it to spoil the way an unsupported selector would. A malformed *rule* --
// unterminated comment, unterminated brace, empty selector -- is named in
// `*refusals` exactly like an unsupported construct, because from the
// caller's side "the syntax was broken" and "the syntax is one this module
// does not implement" have the same right answer: skip it, say so, keep
// going.
namespace np {

// One CSS declaration: still text on both sides -- see the header's opening
// section on why this module does not parse values.
struct SvgDeclaration {
  std::string property;
  std::string value;
  bool important = false;
};

// The element facts a selector can match on, filled in by the caller from
// its own DOM. Deliberately not an XML node -- see this header's section on
// why.
struct SvgElementView {
  std::string tag;
  std::string id;
  std::vector<std::string> classes;
  const SvgElementView* parent = nullptr;
};

// CSS specificity as the standard (ids, classes, types) triple. `*` and an
// omitted type contribute to neither `classes` nor `types`.
struct SvgSpecificity {
  uint32_t ids = 0;
  uint32_t classes = 0;
  uint32_t types = 0;
};

// Ordinary tuple comparison, most significant field first: one id outranks
// any number of classes, one class outranks any number of types.
bool operator<(const SvgSpecificity& a, const SvgSpecificity& b) noexcept;

// How a compound selector relates to the one before it in its selector.
// `kSelf` marks the first compound, which has nothing before it to relate
// to.
enum class SvgCombinator {
  kSelf,
  kDescendant,  // "g rect" -- rect is a descendant, at any depth, of g
  kChild,       // "g > rect" -- rect is a direct child of g
};

// One simple-or-compound selector, e.g. "rect.foo#bar": a type, an id and a
// set of classes, every one of which must match, plus how this compound
// relates to the previous one in its selector's chain.
struct SvgCompoundSelector {
  std::string type;                  // empty: unconstrained ("*" or omitted)
  std::string id;                    // empty: unconstrained
  std::vector<std::string> classes;  // every one must be present
  SvgCombinator combinator = SvgCombinator::kSelf;
};

// A full selector, left to right in source order: "g > rect.foo" is two
// compounds, `{type="g"}` then `{type="rect", classes=["foo"],
// combinator=kChild}`. `compounds.back()` is matched against the element
// itself.
struct SvgSelector {
  std::vector<SvgCompoundSelector> compounds;
  SvgSpecificity specificity;  // computed once, at parse time
};

// One rule: a comma-separated selector list sharing one declaration block,
// plus its position in the sheet for the source-order tiebreak. A selector
// list is not flattened into separate rules -- the declarations are shared
// and identical either way, so keeping them as one rule is simply fewer
// copies, not a different meaning.
struct SvgRule {
  std::vector<SvgSelector> selectors;
  std::vector<SvgDeclaration> declarations;
  uint32_t sourceOrder = 0;
};

// Refused past this many rules, so a hostile or merely absurd sheet cannot
// make this module allocate without bound. Real SVG documents ship tens of
// rules; this is roughly three orders of magnitude past that.
inline constexpr size_t kMaxSvgStyleRules = 50000;

struct SvgStyleSheet {
  std::vector<SvgRule> rules;  // source order
};

// Parses a <style> element's text body. Every construct this module cannot
// express -- see the header's selector section -- is skipped and named in
// `*refusals`, never kept as a rule that silently matches nothing. Returns
// false only when `out` or `refusals` is null; a sheet that is entirely
// malformed still returns true, with an empty sheet and a full `*refusals`.
bool parseSvgStyleSheet(std::string_view css, SvgStyleSheet* out,
                        std::vector<std::string>* refusals);

// Parses a style="" attribute's declaration list ("fill:red; stroke:none").
// A chunk with no ':' is not a declaration and is silently dropped.
bool parseSvgInlineStyle(std::string_view text,
                         std::vector<SvgDeclaration>* out);

// Resolves one element's computed properties per the cascade order at the
// top of this file. `inheritedFromParent` IS "the inherited value from the
// parent" (cascade step 1) -- this function does not re-derive it from
// `svgPropertyInherits()`, it trusts the caller to have already built that
// map correctly for this element. (`svgPropertyInherits()` exists for the
// caller to do exactly that -- see below.)
std::map<std::string, std::string> svgComputeStyle(
    const SvgElementView& element,
    const std::vector<SvgDeclaration>& presentationAttributes,
    const std::vector<SvgDeclaration>& inlineStyle,
    const SvgStyleSheet& sheet,
    const std::map<std::string, std::string>& inheritedFromParent);

// Whether `property` inherits, per SVG 1.1's property tables. A name outside
// both the inheriting and non-inheriting lists answers false -- see this
// header's section on why not inheriting is the safer default. The caller
// uses this to build the `inheritedFromParent` map it passes to THIS
// element's children: copy through the entries `svgComputeStyle()` just
// returned for which this answers true, and nothing else.
bool svgPropertyInherits(std::string_view property) noexcept;

}  // namespace np
