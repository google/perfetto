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

import {Grid} from './grid';

// The example from the Grid comment: lines at x = 0, 10, 30, 40 and
// y = 0, 10, 20, 40. The repeated edges (0 and 40 from both rectangles and the
// area) get one line each.
const GRID = new Grid([
  {left: 0, top: 0, right: 40, bottom: 40},
  {left: 0, top: 0, right: 30, bottom: 20},
  {left: 10, top: 10, right: 40, bottom: 40},
]);

describe('Grid', () => {
  test('draws one line per distinct edge', () => {
    expect(GRID.size).toEqual({columns: 3, rows: 3});
  });

  test('a grid with no lines has no cells', () => {
    expect(new Grid([]).size).toEqual({columns: 0, rows: 0});
  });

  test('finds the cells a rectangle covers', () => {
    expect(
      GRID.findCellsCoveredBy({left: 10, top: 10, right: 40, bottom: 40}),
    ).toEqual({firstColumn: 1, endColumn: 3, firstRow: 1, endRow: 3});
  });

  test('finds the cell that contains a point', () => {
    expect(GRID.findCellAt(35, 15)).toEqual({column: 2, row: 1});
    expect(GRID.findCellAt(5, 30)).toEqual({column: 0, row: 2});
  });

  test('a point on a line belongs to the cell after it', () => {
    expect(GRID.findCellAt(10, 20)).toEqual({column: 1, row: 2});
  });

  test('a point outside the grid is in no cell', () => {
    expect(GRID.findCellAt(-5, 15)).toBeUndefined();
    expect(GRID.findCellAt(15, 40)).toBeUndefined();
  });

  test('gets the area a span covers', () => {
    expect(
      GRID.getBounds({firstColumn: 1, endColumn: 3, firstRow: 0, endRow: 1}),
    ).toEqual({left: 10, top: 0, right: 40, bottom: 10});
  });

  test('measures the distance to the nearest line', () => {
    expect(GRID.measureDistanceToNearestLine(12, 5)).toBe(2);
    expect(GRID.measureDistanceToNearestLine(20, 15)).toBe(5);
  });
});
