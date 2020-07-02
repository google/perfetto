# Making a new SDK release

This guide shows how to make a new Perfetto SDK release.

Before snapshotting a release, check that no [release-blockers](http://b/savedsearches/5776355) are
open.

Check out the code:

```bash
git clone https://android.googlesource.com/platform/external/perfetto
cd perfetto
```

Next, decide the version number for the new release (vX.Y).
The major version number (X) is incremented on every release (monthly).
The minor version number is incremented only for minor changes / fixes on top of the monthly
release (cherry-picks on the releases/vN.x branch). 

Continue with the appropriate section below.

## a) Creating a new major version

Create a release branch for the new major version ("5.x" here):

```bash
git fetch origin
git push origin origin/master:refs/heads/releases/v5.x
git fetch origin
git checkout -b releases/v5.x -t origin/releases/v5.x
```

Continue with [building the release](#building-and-tagging-the-release).

## b) Bumping the minor version

Check out the existing release branch ("5.x" here) and merge in the desired
revision for the new release, resolving any conflicts you may encounter.

```bash
git checkout -b releases/v5.x -t origin/releases/v5.x
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

## Building and tagging the release

1. Generate and commit the amalgamated source files.

```bash
tools/gen_amalgamated --output sdk/perfetto
git add sdk/perfetto.{cc,h}
git commit -m "Amalgamated source for vX.Y"
```

2. Check that the SDK example code works with the new release.

```bash
cd examples/sdk
cmake -B build
cmake --build build
```

3. Upload the new release for review.

```bash
git cl upload --no-squash
```

If you get an error about a missing Change-Id field (`remote: ERROR: commit
a7c7c4c: missing Change-Id in message footer`), install the commit-msg hook
script and amend the change to make sure that field is present:

```bash
curl -Lo .git/hooks/commit-msg http://android-review.googlesource.com/tools/hooks/commit-msg
chmod u+x .git/hooks/commit-msg
git commit --amend
```

4. Once the release has been reviewed and landed, create and push the tag for
   it ("vX.Y" being the new version).

```bash
# This brings the branch up to date with the CL landed in the step above.
git pull

git status
# Should print: Your branch is up to date with 'origin/releases/v5.x'.
# Do NOT proceed if your branch has diverged from origin/releases/vX.X

git tag -a -m "Perfetto vX.Y" vX.Y
git push origin vX.Y
```

5. Update the documentation to point to the latest release.

   - [docs/instrumentation/tracing-sdk.md](/docs/instrumentation/tracing-sdk.md)
   - [examples/sdk/README.md](/examples/sdk/README.md)

Phew, you're done!
