# Branching Perfetto for Chrome milestones

Merging a (set of) Perfetto change(s) to a Chrome milestone release requires
creation of a branch in the perfetto repo, cherry-picking of the change(s) to
the branch, and updating the `DEPS` file in Chrome's milestone branch to point
to the new perfetto branch's head.

[TOC]

## Creating the perfetto branch {#branch}

1.  Determine the branch name: **`chromium/XXXX`**, where `XXXX` is the branch
    number of the milestone (see
    [Chromium Dash](https://chromiumdash.appspot.com/branches)). Example for
    M87: `chromium/4280`.

1.  Check if the branch already exists: if yes, skip to
    [cherry-picking](#all-tables). To check, you can search for it in
    [Gerrit's branch page](https://android-review.googlesource.com/admin/repos/platform/external/perfetto,branches).

1.  Look up the appropriate base revision for the branch. You should use the
    revision that Chromium's `DEPS` of the milestone branch points to (search
    for `perfetto` in the file). The `DEPS` file for branch XXXX is at:

    `https://chromium.googlesource.com/chromium/src.git/+/refs/branch-heads/XXXX/DEPS`

    Example for M87:
    [`DEPS`](https://chromium.googlesource.com/chromium/src.git/+/refs/branch-heads/4280/DEPS)
    (at time of writing) points to `f4cf78e052c9427d8b6c49faf39ddf2a2e236069`.

1.  Create the branch - the easiest way to do this is via
    [Gerrit's branch page](https://android-review.googlesource.com/admin/repos/platform/external/perfetto,branches).
    The `NEW BRANCH` button on the top right opens a wizard - fill in the branch
    name and base revision determined above. If this fails with a permission
    issue, contact the [Discord chat](https://discord.gg/35ShE3A) or
    [perfetto-dev](https://groups.google.com/forum/#!forum/perfetto-dev) mailing
    list.

## Cherry-picking the change(s) {#cherry-pick}

1.  If there are no merge conflicts, cherry-picking via Gerrit will be easiest.
    To attempt this, open your change in Gerrit and use the `Cherry pick` entry
    in the overflow menu on the top right, providing the `chromium/XXXX` branch
    name (see [above](#branch)).

1.  Otherwise, merge the patch locally into a branch tracking
    `origin/chromium/XXXX` and upload a Gerrit change as usual:

    ```
    $ git fetch origin
    $ git checkout -tb cpick origin/chromium/XXXX
    $ git cherry-pick -x <commit hash>    # Resolve conflicts manually.
    $ tools/gen_all out/xxx               # If necessary.
    $ git cl upload    # Remove "Change-Id:" lines from commit message.
    ```

1.  Send the patch for review and land it. Note the commit's revision hash.

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
