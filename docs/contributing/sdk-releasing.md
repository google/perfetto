# Making a new SDK release

This guide shows how to make a new Perfetto SDK release.

First, check out the code:

```bash
git clone https://android.googlesource.com/platform/external/perfetto
cd perfetto
```

Next, decide the version number for the new release (vX.Y). In general minor
updates should increment the minor version number (Y) while larger, more
significant behavioral changes should be reflected in the major version
number (X).

Continue with the appropriate section below.

## a) Creating a new major version

Create a release branch for the new major version ("5.x" here) and merge in
the code for the new release:

```bash
git fetch origin
git push origin origin/master:refs/heads/releases/v5.x
git fetch origin
git checkout -b releases/v5.x -t origin/master
git merge <sha1>
```

Continue with [building the release](#building-and-tagging-the-release).

## b) Bumping the minor version

Check out the existing release branch ("4.x" here) and merge in the desired
revision for the new release, resolving any conflicts you may encounter.

```bash
git checkout -b releases/v4.x -t origin/releases/v4.x
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
git tag -a -m "Perfetto vX.Y" vX.Y
git push origin vX.Y
```

5. Update the documentation to point to the latest release.

   - [docs/instrumentation/tracing-sdk.md](/docs/instrumentation/tracing-sdk.md)
   - [examples/sdk/README.md](/examples/sdk/README.md)

Phew, you're done!
