# Become a committer

## TL;DR

- Committership gives write access to the dev/* branches of the repo and
  Perfetto CI bots.
- You don't need to be a committer to contribute patches to Perfetto as you can
  work from forked repos like in any other GitHub project.
- If you contribute frequently to the project, having committer access to dev/*
  branches can make certain workflows easier (e.g., stacked patches).
- You can become a Perfetto committer if you have a track record of high quality
  contributions to the project.

## What is a committer

Technically, a committer is someone who can submit their own patches or patches
from others. A committer can also review patches from others, though all patches
need to either be authored by or reviewed by a CODEOWNER as well.

This privilege is granted with some expectation of responsibility: committers
are people who care about the Perfetto project and want to help maintain it and
meet its goals. A committer is not just someone who can make changes, but
someone who has demonstrated their ability to collaborate with the team, get the
most knowledgeable people to review code, contribute high-quality code, and
follow through to fix issues (in code or tests).

A committer is a contributor to the Perfetto project success and a citizen
helping the projects succeed.

## Becoming a committer

To become a committer, you must get at least ten non-trivial patches merged into
Perfetto, and get an existing committer to nominate you. You will need at least
two committers, one of which a top-level owners, to support the nomination.

We want to see sufficient evidence that you can follow Perfetto best practices
and in situations where you're uncertain, you ask for additional guidance
effectively. Perhaps the most important aspect of being a committer is that you
will be able to review and approve other people's changes, so we're looking for
whether we think you'll do a good job at that.

So, in addition to actually making the code changes, you're basically
demonstrating your:

- Commitment to the project (10+ good patches require a lot of valuable time)
- Ability to collaborate with the team and communicate well
- Understanding of how the team works (policies, processes for testing and code
review, etc)
- Understanding of the projects' code base and coding style
- Ability to judge when a patch might be ready for review and to submit (your
work should not generally have glaring flaws unless you're explicitly requesting
feedback on an incomplete patch)
- Ability to write good code (last but certainly not least)

## Non-trivial patches

It is unfortunately not easy to define what a non-trivial patch is, because
a one-line change might be subtle, and changes that touch lots of files might
still be trivial. For example, changes that are more-or-less mechanical
(e.g., renaming functions) will probably be considered trivial.

Even a small change is non-trivial if the rationale or benefit was non-trivial
to arrive at.

If you aren't certain whether your work meets the bar, ask an existing
committer.

## Nomination process

If you think you might be ready to be a committer, ask one of the reviewers of
your CLs or another committer familiar with your work to see if they will
nominate you.

If they are, they nominate you by sending a pull request that edits the
Committers section of the /CONTRIBUTORS.txt.

The CONTRIBUTORS.txt entry should have the following:

- First and last name
- Email address.
- GitHub handle.
- A one line description of the area you work on / retain knowledge of.

The pull request comment should have:

- An explanation of why you should be a committer.
- A list of representative landed patches.

Two other committers need to second your nomination by approving the PR.

We will wait five working days (UK) after the nomination for votes and
discussion. If there is discussion, we'll wait an additional two working days
(UK) after the last message in the discussion, to ensure people have time to
review the nomination.

If you get the votes and no one objects, at that point you become a committer.
If anyone objects or wants more information, the committers discuss and usually
come to a consensus. If issues can't be resolved, there's a vote among current
committers.

## Maintaining committer status

A community of committers working together to move the project forward is
essential to creating successful projects that are rewarding to work on. If
there are problems or disagreements within the community, they can usually be
solved through open discussion and debate.

In the unhappy event that a committer continues to disregard good citizenship
(or actively disrupts the project), we may need to revoke that person's status.
The process is the same as for nominating a new committer: someone suggests the
revocation with a good reason, two people second the motion, and a vote may be
called if consensus cannot be reached.

In addition, as a security measure, if you are inactive on for more than
a year, we may revoke your committer privileges and remove your address(es)
from any OWNERS files. This is not meant as a punishment, so if you wish to
resume contributing after that, contact a maintainer to ask that it be restored,
and we will normally do so.

[Props: Much of this was inspired by/copied from the committer policies of
Chromium, WebKit and Mozilla.]
