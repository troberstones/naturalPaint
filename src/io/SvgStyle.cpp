#include "io/SvgStyle.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <utility>

namespace np {

namespace {

// A selector this long is not something a hand-written or exported document
// produces; capping it bounds both the memory one selector can hold and the
// cost of matching it (matching recurses once per compound).
//
// **64 specifically, because `matchChain()`'s failure memo packs the compound
// index into a `uint64_t` bitmask.** That is the cap being spent deliberately
// rather than a round number -- see the memo's own comment for why matching
// needs one at all.
constexpr size_t kMaxCompoundsPerSelector = 64;

// A style="" block or a <style> rule's body with more declarations than this
// is not a legitimate one; the rest are dropped rather than grown into an
// unbounded vector.
constexpr size_t kMaxDeclarationsPerBlock = 4096;

bool isCssSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

std::string trim(const std::string& s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && isCssSpace(s[b])) ++b;
  while (e > b && isCssSpace(s[e - 1])) --e;
  return s.substr(b, e - b);
}

struct StrippedCss {
  std::string text;
  bool truncated = false;  // an unterminated comment ate the rest of the input
};

// Removes /* ... */ comments. An unterminated comment consumes to the end of
// the input -- bounded, and the only sane thing to do with "malformed
// forever" rather than guess where the author meant it to end. Comments do
// not leave a separating space behind (matching real CSS tokenising, where a
// comment is removed before tokens are split), so "a/**/b" becomes "ab".
StrippedCss stripCssComments(std::string_view text) {
  StrippedCss result;
  result.text.reserve(text.size());
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    if (text[i] == '/' && i + 1 < n && text[i + 1] == '*') {
      const size_t end = text.find("*/", i + 2);
      if (end == std::string_view::npos) {
        result.truncated = true;
        break;
      }
      i = end + 2;
      continue;
    }
    result.text.push_back(text[i]);
    ++i;
  }
  return result;
}

// If `value` ends in `!important` (any case, any whitespace around the
// `!`), strips it and returns true. Comparing byte-for-byte after lowering
// is enough here -- SVG/CSS keywords are ASCII.
bool stripTrailingImportant(std::string* value) {
  static constexpr std::string_view kWord = "important";
  std::string& v = *value;
  size_t end = v.size();
  while (end > 0 && isCssSpace(v[end - 1])) --end;
  if (end < kWord.size()) return false;
  const size_t wordStart = end - kWord.size();
  for (size_t k = 0; k < kWord.size(); ++k) {
    if (std::tolower(static_cast<unsigned char>(v[wordStart + k])) != kWord[k]) {
      return false;
    }
  }
  size_t bang = wordStart;
  while (bang > 0 && isCssSpace(v[bang - 1])) --bang;
  if (bang == 0 || v[bang - 1] != '!') return false;
  size_t newEnd = bang - 1;
  while (newEnd > 0 && isCssSpace(v[newEnd - 1])) --newEnd;
  v = v.substr(0, newEnd);
  return true;
}

// The first whitespace/`{`/`;`-delimited word of an at-rule, for messages:
// "@media", "@import", ...
std::string atRuleName(const std::string& css, size_t start, size_t limit) {
  size_t k = start;
  while (k < limit && !isCssSpace(css[k]) && css[k] != '{' && css[k] != ';') {
    ++k;
  }
  return css.substr(start, k - start);
}

// One compound token's own syntax: "rect", ".foo", "#bar", "*", "rect.foo#bar".
// Refuses attribute selectors, pseudo-classes/elements, and a stray '*'
// anywhere but the leading "any type" position -- all by returning false
// with `*refusal` naming what was found.
bool parseCompoundToken(const std::string& token, SvgCompoundSelector* out,
                        std::string* refusal) {
  if (token.empty()) {
    *refusal = "empty compound selector";
    return false;
  }
  if (token.find('[') != std::string::npos) {
    *refusal = "attribute selector '" + token + "'";
    return false;
  }
  if (token.find(':') != std::string::npos) {
    *refusal = "pseudo-class or pseudo-element '" + token + "'";
    return false;
  }
  if (token.find('*') != std::string::npos) {
    const bool leadingUniversal =
        token[0] == '*' &&
        (token.size() == 1 || token[1] == '.' || token[1] == '#');
    if (!leadingUniversal || token.find('*', 1) != std::string::npos) {
      *refusal = "unrecognised selector syntax '" + token + "'";
      return false;
    }
  }

  size_t i = 0;
  const size_t n = token.size();
  if (token[0] == '*') {
    ++i;  // universal: type stays empty
  } else {
    const size_t start = i;
    while (i < n && token[i] != '.' && token[i] != '#') ++i;
    if (i > start) out->type = token.substr(start, i - start);
  }
  while (i < n) {
    if (token[i] == '.') {
      ++i;
      const size_t start = i;
      while (i < n && token[i] != '.' && token[i] != '#') ++i;
      if (i == start) {
        *refusal = "malformed class selector in '" + token + "'";
        return false;
      }
      out->classes.push_back(token.substr(start, i - start));
    } else if (token[i] == '#') {
      ++i;
      const size_t start = i;
      while (i < n && token[i] != '.' && token[i] != '#') ++i;
      if (i == start) {
        *refusal = "malformed id selector in '" + token + "'";
        return false;
      }
      out->id = token.substr(start, i - start);
    } else {
      *refusal = "unrecognised selector syntax '" + token + "'";
      return false;
    }
  }
  return true;
}

// One comma-separated selector alternative: tokenises on whitespace and '>',
// refuses '+'/'~' (sibling combinators), a leading or dangling combinator,
// and each unsupported compound -- naming whichever was found in `*refusal`.
bool parseSelectorPiece(const std::string& raw, SvgSelector* out,
                        std::string* refusal) {
  const std::string text = trim(raw);
  if (text.empty()) {
    *refusal = "empty selector -- skipped";
    return false;
  }

  std::vector<SvgCompoundSelector> compounds;
  SvgCombinator pending = SvgCombinator::kSelf;
  bool pendingSet = false;
  size_t i = 0;
  const size_t n = text.size();

  while (i < n) {
    while (i < n && isCssSpace(text[i])) ++i;
    if (i >= n) break;
    const char c = text[i];

    if (c == '>') {
      if (compounds.empty() && !pendingSet) {
        *refusal = "selector '" + text + "' starts with a combinator -- skipped";
        return false;
      }
      pending = SvgCombinator::kChild;
      pendingSet = true;
      ++i;
      continue;
    }
    if (c == '+' || c == '~') {
      *refusal = std::string("sibling combinator '") + c + "' in selector '" +
                text + "' -- skipped";
      return false;
    }

    const size_t start = i;
    while (i < n && !isCssSpace(text[i]) && text[i] != '>' && text[i] != '+' &&
          text[i] != '~') {
      ++i;
    }
    const std::string token = text.substr(start, i - start);

    SvgCompoundSelector compound;
    std::string tokenRefusal;
    if (!parseCompoundToken(token, &compound, &tokenRefusal)) {
      *refusal = tokenRefusal + " in selector '" + text + "' -- skipped";
      return false;
    }
    compound.combinator =
        compounds.empty() ? SvgCombinator::kSelf
                          : (pendingSet ? pending : SvgCombinator::kDescendant);
    compounds.push_back(std::move(compound));
    if (compounds.size() > kMaxCompoundsPerSelector) {
      *refusal = "selector '" + text + "' exceeds " +
                std::to_string(kMaxCompoundsPerSelector) +
                " compounds -- skipped";
      return false;
    }
    pendingSet = false;
    pending = SvgCombinator::kSelf;
  }

  if (compounds.empty()) {
    *refusal = "selector '" + text + "' has no compounds -- skipped";
    return false;
  }
  if (pendingSet) {
    *refusal =
        "selector '" + text + "' ends with a dangling combinator -- skipped";
    return false;
  }

  SvgSpecificity spec;
  for (const auto& cp : compounds) {
    if (!cp.id.empty()) ++spec.ids;
    spec.classes += static_cast<uint32_t>(cp.classes.size());
    if (!cp.type.empty()) ++spec.types;
  }
  out->compounds = std::move(compounds);
  out->specificity = spec;
  return true;
}

bool matchesCompound(const SvgCompoundSelector& c, const SvgElementView& e) {
  if (!c.type.empty() && c.type != e.tag) return false;
  if (!c.id.empty() && c.id != e.id) return false;
  for (const auto& cls : c.classes) {
    if (std::find(e.classes.begin(), e.classes.end(), cls) == e.classes.end()) {
      return false;
    }
  }
  return true;
}

// The failure memo, and why matching cannot do without one.
//
// `matchChain()` below is a right-to-left backtracking match, and the naive
// form of it is **exponential**, not merely slow. Each descendant combinator
// loops over every ancestor and recurses, so a selector with `k` descendant
// combinators against a document `d` levels deep costs O(d^k). With the cap
// above that is 64 nested loops, and `g g g g g g g g rect` against a
// thousand nested `<g>` elements does not finish. Selector text and document
// depth both come from a file this build did not write, so that is a hang on
// hostile input -- the same class of denial the XML parser was chosen to make
// impossible, reintroduced one layer up.
//
// `matchChain(idx, element)` is a pure function of its two arguments, so
// remembering which pairs have already failed collapses it to O(compounds x
// depth). The memo maps an element to a bitmask of the `idx` values known to
// fail against it; `kMaxCompoundsPerSelector` is 64 precisely so that mask is
// one `uint64_t` and the lookup is a hash plus a bit test.
//
// Only failures are recorded. A success returns immediately and never asks
// again, so memoising it would cost a write for no read.
using MatchMemo = std::unordered_map<const SvgElementView*, uint64_t>;

// `compounds[idx]` must match `element`; `compounds[idx].combinator` describes
// how `compounds[idx]` relates to `compounds[idx - 1]`, which must then match
// some ancestor (kDescendant, tried outward until one works) or exactly the
// parent (kChild). Recursion depth is bounded by `idx`, itself bounded by
// `kMaxCompoundsPerSelector`.
bool matchChain(const std::vector<SvgCompoundSelector>& compounds, size_t idx,
                const SvgElementView& element, MatchMemo& failed) {
  const uint64_t bit = uint64_t{1} << idx;
  const auto it = failed.find(&element);
  if (it != failed.end() && (it->second & bit) != 0) return false;

  auto rememberFailure = [&]() -> bool {
    failed[&element] |= bit;
    return false;
  };

  if (!matchesCompound(compounds[idx], element)) return rememberFailure();
  if (idx == 0) return true;
  if (compounds[idx].combinator == SvgCombinator::kChild) {
    if (!element.parent) return rememberFailure();
    if (matchChain(compounds, idx - 1, *element.parent, failed)) return true;
    return rememberFailure();
  }
  for (const SvgElementView* p = element.parent; p != nullptr; p = p->parent) {
    if (matchChain(compounds, idx - 1, *p, failed)) return true;
  }
  return rememberFailure();
}

bool svgSelectorMatchesElement(const SvgSelector& selector,
                               const SvgElementView& element) {
  if (selector.compounds.empty()) return false;
  // One memo per selector-against-element query. It cannot outlive the call:
  // the keys are borrowed `SvgElementView*`, and the caller owns that tree.
  MatchMemo failed;
  return matchChain(selector.compounds, selector.compounds.size() - 1, element,
                    failed);
}

}  // namespace

bool operator<(const SvgSpecificity& a, const SvgSpecificity& b) noexcept {
  if (a.ids != b.ids) return a.ids < b.ids;
  if (a.classes != b.classes) return a.classes < b.classes;
  return a.types < b.types;
}

bool svgPropertyInherits(std::string_view property) noexcept {
  // SVG 1.1's own inheriting property set -- see io/SvgStyle.hpp's section
  // on why anything outside both this list and the (unenumerated, because
  // unnecessary -- see the header) non-inheriting list answers false.
  static constexpr std::string_view kInherited[] = {
      "fill", "fill-opacity", "fill-rule",
      "stroke", "stroke-opacity", "stroke-width", "stroke-linecap",
      "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray",
      "stroke-dashoffset",
      "color", "visibility", "clip-rule", "color-interpolation",
      "shape-rendering",
      "font-family", "font-size", "font-style", "font-weight",
      "text-anchor", "letter-spacing", "word-spacing",
      "direction", "writing-mode",
  };
  for (std::string_view p : kInherited) {
    if (p == property) return true;
  }
  return false;
}

bool parseSvgInlineStyle(std::string_view textRaw,
                         std::vector<SvgDeclaration>* out) {
  if (!out) return false;
  out->clear();

  const std::string text = stripCssComments(textRaw).text;
  const size_t n = text.size();
  size_t i = 0;
  size_t count = 0;

  while (i < n) {
    const size_t start = i;
    bool inSingle = false;
    bool inDouble = false;
    while (i < n) {
      const char c = text[i];
      if (inSingle) {
        if (c == '\'') inSingle = false;
      } else if (inDouble) {
        if (c == '"') inDouble = false;
      } else if (c == '\'') {
        inSingle = true;
      } else if (c == '"') {
        inDouble = true;
      } else if (c == ';') {
        break;
      }
      ++i;
      // An unterminated quote simply runs to the end of the chunk on the
      // next iteration's bounds check -- bounded, never a hang.
    }
    const std::string chunk = trim(text.substr(start, i - start));
    if (i < n) ++i;  // consume the ';'

    if (chunk.empty()) continue;
    if (++count > kMaxDeclarationsPerBlock) break;

    const size_t colon = chunk.find(':');
    if (colon == std::string::npos) continue;  // no colon: not a declaration
    std::string prop = trim(chunk.substr(0, colon));
    std::string value = trim(chunk.substr(colon + 1));
    if (prop.empty()) continue;
    const bool important = stripTrailingImportant(&value);
    out->push_back(SvgDeclaration{std::move(prop), std::move(value), important});
  }
  return true;
}

bool parseSvgStyleSheet(std::string_view cssRaw, SvgStyleSheet* out,
                        std::vector<std::string>* refusals) {
  if (!out || !refusals) return false;
  out->rules.clear();

  const StrippedCss stripped = stripCssComments(cssRaw);
  if (stripped.truncated) {
    refusals->push_back(
        "unterminated comment ('/* ...') -- rest of sheet ignored");
  }
  const std::string& css = stripped.text;
  const size_t n = css.size();
  size_t pos = 0;
  uint32_t sourceOrder = 0;

  while (pos < n) {
    while (pos < n && isCssSpace(css[pos])) ++pos;
    if (pos >= n) break;

    if (css[pos] == '@') {
      // At-rule: either "@foo ...;" or "@foo ... { ... }" (braces possibly
      // nested, e.g. @media wrapping ordinary rules). Skip the whole
      // construct with one balanced-brace scan; never descend into it.
      const size_t start = pos;
      size_t j = pos;
      int depth = 0;
      bool sawBlock = false;
      for (; j < n; ++j) {
        const char c = css[j];
        if (c == '{') {
          ++depth;
          sawBlock = true;
        } else if (c == '}') {
          if (depth > 0) --depth;
          if (sawBlock && depth == 0) {
            ++j;
            break;
          }
        } else if (c == ';' && depth == 0) {
          ++j;
          break;
        }
      }
      refusals->push_back("at-rule '" + atRuleName(css, start, n) +
                          "' -- skipped");
      pos = j;
      continue;
    }

    const size_t brace = css.find('{', pos);
    if (brace == std::string::npos) {
      refusals->push_back("rule '" + trim(css.substr(pos)) +
                          "' has no '{' -- rest of sheet ignored");
      break;
    }
    const std::string selectorText = css.substr(pos, brace - pos);

    // The matching '}', counting nested braces so a stray '{' inside a
    // malformed declaration block does not end the rule early.
    size_t k = brace + 1;
    int depth = 1;
    while (k < n && depth > 0) {
      if (css[k] == '{') ++depth;
      else if (css[k] == '}') --depth;
      ++k;
    }
    if (depth > 0) {
      refusals->push_back("rule '" + trim(selectorText) +
                          "' has no matching '}' -- rest of sheet ignored");
      break;
    }
    const std::string body = css.substr(brace + 1, (k - 1) - (brace + 1));
    pos = k;

    if (out->rules.size() >= kMaxSvgStyleRules) {
      refusals->push_back("sheet exceeds " +
                          std::to_string(kMaxSvgStyleRules) +
                          " rules -- remaining rules ignored");
      break;
    }

    // Split the selector list on top-level commas. None of the supported
    // constructs use a comma, and the one unsupported construct that could
    // (an attribute selector's quoted value, "[x=\"a,b\"]") is refused
    // outright regardless of where a naive split lands inside it.
    std::vector<std::string> parts;
    size_t p = 0;
    while (p <= selectorText.size()) {
      const size_t comma = selectorText.find(',', p);
      if (comma == std::string::npos) {
        parts.push_back(selectorText.substr(p));
        break;
      }
      parts.push_back(selectorText.substr(p, comma - p));
      p = comma + 1;
    }

    std::vector<SvgSelector> selectors;
    for (const std::string& part : parts) {
      SvgSelector selector;
      std::string refusal;
      if (parseSelectorPiece(part, &selector, &refusal)) {
        selectors.push_back(std::move(selector));
      } else {
        refusals->push_back(refusal);
      }
    }
    if (selectors.empty()) continue;  // every alternative refused

    std::vector<SvgDeclaration> declarations;
    parseSvgInlineStyle(body, &declarations);

    SvgRule rule;
    rule.selectors = std::move(selectors);
    rule.declarations = std::move(declarations);
    rule.sourceOrder = sourceOrder++;
    out->rules.push_back(std::move(rule));
  }
  return true;
}

std::map<std::string, std::string> svgComputeStyle(
    const SvgElementView& element,
    const std::vector<SvgDeclaration>& presentationAttributes,
    const std::vector<SvgDeclaration>& inlineStyle, const SvgStyleSheet& sheet,
    const std::map<std::string, std::string>& inheritedFromParent) {
  // Tier 1: the inherited value from the parent, verbatim.
  std::map<std::string, std::string> result = inheritedFromParent;

  // Tier 2: presentation attributes. Applied unconditionally before any
  // sheet rule is even considered -- see the header on why this, and not a
  // specificity-zero entry compared against sheet rules, is the correct
  // model: it guarantees any matching sheet rule outranks it, which a
  // specificity comparison alone could not if two sheet rules tied at
  // specificity zero (a bare "*" rule) with a presentation attribute.
  for (const auto& d : presentationAttributes) {
    result[d.property] = d.value;
  }

  // Gather every sheet declaration that matches, split by importance, each
  // tagged with the specificity and source order of the selector that
  // matched it (a rule with several comma-separated selectors can match via
  // more than one, each at its own specificity -- applying the shared
  // declarations again at that other specificity is harmless, since the
  // resulting value at each such application is identical).
  struct Matched {
    const SvgSpecificity* specificity;
    uint32_t order;
    const SvgDeclaration* declaration;
  };
  std::vector<Matched> normal;
  std::vector<Matched> important;
  for (const auto& rule : sheet.rules) {
    for (const auto& selector : rule.selectors) {
      if (!svgSelectorMatchesElement(selector, element)) continue;
      for (const auto& decl : rule.declarations) {
        Matched m{&selector.specificity, rule.sourceOrder, &decl};
        (decl.important ? important : normal).push_back(m);
      }
    }
  }
  auto bySpecificityThenOrder = [](const Matched& a, const Matched& b) {
    if (*a.specificity < *b.specificity) return true;
    if (*b.specificity < *a.specificity) return false;
    return a.order < b.order;
  };
  std::stable_sort(normal.begin(), normal.end(), bySpecificityThenOrder);
  std::stable_sort(important.begin(), important.end(), bySpecificityThenOrder);

  // Tier 3: matching sheet rules, lowest priority first so the last write
  // is the highest-specificity (or latest-in-source, at a tie) one.
  for (const auto& m : normal) {
    result[m.declaration->property] = m.declaration->value;
  }

  // Tier 4: style="" declarations, in the order they were written -- a
  // repeated property within one style attribute is a plain last-one-wins,
  // same as it would be applied inline in a browser.
  for (const auto& d : inlineStyle) {
    if (!d.important) result[d.property] = d.value;
  }

  // Tier 5: !important, mirroring tiers 3 and 4's relative order among
  // themselves -- sheet-important by specificity/order, then inline-
  // important last -- but as a whole beating every tier above regardless of
  // that tier's own specificity.
  for (const auto& m : important) {
    result[m.declaration->property] = m.declaration->value;
  }
  for (const auto& d : inlineStyle) {
    if (d.important) result[d.property] = d.value;
  }

  // The literal keyword `inherit` forces cascade step 1 for that one
  // property, whatever tier actually won above. A property with no entry to
  // inherit from (the root element, or a name the parent's own computed
  // style never set) has nothing to fall back to and is dropped rather than
  // left holding the un-parseable text "inherit".
  for (auto it = result.begin(); it != result.end();) {
    if (it->second == "inherit") {
      const auto found = inheritedFromParent.find(it->first);
      if (found != inheritedFromParent.end()) {
        it->second = found->second;
        ++it;
      } else {
        it = result.erase(it);
      }
    } else {
      ++it;
    }
  }

  return result;
}

}  // namespace np
