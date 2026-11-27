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

import {type LabeledRect, PaintedRects} from './painted_rects';

function rect(
  label: string,
  left: number,
  top: number,
  right: number,
  bottom: number,
): LabeledRect {
  return {label, bounds: {left, top, right, bottom}};
}

// Two overlapping rectangles on a 40 × 40 area: A, then B.
const A = rect('A', 0, 0, 30, 20);
const B = rect('B', 10, 10, 40, 40);

// The label in each cell of the A + B grid, row by row, '·' for none.
function labelsPerCell(painted: PaintedRects): string[] {
  const xs = [5, 20, 35];
  const ys = [5, 15, 30];
  return ys.map((y) =>
    xs.map((x) => painted.findLabelAt(x, y) ?? '·').join(''),
  );
}

describe('PaintedRects', () => {
  test('later rectangles end up on top', () => {
    expect(labelsPerCell(new PaintedRects([A, B]))).toEqual([
      'AA·',
      'ABB',
      '·BB',
    ]);
    expect(labelsPerCell(new PaintedRects([B, A]))).toEqual([
      'AA·',
      'AAB',
      '·BB',
    ]);
  });

  test('points no rectangle covers have no label', () => {
    const painted = new PaintedRects([A, B]);
    expect(painted.findLabelAt(35, 5)).toBeUndefined();
    expect(painted.findLabelAt(50, 50)).toBeUndefined();
  });

  test('a background rectangle labels the uncovered points', () => {
    const background = rect('bg', 0, 0, 40, 40);
    const painted = new PaintedRects([background, A, B]);
    expect(painted.findLabelAt(35, 5)).toBe('bg');
  });

  test('no rectangles means no labels and no regions', () => {
    const painted = new PaintedRects([]);
    expect(painted.findLabelAt(0, 0)).toBeUndefined();
    expect(painted.listRegions()).toEqual([]);
  });

  test('lists covered runs of cells with the same label as regions', () => {
    expect(new PaintedRects([A, B]).listRegions()).toEqual([
      {bounds: {left: 0, top: 0, right: 30, bottom: 10}, label: 'A'},
      {bounds: {left: 0, top: 10, right: 10, bottom: 20}, label: 'A'},
      {bounds: {left: 10, top: 10, right: 40, bottom: 20}, label: 'B'},
      {bounds: {left: 10, top: 20, right: 40, bottom: 40}, label: 'B'},
    ]);
  });

  test('measures the distance to the nearest rectangle edge', () => {
    const painted = new PaintedRects([A, B]);
    expect(painted.measureDistanceToEdge(12, 5)).toBe(2);
  });
});
