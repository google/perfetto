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

import m from 'mithril';
import {perfettoSqlTypeToString} from '../../../trace_processor/perfetto_sql_type';
import {ColumnDef} from '../graph_utils';

export interface ColumnsTabAttrs {
  readonly outputColumns: ColumnDef[] | undefined;
  readonly activeNodeId: string | undefined;
}

export function renderColumnsTab(attrs: ColumnsTabAttrs): m.Children {
  const {outputColumns, activeNodeId} = attrs;

  if (!outputColumns || outputColumns.length === 0) {
    return m(
      '',
      {
        style: {
          display: 'flex',
          flex: '1',
          alignItems: 'center',
          justifyContent: 'center',
          opacity: '0.5',
          fontSize: '13px',
        },
      },
      activeNodeId ? 'No columns available' : 'Select a node',
    );
  }

  return m(
    '',
    {
      style: {
        display: 'flex',
        flexDirection: 'column',
        flex: '1',
        overflow: 'auto',
        padding: '8px',
        gap: '4px',
      },
    },
    outputColumns.map((col, i) =>
      m(
        '.pf-qb-col-row',
        {key: i},
        [
          m('span.pf-qb-col-name', col.name),
          m('span.pf-qb-col-type', perfettoSqlTypeToString(col.type)),
        ],
      ),
    ),
  );
}
