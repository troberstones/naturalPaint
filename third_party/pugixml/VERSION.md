# pugixml

- **Upstream:** https://github.com/zeux/pugixml
- **Tag:** `v1.14` (`PUGIXML_VERSION 1140`)
- **Licence:** MIT — see `LICENSE.md`, copied from the same tag.

## Files, and their upstream paths

| Here | Upstream | SHA-256 |
|---|---|---|
| `pugixml.hpp` | `src/pugixml.hpp` | `68ca7eac371875363257c85eea74e8e3d0a2d16e478d6685e140a9217bee36b4` |
| `pugiconfig.hpp` | `src/pugiconfig.hpp` | `797e895912fdc50e8265a99000aeebf20dd6901afdb9be8d0d9497ae14333e24` |
| `pugixml.cpp` | `src/pugixml.cpp` | `9a9e1cf3afee5d70da7d9aa9eb1bfaba9752467f0f5e2dd8fca18cc80aeb57f9` |

All three are **byte-identical to the tag**. `pugiconfig.hpp` in particular is
unedited: the one build option this project sets, `PUGIXML_NO_XPATH`, is passed
from `src/CMakeLists.txt` as a compile definition instead. That keeps the above
hashes verifiable in one command —

```bash
shasum -a 256 third_party/pugixml/pugixml.hpp third_party/pugixml/pugiconfig.hpp third_party/pugixml/pugixml.cpp
```

— which an edited config header would quietly make impossible.

## Why this parser

An SVG is a file this build did not write. The two classic XML attacks are both
parser *features* being abused: XXE needs external entity resolution, and the
billion-laughs expansion needs a DTD-declared entity table.

pugixml has neither, and that was checked in the source rather than taken from
the documentation:

- `strconv_escape()` (`pugixml.cpp`) expands exactly `&#…;` plus `&amp;`,
  `&apos;`, `&gt;`, `&lt;` and `&quot;`. There is no user-entity table to grow.
- The strings `SYSTEM` and `PUBLIC` do not occur anywhere in `pugixml.cpp`, so
  there is no code path that could resolve an external identifier.
- `parse_doctype` — which this project does not enable — only *preserves the
  `<!DOCTYPE>` node in the tree*. It does not process the DTD.

So both attacks are impossible by construction, rather than by hardening this
project would have to keep correct itself. Being MIT and three files is why it
is cheap; the above is why it is the right one.
