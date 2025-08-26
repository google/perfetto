# Perfetto Developer Tools

The Perfetto team have created several scripts/tools which navigating and
working with the Perfetto codebase. This page is mainly targetted towards
frequent contributors to Perfetto (e.g. team members or external contribtors
sending a lot of PRs).

These tools have a bit of a learning curve to them but can significantly
accelerate the developer experience.

## Continuous integration

The Perfetto CI on GitHub Actions covers building and testing
on most platforms and toolchains within ~30 mins. Anecdotally most build
failures and bugs are detected at the Perfetto CI level.

You can also
[test a pending Perfetto CL against Chrome's TryBots](testing.md#chromium).

## "Stacked diffs" with GitHub

From our days on Android, Perfetto has long worked on a "stacked-diff" model and
its one that we like a lot inside the team. However, Github is very much _not_
optimized for stack diffs.

We've explored a bunch of tools in the ecosystem (git-town, Graphite, git-spice,
git-stack) and for various reasons, none of them fit all our requirements. These
are:

1. Use branches as a fundamental unit of a "PR" _not_ commits

- This is what we're used to from our Android days and we all like it.

2. Does not replace normal git commands but makes working with them easier

- We're not looking for a system which wants to take over all parts of git, just
  something which makes things a bit easier

3. _important_ Handles updating stacks by merging **not** rebasing

- The most critical one which knocks out a bunch of tools.
- The main problem here is that GitHub breaks badly for reviewers if you
  rebase + force-push.
- While it's much nicer for authors, the cost to reviewers is too high.

To this end, we have developed a bunch of Python scripts which live in our tools
folder and implement the above. You can add them to your git aliases. Modify
your `.gitconfig` to include the following:

```
[alias]
    # Create a new branch based on a parent and set its parent config
    # Usage: git new-branch <new_branch_name> [--parent <parent_branch>]
    new-branch   = "!f() { ./tools/git_new_branch.py \"$@\"; }; f"

    # Set the parent branch for the target branch (default: current)
    # Usage: git set-parent <parent_branch> [--target <branch>]
    set-parent   = "!f() { ./tools/git_set_parent.py \"$@\"; }; f"

    # Renames local branch and updates children's parent config
    # Usage: git rename-branch --new <new_name> [--target <old_name>]
    rename-branch = "!f() { ./tools/git_rename_branch.py \"$@\"; }; f"

    # Checkout the configured parent of the target branch (default: current)
    # Usage: git goto-parent [--target <branch>]
    goto-parent  = "!f() { ./tools/git_goto_parent.py \"$@\"; }; f"

    # Find and checkout a child of the target branch (default: current)
    # Usage: git goto-child [--target <branch>]
    goto-child   = "!f() { ./tools/git_goto_child.py \"$@\"; }; f"

    # Update local stack segment (target+ancestors+descendants) via merges
    # Usage: git update-stack [--target <branch>]
    update-stack = "!f() { ./tools/git_update_stack.py \"$@\"; }; f"

    # Update ALL local stacks via merges using topological sort
    # Usage: git update-all
    update-all   = "!f() { ./tools/git_update_all.py \"$@\"; }; f"

    # Push full stack segment (target+ancestors+descendants) and sync GitHub PRs
    # Usage: git sync-stack [--target <branch>] [--remote <name>] [--draft] [--force]
    sync-stack   = "!f() { ./tools/git_sync_stack.py \"$@\"; }; f"

    # Prune all local branches identical (no diff) to ~their effective parent
    # Usage: git prune-all [--dry-run]
    prune-all = "!f() { ./tools/git_prune_all.py \"$@\"; }; f"
```

All of these tools work by adding an entry to the repo's gitconfig called
`branch.{branch_name}.parent` which keeps track of the parent branch. This is
then used to figure out what a "stack" is and then perform operations on it.

A normal workflow using these tools might look like ths:

```
# Create a branch for the feature.
git new-branch dev/${USER}/my-feature

# .... hack away, make changes

# Commit; will be used as PR title
git commit -a -m 'My feature'

# Create a new branch for adding something on top of the feature.
git new-branch dev/${USER}/my-feature-2 --current-parent

# ... hack away, make changes

# Commit; will be used as PR title
git commit -a -m 'My feature changes'

# Make GitHub PRs out of the above, correctly setting up base branches and
# PR descriptions based on commit messages.
git sync-stack

# Go to my-feature to respond to review
git goto-parent

# ... make changes for review

git commit -a -m 'Respond to review'

# Update the stack so that my-feature-2 also has this commit.
git update-stack

# Sync to Github.
git sync-all

# ... my-feature is approved and merged on GitHub.

# Do a merge again to make everything up-to-date.
git update-stack

# Prune my-feature now it's no longer necessary
git prune-all
```
