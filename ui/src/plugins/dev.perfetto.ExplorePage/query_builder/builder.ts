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
import {ColumnControllerRows} from './column_controller';
import {QueryNode} from '../query_state';
import {JoinState, QueryBuilderJoin} from './operations/join';
import {Intent} from '../../../widgets/common';
import {showModal} from '../../../widgets/modal';
import {DataSourceViewer} from './data_source_viewer';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {getLastFinishedNode} from '../query_state';
import protos from '../../../protos';
import {AsyncLimiter} from '../../../base/async_limiter';
import {TextInput} from '../../../widgets/text_input';
import {
  StdlibTableState,
  SimpleSlicesAttrs,
  SimpleSlicesState,
} from './source_nodes';

export interface QueryBuilderTable {
  name: string;
  asSqlTable: SqlTable;
  columnOptions: ColumnControllerRows[];
  sql: string;
}

export interface QueryBuilderAttrs extends PageWithTraceAttrs {
  readonly sqlModules: SqlModules;
  readonly rootNode?: QueryNode;
  readonly onRootNodeCreated: (node: QueryNode) => void;
}

export class QueryBuilder implements m.ClassComponent<QueryBuilderAttrs> {
  private readonly analyzeSQAsyncLimiter = new AsyncLimiter();
  private analyzedSq?: protos.AnalyzeStructuredQueryResult;

  view({attrs}: m.CVnode<QueryBuilderAttrs>) {
    const trace = attrs.trace;
    const sq = new protos.PerfettoSqlStructuredQuery();

    if (this.analyzedSq === undefined) {
      this.analyzeSQAsyncLimiter.schedule(async () => {
        this.analyzedSq = await trace.engine.analyzeStructuredQuery([sq]);
      });
    }

    // Create starting node.
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
              attrs.sqlModules.listTablesNames(),
            );
            if (tableName === undefined) {
              return;
            }
            const sqlTable = attrs.sqlModules.getTable(tableName);
            if (sqlTable === undefined) {
              return;
            }
            attrs.onRootNodeCreated(new StdlibTableState(sqlTable));
          },
        }),
        m(MenuItem, {
          label: 'Slices',
          onclick: () => {
            const sliceAttrs: SimpleSlicesAttrs = {};
            simpleSlicesModal(sliceAttrs, () => {
              const newSlices = new SimpleSlicesState(sliceAttrs);
              if (newSlices.validate()) {
                attrs.onRootNodeCreated(new SimpleSlicesState(sliceAttrs));
              }
            });
          },
        }),
        m(MenuItem, {label: 'Interval intersect', disabled: true}),
        m(MenuItem, {label: 'SQL', disabled: true}),
      );
    }

    // Followup node
    function chooseOperationButton() {
      return m(
        PopupMenu,
        {
          trigger: m(Button, {
            label: '+',
            intent: Intent.Primary,
          }),
        },
        m(MenuItem, {
          label: 'JOIN',
          disabled: true,
          onclick: () => {
            if (attrs.rootNode === undefined) {
              return;
            }
            const curNode = getLastFinishedNode(attrs.rootNode);
            if (curNode === undefined) {
              return;
            }
            const newJoinState = new JoinState(curNode);
            joinModal(newJoinState, () => {
              newJoinState.validate();
              curNode.nextNode = newJoinState;
            });
          },
        }),
        m(MenuItem, {label: 'INTERSECT', disabled: true}),
      );
    }

    function joinModal(joinState: JoinState, f: () => void) {
      function Operations() {
        return {
          view: () => {
            return m(QueryBuilderJoin, {
              sqlModules: attrs.sqlModules,
              joinState: joinState,
            });
          },
        };
      }

      const content = () => m(Operations);

      showModal({
        title: `JOIN`,
        buttons: [
          {
            text: 'Add node',
            action: f,
          },
        ],
        content,
      });
    }

    function simpleSlicesModal(slicesAttrs: SimpleSlicesAttrs, f: () => void) {
      function Operations() {
        return {
          view: () => {
            return m(
              '',
              m(
                '',
                'Slice name glob ',
                m(TextInput, {
                  id: 'slice_name_glob',
                  type: 'string',
                  oninput: (e: KeyboardEvent) => {
                    if (!e.target) return;
                    slicesAttrs.slice_name = (
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
                  oninput: (e: KeyboardEvent) => {
                    if (!e.target) return;
                    slicesAttrs.thread_name = (
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
                  oninput: (e: KeyboardEvent) => {
                    if (!e.target) return;
                    slicesAttrs.process_name = (
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
                  oninput: (e: KeyboardEvent) => {
                    if (!e.target) return;
                    slicesAttrs.track_name = (
                      e.target as HTMLInputElement
                    ).value.trim();
                  },
                }),
              ),
            );
          },
        };
      }

      const content = () => m(Operations);

      showModal({
        title: `Slices source`,
        buttons: [
          {
            text: 'Add node',
            action: f,
          },
        ],
        content,
      });
    }

    function renderNodesPanel(): m.Children {
      function renderNodes() {
        let row = 1;
        if (attrs.rootNode === undefined) {
          return m(
            '',
            {
              style: {
                gridColumn: 3,
                gridRow: row,
              },
            },
            chooseSourceButton(),
          );
        }

        let curNode: QueryNode | undefined = attrs.rootNode;
        const nodes: m.Child[] = [];
        while (curNode && curNode.finished) {
          nodes.push(
            m(
              '',
              {
                style: {
                  gridColumn: 3,
                  gridRow: row,
                },
              },
              m(Button, {
                label: curNode.getTitle(),
                intent: Intent.Primary,
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
            {
              style: {
                gridColumn: 3,
                gridRow: row,
              },
            },
            chooseOperationButton(),
          ),
        );
        return nodes;
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
        renderNodes(),
      );
    }

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
      m(
        '',
        {
          style: {
            gridColumn: 1,
          },
        },
        renderNodesPanel(),
      ),
      m(
        '',
        {
          style: {
            gridColumn: 2,
          },
        },
        attrs.rootNode?.finished && [
          m(DataSourceViewer, {
            trace: attrs.trace,
            queryNode: attrs.rootNode,
          }),
        ],
      ),
    );
  }
}
