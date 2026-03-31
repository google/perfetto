// Copyright (C) 2025 The Android Open Source Project
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
 * Utility functions for the query builder.
 */

import {
  PerfettoSqlType,
  isQuantitativeType,
  typesEqual,
} from '../../../trace_processor/perfetto_sql_type';
import {ColumnInfo} from './column_info';

/**
 * Checks if a column type is numeric/quantitative.
 * Numeric types include: int, double, duration, timestamp, boolean, id,
 * joinid, and arg_set_id.
 *
 * @param type The PerfettoSqlType to check
 * @returns true if the type is numeric
 */
export function isNumericType(type?: PerfettoSqlType): boolean {
  if (type === undefined) return false;
  return isQuantitativeType(type);
}

/**
 * Checks if a column type is a string type.
 *
 * @param type The PerfettoSqlType to check
 * @returns true if the type is a string
 */
export function isStringType(type?: PerfettoSqlType): boolean {
  if (type === undefined) return false;
  return type.kind === 'string';
}

/**
 * Checks if a column is compatible with a specific aggregation operation.
 *
 * @param col The column to check (must have an optional `type` field)
 * @param col.type The SQL type of the column
 * @param op The aggregation operation (e.g., 'SUM', 'COUNT', 'MEAN', etc.)
 * @returns true if the column is compatible with the operation
 */
export function isColumnValidForAggregation(
  col: {type?: PerfettoSqlType},
  op?: string,
): boolean {
  if (!op) return true;

  const isNumeric = isNumericType(col.type);
  const isString = isStringType(col.type);

  switch (op) {
    case 'MEAN':
    case 'MEDIAN':
    case 'PERCENTILE':
    case 'DURATION_WEIGHTED_MEAN':
      // These operations require numeric types
      return isNumeric;
    case 'GLOB':
      // GLOB requires string types
      return isString;
    case 'COUNT':
    case 'COUNT(*)':
    case 'SUM':
    case 'MIN':
    case 'MAX':
    default:
      // These operations work on all types
      return true;
  }
}

/**
 * Gets a human-readable description of the type requirements for an aggregation operation.
 *
 * @param op The aggregation operation
 * @returns A description of the type requirements
 */
export function getAggregationTypeRequirements(op: string): string {
  switch (op) {
    case 'MEAN':
    case 'MEDIAN':
    case 'PERCENTILE':
    case 'DURATION_WEIGHTED_MEAN':
      return 'Requires numeric column';
    case 'GLOB':
      return 'Requires string column';
    case 'COUNT(*)':
      return 'No column required';
    case 'COUNT':
    case 'SUM':
    case 'MIN':
    case 'MAX':
      return 'Works with any column type';
    default:
      return 'Unknown operation';
  }
}

export interface GetCommonColumnsOptions {
  // Column names to exclude from the result
  excludedColumns?: Set<string>;
  // Column types to exclude from the result
  excludedTypes?: Set<PerfettoSqlType['kind']>;
}

/**
 * Finds columns that exist in all provided column arrays.
 * Returns the intersection of column names, optionally filtered by exclusions.
 *
 * @param columnArrays Array of ColumnInfo arrays to find common columns across
 * @param options Optional exclusion filters for column names and types
 * @returns Sorted array of common column names
 */
export function getCommonColumns(
  columnArrays: ColumnInfo[][],
  options?: GetCommonColumnsOptions,
): string[] {
  if (columnArrays.length === 0) {
    return [];
  }

  const excludedColumns = options?.excludedColumns ?? new Set();
  const excludedTypes = options?.excludedTypes ?? new Set();

  const isTypeExcluded = (type?: PerfettoSqlType): boolean => {
    if (type === undefined) return false;
    return excludedTypes.has(type.kind);
  };

  // Start with columns from the first array
  const firstArray = columnArrays[0];
  const commonColumns = new Set(
    firstArray
      .filter((c) => !excludedColumns.has(c.name) && !isTypeExcluded(c.type))
      .map((c) => c.name),
  );

  // Intersect with columns from remaining arrays
  for (let i = 1; i < columnArrays.length; i++) {
    const colsMap = new Map(columnArrays[i].map((c) => [c.name, c.type]));
    for (const col of commonColumns) {
      const colType = colsMap.get(col);
      if (colType === undefined || isTypeExcluded(colType)) {
        commonColumns.delete(col);
      }
    }
  }

  return Array.from(commonColumns).sort();
}

/**
 * Checks if two PerfettoSqlType values are equal.
 * Convenience re-export for use in the query builder.
 */
export {typesEqual};
