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
import {MenuItem} from '../../../../../widgets/menu';
import {TableColumn} from '../table_column';
import {SqlTableState} from '../state';
import {
  PerfettoSqlType,
  PerfettoSqlTypes,
  typesEqual,
  underlyingSqlType,
} from '../../../../../trace_processor/perfetto_sql_type';
import {createTableColumn} from '../create_column';
import {sqlColumnId, SqlExpression} from '../sql_column';

type CastParams = {
  type: PerfettoSqlType;
};

const CASTS = {
  int: {
    type: PerfettoSqlTypes.INT,
  },
  float: {
    type: PerfettoSqlTypes.FLOAT,
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
          const columnToCast: TableColumn = (() => {
            if (column.origin?.kind === 'cast') {
              return column.origin.source;
            }
            return column;
          })();
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
          return state.replaceColumnAtIndex(
            columnIndex,
            createTableColumn({
              trace: state.trace,
              column: new SqlExpression(
                castExpression,
                [columnToCast.column],
                `cast<${label}>(${sqlColumnId(columnToCast.column)})`,
              ),
              type: params.type,
              origin: {
                kind: 'cast',
                source: columnToCast,
              },
            }),
          );
        },
      }),
    );
}
