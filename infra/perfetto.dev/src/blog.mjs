// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Everything the blog needs that the docs don't: reading posts off the `blog`
// branch worktree, their front matter, the generated cover artwork, the Atom
// feed and the author avatars.
//
// Posts live in //blog, an orphan-branch worktree (see ensureBlogWorktree in
// build.mjs). One directory per post, named YYYY-MM-DD-slug, holding post.md
// and its images. The date orders the feed and renders the byline but is
// dropped from the URL, so fixing a wrong date never breaks a permalink; the
// images are hoisted to /blog/media/<slug>/ so that /blog/<slug> can stay a
// plain extensionless file the way the docs pages are.

import * as fs from "node:fs";
import * as path from "node:path";
import { PNG } from "pngjs";

const pjoin = path.join;

// ---------------------------------------------------------------------------
// Posts.
// ---------------------------------------------------------------------------

const POST_DIR_RE = /^(\d{4})-(\d{2})-(\d{2})-([a-z0-9]+(?:-[a-z0-9]+)*)$/;

// Front matter is a `---` delimited block of flat `key: value` pairs. It is
// deliberately not YAML: three keys do not justify a YAML parser in a
// package.json that hasn't got one. Values run to end of line, so a colon or a
// '#' inside a summary is safe.
const REQUIRED = ["title", "author", "summary"];

export function parseFrontMatter(raw, srcForErrors) {
  const where = srcForErrors ? ` in ${srcForErrors}` : "";
  const lines = raw.split("\n");
  if (lines[0].trim() !== "---") {
    throw new Error(
      `Missing front matter${where}: the file must start with ---`,
    );
  }
  const fields = {};
  let i = 1;
  for (; i < lines.length; i++) {
    if (lines[i].trim() === "---") break;
    if (lines[i].trim() === "") continue;
    const m = /^([a-z][a-z0-9_]*):[ \t]*(.*)$/.exec(lines[i]);
    if (!m) {
      throw new Error(
        `Bad front matter line${where}: ${JSON.stringify(lines[i])}. ` +
          `Expected \`key: value\`.`,
      );
    }
    if (m[1] in fields) {
      throw new Error(`Duplicate front matter key '${m[1]}'${where}`);
    }
    fields[m[1]] = m[2].trim();
  }
  if (i >= lines.length) {
    throw new Error(`Unterminated front matter${where}: missing closing ---`);
  }
  for (const k of REQUIRED) {
    if (!fields[k]) {
      throw new Error(`Front matter${where} is missing required key '${k}'`);
    }
  }
  // Authors are @-prefixed GitHub handles, matching rfcs/template.md, so the
  // byline and the avatar lookup cannot disagree about what the value is.
  const authors = fields.author.split(/\s+/);
  for (const a of authors) {
    if (!/^@[A-Za-z0-9](?:[A-Za-z0-9]|-(?=[A-Za-z0-9])){0,38}$/.test(a)) {
      throw new Error(
        `Bad author${where}: ${JSON.stringify(a)}. ` +
          `Expected an @-prefixed GitHub handle, e.g. '@primiano'.`,
      );
    }
  }
  fields.authors = authors.map((a) => a.slice(1));
  return { fields, body: lines.slice(i + 1).join("\n") };
}

// The card image: the first image the body references, else a cover generated
// from the title. Pure -- it touches no disk and cannot fail.
export function coverFor(slug, body) {
  // Videos are written with image syntax (see renderImage), but a card is an
  // <img> -- picking one would render nothing and push a multi-megabyte file
  // onto the index. Take the first still image instead.
  const re = /!\[[^\]]*\]\(\s*([^)\s]+)/g;
  let m;
  while ((m = re.exec(body)) !== null) {
    const file = m[1];
    if (/^(https?:)|^\//.test(file)) continue;
    if (/\.(mp4|webm)$/i.test(file)) continue;
    // renderImage() already records anything the body references.
    return { sitePath: `blog/media/${slug}/${file}`, generated: false };
  }
  return { sitePath: `blog/media/${slug}/cover.svg`, generated: true };
}

// Author avatars are committed as authors/<handle>.png in the blog branch (see
// tools/fetch_avatar). Anyone without one falls back to a single checked-in
// placeholder, which keeps the build offline and off the network.
// github.com/<handle>.png serves whatever the user uploaded, so
// tools/fetch_avatars commits either a .png or a .jpg.
const AVATAR_EXTS = ["png", "jpg"];

// Bylines that are not GitHub accounts. Release announcements are the project
// speaking, not a person, and there is no github.com/perfetto-team to link to
// -- so the handle renders as plain text and its avatar is committed by hand.
const NON_GITHUB_AUTHORS = new Set(["perfetto-team"]);

export function avatarUrl(blogDir, handle) {
  for (const ext of AVATAR_EXTS) {
    const committed = pjoin(blogDir, "authors", `${handle}.${ext}`);
    if (fs.existsSync(committed)) {
      return {
        url: `/blog/media/authors/${handle}.${ext}`,
        copyFrom: committed,
      };
    }
  }
  return { url: "/assets/default-avatar.png", copyFrom: null };
}

// Reads every post directory. Anything malformed throws: a broken post should
// fail the build loudly, not vanish silently from the index.
export function collectPosts(blogDir) {
  if (!fs.existsSync(blogDir)) return [];
  const posts = [];
  const bySlug = new Map();
  for (const dirName of fs.readdirSync(blogDir).sort()) {
    const dir = pjoin(blogDir, dirName);
    if (!fs.statSync(dir).isDirectory()) continue;
    const mdPath = pjoin(dir, "post.md");
    if (!fs.existsSync(mdPath)) continue; // e.g. the authors/ directory.

    const m = POST_DIR_RE.exec(dirName);
    if (!m) {
      throw new Error(
        `Post directory '${dirName}' must be named YYYY-MM-DD-slug with a ` +
          `real date and a lowercase hyphenated slug.`,
      );
    }
    const [, y, mo, d, slug] = m;
    const date = new Date(Date.UTC(+y, +mo - 1, +d));
    if (
      date.getUTCFullYear() !== +y ||
      date.getUTCMonth() !== +mo - 1 ||
      date.getUTCDate() !== +d
    ) {
      throw new Error(`Post directory '${dirName}' has an invalid date.`);
    }
    // The date is not in the URL, so two posts a year apart can still collide.
    const clash = bySlug.get(slug);
    if (clash) {
      throw new Error(
        `'${dirName}' and '${clash}' both publish at /blog/${slug}. ` +
          `The date is not part of the URL, so slugs must be unique.`,
      );
    }
    bySlug.set(slug, dirName);

    const raw = fs.readFileSync(mdPath, "utf8");
    const { fields, body } = parseFrontMatter(raw, mdPath);
    const cover = coverFor(slug, body);
    const thumb = thumbnailFor(cover);
    posts.push({
      dir,
      mdPath,
      slug,
      body,
      title: fields.title,
      summary: fields.summary,
      authors: fields.authors.map((handle) => ({
        handle,
        url: NON_GITHUB_AUTHORS.has(handle)
          ? null
          : `https://github.com/${handle}`,
        avatar: avatarUrl(blogDir, handle).url,
      })),
      isoDate: `${y}-${mo}-${d}`,
      sortKey: date.getTime(),
      cover,
      // Where a thumbnail would go, or null if this cover cannot have one.
      // addBlogThumbnails() decides whether one was actually produced and
      // upgrades cardUrl; until then the card points at the full image, so a
      // cover that turns out to be untouchable degrades rather than 404s.
      thumbSitePath: thumb,
      cardUrl: "/" + cover.sitePath,
    });
  }
  posts.sort((a, b) => b.sortKey - a.sortKey); // Newest first.
  return posts;
}

// ---------------------------------------------------------------------------
// Index thumbnails.
//
// Cards are 320px wide but the covers are full-resolution screenshots -- v55's
// is half a megabyte on its own. Downscaling them for the index turns ~1MB of
// cover art into ~100KB, which matters on a page that shows every post.
//
// Only PNG is handled, which is what GitHub release screenshots are. Anything
// else (or anything pngjs cannot decode) falls back to the full-size image, so
// an unusual cover degrades to a slower card rather than a broken build.
// ---------------------------------------------------------------------------

// 2x the 320px card, so the thumbnail still looks right on a HiDPI screen.
const THUMB_WIDTH = 640;

export function thumbnailFor(cover) {
  if (!/\.png$/i.test(cover.sitePath)) return null;
  const name = cover.sitePath.slice(cover.sitePath.lastIndexOf("/") + 1);
  const slug = cover.sitePath.split("/")[2];
  return `blog/media/thumbnails/${slug}-${name}`;
}

// Box-filter downscale. Averaging every source pixel that lands in a
// destination pixel avoids the aliasing a naive nearest-neighbour pick gives
// on screenshots full of thin UI lines.
export function makeThumbnail(srcAbsPath) {
  let src;
  try {
    src = PNG.sync.read(fs.readFileSync(srcAbsPath));
  } catch (e) {
    return null; // Not decodable; caller falls back to the full image.
  }
  if (src.width <= THUMB_WIDTH) return null; // Already small enough.

  const w = THUMB_WIDTH;
  const h = Math.max(1, Math.round((src.height * w) / src.width));
  const dst = new PNG({ width: w, height: h });
  const xr = src.width / w;
  const yr = src.height / h;

  for (let y = 0; y < h; y++) {
    const y0 = Math.floor(y * yr);
    const y1 = Math.min(src.height, Math.max(y0 + 1, Math.floor((y + 1) * yr)));
    for (let x = 0; x < w; x++) {
      const x0 = Math.floor(x * xr);
      const x1 = Math.min(src.width, Math.max(x0 + 1, Math.floor((x + 1) * xr)));
      let r = 0, g = 0, b = 0, a = 0, n = 0;
      for (let sy = y0; sy < y1; sy++) {
        for (let sx = x0; sx < x1; sx++) {
          const i = (src.width * sy + sx) << 2;
          r += src.data[i];
          g += src.data[i + 1];
          b += src.data[i + 2];
          a += src.data[i + 3];
          n++;
        }
      }
      const o = (w * y + x) << 2;
      dst.data[o] = r / n;
      dst.data[o + 1] = g / n;
      dst.data[o + 2] = b / n;
      dst.data[o + 3] = a / n;
    }
  }
  return PNG.sync.write(dst, { deflateLevel: 9 });
}

// ---------------------------------------------------------------------------
// Atom feed.
// ---------------------------------------------------------------------------

const SITE = "https://perfetto.dev";

const xml = (s) =>
  String(s)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");

export function atomFeed(posts) {
  // <updated> is the newest post's date, never a build timestamp: otherwise
  // every build would emit a different file and churn the deploy.
  const updated = posts.length
    ? `${posts[0].isoDate}T00:00:00Z`
    : "1970-01-01T00:00:00Z";
  const entries = posts
    .map(
      (p) => `  <entry>
    <title>${xml(p.title)}</title>
    <link href="${SITE}/blog/${p.slug}"/>
    <id>${SITE}/blog/${p.slug}</id>
    <updated>${p.isoDate}T00:00:00Z</updated>
    <published>${p.isoDate}T00:00:00Z</published>
${p.authors
  .map(
    (a) =>
      `    <author><name>@${xml(a.handle)}</name>` +
      (a.url ? `<uri>${xml(a.url)}</uri>` : "") +
      `</author>`,
  )
  .join("\n")}
    <summary>${xml(p.summary)}</summary>
  </entry>`,
    )
    .join("\n");

  return `<?xml version="1.0" encoding="utf-8"?>
<feed xmlns="http://www.w3.org/2005/Atom" xml:base="${SITE}/">
  <title>Perfetto Blog</title>
  <subtitle>Release notes, performance investigations and deep dives from the people who build Perfetto.</subtitle>
  <link rel="self" href="${SITE}/blog/atom.xml"/>
  <link rel="alternate" type="text/html" href="${SITE}/blog/"/>
  <id>${SITE}/blog/</id>
  <updated>${updated}</updated>
${entries}
</feed>
`;
}

// ---------------------------------------------------------------------------
// Cover artwork.
//
// Everything is a pure function of the title's hash: the motif, the two hues
// and the shape variation. Same title in, same picture out.
//
// Deliberate constraint: at most TWO hues per image, used at several
// brightnesses. Drawing from the whole palette at once reads as confetti at
// card size; two hues with a brightness ramp reads as a diagram.
//
// Covers are generated during the build, not committed. Retitling a post
// therefore changes its cover, which is an accepted trade for having no
// generated artefacts in the blog branch and no extra step when writing.
//
// Flat, text-free, same visual language as the hand-drawn illustrations in
// src/assets/. No text is ever drawn into the image.
// ---------------------------------------------------------------------------

const W = 320,
  H = 180;
const GROUND = "#f1f3f4"; // Matches the ground of the existing illustrations.
const RULE = "#e1e3e5"; // Lane/baseline rules. Structure, not colour.

// Brightness ramps, darkest first, from the Google palette already used by the
// site's illustrations, so generated art sits next to them cleanly.
const RAMPS = {
  blue: ["#1a73e8", "#4285f4", "#8ab4f8", "#c2dbff"],
  red: ["#c5221f", "#ea4335", "#f28b82", "#fad2cf"],
  yellow: ["#ea8600", "#fbbc04", "#fdd663", "#feefc3"],
  green: ["#137333", "#34a853", "#81c995", "#ceead6"],
  teal: ["#0e7c86", "#12a4b0", "#6fd0d9", "#b8e8ec"],
  purple: ["#7627bb", "#a142f4", "#c58af9", "#e9d2fc"],
  grey: ["#3c4043", "#5f6368", "#9aa0a6", "#dadce0"],
};
// The first hue is always chromatic, so no post gets an all-grey cover.
const CHROMATIC = ["blue", "red", "yellow", "green", "teal", "purple"];
const SECONDARY = CHROMATIC.concat(["grey"]);

function fnv(s) {
  let h = 2166136261 >>> 0;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 16777619) >>> 0;
  }
  return h;
}

function rng(seed) {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

const rect = (x, y, w, h, fill, rx) =>
  w <= 0
    ? ""
    : `<rect x="${x.toFixed(1)}" y="${y.toFixed(1)}" width="${w.toFixed(1)}" ` +
      `height="${h.toFixed(1)}" rx="${rx === undefined ? 2.5 : rx}" fill="${fill}"/>`;

// Each motif is drawn from something Perfetto actually shows you. `c.a` and
// `c.b` are the two brightness ramps, darkest first.
const MOTIFS = {
  // Nested slice track: the shape of a TrackEvent trace.
  slices(rnd, c) {
    let s = "";
    const pad = 26,
      top = 30,
      rowH = 20,
      gap = 7;
    for (let row = 0; row < 5; row++) {
      const y = top + row * (rowH + gap);
      let x = pad + row * rnd() * 10;
      const end = W - pad - row * rnd() * 14;
      const ramp = row % 2 ? c.b : c.a;
      while (x < end - 8) {
        let w = Math.max(10, (end - x) * (0.22 + rnd() * 0.55));
        if (x + w > end) w = end - x;
        s += rect(x, y, w, rowH, ramp[row === 0 ? 0 : rnd() > 0.55 ? 1 : 2]);
        x += w + 4;
      }
    }
    return s;
  },

  // Flame graph: the shape of a profile.
  flame(rnd, c) {
    let s = "";
    const pad = 24,
      rowH = 18,
      step = 24;
    let segs = [[pad, W - pad * 2]];
    for (let row = 0; row < 6 && segs.length; row++) {
      const y = H - 18 - rowH - row * step;
      const ramp = row % 2 ? c.b : c.a;
      const next = [];
      for (const seg of segs) {
        const x = seg[0],
          w = seg[1];
        if (w < 14) continue;
        s += rect(
          x,
          y,
          w,
          rowH,
          ramp[Math.min(3, (row >> 1) + (rnd() > 0.6 ? 1 : 0))],
        );
        const n = 1 + ((rnd() * 2.4) | 0);
        let cx = x;
        const avail = w * (0.55 + rnd() * 0.4);
        for (let i = 0; i < n; i++) {
          const cw = avail / n - 4;
          if (cw > 12) next.push([cx + 3, cw]);
          cx += cw + 7;
        }
      }
      segs = next;
    }
    return s;
  },

  // Scheduler lanes: threads running, blocked, preempted.
  sched(rnd, c) {
    let s = "";
    const pad = 24,
      top = 26,
      laneH = 16,
      step = 26;
    for (let l = 0; l < 6; l++) {
      const y = top + l * step;
      if (y + laneH > H - 4) break;
      s += rect(pad, y + laneH / 2 - 1, W - pad * 2, 2, RULE, 1);
      let x = pad + rnd() * 16;
      const ramp = l % 2 ? c.b : c.a;
      while (x < W - pad - 10) {
        let w = 8 + rnd() * 46;
        if (x + w > W - pad) w = W - pad - x;
        s += rect(x, y, w, laneH, ramp[rnd() > 0.45 ? 1 : 2]);
        x += w + 6 + rnd() * 26;
      }
    }
    return s;
  },

  // Async flows: work crossing threads, processes or machines.
  flows(rnd, c) {
    let s = "";
    const pad = 26,
      barH = 18;
    const rows = [46, 92, 138];
    const anchors = [];
    rows.forEach((y, i) => {
      let x = pad + rnd() * 20;
      const ramp = i % 2 ? c.b : c.a;
      const row = [];
      while (x < W - pad - 20) {
        let w = 26 + rnd() * 54;
        if (x + w > W - pad) w = W - pad - x;
        s += rect(x, y, w, barH, ramp[i === 1 ? 2 : 1]);
        row.push([x, x + w]);
        x += w + 14 + rnd() * 30;
      }
      anchors.push(row);
    });
    for (let i = 0; i < rows.length - 1; i++) {
      const a = anchors[i],
        b = anchors[i + 1];
      for (let j = 0; j < Math.min(a.length, b.length); j++) {
        const x1 = a[j][1] - 4,
          y1 = rows[i] + barH;
        const x2 = b[j][0] + 6,
          y2 = rows[i + 1];
        s +=
          `<path d="M${x1.toFixed(1)},${y1} C${x1.toFixed(1)},${y1 + 16} ` +
          `${x2.toFixed(1)},${y2 - 16} ${x2.toFixed(1)},${y2}" fill="none" ` +
          `stroke="${c.a[2]}" stroke-width="2"/>`;
        s += `<circle cx="${x2.toFixed(1)}" cy="${y2}" r="3" fill="${c.a[1]}"/>`;
      }
    }
    return s;
  },

  // Sampled columns: how big, how fast, how often.
  columns(rnd, c) {
    const pad = 24,
      baseline = H - 32,
      n = 16,
      gap = 4;
    const bw = (W - pad * 2 - gap * (n - 1)) / n;
    let s = rect(pad, baseline, W - pad * 2, 3, RULE, 1.5);
    for (let i = 0; i < n; i++) {
      const x = pad + i * (bw + gap);
      let y = baseline;
      const segs = 1 + ((rnd() * 3) | 0);
      for (let k = 0; k < segs; k++) {
        const h = 12 + rnd() * 34;
        if (y - h < 24) break;
        y -= h;
        const ramp = k % 2 ? c.b : c.a;
        s += rect(x, y, bw, h - 2, ramp[k === 0 ? 1 : 2], 2);
      }
    }
    return s;
  },
};
const MOTIF_NAMES = Object.keys(MOTIFS);

// Returns {svg, motif, hues} for a post title.
export function generateCover(title) {
  const h = fnv(title);
  const rnd = rng(h);
  const motif = MOTIF_NAMES[h % MOTIF_NAMES.length];
  const aName = CHROMATIC[(h >>> 8) % CHROMATIC.length];
  const others = SECONDARY.filter((n) => n !== aName);
  const bName = others[(h >>> 16) % others.length];
  const colors = { a: RAMPS[aName], b: RAMPS[bName] };
  const svg =
    `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${W} ${H}" ` +
    `width="${W}" height="${H}" role="img" aria-hidden="true">` +
    `<rect width="${W}" height="${H}" fill="${GROUND}"/>` +
    MOTIFS[motif](rnd, colors) +
    `</svg>\n`;
  return { svg, motif, hues: [aName, bName] };
}
