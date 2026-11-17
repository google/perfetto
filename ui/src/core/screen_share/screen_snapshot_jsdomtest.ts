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

import {UNATTRIBUTED} from '../ownership';
import {ScreenSnapshot} from './screen_snapshot';

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

function div(attrs: {owner?: string; style?: string} = {}): HTMLElement {
  const el = document.createElement('div');
  if (attrs.owner !== undefined) el.setAttribute('data-plugin', attrs.owner);
  if (attrs.style !== undefined) el.setAttribute('style', attrs.style);
  return el;
}

// The boxes a snapshot of the page reads, as plain objects.
function readBoxes() {
  return ScreenSnapshot.take().boxes.map(({owner, bounds}) => ({
    owner,
    left: bounds.left,
    top: bounds.top,
    right: bounds.right,
    bottom: bounds.bottom,
  }));
}

describe('ScreenSnapshot.take', () => {
  test('returns each labeled element with its box, in page order', () => {
    const outer = layout(div({owner: 'outer'}), 0, 0, 100, 100);
    const inner = layout(div({owner: 'inner'}), 10, 10, 20, 20);
    outer.append(inner);
    root.append(outer);
    expect(readBoxes()).toEqual([
      {owner: 'outer', left: 0, top: 0, right: 100, bottom: 100},
      {owner: 'inner', left: 10, top: 10, right: 20, bottom: 20},
    ]);
  });

  test("credits a wrapper's children to the wrapper's owner", () => {
    const wrapper = div({owner: 'a', style: 'display: contents'});
    wrapper.append(layout(div(), 0, 0, 10, 10), layout(div(), 0, 10, 10, 20));
    root.append(wrapper);
    expect(readBoxes()).toEqual([
      {owner: 'a', left: 0, top: 0, right: 10, bottom: 10},
      {owner: 'a', left: 0, top: 10, right: 10, bottom: 20},
    ]);
  });

  test('a labeled child of a wrapper is listed again with its own owner', () => {
    const wrapper = div({owner: 'a', style: 'display: contents'});
    wrapper.append(layout(div({owner: 'b'}), 0, 0, 10, 10));
    root.append(wrapper);
    expect(readBoxes().map((b) => b.owner)).toEqual(['a', 'b']);
  });
});

describe('ScreenSnapshot', () => {
  const VIEWPORT = {left: 0, top: 0, right: 100, bottom: 100};
  const box = (
    owner: string,
    left: number,
    top: number,
    right: number,
    bottom: number,
  ) => ({owner, bounds: {left, top, right, bottom}});

  test('parts no box covers are unattributed', () => {
    const snapshot = new ScreenSnapshot(VIEWPORT, [box('a', 0, 0, 50, 50)]);
    expect(snapshot.findOwnerAt(20, 20)).toBe('a');
    expect(snapshot.findOwnerAt(70, 70)).toBe(UNATTRIBUTED);
  });

  test('shares are exact, in thousandths, including unattributed', () => {
    // The viewport is 100 x 100, so a thousandth is 10 square pixels.
    const snapshot = new ScreenSnapshot(VIEWPORT, [
      box('a', 0, 0, 50, 100),
      box('b', 50, 0, 100, 40),
      box('c', 20, 20, 70, 70),
    ]);
    expect(snapshot.computeShares()).toEqual(
      new Map([
        ['a', 350],
        ['b', 160],
        ['c', 250],
        [UNATTRIBUTED, 240],
      ]),
    );
  });

  test('shares add up to 1000 and leave out owners that round to zero', () => {
    const snapshot = new ScreenSnapshot(VIEWPORT, [
      box('a', 0, 0, 100, 100),
      box('tiny', 0, 0, 1, 1),
    ]);
    expect(snapshot.computeShares()).toEqual(new Map([['a', 1000]]));
  });

  test('an empty screen has no shares', () => {
    const empty = {left: 0, top: 0, right: 0, bottom: 0};
    expect(new ScreenSnapshot(empty, []).computeShares()).toEqual(new Map());
  });
});
