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

import {hslToRgb, rgbToHsl} from '../../base/color';
import {clamp} from '../../base/math_utils';
import {getChartThemeColors} from '../../components/widgets/charts/chart_theme';
import {
  type ColorEncoding,
  type ColorMode,
  NULL_CATEGORY,
  NUMERIC_LUT_SIZE,
  TOP_N_CATEGORIES,
} from './types';

export interface ScatterTheme {
  readonly palette: ReadonlyArray<Uint8Array>;
  readonly uniform: Uint8Array;
  readonly nullColor: Uint8Array;
  readonly background: Uint8Array;
  readonly text: string;
  readonly grid: string;
}

export interface LegendEntry {
  readonly label: string;
  readonly css: string;
  readonly index: number;
  // Number of rows (full dataset, not just the viewport) in this entry.
  readonly count: number;
}

const DEFAULT_PALETTE_HEX = [
  '#4285f4',
  '#ea4335',
  '#fbbc04',
  '#34a853',
  '#ff6d01',
  '#46bdc6',
  '#9334e6',
  '#185abc',
];

export function parseCssColor(
  input: string,
  fallback: Uint8Array = new Uint8Array([0, 0, 0, 255]),
): Uint8Array {
  const str = input.trim();
  if (str.startsWith('#')) {
    const hex = str.slice(1);
    if (hex.length === 3 || hex.length === 4) {
      const r = parseInt(hex[0] + hex[0], 16);
      const g = parseInt(hex[1] + hex[1], 16);
      const b = parseInt(hex[2] + hex[2], 16);
      const a = hex.length === 4 ? parseInt(hex[3] + hex[3], 16) : 255;
      if (!Number.isNaN(r) && !Number.isNaN(g) && !Number.isNaN(b)) {
        return new Uint8Array([r, g, b, a]);
      }
    } else if (hex.length === 6 || hex.length === 8) {
      const r = parseInt(hex.slice(0, 2), 16);
      const g = parseInt(hex.slice(2, 4), 16);
      const b = parseInt(hex.slice(4, 6), 16);
      const a = hex.length === 8 ? parseInt(hex.slice(6, 8), 16) : 255;
      if (!Number.isNaN(r) && !Number.isNaN(g) && !Number.isNaN(b)) {
        return new Uint8Array([r, g, b, a]);
      }
    }
  }

  const rgbMatch = str.match(
    /^rgba?\(\s*([\d.]+)\s*,\s*([\d.]+)\s*,\s*([\d.]+)(?:\s*,\s*([\d.]+))?\s*\)$/i,
  );
  if (rgbMatch !== null) {
    const r = clamp(Math.round(parseFloat(rgbMatch[1])), 0, 255);
    const g = clamp(Math.round(parseFloat(rgbMatch[2])), 0, 255);
    const b = clamp(Math.round(parseFloat(rgbMatch[3])), 0, 255);
    const a =
      rgbMatch[4] !== undefined
        ? clamp(Math.round(parseFloat(rgbMatch[4]) * 255), 0, 255)
        : 255;
    return new Uint8Array([r, g, b, a]);
  }

  const lower = str.toLowerCase();
  if (lower === 'white') return new Uint8Array([255, 255, 255, 255]);
  if (lower === 'black') return new Uint8Array([0, 0, 0, 255]);
  if (lower === 'transparent') return new Uint8Array([0, 0, 0, 0]);
  if (lower === 'gray' || lower === 'grey') {
    return new Uint8Array([128, 128, 128, 255]);
  }
  return new Uint8Array(fallback);
}

export function readScatterTheme(el: Element): ScatterTheme {
  let chartTheme: ReturnType<typeof getChartThemeColors> | undefined;
  let neutralCss = '';
  try {
    if (
      typeof window !== 'undefined' &&
      typeof window.getComputedStyle === 'function'
    ) {
      chartTheme = getChartThemeColors(el);
      const style = window.getComputedStyle(el);
      neutralCss =
        style.getPropertyValue('--pf-chart-color-neutral').trim() ||
        style.getPropertyValue('--pf-color-text-hint').trim();
    }
  } catch {
    // Fallback defaults used in non-DOM environments.
  }

  const palette = DEFAULT_PALETTE_HEX.map((hex, i) => {
    const def = parseCssColor(hex);
    const cssVal = chartTheme?.chartColors[i] ?? '';
    return cssVal.length > 0 ? parseCssColor(cssVal, def) : def;
  });

  const resolve = (val: string | undefined, hexFallback: string) => {
    const def = parseCssColor(hexFallback);
    return val && val.length > 0 ? parseCssColor(val, def) : def;
  };

  return {
    palette,
    uniform: resolve(chartTheme?.accentColor, '#2667e7'),
    nullColor: resolve(neutralCss, '#9e9e9e'),
    background: resolve(chartTheme?.backgroundColor, '#ffffff'),
    text: chartTheme?.textColor || '#333333',
    grid: chartTheme?.borderColor || '#cccccc',
  };
}

const VIRIDIS_CONTROL_POINTS: ReadonlyArray<[number, number, number, number]> =
  [
    [0.0, 68, 1, 84],
    [0.125, 72, 35, 116],
    [0.25, 64, 67, 135],
    [0.375, 52, 94, 141],
    [0.5, 42, 120, 142],
    [0.625, 33, 145, 140],
    [0.75, 53, 183, 121],
    [0.875, 144, 215, 67],
    [1.0, 253, 231, 37],
  ];

export function createViridisLut(): Uint8Array {
  const lut = new Uint8Array(NUMERIC_LUT_SIZE * 4);
  const nPoints = VIRIDIS_CONTROL_POINTS.length;
  for (let i = 0; i < NUMERIC_LUT_SIZE; i++) {
    const t = i / (NUMERIC_LUT_SIZE - 1);
    let idx = 0;
    while (idx < nPoints - 2 && VIRIDIS_CONTROL_POINTS[idx + 1][0] < t) idx++;
    const p0 = VIRIDIS_CONTROL_POINTS[idx];
    const p1 = VIRIDIS_CONTROL_POINTS[idx + 1];
    const f = p1[0] > p0[0] ? (t - p0[0]) / (p1[0] - p0[0]) : 0;
    lut[i * 4 + 0] = clamp(Math.round(p0[1] + f * (p1[1] - p0[1])), 0, 255);
    lut[i * 4 + 1] = clamp(Math.round(p0[2] + f * (p1[2] - p0[2])), 0, 255);
    lut[i * 4 + 2] = clamp(Math.round(p0[3] + f * (p1[3] - p0[3])), 0, 255);
    lut[i * 4 + 3] = 255;
  }
  return lut;
}

let cachedViridisLut: Uint8Array | undefined;
function getCachedViridisLut(): Uint8Array {
  if (cachedViridisLut === undefined) cachedViridisLut = createViridisLut();
  return cachedViridisLut;
}

export function getCategoryColor(
  index: number,
  theme: ScatterTheme,
): Uint8Array {
  if (index >= TOP_N_CATEGORIES) return new Uint8Array([158, 158, 158, 255]);
  const palette = theme.palette;
  const baseCount = palette.length;
  if (index < baseCount) return new Uint8Array(palette[index]);

  const base = palette[index % baseCount];
  const [h, s, l] = rgbToHsl([base[0], base[1], base[2]]);
  if (index < baseCount * 2) {
    const newL = clamp(l > 50 ? l - 15 : l + 20, 25, 85);
    const [r, g, b] = hslToRgb((h + 35) % 360, s, newL);
    return new Uint8Array([r, g, b, 255]);
  }
  const newL = clamp(l + 10, 30, 80);
  const [r, g, b] = hslToRgb((h + 70) % 360, s, newL);
  return new Uint8Array([r, g, b, 255]);
}

export function buildColorEncoding(
  mode: ColorMode,
  theme: ScatterTheme,
  hidden: ReadonlySet<number>,
): ColorEncoding {
  const nullColor = new Uint8Array(theme.nullColor);
  if (hidden.has(NULL_CATEGORY)) nullColor[3] = 0;

  if (mode.kind === 'none') {
    return {
      kind: 'uniform',
      lut: new Uint8Array(theme.uniform),
      nullColor,
      min: 0,
      max: 0,
    };
  }
  if (mode.kind === 'numeric') {
    return {
      kind: 'numeric',
      lut: new Uint8Array(getCachedViridisLut()),
      nullColor,
      min: mode.min,
      max: mode.max,
    };
  }

  const catCount = mode.categories.length + (mode.hasOther ? 1 : 0);
  const lut = new Uint8Array(catCount * 4);
  for (let i = 0; i < catCount; i++) {
    const isOther = mode.hasOther && i === mode.categories.length;
    const color = getCategoryColor(isOther ? TOP_N_CATEGORIES : i, theme);
    lut[i * 4 + 0] = color[0];
    lut[i * 4 + 1] = color[1];
    lut[i * 4 + 2] = color[2];
    lut[i * 4 + 3] = hidden.has(i) ? 0 : color[3];
  }
  return {kind: 'categorical', lut, nullColor, min: 0, max: 0};
}

export function legendEntries(
  mode: ColorMode,
  encoding: ColorEncoding,
): LegendEntry[] {
  if (mode.kind !== 'categorical') return [];
  const lutCss = (i: number) =>
    `rgb(${encoding.lut[i * 4]}, ${encoding.lut[i * 4 + 1]}, ${encoding.lut[i * 4 + 2]})`;
  // Real categories sorted by count desc (ties keep colour order). The "Other"
  // and NULL buckets aren't categories, so they stay pinned at the bottom.
  const entries: LegendEntry[] = mode.categories
    .map((label, i) => ({
      label,
      css: lutCss(i),
      index: i,
      count: mode.counts[i] ?? 0,
    }))
    .sort((a, b) => b.count - a.count || a.index - b.index);
  if (mode.hasOther) {
    const idx = mode.categories.length;
    entries.push({
      label: 'Other',
      css: lutCss(idx),
      index: idx,
      count: mode.otherCount,
    });
  }
  if (mode.nullCount > 0) {
    const [nr, ng, nb] = encoding.nullColor;
    entries.push({
      label: 'NULL',
      css: `rgb(${nr}, ${ng}, ${nb})`,
      index: NULL_CATEGORY,
      count: mode.nullCount,
    });
  }
  return entries;
}

export function gradientCss(): string {
  const lut = getCachedViridisLut();
  const stops = [0, 64, 128, 192, 255].map((idx) => {
    const pct = Math.round((idx / 255) * 100);
    return `rgb(${lut[idx * 4]}, ${lut[idx * 4 + 1]}, ${lut[idx * 4 + 2]}) ${pct}%`;
  });
  return `linear-gradient(to right, ${stops.join(', ')})`;
}
