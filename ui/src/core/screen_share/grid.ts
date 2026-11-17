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

import type {Bounds2D} from '../../base/geom';

/** One cell of a Grid: its column and row, counted from 0. */
export interface Cell {
  readonly column: number;
  readonly row: number;
}

/** How many columns and rows a Grid has. */
export interface GridSize {
  readonly columns: number;
  readonly rows: number;
}

/**
 * A block of cells in a Grid: the columns from firstColumn up to, but not
 * including, endColumn, and the rows from firstRow up to endRow.
 *
 * E.g. {firstColumn: 1, endColumn: 3, firstRow: 0, endRow: 1} is the two
 * cells in row 0, columns 1 and 2.
 */
export interface CellSpan {
  readonly firstColumn: number;
  readonly endColumn: number;
  readonly firstRow: number;
  readonly endRow: number;
}

/**
 * An uneven grid: its lines are drawn at chosen positions rather than at even
 * steps, so each column and each row can have a different size. A cell is
 * where a column and a row cross.
 *
 * Drawing the lines along the edges of some rectangles makes every rectangle
 * cover whole cells, so they can be handled cell by cell instead of pixel by
 * pixel.
 *
 * E.g. lines along the edges of A (0, 0)–(30, 20) and B (10, 10)–(40, 40)
 * make 3 columns and 3 rows of different sizes:
 *
 *                           x: 0    10        30   40
 *   ┌──────────────┐           ┌────┬─────────┬────┐ y = 0
 *   │ A            │           │    │         │    │
 *   │    ┌─────────┼────┐      ├────┼─────────┼────┤ y = 10
 *   │    │         │    │      │    │         │    │
 *   └────┼─────────┘    │      ├────┼─────────┼────┤ y = 20
 *        │              │      │    │         │    │
 *        │              │      │    │         │    │
 *        │            B │      │    │         │    │
 *        └──────────────┘      └────┴─────────┴────┘ y = 40
 */
export class Grid {
  private readonly columns: GridLines;
  private readonly rows: GridLines;

  /** Draws a grid with a line along every edge of `boxes`. */
  constructor(boxes: ReadonlyArray<Bounds2D>) {
    this.columns = new GridLines(boxes.flatMap((b) => [b.left, b.right]));
    this.rows = new GridLines(boxes.flatMap((b) => [b.top, b.bottom]));
  }

  get size(): GridSize {
    return {columns: this.columns.cellCount, rows: this.rows.cellCount};
  }

  /**
   * Finds the cells `bounds` covers. Its edges must be lines of this grid.
   *
   * E.g. in the grid above, (10, 10)–(40, 40) covers columns 1–2 and rows 1–2.
   */
  findCellsCoveredBy(bounds: Bounds2D): CellSpan {
    return {
      firstColumn: this.columns.findLastLineAtOrBefore(bounds.left),
      endColumn: this.columns.findLastLineAtOrBefore(bounds.right),
      firstRow: this.rows.findLastLineAtOrBefore(bounds.top),
      endRow: this.rows.findLastLineAtOrBefore(bounds.bottom),
    };
  }

  /**
   * Finds the cell that contains the point (x, y), or undefined if the point
   * is outside the grid. A point on a line belongs to the cell after it.
   */
  findCellAt(x: number, y: number): Cell | undefined {
    const column = this.columns.findCellContaining(x);
    const row = this.rows.findCellContaining(y);
    if (column === undefined || row === undefined) return undefined;
    return {column, row};
  }

  /** Gets the area the cells in `span` cover. */
  getBounds(span: CellSpan): Bounds2D {
    return {
      left: this.columns.getStart(span.firstColumn),
      right: this.columns.getStart(span.endColumn),
      top: this.rows.getStart(span.firstRow),
      bottom: this.rows.getStart(span.endRow),
    };
  }
}

// The lines of a Grid in one direction, as positions: sorted, without repeats.
// Line i and line i + 1 bound cell i in that direction, which is a column or a
// row.
//
// E.g. lines at [0, 10, 30, 40] make 3 cells: 0–10, 10–30 and 30–40.
class GridLines {
  private readonly lines: ReadonlyArray<number>;

  // Draws a line at each distinct position.
  constructor(positions: ReadonlyArray<number>) {
    this.lines = Array.from(new Set(positions)).sort((a, b) => a - b);
  }

  get cellCount(): number {
    return Math.max(0, this.lines.length - 1);
  }

  // Gets where line number `line` is, which is where cell `line` starts.
  getStart(line: number): number {
    return this.lines[line] ?? 0;
  }

  // Finds the cell that contains `position`, or undefined if `position` is
  // before the first line or at or after the last one.
  findCellContaining(position: number): number | undefined {
    const first = this.lines[0] ?? 0;
    const last = this.lines[this.lines.length - 1] ?? 0;
    if (position < first || position >= last) return undefined;
    return this.findLastLineAtOrBefore(position);
  }

  // Finds the number of the last line at or before `position` by binary
  // search, or 0 if `position` is before every line. For a position on a
  // line, that's the line itself.
  findLastLineAtOrBefore(position: number): number {
    let lo = 0;
    let hi = this.lines.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if ((this.lines[mid] ?? 0) <= position) lo = mid;
      else hi = mid - 1;
    }
    return lo;
  }
}
