# Making a new Python library release

This guide shows how to make a new Perfetto Python library release to PyPI.

The release process is split into two stages, both orchestrated by the
`tools/release/release_python.py` script.

## Stage 1: Bumping the version

The first stage creates a pull request to update the package version.

1. Run the release script from the root of the repository.

```bash
tools/release/release_python.py --bump-version
```

The script will guide you through the following steps:

- **Versioning**: It will show you the current version from `python/setup.py` and prompt you for the new version.
- **Branching**: It will prompt you for a new branch name and create it.
- **Committing**: It will update the `version` in `python/setup.py` and create a commit.

2. Once the script completes, push the new branch and create a pull request.

3. After the pull request is reviewed and landed, proceed to Stage 2.

## Stage 2: Publishing the release and updating the download URL

The second stage publishes the package to PyPI and then creates a second pull request to update the source code with the correct download URL.

1. Find the commit hash of the landed version bump CL from Stage 1.

2. Run the release script again, providing the landed commit hash.

```bash
tools/release/release_python.py --publish --commit <landed-commit-hash>
```

The script will then perform the following steps:

- **Checkout**: It will check out the specified commit.
- **Build & Publish**: It will temporarily update the `download_url` in `python/setup.py`, build the package, and upload it to PyPI. You will be prompted for your PyPI credentials. For the username, use `__token__`. For the password (API token), find "Perfetto PyPi API Key" on http://go/valentine.
- **Cleanup**: It will remove the temporary build artifacts.
- **Final URL Update**: After publishing, the script will prompt you for a new branch name. It will then create a new commit on that branch that updates the `download_url` in `python/setup.py` to point to the commit from the `--commit` argument.

3. Once the script completes, push the new branch for the `download_url` update and create a second pull request. After this final PR is landed, the release is complete.
