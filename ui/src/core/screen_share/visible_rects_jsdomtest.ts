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

import {Rect2D} from '../../base/geom';
import {VisibleRects} from './visible_rects';

let root: HTMLElement;
let defaults: HTMLStyleElement;

beforeEach(() => {
  // jsdom leaves properties nobody set empty, where a browser computes their
  // initial value, and doesn't expand the overflow shorthand.
  defaults = document.createElement('style');
  defaults.textContent =
    '* { overflow-x: visible; overflow-y: visible; position: static; }';
  document.head.appendChild(defaults);
  root = document.createElement('div');
  document.body.appendChild(root);
});

afterEach(() => {
  root.remove();
  defaults.remove();
});

// jsdom has no layout, so give an element a box by hand. Elements without
// one report no client rects, as hidden elements and display: contents
// wrappers do in a browser.
function layout(
  el: HTMLElement,
  left: number,
  top: number,
  right: number,
  bottom: number,
): HTMLElement {
  const rect = new DOMRect(left, top, right - left, bottom - top);
  el.getBoundingClientRect = () => rect;
  el.getClientRects = () => [rect] as unknown as DOMRectList;
  return el;
}

function div(style?: string): HTMLElement {
  const el = document.createElement('div');
  if (style !== undefined) el.setAttribute('style', style);
  return el;
}

const VIEWPORT = new Rect2D({left: 0, top: 0, right: 1000, bottom: 1000});

// The visible parts of `el`, as plain objects.
function visibleParts(el: Element) {
  return new VisibleRects(VIEWPORT)
    .of(el)
    .map(({left, top, right, bottom}) => ({left, top, right, bottom}));
}

describe('VisibleRects', () => {
  test("returns an element's box", () => {
    const el = layout(div(), 10, 10, 20, 20);
    root.append(el);
    expect(visibleParts(el)).toEqual([
      {left: 10, top: 10, right: 20, bottom: 20},
    ]);
  });

  test("measures a wrapper's children instead of the wrapper", () => {
    const wrapper = div('display: contents');
    wrapper.append(layout(div(), 0, 0, 10, 10), layout(div(), 0, 10, 10, 20));
    root.append(wrapper);
    expect(visibleParts(wrapper)).toEqual([
      {left: 0, top: 0, right: 10, bottom: 10},
      {left: 0, top: 10, right: 10, bottom: 20},
    ]);
  });

  test('looks through nested wrappers', () => {
    const outer = div('display: contents');
    const inner = div('display: contents');
    inner.append(layout(div(), 0, 0, 10, 10));
    outer.append(inner);
    root.append(outer);
    expect(visibleParts(outer)).toEqual([
      {left: 0, top: 0, right: 10, bottom: 10},
    ]);
  });

  test('a wrapper cuts nothing down, even if it sets overflow', () => {
    const wrapper = div(
      'display: contents; overflow-x: hidden; overflow-y: hidden',
    );
    const child = layout(div(), 0, 0, 10, 10);
    wrapper.append(child);
    root.append(wrapper);
    expect(visibleParts(child)).toEqual([
      {left: 0, top: 0, right: 10, bottom: 10},
    ]);
  });

  test('measures text directly inside a wrapper', () => {
    const wrapper = div('display: contents');
    wrapper.append('Title');
    root.append(wrapper);
    // jsdom can't measure text either.
    const rect = new DOMRect(5, 5, 30, 10);
    Range.prototype.getBoundingClientRect = () => rect;
    try {
      expect(visibleParts(wrapper)).toEqual([
        {left: 5, top: 5, right: 35, bottom: 15},
      ]);
    } finally {
      delete (Range.prototype as Partial<Range>).getBoundingClientRect;
    }
  });

  test('measures what is inside an element with no size', () => {
    const holder = layout(div(), 0, 0, 0, 0);
    holder.append(layout(div(), 40, 40, 60, 80));
    root.append(holder);
    expect(visibleParts(holder)).toEqual([
      {left: 40, top: 40, right: 60, bottom: 80},
    ]);
  });

  test('hidden elements have no visible parts', () => {
    const none = div('display: none');
    const invisible = layout(div('visibility: hidden'), 0, 0, 9, 9);
    root.append(none, invisible);
    expect(visibleParts(none)).toEqual([]);
    expect(visibleParts(invisible)).toEqual([]);
  });

  test('cuts a box down to an ancestor that scrolls', () => {
    const scroller = layout(
      div('overflow-x: auto; overflow-y: auto'),
      0,
      0,
      50,
      50,
    );
    const el = layout(div(), 0, 25, 50, 100);
    scroller.append(el);
    root.append(scroller);
    expect(visibleParts(el)).toEqual([
      {left: 0, top: 25, right: 50, bottom: 50},
    ]);
  });

  test('a box scrolled out of view has no visible parts', () => {
    const scroller = layout(
      div('overflow-x: auto; overflow-y: auto'),
      0,
      0,
      50,
      50,
    );
    const el = layout(div(), 0, 60, 50, 100);
    scroller.append(el);
    root.append(scroller);
    expect(visibleParts(el)).toEqual([]);
  });

  test('cuts a box down to the viewport', () => {
    const el = layout(div(), 990, 0, 1050, 10);
    root.append(el);
    expect(visibleParts(el)).toEqual([
      {left: 990, top: 0, right: 1000, bottom: 10},
    ]);
  });
});
