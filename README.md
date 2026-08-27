# Perfetto blog

Source for the posts published at <https://perfetto.dev/blog>.

This is an orphan branch: it shares no history with `main` and contains no code.
Writing a post never touches the code repository, and post images never land in
the main branch's object store.

**TL;DR**

- One directory per post, named `YYYY-MM-DD-short-title`.
- The markdown goes in `post.md`; images sit next to it.
- Open a pull request against the `blog` branch. No CI runs on it.

## Getting set up

**Do this from a normal `main` checkout of Perfetto, not from a standalone
clone of this branch.** This branch contains no code: the generator that turns
these files into a website lives on `main`, under `infra/perfetto.dev/`, and it
looks for the posts in a `blog/` directory at the root of that checkout. On its
own, this branch is just markdown with nothing to render it.

So check this branch out as a worktree inside a `main` checkout:

```sh
# From the root of a perfetto checkout that is on main.
git fetch origin blog
git worktree add blog blog
```

That gives you `<perfetto>/blog`, which is where the build expects to find the
posts. `/blog` is listed in `main`'s `.gitignore`, so the worktree does not show
up as untracked in the code repo.

If you already have the `blog` branch checked out somewhere else, git will
refuse to check it out twice. Symlink it instead:

```sh
ln -s /path/to/your/blog/checkout blog
```

The build treats the directory as optional: with no `blog/` worktree the site
still builds, just without a blog. That is deliberate, so a contributor who only
touches C++ never has to care that this branch exists -- but it does mean a
missing worktree looks like a working build with no posts in it rather than an
error.

## Adding a post

```txt
2026-08-25-chasing-a-40ms-jank/
├── post.md
├── sched-track.png
└── flamegraph.png
```

`post.md` starts with required front matter:

```markdown
---
title: Chasing a 40ms jank down to a 3µs futex
author: @stevegolton
summary: A scroll stutter that no in-process profiler could explain.
---

Body starts here. The title above is the page's `<h1>`; do not repeat it.
```

| Field | |
|---|---|
| `title` | Rendered as the `<h1>` and the browser title. |
| `author` | One or more GitHub handles, space separated: `@a @b`. Avatars come from GitHub. |
| `summary` | One or two sentences. Used on the index card, the `<meta description>` and the Atom feed. |

The publish date comes from the directory name — there is no `date:` field to
forget to update.

## URLs

The date is dropped from the URL, and images are moved under `/blog/media/`:

| Source | Published at |
|---|---|
| `2026-08-25-chasing-a-40ms-jank/post.md` | `/blog/chasing-a-40ms-jank` |
| `2026-08-25-chasing-a-40ms-jank/sched-track.png` | `/blog/media/chasing-a-40ms-jank/sched-track.png` |

Two posts must not reduce to the same slug once the date is dropped; the build
fails if they do.

## Writing

Reference an image the obvious way — a plain relative filename. The build
rewrites the path:

```markdown
![The scheduling track](sched-track.png)
```

Links to other posts and to the docs use absolute site paths:

```markdown
[an earlier post](/blog/some-other-post)
[the ftrace docs](/docs/data-sources/ftrace)
```

Do not use `../` in links; the build rejects them. Everything else is ordinary
Perfetto markdown — the same renderer as the docs, so callouts (`NOTE:`,
`TIP:`, `WARNING:`), mermaid diagrams, `<?tabs>` blocks and syntax-highlighted
code all work.

## Cover images

Every post gets a card image on the index, and there is nothing to do about it.

If the post body references an image, the first one is used. Otherwise the build
generates one from the title: a flat, abstract, text-free SVG. Nothing is
committed and there is no step to remember.

NOTE: because the generated image is derived from the title, retitling a post
changes its cover.

## Previewing

From the root of the `main` checkout that holds the worktree (see *Getting set
up* above):

```sh
infra/perfetto.dev/run-dev-server
```

then open <http://localhost:8082/blog/> (pass `--port` if that one is taken).
The dev server reads the posts
straight out of the `blog/` worktree, rebuilds on save and reloads the page for
you.

If the blog index comes up empty, the worktree is missing -- the build does not
create it for you.
