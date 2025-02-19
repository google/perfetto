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
import {ColumnControllerRow} from './column_controller';
import {QueryNode} from '../query_node';
import {JoinState, QueryBuilderJoin} from './operations/join';
import {Intent} from '../../../widgets/common';
import {showModal} from '../../../widgets/modal';
import {DataSourceViewer} from './data_source_viewer';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {getLastFinishedNode} from '../query_node';
import {TextInput} from '../../../widgets/text_input';
import {
  StdlibTableState,
  SimpleSlicesAttrs,
  SimpleSlicesState,
  SqlSourceAttrs,
  SqlSourceState,
} from './source_nodes';
import {
  GroupByAttrs,
  GroupByNode,
  GroupByOperation,
} from './operations/groupy_by';
import {FilterAttrs, FilterNode, FilterOperation} from './operations/filter';

export interface QueryBuilderTable {
  name: string;
  asSqlTable: SqlTable;
  columnOptions: ColumnControllerRow[];
  sql: string;
}

export interface QueryBuilderAttrs extends PageWithTraceAttrs {
  readonly sqlModules: SqlModules;
  readonly rootNode?: QueryNode;
  readonly onRootNodeCreated: (node: QueryNode) => void;
}

export class QueryBuilder implements m.ClassComponent<QueryBuilderAttrs> {
  view({attrs}: m.CVnode<QueryBuilderAttrs>) {
    const {trace, sqlModules, rootNode, onRootNodeCreated} = attrs;

    function createModal(
      title: string,
      content: () => m.Children,
      onAdd: () => void,
    ) {
      showModal({
        title,
        buttons: [{text: 'Add node', action: onAdd}],
        content,
      });
    }

    function chooseSourceButton(): m.Child {
      return m(
        PopupMenu,
        {
          trigger: m(Button, {
            label: 'Choose a source',
            intent: Intent.Primary,
          }),
        },
        m(MenuItem, {
          label: 'Table',
          onclick: async () => {
            const tableName = await trace.omnibox.prompt(
              'Choose a table...',
              sqlModules.listTablesNames(),
            );
            if (!tableName) return;

            const sqlTable = sqlModules.getTable(tableName);
            if (!sqlTable) return;

            onRootNodeCreated(new StdlibTableState(sqlTable));
          },
        }),
        m(MenuItem, {
          label: 'Slices',
          onclick: () =>
            createModal('Slices source', () => m(SimpleSlicesModalContent), () => {
              const newSlices = new SimpleSlicesState(simpleSlicesAttrs);
              if (newSlices.validate()) {
                onRootNodeCreated(newSlices);
              }
            }),
        }),
        m(MenuItem, {
          label: 'SQL',
          onclick: () =>
            createModal('SQL source', () => m(SqlSourceModalContent), () => {
              const newSqlSource = new SqlSourceState(sqlSourceAttrs);
              if (newSqlSource.validate()) {
                onRootNodeCreated(newSqlSource);
              }
            }),
        }),
        m(MenuItem, {label: 'Interval intersect', disabled: true}),
      );
    }

    function chooseOperationButton(): m.Child {
      return m(
        PopupMenu,
        {
          trigger: m(Button, {label: '+', intent: Intent.Primary}),
        },
        m(MenuItem, {
          label: 'GROUP BY',
          onclick: () => {
            if (!rootNode) return;
            const curNode = getLastFinishedNode(rootNode);
            if (!curNode) return;

            const newGroupByAttrs: GroupByAttrs = {prevNode: curNode};
            createModal(
              'GROUP BY',
              () => m(GroupByOperation, newGroupByAttrs),
              () => {
                curNode.nextNode = new GroupByNode(newGroupByAttrs);
              },
            );
          },
        }),
        m(MenuItem, {
          label: 'FILTER',
          onclick: () => {
            if (!rootNode) return;
            const curNode = getLastFinishedNode(rootNode);
            if (!curNode) return;
            const newFilterAttrs: FilterAttrs = {prevNode: curNode};
            createModal('FILTER', () => m(FilterOperation, newFilterAttrs), () => {
              curNode.nextNode = new FilterNode(newFilterAttrs);
            });
          },
        }),
        m(MenuItem, {
          label: 'JOIN',
          disabled: true,
          onclick: () => {
            if (!rootNode) return;
            const curNode = getLastFinishedNode(rootNode);
            if (!curNode) return;

            const newJoinState = new JoinState(curNode);
            createModal(
              'JOIN',
              () => m(QueryBuilderJoin, {sqlModules, joinState: newJoinState}),
              () => {
                newJoinState.validate();
                curNode.nextNode = newJoinState;
              },
            );
          },
        }),
      );
    }

    function renderNodesPanel(): m.Children {
      const nodes: m.Child[] = [];
      let row = 1;

      if (!rootNode) {
        nodes.push(
          m('', {style: {gridColumn: 3, gridRow: row}}, chooseSourceButton()),
        );
      } else {
        let curNode: QueryNode | undefined = rootNode;
        while (curNode) {
          nodes.push(
            m(
              '',
              {style: {gridColumn: 3, gridRow: row}},
              m(Button, {
                label: curNode.getTitle(),
                intent: Intent.Primary,
                // TODO(mayzner): Add logic for button.
                onclick: async () => {},
              }),
            ),
          );
          row++;
          curNode = curNode.nextNode;
        }

        nodes.push(
          m(
            '',
            {style: {gridColumn: 3, gridRow: row}},
            chooseOperationButton(),
          ),
        );
      }

      return m(
        '',
        {
          style: {
            display: 'grid',
            gridTemplateColumns: 'repeat(5, 1fr)',
            gridTemplateRows: 'repeat(3, 1fr)',
            gap: '10px',
          },
        },
        nodes,
      );
    }

    const simpleSlicesAttrs: SimpleSlicesAttrs = {};
    const SimpleSlicesModalContent = {
      view: () => {
        return m(
          '',
          m(
            '',
            'Slice name glob ',
            m(TextInput, {
              id: 'slice_name_glob',
              type: 'string',
              oninput: (e: Event) => {
                if (!e.target) return;
                simpleSlicesAttrs.slice_name = (
                  e.target as HTMLInputElement
                ).value.trim();
              },
            }),
          ),
          m(
            '',
            'Thread name glob ',
            m(TextInput, {
              id: 'thread_name_glob',
              type: 'string',
              oninput: (e: Event) => {
                if (!e.target) return;
                simpleSlicesAttrs.thread_name = (
                  e.target as HTMLInputElement
                ).value.trim();
              },
            }),
          ),
          m(
            '',
            'Process name glob ',
            m(TextInput, {
              id: 'process_name_glob',
              type: 'string',
              oninput: (e: Event) => {
                if (!e.target) return;
                simpleSlicesAttrs.process_name = (
                  e.target as HTMLInputElement
                ).value.trim();
              },
            }),
          ),
          m(
            '',
            'Track name glob ',
            m(TextInput, {
              id: 'track_name_glob',
              type: 'string',
              oninput: (e: Event) => {
                if (!e.target) return;
                simpleSlicesAttrs.track_name = (
                  e.target as HTMLInputElement
                ).value.trim();
              },
            }),
          ),
        );
      },
    };

    const sqlSourceAttrs: SqlSourceAttrs = {};
    const SqlSourceModalContent = {
      view: () => {
        return m(
          '',
          m(
            '',
            'Preamble',
            m(TextInput, {
              id: 'preamble',
              type: 'string',
              oninput: (e: Event) => {
                if (!e.target) return;
                sqlSourceAttrs.preamble = (
                  e.target as HTMLInputElement
                ).value.trim();
              },
            }),
          ),
          m(
            '',
            'Sql ',
            m(TextInput, {
              id: 'sql_source',
              type: 'string',
              oninput: (e: Event) => {
                if (!e.target) return;
                sqlSourceAttrs.sql = (e.target as HTMLInputElement).value
                  .trim()
                  .split(';')[0];
              },
            }),
          ),
          m(
            '',
            'Column names (comma separated strings) ',
            m(TextInput, {
              id: 'columns',
              type: 'string',
              oninput: (e: Event) => {
                if (!e.target) return;
                const colsStr = (e.target as HTMLInputElement).value.trim();
                sqlSourceAttrs.columns = colsStr
                  .split(',')
                  .map((col) => col.trim())
                  .filter(Boolean);
              },
            }),
          ),
        );
      },
    };

    return m(
      '',
      {
        style: {
          display: 'grid',
          gridTemplateColumns: '50% 50%',
          gridTemplateRows: '50% 50%',
          gap: '10px',
        },
      },
      m('', {style: {gridColumn: 1}}, renderNodesPanel()),
      m(
        '',
        {style: {gridColumn: 2}},
        rootNode && [m(DataSourceViewer, {trace, queryNode: rootNode})],
      ),
    );
  }
}
