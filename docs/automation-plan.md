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
| **49 UI call sites call appliers directly** — 29 across the 28 public `FilterOps`/`AdjustmentOps` appliers, 4 layer-command sites and 16 layer-setter sites — so a recorder tapping anything below them sees pixels, not intent | migrate each to `applyCommand()`. One-line changes, no new behaviour; the appliers keep their signatures and their tests | 2 d |
| **No recorder** | `app/Recorder`: armed / recording / stopped, appending to a `std::vector<Command>` from inside `applyCommand()` | 0.5 d |
| **No action model or file** | `ops/Action` (model) + `io/ActionFile` (text form), split the way `core/OpStack` + `io/OpSerial` already are | 1.5 d |
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
    { "cmd": "filter_gaussian_blur",  "sigma": 4.0 },
    { "cmd": "set_layer_blend",       "layer": "Background", "mode": "subtract" },
    { "cmd": "adjust_threshold",      "threshold": 0.5, "amount": 1.0 },
    { "cmd": "image_size", "width": 512, "height": 512, "kernel": "catmull_rom" }
  ] }
```

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

**2 — Migrate the call sites (2 d).** Each UI site calls `applyCommand()` instead of
the applier. No behaviour changes and no applier signature changes; this is what puts
every route — menu, panel, dialog, key binding — through one door.
*Gate:* zero diff in `--selftest` beyond additions; the Filter and Adjustments golden
views unchanged.

**3 — `app/Recorder` (0.5 d).** A session sink `applyCommand()` appends to while armed.
*Gate:* recording *"flatten, blur, set blend, threshold"* by driving `applyCommand()`
directly produces exactly five steps, `select_layer` included, in order.

**4 — `ops/Action` + `io/ActionFile` (1.5 d).** Model, text form, round trip. Follow
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
  (`io/OpSerial.cpp:32`–`33`). The code is right, the comment is stale; fix it in step 4
  rather than trusting it.

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
input-hash assertion, the input/output collision refusal). Golden gains the ACTIONS panel
and BATCH dialog views.

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

- [ ] `Command{std::string id; JsonValue params;}` and `CommandResult{ok, status, warnings}`
- [ ] `applyCommand(OpenDocument&, const Command&)`
- [ ] registry: id → `{apply, precondition, paramNames}`; lookup by id, never by ordinal
- [ ] registrations — filters (7): blur, sharpen, unsharp, noise, emboss, median, motion blur
- [ ] registrations — adjustments (19) and the four auto solvers
- [ ] registrations — document: `image_size`, `canvas_size`, `crop_to_selection`, `trim_to_content`
- [ ] registrations — `LayerCommand` (all of `allLayerCommands()`), incl. `flatten_image`
- [ ] registrations — `LayerSetCommand` (all of `allLayerSetCommands()`)
- [ ] registrations — `core/LayerOps` setters, incl. `set_layer_blend`
- [ ] registrations — op-stack edits, carrying an op in `io/OpSerial`'s text encoding
- [ ] registrations — selection: select all, deselect, invert, load channel as selection
- [ ] `select_layer` by name (the target-state command §5 requires)
- [ ] **the exhaustiveness test** + an exception table that starts empty but for the
      `AppState`-only actions of §1, each with a stated reason
- [ ] **gate:** every id round-trips through `JsonValue` and back; unknown id refuses by name

### Step 2 — migrate the call sites (49)

- [ ] 29 sites across the 28 public `FilterOps`/`AdjustmentOps` appliers
- [ ] 4 `applyLayerCommand` / `applyLayerSetCommand` sites
- [ ] 16 `core/LayerOps` setter sites
- [ ] **gate:** Filter and Adjustments golden views unchanged; `--selftest` additions-only

### Step 3 — `app/Recorder`

- [ ] arm / record / stop, appending from inside `applyCommand()`
- [ ] emits `select_layer` whenever the active layer changes
- [ ] refuses to record a selection-bounded step taken under an unnamed marquee (§7)
- [ ] **gate:** the user's own case records as exactly five steps, in order

### Step 4 — `ops/Action` + `io/ActionFile`

- [ ] `Action{name, steps}`; `.npaction` JSON with an `"npaction": 1` version
- [ ] writer, reader, and the library directory under Application Support
- [ ] `actionFromLayerOps()` — PRD P6's converter, an op stack to steps
- [ ] **gate:** a **hand-typed** fixture decodes correctly; round trip exact incl. float bits
- [ ] **gate:** `stack → hex → stack → text → stack` agrees (the two-encoder drift guard)

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

### Sabotage, at gather

- [ ] break the input/output collision check → `Batch` test goes red
- [ ] break the non-`PointA` refusal → `Action` test goes red
- [ ] break the `select_layer` emission → `Recorder` test goes red

---

## 10. Record

Filled in as steps complete. Empty until step 0 runs.

| date | step | finding |
|---|---|---|
