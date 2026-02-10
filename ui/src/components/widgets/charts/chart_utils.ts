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
 * Generate nice tick values for a chart axis.
 *
 * @param min Minimum value of the axis range.
 * @param max Maximum value of the axis range.
 * @param count Number of ticks to generate.
 * @param integer When true, ticks are rounded to integers and deduplicated.
 */
export function generateTicks(
  min: number,
  max: number,
  count: number,
  integer = false,
): number[] {
  if (min === max) return [min];
  if (count <= 1) return [min];

  const range = max - min;
  const step = range / (count - 1);
  const ticks: number[] = [];

  for (let i = 0; i < count; i++) {
    let tick = min + i * step;
    if (integer) tick = Math.round(tick);
    ticks.push(tick);
  }

  // Deduplicate when rounding creates duplicates (small integer ranges)
  if (integer) {
    return [...new Set(ticks)];
  }
  return ticks;
}

/**
 * Estimate how many axis ticks fit without overlapping, given the available
 * width (in SVG viewbox units) and a formatter. Samples several values
 * across the range to find the widest label, then computes how many fit.
 */
export function estimateTickCount(
  availableWidth: number,
  min: number,
  max: number,
  formatter: (v: number) => string = formatNumber,
): number {
  const charWidth = 6;
  const minGap = 15;
  // Sample min, max, and a few intermediate values to find worst-case width.
  const samples = [min, max, (min + max) / 2, min + (max - min) * 0.25];
  const maxLen = Math.max(...samples.map((v) => formatter(v).length));
  const labelWidth = maxLen * charWidth;
  const tickSlotWidth = labelWidth + minGap;
  return Math.min(7, Math.max(2, Math.floor(availableWidth / tickSlotWidth)));
}

/**
 * Format a number for display on chart axes.
 */
export function formatNumber(value: number): string {
  if (Number.isInteger(value)) {
    return value.toLocaleString();
  }
  if (Math.abs(value) >= 1000) {
    return value.toLocaleString(undefined, {maximumFractionDigits: 0});
  }
  if (Math.abs(value) >= 1) {
    return value.toLocaleString(undefined, {maximumFractionDigits: 2});
  }
  // For very small numbers, use more precision
  return value.toPrecision(3);
}

/**
 * Generate tick values for a logarithmic scale (powers of 10).
 */
export function generateLogTicks(max: number): number[] {
  if (max <= 1) return [1];
  const ticks: number[] = [1];
  let power = 1;
  while (Math.pow(10, power) <= max) {
    ticks.push(Math.pow(10, power));
    power++;
  }
  return ticks;
}

/**
 * Aggregation types supported by chart loaders.
 */
export type AggregationType =
  | 'SUM'
  | 'AVG'
  | 'MIN'
  | 'MAX'
  | 'COUNT'
  | 'COUNT_DISTINCT';

/**
 * Whether an aggregation always produces integer results.
 */
export function isIntegerAggregation(agg: AggregationType): boolean {
  return agg === 'COUNT' || agg === 'COUNT_DISTINCT';
}

/**
 * Default chart colors for multi-series charts.
 */
export const CHART_COLORS = [
  'var(--pf-chart-color-1, #4285f4)',
  'var(--pf-chart-color-2, #ea4335)',
  'var(--pf-chart-color-3, #fbbc04)',
  'var(--pf-chart-color-4, #34a853)',
  'var(--pf-chart-color-5, #ff6d01)',
  'var(--pf-chart-color-6, #46bdc6)',
  'var(--pf-chart-color-7, #9334e6)',
  'var(--pf-chart-color-8, #185abc)',
];

/**
 * Truncate a label to fit within a maximum character count.
 * Adds an ellipsis if truncation is needed.
 *
 * @param label The text to truncate.
 * @param maxChars Maximum characters to allow.
 */
export function truncateLabel(label: string, maxChars: number): string {
  if (label.length <= maxChars) return label;
  return label.substring(0, maxChars - 1) + '\u2026';
}

// ---------------------------------------------------------------------------
// SQL helpers shared across chart loaders
// ---------------------------------------------------------------------------

/**
 * Build the SQL aggregation expression for a column.
 */
export function sqlAggExpression(column: string, agg: AggregationType): string {
  switch (agg) {
    case 'SUM':
      return `SUM(${column})`;
    case 'AVG':
      return `AVG(${column})`;
    case 'MIN':
      return `MIN(${column})`;
    case 'MAX':
      return `MAX(${column})`;
    case 'COUNT':
      return `COUNT(${column})`;
    case 'COUNT_DISTINCT':
      return `COUNT(DISTINCT ${column})`;
  }
}

/**
 * Build a SQL `column IN (...)` clause from a list of values.
 * String values are properly escaped. Returns empty string if values is empty.
 */
export function sqlInClause(
  column: string,
  values: ReadonlyArray<string | number>,
): string {
  if (values.length === 0) return '';
  const literals = values.map((v) =>
    typeof v === 'string' ? `'${v.replace(/'/g, "''")}'` : `${v}`,
  );
  return `${column} IN (${literals.join(', ')})`;
}

/**
 * Build a SQL range filter clause: `column >= min AND column <= max`.
 */
export function sqlRangeClause(
  column: string,
  range: {readonly min: number; readonly max: number},
): string {
  return `${column} >= ${range.min} AND ${column} <= ${range.max}`;
}
