# Release Automation and Unified Canary/Stable Branches

**Authors:** @lalitm

**Status:** Draft

## Problem

Perfetto's release process today is a patchwork of partially-automated scripts
and manual steps spread across three loosely-coupled pipelines — the SDK/native
binaries, the UI, and the Python package. Each has its own versioning model,
its own branching convention, and its own notion of "canary" vs "stable". The
result is a release workflow that is:

1. **Manual and error-prone.** `tools/release/release_perfetto.py` requires a
   human to run it locally, answer prompts, wait on LUCI builds, hand-upload
   artifacts, and manually create the GitHub release. A single release is a
   multi-hour, multi-terminal babysitting exercise.

2. **Fragmented across artifacts.** The SDK uses `releases/vX.x` maintenance
   branches and `vX.Y` tags. The UI uses `ui-canary` / `ui-stable` branches
   plus a `channels.json` file in `main` that pins each channel to a specific
   SHA. The Python package uses a hardcoded version string in `python/setup.py`
   and a separate `release_python.py` script that is not run as part of the
   normal release flow. These three things drift: the UI on `ui-stable` may
   correspond to a completely different commit than the SDK tagged `vX.Y`.

3. **No clear "release-in-progress" state.** There is no single branch a
   contributor can look at and say "this is what v54 will contain". The
   decision of what goes into v54 is implicit in whoever ran the release
   script and when.

4. **UI channels driven by a file, not a branch.** `channels.json` was a
   pragmatic choice when it was introduced but it means:
   - Cherry-picks to a channel require two PRs (one to the channel branch,
     one to `main` to bump the SHA).
   - The source of truth for "what is canary" is a JSON file, not the branch
     named `ui-canary`.
   - Anyone with commit access to `main` can redirect a channel.

5. **No PyPI automation.** Python releases happen rarely and out-of-band from
   SDK releases. The version in `setup.py` is updated by hand, and pushing to
   PyPI is a manual `twine upload`.

6. **LUCI build completion is not observable from GitHub.** Builds are
   triggered by tag push (`refs/tags/upstream/v.+`) but there is no hook back
   into the GitHub release. Whoever cut the release has to watch LUCI, wait
   for all four platforms to finish, download the artifacts, and upload them
   to the release by hand.

We want to get to a world where cutting a release is a small number of button
clicks in the GitHub UI, where the branch state at any moment tells you
exactly what is in flight, and where the SDK, UI, and Python package move
together.

## Decision

Pending.

## Design

### Summary

Replace the per-release `releases/vX.x` branches and the UI's `channels.json`
file with two long-lived branches — `canary` and `stable` — that are shared by
the SDK, the UI, and the Python package. All transitions between branches
(`main` → `canary`, `canary` → `stable`) are triggered by clicking
`workflow_dispatch` buttons in the GitHub Actions UI. The `stable` promotion
is what tags the release, and the tag push is the single event that fans out
to LUCI (native binaries), Cloud Build (UI), and GitHub Actions (PyPI + GitHub
Release creation). LUCI artifact attachment is reconciled by a final
human-triggered "finalize release" button.

Independent artifact producers (LUCI, Cloud Build, GH Actions) converge on
the same GitHub release using a "create draft if missing, else upload to
existing" idempotent pattern, so no producer needs to wait for any other.

### Branches

Three long-lived branches, each protected (no direct pushes; PRs only):

| Branch | Represents | Updated by |
|---|---|---|
| `main` | HEAD of development | normal PR merges |
| `canary` | Next release, feature-locked | button: `main` → `canary` (fast-forward) |
| `stable` | Current shipping release | button: `canary` → `stable` (fast-forward + tag) |

The existing `releases/vX.x` branches are frozen in place as historical
artifacts. We do not create new ones and we do not backport to them. This
matches current practice: we already only support the latest release.

The existing `ui-canary` and `ui-stable` branches go away, replaced by the
unified `canary` / `stable`. The `channels.json` file is deleted.

### Version numbering

The version is derived from the top of the `CHANGELOG` file, as today, via
`tools/write_version_header.py`. This now becomes the source of truth for
every artifact:

- Native binaries: `vX.Y` (C++ header generated at build time).
- UI: `vX.Y-<9-char-SHA>` (as today).
- Python package: `0.X.Y` (auto-derived from `CHANGELOG` at wheel-build time;
  the hardcoded string in `python/setup.py` is removed). The `0.` prefix
  keeps the package in the pre-1.0 series while encoding the Perfetto
  release in the minor/patch components, giving a monotonic jump from the
  current `0.16.0`.

The CHANGELOG is bumped in a normal PR to `main` before the `main` → `canary`
fast-forward. The person cutting canary is responsible for confirming that
`CHANGELOG` already contains the intended `vX.Y` entry.

### Release lifecycle

```
                 main
                  │
                  │  (1) click "Cut canary"  ─── fast-forward canary to main SHA
                  ▼
                canary  ◄────── cherry-pick PRs target canary directly
                  │
                  │  (2) UI Cloud Build auto-deploys canary channel
                  │      on every push to `canary`
                  │
                  │  (3) click "Promote canary to stable"
                  ▼
                stable  ──► tag vX.Y pushed as part of the same action
                  │
                  │  (4) tag push triggers in parallel:
                  │        - LUCI official builds (4 platforms)
                  │        - Cloud Build UI deploy to stable channel
                  │        - GH Actions: build+publish PyPI wheel
                  │        - GH Actions: create draft GitHub release
                  │
                  │  (5) click "Finalize release" once LUCI is done
                  ▼
            artifacts attached to draft
                  │
                  │  (6) human reviews notes and clicks "Publish" in the
                  │      GitHub UI
                  ▼
                 released
```

### The four buttons

All four are `workflow_dispatch` workflows under `.github/workflows/`.

**Button 1: `cut-canary.yml`**. Creates a merge commit on `canary` whose
tree equals the current tip of `main`, using the `git merge -s ours` + `git
read-tree -m -u` pattern (same approach as the existing
`merge-main-to-canary.yml`). This always moves forward (no force-push
needed) and handles the normal case where cherry-picks on `canary` have
different SHAs than their `main` counterparts, so a true "fast-forward"
would never succeed after one release cycle.

Convention: every fix lands on `main` first and is then cherry-picked to
`canary`. Anything that exists only on `canary` will be silently dropped at
the next cut, because we're replacing `canary`'s tree with `main`'s. The
dropped commit is still in canary's git history and can be re-cherry-picked
if needed. Opens no PR; the push itself is the event. Pushes to `canary`
trigger Cloud Build to redeploy the canary UI.

**Button 2: `promote-stable.yml`**. Applies the same tree-replacing merge
pattern to push `canary`'s tree onto `stable`, then creates and pushes the
`vX.Y` tag (version read from `CHANGELOG`) pointing at the resulting
`stable` HEAD. This single action is what triggers every downstream build.

**Button 3: `finalize-release.yml`**. Takes a version input (e.g. `v54.0`).
Downloads the LUCI artifacts from GCS, verifies they match the expected
manifest, and attaches them to the draft GitHub release. Leaves the
release as a draft — a maintainer reviews the release notes and clicks
"Publish" manually from the GitHub UI. Idempotent — safe to re-run.

Cherry-picks are opened as regular PRs targeting `canary`. There is no
"hotfix directly to stable" path: even emergency fixes go `main` → `canary`
→ `stable` via the normal buttons. This keeps the invariant that `stable` is
always an ancestor of `canary`, which in turn keeps the promotion button a
pure fast-forward.

### Artifact flow on tag push

The `vX.Y` tag push is the single event that fans out. Four independent
producers each converge on the same GitHub release using the
"create-if-missing, upload-on-exists" pattern:

```bash
if gh release view "$TAG" > /dev/null 2>&1; then
  gh release upload "$TAG" <files> --clobber
else
  gh release create "$TAG" --draft --generate-notes <files>
fi
```

Producers:

1. **LUCI native builds** (Linux, macOS, Windows, Android). Triggered as
   today by the `refs/tags/upstream/v.+` scheduler rule. Each builder already
   uploads to `gs://perfetto-luci-artifacts/<git-revision>/<arch>/<binary>`
   (with a parallel `latest/` alias), and the SDK source zips land at
   `gs://perfetto-luci-artifacts/<version>/sdk/`. Button 3 reconciles these
   existing paths into the GitHub release — no new bucket or layout is
   required.

2. **UI Cloud Build.** The existing trigger (formerly keyed on changes to
   `channels.json`) is re-keyed to fire on pushes to the `stable` branch.
   Builds the UI from `stable` HEAD and deploys to the stable channel.

3. **PyPI publish workflow** (`publish-pypi.yml`). Triggered by tag push.
   Builds a pure-Python wheel with version derived from `CHANGELOG`, publishes
   to PyPI using OIDC trusted publishing (no long-lived token). Does not ship
   native binaries in v1.

4. **GitHub Release draft** (`draft-release.yml`). Triggered by tag push.
   Creates the draft release; the body is pre-populated with the
   corresponding `vX.Y` section of the `CHANGELOG` verbatim, so even if no
   human touches it the release still has useful content. The draft exists
   from the moment the tag is pushed; LUCI artifacts are attached
   asynchronously.

   Release notes themselves are human-authored, not automated. Before
   clicking `finalize-release` a maintainer is expected to edit the draft's
   body in the GitHub UI to replace the raw CHANGELOG with prose release
   notes — thematic grouping, highlights, links to docs, etc. The existing
   `tools/release/gen_release_notes.py` script (an AI-prompt generator for
   exactly this authoring step) is retained for this workflow. It is
   deliberately not invoked from CI: the release notes are a curated
   artifact, and the cost of a bad auto-generated announcement is higher
   than the cost of one manual editing step per release.

### LUCI → GitHub bridge

The tag-push → LUCI path already works today. The missing piece is notifying
GitHub when LUCI is done so artifacts can be attached.

For v1 we deliberately avoid a webhook. Instead, button 3 (`finalize-release`)
is a manually-triggered GH Actions workflow that a human clicks once LUCI is
green. It reads from the existing `gs://perfetto-luci-artifacts/` paths
(which are world-readable via `storage.cloud.google.com`) and uploads into
the draft release. This keeps the secret surface minimal — GH Actions already
has `GITHUB_TOKEN`, and no GCS credential is needed since the bucket is
public — at the cost of one extra click per release.

### UI: `channels.json` removal

The `channels.json` file and the associated `ui-canary` / `ui-stable` branches
go away. The UI Cloud Build trigger is reconfigured to build:

- `autopush` channel on every push to `main` (as today).
- `canary` channel on every push to `canary`.
- `stable` channel on every push to `stable`.

Because `canary` and `stable` are protected branches, there is no way to
accidentally redirect a channel without going through a PR, which is a
strictly stronger guarantee than `channels.json` provides today.

The UI version string (`vX.Y-<SHA>`) continues to be computed from `CHANGELOG`
+ branch HEAD SHA at build time, so it remains meaningful on every channel.

`channels.json` is not a public API, but as it has lived in the repo for a
long time the removal will be called out in the CHANGELOG and in the first
release's GitHub release notes.

### Python packaging

For v1, the PyPI release is a pure-Python wheel (no bundled
`trace_processor_shell`) on stable tags only. The version is auto-derived
from `CHANGELOG`, killing the hardcoded `0.16.0` in `python/setup.py`.

Bundling `trace_processor_shell` into the wheel — so that `pip install
perfetto` gives you a working CLI — is an attractive future step but has
non-trivial packaging implications (multi-platform wheels, LUCI artifact →
PyPI pipeline, manylinux compatibility). Out of scope for v1.

### What happens to existing scripts

- `tools/release/release_perfetto.py`: deleted. Its responsibilities are
  split across the four GH Actions workflows.
- `tools/release/release_python.py`: deleted. Replaced by `publish-pypi.yml`.
- `tools/release/gen_release_notes.py`: retained. Invoked by the draft-release
  workflow if `--generate-notes` is insufficient.
- `tools/release/package-github-release-artifacts`: retained, invoked from
  LUCI builders to produce the per-platform zips.
- `tools/release/roll-prebuilts`: unchanged (separate concern: rolling
  prebuilts into the repo, not cutting a release).

### Protected branch configuration

- `main`, `canary`, `stable`: all require PR, no direct push, no
  force-push, require status checks.
- `canary` and `stable` additionally require a review from a designated
  release-approver group (to prevent drive-by cherry-picks).
- The `vX.Y` tag namespace is restricted to being created by the
  `promote-stable.yml` workflow's GH App / token (not by humans).

## Alternatives considered

### Keep `releases/vX.x` maintenance branches

Pro:

* Familiar. Allows in-theory backporting.

Con:

* We don't actually backport in practice — the last non-trivial backport was
  years ago.
* Forces the human to decide "is this a new major or minor" at release time,
  which has no meaning for how we actually develop.
* Branches accumulate indefinitely; the repo has 15+ of them already.

### Keep `channels.json`

Pro:

* Already works.
* Channel redirection is a single-file PR.

Con:

* Indirection: `ui-stable` branch exists but is not the source of truth.
* Two PRs for every cherry-pick.
* Different mental model from the SDK side of the release.

### LUCI → GitHub via webhook instead of manual finalize button

Pro:

* One fewer button click.
* Release publishes automatically the moment the last platform finishes.

Con:

* Requires standing up a Cloud Function (or similar) with a GitHub App
  credential.
* Adds a new failure mode (webhook delivery).
* v1 has humans in the loop anyway for the two earlier buttons, so adding
  one more at the end is marginal.

Revisit once the rest of the flow is proven.

### Tag on canary cut instead of on stable promotion

Pro:

* Artifacts would be buildable on canary, giving us pre-release binaries.

Con:

* Two tags per release (`vX.Y-canary`, `vX.Y`) is more machinery.
* LUCI builds are expensive; building twice per release doubles the spend for
  marginal benefit.
* The UI already provides a canary channel with real user exposure; we don't
  need binary prereleases.

### Monorepo-style unified version (e.g. `54.0.0` everywhere)

Pro:

* Uniform across SDK, UI, Python.

Con:

* Breaks existing `vX.Y` convention and existing consumers' parsers.
* Python's `X.Y.Z` semantics don't quite map (we never ship `.Z` patch
  versions).

Keep the existing `vX.Y` for SDK/UI; map it to `0.X.Y` for Python.

## Open questions

None at this time.

