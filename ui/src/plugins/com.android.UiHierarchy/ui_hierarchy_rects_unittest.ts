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

import {
  boundsOf,
  pointInPoly,
  polygonArea,
  rectsIntersect,
} from './ui_hierarchy_rects';

const SQUARE = [
  {x: 0, y: 0},
  {x: 10, y: 0},
  {x: 10, y: 10},
  {x: 0, y: 10},
];

// A diamond inscribed in the 10x10 square.
const DIAMOND = [
  {x: 5, y: 0},
  {x: 10, y: 5},
  {x: 5, y: 10},
  {x: 0, y: 5},
];

describe('boundsOf', () => {
  test('bounding box of the points', () => {
    expect(boundsOf(DIAMOND)).toEqual({left: 0, top: 0, right: 10, bottom: 10});
    expect(
      boundsOf([
        {x: -3, y: 4},
        {x: 7, y: -2},
      ]),
    ).toEqual({left: -3, top: -2, right: 7, bottom: 4});
  });
});

describe('rectsIntersect', () => {
  const a = {left: 0, top: 0, right: 10, bottom: 10};

  test('overlapping and contained rects intersect', () => {
    expect(rectsIntersect(a, {left: 5, top: 5, right: 15, bottom: 15})).toBe(
      true,
    );
    expect(rectsIntersect(a, {left: 2, top: 2, right: 3, bottom: 3})).toBe(
      true,
    );
  });

  test('disjoint and edge-touching rects do not', () => {
    expect(rectsIntersect(a, {left: 20, top: 0, right: 30, bottom: 10})).toBe(
      false,
    );
    expect(rectsIntersect(a, {left: 10, top: 0, right: 20, bottom: 10})).toBe(
      false,
    );
  });
});

describe('pointInPoly', () => {
  test('inside and outside a square', () => {
    expect(pointInPoly({x: 5, y: 5}, SQUARE)).toBe(true);
    expect(pointInPoly({x: 15, y: 5}, SQUARE)).toBe(false);
  });

  test('corners of the square are outside the diamond', () => {
    expect(pointInPoly({x: 5, y: 5}, DIAMOND)).toBe(true);
    expect(pointInPoly({x: 1, y: 1}, DIAMOND)).toBe(false);
  });
});

describe('polygonArea', () => {
  test('area regardless of winding', () => {
    expect(polygonArea(SQUARE)).toBe(100);
    expect(polygonArea([...SQUARE].reverse())).toBe(100);
    expect(polygonArea(DIAMOND)).toBe(50);
  });

  test('degenerate polygons have no area', () => {
    expect(
      polygonArea([
        {x: 0, y: 0},
        {x: 5, y: 5},
        {x: 10, y: 10},
      ]),
    ).toBe(0);
  });
});
