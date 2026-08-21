# Standing rules for agents implementing a PLAN.md step

Prepended to every step brief. Written 2026-08-21 after measuring steps 8 and 9
(256k tokens / 42 min, and 438k tokens / 50 min). The measurements below are the
reason each rule exists — none of them trades away rigour, because the
mechanical work turned out to be nearly free and the *reading and writing* is
where the cost is.

Measured on this machine:

| | |
|---|---|
| from-scratch build, one config | 66 s |
| incremental rebuild after editing `SelfTest.cpp` | 10 s |
| one `--selftest` run | 2.9 s |
| `--selftest` output | **1768 lines** |
| `SelfTest.cpp` | **15 700+ lines** |

So three clean builds of both configs plus ten test runs is about **7 minutes**.
Roughly **85% of a step is reading, thinking and writing** — that is what these
rules cut.

## 1. Never read the full `--selftest` output

It is 1768 lines, ~26k tokens. Reading it after every run is the single largest
cost in a step, and almost all of it is output that was already passing.

While iterating, run this and read only what it prints:

```
./build/src/naturalPaint --selftest 2>&1 | grep -E "FAIL| PASS$|^  <your-label-prefix>"
```

Read the full output **once**, at the end, and only to confirm additions only.

## 2. Iterate with incremental builds

`cmake --build <dir> -j` is 10 s; a from-scratch build is 66 s. Do **one** clean
build of each configuration at the very end. Do not `rm -rf` between iterations.

The reviewer rebuilds both configurations from scratch independently anyway —
that is the actual guarantee, so an agent repeating it is pure duplication.

## 3. Do not re-read what the brief already gives you

The brief carries the API surface and line anchors you need. Read a file in full
only when you are about to change its design, not to look up a signature.
`SelfTest.cpp` in particular is 15 700+ lines: navigate to your anchor, do not
survey it.

## 4. One home per rationale

A design decision gets written **once**, in the header of the module that owns
it. Then:

* `main.cpp`'s `ok`-chain comment: **two lines**, naming the step and what the
  test covers. Not the argument.
* `PLAN.md` Findings row: **three lines**, the result and the one surprise. Not
  the argument.
* The commit message: **written by the reviewer, not by you.**

Step 8 wrote its serial-vs-index argument four times. That is output tokens, the
expensive kind, spent on duplication.

## 4b. Mark every run-to-run-variable line `[measured]`

If a line you print can differ between two runs of the *same* binary — a
timing, a rate, an RSS figure, anything divided by a duration — it **must**
contain the literal token `[measured]`.

That token is what the reviewer's regression filter keys off. A varying line
without it shows up as a *changed* line in the additions-only diff, and the
reviewer has to stop, re-run the previous binary twice, prove the line noisy,
and extend the filter by hand. That has now happened twice in a row — step 9's
`marginal cost of the layer:` line and step 2's four document-texture timing
lines.

The corollary is just as important: **do not put `[measured]` on a line that is
deterministic.** It would exclude a real assertion from the diff, which is worse
than the noise it was meant to suppress.

## 5. What is NOT being cut

* **Assertion count and rigour.** Step 8's 53 checks are the value.
* **Running a rejected alternative beside the built one** and printing both.
  That pattern is what surfaced the eviction finding in step 7 and the clipping
  reading in step 9.
* **Empirically derived, measured, printed bounds.** Never guessed.
* **The reviewer's independent from-scratch rebuild.** It caught a contaminated
  baseline on 2026-08-20 and costs 2 minutes.

## 6. The house rules that keep being broken

1. `color/ core/ ops/ app/ io/ ui/ gfx/ paint/ sim/ brush/` are **directory
   groupings only, never C++ namespaces**. Everything is flat `namespace np`.
   There is no `np::core::`. Includes are `#include "core/History.hpp"`.
2. **PLAN.md §1.5: "An unexercised build option is not a seam."** No
   `#ifdef`'d-out tests. A `kOiioBuild` constant carries the configuration and
   each assertion states the correct answer for both.
3. **Wire every new test into `main.cpp`'s `ok` chain**, not merely define it. A
   test that was defined and never called has shipped on this project before.
4. Both configurations must build with **zero warnings naming a `src/` path**.
5. **Do not change or remove any existing `--selftest` output line.** The
   reviewer diffs against HEAD and expects additions only.
6. **Do not commit.** The reviewer reviews, corrects, and commits.

## 7. Report honestly

Say which claims you measured and which you reasoned. Say what you could not
make work. The reviewer re-reads every changed file, rebuilds from scratch and
re-runs the suite against an isolated-worktree baseline — a self-report that
does not survive that costs a full cycle.
