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

// Single source of truth for heading anchor ids: render.mjs writes the
// <a name="..."> on the page and search_index.mjs deep-links to it. Both
// must slugify the same input -- the heading's inline-rendered HTML, e.g.
// "Using <code>foo</code>" -- or a search result's #fragment won't match the
// page.
export function headingAnchor(renderedText, level) {
  // An explicit {#anchor} wins at any level.
  const explicit = /{#([\w-_.]+)}/.exec(renderedText);
  if (explicit) {
    return explicit[1];
  }
  // Otherwise infer the anchor from the text, but only for h2 and h3 (the only
  // levels the right-hand-side TOC links to).
  if (level >= 2 && level <= 3) {
    return (
      renderedText
        // Drop tag attributes, keep tag names. render.mjs rewrites link/image
        // hrefs (e.g. to source.chromium.org) but search_index.mjs doesn't, so
        // slugifying a raw href would make the two anchors disagree.
        // Attribute-less tags like <code> are untouched, so ids stay stable.
        .replace(/<([a-z][a-z0-9]*)\b[^>]*>/gi, "<$1>")
        .toLowerCase()
        .replace(/[^\w]+/g, "-")
        .replace(/[-]+/g, "-")
    ); // Drop consecutive '-'s.
  }
  return "";
}
