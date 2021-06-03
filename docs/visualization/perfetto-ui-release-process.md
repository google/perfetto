# Perfetto UI Release Process

The UI has three release channels which are configured by the [channels.json](https://github.com/google/perfetto/blob/master/ui/release/channels.json) file. The channels are:
- `stable`, the version visitors to ui.perfetto.dev received by default. Updated every four weeks.
- `canary`, a less stable but fresher release. Updated every 1-2 weeks.
- `autopush`, the current HEAD version of the UI. Unstable.

The release process is based around a four week cycle.
- Week 1: Update `canary` to `HEAD`
- Week 2: Update `canary` to `HEAD`
- Week 3: Cherry-pick bug fixes to `canary`
- Week 4: Update `stable` to current `canary`, update `canary` to `HEAD`

After the fourth week the cycle repeats from week one.

## Changing release channel

NOTE: The channel setting is persistent across page reloads.

The channel the UI is currently using is displayed in the top left corner.
If the tag after the logo shows `autopush` or `canary` that is the current channel
and if no tag is displayed the current channel is `stable`.

![perfetto-ui-channel.png](/docs/images/perfetto-ui-channel.png)

To change the channel the UI is using between `stable` and `canery` you can use the toggle on the [entrance page](https://ui.perfetto.dev).

![perfetto-ui-channel-toggle.png](/docs/images/perfetto-ui-channel-toggle.png)

To change to the `autopush` channel open devtools and enter `localStorage.setItem('perfettoUiChannel', 'autopush');` then reload.

## Which version am I using?

You can see the version of the UI you are currently using in the bottom left hand corner of the UI.

![perfetto-ui-version.png](/docs/images/perfetto-ui-version.png)

Clicking on the version number takes you to Github where you can see which commits are part of this version. The version number format is `v<Perfetto version>.0.<Commits since that version>`.

