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
