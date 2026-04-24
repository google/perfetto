# Perfetto UI Release Process

The UI has three release channels. Each channel is served from the HEAD of a
long-lived branch:

- `stable`, the version served by default on ui.perfetto.dev.
  Served from the `stable` branch and updated every four weeks.
- `canary`, a less stable but fresher release. Updated every 1-2 weeks.
  Served from the `canary` branch.
- `autopush`, the current HEAD version of the UI. Unstable. Served from the
  `main` branch.

The release process is based around a four week cycle.

- Week 1: Cut `canary` from `main`.
- Week 2: Cut `canary` from `main`.
  Canary stabilization week 1/2 starts here.
  Only critical bug fixes can be cherry-picked onto `canary`.
- Week 3: Canary stabilization week 2/2.
- Week 4: Promote current `canary` to `stable`, then cut `canary` from `main`.

After the fourth week the cycle repeats from week one.
This is so that:

- Canary soaks for two weeks before being promoted to stable.
- Newer features can be tried out in Canary within a week, or two at most (if
  in the stabilization weeks).
- Stable users aren't disrupted more than once per month.

## Changing release channel

NOTE: The channel setting is persistent across page reloads.

The channel the UI is currently using is displayed in the top left corner.
If the tag after the logo shows `autopush` or `canary` that is the current channel
and if no tag is displayed the current channel is `stable`.

![perfetto-ui-channel.png](/docs/images/perfetto-ui-channel.png)

To change the channel the UI is using between `stable` and `canary` you can use the toggle on the [entrance page](https://ui.perfetto.dev).

![perfetto-ui-channel-toggle.png](/docs/images/perfetto-ui-channel-toggle.png)

To change to the `autopush` channel, open the `Flags` screen in the `Support`
section of the sidebar, and choose `Autopush` in `Release channel`.

![perfetto-ui-channel-autopush-toggle.png](/docs/images/perfetto-ui-channel-autopush-toggle.png)

## Which version am I using?

You can see the version of the UI you are currently using in the bottom left hand corner of the UI.

![perfetto-ui-version.png](/docs/images/perfetto-ui-version.png)

Clicking on the version number takes you to GitHub where you can see which commits are part of this version. The version number format is `v<maj>.<min>.<Commit SHA1 prefix>` where `<maj>.<min>` are extracted from the top entry in the
[CHANGELOG](/CHANGELOG).

## Cherry-picking a change

If a change needs to be backported onto canary or stable branches, do the
following:

```bash
git fetch origin
git checkout -b cherry-pick-canary origin/canary
git cherry-pick -x $SHA1_OF_ORIGINAL_CL
git cl upload

# Repeat from origin/stable if needed.
```

Once the cherry-pick lands on `canary` or `stable`, the push to that branch
triggers Cloud Build for the corresponding UI channel. There is no separate
channel pinning file to update.

To do the normal release-channel moves, use the GitHub Actions workflows:

- `Cut canary (open PR merging main -> canary)` opens a PR against `canary`.
  When that PR is merged, Cloud Build redeploys the canary channel.
- `Promote to stable (open PR merging canary -> stable)` opens a PR against
  `stable`. When that PR is merged, Cloud Build redeploys the stable channel
  and `tag-on-stable-push.yml` creates the release tag and draft release.

Googlers: You can check build progress and logs on
[go/perfetto-ui-build-status](http://go/perfetto-ui-build-status). See also
[go/perfetto-ui-autopush](http://go/perfetto-ui-autopush) and
[go/perfetto-ui-channels](http://go/perfetto-ui-channels) for the design docs of
the serving infrastructure.

## Publishing the Perfetto Chrome extension
Googlers: see go/perfetto-release-chrome-extension
