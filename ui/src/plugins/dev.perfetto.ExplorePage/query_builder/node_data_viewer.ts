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

import {TextParagraph} from '../../../widgets/text_paragraph';
import {QueryTable} from '../../../components/query_table/query_table';
import {runQueryForQueryTable} from '../../../components/query_table/queries';
import {AsyncLimiter} from '../../../base/async_limiter';
import {QueryResponse} from '../../../components/query_table/queries';
import {Trace} from '../../../public/trace';
import {Query, queryToRun} from './query_node_explorer';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {Button} from '../../../widgets/button';
import {Icons} from '../../../base/semantic_icons';

export interface NodeDataViewerAttrs {
  readonly query?: Query | Error;
  readonly executeQuery: boolean;
  readonly trace: Trace;
  readonly onQueryExecuted: () => void;
  readonly onPositionChange: (pos: 'left' | 'right' | 'bottom') => void;
}

export class NodeDataViewer implements m.ClassComponent<NodeDataViewerAttrs> {
  private readonly tableAsyncLimiter = new AsyncLimiter();
  private queryResult?: QueryResponse;

  view({attrs}: m.CVnode<NodeDataViewerAttrs>) {
    const runQuery = () => {
      this.tableAsyncLimiter.schedule(async () => {
        if (
          attrs.query === undefined ||
          attrs.query instanceof Error ||
          !attrs.executeQuery
        ) {
          return;
        }

        this.queryResult = await runQueryForQueryTable(
          queryToRun(attrs.query),
          attrs.trace.engine,
        );
        attrs.onQueryExecuted();
      });
    };
    const queryErrors = () => {
      if (attrs.query === undefined) {
        return `No data to display`;
      }
      if (attrs.query instanceof Error) {
        return `Error: ${attrs.query.message}`;
      }
      if (this.queryResult === undefined) {
        runQuery();
        return `Typing...`;
      }
      if (this.queryResult.error !== undefined) {
        return `Error: ${this.queryResult.error}`;
      }
      return undefined;
    };

    runQuery();
    const errors = queryErrors();
    return [
      m(
        '.pf-node-data-viewer',
        m(
          '.pf-node-data-viewer__title-row',
          m('.title', 'Query data'),
          m('span.spacer'), // Push menu to the right
          m(
            PopupMenu,
            {
              trigger: m(Button, {
                icon: Icons.ContextMenuAlt,
              }),
            },
            [
              m(MenuItem, {
                label: 'Left',
                onclick: () => {
                  attrs.onPositionChange('left');
                },
              }),
              m(MenuItem, {
                label: 'Right',
                onclick: () => {
                  attrs.onPositionChange('right');
                },
              }),
              m(MenuItem, {
                label: 'Bottom',
                onclick: () => {
                  attrs.onPositionChange('bottom');
                },
              }),
            ],
          ),
        ),
        errors
          ? m(TextParagraph, {text: errors ?? ''})
          : m(
              'article',
              {
                style: {
                  display: 'flex',
                  flexDirection: 'column',
                  flexGrow: 1,
                },
              },
              [
                m(QueryTable, {
                  trace: attrs.trace,
                  query:
                    attrs.query instanceof Error ? '' : queryToRun(attrs.query),
                  resp: this.queryResult,
                  fillParent: true,
                }),
              ],
            ),
      ),
    ];
  }
}
