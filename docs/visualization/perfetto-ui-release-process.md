# Perfetto UI Release Process

The UI has three release channels which are configured by the
[channels.json](/ui/release/channels.json) file. The channels are:

- `stable`, the version served by default on ui.perfetto.dev.
  Updated every four weeks.
- `canary`, a less stable but fresher release. Updated every 1-2 weeks.
- `autopush`, the current HEAD version of the UI. Unstable.

The release process is based around a four week cycle.

- Week 1: Update `canary` to `HEAD`.
- Week 2: Update `canary` to `HEAD`.
  Canary stabilization week 1/2 starts here.
  Only critical bug fixes can be cherry-picked onto `canary`.
- Week 3: Canary stabilization week 2/2.
- Week 4: Update `stable` to current `canary`, update `canary` to `HEAD`.

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

Clicking on the version number takes you to Github where you can see which commits are part of this version. The version number format is `v<maj>.<min>.<Commit SHA1 prefix>` where `<maj>.<min>` are extracted from the top entry in the
[CHANGELOG](/CHANGELOG).

## Cherry-picking a change

If a change needs to be backported onto canary or stable branches, do the
following:

```bash
git fetch origin
git co -b ui-canary -t origin/ui-canary
git cherry-pick -x $SHA1_OF_ORIGINAL_CL
git cl upload

# Repeat for origin/ui-stable branch if needed.
```

Once the cherry-picks are landed, send out a CL to update the
[channels.json](/ui/release/channels.json) in the `main` branch. See
[r.android.com/1726101](https://r.android.com/1726101) for an example.

```json
{
  "channels": [
    {
      "name": "stable",
      "rev": "6dd6756ffbdff4f845c4db28e1fd5aed9ba77b56"
      //     ^ This should point to the HEAD of origin/ui-stable.
    },
    {
      "name": "canary",
      "rev": "3e21f613f20779c04b0bcc937f2605b9b05556ad"
      //     ^ This should point to the HEAD of origin/ui-canary.
    },
    {
      "name": "autopush",
      "rev": "HEAD"
      //     ^ Don't touch this one.
    }
  ]
}
```

The state of `channels.json` in the other branches is irrelevant, the release
infrastructure only looks at the `main` branch to determine the pinning of
each channel.

After the `channels.json` CL lands, the build infrastructure will pick it up
and update ui.perfetto.dev within ~30 mins.

Googlers: You can check build progress and logs on
[go/perfetto-ui-build-status](http://go/perfetto-ui-build-status). See also
[go/perfetto-ui-autopush](http://go/perfetto-ui-autopush) and
[go/perfetto-ui-channels](http://go/perfetto-ui-channels) for the design docs of
the serving infrastructure.

## Publishing the Perfetto Chrome extension
Googlers: see go/perfetto-release-chrome-extension
