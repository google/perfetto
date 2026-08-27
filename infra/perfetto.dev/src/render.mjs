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

// Renders the docs markdown into HTML.
//
// This used to be a one-process-per-page CLI (markdown_render.js) driven by a
// GN `md_to_html` template. It is now a module: build.mjs renders every page in
// a single process. The two consequences worth knowing about:
//
//  - No module-level mutable state. Each render gets its own context, so pages
//    can be rendered concurrently without leaking `title` into each other.
//  - Images referenced by a page are *returned* (ctx.assets) rather than copied
//    to disk as a side effect. That list used to be serialized into a ninja
//    depfile so ninja could re-render a page when an image changed; in-process
//    it is just a return value that feeds the next build's cache signature.

import fs from "node:fs";
import path from "node:path";
import ejs from "ejs";
import { Renderer, marked } from "marked";
import hljs from "highlight.js";
import { headingAnchor } from "./md_utils.mjs";

const CS_BASE_URL = "https://github.com/google/perfetto/blob/main/";

const SRC_DIR = path.dirname(new URL(import.meta.url).pathname);
export const ROOT_DIR = path.dirname(path.dirname(path.dirname(SRC_DIR)));

// fs.existsSync() memo for the dead-link checker. A doc links to hundreds of
// targets and watch mode re-renders often, so this saves a lot of stat()s.
// Cleared by resetLinkCache() whenever the watcher sees a change.
const existsCache = new Map();

function existsCached(absPath) {
  let v = existsCache.get(absPath);
  if (v === undefined) {
    v = fs.existsSync(absPath);
    existsCache.set(absPath, v);
  }
  return v;
}

export function resetLinkCache() {
  existsCache.clear();
}

// Creates the mutable per-render state. `mdFile` is the absolute path of the
// markdown being rendered; it anchors relative link resolution.
export function newContext(mdFile) {
  return {
    mdFile,
    title: "",
    // sitePath -> absolute source path, for every image the page references.
    assets: new Map(),
  };
}

function hrefInDocs(ctx, href) {
  if (href.match(/^(https?:)|^(mailto:)|^#/)) {
    return undefined;
  }
  let pathFromRoot;
  if (href.startsWith("/")) {
    pathFromRoot = href;
  } else {
    const curDocDir = "/" + path.relative(ROOT_DIR, path.dirname(ctx.mdFile));
    pathFromRoot = path.join(curDocDir, href);
  }
  if (pathFromRoot.startsWith("/docs/")) {
    return pathFromRoot;
  }
  return undefined;
}

function assertNoDeadLink(ctx, relPathFromRoot) {
  relPathFromRoot = relPathFromRoot.replace(/\#.*$/g, ""); // Remove #line.

  // Skip check for build-time generated reference pages.
  if (relPathFromRoot.endsWith(".autogen")) return;

  const fullPath = path.join(ROOT_DIR, relPathFromRoot);
  if (!existsCached(fullPath) && !existsCached(fullPath + ".md")) {
    // The source file isn't named here: build.mjs prefixes it, and marked
    // re-wraps whatever we throw.
    throw new Error(`Dead link: ${relPathFromRoot}`);
  }
}

function renderHeading(ctx, text, level) {
  // The anchor id is derived by headingAnchor() (shared with search_index.mjs
  // so search deep-links stay in sync). Note the right-hand-side TOC is
  // dynamically generated from these anchors (explicit or implicit).
  if (level === 1 && !ctx.title) {
    ctx.title = text;
  }
  const anchorId = headingAnchor(text, level);
  // Strip an explicit {#anchor} marker from the visible heading text.
  text = text.replace(/{#[\w-_.]+}/, "");
  let anchor = "";
  if (anchorId) {
    anchor = `<a name="${anchorId}" class="anchor" href="#${anchorId}"></a>`;
  }
  return `<h${level}>${anchor}${text}</h${level}>`;
}

function renderLink(ctx, originalLinkFn, href, title, text) {
  if (href.startsWith("../")) {
    throw new Error(
      `Don\'t use relative paths in docs, always use /docs/xxx ` +
        `or /src/xxx for both links to docs and code (${href})`,
    );
  }
  const docsHref = hrefInDocs(ctx, href);
  let sourceCodeLink = undefined;
  if (docsHref !== undefined) {
    // Check that the target doc exists. Skip the check on /reference/ files
    // that are typically generated at build time.
    assertNoDeadLink(ctx, docsHref);
    href = docsHref.replace(/[.](md|autogen)\b/, "");
    href = href.replace(/\/README$/, "/");
  } else if (href.startsWith("/") && !href.startsWith("//")) {
    // /tools/xxx -> github/tools/xxx.
    sourceCodeLink = href;
  }
  if (sourceCodeLink !== undefined) {
    // Fix up line anchors for GitHub link: #42 -> #L42.
    sourceCodeLink = sourceCodeLink.replace(/#(\d+)$/g, "#L$1");

    // Strip the / prefix from the link, as CS_BASE_URL already endsin '/'.
    sourceCodeLink = sourceCodeLink.replace(/^[/]/,'');
    assertNoDeadLink(ctx, sourceCodeLink);
    href = CS_BASE_URL + sourceCodeLink;
  }
  return originalLinkFn(href, title, text);
}

function renderCode(text, lang) {
  if (lang === "mermaid") {
    return `<div class="mermaid">${text}</div>`;
  }

  let hlHtml = "";
  if (lang) {
    // ignoreIllegals is passed explicitly: it defaulted to false in the
    // deprecated highlight(lang, code) form this replaced, but defaults to true
    // in the object form. Verified to produce byte-identical output site-wide.
    hlHtml = hljs.highlight(text, {
      language: lang,
      ignoreIllegals: false,
    }).value;
  } else {
    hlHtml = hljs.highlightAuto(text).value;
  }
  // Wrap in a positioned container so a "copy" button can be overlaid in the
  // top-right corner (see setupCodeCopy() in script.js).
  return (
    `<div class="code-block-wrapper">` +
    `<button class="code-copy-button" type="button" aria-label="Copy code" ` +
    `title="Copy to clipboard"></button>` +
    `<code class="hljs code-block">${hlHtml}</code>` +
    `</div>`
  );
}

function renderImage(ctx, originalImgFn, href, title, text) {
  const docsHref = hrefInDocs(ctx, href);
  if (docsHref !== undefined) {
    // Record it rather than copying: build.mjs owns the output map, and this
    // list doubles as the page's dynamic dependency set (the old depfile).
    ctx.assets.set(docsHref.replace(/^\//, ""), ROOT_DIR + docsHref);
  }
  if (href.endsWith(".svg")) {
    return `<object type="image/svg+xml" data="${href}"></object>`;
  }
  return originalImgFn(href, title, text);
}

function renderListItem(text) {
  // Detect a trailing {.class1 .class2} attribute block (used in toc.md to
  // annotate audience, e.g. {.tag-android .tag-linux}).  Hoist the classes
  // onto the <li> so the sidebar can filter with pure CSS.
  const m = text.match(
    /\s*\{(\.[a-z][a-z0-9-]*(?:\s+\.[a-z][a-z0-9-]*)*)\}(\s*<\/p>)?\s*$/,
  );
  if (m) {
    const cls = m[1]
      .split(/\s+/)
      .map((c) => c.slice(1))
      .join(" ");
    const tail = m[2] || "";
    return `<li class="${cls}">${text.slice(0, m.index)}${tail}</li>\n`;
  }
  return `<li>${text}</li>\n`;
}

function renderParagraph(text) {
  let cssClass = "";
  if (text.startsWith("NOTE:")) {
    cssClass = "note";
  } else if (text.startsWith("TIP:")) {
    cssClass = "tip";
  } else if (text.startsWith("TODO:") || text.startsWith("FIXME:")) {
    cssClass = "todo";
  } else if (text.startsWith("WARNING:")) {
    cssClass = "warning";
  } else if (text.startsWith("Summary:")) {
    cssClass = "summary";
  }
  if (cssClass != "") {
    cssClass = ` class="callout ${cssClass}"`;
  }

  // Rudimentary support of definition lists.
  var colonStart = text.search("\n:");
  if (colonStart != -1) {
    var key = text.substring(0, colonStart);
    var value = text.substring(colonStart + 2);
    return `<dl><dt><p>${key}</p></dt><dd><p>${value}</p></dd></dl>`;
  }

  return `<p${cssClass}>${text}</p>\n`;
}

function renderHtml(ctx, originalHtmlFn, raw) {
  if (!raw.trim().startsWith("<?tabs>")) {
    return originalHtmlFn(raw);
  }
  const sanitized = raw.replace("<?tabs>", "").replace("</tabs?>", "");
  const tabs = sanitized
    .split("TAB: ")
    .map((x) => x.trim())
    .filter((x) => x.length !== 0);
  const buttons = [];
  const content = [];
  for (const tab of tabs) {
    const eol = tab.indexOf("\n");
    buttons.push(tab.substring(0, eol));
    // Recurses with the same ctx, so nested links/images are resolved against
    // the same source file and land in the same asset set.
    content.push(renderMarkdown(tab.substring(eol + 1), ctx));
  }
  return `
    <div class="tab-box">
      <div class="tab-buttons">
      ${buttons
        .map((x) => `<button class="tab-button">${x}</button>`)
        .join("\n")}
      </div>
      ${content
        .map((x) => `<div class="tab-content"><p>${x}</p></div>`)
        .join("\n")}
    </div>
  `;
}

// Renders markdown to HTML, accumulating title/assets into `ctx`.
export function renderMarkdown(rawMarkdown, ctx) {
  const renderer = new Renderer();
  const originalLinkFn = renderer.link.bind(renderer);
  const originalImgFn = renderer.image.bind(renderer);
  renderer.link = (hr, ti, te) => renderLink(ctx, originalLinkFn, hr, ti, te);
  renderer.image = (hr, ti, te) => renderImage(ctx, originalImgFn, hr, ti, te);
  renderer.code = renderCode;
  renderer.heading = (text, level) => renderHeading(ctx, text, level);
  renderer.paragraph = renderParagraph;
  renderer.listitem = renderListItem;
  const originalHtmlFn = renderer.html.bind(renderer);
  renderer.html = (html) => renderHtml(ctx, originalHtmlFn, html);

  return marked.parse(rawMarkdown, { renderer: renderer });
}

// EJS templates are compiled once and reused across all ~150 pages.
const templateCache = new Map();

export function compiledTemplate(templatePath) {
  let fn = templateCache.get(templatePath);
  if (fn === undefined) {
    const src = fs.readFileSync(templatePath, "utf8");
    fn = ejs.compile(src, { filename: templatePath });
    templateCache.set(templatePath, fn);
  }
  return fn;
}

export function resetTemplateCache() {
  templateCache.clear();
}

const FALLBACK_TITLE =
  "Perfetto - System profiling, app tracing and trace analysis";

// Renders one page. Returns {html, assets, title}.
//   markdown     raw markdown source, or null for template-only pages (index).
//   mdFile       absolute path the markdown came from (for link resolution).
//   templatePath EJS template, or null to emit the bare markdown HTML (_nav).
//   sitePath     output path relative to the site root, e.g. "docs/faq".
//   nav          the rendered _nav.html fragment, or undefined.
export function renderPage({ markdown, mdFile, templatePath, sitePath, nav }) {
  const ctx = newContext(mdFile);
  const markdownHtml = markdown === null ? "" : renderMarkdown(markdown, ctx);

  if (!templatePath) {
    return { html: markdownHtml, assets: ctx.assets, title: ctx.title };
  }

  const templateData = {
    markdown: markdownHtml,
    title: ctx.title ? `${ctx.title} - Perfetto Tracing Docs` : FALLBACK_TITLE,
    fileName: "/" + sitePath,
  };
  if (nav !== undefined) {
    templateData["nav"] = nav;
  }
  const html = compiledTemplate(templatePath)(templateData);
  return { html, assets: ctx.assets, title: ctx.title };
}
