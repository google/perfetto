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

import type {DataBounds, PointColumns, PointSet} from './types';

let nextPointSetId = 1;

export interface NearestResult {
  readonly index: number;
  readonly distPx: number;
}

/**
 * Wraps 2D LOD operator viewport columns into an immutable PointSet.
 */
export function buildOperatorPointSet(
  cols: PointColumns,
  bounds: DataBounds,
): PointSet {
  return {
    id: nextPointSetId++,
    count: cols.x.length,
    bounds,
    x: cols.x,
    y: cols.y,
    row: cols.row,
    color: cols.color,
    weight: cols.weight,
  };
}

/**
 * Finds the nearest point to a cursor position within a pixel radius.
 * Ties prefer larger indices (drawn later / on top). Points for which `skip`
 * returns true (e.g. hidden categories) are ignored.
 */
export function nearest(
  set: PointSet,
  cursorX: number,
  cursorY: number,
  scaleX: number,
  scaleY: number,
  radiusPx: number,
  skip?: (index: number) => boolean,
): NearestResult | undefined {
  if (radiusPx < 0 || scaleX <= 0 || scaleY <= 0 || set.count === 0) {
    return undefined;
  }

  const maxDistSq = radiusPx * radiusPx;
  let bestDistSq = maxDistSq;
  let bestIdx = -1;
  const {x, y, count} = set;

  for (let i = 0; i < count; i++) {
    const dPx = (x[i] - cursorX) * scaleX;
    const dPy = (y[i] - cursorY) * scaleY;
    const distSq = dPx * dPx + dPy * dPy;
    if (
      distSq <= maxDistSq &&
      (distSq < bestDistSq || (distSq === bestDistSq && i > bestIdx)) &&
      (skip === undefined || !skip(i))
    ) {
      bestDistSq = distSq;
      bestIdx = i;
    }
  }

  return bestIdx === -1
    ? undefined
    : {index: bestIdx, distPx: Math.sqrt(bestDistSq)};
}
