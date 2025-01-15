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

import {PageWithTraceAttrs} from '../../../public/page';
import {Button} from '../../../widgets/button';
import {SqlModules, SqlTable} from '../../dev.perfetto.SqlModules/sql_modules';
import {Section} from '../../../widgets/section';
import {SegmentedButtons} from '../../../widgets/segmented_buttons';
import {
  ColumnController,
  ColumnControllerDiff,
  ColumnControllerRows,
} from './column_controller';
import {QueryNode, StdlibTableState, NodeType} from '../query_state';
import {JoinState, QueryBuilderJoin} from './operations/join';

export interface QueryBuilderTable {
  name: string;
  asSqlTable: SqlTable;
  columnOptions: ColumnControllerRows[];
  sql: string;
}

export interface QueryBuilderAttrs extends PageWithTraceAttrs {
  readonly sqlModules: SqlModules;
  readonly queryNode?: QueryNode;
  readonly onQueryNodeCreated: (node: QueryNode) => void;
}

export class QueryBuilder implements m.ClassComponent<QueryBuilderAttrs> {
  view({attrs}: m.CVnode<QueryBuilderAttrs>) {
    const trace = attrs.trace;

    function renderChooseTable(): m.Child {
      return m(Button, {
        label: 'Choose a table',
        onclick: async () => {
          const tableName = await trace.omnibox.prompt(
            'Choose a table...',
            attrs.sqlModules.listTablesNames(),
          );
          if (tableName === undefined) {
            return;
          }
          const sqlTable = attrs.sqlModules.getTable(tableName);
          if (sqlTable === undefined) {
            return;
          }
          attrs.onQueryNodeCreated(new StdlibTableState(sqlTable));
        },
      });
    }

    function renderPickColumns(): m.Child {
      console.log(`Started render()`);
      if (
        !attrs.queryNode?.finished ||
        attrs.queryNode.type !== NodeType.kStdlibTable
      ) {
        return;
      }

      return (
        attrs.queryNode.columns &&
        m(ColumnController, {
          hasValidColumns: true,
          options: attrs.queryNode.columns,
          onChange: (diffs: ColumnControllerDiff[]) => {
            diffs.forEach(({id, checked, alias}) => {
              if (attrs.queryNode?.columns === undefined) {
                return;
              }
              for (const option of attrs.queryNode?.columns) {
                if (option.id === id) {
                  option.checked = checked;
                  option.alias = alias;
                }
              }
            });
          },
        })
      );
    }

    function renderOperationButtons(): m.Child {
      return m(SegmentedButtons, {
        ...attrs,
        options: [{label: 'JOIN on ID'}, {label: 'INTERSECT'}],
        selectedOption: 0,
        onOptionSelected: () => {
          if (attrs.queryNode === undefined) {
            return;
          }
          attrs.queryNode.nextNode = new JoinState(attrs.queryNode);
        },
      });
    }

    return m(
      '.explore-page__columnar',
      renderChooseTable(),
      attrs.queryNode?.finished && [
        m('.explore-page__rowish', [
          m(Section, {title: attrs.queryNode.getTitle()}, renderPickColumns()),
          m('.explore-page__columnar', [
            renderOperationButtons(),
            attrs.queryNode.nextNode instanceof JoinState &&
              m(QueryBuilderJoin, {
                sqlModules: attrs.sqlModules,
                joinState: attrs.queryNode.nextNode,
              }),
          ]),
        ]),
      ],
    );
  }
}
