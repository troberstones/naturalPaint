# Salvaged worktree orphans — 2026-09-01

Five files that existed **only** in the working tree of a stale git worktree,
uncommitted, and nowhere on `main`. Preserved here before those worktrees were
removed, so that deleting ~4.4 GB of stale checkouts could not lose anything.

Nothing here is on a path to `main`. This branch is an archive.

## From `.claude/worktrees/epic-ellis-a0e7c5` (last touched 2026-08-26)

- `MacNativeFileDialog.hpp` / `MacNativeFileDialog.mm` (221 lines)
  **Superseded.** A hand-written Cocoa `NSOpenPanel`/`NSSavePanel` wrapper,
  written when adding one was believed to be "a platform-integration job".
  `main`'s `src/ui/FileDialog.hpp` now does the same thing through SDL3's own
  vendored `src/dialog/cocoa/SDL_cocoadialog.m`, which was already in the tree.
  Kept only so the decision is reversible.

- `ModalKeyboard.cpp` (428 lines)
  A `ui/MenuModel` sweep -- "everything a backend could pick, in tree order,
  with the state that decides whether picking it does anything".
  **Not verified as superseded.** `main` has `src/app/selftest/MenuModel.cpp`
  which covers similar ground, but nobody has compared the two assertion sets.
  If menu-model coverage is ever questioned, read this first.

## From `.claude/worktrees/naturalpaint-github-repo-0a8c17` (2026-08-22)

- `brush-system-design-brief.md` (394 lines)
- `layers-panel-design-brief.md` (431 lines)
  Design briefs that were never committed. The brush-editor panel designs live
  in a Claude Design project rather than in this repo, so these may be repo
  drafts of that work or may be unique. Not judged either way here.
