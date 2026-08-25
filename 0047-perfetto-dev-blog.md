# A blog for perfetto.dev

**Authors:** @primiano

**Status:** Draft

**PR:** N/A

This proposes adding `/blog` to perfetto.dev: a chronological, statically
generated blog whose posts live on a dedicated `blog` git branch, rendered by
the existing `infra/perfetto.dev` pipeline.

## Problem

Perfetto has no place to publish prose. We have reference documentation and we
have a `CHANGELOG`, and neither is a thing anybody reads for pleasure or
subscribes to. Three concrete gaps:

* **Releases land silently.** `v58.0` shipped trace merging, Zstd compression, a
  unified stack-sample format and a client/server mode for `trace_processor`.
  As a changelog that is a wall of bullet points. Nobody outside the project
  learns that any of it happened.
* **Investigations have nowhere to go.** `docs/case-studies/memory.md` and
  `docs/case-studies/scheduling-blockages.md` are blog posts filed as
  documentation, because documentation was the only shelf available.
* **We do not talk to the ecosystem that already talks about us.** Collabora
  (Panfrost GPU counters), Ruby's ZJIT, `dotnet-trace` and QEMU all publish
  Perfetto material on their own blogs. Nobody links them together.

### Constraint

perfetto.dev is fully static, served by GCS as the rest of the perfetto.dev site.
No server-side rendering and no runtime dependency on a third party.

## Decision

For now integrated as part of our perfetto.dev website (same hosting infra, same
build process). Use a different branch (blog) to host posts and images, similarly
to what we do for rfcs.

## Design

### Posts live on a dedicated `blog` branch

An orphan branch named `blog`, exactly mirroring how `rfcs` works today: flat
directories at the root, no shared history with `main`, and its own tooling if it
ever needs any.

The point is that writing a post never touches the code repository. A post is not
a code change, should not go through code review latency, and should not appear
in `git log` for `main`. It also means post media does not accrete in the main
branch's object store forever.

`analyze.yml` only runs the test matrix on `main` and `dev/**`, so a PR against
`blog` fires no CI. `blog` is a real branch so that GitHub can open pull
requests against it — which is what makes guest posts and drive-by typo fixes
possible at all.

### Source layout

At the root of the `blog` branch, one directory per post:

```txt
2026-08-25-chasing-a-40ms-jank/
├── post.md
├── sched-track.png
└── flamegraph.png
```

`post.md` carries required front matter:

```yaml
---
title: Chasing a 40ms jank down to a 3µs futex
author: @stevegolton
summary: A scroll stutter that no in-process profiler could explain.
---
```

`author` uses the same `@GithubHandle` notation as `rfcs/template.md`, which
makes multiple authors free (`author: @stevegolton @LalitMaganti`) and gives us
the avatar without a second field. The publish date is parsed from the directory
name; there is no `date:` field to forget to update.

### Production layout

```txt
/blog                                          index
/blog/chasing-a-40ms-jank                      post (extensionless file)
/blog/media/chasing-a-40ms-jank/sched-track.png
/blog/media/authors/stevegolton.png
/blog/atom.xml
```

Two deliberate transformations happen at generation time:

* **The date is elided from the URL.** It exists to order posts and to render a
  byline, not to be part of a permalink. Correcting a wrong publish date should
  not break inbound links.
* **Media is hoisted to `/blog/media/<slug>/`.** This is not cosmetic. A POSIX
  filesystem cannot hold both a file `blog/my-post` and a directory
  `blog/my-post/`, and the output tree is a real directory that `gsutil rsync`
  mirrors into the bucket. Hoisting media is what lets a post stay an
  extensionless file like `docs/analysis/sql-tables`, which in turn means the
  App Engine proxy needs no changes — it only appends `index.html` to
  trailing-slash paths and would otherwise 404 on a bare post URL.

### Rendering

Posts are rendered by the existing `src/markdown_render.js`, gaining a
`--mode=docs|blog` flag. `docs` keeps every current value, so docs output stays
byte-identical; this is the first thing to verify.

Three behaviours are genuinely docs-specific and must be switched off:

* **Absolute hrefs outside `/docs/` are rewritten to `source.chromium.org`** and
  then dead-link checked against the monorepo. A post linking to
  `/blog/other-post` would silently become a broken Chromium URL *and* fail the
  build.
* **Images are copied to the same path under `/docs/`.** Posts need the `media/`
  hoist described above instead.
* **The nav sidebar is read from `docs/_nav.html`.** A post has no sidebar, and
  the template references it unguarded, so rendering would throw.

Everything else is reused as-is: callouts, mermaid, `{#anchor}` ids, code-copy
buttons, `<?tabs>`, and the `../`-forbidding link assertion.

Front matter is parsed by ~20 lines of flat `key: value` handling rather than a
new npm dependency; `package.json` has no YAML parser today and this does not
justify adding one.

### Index, feed and search

* **Index** — a single static HTML page, all posts, newest first, three-up card
  grid with the image on top. No pagination and no infinite-scroll JS: the markup
  for a thousand posts is cheap, streams, and works with JavaScript disabled.
  Images carry `loading="lazy"`.
* **Feed** — Atom 1.0 at `/blog/atom.xml`, automatically geenrated, no dependency.
  Atom rather than RSS 2.0 because it is an actual specification: mandated
  `updated` timestamps and `xml:base`, so dates and relative media URLs resolve
  the same way in every reader.
* **Search** — a second BM25 index alongside the docs one, reusing
  `gen_search_index.js` and the existing client-side ranking wholesale. Search
  from a blog page returns only posts; from a docs page, only docs.

### Card images

Every post gets an image, because a wall of undifferentiated text is a wall.
If `post.md` references an image, the first one becomes the card image.
Otherwise the build generates one, in the flat, text-free, Google-palette
illustration language already used by `src/assets/ui.png` and friends.

Generated covers are **SVG, not PNG**: vector, so they stay sharp on any
display; ~2 KB each; and emitted by ~200 lines of string concatenation, with no
rasteriser and so no new dependency. A PNG would mean adding an image library to
a `package.json` that today has `marked`, `ejs` and `sass` and little else, and
would bake in a fixed resolution.

The picture is a pure function of the **hash of the title**. From that hash we
pick, in order:

* one of five **motifs**, each drawn from something Perfetto actually shows you
  — nested slice tracks, a flame graph, scheduler lanes, async flows, sampled
  columns;
* **two hues**, the first always chromatic so nothing comes out all-grey;
* the shape variation within that motif — segment counts, widths, lane density.

The two-hue limit is the part that matters. Earlier drafts drew from the whole
palette at once and the results read as confetti at card size; constraining each
image to two hues at several brightnesses makes it read as a diagram instead.
Neutral greys are still used for baselines and lane rules, which are structure
rather than colour.

Text is never rendered into an image. Baking the title into artwork — as several
comparable blogs do — duplicates the headline sitting directly beneath it and
turns an index page into visual noise.

The generator runs **once, at post-creation time**, and its output is committed
into the post directory alongside `post.md`. Regenerating on every build would
mean retitling a post silently changed the cover of something already published;
committing it also makes the artwork reviewable in the same pull request as the
prose.

### What we are deliberately not building

Tags, categories, reading-time estimates, a featured-post slot, a newsletter,
comments, and translations. Each can be added later if the volume ever justifies
it; none should be built before there is content to organise. Categories are the
most likely first addition.

**Also, no dark mode.** Not an oversight. Dark mode is a poor default for
long-form prose: a dark field dilates the pupil, which widens the eye's aperture,
degrades depth of field and amplifies optical aberration, so light glyphs bloom
into the background — worse at small sizes, and worst for the third to half of
adults with astigmatism. The positive-polarity advantage is well established:
Buchner & Baumgartner (2007, *Ergonomics*) found proofreading consistently better
with dark-on-light independent of ambient lighting; Piepenbrock et al. (2014)
measured pupil size directly and confirmed the causal chain; MIT AgeLab's Dobres
et al. (2017) found legibility worst of all for light-on-dark in dark rooms.
Readers cannot subjectively detect the deficit. Dark mode is fine for chrome,
UI, code and video — for a wall of prose it trades measurable legibility for an
aesthetic. The trace viewer can keep it; the blog will not have it.

## Alternatives considered

### Posts in the main repo under `docs/`

Pro:

* Zero new infrastructure; works today.
* One checkout, one review process.

Con:

* Every post is a code change against `main`, with code-review latency.
* Post media accretes in the main branch forever, for everyone who clones.
* Conflates reference documentation with dated, opinionated prose, which is
  exactly the confusion that produced two case studies filed as docs.

### `refs/extra/blog` instead of a branch

Pro:

* Outside the default refspec, so it costs nothing to anyone cloning for code.

Con:

* GitHub cannot open pull requests against a non-branch ref, which rules out
  guest posts and external typo fixes.
* No web-UI browsing or editing.
* Every contributor and Cloud Build must add a refspec by hand.
* The saving is theoretical: the repository already carries 541 `dev/*` branches
  and 237 MB of objects, of which `docs/images` alone is 52 MB.

### Keeping the date in the URL

Pro:

* Slugs are unique by construction; no collision check needed.
* Sortable and self-documenting.

Con:

* Correcting a publish date changes the permalink.
* Posts visibly age in the link, which discourages linking to evergreen material.

### Media inside the post directory in production

Pro:

* Source and output layouts would match exactly.

Con:

* Impossible without either a trailing-slash URL scheme (and a matching change to
  the App Engine proxy, which is deployed independently and rarely), or a
  file/directory collision in the output tree.

### A third-party static site generator (Jekyll, Hugo, Astro)

Pro:

* Blog features — feeds, pagination, taxonomies — come for free.
* Well-trodden; contributors may already know it.

Con:

* A second, parallel build system for one section of an existing site.
* We would lose the shared renderer, and with it the callouts, mermaid, tabs,
  code-copy buttons and dead-link checking that make Perfetto markdown what it is.
* `infra/perfetto.dev` is already a markdown-to-static-HTML generator. The
  marginal cost of teaching it about a second content root is smaller than the
  cost of running two generators.

### Hotlinking GitHub avatars

Pro:

* No build-time network access; always current.

Con:

* Sends every reader's IP address to a third party on every page view.
* Breaks if GitHub ever blocks hotlinking.

Fetching once at build time into `/blog/media/authors/` keeps the published page
free of third-party requests entirely.
