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

import type {Bounds2D, Rect2D} from '../../base/geom';

/**
 * Measures which parts of elements you can see on the screen.
 *
 * Most elements are measured by their own box, cut down by every ancestor that
 * clips or scrolls its contents, and by the viewport. A few need more care:
 * - A wrapper without a box of its own is measured through its children and
 *   text, and cuts nothing down.
 * - A fixed element is cut down only by the viewport.
 * - An element with no size is also measured through its children and text,
 *   which can still show outside it.
 * - A hidden element has no visible parts.
 *
 * To save time, it remembers what it has measured about each ancestor. So
 * each reading of the page needs a new one, because the page may have changed
 * since the last one.
 */
export class VisibleRects {
  // The part of the screen through which the contents of each element looked
  // at so far can be seen.
  private readonly visibleAreas = new Map<Element, Rect2D>();

  constructor(private readonly viewport: Rect2D) {}

  /** Returns the parts of `el` you can see. */
  of(el: Element): Rect2D[] {
    const style = getComputedStyle(el);
    if (style.display === 'contents') return this.ofContents(el);
    if (el.getClientRects().length === 0 || style.visibility === 'hidden') {
      return [];
    }
    const rect = el.getBoundingClientRect();
    if (rect.width === 0 || rect.height === 0) return this.ofContents(el);
    return visiblePart(rect, this.visibleAreaFor(el, style));
  }

  // Returns the visible parts of what's inside `el`.
  private ofContents(el: Element): Rect2D[] {
    const parts: Rect2D[] = [];
    for (const node of Array.from(el.childNodes)) {
      if (node instanceof Element) {
        // A child element: measure it like any other, so a wrapper inside a
        // wrapper works too.
        parts.push(...this.of(node));
      } else if (isVisibleText(node)) {
        // A piece of text: it has no element of its own, so measure the text
        // itself, cut down to what can be seen inside `el`.
        parts.push(...visiblePart(textBox(node), this.visibleAreaInside(el)));
      }
    }
    return parts;
  }

  // Returns the part of the screen through which `el` can be seen.
  private visibleAreaFor(el: Element, style: CSSStyleDeclaration): Rect2D {
    if (style.position === 'fixed') return this.viewport;
    return this.visibleAreaInside(el.parentElement);
  }

  // Returns the part of the screen through which the contents of `el` can be
  // seen: the part `el` itself can be seen through, cut down to `el`'s box if
  // `el` clips or scrolls its contents.
  private visibleAreaInside(el: Element | null): Rect2D {
    if (el === null || el === document.documentElement) return this.viewport;
    const known = this.visibleAreas.get(el);
    if (known) return known;
    const style = getComputedStyle(el);
    let area = this.visibleAreaFor(el, style);
    if (clipsItsContents(style)) {
      area = area.intersect(el.getBoundingClientRect());
    }
    this.visibleAreas.set(el, area);
    return area;
  }
}

// Returns the part of `rect` inside `visibleArea`, or nothing if none of it is.
function visiblePart(rect: Bounds2D, visibleArea: Rect2D): Rect2D[] {
  const part = visibleArea.intersect(rect);
  return part.width > 0 && part.height > 0 ? [part] : [];
}

// Returns true if an element hides whatever of its contents falls outside its
// box, as scrolling containers do.
function clipsItsContents(style: CSSStyleDeclaration): boolean {
  if (style.display === 'contents') return false;
  return style.overflowX !== 'visible' || style.overflowY !== 'visible';
}

// Returns true if `node` is text with something other than whitespace.
function isVisibleText(node: Node): node is Text {
  return node instanceof Text && node.data.trim() !== '';
}

// Returns the box around a piece of text.
function textBox(text: Text): DOMRect {
  const range = document.createRange();
  range.selectNodeContents(text);
  return range.getBoundingClientRect();
}
