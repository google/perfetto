// Copyright (C) 2024 The Android Open Source Project
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
import {Icons} from '../../../../../base/semantic_icons';
import {MenuItem} from '../../../../../widgets/menu';
import {
  TableColumn,
  RenderedCell,
  RenderCellContext,
  ListColumnsContext,
} from '../table_column';
import {SqlTableState} from '../state';
import {
  PerfettoSqlType,
  PerfettoSqlTypes,
  typesEqual,
  underlyingSqlType,
} from '../../../../../trace_processor/perfetto_sql_type';
import {SqlColumn, sqlColumnId, SqlExpression} from '../sql_column';
import {SqlValue} from '../../../../../trace_processor/query_result';
import {createTableColumn} from '../columns';

type CastParams = {
  type: PerfettoSqlType;
};

const CASTS = {
  int: {
    type: PerfettoSqlTypes.INT,
  },
  double: {
    type: PerfettoSqlTypes.DOUBLE,
  },
  string: {
    type: PerfettoSqlTypes.STRING,
  },
  boolean: {
    type: PerfettoSqlTypes.BOOLEAN,
  },
  timestamp: {
    type: PerfettoSqlTypes.TIMESTAMP,
  },
  duration: {
    type: PerfettoSqlTypes.DURATION,
  },
  slice_id: {
    type: {
      kind: 'joinid',
      source: {
        table: 'slice',
        column: 'id',
      },
    },
  },
  utid: {
    type: {
      kind: 'joinid',
      source: {
        table: 'thread',
        column: 'id',
      },
    },
  },
  upid: {
    type: {
      kind: 'joinid',
      source: {
        table: 'process',
        column: 'id',
      },
    },
  },
} satisfies Record<string, CastParams>;

// CastColumn wraps another column and provides casting functionality
export class CastColumn implements TableColumn {
  public readonly column: SqlColumn;
  constructor(
    public readonly wrappedColumn: TableColumn,
    public readonly sourceColumn: TableColumn,
    public readonly type: PerfettoSqlType | undefined,
  ) {
    this.column = wrappedColumn.column;
  }

  getTitle(): string | undefined {
    return this.wrappedColumn.getTitle?.();
  }

  renderCell(value: SqlValue, context?: RenderCellContext): RenderedCell {
    // Delegate rendering to the appropriate column type based on the cast type
    // This allows proper formatting for timestamps, durations, etc.
    return this.wrappedColumn.renderCell(value, context);
  }

  listDerivedColumns(context: ListColumnsContext) {
    return this.wrappedColumn.listDerivedColumns?.(context);
  }

  getColumnSpecificMenuItems(args: {
    replaceColumn: (column: TableColumn) => void;
  }): m.Children {
    return m(MenuItem, {
      label: 'Remove cast',
      icon: Icons.Undo,
      onclick: () => args.replaceColumn(this.sourceColumn),
    });
  }
}

export function renderCastColumnMenu(
  column: TableColumn,
  columnIndex: number,
  state: SqlTableState,
): m.Children {
  return Object.entries(CASTS)
    .filter(([_, params]) => {
      if (column.type === undefined) {
        return true;
      }
      return !typesEqual(params.type, column.type);
    })
    .map(([label, params]) =>
      m(MenuItem, {
        label,
        onclick: () => {
          // If this is already a CastColumn, get the original source column.
          const columnToCast: TableColumn =
            column instanceof CastColumn ? column.sourceColumn : column;

          const castExpression = (() => {
            if (
              columnToCast.type !== undefined &&
              underlyingSqlType(columnToCast.type) ===
                underlyingSqlType(params.type)
            ) {
              // If the underlying types are the same, there is no need for a SQL cast, we only need to reinterpret the data.
              return (cols: string[]) => cols[0];
            }
            return (cols: string[]) =>
              `CAST(${cols[0]} AS ${underlyingSqlType(params.type)})`;
          })();

          // Create a CastColumn wrapping the source column
          const castColumn = new CastColumn(
            createTableColumn({
              column: new SqlExpression(
                castExpression,
                [columnToCast.column],
                `cast<${label}>(${sqlColumnId(columnToCast.column)})`,
              ),
              trace: state.trace,
              type: params.type,
            }),
            columnToCast,
            params.type,
          );

          return state.replaceColumnAtIndex(columnIndex, castColumn);
        },
      }),
    );
}
