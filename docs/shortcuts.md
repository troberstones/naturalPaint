# Keyboard shortcuts

The default keymap. macOS notation throughout (`⌘` command, `⌥` option, `⌃` control,
`⇧` shift).

## Principles

1. **Match Photoshop exactly wherever Photoshop has an assignment.** Muscle memory is the
   single largest switching cost for the target user, and every gratuitous difference is
   friction charged on the first day. Where this keymap deviates, §6 says so and why.
2. **Unmodified letters are tools.** Chords are commands. A tool change happens hundreds of
   times an hour and must never need a modifier.
3. **`⇧`+letter cycles within a tool group** — Photoshop's own convention, and the reason
   26 letters can cover more than 26 tools.
4. **Layer-kind-scoped overrides** rather than new global letters, when a tool only makes
   sense on one kind of layer.
5. **Everything is remappable**, because a keymap is an opinion and some of these are
   arbitrary.

---

## 1. Tools

Unmodified single keys. Every one of these matches Photoshop except where §6 notes.

| key | tool | `⇧`+key |
|---|---|---|
| `V` | Move | `⇧V` select edits (Flats) |
| `M` | Marquee — rectangle | ellipse |
| `L` | Lasso — freehand | polygon lasso |
| `W` | Magic wand | |
| `C` | Crop | |
| `I` | Eyedropper | |
| `J` | Heal | |
| `S` | Clone | |
| `B` | Brush | `⇧B` draw bridge (Flats) |
| `E` | Eraser | `⇧E` Blot (Media layers) |
| `N` | Smudge | |
| `G` | Gradient | paint bucket |
| `O` | Dodge | burn |
| `P` | Pen | curve / convert anchor |
| `A` | Path select | |
| `T` | Text | |
| `H` | Hand | |
| `R` | Rotate view | `⇧R` reset rotation |
| `Z` | Zoom | |
| `Q` | Quick mask toggle | |
| `K` | Delete fill (Flats) | `⇧K` group lasso (Flats) |
| `U` | Merge fills (Flats) | `⇧U` draw-merge (Flats) |
| `Y` | Shape fill (Flats) | |

`X` swaps foreground and background; `D` resets them to black and white. `F` and `⇧F` are
the view mirrors — see §3.

### 1.1 Flats tools are scoped, not global

`K`, `U`, `Y`, `⇧K`, `⇧U`, `⇧B` and `⇧V` bind only while a **Flats layer** is active. Two
of the flatting tools need no key of their own at all, which is the argument for scoping
rather than inventing letters:

- **`B` on a Flats layer is the recolour brush.** It is a brush; it paints fills.
- **`G` on a Flats layer is the bucket.** It is the bucket; it carves a fill.

Gap review: `,` and `.` step to the previous and next suggestion, `Enter` accepts the
focused one, `⇧Enter` accepts all. On a Flats layer these override the global brush-preset
cycling, which has nothing to cycle there.

> The scoped set must be **visible** — the tool palette shows the flatting tools when a
> Flats layer is selected. A silent modal keymap is worse than an awkward global one.

---

## 2. Brush and colour

| key | action |
|---|---|
| `[` / `]` | size down / up |
| `⇧[` / `⇧]` | hardness down / up |
| `,` / `.` | previous / next brush preset |
| `1`–`0` | opacity 10 %–100 % |
| `⇧1`–`⇧0` | flow 10 %–100 % |
| `⌥` hold | temporary eyedropper |
| `⌃⌥` drag | size and hardness by dragging — see the ergonomic note |
| `X` | swap foreground / background |
| `D` | default colours |
| `Caps Lock` | precise crosshair cursor |
| `⇧` drag | constrain a stroke to a straight line |

> ⚠️ **`[` and `]` are unreachable while holding a pen in the right hand**, and they are the
> two most-used keys in painting. Photoshop has this problem and never fixed it. The
> `⌃⌥`-drag gesture is therefore **not a convenience, it is the primary path** for size and
> hardness: it happens under the pen, on the canvas, without the off-hand leaving its
> resting position. Left-hand reachability is a requirement for the whole painting set —
> `B`, `E`, `X`, `D`, `Q`, `Space`, `⌘Z` are all in the left half of the keyboard for this
> reason, and any future painting shortcut should be too.

---

## 3. View

| key | action |
|---|---|
| `Space` hold | pan |
| `⌘+` / `⌘−` | zoom in / out |
| `⌘0` | fit to window |
| `⌘1` | 100 % |
| `⌘⌥0` | zoom to selection |
| **`F`** | **mirror view left / right** |
| **`⇧F`** | **mirror view up / down** |
| `R` | rotate view (drag); `⇧R` resets |
| `⌘Y` | grayscale preview |
| `⌘R` | rulers |
| `⌘;` | guides |
| `⌘⇧;` | snapping |
| `⌘'` | grid |
| `Tab` | hide panels |
| `⇧Tab` | hide panels except the tool palette |
| `⌘⇧F` | cycle screen mode |

**`F` for flip is a deliberate deviation** — see §6.

**`⌘Y` for grayscale preview** inherits the slot where Photoshop keeps Proof Colours, which
is fitting: the grayscale value check is what soft proofing became for this audience
(PRD Q3).

> Every entry in this section is **view-only** and must never touch the document. The mirrors
> and the rotation compose into a single view matrix, and pen input maps back through its
> inverse — otherwise painting under a mirror lands in the wrong place.

---

## 4. Document, edit and layers

The standard set. Listed so the gaps are visible, not because any of it is surprising.

| key | action |
|---|---|
| `⌘N` / `⌘⇧N` | new document / new layer |
| `⌘O` | open |
| `⌘S` / `⌘⇧S` / `⌘⌥S` | save / save as / save a copy |
| `⌘⌥⇧W` | Export As |
| `F12` | revert |
| `⌘W` / `⌘Q` | close / quit |
| `⌘Z` / `⌘⇧Z` | undo / **redo** |
| `⌘X` / `⌘C` / `⌘V` | cut / copy / paste |
| `⌘⇧C` | copy merged |
| `⌘⇧V` | paste in place |
| `⌘A` / `⌘D` / `⌘⇧D` | select all / deselect / reselect |
| `⌘⇧I` | inverse selection |
| `⌥⌫` / `⌘⌫` | fill with foreground / background |
| `⌫` | clear |
| `⌘J` | duplicate layer, or selection to a new layer |
| `⌘G` / `⌘⇧G` | group / ungroup layers |
| `⌘⌥G` | clipping mask |
| `⌘E` / `⌘⇧E` / `⌘⌥⇧E` | merge down / merge visible / stamp visible |
| `⌘T` / `⌘⇧T` | free transform / transform again |
| `⌘L` / `⌘M` / `⌘U` | levels / curves / hue-saturation |
| `⌘I` | invert |
| `⌘F` / `⌘⌥F` | repeat last filter / with its dialog |
| `⌘K` | preferences |
| `⌘⌥⇧K` | keymap editor |
| `Esc` | cancel the current operation |
| `Enter` | commit the current operation |

---

## 5. Known collisions, and how they resolve

Writing the keymap down is what surfaced these. Two sets.

### 5.1 autoFlats conflicts with Photoshop on six keys

The absorbed application's own keymap was designed in isolation, and **every one of its
letter bindings collides with a Photoshop meaning**:

| autoFlats | its meaning | Photoshop's meaning | resolved to |
|---|---|---|---|
| `X` | delete fill | swap foreground/background | `K` |
| `R` | lasso group fills | rotate view | `⇧K` |
| `C` | recolour brush | crop | `B`, scoped |
| `D` | draw-merge | default colours | `⇧U` |
| `F` | shape fill | cycle screen mode | `Y` |
| `S` | marquee select edits | clone stamp | `⇧V` |
| `Tab` | cycle gap suggestions | hide panels | `,` / `.`, scoped |

`Space` for pan is the only binding that already agreed. **Photoshop's meaning wins in
every case** — the flatting tools are the newer, narrower feature, and a user who has
flatted for years in autoFlats is still a user who has used Photoshop for longer.

This is worth recording in the migration doc as a porting task, because it is easy to
transliterate the keymap along with the algorithm and inherit all seven conflicts.

### 5.2 `⌘H` cannot be used

Photoshop binds `⌘H` to *hide selection edges*. On macOS `⌘H` hides the application, and
Photoshop resolves this with a first-run prompt that has confused users for two decades.

**Not adopted.** `⌘H` hides the application, as every other macOS application does. Hiding
selection edges is unbound by default and is available in the keymap editor for anyone who
wants it.

---

## 6. Deliberate deviations from Photoshop

Three, each argued rather than incidental.

**`F` and `⇧F` mirror the view; screen mode moves to `⌘⇧F`.** Photoshop uses bare `F` to
cycle screen modes. But mirroring the canvas is something a painter does many times an
hour to catch drawing errors, and screen mode is something they set once a session. A
one-key operation should be the frequent one. The mnemonic is also better than anything
available for a chord.

**`N` is smudge.** Photoshop leaves `N` unassigned and gives smudge no shortcut at all,
which for a painting application is an omission rather than a convention worth honouring.

**`Y` is not the history brush.** Photoshop's `Y` drives painting from an earlier history
state, which needs the non-linear history [ADR-0005](adr/0005-fixed-timestep-and-dab-replay-for-undo.md)
declines. The key is free for us, so the Flats shape-fill tool takes it.

Everything else in §1–§4 that Photoshop assigns, this keymap assigns identically.

---

## 7. The keymap is data

A default keymap is a set of guesses about how someone works, and several of the choices
above are frankly arbitrary — `K`, `U` and `Y` for the flatting tools have no mnemonic at
all. So:

- The keymap is a **file**, loaded at startup, editable in-app and by hand.
- **Conflicts are detected and reported**, not silently resolved by load order. A binding
  that shadows another is an error the editor shows, including shadowing that only occurs
  in one layer-kind scope.
- The **default keymap is a named preset** the user can return to, and a "Photoshop" preset
  is exactly it — because that is what the default already is.
- The in-app list is **searchable by action and by key**, so "what does `⇧K` do" and "how do
  I merge fills" are both answerable.
