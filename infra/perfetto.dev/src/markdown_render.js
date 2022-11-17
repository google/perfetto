// Copyright (C) 2020 The Android Open Source Project
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

const ejs = require('ejs');
const marked = require('marked');
const argv = require('yargs').argv
const fs = require('fs-extra');
const path = require('path');
const hljs = require('highlight.js');

const CS_BASE_URL =
    'https://cs.android.com/android/platform/superproject/+/master:external/perfetto';

const ROOT_DIR = path.dirname(path.dirname(path.dirname(__dirname)));

let outDir = '';
let curMdFile = '';
let title = '';
let depFileFd = undefined;

function hrefInDocs(href) {
  if (href.match(/^(https?:)|^(mailto:)|^#/)) {
    return undefined;
  }
  let pathFromRoot;
  if (href.startsWith('/')) {
    pathFromRoot = href;
  } else {
    curDocDir = '/' + path.relative(ROOT_DIR, path.dirname(curMdFile));
    pathFromRoot = path.join(curDocDir, href);
  }
  if (pathFromRoot.startsWith('/docs/')) {
    return pathFromRoot;
  }
  return undefined;
}

function assertNoDeadLink(relPathFromRoot) {
  relPathFromRoot = relPathFromRoot.replace(/\#.*$/g, '');  // Remove #line.

  // Skip check for build-time generated reference pages.
  if (relPathFromRoot.endsWith('.autogen'))
    return;

  const fullPath = path.join(ROOT_DIR, relPathFromRoot);
  if (!fs.existsSync(fullPath) && !fs.existsSync(fullPath + '.md')) {
    const msg = `Dead link: ${relPathFromRoot} in ${curMdFile}`;
    console.error(msg);
    throw new Error(msg);
  }
}

function renderHeading(text, level) {
  // If the heading has an explicit ${#anchor}, use that. Otherwise infer the
  // anchor from the text but only for h2 and h3. Note the right-hand-side TOC
  // is dynamically generated from anchors (explicit or implicit).
  if (level === 1 && !title) {
    title = text;
  }
  let anchorId = '';
  const explicitAnchor = /{#([\w-_.]+)}/.exec(text);
  if (explicitAnchor) {
    text = text.replace(explicitAnchor[0], '');
    anchorId = explicitAnchor[1];
  } else if (level >= 2 && level <= 3) {
    anchorId = text.toLowerCase().replace(/[^\w]+/g, '-');
    anchorId = anchorId.replace(/[-]+/g, '-');  // Drop consecutive '-'s.
  }
  let anchor = '';
  if (anchorId) {
    anchor = `<a name="${anchorId}" class="anchor" href="#${anchorId}"></a>`;
  }
  return `<h${level}>${anchor}${text}</h${level}>`;
}

function renderLink(originalLinkFn, href, title, text) {
  if (href.startsWith('../')) {
    throw new Error(
        `Don\'t use relative paths in docs, always use /docs/xxx ` +
        `or /src/xxx for both links to docs and code (${href})`)
  }
  const docsHref = hrefInDocs(href);
  let sourceCodeLink = undefined;
  if (docsHref !== undefined) {
    // Check that the target doc exists. Skip the check on /reference/ files
    // that are typically generated at build time.
    assertNoDeadLink(docsHref);
    href = docsHref.replace(/[.](md|autogen)\b/, '');
    href = href.replace(/\/README$/, '/');
  } else if (href.startsWith('/') && !href.startsWith('//')) {
    // /tools/xxx -> github/tools/xxx.
    sourceCodeLink = href;
  }
  if (sourceCodeLink !== undefined) {
    // Fix up line anchors for GitHub link: #42 -> #L42.
    sourceCodeLink = sourceCodeLink.replace(/#(\d+)$/g, '#L$1')
    assertNoDeadLink(sourceCodeLink);
    href = CS_BASE_URL + sourceCodeLink;
  }
  return originalLinkFn(href, title, text);
}

function renderCode(text, lang) {
  if (lang === 'mermaid') {
    return `<div class="mermaid">${text}</div>`;
  }

  let hlHtml = '';
  if (lang) {
    hlHtml = hljs.highlight(lang, text).value
  } else {
    hlHtml = hljs.highlightAuto(text).value
  }
  return `<code class="hljs code-block">${hlHtml}</code>`
}

function renderImage(originalImgFn, href, title, text) {
  const docsHref = hrefInDocs(href);
  if (docsHref !== undefined) {
    const outFile = outDir + docsHref;
    const outParDir = path.dirname(outFile);
    fs.ensureDirSync(outParDir);
    fs.copyFileSync(ROOT_DIR + docsHref, outFile);
    if (depFileFd) {
      fs.write(depFileFd, ` ${ROOT_DIR + docsHref}`);
    }
  }
  if (href.endsWith('.svg')) {
    return `<object type="image/svg+xml" data="${href}"></object>`
  }
  return originalImgFn(href, title, text);
}

function renderParagraph(text) {
  let cssClass = '';
  if (text.startsWith('NOTE:')) {
    cssClass = 'note';
  }
   else if (text.startsWith('TIP:')) {
    cssClass = 'tip';
  }
   else if (text.startsWith('TODO:') || text.startsWith('FIXME:')) {
    cssClass = 'todo';
  }
   else if (text.startsWith('WARNING:')) {
    cssClass = 'warning';
  }
   else if (text.startsWith('Summary:')) {
    cssClass = 'summary';
  }
  if (cssClass != '') {
    cssClass = ` class="callout ${cssClass}"`;
  }

  // Rudimentary support of definition lists.
  var colonStart = text.search("\n:")
  if (colonStart != -1) {
    var key = text.substring(0, colonStart);
    var value = text.substring(colonStart + 2);
    return `<dl><dt><p>${key}</p></dt><dd><p>${value}</p></dd></dl>`
  }

  return `<p${cssClass}>${text}</p>\n`;
}

function render(rawMarkdown) {
  const renderer = new marked.Renderer();
  const originalLinkFn = renderer.link.bind(renderer);
  const originalImgFn = renderer.image.bind(renderer);
  renderer.link = (hr, ti, te) => renderLink(originalLinkFn, hr, ti, te);
  renderer.image = (hr, ti, te) => renderImage(originalImgFn, hr, ti, te);
  renderer.code = renderCode;
  renderer.heading = renderHeading;
  renderer.paragraph = renderParagraph;

  return marked.marked.parse(rawMarkdown, {renderer: renderer});
}

function main() {
  const inFile = argv['i'];
  const outFile = argv['o'];
  outDir = argv['odir'];
  depFile = argv['depfile'];
  const templateFile = argv['t'];
  if (!outFile || !outDir) {
    console.error(
        'Usage: --odir site -o out.html ' +
        '[-i input.md] [-t templ.html] ' +
        '[--depfile depfile.d]');
    process.exit(1);
  }
  curMdFile = inFile;

  if (depFile) {
    const depFileDir = path.dirname(depFile);
    fs.ensureDirSync(depFileDir);
    depFileFd = fs.openSync(depFile, 'w');
    fs.write(depFileFd, `${outFile}:`);
  }
  let markdownHtml = '';
  if (inFile) {
    markdownHtml = render(fs.readFileSync(inFile, 'utf8'));
  }

  if (templateFile) {
    // TODO rename nav.html to sitemap or something more mainstream.
    const navFilePath = path.join(outDir, 'docs', '_nav.html');
    const fallbackTitle =
        'Perfetto - System profiling, app tracing and trace analysis';
    const templateData = {
      markdown: markdownHtml,
      title: title ? `${title} - Perfetto Tracing Docs` : fallbackTitle,
      fileName: '/' + path.relative(outDir, outFile),
    };
    if (fs.existsSync(navFilePath)) {
      templateData['nav'] = fs.readFileSync(navFilePath, 'utf8');
    }
    ejs.renderFile(templateFile, templateData, (err, html) => {
      if (err)
        throw err;
      fs.writeFileSync(outFile, html);
      process.exit(0);
    });
  } else {
    fs.writeFileSync(outFile, markdownHtml);
    process.exit(0);
  }
}

main();
