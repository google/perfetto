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
import {classNames} from '../../base/classnames';
import {Icons} from '../../base/semantic_icons';
import {AddDebugTrackMenu} from '../../components/tracks/add_debug_track_menu';
import {DataSource} from '../../components/widgets/datagrid/data_source';
import {DataGrid, renderCell} from '../../components/widgets/datagrid/datagrid';
import {
  ColumnSchema,
  SchemaRegistry,
} from '../../components/widgets/datagrid/datagrid_schema';
import {Trace} from '../../public/trace';
import {Row} from '../../trace_processor/query_result';
import {Anchor} from '../../widgets/anchor';
import {Button} from '../../widgets/button';
import {Callout} from '../../widgets/callout';
import {Intent} from '../../widgets/common';
import {Icon} from '../../widgets/icon';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {Popup, PopupPosition} from '../../widgets/popup';
import {Stack} from '../../widgets/stack';

// Reusable component for displaying SQL query results.
//
// Handles two states:
//   - Error: shows an error icon with the preformatted error message.
//   - Success: shows results in a DataGrid with a toolbar.
//
// On success, the toolbar shows:
//   - Row count and query time
//   - ID column linking menu (auto-detects slice-like results)
//   - Export button
//   - Add debug track button
// A warning callout appears when multiple statements returned results.

// Tables whose IDs can be linked from the query results.
// 'auto' uses heuristics to detect slice-like rows.
const ID_TABLE_OPTIONS: ReadonlyArray<{label: string; sqlTable: string}> = [
  {label: 'Auto-Detect', sqlTable: 'auto'},
  {label: 'slice.id', sqlTable: 'slice'},
  {label: 'sched_slice.id', sqlTable: 'sched_slice'},
  {label: 'thread_state.id', sqlTable: 'thread_state'},
];

interface ResultsError {
  readonly kind: 'error';
  readonly errorMessage: string;
}

interface ResultsSuccess {
  readonly kind: 'success';
  readonly columns: string[];
  readonly rows: Row[];
  readonly dataSource: DataSource;
  readonly rowCount: number;
  readonly queryTimeMs: number;
  readonly query: string;
  readonly lastStatementSql: string;
  readonly statementCount: number;
  readonly statementWithOutputCount: number;
}

export type ResultsData = ResultsError | ResultsSuccess;

export interface ResultsTableAttrs {
  readonly data: ResultsData;
  readonly fillHeight?: boolean;
  readonly trace: Trace;

  // Called when a user clicks an ID link. The sqlTable and id identify the row.
  readonly onIdClick?: (
    sqlTable: string,
    id: number,
    doubleClick: boolean,
  ) => void;
}

export class ResultsTable implements m.Component<ResultsTableAttrs> {
  // The selected table for linking ID column values.
  private selectedIdTable = ID_TABLE_OPTIONS[0].sqlTable;

  view({attrs}: m.Vnode<ResultsTableAttrs>) {
    const {data, fillHeight} = attrs;

    return m(
      '.pf-results-table',
      {
        className: classNames(fillHeight && 'pf-results-table--fill-height'),
      },
      this.renderBody(attrs, data),
    );
  }

  private renderBody(attrs: ResultsTableAttrs, data: ResultsData): m.Children {
    switch (data.kind) {
      case 'error':
        return m(
          '.pf-results-table__error',
          m(Icon, {
            className: 'pf-results-table__error-icon',
            icon: 'error',
            intent: Intent.Danger,
          }),
          m('pre.pf-results-table__error-message', data.errorMessage),
        );
      case 'success':
        return this.renderResults(attrs, data);
    }
  }

  private renderResults(
    attrs: ResultsTableAttrs,
    data: ResultsSuccess,
  ): m.Children {
    const schema: SchemaRegistry = {};
    const rootSchema: ColumnSchema = {};

    const hasIdColumn = data.columns.includes('id');
    const autoDetected = this.detectAutoTable(data.columns);
    const resolvedTable = this.resolveIdTable(data.columns);

    for (const col of data.columns) {
      const cellRenderer =
        col === 'id' && attrs.onIdClick
          ? (value: Row[string]) =>
              this.renderIdCell(value, resolvedTable, attrs.onIdClick!)
          : undefined;
      rootSchema[col] = {
        title: col,
        cellRenderer,
      };
    }
    schema['root'] = rootSchema;

    const selectedLabel =
      this.selectedIdTable === 'auto'
        ? resolvedTable !== undefined
          ? `Auto-Detect (${resolvedTable}.id)`
          : 'Auto-Detect'
        : `${this.selectedIdTable}.id`;

    const linkingButton =
      hasIdColumn &&
      m(
        PopupMenu,
        {
          trigger: m(Button, {
            label: `Interpret id as: ${selectedLabel}`,
            icon: 'link',
          }),
          position: PopupPosition.Bottom,
        },
        ID_TABLE_OPTIONS.map((opt) =>
          m(MenuItem, {
            label:
              opt.sqlTable === 'auto'
                ? `Auto-Detect (${autoDetected})`
                : opt.label,
            active: this.selectedIdTable === opt.sqlTable,
            onclick: () => {
              this.selectedIdTable = opt.sqlTable;
            },
          }),
        ),
      );

    const toolbarLeft = m(
      Stack,
      {orientation: 'horizontal', spacing: 'small'},
      `Returned ${data.rowCount.toLocaleString()} rows in ${data.queryTimeMs.toLocaleString()} ms`,
    );

    const debugTrackButton = m(
      Popup,
      {
        trigger: m(Button, {label: 'Add debug track', icon: 'add_chart'}),
        position: PopupPosition.Top,
      },
      m(AddDebugTrackMenu, {
        trace: attrs.trace,
        query: data.lastStatementSql,
        availableColumns: data.columns,
        onAdd: () => attrs.trace.navigate('#!/viewer'),
      }),
    );

    const multiStatementWarning =
      data.statementWithOutputCount > 1 &&
      m(
        Callout,
        {icon: 'warning'},
        `${data.statementWithOutputCount} out of ${data.statementCount} ` +
          'statements returned a result. ' +
          'Only the results for the last statement are displayed.',
      );

    return [
      multiStatementWarning,
      m(DataGrid, {
        schema: schema,
        rootSchema: 'root',
        data: data.dataSource,
        fillHeight: true,
        emptyStateMessage: 'Query returned no rows',
        toolbarItemsLeft: toolbarLeft,
        toolbarItemsRight: [linkingButton, debugTrackButton],
        showExportButton: true,
      }),
    ];
  }

  private renderIdCell(
    value: Row[string],
    resolvedTable: string | undefined,
    onIdClick: (sqlTable: string, id: number, doubleClick: boolean) => void,
  ): m.Children {
    const cell = renderCell(value, 'id');
    const id =
      typeof value === 'bigint'
        ? Number(value)
        : typeof value === 'number'
          ? value
          : undefined;
    if (resolvedTable !== undefined && id !== undefined) {
      return m(
        Anchor,
        {
          title: `Go to ${resolvedTable} on the timeline`,
          icon: Icons.UpdateSelection,
          onclick: () => onIdClick(resolvedTable, id, false),
          ondblclick: () => onIdClick(resolvedTable, id, true),
        },
        cell,
      );
    }
    return cell;
  }

  // Resolve the SQL table name for ID linking based on the current
  // selectedIdTable setting. In auto mode, checks columns for slice-like shape.
  private resolveIdTable(columns: string[]): string | undefined {
    if (this.selectedIdTable === 'auto') {
      return this.isSliceish(columns) ? 'slice' : undefined;
    }
    return this.selectedIdTable;
  }

  // Check columns to determine what 'auto' would detect.
  private detectAutoTable(columns: string[]): string {
    return this.isSliceish(columns) ? 'slice.id' : 'none';
  }

  // A result set looks slice-ish if it has id, ts, dur, and track_id columns.
  private isSliceish(columns: string[]): boolean {
    return (
      columns.includes('id') &&
      columns.includes('ts') &&
      columns.includes('dur') &&
      columns.includes('track_id')
    );
  }
}
