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

'use strict';

let tocAnchors = [];
let lastMouseOffY = 0;
let onloadFired = false;
const postLoadActions = [];
let tocEventHandlersInstalled = false;
let resizeObserver = undefined;

// Handles redirects from the old docs.perfetto.dev.
const legacyRedirectMap = {
  '#/contributing': '/docs/contributing/getting-started#community',
  '#/build-instructions': '/docs/contributing/build-instructions',
  '#/testing': '/docs/contributing/testing',
  '#/app-instrumentation': '/docs/instrumentation/tracing-sdk',
  '#/recording-traces': '/docs/instrumentation/tracing-sdk#recording',
  '#/running': '/docs/quickstart/android-tracing',
  '#/long-traces': '/docs/concepts/config#long-traces',
  '#/detached-mode': '/docs/concepts/detached-mode',
  '#/heapprofd': '/docs/data-sources/native-heap-profiler',
  '#/java-hprof': '/docs/data-sources/java-heap-profiler',
  '#/trace-processor': '/docs/analysis/trace-processor',
  '#/analysis': '/docs/analysis/trace-processor#annotations',
  '#/metrics': '/docs/analysis/metrics',
  '#/traceconv': '/docs/quickstart/traceconv',
  '#/clock-sync': '/docs/concepts/clock-sync',
  '#/architecture': '/docs/concepts/service-model',
};

function doAfterLoadEvent(action) {
  if (onloadFired) {
    return action();
  }
  postLoadActions.push(action);
}

function setupSandwichMenu() {
  const header = document.querySelector('.site-header');
  const docsNav = document.querySelector('.nav');
  const menu = header.querySelector('.menu');
  menu.addEventListener('click', (e) => {
    e.preventDefault();

    // If we are displaying any /docs, toggle the navbar instead (the TOC).
    if (docsNav) {
      // |after_first_click| is to avoid spurious transitions on page load.
      docsNav.classList.add('after_first_click');
      updateNav();
      setTimeout(() => docsNav.classList.toggle('expanded'), 0);
    } else {
      header.classList.toggle('expanded');
    }
  });
}

// (Re-)Generates the Table Of Contents for docs (the right-hand-side one).
function updateTOC() {
  const tocContainer = document.querySelector('.docs .toc');
  if (!tocContainer)
    return;
  const toc = document.createElement('ul');
  const anchors = document.querySelectorAll('.doc a.anchor');
  tocAnchors = [];
  for (const anchor of anchors) {
    const li = document.createElement('li');
    const link = document.createElement('a');
    link.innerText = anchor.parentElement.innerText;
    link.href = anchor.href;
    link.onclick = () => {
      onScroll(link)
    };
    li.appendChild(link);
    if (anchor.parentElement.tagName === 'H3')
      li.style.paddingLeft = '10px';
    toc.appendChild(li);
    doAfterLoadEvent(() => {
      tocAnchors.push(
          {top: anchor.offsetTop + anchor.offsetHeight / 2, obj: link});
    });
  }
  tocContainer.innerHTML = '';
  tocContainer.appendChild(toc);

  // Add event handlers on the first call (can be called more than once to
  // recompute anchors on resize).
  if (tocEventHandlersInstalled)
    return;
  tocEventHandlersInstalled = true;
  const doc = document.querySelector('.doc');
  const passive = {passive: true};
  if (doc) {
    const offY = doc.offsetTop;
    doc.addEventListener('mousemove', (e) => onMouseMove(offY, e), passive);
    doc.addEventListener('mouseleave', () => {
      lastMouseOffY = 0;
    }, passive);
  }
  window.addEventListener('scroll', () => onScroll(), passive);
  resizeObserver = new ResizeObserver(() => requestAnimationFrame(() => {
                                        updateNav();
                                        updateTOC();
                                      }));
  resizeObserver.observe(doc);
}

// Highlights the current TOC anchor depending on the scroll offset.
function onMouseMove(offY, e) {
  lastMouseOffY = e.clientY - offY;
  onScroll();
}

function onScroll(forceHighlight) {
  const y = document.documentElement.scrollTop + lastMouseOffY;
  let highEl = undefined;
  for (const x of tocAnchors) {
    if (y < x.top)
      continue;
    highEl = x.obj;
  }
  for (const link of document.querySelectorAll('.docs .toc a')) {
    if ((!forceHighlight && link === highEl) || (forceHighlight === link)) {
      link.classList.add('highlighted');
    } else {
      link.classList.remove('highlighted');
    }
  }
}

// This function needs to be idempotent as it is called more than once (on every
// resize).
function updateNav() {
  const curDoc = document.querySelector('.doc');
  let curFileName = '';
  if (curDoc)
    curFileName = curDoc.dataset['mdFile'];

  // First identify all the top-level nav entries (Quickstart, Data Sources,
  // ...) and make them compressible.
  const toplevelSections = document.querySelectorAll('.docs .nav > ul > li');
  const toplevelLinks = [];
  for (const sec of toplevelSections) {
    const childMenu = sec.querySelector('ul');
    if (!childMenu) {
      // Don't make it compressible if it has no children (e.g. the very
      // first 'Introduction' link).
      continue;
    }

    // Don't make it compressible if the entry has an actual link (e.g. the very
    // first 'Introduction' link), because otherwise it become ambiguous whether
    // the link should toggle or open the link.
    const link = sec.querySelector('a');
    if (!link || !link.href.endsWith('#'))
      continue;

    sec.classList.add('compressible');

    // Remember the compressed status as long as the page is opened, so clicking
    // through links keeps the sidebar in a consistent visual state.
    const memoKey = `docs.nav.compressed[${link.innerHTML}]`;

    if (sessionStorage.getItem(memoKey) === '1') {
      sec.classList.add('compressed');
    }
    doAfterLoadEvent(() => {
      childMenu.style.maxHeight = `${childMenu.scrollHeight + 40}px`;
    });

    toplevelLinks.push(link);
    link.onclick = (evt) => {
      evt.preventDefault();
      sec.classList.toggle('compressed');
      if (sec.classList.contains('compressed')) {
        sessionStorage.setItem(memoKey, '1');
      } else {
        sessionStorage.removeItem(memoKey);
      }
    };
  }

  const exps = document.querySelectorAll('.docs .nav ul a');
  let found = false;
  for (const x of exps) {
    // If the url of the entry matches the url of the page, mark the item as
    // highlighted and expand all its parents.
    if (!x.href)
      continue;
    const url = new URL(x.href);
    if (x.href.endsWith('#')) {
      // This is a non-leaf link to a menu.
      if (toplevelLinks.indexOf(x) < 0) {
        x.removeAttribute('href');
      }
    } else if (url.pathname === curFileName && !found) {
      x.classList.add('selected');
      doAfterLoadEvent(() => x.scrollIntoViewIfNeeded());
      found = true;  // Highlight only the first occurrence.
    }
  }
}

// If the page contains a ```mermaid ``` block, lazily loads the plugin and
// renders.
function initMermaid() {
  const graphs = document.querySelectorAll('.mermaid');

  // Skip if there are no mermaid graphs to render.
  if (!graphs.length)
    return;

  const script = document.createElement('script');
  script.type = 'text/javascript';
  script.src = '/assets/mermaid.min.js';
  const themeCSS = `
  .cluster rect { fill: #FCFCFC; stroke: #ddd }
  .node rect { fill: #DCEDC8; stroke: #8BC34A}
  .edgeLabel:not(:empty) {
      border-radius: 6px;
      font-size: 0.9em;
      padding: 4px;
      background: #F5F5F5;
      border: 1px solid #DDDDDD;
      color: #666;
  }
  `;
  script.addEventListener('load', () => {
    mermaid.initialize({
      startOnLoad: false,
      themeCSS: themeCSS,
      securityLevel: 'loose',  // To allow #in-page-links
    });
    for (const graph of graphs) {
      requestAnimationFrame(() => {
        mermaid.init(undefined, graph);
        graph.classList.add('rendered');
      });
    }
  })
  document.body.appendChild(script);
}

function setupSearch() {
  const URL =
      'https://www.googleapis.com/customsearch/v1?key=AIzaSyBTD2XJkQkkuvDn76LSftsgWOkdBz9Gfwo&cx=007128963598137843411:8suis14kcmy&q='
  const searchContainer = document.getElementById('search');
  const searchBox = document.getElementById('search-box');
  const searchRes = document.getElementById('search-res')
  if (!searchBox || !searchRes) return;

  document.body.addEventListener('keydown', (e) => {
    if (e.key === '/' && e.target.tagName.toLowerCase() === 'body') {
      searchBox.setSelectionRange(0, -1);
      searchBox.focus();
      e.preventDefault();
    } else if (e.key === 'Escape' && searchContainer.contains(e.target)) {
      searchBox.blur();

      // Handle the case of clicking Tab and moving down to results.
      e.target.blur();
    }
  });

  let timerId = -1;
  let lastSearchId = 0;

  const doSearch = async () => {
    timerId = -1;
    searchRes.style.width = `${searchBox.offsetWidth}px`;

    // `searchId` handles the case of two subsequent requests racing. This is to
    // prevent older results, delivered in reverse order, to replace newer ones.
    const searchId = ++lastSearchId;
    const f = await fetch(URL + encodeURIComponent(searchBox.value));
    const jsonRes = await f.json();
    const results = jsonRes['items'];
    searchRes.innerHTML = '';
    if (results === undefined || searchId != lastSearchId) {
      return;
    }
    for (const res of results) {
      const link = document.createElement('a');
      link.href = res.link;
      const title = document.createElement('div');
      title.className = 'sr-title';
      title.innerText = res.title.replace(' - Perfetto Tracing Docs', '');
      link.appendChild(title);

      const snippet = document.createElement('div');
      snippet.className = 'sr-snippet';
      snippet.innerText = res.snippet;
      link.appendChild(snippet);

      const div = document.createElement('div');
      div.appendChild(link);
      searchRes.appendChild(div);
    }
  };

  searchBox.addEventListener('keyup', () => {
    if (timerId >= 0) return;
    timerId = setTimeout(doSearch, 200);
  });
}

window.addEventListener('DOMContentLoaded', () => {
  updateNav();
  updateTOC();
});

window.addEventListener('load', () => {
  setupSandwichMenu();
  initMermaid();

  // Don't smooth-scroll on pages that are too long (e.g. reference pages).
  if (document.body.scrollHeight < 10000) {
    document.documentElement.style.scrollBehavior = 'smooth';
  } else {
    document.documentElement.style.scrollBehavior = 'initial';
  }

  onloadFired = true;
  while (postLoadActions.length > 0) {
    postLoadActions.shift()();
  }

  updateTOC();
  setupSearch();

  // Enable animations only after the load event. This is to prevent glitches
  // when switching pages.
  document.documentElement.style.setProperty('--anim-enabled', '1')
});

const fragment = location.hash.split('?')[0].replace('.md', '');
if (fragment in legacyRedirectMap) {
  location.replace(legacyRedirectMap[fragment]);
}