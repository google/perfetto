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

2. Check that the [SDK example
   code](https://github.com/skyostil/perfetto-sdk-example) works with the new
   release.

```bash
git clone https://github.com/skyostil/perfetto-sdk-example /tmp/perfetto-sdk-example
cp -r sdk/ /tmp/perfetto-sdk-example/perfetto
pushd /tmp/perfetto-sdk-example
cmake -B build
cmake --build build
popd
```

3. Upload the new release for review.

```bash
git cl upload
```

4. Once the release has been reviewed and landed, create and push the tag for
   it ("vX.Y" being the new version).

```bash
git tag -a -m "Perfetto vX.Y" vX.Y
git push origin vX.Y
```

5. Roll the SDK example code to the new release.

```bash
pushd /tmp/perfetto-sdk-example
rm -rf perfetto/sdk
git submodule update --init --recursive
cd perfetto
git checkout vX.Y
cd ..
git add perfetto
git commit -m "Roll to perfetto vX.Y"
git push
popd
```

6. Update [the documentation](../instrumentation/tracing-sdk.md) to point to the
   latest release.

Phew, you're done!
