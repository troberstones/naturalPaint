# Automation: what a new feature owes the recorder

Phase 19 made this application's document edits recordable, replayable and
batchable. That is not a property the codebase has once; it is a property every
subsequent feature either keeps or quietly breaks. This document is for
whoever adds the next one.

It is deliberately not a summary of `docs/automation-plan.md`. That file is the
build plan — what was decided, in what order, and why — and it is written
backwards, from the finished thing. This one is written forwards, from your
change.

**The failure mode is the reason this document exists.** A feature that skips
every step below still works. The button does the thing, the pixels move, the
undo entry appears, `--selftest` stays green and the golden views stay green.
The only symptom is that a user who presses Record, does the thing, and presses
Stop gets an action file with the step missing — and then a batch of four
hundred files that is silently, uniformly wrong. Nothing in the build tells you
this happened, because nothing was broken; something was merely absent.

That has already happened once *after* Phase 19 was built. A branch converted
every parameter dialog onto `ui/Dialog` and, in doing so, replaced each
dialog's `Command` argument with a lambda that called the applier directly. It
merged clean in twenty places, ran correctly, and recorded nothing. See §7.

---

## 1. The one rule

> **A command is recordable if and only if it can be expressed as a function of
> an `OpenDocument` alone.**

`app/Command.hpp` §1 states it and argues it. It sorts every feature without
anyone arguing about any of them:

* Reads or writes the *document* — layers, pixels, masks, the op stack, the
  document's own extent: **recordable**. Register it.
* Reads or writes *session* state — the zoom, the active tool, the panel
  layout, the clipboard, `core::History`, a live on-canvas gesture:
  **not recordable**, and its absence is not a gap. Say so in one sentence and
  move on.

The rule is about the *signature*, not about difficulty. "Hard to encode" is
not on the list. If your applier takes an `OpenDocument&` and some parameters,
it is recordable, and the honest answer to "I haven't done it yet" is
`NotYetRegistered` (§4), never `NotRecordable`.

---

## 2. If it is recordable: four edits, and none of them is optional

### 2.1 A row in the table

Add a `CommandSpec` to whichever `register*Commands()` function your feature
belongs to — `app/CommandsImage.cpp`, `CommandsLayers.cpp`,
`CommandsOpStack.cpp`, `CommandsPatterns.cpp`. A new family gets a new file and
a new `register*` call in `app/Command.cpp`'s `table()`; they are separate
translation units on purpose, so that two branches adding two commands do not
conflict.

The row's six fields, and what each one is actually for:

| field | what goes wrong without it |
| --- | --- |
| `id` | stable, `lower_snake_case`, **written into files**. Renaming one breaks every action already saved. Never an enum ordinal — a newer build has more commands than the one that wrote the file. |
| `label` | the ACTIONS panel's step text. Free to reword; the id is not. |
| `paramNames` | every key your applier reads, in the order the encoder writes them. `--selftest` (CommandCallsites §C) asserts the encoder writes no key the row does not advertise. |
| `unavailableReason` | returns a *sentence* when the command cannot run now, empty when it can. This is what turns "the UI would have greyed this out" into a named refusal at replay time instead of a silent no-op. |
| `apply` | called only when the precondition returned empty. |
| `selectionBounded` | true when an **absent** selection silently means "the whole canvas" for this command. Narrower than "reads the selection" — see §3. |

### 2.2 An encoder, beside the reader

The applier reads a `JsonValue`; write the function that builds one in the same
header, next to it (`gaussianBlurCommand(sigma)`, `setLayerOpacityCommand(v)`,
`imageSizeCommand(w, h, kernel)`). Beside the reader, so that a parameter added
to one is visible from the other. A UI that builds a `Command{}` literal
inline is a second encoder that will drift.

### 2.3 The UI goes through a boundary, never through the applier

There are exactly three doors from `ui/MacPaintUI.cpp` into the command layer,
and they are at file scope — not in the anonymous namespace — precisely so
`--selftest` can call the real ones rather than a copy:

* `runPixelCommand(od, command, nothingChangedText)` — every Filter and
  Adjustments dialog. Its shared footer, `pixelOpFooter()`, takes a `Command`
  and nothing else; there is no callable overload, so a new dialog **cannot**
  reach its applier directly and still compile.
* `runLayerGesture(od, LayerCommand)` — the Layer menu and the LAYERS panel's
  buttons.
* `runActiveLayerSetter(od, command)` — the BLEND combo, the OPACITY meter,
  Layer Properties' controls.

All three call `applyCommand()`, which is where the recorder's single tap sits
(`RecorderTap`, straddling the applier). Both refusal paths return in front of
that tap, which is what makes "a refused command is not a step" a property of
where the tap sits rather than of a flag someone remembers to check.

If your feature fits none of the three, add a fourth boundary next to them —
at file scope, with its own `--selftest` case in
`app/selftest/CommandCallsites.cpp` §A. Do not call `applyCommand()` from
inside a widget: an anonymous-namespace call site is one `--selftest` cannot
reach, and "a test that tests a copy" has cost this project real defects twice.

### 2.4 A classification in `coverageFor()`

If your feature has a `MenuAction`, `app/CommandCoverage.cpp` will not compile
until you classify it — `coverageFor()` is an exhaustive `switch` and
`-Werror=switch` is on (src/CMakeLists.txt:827). Three answers:

* `Registered` with the `commandId` — asserted to resolve against the table.
* `NotRecordable` with a **reason naming the session state it needs**.
* `NotYetRegistered` with a reason saying what is missing. `--selftest`
  (Command §G) asserts the *exact count* of these. The list may shrink freely;
  it cannot grow without someone editing that number by hand, which is the
  review a gap of this kind never otherwise gets.

---

## 3. Parameter rules, all four of which exist because of a real defect

1. **Enums cross as names, never as ordinals.** `canvasAnchorName()`, blend
   mode wire names, resample kernel names. An ordinal moves the moment someone
   appends an enumerator, and the file that already exists then means something
   else. `io/OpSerial.hpp` argues this at length for the document format; it is
   the same argument twice over here, because an action is read by builds with
   more commands than the one that wrote it.
2. **Layers cross by name, never by index.** `layerIndexNamed()` is the only
   resolver. An action recorded on one document is replayed on another whose
   layers differ in order and count; an index silently addresses a different
   layer and reports success. A name that matches nothing is a named refusal.
   A *duplicated* name is reported, not resolved silently (Recorder §F).
3. **Session state never reaches a file.** A selection is session state. If an
   absent selection would silently mean "the whole canvas" for your command,
   set `selectionBounded` — the recorder then refuses to record that step under
   a live marquee no saved channel names, because the step would replay with
   nothing selected, cover everything, and report success. Nothing in the file
   would be *wrong*; the file would be missing the half of the state that made
   the step mean what it meant. `--selftest` Command §H walks the whole table
   and requires the flag on every row whose precondition is
   `pixelOpUnavailable`, so a filter registered next month gets the rule for
   free.
4. **A silent no-op is the failure this feature exists to have.** Prefer
   refusing a parameter that would be an identity (sigma 0, amount 0) over
   accepting it and reporting a success that moved nothing. Where a success
   legitimately moves nothing, `CommandResult::texelsChanged == 0` with
   `changesPixels` set is what the replayer turns into a warning and the batch
   report turns into a conspicuous `Written, UNCHANGED` row.

---

## 4. If it is *not* recordable

Say which session state it needs, in `coverageFor()`, in one sentence. "The
clipboard is process state shared with other applications." "Navigates
`core::History`, which is a stack of whole documents." "Begins an interactive
gesture with on-canvas handles; the session owns the live transform."

A reason that does not name a piece of session state is usually a
`NotYetRegistered` wearing the wrong label.

---

## 5. Verifying it

**`--selftest` wiring is three places**, and a section wired into two of them
compiles, runs nothing, and passes:

1. `src/CMakeLists.txt` — the source list.
2. `src/app/SelfTest.hpp` — the declaration.
3. `src/main.cpp` — the **call** *and* its term in the boolean aggregation. A
   call whose result is not `&&`-ed into the total is a section that cannot
   fail the run.

**Sabotage the production line, not the test.** Break the exact production line
your new assertion covers and confirm *that assertion* goes red. Commit first;
mark it `// SABOTAGE-TEMP` (plain `SABOTAGE` collides with the permanent
"SABOTAGE PROOF" comments already in the tree) and grep for `SABOTAGE-TEMP`
before you merge. Two traps this project has actually hit: an assertion that
located its subject with the same helper it was testing, so code and oracle
moved together; and an assertion whose fixture was uniform, so the sabotaged
and unsabotaged paths both wrote nothing and compared equal.

**Golden views** (`tools/golden/run_golden.sh`) need a display and a GPU and
are *not* run by `--selftest`. A new view is nine parallel indexed arrays, all
of which must stay the same length — a two-branch append merges clean and
lands one element short, so check the lengths after any merge. If your feature
reads a directory under `$HOME`, add its environment override to the harness's
isolation block, or the view photographs the machine it ran on.

**Contention only ever adds failures.** Both `--selftest` (hardcoded `/tmp`
fixtures) and the golden harness (a competing app instance) are unsafe against
a second run. A failure that does not reproduce on a quiet machine was noise;
one clean run settles it. Never bless a golden reference from a contended run.

---

## 6. Running it without a window

    naturalPaint --batch <action.npaction> <output-dir> <file> [file...]
                 [--batch-format <fmt>] [--batch-template <tpl>]

Everything reachable this way is reachable because it is a function of an
`OpenDocument`. If your feature needs the GPU, the window or `AppState` to
produce its result, it is not a command — and a command whose batch behaviour
differed from its interactive behaviour would differ *silently*, which is why
`applyCommand()` takes an `OpenDocument&` and knows about nothing above it.

---

## 7. What is *not* enforced, stated plainly

The chain above is strong where it applies and has two holes. Knowing where
they are is the point of writing them down.

* **A panel button with no `MenuAction` escapes `coverageFor()` entirely.**
  `-Werror=switch` fires on the enum; a control that is not in a menu is not in
  the enum, and nothing will tell you it is unrecordable. `runFlatsExpand()`
  (ui/MacPaintUI.cpp) is the live example: it is a FLATS panel button, it calls
  `applyFlatsExpand()` directly, it is a document edit by §1's rule, it has no
  command row — and no assertion anywhere mentions it. If your feature is
  reached from a panel rather than a menu, this section is the only thing
  standing between it and the same fate.
* **`--selftest` proves the three boundaries reach `applyCommand()`. It cannot
  prove a dialog calls a boundary.** `app/selftest/CommandCallsites.cpp` §A
  calls `runPixelCommand()` and checks a step was recorded; it has no way to
  assert that `drawGaussianBlurDialog()` is what called it. This is exactly the
  hole the `ui/Dialog` conversion fell through: every assertion in this
  paragraph stayed green while twenty dialogs stopped recording. The structural
  guard now in place is that `pixelOpFooter()` accepts only a `Command` — keep
  it that way, and prefer widening a shared footer over letting a new dialog
  hand-roll its own commit path.

When you touch either of these areas, the reviewable question is not "did the
suite pass". It is: **record it, stop, and read the file.** If the step is not
in the `.npaction`, nothing else you check will tell you.

---

## 8. Currently open

Kept here rather than in the plan because these are what the next contributor
walks into, not what a finished phase decided:

* The five `NotYetRegistered` document edits this section used to name --
  `NumericTransform`, `DeleteSelection`, `Inpaint`, `RemoveLightingGradient`
  and `Offset` -- are now all `Registered` (`numeric_transform`,
  `delete_selection`, `filter_inpaint`, `filter_remove_lighting_gradient`,
  `filter_offset`). `DeleteSelection` and `Inpaint` both turned out to need the
  recorder's existing channel-match rule (§3's `selectionBounded`), not a
  second mechanism for "how a step names the selection it acted through".
  `NumericTransform` is registered for the whole-active-layer case only; a
  live selection still goes through `app/TransformSession`'s interactive path,
  untouched. The count in `app/selftest/Command.cpp` §G is 0.
* Six layer call sites that cannot be migrated until `Layer::id` is handed out.
  A layer name is not unique, so a control acting on a panel *row* has no target
  a command can name. This is smaller than it sounds: the identity machinery
  exists in full — `Layer::id` is a `uint64_t`, `Document::nextLayerId` is its
  counter, `core::normalizeLayerIds()` assigns and de-duplicates, and ids
  already round-trip through `np:comps`. They are handed out lazily, and
  `captureLayerComp()` is the only caller. Closing the six is "call the existing
  normalizer earlier, and let a command address a layer by id as well as by
  name", not "invent stable layer identity".
* `applyFlatsExpand()` has no command row, and no `MenuAction` to force the
  question — see §7.
* `io/ExportStates.cpp` still writes its bytes through an inline eight-line
  `fwrite`. Its own comment says "this is the second consumer, and a third is
  when the writer should be hoisted"; the third consumer arrived
  (`app/Batch.cpp`), the writer *was* hoisted
  (`writeEncodedExportToFile()`, io/ExportAs.hpp), and this copy was not
  converted with it.
