# Making a new Python library release

This guide shows how to make a new Perfetto Python library release to PyPI.

The package version is derived automatically from the `CHANGELOG` (the top
`vX.Y` entry maps to the PyPI version `0.X.Y`), so there is no separate
version-bump step.

The normal path is the `publish-pypi` job of the
`finalize-release.yml` GitHub workflow, which runs as part of finalizing a
release. The manual path below, driven by `tools/release/release_python.py`,
is the fallback.

## Prebuilt pinning

The package ships the prebuilt manifests under
`python/perfetto/prebuilts/manifests`, and `TraceProcessor` downloads the
`trace_processor` pinned there. Those manifests are rolled on `main` only
after a release is tagged, because they hash the binaries LUCI builds from the
tag. So the tree at tag `vX.Y` still pins the previous release, and a package
built naively from the tag ships the wrong `trace_processor`.

Both release paths therefore regenerate the manifests for `vX.Y` before
building, and `python/setup.py` refuses to build when
`PERFETTO_PYPI_RELEASE` is set and the pin in
`python/perfetto/prebuilts/manifests/version.py` does not match the package
version. Never publish a package built without that variable.

## Prerequisites

- Run the script from the root of the repository.
- A Python virtual environment must exist at `.venv` (the script uses
  `.venv/bin/python`).
- A clean git working directory (no uncommitted changes).
- PyPI credentials: the username is `__token__`. For the password (API token),
  find "Perfetto PyPi API Key" on http://go/valentine.

## Publishing

1. Pick the release commit to publish from — normally the `vX.Y` tag commit.
   For example:

```bash
COMMIT=$(git rev-parse v56.0^{commit})
```

2. Run the release script, passing that commit:

```bash
tools/release/release_python.py --publish --commit "$COMMIT"
```

The script will then perform the following steps:

- **Checkout**: It will check out the specified commit.
- **Roll manifests**: It will run
  `tools/release/roll-prebuilts --manifests-only vX.Y` so the package pins
  the prebuilts of the release being published. The LUCI builds for the tag
  must have finished, otherwise the download fails.
- **Build & Publish**: It will temporarily update the `download_url` in
  `python/setup.py` to that commit's source archive, build the package (the
  version is read from the `CHANGELOG`), and, after you confirm, upload it to
  PyPI. You will be prompted for your PyPI credentials.
- **Cleanup**: It will remove the temporary build artifacts and restore
  `python/setup.py` and the manifests.
- **Final URL Update**: After publishing, the script will prompt you for a new
  branch name. It will then create a new commit on that branch that updates the
  `download_url` in `python/setup.py` to point to the commit from the
  `--commit` argument.

3. Once the script completes, push the new branch for the `download_url` update
   and create a pull request. After this final PR is landed, the release is
   complete.
