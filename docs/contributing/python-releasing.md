# Making a new Python library release

This guide shows how to make a new Perfetto Python library release to PyPI.

The package version is derived automatically from the `CHANGELOG` (the top
`vX.Y` entry maps to the PyPI version `0.X.Y`), so there is no separate
version-bump step. Publishing is a single stage, driven by the
`tools/release/release_python.py` script.

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
- **Build & Publish**: It will temporarily update the `download_url` in
  `python/setup.py` to that commit's source archive, build the package (the
  version is read from the `CHANGELOG`), and, after you confirm, upload it to
  PyPI. You will be prompted for your PyPI credentials.
- **Cleanup**: It will remove the temporary build artifacts and restore
  `python/setup.py`.
- **Final URL Update**: After publishing, the script will prompt you for a new
  branch name. It will then create a new commit on that branch that updates the
  `download_url` in `python/setup.py` to point to the commit from the
  `--commit` argument.

3. Once the script completes, push the new branch for the `download_url` update
   and create a pull request. After this final PR is landed, the release is
   complete.
