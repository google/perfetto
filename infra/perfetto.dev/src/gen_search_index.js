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

"use strict";

// Builds the client-side full-text search index (search_index.json) consumed by
// setupSearch() in assets/script.js.
//
// Inputs (wired up in BUILD.gn):
//   --full     <md>   Hand-written docs: title, headings and body are indexed.
//   --gen-dir  <dir>  Generated reference pages (SQL tables, stdlib, protos):
//                     every *.md is indexed by title + headings only (bodies are
//                     huge), with its URL from toc.md's `.autogen` entry.
//
// A --full doc's URL is derived from its path relative to --doc-root, mirroring
// the out_html mapping in BUILD.gn (docs/foo/bar.md -> /docs/foo/bar,
// README.md -> /docs/).

const fs = require("fs");
const path = require("path");
const marked = require("marked");
const argv = require("yargs").argv;
const {headingAnchor} = require("./md_utils");

// Normalizes a yargs option that may be absent, a single value, or (for a
// repeated flag) an array, into a plain array.
function toArray(v) {
  if (v === undefined) return [];
  return Array.isArray(v) ? v : [v];
}

// Strips markdown/inline syntax down to readable plain text for indexing and
// snippets.
function stripInline(s) {
  return s
    .replace(/{[#.][^}]*}/g, "")             // {#anchor} and {.tag-foo} attributes.
    .replace(/!\[[^\]]*\]\([^)]*\)/g, " ")   // Images.
    .replace(/\[([^\]]*)\]\([^)]*\)/g, "$1") // Links -> link text.
    .replace(/`([^`]*)`/g, "$1")             // Inline code.
    .replace(/[*_~]+/g, "")                  // Emphasis markers.
    .replace(/\s+/g, " ")
    .trim();
}

// Extracts {title, headings, body} from a Markdown doc. We tokenize with
// marked's block lexer -- the same parser markdown_render.js renders the page
// with -- so the indexer and the page agree on what is a heading vs. code, and
// heading anchors line up for deep-links even around gnarly constructs like
// fenced code inside a list.
function parseMarkdown(md) {
  let title = "";
  const headings = []; // {t, a}
  const bodyParts = [];
  const pushText = (s) => { const t = stripInline(s); if (t) bodyParts.push(t); };

  const walk = (tokens) => {
    for (const tok of tokens) {
      switch (tok.type) {
        case "heading": {
          const text = stripInline(tok.text);
          if (tok.depth === 1 && !title) {
            title = text;
          } else if (text) {
            // Anchor from the inline-rendered heading, matching what the page
            // emits so deep-links land, e.g. "Using `foo`" -> "using-code-foo-code".
            headings.push({t: text, a: headingAnchor(marked.parseInline(tok.text), tok.depth)});
          }
          if (text) bodyParts.push(text);
          break;
        }
        case "table":
          for (const cell of tok.header) pushText(cell.text);
          for (const row of tok.rows) for (const cell of row) pushText(cell.text);
          break;
        case "list":
          walk(tok.items);
          break;
        case "list_item":
        case "blockquote":
          walk(tok.tokens);
          break;
        case "code":  // Index code too: people search config keys and API names.
          pushText(tok.text || "");
          break;
        case "html":  // Raw HTML and blank/rule tokens carry no prose.
        case "space":
        case "hr":
          break;
        default:      // paragraph, loose text, etc.
          pushText(tok.text || "");
      }
    }
  };
  walk(marked.lexer(md));
  return {title, headings, body: bodyParts.join(" ")};
}

function urlForFullDoc(mdPath, docRoot) {
  let rel = path.relative(docRoot, mdPath).split(path.sep).join("/");
  rel = rel.replace(/\.md$/, "");
  if (rel === "README") return "/docs/";
  return "/docs/" + rel;
}

// Maps doc URL -> its nav label from toc.md. These curated labels ("Boot
// Tracing") are often what people search, while the on-page H1 is a fuller
// sentence ("Recording traces on Android boot"), so they're worth indexing.
function parseTocLabels(tocPath) {
  const map = new Map();
  const md = fs.readFileSync(tocPath, "utf8");
  const re = /\[([^\]]+)\]\(([^)]+?\.md)\)/g; // [Label](relative/path.md)
  let m;
  while ((m = re.exec(md)) !== null) {
    const label = stripInline(m[1]);
    const rel = m[2].replace(/^\.\//, "").replace(/\.md$/, "");
    const url = rel === "README" ? "/docs/" : "/docs/" + rel;
    if (label) map.set(url, label);
  }
  return map;
}

// Slug -> URL for generated pages, from toc.md's ".autogen" links -- where a
// globbed .md, which carries no URL of its own, gets one.
function parseGenUrls(tocPath) {
  const map = new Map();
  const md = fs.readFileSync(tocPath, "utf8");
  const re = /\[[^\]]+\]\(([^)]+?)\.autogen\)/g;
  let m;
  while ((m = re.exec(md)) !== null) {
    const rel = m[1].replace(/^\.\//, "");     // "reference/trace-config-proto"
    map.set(rel.split("/").pop(), "/docs/" + rel); // slug -> url
  }
  return map;
}

// A generated .md's filename to its toc slug, e.g. gen_trace_config_proto.md ->
// "trace-config-proto", stdlib_docs.md -> "stdlib-docs".
function genFileSlug(file) {
  return file.replace(/\.md$/, "").replace(/^gen_/, "").replace(/_/g, "-");
}

// Lowercase alphanumeric tokens, keeping a trailing "++"/"#" so "c++" and "c#"
// stay searchable. Tokens shorter than 2 chars are dropped as noise.
function searchTokenize(str) {
  const out = [];
  const re = /[a-z0-9]+(\+\+|#)?/g;
  let m;
  while ((m = re.exec(str.toLowerCase())) !== null) {
    if (m[0].length >= 2) out.push(m[0]);
  }
  return out;
}

// Builds the BM25 inverted index here rather than in the browser, which would
// otherwise tokenize every doc on load. Field boosts: a hit in the title matters
// far more than one in the body. Returns:
//   terms       -- sorted unique tokens
//   post        -- parallel to terms; each a flat [docIdx, weight, ...] array
//   docLen      -- per-doc total weighted token count (BM25 length normalization)
//   titleTokens -- per-doc tokenized title, for the title boost
//   navTokens   -- per-doc tokenized nav label, or null; also for the title boost
function buildInvertedIndex(docs) {
  const FIELD_BOOST = {title: 8, heading: 4, body: 1};
  const postings = new Map();  // token -> Map(docIdx -> weight)
  const docLen = new Array(docs.length).fill(0);
  const titleTokens = new Array(docs.length);
  const navTokens = new Array(docs.length).fill(null);
  const addTokens = (i, tokens, boost) => {
    for (const tok of tokens) {
      let postingList = postings.get(tok);
      if (postingList === undefined) {
        postingList = new Map();
        postings.set(tok, postingList);
      }
      postingList.set(i, (postingList.get(i) || 0) + boost);
      docLen[i] += boost;
    }
  };
  for (let i = 0; i < docs.length; i++) {
    const d = docs[i];
    titleTokens[i] = searchTokenize(d.t || "");
    addTokens(i, titleTokens[i], FIELD_BOOST.title);
    // The toc.md nav label (d.n) is a curated keyword alias -- index at title
    // weight. Same for the URL slug (last path segment, e.g. "perfetto-cli").
    if (d.n) {
      navTokens[i] = searchTokenize(d.n);
      addTokens(i, navTokens[i], FIELD_BOOST.title);
    }
    const slug = d.u.split("/").filter(Boolean).pop() || "";
    addTokens(i, searchTokenize(slug), FIELD_BOOST.title);
    for (const h of d.h || []) {
      addTokens(i, searchTokenize(h.t), FIELD_BOOST.heading);
    }
    if (d.b) {
      addTokens(i, searchTokenize(d.b), FIELD_BOOST.body);
    }
  }
  const terms = [...postings.keys()].sort();
  const post = terms.map((t) => {
    const flat = [];
    for (const [docIdx, weight] of postings.get(t)) {
      flat.push(docIdx, weight);
    }
    return flat;
  });
  return {terms, post, docLen, titleTokens, navTokens};
}

function main() {
  const outFile = argv["out"];
  const docRoot = argv["doc-root"];
  if (!outFile) throw new Error("Missing --out");
  const navLabels = argv["toc"] ? parseTocLabels(argv["toc"]) : new Map();
  const docs = [];

  // Attaches the toc.md nav label as `n`, unless it just repeats the title.
  const withNav = (doc) => {
    const nav = navLabels.get(doc.u);
    if (nav && nav !== doc.t) doc.n = nav;
    return doc;
  };

  for (const mdPath of toArray(argv["full"])) {
    const url = urlForFullDoc(mdPath, docRoot);
    // These build to HTML but aren't real search targets: the docsify cover
    // image and the agent-facing contributor guides.
    if (url === "/docs/_coverpage" || url.startsWith("/docs/AGENTS")) continue;
    const parsed = parseMarkdown(fs.readFileSync(mdPath, "utf8"));
    if (!parsed.title) continue; // Skip empty redirect stubs (src/empty.md).
    docs.push(withNav({u: url, t: parsed.title, h: parsed.headings, b: parsed.body}));
  }

  // Generated reference pages: index title + headings only (bodies are huge).
  // The URL comes from toc.md; a .md with no `.autogen` entry there isn't a page.
  // TODO: indexing their bodies too grows the gzipped index ~60% and lets these
  // dumps dominate generic queries -- so it'd want a separate, lazily-loaded index.
  const genDir = argv["gen-dir"];
  if (genDir) {
    const genUrls = parseGenUrls(argv["toc"]);
    for (const file of fs.readdirSync(genDir)) {
      if (!file.endsWith(".md")) continue;
      const url = genUrls.get(genFileSlug(file));
      if (!url) continue;
      const parsed = parseMarkdown(fs.readFileSync(path.join(genDir, file), "utf8"));
      docs.push(withNav({u: url, t: parsed.title || url, h: parsed.headings}));
    }
  }

  docs.sort((a, b) => a.u.localeCompare(b.u)); // Deterministic output.
  const index = buildInvertedIndex(docs);
  fs.mkdirSync(path.dirname(outFile), {recursive: true});
  fs.writeFileSync(outFile, JSON.stringify({docs, ...index}));
}

main();
