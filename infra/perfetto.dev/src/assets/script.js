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

"use strict";

let tocAnchors = [];
let lastMouseOffY = 0;
let onloadFired = false;
const postLoadActions = [];
let tocEventHandlersInstalled = false;
let resizeObserver = undefined;

function doAfterLoadEvent(action) {
  if (onloadFired) {
    return action();
  }
  postLoadActions.push(action);
}

function setupSandwichMenu() {
  const header = document.querySelector(".site-header");
  const docsNav = document.querySelector(".nav");
  const menu = header.querySelector(".menu");
  menu.addEventListener("click", (e) => {
    e.preventDefault();

    // If we are displaying any /docs, toggle the navbar instead (the TOC).
    if (docsNav) {
      // |after_first_click| is to avoid spurious transitions on page load.
      docsNav.classList.add("after_first_click");
      updateNav();
      setTimeout(() => docsNav.classList.toggle("expanded"), 0);
    } else {
      header.classList.toggle("expanded");
    }
  });
}

// (Re-)Generates the Table Of Contents for docs (the right-hand-side one).
function updateTOC() {
  const tocContainer = document.querySelector(".docs .toc");
  if (!tocContainer) return;
  const toc = document.createElement("ul");
  const anchors = document.querySelectorAll(".doc a.anchor");
  tocAnchors = [];
  for (const anchor of anchors) {
    const li = document.createElement("li");
    const link = document.createElement("a");
    link.innerText = anchor.parentElement.innerText;
    link.href = anchor.href;
    link.onclick = () => {
      onScroll(link);
    };
    li.appendChild(link);
    if (anchor.parentElement.tagName === "H3") li.style.paddingLeft = "10px";
    toc.appendChild(li);
    doAfterLoadEvent(() => {
      tocAnchors.push({
        top: anchor.offsetTop + anchor.offsetHeight / 2,
        obj: link,
      });
    });
  }
  tocContainer.innerHTML = "";
  tocContainer.appendChild(toc);

  // Add event handlers on the first call (can be called more than once to
  // recompute anchors on resize).
  if (tocEventHandlersInstalled) return;
  tocEventHandlersInstalled = true;
  const doc = document.querySelector(".doc");
  const passive = { passive: true };
  if (doc) {
    const offY = doc.offsetTop;
    doc.addEventListener("mousemove", (e) => onMouseMove(offY, e), passive);
    doc.addEventListener(
      "mouseleave",
      () => {
        lastMouseOffY = 0;
      },
      passive,
    );
  }
  window.addEventListener("scroll", () => onScroll(), passive);
  resizeObserver = new ResizeObserver(() =>
    requestAnimationFrame(() => {
      updateNav();
      updateTOC();
    }),
  );
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
    if (y < x.top) continue;
    highEl = x.obj;
  }
  for (const link of document.querySelectorAll(".docs .toc a")) {
    if ((!forceHighlight && link === highEl) || forceHighlight === link) {
      link.classList.add("highlighted");
    } else {
      link.classList.remove("highlighted");
    }
  }
}

function scrollIntoViewIfNeeded(element, container, margin = 100) {
  const containerTop = container.scrollTop;
  const containerBottom = containerTop + container.clientHeight;

  const elementTop = element.offsetTop;
  const elementBottom = elementTop + element.offsetHeight;
  if (elementTop < containerTop) {
    container.scrollTo({
      top: elementTop - margin,
    });
  } else if (elementBottom > containerBottom) {
    container.scrollTo({
      top: elementBottom - container.clientHeight + margin,
    });
  }
}

// Audiences shown in the nav tag bar. Tags here must match the backtick
// tags attached to leaf entries in docs/toc.md.
const NAV_AUDIENCES = [
  { tag: 'android', label: 'Android' },
  { tag: 'linux', label: 'Linux' },
  { tag: 'cpp', label: 'C++ dev' },
  { tag: 'chrome', label: 'Chrome' },
  { tag: 'perf', label: 'Perf dev' },
  { tag: 'contrib', label: 'Contributors' },
];
const NAV_DEFAULT_TAGS = ['android'];

// This function needs to be idempotent as it is called more than once (on every
// resize).
function updateNav() {
  const curDoc = document.querySelector(".doc");
  let curFileName = "";
  if (curDoc) curFileName = curDoc.dataset["mdFile"];

  const nav = document.querySelector('.docs .nav');
  if (!nav) return;
  const rootUl = nav.querySelector(':scope > ul');
  if (!rootUl) return;

  // --- Step 1: Load the set of active audience tags. ---
  const validTags = new Set(NAV_AUDIENCES.map((a) => a.tag));
  let activeTags = null;
  try {
    const raw = sessionStorage.getItem('docs.nav.activeTags');
    if (raw) activeTags = JSON.parse(raw).filter((t) => validTags.has(t));
  } catch (e) {
    activeTags = null;
  }
  if (!activeTags) activeTags = NAV_DEFAULT_TAGS.slice();
  const activeSet = new Set(activeTags);
  sessionStorage.setItem(
    'docs.nav.activeTags', JSON.stringify([...activeSet]));

  // --- Step 2: Build/refresh the audience chip bar. ---
  let filterBox = nav.querySelector(':scope > .audience-filter');
  if (!filterBox) {
    filterBox = document.createElement('div');
    filterBox.className = 'audience-filter';
    const caption = document.createElement('div');
    caption.className = 'audience-filter-caption';
    caption.textContent = 'Perfetto for:';
    const chipBar = document.createElement('div');
    chipBar.className = 'audience-tags';
    filterBox.appendChild(caption);
    filterBox.appendChild(chipBar);
    nav.insertBefore(filterBox, rootUl);
  }
  const chipBar = filterBox.querySelector(':scope > .audience-tags');
  chipBar.innerHTML = '';
  for (const a of NAV_AUDIENCES) {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'audience-tag';
    btn.textContent = a.label;
    if (activeSet.has(a.tag)) btn.classList.add('active');
    btn.addEventListener('click', () => {
      const next = new Set(activeSet);
      if (next.has(a.tag)) next.delete(a.tag);
      else next.add(a.tag);
      sessionStorage.setItem(
        'docs.nav.activeTags', JSON.stringify([...next]));
      updateNav();
    });
    chipBar.appendChild(btn);
  }

  // --- Step 3: Parse per-leaf audience tags from <code> markers. ---
  // Each leaf's tags come from a trailing `tag1 tag2` in the markdown,
  // rendered by marked.js as a <code> child of the <li> (or its <p>).
  const allLis = rootUl.querySelectorAll('li');
  for (const li of allLis) {
    if (li.dataset.navTagsParsed) continue;
    const code = li.querySelector(':scope > code') ||
                 li.querySelector(':scope > p > code');
    if (code) {
      li.dataset.navTags = code.textContent.trim();
    }
    li.dataset.navTagsParsed = '1';
  }

  // --- Step 4: Filter leaves by active audience, then collapse empty
  // ancestors. Visibility is decided bottom-up. ---
  const showAll = activeSet.size === 0;
  const hasTag = (li) => {
    if (!li.dataset.navTags) return null;
    if (showAll) return true;
    return li.dataset.navTags.split(/\s+/).some((t) => activeSet.has(t));
  };

  const isVisible = new Map();
  const setVisible = (li, v) => {
    isVisible.set(li, v);
    li.style.display = v ? '' : 'none';
  };

  // Bottom-up pass: leaf visibility from tags; parent visible if any child is.
  const process = (li) => {
    const childUl = li.querySelector(':scope > ul');
    if (!childUl) {
      // Leaf: visible iff untagged (universal) or tag set includes active tag.
      const match = hasTag(li);
      setVisible(li, match === null ? true : match);
      return isVisible.get(li);
    }
    let anyChildVisible = false;
    for (const child of childUl.querySelectorAll(':scope > li')) {
      if (process(child)) anyChildVisible = true;
    }
    setVisible(li, anyChildVisible);
    return anyChildVisible;
  };
  for (const li of rootUl.querySelectorAll(':scope > li')) {
    process(li);
  }

  // --- Step 5: Make inner sections (categories) compressible. ---
  for (const sec of rootUl.querySelectorAll('li')) {
    if (sec.parentElement === rootUl) continue;

    const childMenu = sec.querySelector(':scope > ul');
    if (!childMenu) continue;

    const link = sec.querySelector(':scope > p > a') ||
                 sec.querySelector(':scope > a');
    if (!link || !link.href || !link.href.endsWith('#')) continue;

    sec.classList.add('compressible');

    const memoKey = `docs.nav.compressed[${link.innerHTML}]`;
    const memo = sessionStorage.getItem(memoKey);
    if (memo === '1') {
      sec.classList.add('compressed');
    } else if (memo === '0') {
      sec.classList.remove('compressed');
    }

    doAfterLoadEvent(() => {
      childMenu.style.maxHeight = `${childMenu.scrollHeight + 40}px`;
    });

    link.onclick = (evt) => {
      evt.preventDefault();
      sec.classList.toggle('compressed');
      if (sec.classList.contains('compressed')) {
        sessionStorage.setItem(memoKey, '1');
      } else {
        sessionStorage.setItem(memoKey, '0');
      }
    };
  }

  // --- Step 6: Highlight the current page. ---
  const allLinks = nav.querySelectorAll('ul a');
  let found = false;
  for (const x of allLinks) {
    if (!x.href) continue;
    const url = new URL(x.href);
    if (x.href.endsWith("#")) {
      // Remove href from non-compressible # links.
      const parentLi = x.closest('li');
      if (parentLi && !parentLi.classList.contains('compressible')) {
        x.removeAttribute("href");
      }
    } else if ((url.pathname === curFileName ||
                url.pathname + 'index.html' === curFileName) && !found) {
      x.classList.add('selected');

      // Walk up the DOM to expand all ancestor compressible sections.
      let el = x.closest('li');
      while (el) {
        if (el.classList.contains('compressible') &&
            el.classList.contains('compressed')) {
          el.classList.remove('compressed');
          const elLink = el.querySelector(':scope > p > a') ||
                         el.querySelector(':scope > a');
          if (elLink) {
            sessionStorage.setItem(
              `docs.nav.compressed[${elLink.innerHTML}]`, '0');
          }
        }
        el = el.parentElement ? el.parentElement.closest('li') : null;
      }

      if (!onloadFired) {
        scrollIntoViewIfNeeded(x, nav);
      }
      found = true;
    } else {
      x.classList.remove('selected');
    }
  }
}

// If the page contains a ```mermaid ``` block, lazily loads the plugin and
// renders.
function initMermaid() {
  const graphs = document.querySelectorAll(".mermaid");

  // Skip if there are no mermaid graphs to render.
  if (!graphs.length) return;

  const script = document.createElement("script");
  script.type = "text/javascript";
  script.src = "/assets/mermaid.min.js";
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
  script.addEventListener("load", () => {
    mermaid.initialize({
      startOnLoad: false,
      themeCSS: themeCSS,
      securityLevel: "loose", // To allow #in-page-links
    });
    for (const graph of graphs) {
      requestAnimationFrame(() => {
        mermaid.init(undefined, graph);
        graph.classList.add("rendered");
      });
    }
  });
  document.body.appendChild(script);
}

function setupSearch() {
  const URL =
    "https://www.googleapis.com/customsearch/v1?key=AIzaSyBTD2XJkQkkuvDn76LSftsgWOkdBz9Gfwo&cx=007128963598137843411:8suis14kcmy&q=";
  const searchContainer = document.getElementById("search");
  const searchBox = document.getElementById("search-box");
  const searchRes = document.getElementById("search-res");
  if (!searchBox || !searchRes) return;

  document.body.addEventListener("keydown", (e) => {
    if (e.key === "/" && e.target.tagName.toLowerCase() === "body") {
      searchBox.setSelectionRange(0, -1);
      searchBox.focus();
      e.preventDefault();
    } else if (e.key === "Escape" && searchContainer.contains(e.target)) {
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
    const results = jsonRes["items"];
    searchRes.innerHTML = "";
    if (results === undefined || searchId != lastSearchId) {
      return;
    }
    for (const res of results) {
      const link = document.createElement("a");
      link.href = res.link;
      const title = document.createElement("div");
      title.className = "sr-title";
      title.innerText = res.title.replace(" - Perfetto Tracing Docs", "");
      link.appendChild(title);

      const snippet = document.createElement("div");
      snippet.className = "sr-snippet";
      snippet.innerText = res.snippet;
      link.appendChild(snippet);

      const div = document.createElement("div");
      div.appendChild(link);
      searchRes.appendChild(div);
    }
  };

  searchBox.addEventListener("keyup", () => {
    if (timerId >= 0) return;
    timerId = setTimeout(doSearch, 200);
  });
}

function setupTabs() {
  const tabs = document.body.querySelectorAll('.tab-box');
  for (const tab of tabs) {
    const tabButtons = tab.querySelectorAll('.tab-button');
    const tabContents = tab.querySelectorAll('.tab-content');
    if (tabButtons.length !== tabContents.length || tabButtons.length === 0) {
      continue;
    }
    let active = undefined;
    const updateSelected = (newActive) => {
      if (active !== undefined) {
        tabButtons[active].classList.remove('tab-button-selected');
        tabContents[active].classList.remove('tab-content-selected');
      }
      tabButtons[newActive].classList.add('tab-button-selected');
      tabContents[newActive].classList.add('tab-content-selected');
      active = newActive;
    };
    for (let i = 0; i < tabButtons.length; ++i) {
      tabButtons[i].addEventListener('click', (e) => {
        e.preventDefault();
        updateSelected(i);
      });
    }
    updateSelected(0);
  }
}

function setupChineseDocsBanner() {
  if (!location.pathname.startsWith('/docs/')) return;
  const langs = navigator.languages || [navigator.language || ''];
  if (!langs.some(l => l.startsWith('zh'))) return;
  const dismissedAt = localStorage.getItem('cn-docs-banner-dismissed');
  // Dismiss for 30 days when clicking on the X.
  if (dismissedAt) {
    const elapsed = Date.now() - parseInt(dismissedAt, 10);
    if (elapsed < 30 * 24 * 60 * 60 * 1000) return;
  }
  const banner = document.createElement('div');
  banner.className = 'cn-docs-banner';
  const span = document.createElement('span');
  const link = document.createElement('a');
  link.href = 'https://gugu-perf.github.io/perfetto-docs-zh-cn/';
  link.target = '_blank';
  link.rel = 'noopener';
  link.textContent = 'gugu-perf.github.io/perfetto-docs-zh-cn';
  span.append(
    '\u{1F1E8}\u{1F1F3} A community-maintained Chinese translation of ' +
    'the Perfetto docs is available at ', link,
    '. This is a community project and is not affiliated with Google.');
  const btn = document.createElement('button');
  btn.title = 'Dismiss';
  btn.textContent = '\u00d7';
  btn.addEventListener('click', () => {
    banner.remove();
    localStorage.setItem('cn-docs-banner-dismissed', String(Date.now()));
  });
  banner.append(span, btn);
  const header = document.querySelector('body > header');
  header.insertAdjacentElement('afterend', banner);
}

window.addEventListener('DOMContentLoaded', () => {
  updateNav();
  updateTOC();
  setupChineseDocsBanner();
});

window.addEventListener("load", () => {
  setupSandwichMenu();
  initMermaid();

  // Don't smooth-scroll on pages that are too long (e.g. reference pages).
  if (document.body.scrollHeight < 10000) {
    document.documentElement.style.scrollBehavior = "smooth";
  } else {
    document.documentElement.style.scrollBehavior = "initial";
  }

  onloadFired = true;
  while (postLoadActions.length > 0) {
    postLoadActions.shift()();
  }

  updateTOC();
  setupSearch();
  setupTabs();

  // Enable animations only after the load event. This is to prevent glitches
  // when switching pages.
  document.documentElement.style.setProperty("--anim-enabled", "1");
});

// Handles redirects from the old docs.perfetto.dev.
const legacyRedirectMap = {
  "#/contributing": "/docs/contributing/getting-started#community",
  "#/build-instructions": "/docs/contributing/build-instructions",
  "#/testing": "/docs/contributing/testing",
  "#/app-instrumentation": "/docs/instrumentation/tracing-sdk",
  "#/recording-traces": "/docs/instrumentation/tracing-sdk#recording",
  "#/running": "/docs/quickstart/android-tracing",
  "#/long-traces": "/docs/concepts/config#long-traces",
  "#/detached-mode": "/docs/concepts/detached-mode",
  "#/heapprofd": "/docs/data-sources/native-heap-profiler",
  "#/java-hprof": "/docs/data-sources/java-heap-profiler",
  "#/trace-processor": "/docs/analysis/trace-processor",
  "#/analysis": "/docs/analysis/trace-processor#annotations",
  "#/metrics": "/docs/analysis/metrics",
  "#/traceconv": "/docs/quickstart/traceconv",
  "#/clock-sync": "/docs/concepts/clock-sync",
  "#/architecture": "/docs/concepts/service-model",
};

const fragment = location.hash.split("?")[0].replace(".md", "");
if (fragment in legacyRedirectMap) {
  location.replace(legacyRedirectMap[fragment]);
}

// Pages which have been been removed/renamed/moved and need to be redirected
// to their new home.
const redirectMap = {
  '/docs/analysis/common-queries': '/docs/getting-started/android-trace-analysis',
  '/docs/analysis/pivot-tables': '/docs/visualization/perfetto-ui#pivot-tables',
  '/docs/contributing/embedding': '/docs/analysis/trace-processor#embedding',
  '/docs/contributing/perfetto-in-the-press': '/docs/#who-uses-perfetto',
  '/docs/contributing/ui-development': '/docs/contributing/ui-getting-started',
  '/docs/quickstart/android-tracing': '/docs/getting-started/system-tracing',
  '/docs/quickstart/callstack-sampling': '/docs/getting-started/cpu-profiling',
  '/docs/quickstart/chrome-tracing': '/docs/getting-started/chrome-tracing',
  '/docs/quickstart/heap-profiling': '/docs/getting-started/memory-profiling',
  '/docs/quickstart/linux-tracing': '/docs/getting-started/system-tracing',
  '/docs/quickstart/trace-analysis': '/docs/analysis/getting-started',
};

if (location.pathname in redirectMap) {
  location.replace(redirectMap[location.pathname]);
}
