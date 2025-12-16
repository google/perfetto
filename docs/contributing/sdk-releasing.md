# Making a new SDK release

This guide shows how to make a new Perfetto SDK release.

Before snapshotting a release, check that no [release-blockers](http://b/savedsearches/5776355) are
open.

Check out the code, then decide the version number for the new release (vX.Y).
The major version number (X) is incremented on every release (monthly).
The minor version number is incremented only for minor changes / fixes on top of the monthly
release (cherry-picks on the releases/vN.x branch).

Continue with the appropriate section below.

## a) Creating a new major version

Make sure that the current main branch builds on
[LUCI](https://luci-scheduler.appspot.com/jobs/perfetto) by triggering all the
builds and waiting for their success. If any of the builds fail, fix the failure
on main before proceeding.

Create an entry in CHANGELOG with the new major version: this usually involves
renaming the "Unreleased" entry to the version number you chose earlier
([example](https://r.android.com/2417175)).

Test that the perfetto build tools can parse the CHANGELOG: after building,
running `perfetto --version` should show your new version number.

Upload the CHANGELOG change and submit it on the main branch.

Create a release branch for the new major version ("v16.x" here):

```bash
git fetch origin
git push origin origin/main:refs/heads/releases/v16.x
git fetch origin
git checkout -b releases/v16.x -t origin/releases/v16.x
```

Continue with [building the release](#building-and-tagging-the-release).

## b) Bumping the minor version

Check out the existing release branch ("5.x" here) and merge in the desired
revision for the new release, resolving any conflicts you may encounter.

```bash
git checkout -b releases/v16.x -t origin/releases/v16.x
```

If you only want to introduce one or two patches in the new release, consider
cherry-picking them individually:

```bash
git cherry-pick <sha1>
```

Otherwise, you can do a full merge:

```bash
git merge <sha1>
```

Update the CHANGELOG with a dedicated entry for the new minor version.
This is important because the
[write_version_header.py](/tools/write_version_header.py) script, which is
invoked by the build system, looks at the CHANGELOG to work out the latest
v${maj}.${min} version.

For an example see [r.android.com/1730332](https://r.android.com/1730332)

```txt
v16.1 - 2021-06-08:
  Tracing service and probes:
    * Cherry-pick of r.android.com/1716718 which missed the v16 branch ... .


v16.0 - 2021-06-01:
  ...
```

## Tagging the release

1. Once all changes for the release have been merged into the release branch,
   create and push the tag for it ("vX.Y" being the new version).

```bash
# Ensure the branch is up to date
git pull

git status
# Should print: Your branch is up to date with 'origin/releases/v16.x'.
# Do NOT proceed if your branch has diverged from origin/releases/vX.X

git tag -a -m "Perfetto vX.Y" vX.Y
git push origin vX.Y
```

2. Update the documentation to point to the latest release.

   - [docs/instrumentation/tracing-sdk.md](/docs/instrumentation/tracing-sdk.md)
   - [examples/sdk/README.md](/examples/sdk/README.md)

6. Send an email with the CHANGELOG to perfetto-dev@ with perfetto-announce@
   bcc'ed (internal - make sure you have permissions before doing so) and to the
   [public perfetto-dev](https://groups.google.com/forum/#!forum/perfetto-dev).

## Creating a GitHub release with prebuilts and SDK sources

3. Within few mins the LUCI scheduler will trigger builds of prebuilt binaries
   on https://luci-scheduler.appspot.com/jobs/perfetto . Wait for all the bots
   to have completed successfully and be back into the WAITING state.

4. **IMPORTANT**: Check out the release tag before running the packaging script:

```bash
git checkout vX.Y
```

5. Run `tools/release/package-github-release-artifacts vX.Y`. This will:
   - Verify the working directory is clean (no uncommitted changes)
   - Verify you're on the correct git tag (vX.Y)
   - Download the prebuilt binaries from LUCI
   - Generate amalgamated SDK source files **from the current checkout**
   - Package everything into `/tmp/perfetto-vX.Y-github-release/`

  - There must be 12 zips in total:
    - 10 prebuilt binaries: linux-{arm,arm64,amd64},
      android-{arm,arm64,x86,x64}, mac-{amd64,arm64}, win-amd64
    - 2 SDK source zips: perfetto-cpp-sdk-src.zip, perfetto-c-sdk-src.zip
  - If one or more prebuilt zips are missing it means that one of the LUCI bots failed,
    check the logs (follow the "Task URL: " link) from the invocation log.
  - If this happens you'll need to respin a vX.(Y+1) release with the fix
    (look at the history v20.1, where a Windows failure required a respin).

6. Open https://github.com/google/perfetto/releases/new and
  - Select "Choose Tag" -> vX.Y
  - "Release title" -> "Perfetto vX.Y"
  - "Describe release" -> Copy the CHANGELOG, wrapping it in triple backticks.
  - "Attach binaries" -> Attach all twelve .zip files from the previous step
    (10 prebuilt binaries + 2 SDK source zips).

7. Run `tools/roll-prebuilts vX.Y`. It will update the SHA256 into the various
   scripts under `tools/`. Upload a CL with the changes.

8. Send an email with the CHANGELOG to perfetto-dev@ (internal) and to the
   [public perfetto-dev](https://groups.google.com/forum/#!forum/perfetto-dev).

9. Phew, you're done!
