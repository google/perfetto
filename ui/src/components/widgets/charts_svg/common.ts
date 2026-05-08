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

import m from 'mithril';

// Number of --pf-chart-color-N CSS variables defined by the theme.
const CHART_COLOR_COUNT = 8;

// Series colour for the i-th series, as a CSS variable reference.
export function chartColorVar(i: number): string {
  return `var(--pf-chart-color-${(i % CHART_COLOR_COUNT) + 1})`;
}

export const TEXT_COLOR = 'var(--pf-color-text)';
export const BORDER_COLOR = 'var(--pf-color-border)';

// Default approximate tick count used when picking nice axis steps.
const APPROX_TICK_COUNT = 5;

export interface AxisRange {
  readonly min: number;
  readonly max: number;
  readonly ticks: ReadonlyArray<number>;
}

// Default short-form numeric formatter for tick labels and tooltips.
export function defaultFmt(v: number): string {
  if (!isFinite(v)) return String(v);
  if (Math.abs(v) >= 1e6) return (v / 1e6).toFixed(1) + 'M';
  if (Math.abs(v) >= 1e3) return (v / 1e3).toFixed(1) + 'k';
  if (Number.isInteger(v)) return v.toString();
  return v.toFixed(2);
}

// Strip floating-point fuzz introduced by repeated addition.
export function round(v: number, step: number): number {
  const decimals = Math.max(0, -Math.floor(Math.log10(step)));
  return Number(v.toFixed(decimals + 2));
}

// Rough text width estimate at 10px monospace-ish font (~6px per char).
// TODO(stevegolton): Replace this with proper text measurement - measureText()
// or maybe https://github.com/chenglou/pretext.
export function estimateLabelWidth(labels: ReadonlyArray<string>): number {
  let maxWidth = 0;
  for (const label of labels) {
    const width = label.length * 6;
    if (width > maxWidth) maxWidth = width;
  }
  return maxWidth;
}

// Axis range with exact bounds (no rounding to "nice" numbers). Tick
// positions are picked at nice round values that fall within the supplied
// bounds — including the bounds themselves at the ends.
export function rangeWithFixedBounds(
  boundMin: number,
  boundMax: number,
): AxisRange {
  if (!isFinite(boundMin) || !isFinite(boundMax) || boundMin === boundMax) {
    return {min: boundMin, max: boundMax, ticks: [boundMin, boundMax]};
  }
  const span = boundMax - boundMin;
  const rough = span / APPROX_TICK_COUNT;
  const mag = Math.pow(10, Math.floor(Math.log10(rough)));
  const norm = rough / mag;
  let step: number;
  if (norm < 1.5) step = 1 * mag;
  else if (norm < 3) step = 2 * mag;
  else if (norm < 7) step = 5 * mag;
  else step = 10 * mag;

  const ticks: number[] = [boundMin];
  const firstInner = Math.ceil(boundMin / step) * step;
  for (let t = firstInner; t < boundMax - step / 2; t += step) {
    if (t > boundMin + step / 4) ticks.push(round(t, step));
  }
  ticks.push(boundMax);
  return {min: boundMin, max: boundMax, ticks};
}

// Generate a "nice" axis range with round-number ticks.
// `integer` clamps the step to >= 1 and rounds tick values to integers.
// `minInterval` clamps the step to be at least that value (e.g. pass 1024 so
// ticks land on whole MB boundaries when data is in KB).
export function niceRange(
  rawMin: number,
  rawMax: number,
  opts: {integer?: boolean; minInterval?: number} = {},
): AxisRange {
  if (!isFinite(rawMin) || !isFinite(rawMax) || rawMin === rawMax) {
    const v = isFinite(rawMin) ? rawMin : 0;
    return {min: v - 1, max: v + 1, ticks: [v - 1, v, v + 1]};
  }
  const span = rawMax - rawMin;
  const rough = span / APPROX_TICK_COUNT;
  const mag = Math.pow(10, Math.floor(Math.log10(rough)));
  const norm = rough / mag;
  let step: number;
  if (norm < 1.5) step = 1 * mag;
  else if (norm < 3) step = 2 * mag;
  else if (norm < 7) step = 5 * mag;
  else step = 10 * mag;

  if (opts.integer && step < 1) step = 1;
  if (opts.minInterval !== undefined && step < opts.minInterval) {
    step = opts.minInterval;
  }

  const niceMin = Math.floor(rawMin / step) * step;
  const niceMax = Math.ceil(rawMax / step) * step;
  const ticks: number[] = [];
  for (let t = niceMin; t <= niceMax + step / 2; t += step) {
    ticks.push(opts.integer ? Math.round(t) : round(t, step));
  }
  return {min: niceMin, max: niceMax, ticks};
}

// Log-scale axis range. Bounds are snapped to powers of 10; ticks land at
// each power of 10 within the range. Caller must ensure both bounds > 0.
export function logRange(rawMin: number, rawMax: number): AxisRange {
  if (!isFinite(rawMin) || !isFinite(rawMax) || rawMin <= 0 || rawMax <= 0) {
    return {min: 1, max: 10, ticks: [1, 10]};
  }
  const lo = Math.floor(Math.log10(rawMin));
  const hi = Math.ceil(Math.log10(rawMax));
  const ticks: number[] = [];
  for (let p = lo; p <= hi; p++) ticks.push(Math.pow(10, p));
  return {min: Math.pow(10, lo), max: Math.pow(10, hi), ticks};
}

// A point marker: a white dot ringed in the series color.
export function pointMarker(cx: number, cy: number, color: string, r: number) {
  return m('circle', {
    'cx': cx,
    'cy': cy,
    'r': r,
    'fill': '#fff',
    'stroke': color,
    'stroke-width': 2,
  });
}
