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

// Web worker backing the docs search box (see setupSearch() in script.js).
// Fetching search_index.json and building the BM25 index tokenizes ~1.2MB of
// text, which is too slow to run on the main thread (it froze the box on the
// first keystroke, issue #6654). Doing it here keeps the page responsive: the
// main thread only sends a query string and renders the small result list we
// post back.

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

function buildSearchIndex(docs) {
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
  return {
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

// The built index, and the in-flight build promise (mirrors the singleton in
// the old main-thread loader: a failed build clears it so the next query
// retries).
let searchIndex = null;
let searchIndexPromise = null;

function ensureSearchIndex() {
  if (searchIndexPromise === null) {
    searchIndexPromise = fetch("/assets/search_index.json")
      .then((resp) => {
        if (!resp.ok) throw new Error(`search index HTTP ${resp.status}`);
        return resp.json();
      })
      .then((data) => {
        searchIndex = buildSearchIndex(data.docs);
        return searchIndex;
      })
      .catch((e) => {
        searchIndexPromise = null;  // Allow a retry on the next query.
        throw e;
      });
  }
  return searchIndexPromise;
}

self.addEventListener("message", (e) => {
  const {query, limit} = e.data;
  ensureSearchIndex()
    .then((idx) => {
      const terms = searchTokenize(query);
      // Return only what the main thread renders -- title, a body snippet and a
      // deep-link anchor -- not the full (large) doc bodies.
      const results = searchRank(idx, query, limit).map((doc) => ({
        u: doc.u,
        t: doc.t,
        snippet: searchSnippet(doc, terms),
        anchor: searchAnchor(doc, terms),
      }));
      self.postMessage({type: "results", query, terms, results});
    })
    .catch(() => {});  // Index failed to load; a later query retries.
});

// Start building as soon as the worker spawns, and tell the main thread when
// the index is ready so it can decide whether to show a "loading" placeholder.
ensureSearchIndex()
  .then(() => self.postMessage({type: "ready"}))
  .catch(() => {});
