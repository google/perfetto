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

import {AggregateFunction} from '../datagrid/model';

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
 * Whether an aggregation always produces integer results.
 */
export function isIntegerAggregation(agg: AggregateFunction): boolean {
  return agg === 'COUNT_DISTINCT';
}

// ---------------------------------------------------------------------------
// SQL helpers shared across chart loaders
// ---------------------------------------------------------------------------

// Valid SQL column name: identifier chars only (letters, digits, underscore).
const VALID_COLUMN_RE = /^[a-zA-Z_][a-zA-Z0-9_]*$/;

/**
 * Validate that a string is a safe SQL column identifier.
 * Throws if the name contains non-identifier characters.
 */
export function validateColumnName(name: string): void {
  if (!VALID_COLUMN_RE.test(name)) {
    throw new Error(`Invalid SQL column name: '${name}'`);
  }
}

// ---------------------------------------------------------------------------
// ECharts brush helpers
// ---------------------------------------------------------------------------

// Re-export EChartBrushEndParams for use by other chart modules.
export {type EChartBrushEndParams} from './echart_view';

/**
 * Extract the numeric brush range from an ECharts brushEnd event.
 * Returns [min, max] if a valid range was selected, undefined otherwise.
 *
 * This utility centralizes the brush range extraction logic used across
 * different chart types (line, scatter, bar, histogram).
 */
export function extractBrushRange(
  params: unknown,
): [number, number] | undefined {
  const p = params as {
    areas?: ReadonlyArray<{coordRange?: [number, number]}>;
  };
  const areas = p.areas;
  if (areas !== undefined && areas.length > 0 && areas[0].coordRange) {
    const [a, b] = areas[0].coordRange;
    return [Math.min(a, b), Math.max(a, b)];
  }
  return undefined;
}
