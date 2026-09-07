# Upstream Sync

How Open Shaders stays current with upstream `community-shaders/skyrim-community-shaders` without losing the fork-specific CI policy, branding, and feature setup.

## Mechanism

Upstream syncs land as **merge commits** on `dev`, never as rebases, and **always through a reviewed PR — never a direct push to `dev`**, even for a maintainer with bypass permissions on the `dev` ruleset. The scheduled `Maint: Sync upstream/dev` workflow runs a three-way merge of `upstream/dev` into a `sync/upstream-dev-<date>` branch and opens a PR against `dev`; it does not push to `dev` itself. Two pieces make the merge itself safe:

1. **`.gitattributes` with `merge=ours` entries** for every file the fork owns end-to-end (CI workflows, `.releaserc`, README). During the merge, git's `ours` driver keeps the fork's version of those paths verbatim — upstream's changes to them are discarded without surfacing as conflicts.
2. **The `ours` merge driver itself** must be defined locally. `merge=ours` in `.gitattributes` _references_ a driver but doesn't define one. The sync workflow defines it as a no-op (`git config merge.ours.driver true`). **Local contributors who run the merge by hand must run the same command once per clone** — see [Setup](#setup-once-per-clone) below.

## Why merge, not rebase

We previously used `git rebase upstream/dev`. It silently regressed fork-owned files on every sync. The mechanism: upstream cherry-picks one of our fork commits → we rebase later → git detects the duplicate via patch-id and skips our commit as "already applied" → an upstream follow-up that deletes or edits the same file then applies cleanly. End result: the fork loses content with zero merge conflicts and zero log noise. The rebase reports success.

A 3-way merge consults both sides at every path independently of patch-id. Our `merge=ours` driver fires for fork-owned paths; everything else gets a real 3-way merge. Either a clean result or a visible conflict — nothing silent.

See `.gitattributes` for the current fork-owned list. When a file should join or leave that list, update both the attributes and the comment block above the list explaining why.

## Setup (once per clone)

```bash
git config merge.ours.driver true
```

That's it. The `ours` driver is intentionally not built into git (for security — driver definitions can run arbitrary commands), so each clone declares it locally. The sync CI does this in its own setup step.

If you've already run an upstream merge without this config, git would have raised an "unknown merge driver 'ours'" warning and used the default 3-way merge for those files, potentially producing surprising conflicts in fork-owned paths. Re-run with the driver configured and the conflicts disappear.

## Running a sync manually

```bash
# Make sure local dev matches origin/dev before merging upstream.
# A stale local dev would produce a PR based on an out-of-date branch
# or have you re-resolving conflicts already resolved by a prior run.
git fetch origin dev upstream/dev
BRANCH="sync/upstream-dev-$(date +%Y%m%d)"
git switch -c "$BRANCH" origin/dev

git merge --no-ff --no-edit \
    -m "chore(sync): merge upstream/dev as of $(git rev-parse --short upstream/dev)" \
    upstream/dev
# resolve conflicts (only in non-fork-owned paths), then:
git add <resolved-files>
git commit --no-edit
git push origin "$BRANCH"
gh pr create --base dev \
    --title "chore(sync): merge upstream/dev as of $(git rev-parse --short upstream/dev)" \
    --body "Merges upstream community-shaders/skyrim-community-shaders dev as of $(git rev-parse --short upstream/dev)."
```

**Never push the merge straight to `dev`**, and never use an admin/PAT bypass of the `dev` ruleset to skip the PR — this applies even to a trivial, conflict-free sync. Merge the PR with **"Create a merge commit"** (never squash, never rebase-merge — either would break the ancestry a future sync's 3-way merge depends on; see [Commit structure](#commit-structure-for-a-conflicted-sync)).

The scheduled workflow opens this PR automatically every Monday 08:00 UTC. Manual dispatch via `gh workflow run "Maint: Sync upstream/dev"` is available for urgent syncs and accepts a `dry_run` flag (fetches and merges locally in the runner, but skips pushing the branch/opening the PR).

## Versioning and changelog interaction

The merge commit's message is `chore(sync): merge upstream/dev as of <sha>`. semantic-release sees it as a `chore` and doesn't release on the commit itself.

**However**, semantic-release's default DAG walk follows the merge into upstream's commit history. Upstream's `feat:` and `fix:` commits that came in via the merge are visible to the commit analyzer and **do** drive version bumps in our release stream. This is deliberate: the fork's version reflects everything actually shipped to users, including upstream fixes that arrived via merge.

When upstream cherry-picks one of our commits and that cherry-pick lands in our merge, both copies of the same logical change get walked. Version-wise this is harmless (one release can only bump once at the max severity). Changelog-wise it produces a duplicate entry. If this becomes annoying, the fix is a `writerOpts.transform` in `.releaserc` that dedupes by patch-id — not done preemptively.

## When the workflow halts

A real conflict (in a file _not_ on the fork-owned list) means upstream and the fork have both meaningfully changed the same code. Examples we'd expect:

-   Both forks bump the same feature INI version.
-   We add a method to a class upstream also modified.
-   We rename a function upstream also renamed.

The workflow `git merge --abort`s, posts the conflicted file list to the workflow summary, and exits non-zero. Resolution is manual: clone, run the same merge locally, resolve, push the branch, and open the PR (see [Running a sync manually](#running-a-sync-manually)).

### Conflict Resolution Guidelines

> **Open Shaders prerelease invariant:** every Alpha and Beta feature remains disabled by default, regardless of core status. Do not take upstream's core-only behavior in `src/Feature.h`, and keep the default-disabled profile in `tools/build-shader-cache.py` aligned with it.

1. **Resolve conflicts in favor of keeping VR** (this fork is the VR maintainer). Typical conflicts are just fork CI config or feature `.ini` versions — resolve `--ours`.
2. If upstream ships a VR removal, revert it and keep VR.
3. **Verify ancestry after landing:** `git merge-base --is-ancestor <upstream-sha> HEAD` must pass for each adopted upstream commit.
4. **`CSEditor` vs `SceneSelector`:** this fork split weather-editor UI/logic out of `CSEditor` into its own `Features/SceneSelector` class; upstream never made that split and still lands weather-lock/weather-editor changes directly in `CSEditor.cpp`/`.h`. Taking upstream's side of a `CSEditor` conflict wholesale (e.g. re-adding `WeatherDetailsWindowSettings`, `DrawSettings()`, `PostPostLoad()`) reintroduces state this fork already owns on `SceneSelector`, producing undefined-symbol compile errors. Redirect any new feature-owning override to `SceneSelector` instead; generic engine-level hooks (e.g. `EditorWindow::InstallWeatherLockHooks()`/`MaintainWeatherLock()`) can stay called from either side. Verify with `grep -rn "WeatherDetailsWindowSettings\|SceneSelector" src/Features/CSEditor.*` (expect no matches) plus a clean `BuildRelease.bat Dev-Fast` link.

5. **Incompatible-DLL blocklist:** upstream keeps this list inline in `src/XSEPlugin.cpp`; this fork moved it to `src/Compatibility.h` so each entry can carry a user-facing reason. An upstream `chore: block <mod>` commit conflicts on the removed array — translate the new entry into `Compatibility.h` rather than restoring the array, or the block silently stops applying. Verify with `grep -c "Data/SKSE/Plugins" src/Compatibility.h` against the upstream array length.
6. **A shader-touching sync needs a live render check, not just a diff audit.** A fork-loss/upstream-loss pass only verifies that each side's own content is textually still present after the merge -- it cannot catch a case where our preserved fork code and upstream's newly-merged code are each individually correct in their own original context but semantically disagree once combined (e.g. upstream's new C++ constant-buffer binding logic assuming a shader register layout our fork's `.hlsl` doesn't use). That class of bug is invisible to any line-level diff; it only shows up as a wrong pixel on screen. If a sync touches anything under `package/Shaders/`, boot the deployed build and look at the affected feature in-game (devbench screenshot of an exterior cell is enough for grass/lighting) before calling the sync verified -- compiling clean and passing both structural-preservation passes is not sufficient on its own.
7. **A fork-loss/upstream-loss pass must cover every changed file, not a self-reported sample.** Give each verification agent the full `git diff <fork-pre-sync>..<HEAD> --stat` file list as an explicit checklist, and cross-check its report against that list before trusting a PASS verdict -- a summary that never names a changed file is not evidence that file was reviewed, no matter how confident it sounds. The files most likely to get silently skipped are exactly the ones item 6 warns about: shader sources paired with new upstream C++ that assumes a specific contract.

### Commit structure for a conflicted sync

When a sync has real (non-fork-owned-path) conflicts, keep the merge node itself free of fork decisions and isolate every fork re-adaptation in one separate follow-up commit:

1. Resolve conflicts **toward upstream** (`-X theirs`, or take-theirs by hand) so the merge commit is pure — it should look like what upstream intended, nothing fork-specific baked in.
2. Add exactly one commit on top, `fix(sync): re-apply fork divergences for <ref>`, holding every fork re-adaptation the merge just dropped: VR preservation, i18n combines, `globals::game::*` renames, dropping upstream duplicates of fork-owned code.

This keeps the upstream commits reviewable in isolation and turns the tail commit into a durable checklist — the next sync's conflicts usually hit the same spots, so reading the prior tail commit tells you exactly what to re-apply instead of rediscovering it from scratch. The merge's second parent must still be the real upstream ref (this is not a cherry-pick/replay of upstream's commits plus one extra — that rewrites SHAs and breaks ancestry the same way squashing does). A single trivial one-line conflict doesn't need the ceremony of a separate tail commit; reserve this structure for syncs where fork re-adaptation is non-trivial enough to be worth documenting for next time.

If you do recurring syncs, enabling `git rerere` is worth the one-time setup — it caches each conflict resolution and replays it the next time the same hunks conflict. Per-clone setting, not repo-wide:

```bash
git config rerere.enabled true
git config rerere.autoupdate true
```

Caches live in `.git/rr-cache/` and aren't pushed, so each maintainer builds their own. CI runners start with empty caches every run and benefit nothing from rerere — only the maintainers doing the merges locally see the time savings.

## Inspecting what a sync did

Each sync workflow run leaves a summary on the run page with:

-   Upstream tip SHA
-   `git diff --stat` of files changed
-   `git log --oneline` of commits brought in

The PR itself is the primary review surface — read its diff and description before approving/merging, same as any other PR.

For deeper inspection after the PR is merged:

```bash
# all changes since the last sync merge
git log --first-parent --merges --grep='chore(sync)' -1   # find the merge commit
git diff <merge-commit>~1..<merge-commit>                  # changes the merge introduced
```
