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

import {SqlValue} from '../../../trace_processor/query_result';
import {DataGridDataSource, ValueFormatter} from './common';

/**
 * Default value formatter that converts SqlValue to string.
 * Handles special cases like DURATION_NS and PERCENT format hints.
 */
export function defaultValueFormatter(value: SqlValue): string {
  if (value === null) {
    return 'null';
  } else if (value instanceof Uint8Array) {
    return `Blob: ${value.byteLength.toLocaleString()} bytes`;
  } else {
    return `${value}`;
  }
}

export interface ExportOptions {
  readonly columns: ReadonlyArray<string>;
  readonly columnNames?: Record<string, string>;
  readonly format: 'tsv' | 'json' | 'markdown';
  readonly valueFormatter?: ValueFormatter;
  readonly formatHints?: Record<string, string>;
}

/**
 * Export data from a DataGridDataSource in the specified format.
 * Respects the current filters and sorting of the data source.
 */
export async function exportDataSource(
  dataSource: DataGridDataSource,
  options: ExportOptions,
): Promise<string> {
  const {
    columns,
    columnNames = {},
    format,
    valueFormatter = defaultValueFormatter,
    formatHints = {},
  } = options;

  // Get all rows from the data source
  const rows = await dataSource.exportData();

  // Format rows as strings
  const formattedRows = rows.map((row) => {
    const formattedRow: Record<string, string> = {};
    for (const col of columns) {
      const value = row[col];
      const formatHint = formatHints[col];
      formattedRow[col] = valueFormatter(value, col, formatHint);
    }
    return formattedRow;
  });

  // Convert to the requested format
  switch (format) {
    case 'tsv':
      return formatAsTSV(columns, columnNames, formattedRows);
    case 'json':
      return formatAsJSON(formattedRows);
    case 'markdown':
      return formatAsMarkdown(columns, columnNames, formattedRows);
    default:
      throw new Error(`Unknown export format: ${format}`);
  }
}

/**
 * Format data as TSV (tab-separated values)
 */
export function formatAsTSV(
  columns: ReadonlyArray<string>,
  columnNames: Record<string, string>,
  rows: Array<Record<string, string>>,
): string {
  const lines: string[] = [];

  // Header row
  const headerCells = columns.map((col) => columnNames[col] ?? col);
  lines.push(headerCells.join('\t'));

  // Data rows
  for (const row of rows) {
    const cells = columns.map((col) => row[col] ?? '');
    lines.push(cells.join('\t'));
  }

  return lines.join('\n');
}

/**
 * Format data as JSON
 */
export function formatAsJSON(rows: Array<Record<string, string>>): string {
  return JSON.stringify(rows, null, 2);
}

/**
 * Format data as Markdown table
 */
export function formatAsMarkdown(
  columns: ReadonlyArray<string>,
  columnNames: Record<string, string>,
  rows: Array<Record<string, string>>,
): string {
  if (columns.length === 0) return '';

  const lines: string[] = [];

  // Helper to escape markdown pipes
  const escapePipe = (value: string): string => {
    return value.replace(/\|/g, '\\|');
  };

  // Header row
  const headerCells = columns.map((col) => escapePipe(columnNames[col] ?? col));
  lines.push(`| ${headerCells.join(' | ')} |`);

  // Separator row
  const separators = columns.map(() => '---');
  lines.push(`| ${separators.join(' | ')} |`);

  // Data rows
  for (const row of rows) {
    const cells = columns.map((col) => escapePipe(row[col] ?? ''));
    lines.push(`| ${cells.join(' | ')} |`);
  }

  return lines.join('\n');
}
