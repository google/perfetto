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

const TAG_PREFIX = 'tag-';
const TAG_LABELS = {'cpp-rust': 'C++/Rust'};
const NAV_DEFAULT_TAG = 'android';

function tagLabel(tag) {
  return TAG_LABELS[tag] || tag.charAt(0).toUpperCase() + tag.slice(1);
}

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

  // --- Step 1: Discover available tags from CSS classes on <li> elements. ---
  const tagOrder = [];
  const tagSeen = new Set();
  for (const li of rootUl.querySelectorAll('li')) {
    for (const cls of li.classList) {
      if (cls.startsWith(TAG_PREFIX)) {
        const tag = cls.slice(TAG_PREFIX.length);
        if (!tagSeen.has(tag)) {
          tagSeen.add(tag);
          tagOrder.push(tag);
        }
      }
    }
  }

  // --- Step 2: Load single active tag from session ('' = show all). ---
  let activeTag = sessionStorage.getItem('docs.nav.activeTag');
  if (activeTag === null) {
    activeTag = NAV_DEFAULT_TAG;
  } else if (activeTag !== '' && !tagSeen.has(activeTag)) {
    activeTag = NAV_DEFAULT_TAG;
  }

  // --- Step 3: Build/refresh the audience chip bar (single-select + All). ---
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

  const makeChip = (label, tag) => {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'audience-tag';
    btn.textContent = label;
    if (tag === activeTag) btn.classList.add('active');
    btn.addEventListener('click', () => {
      sessionStorage.setItem('docs.nav.activeTag', tag);
      updateNav();
    });
    chipBar.appendChild(btn);
  };
  for (const tag of tagOrder) makeChip(tagLabel(tag), tag);
  makeChip('All', '');

  // --- Step 4: Filter leaves by active tag, then collapse empty ancestors.
  // Visibility is decided bottom-up so parents with no visible children hide.
  // Exception: the leaf for the currently-viewed page is always kept visible
  // (and its ancestors stay expanded) so the user never loses their place in
  // the sidebar when the active chip would otherwise hide it. ---
  const activeCls = activeTag ? TAG_PREFIX + activeTag : '';
  const isTagged = (li) => {
    for (const c of li.classList) if (c.startsWith(TAG_PREFIX)) return true;
    return false;
  };
  const isCurrentPageLeaf = (li) => {
    if (!curFileName) return false;
    if (li.querySelector(':scope > ul')) return false;
    const a = li.querySelector(':scope a[href]');
    if (!a) return false;
    try {
      const p = new URL(a.href).pathname;
      return p === curFileName || p + 'index.html' === curFileName;
    } catch (_) {
      return false;
    }
  };
  const process = (li) => {
    const childUl = li.querySelector(':scope > ul');
    if (!childUl) {
      const vis = !isTagged(li) || !activeCls || li.classList.contains(activeCls)
                  || isCurrentPageLeaf(li);
      li.style.display = vis ? '' : 'none';
      return vis;
    }
    let any = false;
    for (const child of childUl.querySelectorAll(':scope > li')) {
      if (process(child)) any = true;
    }
    li.style.display = any ? '' : 'none';
    return any;
  };
  for (const li of rootUl.querySelectorAll(':scope > li')) process(li);

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

// ---------------------------------------------------------------------------
// Client-side docs search. We ship a build-time full-text index
// (assets/search_index.json, built by src/gen_search_index.js) and rank it
// locally with BM25. The old Google Custom Search Engine couldn't see a doc
// until Googlebot crawled it, often weeks after it shipped.
// ---------------------------------------------------------------------------

// The parsed index, loaded lazily on first interaction (see loadSearchIndex).
let searchIndex = null;
let searchIndexPromise = null;

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

async function loadSearchIndex() {
  const resp = await fetch("/assets/search_index.json");
  if (!resp.ok) throw new Error(`search index HTTP ${resp.status}`);
  const docs = (await resp.json()).docs;

  // Field boosts: a hit in the title matters far more than one in the body.
  const FIELD_BOOST = {title: 8, heading: 4, body: 1};
  const postings = new Map();
  const docLen = new Array(docs.length).fill(0);
  const titleTokens = new Array(docs.length);
  const navTokens = new Array(docs.length);
  const addTokens = (docIdx, tokens, boost) => {
    for (const tok of tokens) {
      let postingList = postings.get(tok);
      if (postingList === undefined) {
        postingList = new Map();
        postings.set(tok, postingList);
      }
      postingList.set(docIdx, (postingList.get(docIdx) || 0) + boost);
      docLen[docIdx] += boost;
    }
  };
  for (let i = 0; i < docs.length; i++) {
    const d = docs[i];
    const tt = searchTokenize(d.t || "");
    titleTokens[i] = tt;
    addTokens(i, tt, FIELD_BOOST.title);
    // The toc.md nav label (d.n), when it differs from the title, is a curated
    // keyword alias -- index it at title weight too.
    if (d.n) {
      navTokens[i] = searchTokenize(d.n);
      addTokens(i, navTokens[i], FIELD_BOOST.title);
    }
    // The URL slug (last path segment, e.g. "perfetto-cli", "stdlib-docs") is a
    // curated keyword, often searched even when the word never appears in the
    // prose. searchTokenize splits it on the "-"/"_"; index it at title weight.
    const slug = d.u.split("/").filter(Boolean).pop() || "";
    addTokens(i, searchTokenize(slug), FIELD_BOOST.title);
    for (const h of d.h || []) addTokens(i, searchTokenize(h.t), FIELD_BOOST.heading);
    if (d.b) addTokens(i, searchTokenize(d.b), FIELD_BOOST.body);
  }
  let total = 0;
  for (const l of docLen) total += l;
  searchIndex = {
    docs,
    postings,
    // Sorted so searchPostings() can binary-search the prefix range.
    sortedTerms: [...postings.keys()].sort(),
    docLen,
    titleTokens,
    navTokens,
    avgdl: docLen.length ? total / docLen.length : 1,
    N: docs.length,
  };
  return searchIndex;
}

// Index of the first element of the sorted array `arr` that is >= `key`.
function lowerBound(arr, key) {
  let lo = 0;
  let hi = arr.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (arr[mid] < key) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

function ensureSearchIndex() {
  if (searchIndexPromise === null) {
    searchIndexPromise = loadSearchIndex().catch((e) => {
      searchIndexPromise = null;  // Allow a retry on the next keystroke.
      throw e;
    });
  }
  return searchIndexPromise;
}

// Returns {df, tf: Map(docIdx -> weightedTf)} for a query term. The final,
// still-being-typed term also matches everything it prefixes, so "trace"
// surfaces "traceconv" and "tracing"; earlier terms match exactly.
function searchPostings(idx, term, allowPrefix) {
  const exact = idx.postings.get(term);
  if (!allowPrefix) return exact === undefined ? null : {df: exact.size, tf: exact};
  const merged = new Map();
  const union = (postingList) => {
    for (const [docIdx, w] of postingList) merged.set(docIdx, (merged.get(docIdx) || 0) + w);
  };
  if (exact !== undefined) union(exact);
  // Prefix matches are a contiguous run in sortedTerms: walk from the first
  // term >= `term` and stop at the first miss.
  let matched = 0;
  for (let i = lowerBound(idx.sortedTerms, term); i < idx.sortedTerms.length; i++) {
    const t = idx.sortedTerms[i];
    if (!t.startsWith(term)) break;
    if (t === term) continue;
    union(idx.postings.get(t));
    if (++matched >= 200) break;  // Cap fan-out on very short prefixes.
  }
  return merged.size ? {df: merged.size, tf: merged} : null;
}

// Ranks docs against the query with BM25. Returns up to `limit` doc objects.
function searchRank(idx, query, limit) {
  const terms = searchTokenize(query);
  if (terms.length === 0) return [];
  const k1 = 1.2;
  const b = 0.75;
  const scores = new Map();
  for (let ti = 0; ti < terms.length; ti++) {
    const p = searchPostings(idx, terms[ti], ti === terms.length - 1);
    if (p === null) continue;
    // BM25, the standard ranking function: https://en.wikipedia.org/wiki/Okapi_BM25
    //   idf  -- rarer terms (matching fewer docs, `df`) count for more.
    //   score -- rises with term frequency `tf` but saturates (`k1`), and is
    //            penalised for long docs (`b`, via dl/avgdl) so a short focused
    //            page outranks a long one that just repeats the word.
    const idf = Math.log(1 + (idx.N - p.df + 0.5) / (p.df + 0.5));
    for (const [docIdx, tf] of p.tf) {
      // Floor the length used for normalization so near-empty pages (e.g. the
      // body-less generated reference pages) don't win on BM25's short-doc bonus.
      const dl = Math.max(idx.docLen[docIdx], 0.5 * idx.avgdl);
      const s = idf * (tf * (k1 + 1)) / (tf + k1 * (1 - b + b * dl / idx.avgdl));
      scores.set(docIdx, (scores.get(docIdx) || 0) + s);  // sum over query terms
    }
  }
  // Boost title matches. Past an exact title, reward how much of the title the
  // query covers -- so a short "Perfetto UI" beats a long "...Perfetto UI..." --
  // with a small extra bump when the query leads the title, which keeps "Batch
  // Trace Processor" below "Trace Processor (C++)". Tokenized so title
  // punctuation can't block a match; the nav label counts as a title too, so
  // "boot tracing" finds the page labelled "Boot Tracing".
  const phrase = terms.join(" ");
  const titleBoost = (toks) => {
    const norm = toks.join(" ");
    if (norm === phrase) return 12;
    if (!terms.every((t) => toks.includes(t))) return norm.includes(phrase) ? 3 : 0;
    return 3 + 6 * (terms.length / toks.length) + (norm.startsWith(phrase + " ") ? 3 : 0);
  };
  for (const docIdx of scores.keys()) {
    let boost = titleBoost(idx.titleTokens[docIdx]);
    const nav = idx.navTokens[docIdx];
    if (nav) boost = Math.max(boost, titleBoost(nav));
    if (boost) scores.set(docIdx, scores.get(docIdx) + boost);
  }
  return [...scores.entries()]
    .sort((a, b) => b[1] - a[1])
    .slice(0, limit)
    .map(([docIdx]) => idx.docs[docIdx]);
}

function escapeRegExp(s) {
  return s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
}

// Appends `text` to `el`, wrapping any occurrence of a query term in <mark>.
// Builds DOM text nodes (never innerHTML) so index content can't inject markup.
function appendHighlighted(el, text, terms) {
  if (terms.length === 0) {
    el.appendChild(document.createTextNode(text));
    return;
  }
  const re = new RegExp("(" + terms.map(escapeRegExp).join("|") + ")", "ig");
  let last = 0;
  let m;
  while ((m = re.exec(text)) !== null) {
    if (m.index > last) {
      el.appendChild(document.createTextNode(text.slice(last, m.index)));
    }
    const mark = document.createElement("mark");
    mark.textContent = m[0];
    el.appendChild(mark);
    last = m.index + m[0].length;
    if (m.index === re.lastIndex) re.lastIndex++;  // Guard against empty match.
  }
  if (last < text.length) el.appendChild(document.createTextNode(text.slice(last)));
}

// Extracts a ~180 char window of body text centred on the first term match.
function searchSnippet(doc, terms) {
  const text = doc.b || (doc.h || []).map((h) => h.t).join(" · ") || doc.t || "";
  const lower = text.toLowerCase();
  let pos = -1;
  for (const t of terms) {
    const i = lower.indexOf(t);
    if (i !== -1 && (pos === -1 || i < pos)) pos = i;
  }
  if (pos === -1) pos = 0;
  const start = Math.max(0, pos - 40);
  let out = text.slice(start, start + 180);
  if (start > 0) out = "…" + out;
  if (start + 180 < text.length) out += "…";
  return out;
}

// If a query term matches a heading with an anchor, deep-link to it.
function searchAnchor(doc, terms) {
  for (const h of doc.h || []) {
    if (h.a && terms.some((t) => h.t.toLowerCase().includes(t))) return h.a;
  }
  return "";
}

function setupSearch() {
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

  let results = [];
  let selected = -1;

  const highlightSelected = () => {
    const items = searchRes.children;
    for (let i = 0; i < items.length; i++) {
      items[i].classList.toggle("sr-selected", i === selected);
    }
    if (selected >= 0 && items[selected]) {
      items[selected].scrollIntoView({block: "nearest"});
    }
  };

  const render = (query) => {
    const terms = searchTokenize(query);
    searchRes.style.width = `${searchBox.offsetWidth}px`;
    searchRes.innerHTML = "";
    selected = -1;
    for (const doc of results) {
      const anchor = searchAnchor(doc, terms);
      const link = document.createElement("a");
      link.href = doc.u + (anchor ? "#" + anchor : "");

      const title = document.createElement("div");
      title.className = "sr-title";
      appendHighlighted(title, doc.t, terms);
      link.appendChild(title);

      const snippet = document.createElement("div");
      snippet.className = "sr-snippet";
      appendHighlighted(snippet, searchSnippet(doc, terms), terms);
      link.appendChild(snippet);

      const div = document.createElement("div");
      div.appendChild(link);
      searchRes.appendChild(div);
    }
  };

  const runSearch = async () => {
    const query = searchBox.value.trim();
    if (query === "") {
      results = [];
      searchRes.innerHTML = "";
      return;
    }
    let idx;
    try {
      idx = await ensureSearchIndex();
    } catch (e) {
      return;  // Index failed to load; leave the box empty rather than error.
    }
    if (searchBox.value.trim() !== query) return;  // A newer query superseded us.
    results = searchRank(idx, query, 8);
    render(query);
  };

  // Start fetching the index as soon as the user engages with the box.
  searchBox.addEventListener("focus", () => { ensureSearchIndex().catch(() => {}); },
                             {once: true});

  let timerId = -1;
  searchBox.addEventListener("input", () => {
    if (timerId >= 0) clearTimeout(timerId);
    timerId = setTimeout(() => { timerId = -1; runSearch(); }, 120);
  });

  searchBox.addEventListener("keydown", (e) => {
    if (results.length === 0) return;
    if (e.key === "ArrowDown") {
      selected = Math.min(results.length - 1, selected + 1);
      highlightSelected();
      e.preventDefault();
    } else if (e.key === "ArrowUp") {
      selected = Math.max(0, selected - 1);
      highlightSelected();
      e.preventDefault();
    } else if (e.key === "Enter") {
      const link = searchRes.querySelectorAll("a")[selected >= 0 ? selected : 0];
      if (link) window.location.href = link.href;
    }
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

// Wires up the "copy" button overlaid on each code block (see renderCode() in
// markdown_render.js).
function setupCodeCopy() {
  for (const btn of document.querySelectorAll(".code-copy-button")) {
    const code = btn.parentElement.querySelector("code");
    if (!code) continue;
    btn.addEventListener("click", async () => {
      try {
        await navigator.clipboard.writeText(code.textContent);
      } catch (e) {
        return;
      }
      btn.classList.add("copied");
      setTimeout(() => btn.classList.remove("copied"), 1500);
    });
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
  setupCodeCopy();

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
  '/docs/case-studies/android-boot-tracing': '/docs/getting-started/local-android-trace-recording#boot-tracing',
  '/docs/case-studies/android-outofmemoryerror': '/docs/getting-started/local-android-trace-recording#oom-heap-dump',
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
