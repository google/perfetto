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
import {
  perfettoSqlTypeIcon,
  perfettoSqlTypeToString,
} from '../../../trace_processor/perfetto_sql_type';
import type {ColumnDef} from '../graph_utils';
import {Icon} from '../../../widgets/icon';
import {EmptyState} from '../../../widgets/empty_state';
import './columns_tab.scss';

export interface ColumnsTabAttrs {
  readonly outputColumns: ColumnDef[] | undefined;
  readonly activeNodeId: string | undefined;
}

export class ColumnsTab implements m.ClassComponent<ColumnsTabAttrs> {
  view({attrs}: m.Vnode<ColumnsTabAttrs>) {
    const {outputColumns, activeNodeId} = attrs;

    if (!outputColumns || outputColumns.length === 0) {
      if (!activeNodeId) {
        return m(
          EmptyState,
          {fillHeight: true, title: 'No node selected'},
          'Select a node to see its output columns.',
        );
      }
      return m(
        EmptyState,
        {fillHeight: true, title: 'No columns available'},
        'This node does not declare its output columns.',
      );
    }

    return m(
      'table.pf-spag-cols',
      m('thead', m('tr', m('th', 'Name'), m('th', 'Type'))),
      m(
        'tbody',
        outputColumns.map((col, i) =>
          m('tr.pf-spag-col-row', {key: i}, [
            m('td.pf-spag-col-name', col.name),
            m(
              'td.pf-spag-col-type',
              m(Icon, {icon: perfettoSqlTypeIcon(col.type)}),
              perfettoSqlTypeToString(col.type),
            ),
          ]),
        ),
      ),
    );
  }
}
