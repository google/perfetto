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

import {vi} from 'vitest';
import {ownershipOverlay} from './ownership_debug';

// jsdom has no layout: no element has a box, so the boxes leave the whole
// screen unattributed. The point lookup is faked to find a labeled element
// in the left half of the window, so the two disagree there.
let labeled: HTMLElement;
const elementFromPoint = document.elementFromPoint;

beforeEach(() => {
  labeled = document.createElement('div');
  labeled.setAttribute('data-plugin', 'com.example.A');
  document.body.append(labeled);
  document.elementFromPoint = (x: number) =>
    x < window.innerWidth / 2 ? labeled : null;
  // jsdom has no canvas.
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null);
});

afterEach(() => {
  document.elementFromPoint = elementFromPoint;
  labeled.remove();
  vi.restoreAllMocks();
});

function legendRows(): string[][] {
  return Array.from(
    document.querySelectorAll('.pf-ownership-overlay__row'),
  ).map((row) => Array.from(row.children).map((c) => c.textContent ?? ''));
}

describe('ownershipOverlay', () => {
  test("lists each owner's share of the screen", () => {
    ownershipOverlay.toggle();
    expect(legendRows()[0]).toEqual(['', 'unattributed', '1000']);
    ownershipOverlay.toggle();
  });

  test('lists where the boxes and the point lookup disagree', () => {
    ownershipOverlay.toggle();
    const titles = Array.from(
      document.querySelectorAll('.pf-ownership-overlay__title'),
    ).map((t) => t.textContent);
    expect(titles[1]).toMatch(/^Disagreements: [1-9]\d* of \d+ points$/);
    expect(legendRows()[1]?.[1]).toBe(
      'div: page com.example.A, snapshot unattributed',
    );
    ownershipOverlay.toggle();
  });

  test('is removed when toggled off', () => {
    ownershipOverlay.toggle();
    expect(document.querySelector('.pf-ownership-overlay')).not.toBeNull();
    ownershipOverlay.toggle();
    expect(document.querySelector('.pf-ownership-overlay')).toBeNull();
  });
});
