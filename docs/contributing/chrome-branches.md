# Branching Perfetto for Chrome milestones

Merging a (set of) Perfetto change(s) to a Chrome milestone release requires
creation of a branch in the perfetto repo, cherry-picking of the change(s) to
the branch, and updating the `DEPS` file in Chrome's milestone branch to point
to the new perfetto branch's head.

## Creating the perfetto branch {#branch}

1.  Determine the branch name: `chromium/XXXX`, where `XXXX` is the branch
    number of the milestone (see
    [Chromium Dash](https://chromiumdash.appspot.com/branches)). Example for
    M87: `chromium/4280`.

1.  Check if the branch already exists: if yes, skip to
    [cherry-picking](#all-tables). To check, you can search for it in
    https://github.com/google/perfetto/branches.

1.  Look up the appropriate base revision for the branch. You should use the
    revision that Chromium's `DEPS` of the milestone branch points to (search
    for `perfetto` in the file). The `DEPS` file for branch XXXX is at:

    `https://chromium.googlesource.com/chromium/src.git/+/refs/branch-heads/XXXX/DEPS`

    Example for M87:
    [`DEPS`](https://chromium.googlesource.com/chromium/src.git/+/refs/branch-heads/4280/DEPS)
    (at time of writing) points to `f4cf78e052c9427d8b6c49faf39ddf2a2e236069`.

1.  Create the branch:
    Ask a member of [perfetto-team](https://github.com/orgs/google/teams/perfetto-team/)
    to create a chromium/XXXX branch via `git push origin 4cf78e05:chromium/4280`

## Cherry-picking the change(s) {#cherry-pick}

1.  Cherry-pick the commit locally and send a pull-request against the branch
    as usual.

    ```
    $ git fetch origin
    $ git checkout -tb cpick origin/chromium/XXXX
    $ git cherry-pick -x <commit hash>    # Resolve conflicts manually.
    $ tools/gen_all out/xxx               # If necessary.
    $ gh pr create
    ```

1.  Send the pull request for review and land it.
    Note the commit's revision hash.

## Updating the DEPS file in Chromium

1.  Create, send for review, and land a Chromium patch that edits the top-level
    `DEPS` file on the Chromium's milestone branch. You can also combine this
    step with cherry-picks of any chromium changes. For details, see
    [Chromium's docs](https://www.chromium.org/developers/how-tos/drover). It
    amounts to:

    ```
    $ gclient sync --with_branch_heads
    $ git fetch
    $ git checkout -tb perfetto_uprev refs/remotes/branch-heads/XXXX
    $ ...    # Edit DEPS.
    $ git cl upload
    ```
