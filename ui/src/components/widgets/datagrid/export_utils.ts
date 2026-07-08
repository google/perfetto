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

import type {Row, SqlValue} from '../../../trace_processor/query_result';
import {
  type CellFormatter,
  type SchemaRegistry,
  getColumnInfo,
} from './datagrid_schema';

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
 * Apply cell formatters to all rows, converting SqlValues to strings.
 * @param rows The input rows to format.
 * @param schema Optional schema registry for looking up column info.
 * @param rootSchema Optional name of the root schema for lookup.
 * @param columns The list of column aliases to include in the output.
 * @param aliasToField Optional mapping from column aliases to field paths,
 *   used when column IDs differ from field paths for schema lookup.
 */
export function formatRows(
  rows: readonly Row[],
  schema: SchemaRegistry | undefined,
  rootSchema: string | undefined,
  columns: ReadonlyArray<string>,
  aliasToField?: Record<string, string>,
): Array<Record<string, string>> {
  return rows.map((row) => {
    const formattedRow: Record<string, string> = {};
    for (const colAlias of columns) {
      const value = row[colAlias];
      // Use field path for schema lookup if provided, otherwise use alias
      const fieldPath = aliasToField?.[colAlias] ?? colAlias;
      const formatter =
        schema && rootSchema
          ? getColumnInfo(schema, rootSchema, fieldPath)?.cellFormatter ??
            defaultValueFormatter
          : defaultValueFormatter;
      formattedRow[colAlias] = formatter(value, row);
    }
    return formattedRow;
  });
}

/**
 * Build a mapping of column paths to display names.
 * @param schema Optional schema registry for looking up column info.
 * @param rootSchema Optional name of the root schema for lookup.
 * @param columns The list of column paths to get names for.
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
