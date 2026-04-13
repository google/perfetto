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

/**
 * Returns a Y-axis tick interval (in KB) that produces clean KB/MB/GB labels.
 * Targets ~5 ticks by picking the smallest interval from the sequence
 * 1, 2, 5, 10, 20, 50, 100, 200, 500 × {KB, MB, GB} that keeps tick count ≤ 6.
 */
export function niceKbInterval(maxKb: number): number {
  if (maxKb <= 0) return 1;
  const rawInterval = maxKb / 5;
  const steps = [1, 2, 5, 10, 20, 50, 100, 200, 500];
  for (const unit of [1, 1024, 1024 * 1024]) {
    for (const step of steps) {
      const candidate = unit * step;
      if (candidate >= rawInterval) return candidate;
    }
  }
  return 1024 * 1024 * 512; // fallback: 512 GB
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
  if (kb < 1024) return `${kb.toLocaleString()} KB`;
  if (kb < 1024 * 1024) return `${(kb / 1024).toFixed(1)} MB`;
  return `${(kb / (1024 * 1024)).toFixed(1)} GB`;
}

/** Renders a KB value for a billboard with the unit in a smaller span. */
export function billboardKb(kb: number) {
  let value: string;
  let unit: string;
  if (kb < 1024) {
    value = kb.toLocaleString();
    unit = 'KB';
  } else if (kb < 1024 * 1024) {
    value = (kb / 1024).toFixed(1);
    unit = 'MB';
  } else {
    value = (kb / (1024 * 1024)).toFixed(1);
    unit = 'GB';
  }
  return {value, unit};
}
