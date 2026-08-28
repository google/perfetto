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

// Builds the perfetto.dev docs website.
//
//   docs/**/*.md ---------------\
//   docs/toc.md -> _nav.html ----+
//   *.proto  -> gen_proto -------+--> marked + custom renderers + EJS --> site/
//   stats.h  -> gen_stats -------+
//   *.sql    -> stdlib docs -----+
//   tables/*.py -> sql tables ---/
//
// Incrementality is a content-hash memo table (memo() below. The one dynamic
// dependency -- the images a page references -- is just a value returned by
//  renderPage().

import argparse from "argparse";
import child_process from "node:child_process";
import crypto from "node:crypto";
import fs from "node:fs";
import http from "node:http";
import path from "node:path";
import * as sass from "sass";

import {
  ROOT_DIR,
  renderPage,
  resetLinkCache,
  resetTemplateCache,
} from "./render.mjs";
import { genProtoMd, genSqlTablesMd, genStatsMd } from "./generators.mjs";
import {
  assembleSearchIndex,
  isIndexable,
  parseSearchDoc,
} from "./search_index.mjs";

const pjoin = path.join;
const CUR_DIR = path.dirname(new URL(import.meta.url).pathname);
const SRC_DIR = CUR_DIR;
const DOCS_DIR = pjoin(ROOT_DIR, "docs");
const GEN_SCRATCH = "gen"; // Subdir of outDir for python intermediates.

// Exit code that asks the `build` wrapper script to re-exec us. Used when our
// own sources change in watch mode: ESM modules can't be un-imported, and
// re-execing avoids stacking up a parent process per edit.
const EXIT_RESTART = 75;

const cfg = {
  watch: false,
  verbose: false,
  startHttpServer: false,
  port: 8082,
  host: "0.0.0.0",
  outDir: pjoin(ROOT_DIR, "out/perfetto.dev"),
};

// Pages that were removed/renamed/moved. They still build to (empty) HTML so
// the URL 200s and the redirectMap in src/assets/script.js can bounce it.
const REMOVED_RENAMED_MOVED = [
  "analysis/common-queries.md",
  "analysis/pivot-tables.md",
  "case-studies/android-boot-tracing.md",
  "case-studies/android-outofmemoryerror.md",
  "contributing/embedding.md",
  "contributing/perfetto-in-the-press.md",
  "contributing/ui-development.md",
  "quickstart/android-tracing.md",
  "quickstart/callstack-sampling.md",
  "quickstart/chrome-tracing.md",
  "quickstart/heap-profiling.md",
  "quickstart/linux-tracing.md",
  "quickstart/trace-analysis.md",
];

// Directories whose contents feed the build, watched in --watch mode.
const WATCH_DIRS = [
  "docs",
  "infra/perfetto.dev/src",
  "protos",
  "python",
  "src/trace_processor/perfetto_sql/stdlib",
  "src/trace_processor/storage",
  "src/trace_processor/tables",
];

// ---------------------------------------------------------------------------
// Content-hash memoization.
// ---------------------------------------------------------------------------

const memoCache = new Map(); // key -> {sig, value}
let hashCache = new Map(); // abs path -> sha1, cleared at the start of a build.
let memoStats = { hit: 0, miss: 0 };

function hashFile(absPath) {
  let h = hashCache.get(absPath);
  if (h === undefined) {
    try {
      const sha1Hasher = crypto.createHash("sha1");
      h = sha1Hasher.update(fs.readFileSync(absPath)).digest("hex");
    } catch (e) {
      h = "<missing>";
    }
    hashCache.set(absPath, h);
  }
  return h;
}

// A dep is either {file: absPath} (hashed by content) or any literal value
// (stringified). Literals let a step depend on an upstream step's *result*
// rather than on files, e.g. the search index depends on the rendered pages.
function sigOf(deps) {
  const h = crypto.createHash("sha1");
  for (const d of deps) {
    if (d !== null && typeof d === "object" && d.file !== undefined) {
      h.update(hashFile(d.file));
    } else {
      h.update(String(d));
    }
    h.update("\0");
  }
  return h.digest("hex");
}

async function memo(key, deps, fn) {
  const sig = sigOf(deps);
  const hit = memoCache.get(key);
  if (hit !== undefined && hit.sig === sig) {
    memoStats.hit++;
    return hit.value;
  }
  memoStats.miss++;
  const value = await fn(hit === undefined ? undefined : hit.value);
  memoCache.set(key, { sig, value });
  return value;
}

// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------

function listFilesRecursive(dir, filterFn) {
  let out = [];
  let entries;
  try {
    entries = fs.readdirSync(dir, { withFileTypes: true });
  } catch (e) {
    return out;
  }
  for (const e of entries.sort((a, b) => a.name.localeCompare(b.name))) {
    const full = pjoin(dir, e.name);
    if (e.isDirectory()) {
      out = out.concat(listFilesRecursive(full, filterFn));
    } else if (!filterFn || filterFn(full)) {
      out.push(full);
    }
  }
  return out;
}

function ensureDir(dirPath) {
  fs.mkdirSync(dirPath, { recursive: true });
  return dirPath;
}

let progressShown = false;

// Per-item build log. On a TTY it rewrites a single line in place. With
// --verbose, or when stdout is not a TTY (CI logs, `build > log`), it prints one
// line per item instead -- \r would just concatenate them into one unreadable
// row there, and a real paper trail is the point.
function progress(msg) {
  if (cfg.verbose || !process.stdout.isTTY) {
    console.log(`  ${msg}`);
    return;
  }
  const w = Math.max((process.stdout.columns || 80) - 1, 20);
  const line = msg.length > w ? msg.slice(0, w - 1) + "\u2026" : msg.padEnd(w);
  process.stdout.write(`\r${line}`);
  progressShown = true;
}

// Wipes the progress line so the next console.log starts on a clean row.
function progressClear() {
  if (!progressShown) return;
  const w = Math.max((process.stdout.columns || 80) - 1, 20);
  process.stdout.write(`\r${" ".repeat(w)}\r`);
  progressShown = false;
}

function exec(cmd, args, opts) {
  opts = opts || {};
  if (cfg.verbose) console.log(`${cmd} ${args.join(" ")}`);
  const res = child_process.spawnSync(cmd, args, {
    cwd: ROOT_DIR,
    stdio: ["ignore", opts.stdout || "inherit", "inherit"],
  });
  if (res.status !== 0 && !opts.noErrCheck) {
    throw new Error(`${cmd} ${args.join(" ")} failed with code ${res.status}`);
  }
  return res;
}

// ---------------------------------------------------------------------------
// The build.
// ---------------------------------------------------------------------------

// Returns Map<sitePath, value>, where value is a string/Buffer to write, or
// {copyFrom: absPath} for files copied verbatim (images, pngs, node assets).
// Keeping images as references rather than buffers keeps ~59MB out of RAM.
async function build() {
  hashCache = new Map();
  memoStats = { hit: 0, miss: 0 };
  resetLinkCache();
  // Templates are compiled once per build and reused across all ~150 pages;
  // dropping the cache here means editing an EJS template just works in watch
  // mode without restarting.
  resetTemplateCache();

  const genDir = ensureDir(pjoin(cfg.outDir, GEN_SCRATCH));
  const site = new Map();

  // The nav is rendered from docs/toc.md with no template, and every page
  // inlines it.
  const tocPath = pjoin(DOCS_DIR, "toc.md");
  progress("docs/_nav.html");
  const nav = await memo(
    "nav",
    [{ file: tocPath }],
    () =>
      renderPage({
        markdown: fs.readFileSync(tocPath, "utf8"),
        mdFile: tocPath,
        templatePath: null,
        sitePath: "docs/_nav.html",
      }).html,
  );
  site.set("docs/_nav.html", nav);

  const pages = collectPages(await buildGeneratedMarkdown(genDir), tocPath);
  await renderAllPages(pages, nav, site);
  await addStylesheet(site);
  addStaticAssets(site);
  site.set(
    "assets/search_index.json.gz",
    await buildSearchIndex(pages, tocPath),
  );
  return site;
}

// Enumerates every page the site is made of: the landing page, the docs
// landing page, one per docs/**/*.md, the redirect stubs, and the generated
// reference pages. The three gen-time exec_script(glob.py) calls BUILD.gn used
// -- which is why adding a doc needed a fresh `gn gen` -- are just a directory
// walk here.
function collectPages(genPages, tocPath) {
  const tmplMarkdown = pjoin(SRC_DIR, "template_markdown.html");
  const tmplIndex = pjoin(SRC_DIR, "template_index.html");
  const pages = [];

  pages.push({
    key: "index",
    markdown: null,
    mdFile: tmplIndex,
    templatePath: tmplIndex,
    sitePath: "index.html",
  });
  pages.push({
    key: "readme",
    markdown: fs.readFileSync(pjoin(DOCS_DIR, "README.md"), "utf8"),
    mdFile: pjoin(DOCS_DIR, "README.md"),
    templatePath: tmplMarkdown,
    sitePath: "docs/index.html",
    searchUrl: "/docs/",
  });

  const mdFiles = listFilesRecursive(DOCS_DIR, (f) => f.endsWith(".md")).filter(
    (f) => f !== pjoin(DOCS_DIR, "README.md") && f !== tocPath,
  );
  for (const f of mdFiles) {
    const rel = path.relative(DOCS_DIR, f).replace(/\.md$/, "");
    pages.push({
      key: `md:${rel}`,
      markdown: fs.readFileSync(f, "utf8"),
      mdFile: f,
      templatePath: tmplMarkdown,
      sitePath: `docs/${rel}`,
      searchUrl: `/docs/${rel}`,
    });
  }
  for (const old of REMOVED_RENAMED_MOVED) {
    const rel = old.replace(/\.md$/, "");
    pages.push({
      key: `stub:${rel}`,
      markdown: "",
      mdFile: pjoin(SRC_DIR, "empty.md"),
      templatePath: tmplMarkdown,
      sitePath: `docs/${rel}`,
    });
  }
  for (const g of genPages) {
    pages.push({
      key: `gen:${g.name}`,
      markdown: g.markdown,
      mdFile: g.mdFile,
      templatePath: tmplMarkdown,
      sitePath: g.sitePath,
      searchUrl: g.searchUrl,
      searchFull: false,
    });
  }
  return pages;
}

// Renders every page into `site`. Dead links and bad relative paths throw;
// failures are collected rather than fatal at the first one, so a single build
// reports every broken page and watch mode can keep serving the last good one.
async function renderAllPages(pages, nav, site) {
  const templateDeps = [
    { file: pjoin(SRC_DIR, "template_markdown.html") },
    { file: pjoin(SRC_DIR, "template_index.html") },
    { file: pjoin(SRC_DIR, "template_header.html") },
    { file: pjoin(SRC_DIR, "template_footer.html") },
  ];
  const errors = [];

  const rendered = await Promise.all(
    pages.map(async (p, i) => {
      progress(`[${i + 1}/${pages.length}] ${p.sitePath}`);
      // The images a page references are a *dynamic* dependency, discovered
      // only by rendering it. The cache entry remembers them, and the signature
      // stored after a render already includes them, so a page with images is a
      // cache hit on the very next build rather than always missing once.
      const key = `page:${p.key}`;
      const staticDeps = [
        p.markdown,
        p.sitePath,
        p.templatePath,
        ...templateDeps,
        nav,
      ];
      const asDeps = (files) => files.map((f) => ({ file: f }));
      const prev = memoCache.get(key);
      if (prev !== undefined) {
        if (sigOf([...staticDeps, ...asDeps(prev.imgFiles)]) === prev.sig) {
          memoStats.hit++;
          return prev.value;
        }
      }
      memoStats.miss++;
      try {
        const value = renderPage({
          markdown: p.markdown,
          mdFile: p.mdFile,
          templatePath: p.templatePath,
          sitePath: p.sitePath,
          nav,
        });
        const imgFiles = [...value.assets.values()].sort();
        memoCache.set(key, {
          sig: sigOf([...staticDeps, ...asDeps(imgFiles)]),
          value,
          imgFiles,
        });
        return value;
      } catch (e) {
        memoCache.delete(key);
        // marked appends "Please report this to github.com/markedjs/marked" to
        // anything a custom renderer throws. Ours throw on dead links and bad
        // relative paths, which are the author's bugs, not marked's -- so strip
        // the invitation to go file an upstream issue.
        const msg = e.message.replace(
          /\s*Please report this to https:\/\/github\.com\/markedjs\/marked\.?\s*$/,
          "",
        );
        errors.push(`${path.relative(ROOT_DIR, p.mdFile)}: ${msg}`);
        return null;
      }
    }),
  );

  if (errors.length > 0) {
    const err = new Error(
      `${errors.length} page(s) failed to render:\n  ` + errors.join("\n  "),
    );
    err.isDocsError = true; // A content bug: print it without a JS stack.
    throw err;
  }

  for (let i = 0; i < pages.length; i++) {
    site.set(pages[i].sitePath, rendered[i].html);
    for (const [sitePath, srcAbs] of rendered[i].assets) {
      site.set(sitePath, { copyFrom: srcAbs });
    }
  }
}

async function addStylesheet(site) {
  const scss = pjoin(SRC_DIR, "assets/style.scss");
  // cfg.outDir is a dependency: the source map's paths are relative to the
  // stylesheet's output directory.
  progress("assets/style.css");
  const css = await memo("css", [{ file: scss }, cfg.outDir], () => {
    const res = sass.compile(scss, {
      sourceMap: true,
      sourceMapIncludeSources: false,
      logger: sass.Logger.silent,
    });
    // `sources` must be relative to the directory the .css is written to --
    // that is how a browser resolves them from the map's own URL. dart-sass's
    // JS API hands us absolute file: URLs, so rebase them the way the sass CLI
    // (which the old build shelled out to) did. Key order matches too, so the
    // output is byte-identical to the pre-migration build.
    const cssDir = pjoin(cfg.outDir, "site", "assets");
    const map = JSON.stringify({
      version: res.sourceMap.version,
      sourceRoot: res.sourceMap.sourceRoot || "",
      sources: res.sourceMap.sources.map((s) =>
        path.relative(cssDir, new URL(s).pathname),
      ),
      names: res.sourceMap.names,
      mappings: res.sourceMap.mappings,
      file: "style.css",
    });
    return {
      css: res.css + "\n\n/*# sourceMappingURL=style.css.map */\n",
      map,
    };
  });
  site.set("assets/style.css", css.css);
  site.set("assets/style.css.map", css.map);
}

// src/assets/*.{png,js} plus the two files vendored out of node_modules.
function addStaticAssets(site) {
  const assetDir = pjoin(SRC_DIR, "assets");
  for (const f of listFilesRecursive(
    assetDir,
    (f) => f.endsWith(".png") || f.endsWith(".js"),
  )) {
    progress(`assets/${path.basename(f)}`);
    site.set(`assets/${path.basename(f)}`, { copyFrom: f });
  }
  for (const f of [
    "../node_modules/highlight.js/styles/tomorrow-night.css",
    "../node_modules/mermaid/dist/mermaid.min.js",
  ]) {
    progress(`assets/${path.basename(f)}`);
    site.set(`assets/${path.basename(f)}`, { copyFrom: pjoin(CUR_DIR, f) });
  }
}

// The index takes the in-memory page list,
// Each document is lexed under its own memo key, so a one-word edit re-lexes
// one document rather than all ~150; only the cheap inverted-index assembly
// re-runs.
async function buildSearchIndex(pages, tocPath) {
  const parsedDocs = await Promise.all(
    pages
      .filter((p) => p.searchUrl !== undefined && isIndexable(p.searchUrl))
      .map((p) => {
        const md = p.markdown || "";
        const full = p.searchFull !== false;
        return memo(`sidoc:${p.searchUrl}`, [md, full], () =>
          parseSearchDoc(p.searchUrl, md, full),
        );
      }),
  );
  const indexable = parsedDocs.filter((d) => d !== null);
  progress("assets/search_index.json.gz");
  const tocMd = fs.readFileSync(tocPath, "utf8");
  return memo(
    "search",
    [...indexable.map((d) => JSON.stringify(d)), { file: tocPath }],
    () => assembleSearchIndex(indexable, tocMd),
  );
}

// Runs the reference-doc generators, each memoized on its own inputs.
async function buildGeneratedMarkdown(genDir) {
  const out = [];

  // -- Trace Processor stats, parsed out of stats.h.
  const statsH = pjoin(ROOT_DIR, "src/trace_processor/storage/stats.h");
  out.push({
    name: "sql-stats",
    markdown: await memo("gen:stats", [{ file: statsH }], () =>
      genStatsMd([statsH]),
    ),
    mdFile: pjoin(genDir, "sql-stats.md"),
    sitePath: "docs/analysis/sql-stats",
    searchUrl: "/docs/analysis/sql-stats",
  });

  // -- Proto references. protobufjs parses the .proto source directly; no
  // protoc, no descriptor set, no C++ build involved.
  for (const [name, proto, message, sitePath] of [
    [
      "trace-config-proto",
      "protos/perfetto/config/trace_config.proto",
      "perfetto.protos.TraceConfig",
      "docs/reference/trace-config-proto",
    ],
    [
      "trace-packet-proto",
      "protos/perfetto/trace/trace_packet.proto",
      "perfetto.protos.TracePacket",
      "docs/reference/trace-packet-proto",
    ],
  ]) {
    const protoAbs = pjoin(ROOT_DIR, proto);
    // Protos import each other, so depend on the whole tree: it is only ~5MB to
    // hash and it means an edit to any transitively-included proto is picked up.
    const protoDeps = listFilesRecursive(pjoin(ROOT_DIR, "protos"), (f) =>
      f.endsWith(".proto"),
    ).map((f) => ({ file: f }));
    out.push({
      name,
      markdown: await memo(`gen:${name}`, protoDeps, () =>
        genProtoMd(protoAbs, message),
      ),
      mdFile: pjoin(genDir, `${name}.md`),
      sitePath,
      searchUrl: `/${sitePath}`,
    });
  }

  // -- PerfettoSQL prelude tables. tools/gen_tp_table_docs.py is pure python
  // over the *_tables.py sources; the GN target that used to produce this JSON
  // existed only to pass it that same file list.
  const tableSrcs = listFilesRecursive(
    pjoin(ROOT_DIR, "src/trace_processor/tables"),
    (f) => f.endsWith("_tables.py"),
  );
  const tablesJson = pjoin(genDir, "tables_python_docs.json");
  const tpTableDocsPy = pjoin(ROOT_DIR, "tools/gen_tp_table_docs.py");
  out.push({
    name: "sql-tables",
    markdown: await memo(
      "gen:sql-tables",
      [...tableSrcs.map((f) => ({ file: f })), { file: tpTableDocsPy }],
      () => {
        exec("python3", [
          tpTableDocsPy,
          "--out",
          tablesJson,
          ...tableSrcs,
          "--relative-input-dir",
          ROOT_DIR,
        ]);
        return genSqlTablesMd([tablesJson]);
      },
    ),
    mdFile: pjoin(genDir, "sql-tables.md"),
    sitePath: "docs/analysis/sql-tables",
    searchUrl: "/docs/analysis/sql-tables",
  });

  // -- PerfettoSQL standard library. Two python steps: the .sql files become
  // JSON, the JSON becomes markdown. ui/build.mjs already calls the first one
  // the same way -- with a plain recursive listing rather than the GN metadata
  // walk that used to collect these paths.
  const stdlibDir = pjoin(ROOT_DIR, "src/trace_processor/perfetto_sql/stdlib");
  const sqlFiles = listFilesRecursive(stdlibDir, (f) => f.endsWith(".sql"));
  const stdlibJson = pjoin(genDir, "stdlib_docs.json");
  const stdlibMd = pjoin(genDir, "stdlib_docs.md");
  const stdlibJsonPy = pjoin(ROOT_DIR, "tools/gen_stdlib_docs_json.py");
  const stdlibMdPy = pjoin(SRC_DIR, "gen_stdlib_docs_md.py");
  out.push({
    name: "stdlib-docs",
    markdown: await memo(
      "gen:stdlib",
      [
        ...sqlFiles.map((f) => ({ file: f })),
        { file: stdlibJsonPy },
        { file: stdlibMdPy },
      ],
      () => {
        exec("python3", [stdlibJsonPy, "--json-out", stdlibJson, ...sqlFiles]);
        exec("python3", [
          stdlibMdPy,
          "--input",
          stdlibJson,
          "--output",
          stdlibMd,
        ]);
        return fs.readFileSync(stdlibMd, "utf8");
      },
    ),
    mdFile: stdlibMd,
    sitePath: "docs/analysis/stdlib-docs",
    searchUrl: "/docs/analysis/stdlib-docs",
  });

  return out;
}

// ---------------------------------------------------------------------------
// Writing to disk.
// ---------------------------------------------------------------------------

// Mirrors `site` into <outDir>/site: writes what changed, deletes what's gone.
// The old build rm -rf'd the whole out dir on every invocation; syncing instead
// means `--watch --out` doesn't rewrite 59MB of images on every keystroke.
function writeSite(site) {
  const siteDir = ensureDir(pjoin(cfg.outDir, "site"));
  const wanted = new Set(site.keys());

  for (const abs of listFilesRecursive(siteDir)) {
    const rel = path.relative(siteDir, abs).split(path.sep).join("/");
    if (!wanted.has(rel)) fs.rmSync(abs);
  }

  let written = 0;
  for (const [rel, val] of site) {
    const dst = pjoin(siteDir, rel);
    ensureDir(path.dirname(dst));
    if (val !== null && typeof val === "object" && val.copyFrom !== undefined) {
      const src = fs.statSync(val.copyFrom);
      let dstStat = null;
      try {
        dstStat = fs.statSync(dst);
      } catch (e) {
        /* not there yet */
      }
      if (
        dstStat &&
        dstStat.size === src.size &&
        dstStat.mtimeMs >= src.mtimeMs
      ) {
        continue;
      }
      fs.copyFileSync(val.copyFrom, dst);
    } else {
      const buf = Buffer.isBuffer(val) ? val : Buffer.from(val, "utf8");
      let cur = null;
      try {
        cur = fs.readFileSync(dst);
      } catch (e) {
        /* not there yet */
      }
      if (cur !== null && cur.equals(buf)) continue;
      fs.writeFileSync(dst, buf);
    }
    written++;
  }
  return written;
}

// ---------------------------------------------------------------------------
// Dev server: static files out of the in-memory map + SSE live reload.
// ---------------------------------------------------------------------------

const MIME = {
  css: "text/css",
  gz: "application/gzip",
  js: "application/javascript",
  json: "application/json",
  map: "application/json",
  png: "image/png",
  svg: "image/svg+xml",
};

function mimeFor(sitePath) {
  const ext = sitePath.includes(".") ? sitePath.split(".").pop() : "";
  // Doc pages are deliberately extension-less (/docs/analysis/sql-tables), so
  // the fallthrough is text/html. Production relies on the same assumption via
  // the mime_util/file shim that gsutil uses.
  return MIME[ext] || "text/html";
}

// Injected into HTML responses in --serve mode only, so the templates and the
// deployed site stay free of dev-only markup.
const LIVE_RELOAD_JS = `
<script>
(function() {
  var es = new EventSource('/live_reload');
  var overlayId = '__perfetto_build_error';
  function clearOverlay() {
    var el = document.getElementById(overlayId);
    if (el) el.remove();
  }
  function showOverlay(msg) {
    clearOverlay();
    var el = document.createElement('pre');
    el.id = overlayId;
    el.textContent = 'Build failed:\\n\\n' + msg;
    el.style.cssText = 'position:fixed;inset:0;z-index:2147483647;margin:0;' +
      'padding:24px;background:#2b1a1a;color:#ffb4b4;font:13px/1.5 monospace;' +
      'white-space:pre-wrap;overflow:auto';
    document.body.appendChild(el);
  }
  es.onmessage = function(e) {
    var m = JSON.parse(e.data);
    if (m.kind === 'error') { showOverlay(m.msg); return; }
    if (m.kind === 'css') {
      clearOverlay();
      var l = document.querySelector('link[href*="/assets/style.css"]');
      if (l) l.href = l.href.split('?')[0] + '?' + Date.now();
      return;
    }
    try {
      sessionStorage.setItem('__perfetto_scroll', String(window.scrollY));
    } catch (e) {}
    location.reload();
  };
  window.addEventListener('load', function() {
    try {
      var y = sessionStorage.getItem('__perfetto_scroll');
      if (y !== null) {
        sessionStorage.removeItem('__perfetto_scroll');
        window.scrollTo(0, parseInt(y, 10));
      }
    } catch (e) {}
  });
})();
</script>
`;

const sseClients = new Set();
let currentSite = new Map(); // Last successful build, served by the dev server.

function notifyClients(kind, msg) {
  const payload = `data: ${JSON.stringify({ kind, msg: msg || "" })}\n\n`;
  for (const res of sseClients) res.write(payload);
}

function startServer() {
  const server = http.createServer((req, res) => {
    const uri = req.url.split("?", 1)[0];

    if (uri === "/live_reload") {
      res.writeHead(200, {
        "Content-Type": "text/event-stream",
        "Cache-Control": "no-cache",
        Connection: "keep-alive",
      });
      res.write("\n");
      sseClients.add(res);
      req.on("close", () => sseClients.delete(res));
      return;
    }

    let key = uri.replace(/^\//, "");
    if (key === "" || key.endsWith("/")) key += "index.html";
    if (cfg.verbose) console.debug(req.method, req.url);

    const val = currentSite.get(key);
    if (val === undefined) {
      res.writeHead(404, { "Content-Type": "text/plain" });
      res.end(`404 Not found: ${key}`);
      return;
    }

    const contentType = mimeFor(key);
    let body;
    if (val !== null && typeof val === "object" && val.copyFrom !== undefined) {
      body = fs.readFileSync(val.copyFrom);
    } else {
      body = Buffer.isBuffer(val) ? val : Buffer.from(val, "utf8");
    }
    if (contentType === "text/html") {
      // Note: no Content-Encoding on the .gz search index -- script.js
      // inflates it itself with DecompressionStream.
      body = Buffer.from(
        body.toString("utf8").replace("</body>", LIVE_RELOAD_JS + "</body>"),
        "utf8",
      );
    }
    res.writeHead(200, {
      "Content-Type": contentType,
      "Content-Length": body.length,
      "Cache-Control": "no-cache",
    });
    res.end(body);
  });
  server.on("error", (e) => {
    if (e.code === "EADDRINUSE") {
      console.error(
        `\nPort ${cfg.port} is already in use -- another dev server is ` +
          `probably running.\nPass --port to use a different one.\n`,
      );
      process.exit(1);
    }
    throw e;
  });
  server.listen(cfg.port, cfg.host, () => {
    console.log(`Starting HTTP server on http://localhost:${cfg.port}`);
  });
}

// ---------------------------------------------------------------------------
// Watch mode.
// ---------------------------------------------------------------------------

// True if only the stylesheet changed, in which case the browser can swap the
// <link> instead of reloading and losing scroll position.
function changedPaths(oldSite, newSite) {
  const changed = new Set();
  for (const [k, v] of newSite) {
    const o = oldSite.get(k);
    if (o === undefined) {
      changed.add(k);
    } else if (
      typeof v === "object" &&
      v !== null &&
      v.copyFrom !== undefined
    ) {
      if (typeof o !== "object" || o === null || o.copyFrom !== v.copyFrom) {
        changed.add(k);
      }
    } else if (Buffer.isBuffer(v) || Buffer.isBuffer(o)) {
      const bv = Buffer.isBuffer(v) ? v : Buffer.from(String(v));
      const bo = Buffer.isBuffer(o) ? o : Buffer.from(String(o));
      if (!bv.equals(bo)) changed.add(k);
    } else if (v !== o) {
      changed.add(k);
    }
  }
  for (const k of oldSite.keys()) if (!newSite.has(k)) changed.add(k);
  return changed;
}

async function rebuild() {
  const t0 = performance.now();
  let newSite;
  try {
    newSite = await build();
  } catch (e) {
    progressClear();
    console.error(`\nBuild failed:\n${e.message}\n`);
    notifyClients("error", e.message);
    return;
  }
  const changed = changedPaths(currentSite, newSite);
  const prevSite = currentSite;
  currentSite = newSite;
  if (!cfg.startHttpServer) writeSite(newSite);

  progressClear();
  const ms = Math.round(performance.now() - t0);
  const onlyCss =
    changed.size > 0 &&
    [...changed].every((k) => k.startsWith("assets/style.css"));
  console.log(
    `Rebuilt in ${ms} ms (${changed.size} file(s) changed, ` +
      `${memoStats.hit} cached / ${memoStats.miss} rebuilt)`,
  );
  if (prevSite.size === 0) return; // First build; nothing to notify yet.
  if (changed.size === 0) {
    notifyClients("ok");
  } else {
    notifyClients(onlyCss ? "css" : "reload");
  }
}

let timer = null;
let restartPending = false;
let buildInFlight = false;
let dirtyDuringBuild = false;

function onFileChange(dir, filePath) {
  if (cfg.verbose) console.log("File change detected:", dir, filePath);
  // Only this script's *code* forces a restart: ESM modules can't be
  // un-imported. Everything else under src/ -- the EJS templates, the python
  // generators, style.scss, the images -- is data that a normal rebuild picks
  // up (the template cache is dropped at the top of every build).
  if (filePath && filePath.endsWith(".mjs")) {
    restartPending = true;
  }
  if (buildInFlight) {
    // A change that lands mid-build would otherwise be lost: the file list was
    // globbed before it appeared, and no further event is coming.
    dirtyDuringBuild = true;
    return;
  }
  if (timer !== null) clearTimeout(timer);
  timer = setTimeout(onQuiescent, 50);
}

async function onQuiescent() {
  timer = null;
  if (restartPending) {
    console.log("\nBuild sources changed, restarting...\n");
    process.exit(EXIT_RESTART);
  }
  buildInFlight = true;
  try {
    await rebuild();
  } finally {
    buildInFlight = false;
  }
  if (dirtyDuringBuild) {
    dirtyDuringBuild = false;
    timer = setTimeout(onQuiescent, 50);
  }
}

// Watchers are installed *before* the first build, so a file written while that
// build is running is still noticed.
function startWatching() {
  for (const rel of WATCH_DIRS) {
    const abs = pjoin(ROOT_DIR, rel);
    try {
      fs.watch(abs, { recursive: true }, (_evt, filePath) =>
        onFileChange(abs, filePath),
      );
    } catch (e) {
      console.warn(`Cannot watch ${abs}: ${e.message}`);
    }
  }
}

// ---------------------------------------------------------------------------

async function main() {
  const parser = new argparse.ArgumentParser();
  parser.add_argument("--out", { help: "Output directory" });
  parser.add_argument("--watch", "-w", { action: "store_true" });
  parser.add_argument("--serve", "-s", { action: "store_true" });
  parser.add_argument("--verbose", "-v", { action: "store_true" });
  parser.add_argument("--port", { type: "int", default: 8082 });
  parser.add_argument("--host", { default: "0.0.0.0" });
  const args = parser.parse_args();

  if (args.out) {
    cfg.outDir = path.isAbsolute(args.out)
      ? args.out
      : pjoin(ROOT_DIR, args.out);
  }
  ensureDir(cfg.outDir);
  cfg.watch = !!args.watch;
  cfg.verbose = !!args.verbose;
  cfg.startHttpServer = !!args.serve;
  cfg.port = args.port;
  cfg.host = args.host;

  // Watch first: a file written while the initial build is running would
  // otherwise never be noticed (it wasn't in the glob, and no later event
  // covers it).
  if (cfg.watch) {
    startWatching();
    buildInFlight = true;
  }

  const t0 = performance.now();
  try {
    currentSite = await build();
  } finally {
    buildInFlight = false;
  }
  if (!cfg.startHttpServer) writeSite(currentSite);
  progressClear();
  console.log(
    `Built ${currentSite.size} files in ${Math.round(performance.now() - t0)} ms`,
  );

  if (cfg.startHttpServer) {
    // In serve mode the site is served straight out of memory; a deleted doc
    // 404s immediately instead of lingering as a stale file on disk.
    startServer();
  }
  if (cfg.watch && dirtyDuringBuild) {
    dirtyDuringBuild = false;
    timer = setTimeout(onQuiescent, 50);
  }
  if (!cfg.watch && !cfg.startHttpServer) {
    process.exit(0);
  }
}

main().catch((e) => {
  progressClear();
  console.error(e.isDocsError ? `\n${e.message}\n` : e.stack || e.message);
  process.exit(1);
});
