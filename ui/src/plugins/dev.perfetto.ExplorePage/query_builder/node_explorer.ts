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

import {classNames} from '../../../base/classnames';
import {AsyncLimiter} from '../../../base/async_limiter';
import {ExplorePageHelp} from './help';
import {
  analyzeNode,
  isAQuery,
  NodeType,
  Query,
  QueryNode,
  queryToRun,
  setOperationChanged,
} from '../query_node';
import {Button} from '../../../widgets/button';
import {Icon} from '../../../widgets/icon';
import {Icons} from '../../../base/semantic_icons';
import {FilterDefinition} from '../../../components/widgets/data_grid/common';
import {Operator} from './operations/operation_component';
import {Trace} from '../../../public/trace';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {TextInput} from '../../../widgets/text_input';
import {SqlSourceNode} from './sources/sql_source';
import {CodeSnippet} from '../../../widgets/code_snippet';

export interface NodeExplorerAttrs {
  readonly node?: QueryNode;
  readonly trace: Trace;
  readonly onQueryAnalyzed: (query: Query | Error, reexecute?: boolean) => void;
  readonly onExecute: () => void;
  readonly onchange?: () => void;
}

enum SelectedView {
  kModify = 0,
  kSql = 1,
  kProto = 2,
}

export class NodeExplorer implements m.ClassComponent<NodeExplorerAttrs> {
  private readonly tableAsyncLimiter = new AsyncLimiter();

  private selectedView: number = 0;

  private prevSqString?: string;

  private currentQuery?: Query | Error;
  private sqlForDisplay?: string;

  view({attrs}: m.CVnode<NodeExplorerAttrs>) {
    const {node} = attrs;
    if (!node) {
      return m(ExplorePageHelp);
    }

    const renderModeMenu = (): m.Child => {
      return m(
        PopupMenu,
        {
          trigger: m(Button, {
            icon: Icons.ContextMenuAlt,
          }),
        },
        [
          m(MenuItem, {
            label: 'Modify',
            onclick: () => {
              this.selectedView = SelectedView.kModify;
            },
          }),
          m(MenuItem, {
            label: 'Show SQL',
            onclick: () => {
              this.selectedView = SelectedView.kSql;
            },
          }),
          m(MenuItem, {
            label: 'Show proto',
            onclick: () => {
              this.selectedView = SelectedView.kProto;
            },
          }),
        ],
      );
    };

    const operators = (): m.Child => {
      switch (node.type) {
        case NodeType.kSimpleSlices:
        case NodeType.kTable:
          return m(Operator, {
            filter: {
              sourceCols: node.state.sourceCols,
              filters: node.state.filters,
              onFiltersChanged: (
                newFilters: ReadonlyArray<FilterDefinition>,
              ) => {
                node.state.filters = newFilters as FilterDefinition[];
                attrs.onchange?.();
              },
            },
            groupby: {
              groupByColumns: node.state.groupByColumns,
              aggregations: node.state.aggregations,
            },
            onchange: () => {
              setOperationChanged(node);
              attrs.onchange?.();
            },
          });
        case NodeType.kSqlSource:
          return;
      }
    };

    const getAndRunQuery = (): void => {
      if (node.type === NodeType.kSqlSource) {
        const sql = (node as SqlSourceNode).state.sql ?? '';
        const sq = node.getStructuredQuery();
        const newSqString = sq ? JSON.stringify(sq.toJSON(), null, 2) : '';

        const rawSqlHasChanged =
          !this.currentQuery ||
          !isAQuery(this.currentQuery) ||
          sql !== this.currentQuery.sql;

        if (newSqString !== this.prevSqString || rawSqlHasChanged) {
          if (sq) {
            this.tableAsyncLimiter.schedule(async () => {
              const analyzedQuery = await analyzeNode(node, attrs.trace.engine);
              if (isAQuery(analyzedQuery)) {
                this.sqlForDisplay = queryToRun(analyzedQuery);
              }
              m.redraw();
            });
          }

          this.currentQuery = {
            sql,
            textproto: newSqString,
            modules: [],
            preambles: [],
          };
          attrs.onQueryAnalyzed(this.currentQuery, false);
          this.prevSqString = newSqString;

          if (rawSqlHasChanged) {
            attrs.onExecute();
          }
        }
        return;
      }

      const sq = node.getStructuredQuery();
      if (sq === undefined) return;

      const curSqString = JSON.stringify(sq.toJSON(), null, 2);

      if (curSqString !== this.prevSqString) {
        this.tableAsyncLimiter.schedule(async () => {
          this.currentQuery = await analyzeNode(node, attrs.trace.engine);
          if (!isAQuery(this.currentQuery)) {
            return;
          }
          attrs.onQueryAnalyzed(this.currentQuery);
          attrs.onExecute();
          this.prevSqString = curSqString;
        });
      }
    };

    getAndRunQuery();
    const sql: string =
      this.sqlForDisplay ??
      (isAQuery(this.currentQuery)
        ? queryToRun(this.currentQuery)
        : 'SQL not available.');
    const textproto: string = isAQuery(this.currentQuery)
      ? this.currentQuery.textproto
      : this.currentQuery instanceof Error
        ? this.currentQuery.message
        : 'Proto not available.';

    return [
      m(
        `.pf-node-explorer${
          node.type === NodeType.kSqlSource
            ? '.pf-node-explorer-sql-source'
            : ''
        }`,
        m(
          '.pf-node-explorer__title-row',
          m(
            '.title',
            (!node.validate() ||
              node.state.queryError ||
              node.state.responseError ||
              node.state.dataError) &&
              m(Icon, {
                icon: Icons.Warning,
                filled: true,
                className: classNames(
                  (!node.validate() || node.state.queryError) &&
                    'pf-node-explorer__warning-icon--error',
                  node.state.responseError &&
                    'pf-node-explorer__warning-icon--warning',
                ),
                title:
                  `Invalid node: \n` +
                  (node.state.queryError?.message ?? '') +
                  (node.state.responseError?.message ?? '') +
                  (node.state.dataError?.message ?? ''),
              }),
            m(TextInput, {
              placeholder: node.getTitle(),
              oninput: (e: InputEvent) => {
                if (!e.target) return;
                node.state.customTitle = (
                  e.target as HTMLInputElement
                ).value.trim();
                if (node.state.customTitle === '') {
                  node.state.customTitle = undefined;
                }
              },
            }),
          ),
          m('span.spacer'), // Added spacer to push menu to the right
          renderModeMenu(),
        ),
        m(
          'article',
          this.selectedView === SelectedView.kModify && [
            node.nodeSpecificModify(),
            operators(),
          ],
          this.selectedView === SelectedView.kSql &&
            (isAQuery(this.currentQuery)
              ? m(CodeSnippet, {language: 'SQL', text: sql})
              : m('div', sql)),
          this.selectedView === SelectedView.kProto &&
            (isAQuery(this.currentQuery)
              ? m(CodeSnippet, {text: textproto, language: 'textproto'})
              : m('div', textproto)),
        ),
      ),
    ];
  }
}
