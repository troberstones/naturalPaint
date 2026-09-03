# Scatter / gather

Two scripts for running several agent tracks in parallel against one base
commit, then merging them into a result that is *proven* rather than reported.

```bash
./tools/scatter/scatter.sh $(git rev-parse HEAD) svgpath svgcss filekind
# ... dispatch one agent per track ...
./tools/scatter/gather.sh svgpath svgcss filekind
```

`scatter.sh` builds one git worktree per track, pinned to an explicit SHA, with
the Mixbox submodule initialised, CMake configured against the primary
checkout's already-populated `_deps`, and a private log directory. `gather.sh`
merges the tracks one at a time, then rebuilds and reruns the whole `--selftest`
**here** before calling anything green.

## Partitioning: what makes two tracks non-conflicting

A track owns **whole files**. Two tracks may not edit the same file, with a
small, deliberate exception for the three append-only registration points:

| File | What every track appends |
|---|---|
| `src/CMakeLists.txt` | its source files |
| `src/app/SelfTest.hpp` | its `run…Test()` declaration |
| `src/main.cpp` | the call, and a term in the `ok` chain |

Those three *will* conflict when two tracks insert near the same line, and that
is fine — the hunks are one line each and `gather.sh` stops so they are resolved
by hand. **Resolve them by hand.** A union merge driver has already, in this
repository, silently eaten a `break;` and a `|| tonal` and left a dead
unconditional return behind; all three merged clean and compiled.

Anything that cannot be partitioned this way is not a scatter track. In
particular `src/ui/MacPaintUI.cpp` is ~14,900 lines with every tool's gesture
block inline in `drawUI()`; two tracks landing tools there is a hostile merge
surface, so UI work is serialised rather than scattered.

## What the briefs must carry

Each agent gets: the pinned SHA, its worktree path, its private log directory,
its **exclusive file list**, and an explicit prohibition on touching any file
outside it. Two further rules, both from incidents:

- **Do not ask an agent to prove a sabotage.** Sabotage verification happens at
  gather time, by the integrator, on the merged tree. An agent that is stopped
  mid-verification leaves a live sabotage in production source, and it does not
  announce itself.
- **Do not read a running track's working tree.** `git diff` against a worktree
  whose agent is still writing reviews a patch that no longer exists. Read its
  commits instead — which is what `gather.sh` does.

## Resolving the registration conflicts: keep-both is not enough

The three shared files conflict as predicted, and the resolution *looks* like
"keep both sides". It is not, and the first real gather proved it. Mechanically
keeping both sides of the `main.cpp` conflict produced:

```
gradientToolOk && pathRasterOk && svgPathOk   && vectorLayerOk &&
gradientToolOk && pathRasterOk && svgStyleOk  && vectorLayerOk &&
```

which compiles, and is even semantically harmless — the duplicated terms are
the same booleans. That is exactly why a union driver would have shipped it.
The correct resolution is **one** line carrying both new terms. The same pass
also dropped a blank line the header's own convention requires between
declaration blocks, and left the CMake source list out of alphabetical order.

Read every resolved hunk. There are only three of them.

## What gather proves, and what it does not

It proves: every track committed; no `SABOTAGE` marker in any track's commits
or in the merged tree; the merge was conflict-free or hand-resolved; a build
from here with no `error:` in the log; the full `--selftest` at 0 FAIL, run
against the binary this script just built.

It does not prove the new assertions are *meaningful*. That still takes
deliberately breaking each one and confirming it goes red — by hand, after
gather, in the integration worktree. Twice in this project a green assertion has
measured the wrong thing, once through two consecutive fixes of the same
assertion, and only sabotage caught it.

It happened again on the first gather, on an assertion written by the
integrator rather than by an agent. A new check claimed selector matching was
"bounded, not exponential", reported `0.000 ms`, and **passed with the bound
deliberately removed** — because the selector it used was rejected on its
rightmost compound and never entered the search at all. Reshaped so the
right-hand end matches and the failure is at the far left, the same check
reports `6072 ms` and FAILs when broken, against `0.031 ms` when whole.

Two rules fall out of that:

- **A suspiciously fast measurement is a suspect measurement.** `0.000 ms` was
  the tell, and it was easy to read as success.
- **Size a performance assertion to be slow rather than hung when broken.** A
  sabotage you cannot afford to run is a sabotage you will not run.

## Reporting: verify the numbers, do not relay them

Agent reports have been accurate about substance and unreliable about
arithmetic. On the first wave one track reported "2004 pass" for a suite whose
log showed 7081, and another described an exponential blowup as "could be slow
in a pathological case". Both had done the work correctly.

So: take the file list and the design decisions from the report, and take every
number from the logs yourself. `gather.sh` does this for the build and the
suite; the per-track claims are yours to check.
