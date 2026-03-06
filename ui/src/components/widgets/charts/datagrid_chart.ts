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
import {Row} from '../../../trace_processor/query_result';
import {DataGrid} from '../datagrid/datagrid';
import {CellFormatter, SchemaRegistry} from '../datagrid/datagrid_schema';

/**
 * Data provided to a DatagridChart.
 */
export interface DatagridChartData {
  /** Column names to display. */
  readonly columns: readonly string[];
  /** The rows to display. */
  readonly rows: readonly Row[];
}

export interface DatagridChartAttrs {
  /**
   * Data to display, or undefined if loading.
   */
  readonly data: DatagridChartData | undefined;

  /**
   * Fill parent container height. Defaults to false.
   */
  readonly fillParent?: boolean;

  /**
   * Custom class name for the container.
   */
  readonly className?: string;

  /**
   * Custom cell formatters keyed by column name.
   * Used to format timestamp/duration columns etc.
   */
  readonly cellFormatters?: Readonly<Record<string, CellFormatter>>;

  /**
   * Callback when a row is clicked. Called with the row data.
   */
  readonly onRowClick?: (row: Row) => void;
}

/**
 * A chart widget that displays tabular data using the standard DataGrid.
 * Wraps the existing DataGrid with a minimal schema derived from column names.
 */
export class DatagridChart implements m.ClassComponent<DatagridChartAttrs> {
  view({attrs}: m.Vnode<DatagridChartAttrs>) {
    const {data, fillParent, className, cellFormatters} = attrs;

    if (data === undefined || data.rows.length === 0) {
      return undefined;
    }

    const schema = buildSchemaFromColumns(data.columns, cellFormatters);

    return m(DataGrid, {
      schema,
      rootSchema: 'root',
      // DataGrid expects a mutable Row[] but our source is readonly Row[].
      // The cast is safe because DataGrid only reads from the array.
      data: data.rows as Row[],
      fillHeight: fillParent,
      className,
      canAddColumns: false,
      canRemoveColumns: false,
    });
  }
}

/**
 * Build a minimal SchemaRegistry from column names.
 * Each column becomes a simple leaf ColumnDef.
 */
function buildSchemaFromColumns(
  columns: readonly string[],
  cellFormatters?: Readonly<Record<string, CellFormatter>>,
): SchemaRegistry {
  const schema: SchemaRegistry = {
    root: Object.fromEntries(
      columns.map((col) => {
        const formatter = cellFormatters?.[col];
        return [
          col,
          {title: col, ...(formatter && {cellFormatter: formatter})},
        ];
      }),
    ),
  };
  return schema;
}
