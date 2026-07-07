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
//   --full  <md>       Hand-written docs: title, headings and body are indexed.
//   --gen   <url>=<md> Generated reference pages (SQL tables, stdlib, protos):
//                      title and headings only, as their bodies are huge.
//
// A --full doc's URL is derived from its path relative to --doc-root, mirroring
// the out_html mapping in BUILD.gn (docs/foo/bar.md -> /docs/foo/bar,
// README.md -> /docs/).

const fs = require("fs");
const path = require("path");
const marked = require("marked");
const argv = require("yargs").argv;
const {headingAnchor} = require("./md_anchors");

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
        case "code":  // fenced/indented code, raw HTML, spacers: not searchable text.
        case "html":
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

  for (const spec of toArray(argv["gen"])) {
    const eq = spec.indexOf("=");
    const url = spec.slice(0, eq);
    const mdPath = spec.slice(eq + 1);
    const parsed = parseMarkdown(fs.readFileSync(mdPath, "utf8"));
    docs.push(withNav({
      u: url,
      t: parsed.title || url,
      h: parsed.headings,
      // No body: these pages are huge (trace-packet-proto alone has ~2k
      // headings), so for v1 we index title + headings only -- enough to find a
      // proto message, stdlib package or table section by name, but not their
      // field-level bodies (descriptions, columns, args).
      // TODO: full-text indexing them costs ~+207KB gzipped (+58%) and, worse,
      // lets these dumps dominate generic queries ("duration", "config") since
      // their prose is full of common words. Doing it well likely needs a
      // separate, lazily-fetched reference index rather than folding into this one.
    }));
  }

  docs.sort((a, b) => a.u.localeCompare(b.u)); // Deterministic output.
  fs.mkdirSync(path.dirname(outFile), {recursive: true});
  fs.writeFileSync(outFile, JSON.stringify({docs}));
}

main();
