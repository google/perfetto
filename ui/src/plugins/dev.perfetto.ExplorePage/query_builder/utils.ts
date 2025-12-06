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

import {ColumnInfo} from './column_info';

/**
 * Checks if a column type is numeric/quantitative.
 * Numeric types include: INT, DOUBLE, DURATION, TIMESTAMP, BOOLEAN, ID types, and ARG_SET_ID.
 *
 * @param typeStr The column type string (case-insensitive)
 * @returns true if the type is numeric
 */
export function isNumericType(typeStr: string): boolean {
  const normalized = typeStr.toUpperCase();
  return (
    normalized === 'INT' ||
    normalized === 'DOUBLE' ||
    normalized === 'DURATION' ||
    normalized === 'TIMESTAMP' ||
    normalized === 'BOOLEAN' ||
    normalized.startsWith('ID(') ||
    normalized.startsWith('JOINID(') ||
    normalized === 'ARG_SET_ID'
  );
}

/**
 * Checks if a column type is a string type.
 *
 * @param typeStr The column type string (case-insensitive)
 * @returns true if the type is a string
 */
export function isStringType(typeStr: string): boolean {
  return typeStr.toUpperCase() === 'STRING';
}

/**
 * Checks if a column is compatible with a specific aggregation operation.
 *
 * @param col The column to check
 * @param op The aggregation operation (e.g., 'SUM', 'COUNT', 'MEAN', etc.)
 * @returns true if the column is compatible with the operation
 */
export function isColumnValidForAggregation(
  col: ColumnInfo,
  op?: string,
): boolean {
  if (!op) return true;

  const typeStr = col.type;
  const isNumeric = isNumericType(typeStr);
  const isString = isStringType(typeStr);

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
