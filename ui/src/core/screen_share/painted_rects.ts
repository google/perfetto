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
import {type Cell, type CellSpan, Grid, type GridSize} from './grid';

/** A rectangle with a label. */
export interface LabeledRect {
  readonly bounds: Bounds2D;
  readonly label: string;
}

/**
 * Given a list of labeled rectangles that may overlap, paints them in list
 * order (later ones cover earlier ones), then lets the caller ask
 * findLabelAt(x, y) for the label at a point, and listRegions() for what's
 * visible, as rectangles that don't overlap.
 *
 * It works in three steps, using the painter's algorithm on an uneven Grid (a
 * technique known as coordinate compression):
 *
 * Step 1: draw Grid lines along every rectangle edge, so a rectangle covers
 * each cell either completely or not at all.
 *
 * Step 2: find the cells each rectangle covers (a CellSpan).
 *
 * Step 3: paint each rectangle, in order, in its own color onto the cells it
 * covers, so each cell ends up with the color of the last rectangle that
 * covers it.
 *
 * To find the label at a point, find the cell it's in, then the rectangle of
 * that cell's color.
 *
 * Cost: time and memory grow with the number of cells, which is at most
 * (2n − 1)² for n rectangles. With the 40–60 boxes visible on a real screen,
 * painting takes about 0.1 ms.
 */
export class PaintedRects {
  private readonly grid: Grid;
  private readonly paintedCells: PaintedCells;

  constructor(private readonly rects: ReadonlyArray<LabeledRect>) {
    const bounds: Bounds2D[] = rects.map((r) => r.bounds);
    // Step 1: draw grid lines along every edge.
    this.grid = new Grid(bounds);
    // Step 2: for each rectangle, find the cells it covers.
    const spans: CellSpan[] = bounds.map((b) =>
      this.grid.findCellsCoveredBy(b),
    );
    // Step 3: paint each rectangle in its own color, later ones over earlier.
    this.paintedCells = new PaintedCells({gridSize: this.grid.size, spans});
  }

  /**
   * Finds the label at (x, y), or undefined if no rectangle covers it.
   */
  findLabelAt(x: number, y: number): string | undefined {
    const cell: Cell | undefined = this.grid.findCellAt(x, y);
    if (cell === undefined) return undefined;
    return this.getLabel(this.paintedCells.getColorOf(cell));
  }

  /**
   * Lists the picture as rectangles that don't overlap, each with
   * its label. Together they cover every point some rectangle covers, but
   * there may be more of them than the fewest possible.
   */
  listRegions(): LabeledRect[] {
    const regions: LabeledRect[] = [];
    for (const run of this.paintedCells.listRunsOfSameColor()) {
      const label = this.getLabel(run.color);
      if (label === undefined) continue;
      regions.push({bounds: this.grid.getBounds(run.span), label});
    }
    return regions;
  }

  /** Measures how far (x, y) is from the nearest rectangle edge. */
  measureDistanceToEdge(x: number, y: number): number {
    return this.grid.measureDistanceToNearestLine(x, y);
  }

  // Gets the label of the rectangle painted in `color`: color i is rects[i].
  private getLabel(color: number | undefined): string | undefined {
    return color === undefined ? undefined : this.rects[color]?.label;
  }
}

// One run of neighboring cells in a row with the same color, which is
// undefined if nothing was painted on them.
interface Run {
  readonly span: CellSpan;
  readonly color: number | undefined;
}

// The cells of a Grid, each with the color painted on it last, if any. A color
// is just a number: span i is painted in color i, over whatever was there.
//
// E.g. on a 3 × 3 grid, painting columns 0–1 and rows 0–1 in color 0, then
// columns 1–2 and rows 1–2 in color 1, leaves (· is nothing):
//
//   0 0 ·
//   0 1 1
//   · 1 1
class PaintedCells {
  private readonly size: GridSize;
  // The color of each cell, as coloredCells[row][column], or undefined if
  // nothing was painted on it.
  private readonly coloredCells: Array<Array<number | undefined>>;

  // Paints `spans` onto a grid of `gridSize` cells in order: span i in color
  // i, so later spans paint over earlier ones.
  constructor(args: {gridSize: GridSize; spans: ReadonlyArray<CellSpan>}) {
    this.size = args.gridSize;
    this.coloredCells = Array.from({length: this.size.rows}, () =>
      new Array<number | undefined>(this.size.columns).fill(undefined),
    );
    args.spans.forEach((span, color) => this.paint(span, color));
  }

  // Gets the color of `cell`, or undefined if nothing was painted on it.
  getColorOf(cell: Cell): number | undefined {
    return this.coloredCells[cell.row]?.[cell.column];
  }

  // Lists each run of neighboring cells in a row that have the same color, as
  // a span one row high.
  //
  // E.g. a row with colors "0 1 1" has two runs: column 0, and columns 1–2.
  listRunsOfSameColor(): Run[] {
    const runs: Run[] = [];
    for (let row = 0; row < this.size.rows; row++) {
      const colors = this.coloredCells[row] ?? [];
      let start = 0;
      for (let column = 1; column <= colors.length; column++) {
        // Extend the run while the color stays the same.
        if (column < colors.length && colors[column] === colors[start]) {
          continue;
        }
        const span: CellSpan = {
          firstColumn: start,
          endColumn: column,
          firstRow: row,
          endRow: row + 1,
        };
        runs.push({span, color: colors[start]});
        start = column;
      }
    }
    return runs;
  }

  // Paints every cell of `span` in `color`.
  private paint(span: CellSpan, color: number): void {
    for (let row = span.firstRow; row < span.endRow; row++) {
      this.coloredCells[row]?.fill(color, span.firstColumn, span.endColumn);
    }
  }
}
