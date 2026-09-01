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

// Builds the client-side full-text search index (search_index.json.gz) consumed
// by setupSearch() in assets/script.js.
//
// Hand-written docs are indexed in full; the huge auto-generated reference pages
// (SQL tables, stdlib, protos) are indexed by title + headings only to keep the
// shipped index small.
//
// This used to be a CLI that globbed the generated-markdown directory and
// reverse-engineered each page's URL from its filename via toc.md's `.autogen`
// entries -- which is why BUILD.gn had to force it to run last, and why editing
// a generated page's source needed a clean build to re-index. build.mjs now
// hands it the in-memory page list with URLs already attached, so both problems
// are gone.

import zlib from "node:zlib";
import { lexer, parseInline } from "marked";
import { headingAnchor } from "./md_utils.mjs";

// Strips markdown/inline syntax down to readable plain text for indexing and
// snippets.
function stripInline(s) {
  return s
    .replace(/{[#.][^}]*}/g, "") // {#anchor} and {.tag-foo} attributes.
    .replace(/!\[[^\]]*\]\([^)]*\)/g, " ") // Images.
    .replace(/\[([^\]]*)\]\([^)]*\)/g, "$1") // Links -> link text.
    .replace(/`([^`]*)`/g, "$1") // Inline code.
    .replace(/[*_~]+/g, "") // Emphasis markers.
    .replace(/\s+/g, " ")
    .trim();
}

// Extracts {title, headings, body} from a Markdown doc. We tokenize with
// marked's block lexer -- the same parser render.mjs renders the page with -- so
// the indexer and the page agree on what is a heading vs. code, and heading
// anchors line up for deep-links even around gnarly constructs like fenced code
// inside a list.
function parseMarkdown(md) {
  let title = "";
  const headings = []; // {t, a}
  const bodyParts = [];
  const pushText = (s) => {
    const t = stripInline(s);
    if (t) bodyParts.push(t);
  };

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
            headings.push({
              t: text,
              a: headingAnchor(parseInline(tok.text), tok.depth),
            });
          }
          if (text) bodyParts.push(text);
          break;
        }
        case "table":
          for (const cell of tok.header) pushText(cell.text);
          for (const row of tok.rows)
            for (const cell of row) pushText(cell.text);
          break;
        case "list":
          walk(tok.items);
          break;
        case "list_item":
        case "blockquote":
          walk(tok.tokens);
          break;
        case "code": // Index code too: people search config keys and API names.
          pushText(tok.text || "");
          break;
        case "html": // Raw HTML and blank/rule tokens carry no prose.
        case "space":
        case "hr":
          break;
        default: // paragraph, loose text, etc.
          pushText(tok.text || "");
      }
    }
  };
  walk(lexer(md));
  return { title, headings, body: bodyParts.join(" ") };
}

// Maps doc URL -> its nav label from toc.md. These curated labels ("Boot
// Tracing") are often what people search, while the on-page H1 is a fuller
// sentence ("Recording traces on Android boot"), so they're worth indexing.
function parseTocLabels(tocMd) {
  const map = new Map();
  // Deliberately .md only: the generated reference pages are linked as
  // `.autogen` and have never carried a nav label in the index.
  const re = /\[([^\]]+)\]\(([^)]+?\.md)\)/g; // [Label](relative/path.md)
  let m;
  while ((m = re.exec(tocMd)) !== null) {
    const label = stripInline(m[1]);
    const rel = m[2].replace(/^\.\//, "").replace(/\.md$/, "");
    const url = rel === "README" ? "/docs/" : "/docs/" + rel;
    if (label) map.set(url, label);
  }
  return map;
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
  const FIELD_BOOST = { title: 8, heading: 4, body: 1 };
  const postings = new Map(); // token -> Map(docIdx -> weight)
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
  return { terms, post, docLen, titleTokens, navTokens };
}

// True for pages that build to HTML but aren't real search targets: the docsify
// cover image and the agent-facing contributor guides.
export function isIndexable(url) {
  return url !== "/docs/_coverpage" && !url.startsWith("/docs/AGENTS");
}

// Lexes one document into its indexable parts. Split out from the index build
// so build.mjs can memoize it per document on content hash -- re-lexing all
// ~150 docs to reindex after a one-word edit was the slowest thing left in the
// incremental path.
//
// `full` indexes the body too; otherwise only title + headings are indexed
// (used for the huge generated reference dumps).
// TODO: indexing the generated pages' bodies too grows the gzipped index ~60%
// and lets these dumps dominate generic queries -- so it'd want a separate,
// lazily-loaded index.
export function parseSearchDoc(url, markdown, full) {
  const parsed = parseMarkdown(markdown);
  if (full && !parsed.title) return null; // Skip empty redirect stubs.
  const doc = { u: url, t: parsed.title || url, h: parsed.headings };
  if (full) doc.b = parsed.body;
  return doc;
}

// parsedDocs: parseSearchDoc() results with nulls already filtered out.
export function assembleSearchIndex(parsedDocs, tocMd) {
  const navLabels = parseTocLabels(tocMd);
  const docs = [];
  for (const parsed of parsedDocs) {
    // Copy: parsed docs are memoized, so they must not accumulate an `n` field
    // across builds.
    const doc = { ...parsed };
    const nav = navLabels.get(doc.u);
    if (nav && nav !== doc.t) doc.n = nav;
    docs.push(doc);
  }

  docs.sort((a, b) => a.u.localeCompare(b.u)); // Deterministic output.
  const index = buildInvertedIndex(docs);
  // The docs-site proxy serves this file uncompressed, so gzip it here and let
  // script.js inflate it. Level 9: built once, so size wins over speed.
  const json = JSON.stringify({ docs, ...index });
  return zlib.gzipSync(json, { level: 9 });
}
