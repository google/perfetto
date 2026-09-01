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

// The page-wide snapshot selection, shared between the composition timeline
// (which drives it) and the sections below (which render relative to it).
// Identified by absolute timestamp so each section — which loads its own
// snapshots/dumps — can correlate by nearest ts.

import type {time} from '../../../../base/time';

export interface MemSelection {
  // The inspected snapshot's timestamp.
  readonly sel: time;
  // When a range was brushed, the earlier baseline snapshot to diff against.
  readonly base?: time;
}

// The row of a ts-ascending series nearest to `ts`. Falls back to the last row
// when `ts` is undefined (no selection yet → sections show their latest data).
export function nearestByTs<T extends {ts: time}>(
  rows: readonly T[],
  ts?: time,
): T | undefined {
  if (rows.length === 0) return undefined;
  if (ts === undefined) return rows[rows.length - 1];
  let best = rows[0];
  let bestDist = ts > best.ts ? ts - best.ts : best.ts - ts;
  for (const r of rows) {
    const dist = ts > r.ts ? ts - r.ts : r.ts - ts;
    if (dist < bestDist) {
      best = r;
      bestDist = dist;
    }
  }
  return best;
}
