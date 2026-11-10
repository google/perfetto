---
title: A blog for Perfetto
author: @primiano
summary: Eight years after Perfetto's first commit, we are finally starting a blog. Here is why, and what you can expect to read here: release announcements, case studies of real performance work, and stories from the engineering behind Perfetto itself.
cover: cover.png
---

Perfetto's first commit landed in September 2017. In the eight years since, we
have shipped dozens of releases, written a lot of documentation and answered a
lot of questions. What we never had was a place to just tell stories. This is
that place.

## How we ended up here

We are, it turns out, not fast at choosing where to talk. Back in 2019, when we
wanted a chat to interact with external contributors, most of the team spent
about two weeks trying six different options: plain IRC on Freenode, Slack,
Gitter, Spectrum, Riot/Matrix and Discord. Some were too flaky, some had no
usable mobile app, some made it too hard for anyone outside Google to join. 
We went back and forth for a month until we made a call.

Since then, [our Discord](https://discord.gg/35ShE3A) and
[GitHub Discussions](https://github.com/google/perfetto/discussions) got busier
and busier year after year. In many cases what happens there is support: deep,
specific, and of little interest to anyone outside the thread. That is what
those channels are for after all.

But every now and then, someone shows up with something remarkable. A memory
leak hunted down to a single line. A startup made hundreds of milliseconds
faster. Some fancy integration that turned Perfetto into a hardware RTL
simulation visualizer or a build system profiler. A trace that finally
explained a problem nobody else could.
Those conversations deserved a much bigger audience than a chat thread, and
instead they quietly scrolled away.

We wanted a place where they could live, with a link you can share.

For a while, mostly out of habit, we wrote these stories up as Google Docs.
For one practical reason or another, most of those docs ended up visible only
inside Google. Over time we realized there was rarely anything secret in
them, and the lessons they held were just as interesting and useful to everyone
else. But we didn't have a place to share them

... until now.

## What you will find here

There are three reasons we started this blog, and they map directly onto the
three kinds of posts you will see.

### Release announcements

We started writing proper announcements into GitHub releases a few releases ago.
We are going to broadcast release announcements on this blog as a starter, with
screenshots, with the reasoning behind a feature and with the workflows they
unlocks.
The [v59 announcement](/blog/perfetto-v59) is a good example of what to expect.

### Tracing adventures (case studies)

This is the part we are most excited about. We want to help build a culture of
performance work, where people share not just the fix but the investigation
that led to it: the problem, the trace, the queries and the before/after
numbers.

We are lucky to have a number of performance experts, inside and outside of
Google, already helping us on this journey and writing up their work. We would
like many more voices to join them. Android has been Perfetto's most natural
home so far, but going forward we are planning to expand our coverage with more
topics about Linux embedded. Because in the end the true core of Perfetot has
always been understanding intricate architectural interactions across the
operating system and the hardware.

If Perfetto helped you understand a system better, that story belongs here.

### Engineering posts

Perfetto is also an interesting piece of software to build, and we want to talk
about that too: the design decisions, the dead ends and the occasional clever
trick. A couple of posts we have in the pipeline:

- How Trace Processor's query engine grew from SQLite-backed tables into a
  columnar dataframe engine of its own.
- Making the C SDK's hot paths cheap: category lookups, clock reads and the fast
  path for emitting events.

## Write for us

If you have used Perfetto to make something faster, smaller or less janky, we
would love to publish it. The best case studies have a concrete problem, the
traces that exposed it, the queries or views that made it click, and a
measurable outcome. They do not need to be long, and they do not need to be
about Android. The journey matters more than the final destination.

Anything that has a "teachable moment" is the right content for our BLog.

Posts live on the
[`blog` branch](https://github.com/google/perfetto/tree/blog) of the Perfetto
repository, and its
[README](https://github.com/google/perfetto/blob/blog/README.md) explains how to
add one. You can open a pull request straight away, or, if you are not sure
whether your story is a good fit or how to structure it, come and say hi on
[Discord](https://discord.gg/35ShE3A) first. We are happy to help shape it.

## Follow along

New posts appear in the [Atom feed](/blog/atom.xml). For everything in between,
you know where to find us: on Discord, on GitHub Discussions, and now here.

Happy tracing!
