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

import {formatBytesIec} from '../../base/bytes_format';
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
 * Returns a Y-axis tick interval (in KB) that lands on power-of-2 (IEC)
 * boundaries, so labels rendered with `formatBytesIec` read as clean
 * KiB/MiB/GiB values. Targets ~5 ticks by picking the smallest interval from
 * 1, 2, 4, 8 … 512 × {KiB, MiB, GiB} (1 MiB = 1024 KiB) that keeps ticks ≤ 6.
 */
export function niceKbInterval(maxKb: number): number {
  if (maxKb <= 0) return 1;
  const rawInterval = maxKb / 5;
  const steps = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512];
  for (const unit of [1, 1024, 1024 * 1024]) {
    for (const step of steps) {
      const candidate = unit * step;
      if (candidate >= rawInterval) return candidate;
    }
  }
  return 1024 * 1024 * 512; // fallback: 512 GiB
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

/** Renders a byte value for a billboard with the unit in a smaller span. */
export function billboardBytes(bytes: number) {
  const [value, unit] = formatBytesIec(bytes).split(' ');
  return {value, unit};
}
