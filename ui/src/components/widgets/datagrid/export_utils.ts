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

import {Row, SqlValue} from '../../../trace_processor/query_result';
import {CellFormatter, SchemaRegistry, getColumnInfo} from './datagrid_schema';

/**
 * Default value formatter that converts SqlValue to string.
 * Handles special cases like DURATION_NS and PERCENT format hints.
 */
export function defaultValueFormatter(value: SqlValue): string {
  if (value === undefined) {
    return '';
  } else if (value === null) {
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
  readonly valueFormatter?: CellFormatter;
  readonly formatHints?: Record<string, string>;
}

/**
 * Format data as TSV (tab-separated values).
 * Replaces tabs and newlines in cell values with spaces to maintain format integrity.
 */
export function formatAsTSV(
  columns: ReadonlyArray<string>,
  columnNames: Record<string, string>,
  rows: Array<Record<string, string>>,
): string {
  const lines: string[] = [];

  // Helper to escape TSV special characters
  const escapeTSV = (value: string): string => {
    // Replace tabs and newlines with spaces to prevent breaking the format
    // Handle Windows line endings (\r\n) as a single unit first
    return value.replace(/\r\n/g, ' ').replace(/[\t\n\r]/g, ' ');
  };

  // Header row
  const headerCells = columns.map((col) => escapeTSV(columnNames[col] ?? col));
  lines.push(headerCells.join('\t'));

  // Data rows
  for (const row of rows) {
    const cells = columns.map((col) => escapeTSV(row[col] ?? ''));
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
 * Format data as Markdown table.
 * Escapes special characters to prevent breaking the table format.
 */
export function formatAsMarkdown(
  columns: ReadonlyArray<string>,
  columnNames: Record<string, string>,
  rows: Array<Record<string, string>>,
): string {
  if (columns.length === 0) return '';

  const lines: string[] = [];

  // Helper to escape markdown special characters
  const escapeMarkdown = (value: string): string => {
    return (
      value
        // Backslashes must be escaped first to avoid double-escaping
        .replace(/\\/g, '\\\\')
        // Escape pipes (table delimiters)
        .replace(/\|/g, '\\|')
        // Replace newlines with spaces (can't have newlines in table cells)
        // Handle Windows line endings (\r\n) as a single unit first
        .replace(/\r\n/g, ' ')
        .replace(/[\n\r]/g, ' ')
    );
  };

  // Header row
  const headerCells = columns.map((col) =>
    escapeMarkdown(columnNames[col] ?? col),
  );
  lines.push(`| ${headerCells.join(' | ')} |`);

  // Separator row
  const separators = columns.map(() => '---');
  lines.push(`| ${separators.join(' | ')} |`);

  // Data rows
  for (const row of rows) {
    const cells = columns.map((col) => escapeMarkdown(row[col] ?? ''));
    lines.push(`| ${cells.join(' | ')} |`);
  }

  return lines.join('\n');
}

/**
 * Apply cell formatters to all rows, converting SqlValues to strings.
 */
export function formatRows(
  rows: readonly Row[],
  schema: SchemaRegistry | undefined,
  rootSchema: string | undefined,
  columns: ReadonlyArray<string>,
): Array<Record<string, string>> {
  return rows.map((row) => {
    const formattedRow: Record<string, string> = {};
    for (const colPath of columns) {
      const value = row[colPath];
      const formatter =
        schema && rootSchema
          ? getColumnInfo(schema, rootSchema, colPath)?.cellFormatter ??
            defaultValueFormatter
          : defaultValueFormatter;
      formattedRow[colPath] = formatter(value, row);
    }
    return formattedRow;
  });
}

/**
 * Build a mapping of column paths to display names.
 */
export function buildColumnNames(
  schema: SchemaRegistry | undefined,
  rootSchema: string | undefined,
  columns: ReadonlyArray<string>,
): Record<string, string> {
  const columnNames: Record<string, string> = {};
  for (const colPath of columns) {
    columnNames[colPath] =
      schema && rootSchema
        ? getColumnInfo(schema, rootSchema, colPath)?.def.titleString ?? colPath
        : colPath;
  }
  return columnNames;
}
