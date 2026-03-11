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

import {CellFormatter} from '../../../../components/widgets/datagrid/datagrid_schema';
import {Duration} from '../../../../base/time';
import {
  underlyingSqlType,
  PerfettoSqlType,
} from '../../../../trace_processor/perfetto_sql_type';
import {VisualisationNode} from '../nodes/visualisation_node';

/**
 * Whether a column's underlying SQL type is INTEGER.
 */
export function isIntegerColumn(
  node: VisualisationNode,
  columnName: string,
): boolean {
  const columnInfo = node.sourceCols.find((col) => col.name === columnName);
  const columnType = columnInfo?.column.type;
  return (
    columnType !== undefined && underlyingSqlType(columnType) === 'INTEGER'
  );
}

/**
 * Get the PerfettoSQL type kind for a column, if available.
 */
export function getColumnTypeKind(
  node: VisualisationNode,
  columnName: string,
): PerfettoSqlType['kind'] | undefined {
  const columnInfo = node.sourceCols.find((col) => col.name === columnName);
  return columnInfo?.column.type?.kind;
}

/**
 * Create a numeric formatter for timestamp/duration columns.
 * Returns undefined if the column doesn't need special formatting.
 */
export function getNumericFormatter(
  node: VisualisationNode,
  columnName: string,
): ((value: number) => string) | undefined {
  const kind = getColumnTypeKind(node, columnName);
  if (kind === 'timestamp' || kind === 'duration') {
    return (value: number) => Duration.humanise(BigInt(Math.round(value)));
  }
  return undefined;
}

/**
 * Build a map of CellFormatters for columns that need special formatting
 * (e.g., timestamp/duration columns).
 */
export function buildCellFormatters(
  node: VisualisationNode,
  columns: readonly string[],
): Record<string, CellFormatter> | undefined {
  const formatters: Record<string, CellFormatter> = {};
  let hasAny = false;
  for (const col of columns) {
    const kind = getColumnTypeKind(node, col);
    if (kind === 'timestamp' || kind === 'duration') {
      formatters[col] = (value) => {
        if (typeof value === 'number') {
          return Duration.humanise(BigInt(Math.round(value)));
        }
        if (typeof value === 'bigint') {
          return Duration.humanise(value);
        }
        return String(value ?? '');
      };
      hasAny = true;
    }
  }
  return hasAny ? formatters : undefined;
}
