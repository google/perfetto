# Become a committer

## TL;DR

- Committership gives write access to the dev/* branches of the repo and
  Perfetto CI bots.
- You don't need to be a committer to contribute patches to Perfetto. You can
  work from a fork, as in other GitHub projects.
- If you contribute frequently to the project, having committer access to dev/*
  branches can make certain workflows easier (e.g., stacked patches).
- You can become a Perfetto committer if you have a track record of high-quality
  contributions to the project.

## What is a committer

Technically, a committer is someone who can submit their own patches or patches
from others. A committer can also review patches from others, though every patch
must also be authored or reviewed by a CODEOWNER.

This privilege is granted with some expectation of responsibility: committers
are people who care about the Perfetto project and want to help maintain it and
meet its goals. A committer is not just someone who can make changes, but
someone who has demonstrated their ability to collaborate with the team, get the
most knowledgeable people to review code, contribute high-quality code, and
follow through to fix issues (in code or tests).

## Becoming a committer

To become a committer, you must get at least ten non-trivial patches merged into
Perfetto and get an existing committer to nominate you. At least two committers,
one of whom is a top-level owner, must support the nomination.

We look for evidence that you follow Perfetto best practices and ask for
guidance when you're uncertain. Since committers can review and approve other
people's changes, we also look for good judgment in code review.

Your contributions should demonstrate your:

- Commitment to the project (10+ good patches require a lot of valuable time)
- Ability to collaborate with the team and communicate well
- Understanding of how the team works (policies, processes for testing and code
  review, etc.)
- Understanding of the project's codebase and coding style
- Ability to judge when a patch is ready for review and submission (your work
  should not generally have glaring flaws unless you're explicitly requesting
  feedback on an incomplete patch)
- Ability to write good code

## Non-trivial patches

A non-trivial patch is hard to define: a one-line change might be subtle, while
changes that touch many files might still be trivial. For example, mostly
mechanical changes (e.g., renaming functions) will probably be considered
trivial.

Even a small change is non-trivial if the rationale or benefit was non-trivial
to arrive at.

If you aren't certain whether your work meets the bar, ask an existing
committer.

## Nomination process

If you think you might be ready to be a committer, ask one of the reviewers of
your CLs or another committer familiar with your work to see if they will
nominate you.

If they agree, they nominate you by sending a pull request that edits the
Committers section of /CONTRIBUTORS.txt.

The CONTRIBUTORS.txt entry should have the following:

- First and last name
- Email address.
- GitHub handle.
- A one-line description of the area you work on or know well.

The pull request comment should have:

- An explanation of why you should be a committer.
- A list of representative landed patches.

Two other committers need to second your nomination by approving the PR.

We will wait five working days (UK) after the nomination for votes and
discussion. If there is discussion, we'll wait an additional two working days
(UK) after the last message in the discussion, to ensure people have time to
review the nomination.

If you get the votes and no one objects, you become a committer. If anyone
objects or wants more information, the committers discuss and usually come to a
consensus. If issues can't be resolved, there's a vote among current committers.

## Maintaining committer status

A community of committers working together to move the project forward is
essential to creating successful projects that are rewarding to work on. If
there are problems or disagreements within the community, they can usually be
solved through open discussion and debate.

If a committer continues to disregard good citizenship (or actively disrupts the
project), we may need to revoke that person's status. The process is the same as
for nominating a new committer: someone suggests the revocation with a good
reason, two people second the motion, and a vote may be called if consensus
cannot be reached.

As a security measure, if you are inactive for more than a year, we may revoke
your committer privileges and remove your address(es) from any OWNERS files.
This is not meant as a punishment, so if you wish to resume contributing,
contact a maintainer to ask that your access be restored, and we will normally
do so.

[Props: Much of this was inspired by/copied from the committer policies of
Chromium, WebKit and Mozilla.]
