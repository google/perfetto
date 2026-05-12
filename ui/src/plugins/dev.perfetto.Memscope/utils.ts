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

import type {TsValue} from './sessions/live_session';

/** Maps counter samples to chart points with x in seconds since `t0` (ns). */
export function counterPoints(
  samples: TsValue[] | undefined,
  t0: number,
): {x: number; y: number}[] | undefined {
  if (samples === undefined || samples.length === 0) return undefined;
  return samples.map(({ts, value}) => ({x: (ts - t0) / 1e9, y: value}));
}

/**
 * Returns a Y-axis tick interval (in KB) that produces clean KB/MB/GB labels.
 * Targets ~5 ticks by picking the smallest interval from the sequence
 * 1, 2, 5, 10, 20, 50, 100, 200, 500 × {KB, MB, GB} that keeps tick count ≤ 6.
 *
 * Uses SI units (1 MB = 1000 KB, 1 GB = 1000 MB) to match `formatKb`.
 */
export function niceKbInterval(maxKb: number): number {
  if (maxKb <= 0) return 1;
  const rawInterval = maxKb / 5;
  const steps = [1, 2, 5, 10, 20, 50, 100, 200, 500];
  for (const unit of [1, 1000, 1000 * 1000]) {
    for (const step of steps) {
      const candidate = unit * step;
      if (candidate >= rawInterval) return candidate;
    }
  }
  return 1000 * 1000 * 500; // fallback: 500 GB
}

/** Returns the maximum y value across all series in a chart dataset. */
export function maxSeriesKb(
  series: ReadonlyArray<{readonly points: ReadonlyArray<{readonly y: number}>}>,
): number {
  let max = 0;
  for (const s of series) {
    for (const p of s.points) {
      if (p.y > max) max = p.y;
    }
  }
  return max;
}

export function formatKb(kb: number): string {
  if (kb < 1000) return `${kb.toLocaleString()} KB`;
  if (kb < 1000 * 1000) return `${(kb / 1000).toFixed(1)} MB`;
  return `${(kb / (1000 * 1000)).toFixed(1)} GB`;
}

/** Renders a KB value for a billboard with the unit in a smaller span. */
export function billboardKb(kb: number) {
  let value: string;
  let unit: string;
  if (kb < 1000) {
    value = kb.toLocaleString();
    unit = 'KB';
  } else if (kb < 1000 * 1000) {
    value = (kb / 1000).toFixed(1);
    unit = 'MB';
  } else {
    value = (kb / (1000 * 1000)).toFixed(1);
    unit = 'GB';
  }
  return {value, unit};
}
