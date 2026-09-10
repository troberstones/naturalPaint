# Phase 19 — Automate it: commands, actions, replay and batch (plan)

**Goal.** Two things, sharing one mechanism.

1. **Record and replay.** Arm a recorder, do the work by hand, stop. The result is a
   named, editable, diffable file that replays on another document: *"resize to
   512×512"* at the trivial end, *"flatten image to a layer, blur it, set its blend mode
   to Subtract, threshold it"* at the other.
2. **Batch.** Point that same action at a folder or a chosen file set with an output
   rule, and get a per-file report — with no input modified, and a run that fails on
   file 12 of 40 leaving files 13–40 untouched and saying so.

Requirements: [PRD.md §P](../PRD.md) P1–P6, plus the two P2 image ops
([PLAN.md](../PLAN.md) Phase 19 step 5) parked here because nothing depends on them.

> **This phase's dependencies were satisfied at phase 6.** It is last in PLAN.md because
> nothing depends on it, not because it is low value. For texture preparation — the
> primary user's stated job — it is arguably the highest-value phase in the plan's
> second half.

---

## 1. The two shape decisions, stated first

### A chain, not a node graph

The alternative considered was a node graph with an explicit **file-read node**, a
**file-save node** and processing nodes between. Rejected on three grounds, each a
property of this codebase rather than general taste:

1. **The runner iterates; a chain does not.** P2 is one action over N files. If read is a
   node, either the graph re-runs per file — in which case the read node is a constant
   standing in for "the current input" and earns nothing — or the graph models
   cardinality: foreach nodes, collection ports, fan-out. That is the expensive half of a
   compositor, for nothing §P asks.
2. **A save node deletes P4, the only P0 in the section.** "Never partially overwrites an
   input" is a guarantee the runner can make *because it owns open and save*: it refuses
   a plan whose outputs collide with its inputs before writing a byte. Wired by the user,
   that pre-flight is unprovable. The `fopen` trap in §7 makes this concrete.
3. **A recorded sequence is already linear.** The thing the user does by hand is an
   ordered list of commands. Recording it into a DAG means inventing edges nobody drew.

A DAG buys branching and multi-input — compositing two files, fanning one source to
several treatments. Real features; not in §P; and layers plus `io/ExportStates` already
reach them from the other side. Reach for a graph when someone asks for multi-input
composition or reusable sub-graphs, not before.

### A recorded *command*, never a recorded UI event

Photoshop's Actions record UI events, which is why they break when a dialog changes or a
panel moves. PLAN.md already makes this argument about op stacks; it generalises. The
recorder taps **one function that applies a command to a document**, below every menu,
panel, dialog and key binding. Nothing it writes down contains a widget, a coordinate or
a click.

**The rule that decides what is recordable, and it sorts all 94 menu actions on its own:**

> A command is recordable **iff it can be expressed as a function of an `OpenDocument`
> alone.**

`Zoom In`, `Fit Window`, `Toggle Grayscale Preview`, tool selection and panel layout are
functions of `AppState` — session state, not the document — so they are not recordable
and their absence is not a gap. `Flatten Image`, `Gaussian Blur`, `Set Blend Mode`,
`Image Size` are functions of the document, so they are. The line is not a judgement call
made per command; it is visible in each function's existing signature.

---

## 2. The two kinds of action

They are one file format and one runner, differing in what supplies the document.

| | **Document action** | **Batch action** |
|---|---|---|
| input | the open document | a folder or chosen file set |
| output | the open document, undoable as one history entry | new files through an Export As preset |
| example | *resize to 512×512* | *grade thirty plates and write 8-bit PNGs* |
| extra state | none | source set, output directory, name template, output rule |

A batch action is **a document action plus a source and an output rule**. It is not a
second format and not a second runner: `app/Batch` opens each file, calls the same
`replayAction()` the Actions panel calls, and writes through the export path that
already exists.

---

## 3. What is already true (surveyed 2026-09-08 — checked, not assumed)

The recorder is cheap because the command layer is already built, and mostly already
funnelled. This table is the reason the estimate in §6 is days rather than weeks.

| finding | evidence |
|---|---|
| **A named command vocabulary exists**: 94 enumerators, plus an `int param` for six "family" actions | `ui/MenuModel.hpp:109` `enum class MenuAction : uint16_t` |
| **One dispatch entry point**, which both menu backends route through — "neither contains an action of its own, which is the property that makes the two menu bars incapable of disagreeing" | `ui/MenuModel.hpp:730` `performMenuAction()` |
| **A command queue already exists**, drained at the top of a frame | `ui/MenuModel.hpp:746`–`747` `enqueueMenuAction()` / `dequeueMenuAction()` |
| **A per-command precondition oracle already exists** — the replayer's "is this step legal here?" | `ui/MenuModel.hpp:774` `menuItemEnabled(action, param)` |
| **Layer gestures are already a walked table**, with the "a command not in this enum is a command exactly one menu can reach" discipline written down | `app/LayerEditor.hpp:157` `allLayerCommands()` → `:217` `applyLayerCommand(OpenDocument&, LayerCommand, size_t)`; `core/LayerSetOps.hpp:375` / `app/LayerEditor.hpp:261` for the multi-selection set |
| **Every menu-driven pixel op runs through one choke point** — 19 adjustments + 7 filters | `app/PixelOpBridge.hpp` `computePixelFilter()` / `applyPixelFilter()`; 30 call sites, all inside `app/AdjustmentOps.cpp` (21) and `app/FilterOps.cpp` (9), **none in the UI** |
| **Every public applier is already `(OpenDocument&, Params)`** and already headless | e.g. `app/FilterOps.hpp:177` `applyGaussianBlur(doc, sigma)`, `app/AdjustmentOps.hpp:172` `applyThresholdAdjustment(doc, params)` |
| **"Resize to 512×512" is already one call** | `app/FilterOps.hpp:277` `applyImageSize(doc, w, h, ResampleKernel)` |
| **Value mutations are already a table of setters through one recording funnel** | `core/LayerOps.hpp:272`–`508` (`setLayerBlend`, `setLayerOpacity`, `setLayerName`, …), all via `recordLayerEdit()` (`app/DocumentLifecycle.hpp:730`) |
| **Every layer op stack already serialises exactly**, float bit patterns included, with class/kind codes that are **explicit numbers, not enum ordinals** | `io/OpSerial.hpp:145` `"npops1:"`; nine kinds at `core/OpStack.hpp:85` |
| **A record this build cannot interpret is already preserved verbatim, in place** | `core/OpStack.hpp:79` `OpClass::Unknown` + `Op::unrecognised`; PRD I10 |
| **The batch loop is largely written**: apply state → composite → encode through an Export As preset → name template → collision detection → pre-flight refusal → per-file report → restore the caller's document | `io/ExportStates.hpp:478` `planStateExport()`, `:484` `exportDocumentStates()`; four outcomes (Written / Skipped / Failed / NotAttempted) with per-item warnings |
| **I15's presets are a field assignment, not an adapter** | `io/ExportAs.hpp:180` `ExportRequest`; `ExportStatesRequest::format` takes `preset.request` verbatim |
| **Opening any input is session-free and already reports warnings** | `app/OpenAnyFile.hpp:178` `openAnyFileAsDocument()` |
| **Replaying a grade needs no GPU** — layer op stacks evaluate on the CPU in the compositor | `core/Composite.cpp:123` `layerPointOps()` |
| **Headless CLI modes are established, and exit before the window exists** | `src/main.cpp:2154`–`2155` return before `SDL_Init` at `:2167` |
| **A user-editable JSON library in Application Support is established** | `io/ExportAs.cpp:879` `export-presets.json`, with a `problems()` list |
| **Export encodes the whole file to memory before opening the output** | `exportDocumentWithRequestToFile()`, `io/ExportAs.cpp` |

**And one thing that is true and unhelpful, recorded so nobody spends a day on it:**
`core::History` is **snapshot-based** — a `HistoryEntry` is a whole document state plus a
display `label` (`core/History.hpp:320`). It cannot be replayed as commands and is not
the recorder's tap. The undo stack tells you *that* something happened, never *what*.

## 4. What is missing

| gap | fix | est. |
|---|---|---|
| **No single "apply a command to a document" function.** The vocabulary exists; the appliers exist; nothing joins a stable command id to a params bag to an applier | `app/Command`: a `Command{id, params}`, a registry, `applyCommand(OpenDocument&, const Command&)` | 2 d |
| ~~**49 UI call sites call appliers directly**~~ **DONE (step 2).** 46 sites, counted against the tree: 29 pixel, 4 layer-command, 13 layer-setter — not 16; the other six `setLayerX()` calls outside `core/` and `app/` are main.cpp fixture builders, not user routes. 40 migrated, 6 named exceptions | done: each site calls `applyCommand()`, through `ui/MacPaintUI.hpp`'s three boundary functions. The appliers kept their signatures and their tests. See "step 2, as built" in §6 | done |
| **No recorder** | `app/Recorder`: armed / recording / stopped, appending to a `std::vector<Command>` from inside `applyCommand()` | 0.5 d |
| **No action model or file** | `app/Action` (model) + `io/ActionFile` (text form), split the way `core/OpStack` + `io/OpSerial` already are | 1.5 d |
| **`npops1:` is hex — it fails P5 outright** ("human-readable, and diffable") | the action file gets a **text** encoding; `np:ops` keeps hex, because an EXR header attribute must. One op list, two encodings — see the drift trap | folded above |
| **A JSON reader would be the third copy.** `io/ExportAs.cpp:55` says it in as many words: "a *third* consumer is when this becomes a shared header rather than a judgement call". The other two are there and at `app/Keymap.cpp:27` | extract one `io/Json`; port both existing callers in the same change | 0.5 d |
| **Nothing resolves a step against a *different* document** | resolve layers by **name and kind**, never by index; refuse, by name, what cannot be resolved | 1 d |
| **Class B / C / D op-stack entries have no implementation** — tags that exist so `detectRuns()` is written against `== PointA` (`core/OpStack.hpp:51`) | out of scope; an action carrying one must **refuse or report**, never silently skip | — |
| **The batch runner has no input set and no input/output collision rule** | `app/Batch`: the `exportDocumentStates()` loop with input paths as its source, plus the P4 pre-flight | 1.5 d |
| **No headless entry point** | `--batch <action> <output-dir> <files…>`, beside the two report modes | 0.5 d |
| **No UI** | an ACTIONS panel (record / stop / play / step list, rows editable and deletable) and a BATCH dialog (action, source set, output directory, preset, name template, dry-run plan, report) | 3 d |
| **Lens correction, pattern define/fill** (PRD D22, D27, both P2) | severable from all of the above | 2–3 d |

---

## 5. The action file

**`.npaction`, JSON, one file per action, in
`~/Library/Application Support/naturalPaint/actions/`.** JSON because
`export-presets.json` established the pattern, the reader is written twice already, and
P5 asks only for readable and diffable.

```
{ "npaction": 1,
  "name": "Height prep 512",
  "steps": [
    { "cmd": "select_layer",   "layer": "Base" },
    { "cmd": "flatten_image" },
    { "cmd": "select_layer",   "layer": "Flattened" },
    { "cmd": "filter_gaussian_blur",  "sigma": 4.0 },
    { "cmd": "set_layer_blend",       "mode": "subtract" },
    { "cmd": "adjust_threshold",      "threshold": 0.5, "amount": 1.0 },
    { "cmd": "image_size", "width": 512, "height": 512, "kernel": "Catmull-Rom" }
  ] }
```

> **Three things in this example were wrong when it was written, and each was
> caught by a track building against it rather than by re-reading it.**
> There was no `select_layer` after the `flatten_image`, which contradicts this
> section's own next rule — the flatten moves the active layer, so the pin is
> exactly what the rule demands. The survivor is named `"Flattened"`
> (`core/Merge.cpp`'s `flattenDocument()`), not `"Background"`; PRD C16 removed
> the privileged Background layer entirely. And `"catmull_rom"` is not a kernel
> name any build accepts — `resampleKernelName()` produces `"Catmull-Rom"` and
> `resampleKernelFromName()` folds case but not punctuation, so that exact line
> would have been refused. An example nobody executes is documentation of what
> someone believed, which is why the fix is recorded rather than quietly
> applied.

Four rules, each of which is a silent wrong answer if left implicit:

- **A step is keyed by a stable `cmd` id — never by a menu label, never by an enum
  ordinal.** `layerCommandLabel()` returns menu text ("New Pigment Layer"); menu text is
  UI copy and will be reworded. `io/OpSerial` already refuses to key by an ordinal, for
  the reason `core/OpStack.hpp:85` gives: the enum is appended to. The id is also what
  makes the file diffable, which is P5.
- **Targeting is explicit and by name.** The example's first step is not decoration: after
  `flatten_image` the active layer is a *different* layer, so "blur the active layer" is
  only deterministic if the recording says which layer is active. The recorder therefore
  **emits a `select_layer` step whenever the active layer changes**, and replay refuses,
  by name, a document that has no such layer.
- **Op-stack edits are one command kind, not a special case.** `set_layer_op` /
  `add_layer_op` carry an op in the text encoding of `io/OpSerial`'s record. PRD P6 —
  "recording an action is optional; any document's existing op stack is already one" —
  is then a *converter*: read `Layer::ops`, emit one step per entry.
- **Resolution-dependent parameters carry their unit.** None of the nine point-op kinds
  has one today, but `filter_gaussian_blur`'s σ does, and a σ authored on a 2k plate is
  wrong on an 8k one. Store the unit; a batch across mixed resolutions refuses if the
  unit is pixels and the sizes differ.

**What v1 does not record:** brush strokes. Class C ("recorded stroke") is a tag with no
implementation anywhere in the tree, and stroke replay is its own phase, not a line in
this one.

---

## 6. Steps

**0 — Extract the JSON reader (0.5 d).** `io/Json`: the reader and `escapeJson()`, with
`io/ExportAs.cpp` and `app/Keymap.cpp` ported in the same commit. First, because doing it
after `io/ActionFile` exists means four copies for as long as it takes to notice.
*Gate:* `--selftest` additions-only; export presets and keymaps still load byte-for-byte.

**1 — `app/Command` + the registry (2 d).** `Command{std::string id; Params params;}` and
`applyCommand(OpenDocument&, const Command&) -> CommandResult`, in the shape
`FilterOpResult` / `OpenAnyResult` already use (`ok`, a status sentence, warnings). One
registration per recordable command, each naming its id, its params, its applier and its
precondition.
*Gate:* **the exhaustiveness test**, in the discipline `allLayerCommands()` already
establishes — every `MenuAction`, `LayerCommand`, `LayerSetCommand` and public
`applyX(OpenDocument&, …)` is either registered or listed in an explicit exception table
with a stated reason, and the table starts empty except for the `AppState`-only actions
of §1. A command added later without a registration fails this test rather than being
silently unrecordable.

**2 — Migrate the call sites (2 d). DONE.** Each UI site calls `applyCommand()`
instead of the applier. No behaviour changes and no applier signature changes; this is
what puts every route — menu, panel, dialog, key binding — through one door.
*Gate:* zero diff in `--selftest` beyond additions; the Filter and Adjustments golden
views unchanged.

> **Step 2, as built.** Five things this section got wrong or left implicit, each
> found by doing it rather than by re-reading it.
>
> **The count is 46, not 49.** 29 pixel sites and 4 layer-command sites are exact.
> There are **13** layer-setter sites, all in `ui/MacPaintUI.cpp`; the other six
> `setLayerX()` calls outside `core/` and `app/` are `buildDemoDocument()` and
> `--comps-demo` writing a document state before a screenshot. They address layers
> by index and are not user routes.
>
> **Six sites cannot be migrated, and they are one problem.** The LAYERS panel's
> eye, padlock and inline rename act on **any row**, and clicking the eye
> deliberately does not select it. `applyCommand()` addresses a layer by NAME —
> that is the property that makes an action replayable — and layer names are
> explicitly not unique, so a name taken from a row would resolve to the *first*
> layer sharing it, which is a document a Duplicate Layer produces.
> `runLayerSetCommand()` has that problem plus two of its own:
> `resolveLayerSet()` refuses a name list that does not resolve one-to-one (right
> for a file, fatal for a button), and `CommandResult` cannot carry
> `LayerSetEditResult::selection`, which the panel assigns. `--ui-merge-demo` and
> `--ui-multiselect-demo` address layers by index on purpose and are not user
> actions. **What closes all six is a stable `Layer::id`**, which is 0 on every
> layer this build creates (`app/StrokeSession.hpp` §5) — a separate piece of
> work, and the one thing that would let a command name a row unambiguously.
>
> **One behaviour DID change, knowingly.** The command layer refuses a parameter
> at its documented identity — sigma 0, strength 0, amount 0, density 0 — where
> the applier would have run and changed nothing, because §7 makes that the
> command layer's rule and refusing in a batch but not in the UI would be the two
> paths differing. In the dialog it shows as an explanation where the old code
> closed the popup and said nothing legible: its "Nothing changed" line was drawn
> for exactly one frame, because `CloseCurrentPopup()` takes effect at
> `EndPopup()`. Pinned by an assertion, so changing it back is a decision somebody
> makes on purpose.
>
> **There are no Filter or Adjustments golden views to keep unchanged.** The gate
> above names views that do not exist: `run_golden.sh`'s 58 views include no
> launch flag that opens a Filter or an Image > Adjustments modal, and the three
> chorded adjustments are keymap actions, which the harness does not press. The
> reroute could not move any of the 58 anyway — every line it changed is inside a
> button's `if` body or a lambda only a click reaches, and no widget, string or
> layout moved.
>
> **The migration needed a direction the command layer did not have**: an
> *encoder*, turning the params struct a dialog holds into the JSON its own
> adapter reads. `app/CommandsImage.hpp` and `app/CommandsLayers.hpp` publish
> thirty-seven of them, beside the readers they feed, for the reason
> `app/CommandsOpStack.hpp` already gives about the hex/text pair.

**3 — `app/Recorder` (0.5 d).** A session sink `applyCommand()` appends to while armed.
*Gate:* recording *"flatten, blur, set blend, threshold"* by driving `applyCommand()`
directly produces exactly five steps, `select_layer` included, in order.

**4 — `app/Action` + `io/ActionFile` (1.5 d).** Model, text form, round trip. Follow
`io/OpSerial`'s two rules exactly: a version in the prefix so a reader decides before it
decodes, and stable names so appending to an enum cannot move the format under an
existing file.
*Gate:* a **hand-typed** `.npaction` fixture — written at a keyboard, not produced by the
encoder — decodes to the expected steps; and an encode/decode round trip is exact,
float bit patterns included.

**5 — `replayAction()` (1 d).** Resolve targets by name, consult each step's precondition,
apply, collect warnings; the whole replay is **one history entry**, so a user can undo an
action in one stroke.
*Gate:* an action recorded on document A and replayed on document B, whose layers differ
in order and count, produces exactly the pixels A produced — and one referencing a layer
B lacks refuses B by name without touching a pixel.

**6 — `app/Batch` + `--batch` (2 d).** The `exportDocumentStates()` loop with input paths
as its source: `openAnyFileAsDocument()` → `replayAction()` → the existing encode and
write. `planStateExport()`'s dry-run half is what the UI shows before the user commits.
Per-item warnings carry **import** warnings too — a PSD that imports lossily is a
per-file result, not a silent success. `--batch` sits beside the report modes at
`src/main.cpp:2154`, before `SDL_Init`, which is what makes it testable in `--selftest`
and usable from a shell.
*Gate:* P4 in full — the run stops at the first `Failed`, every later item reports
`NotAttempted`, and every input's bytes are unchanged, asserted by hashing before and
after.

**7 — ACTIONS panel and BATCH dialog (3 d).** Record / stop / play, a step list whose
rows can be reordered and deleted, and the batch dialog reusing `app/ExportDialog`'s
preset control rather than growing a second one.
*Gate:* golden views for the panel recording and idle, and for the dialog's plan and
report states.

**8 — Lens correction, pattern define/fill (2–3 d, severable).** PLAN.md's parked P2 image
ops. Nothing above depends on them.

**Total: ~13.5 days for P1–P6, plus 2–3 for step 8.** Steps 0–5 deliver record-and-replay
with no batch at all, and are worth shipping alone.

---

## 7. Traps

- **`fopen(path, "wb")` truncates before the first byte is written.** Export encodes to
  memory in full first, so a failed *encode* writes nothing — but an output path that
  collides with an input destroys that input the moment the file is opened, encode
  success or not. **P4 cannot be delegated to the export path.** `app/Batch` refuses the
  whole run in pre-flight when any output resolves to any input — after canonicalising
  both, since `./a.exr` and `a.exr` are one file and string equality disagrees. This one
  check is the entire argument against a user-wired save node.
- **The active layer is the state most likely to make a replay wrong and green.** Every
  pixel op targets `activeLayerOf(doc)`. Record the selection, or a replay whose target
  document merely has a different top layer blurs the wrong thing and reports success.
- **A selection is session state and is never in a file** (`app/DocumentLifecycle.hpp:232`
  — it is deliberately outside `Document` so undo does not restore marquees). Every
  destructive op is selection-bounded, so an action that does not record the selection
  silently applies to the whole canvas. Selection-changing commands must be recordable
  steps, and a step recorded while an ad-hoc marquee was live must either refuse at
  **record** time or reference a **saved alpha channel** by name — `Document::channels`
  is document data and does round-trip. Recording marquee coordinates is the wrong
  answer; they are meaningless at another resolution.
- **A silent no-op is the failure mode this feature is built to have.**
  `applyImageSize()` states its own rule — "a no-op the user asked for is not an edit" —
  and records no history; `pixelOpRefusalFor()` refuses a filter on a non-RGB layer. In
  the UI those are a greyed item and a nothing-happens. In a batch they are thirty files
  written *unmodified* and reported as successes. Replay consults `menuItemEnabled()` and
  turns "would be disabled" into a named refusal, and a step whose applier reports zero
  texels changed is a reported warning, never a silent pass.
- **An `Unknown` op record — an action from a newer build — refuses the run.** This is
  deliberately *not* the document round-trip rule. `Op::unrecognised` exists so a document
  survives an older build; an action is *executed*, and executing a step you cannot
  evaluate writes a wrong file that looks fine. Preserve on re-save, refuse on run.
- **Two encoders of one op list will drift.** The hex `npops1:` form and the action's text
  form must come from one description of the kinds and their fields, or the tenth kind
  lands in one and not the other. A `stack → hex → stack → text → stack` round trip in
  `--selftest` is the cheap guard.
- **`performMenuAction()` takes `AppState&` and `applyCommand()` must not.** The moment a
  recordable command reaches for session state, it stops being replayable headlessly and
  the `--batch` path starts differing from the interactive one. The UI path calls the
  document path, never the reverse.
- **`--selftest` counts are read from a stream that interleaves stderr.** Separate the
  streams before believing a pass count moved.
- **Doc rot, found in passing:** `io/OpSerial.hpp:113` says a record is malformed if "the
  class code is 0 and the kind code is not 0..5". The implementation handles 0..8
  (`io/OpSerial.cpp:32`–`33`). The code is right, the comment is stale. **Worse than
  recorded here:** the params table in that same header also stops at `ChannelMixer` and
  never lists Invert, Posterize or Threshold, though the implementation handles all three.
  Fix both rather than trusting either.

---

## 8. Verify

PLAN.md's own sentence, unchanged: *grade one plate, save the stack as an action, run it
over thirty photographs, and confirm every output is correct and no input was modified.*

Plus the user's own two cases, as `--selftest` fixtures: *resize to 512×512* replayed
onto three documents of different sizes, and *flatten → blur → blend Subtract →
threshold* replayed onto a document whose layer names match and one whose names do not
(which must refuse, by name, having changed nothing).

Mechanically: `app/selftest/Command.cpp` (the exhaustiveness table, the registry round
trip), `app/selftest/Action.cpp` (hand-typed fixture, cross-document resolution
refusals), `app/selftest/Batch.cpp` (the thirty-file run, stop-clean-on-failure, the
input-hash assertion, the input/output collision refusal), and
`app/selftest/CommandCallsites.cpp` (step 2's own: that each UI boundary reaches
`applyCommand()` at all, driven with a `Recorder` armed, and that reaching it left the
pixels of all twenty-six pixel commands bit-identical to the applier the control used to
call). Golden gains the ACTIONS panel and BATCH dialog views.

> **What `CommandCallsites.cpp` cannot see, stated because a sabotage proved it.**
> Un-migrating ONE call site — putting the OPACITY meter back on
> `setLayerOpacity()` — leaves the whole suite green. The section asserts that the
> three boundary functions reach `applyCommand()`, not that each of the forty
> controls calls a boundary; the controls are inside ImGui frames nothing headless
> can drive. Nineteen of the forty are safe structurally (they share one tail,
> `drawAdjustmentButtons()` / `performImmediateAdjustment()`, and a site that
> skipped the tail would draw no button); the rest are covered only by review. A
> NEW site added around the door is the case to watch for.

**Sabotage, at gather:** break the input/output collision check; break the non-`PointA`
refusal; break the `select_layer` emission so the recorder omits it. All three are
assertions about a *refusal* or an *absence*, which is the shape that most often passes
for the wrong reason.

---

## 9. Implementation checklist

Ticked as each item lands **and is asserted**, not when the code is written. Branch:
`automation`, worktree `/Users/chrisharvey/naturalPaint-automation`, based on `4eb619d`.
Baseline on that base: **8619 pass, 0 FAIL**, `--selftest` exit 0.

### Step 0 — `io/Json` (the third-consumer extraction)

- [x] `src/io/Json.hpp` / `Json.cpp`, added to `src/CMakeLists.txt`
- [x] the pull reader moved from `io/ExportAs.cpp` — **merged**, not verbatim: it gained
      `parseStringArray()` from `app/Keymap.cpp`, `\b`/`\f`/`\r`/`\uXXXX` decoding, and it
      stores its error instead of printing (the keymap's stderr line moved to its caller)
- [x] `escapeJson()` moved with it, **widened** to escape control characters — the old
      copy wrote them raw and produced a file that would not read back
- [x] a small `JsonValue` DOM on top — null / bool / number / string / array / object
- [x] `io/ExportAs.cpp` uses it; its private copy deleted
- [x] `app/Keymap.cpp` uses it; its private copy deleted
- [x] the "deliberately a second copy" comments at both sites replaced by the real reason
- [x] `app/selftest/Json.cpp`: numbers, escapes, nesting, depth limit, error labels, DOM round trip
- [x] **gate:** `export-presets.json` and `keymaps/default.json` still load; `--selftest`
      additions-only, **8650 pass / 0 FAIL** (was 8619; +31)
- [x] sabotage: narrowing `escapeJson` and dropping the 17-digit fallback each go red

### Step 1 — `app/Command` and the registry

- [x] `Command{std::string id; JsonValue params;}` and `CommandResult{ok, status, warnings}`
- [x] `applyCommand(OpenDocument&, const Command&)`
- [x] registry: id → `{apply, precondition, paramNames}`; lookup by id, never by ordinal
- [x] the plan's composite case runs end to end through `applyCommand()` alone —
      `flatten_image` → `filter_gaussian_blur` → `set_layer_blend` → `adjust_threshold`
- [x] `select_layer`, `new_rgb_layer`, `image_size` (6 of ~50 rows registered)
- [x] sabotage: skipping an unknown id, blanking the precondition hook, and dropping the
      `LayerEditResult::selected` adoption each go red — **two of these did not, at first**
- [x] registrations — filters (7): blur, sharpen, unsharp, noise, emboss, median, motion blur
- [x] registrations — adjustments (15) and the four auto solvers, which is 19 in total,
      not 19 plus 4 — the plan double-counted them
- [x] registrations — document: `canvas_size`, `crop_to_selection`, `trim_to_content`
- [x] registrations — `LayerCommand` (all 23, walked from `allLayerCommands()`)
- [x] registrations — `LayerSetCommand` (all 36, walked from `allLayerSetCommands()`)
- [x] registrations — `core/LayerOps` setters (10)
- [x] registrations — op-stack edits, carrying an op keyed by kind name
- [x] registrations — selection: select all, deselect, invert, save/load channel
- [x] **the exhaustiveness test** — `app/CommandCoverage`, an exhaustive `switch` so a new
      `MenuAction` fails the BUILD, with three answers rather than two: 35 registered,
      51 not recordable (each with its reason), 8 known gaps (count asserted so the list
      can shrink but not grow silently)
- [x] **gate:** every id round-trips; unknown id refuses by name
- [x] sabotage: a claimed-but-unregistered id goes red, and adding a `MenuAction`
      enumerator fails the build by name in `CommandCoverage.cpp`

### Step 2 — migrate the call sites (46, not 49 — see §6's "step 2, as built")

- [x] 29 sites across the 28 public `FilterOps`/`AdjustmentOps` appliers — all migrated
- [x] 4 `applyLayerCommand` / `applyLayerSetCommand` sites — 1 migrated,
      3 named exceptions (`runLayerSetCommand()`, and both demo drivers)
- [x] ~~16~~ **13** `core/LayerOps` setter sites — 10 migrated, 3 named exceptions
      (the eye, the padlock, the inline rename: they address a ROW, and a row has no
      unambiguous name)
- [x] the encoders the migration needed: `app/CommandsImage.hpp` (30) and
      `app/CommandsLayers.hpp` (7), beside the readers they feed
- [x] **gate:** `--selftest` additions-only, 9095 → 9121, 0 FAIL, exit 0
- [x] **gate:** golden — argued, not run. **There are no Filter or Adjustments views
      to move**, and no launch flag reaches a migrated line in any of the 58

### Step 3 — `app/Recorder`

- [x] arm / record / stop, appending from inside `applyCommand()`
- [x] emits `select_layer` whenever the active layer changes
- [x] refuses to record a selection-bounded step taken under an unnamed marquee (§7)
- [x] **gate:** the user's own case records as exactly five steps, in order

### Step 4 — `app/Action` + `io/ActionFile`

- [x] `Action{name, steps}`; `.npaction` JSON with an `"npaction": 1` version
- [x] writer, reader, and the library directory under Application Support
- [x] `actionFromLayerOps()` — PRD P6's converter, an op stack to steps
- [x] **gate:** a **hand-typed** fixture decodes correctly; round trip exact incl. float bits
- [x] **gate:** `stack → hex → stack → text → stack` agrees (the two-encoder drift guard)

### Step 5 — `replayAction()`

- [ ] resolve layers by name and kind; refuse by name, having changed nothing
- [ ] consult each step's precondition (`menuItemEnabled`-equivalent) before applying
- [ ] a step that changes zero texels is a reported warning, never a silent pass
- [ ] a non-`PointA` or `Unknown` op-stack step refuses the run
- [ ] the whole replay is one history entry
- [ ] **gate:** A-recorded → B-replayed produces A's pixels; a missing layer refuses

### Step 6 — `app/Batch` and `--batch`

- [ ] `BatchRequest`: action, source set, output directory, `ExportRequest`, name template
- [ ] the pre-flight: **every output path canonicalised against every input path**
- [ ] the loop: `openAnyFileAsDocument()` → `replayAction()` → existing encode and write
- [ ] import warnings carried into the per-file report
- [ ] stop at first `Failed`; the rest report `NotAttempted`
- [ ] `--batch` beside the report modes, before `SDL_Init`
- [ ] **gate:** 30-file run in `--selftest`; every input's bytes unchanged (hashed both sides)

### Step 7 — UI

- [ ] ACTIONS panel: record / stop / play, step list, reorder, delete
- [ ] BATCH dialog: action, sources, output dir, preset, template, dry run, report
- [ ] golden views: panel idle, panel recording, dialog plan, dialog report

### Step 8 — the parked P2 image ops (severable)

- [ ] lens correction (PRD D22)
- [ ] pattern define / fill (PRD D27)

### The scatter wave — dispatched 2026-09-09, all six based on `8ab193d`

One file per track, which is why the table was split into families first: six branches
appending to one `std::vector<CommandSpec>` literal is one shared conflict, and
`run_golden.sh`'s nine parallel view arrays already sprang that trap once.

| branch | worktree | owns | step |
|---|---|---|---|
| `scatter/image` | `np-image` | `app/CommandsImage.cpp` — 7 filters, 19 adjustments + 4 auto, 4 document-geometry | 1 |
| `scatter/layers` | `np-layers` | `app/CommandsLayers.cpp` — `LayerCommand`, `LayerSetCommand`, the value setters | 1 |
| `scatter/opstack` | `np-opstack` | `app/CommandsOpStack.cpp` — op-stack rows and the selection rows | 1 |
| `scatter/recorder` | `np-recorder` | `app/Recorder` | 3 |
| `scatter/actionfile` | `np-actionfile` | `app/Action`, `io/ActionFile` | 4 |
| `scatter/patterns` | `np-patterns` | lens correction, pattern define/fill | 8 |

Steps 2 (migrate the 49 call sites), 5 (replay), 6 (batch) and 7 (UI) are the sequential
chain and are not scattered — each needs the one before it.

**Gather-time checks, none of which a track can do for itself:**

- [x] every declared `run*Test()` is called **and** its term is in the aggregation — the
      parallel-array trap. Six branches appended to one boolean expression and git
      auto-merged one of those edits **without a conflict**; the check confirmed the term
      survived, which is the case that would otherwise have gone unnoticed
- [x] `grep -rn SABOTAGE-TEMP src/` is empty (plain `SABOTAGE` is not a usable marker:
      several permanent "SABOTAGE PROOF" sections already contain it)
- [x] the exhaustiveness test itself, over every vocabulary at once — `app/CommandCoverage`
- [x] **a cross-track tripwire fired on merge, exactly as its author designed.**
      `app/selftest/ActionFile.cpp` asserted, while `add_layer_op` was unregistered, that a
      converted action is refused at load — and flipped, the moment that row merged in, to
      demanding the converter carry every parameter the row advertises. It went red on the
      merge and stayed red until `actionFromLayerOps()` was finished against the real codec
- [x] re-run the load-bearing sabotages against the **merged** production line
- [x] **a stale object nearly cost a false regression.** Merging `main` (one commit, a
      CoreText font change) turned five menu-model assertions red, including a `constexpr`
      count that cannot depend on a font. The cause was a *failed* sabotage build leaving
      an object compiled against a header state that no longer existed, which the next
      successful incremental build did not refresh. **After any build that fails, touch the
      reverted file before believing the next result** — otherwise the blame lands on
      whatever was merged next

**Merged result: 8971 pass / 0 FAIL after the six merges, 8976 with the gather work and
`main` folded in.** The wave added
287 assertions to a 8684 baseline.

### Sabotage, at gather

- [ ] break the input/output collision check → `Batch` test goes red
- [ ] break the non-`PointA` refusal → `Action` test goes red
- [ ] break the `select_layer` emission → `Recorder` test goes red

---

## 10. Record

Filled in as steps complete. Empty until step 0 runs.

| date | step | finding |
|---|---|---|
| 2026-09-09 | 2 | The count was 49 and is 46: the layer-setter figure was 16, and thirteen of those are UI sites — the other six are main.cpp fixture builders. |
| 2026-09-09 | 2 | **Six sites cannot be migrated at all, and they are one problem**: a layer name is not unique, so a control that acts on a ROW has no target a command can name. `Layer::id` is what would close all six, and it is 0 on every layer this build creates. |
| 2026-09-09 | 2 | The step's own gate names golden views that do not exist. There is no launch flag that opens a Filter or an Adjustments modal, so the harness has never photographed one. |
| 2026-09-09 | 2 | Un-migrating ONE call site leaves the suite green — an inert sabotage, recorded because it bounds what the new section proves. It asserts the three boundaries reach `applyCommand()`, not that each of the forty controls calls a boundary. |
