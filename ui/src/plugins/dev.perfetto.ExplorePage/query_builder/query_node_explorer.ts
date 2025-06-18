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

import {AsyncLimiter} from '../../../base/async_limiter';
import {NodeType, QueryNode} from '../query_node';
import {Engine} from '../../../trace_processor/engine';
import protos from '../../../protos';
import {copyToClipboard} from '../../../base/clipboard';
import {Button} from '../../../widgets/button';
import {Icon} from '../../../widgets/icon';
import {Icons} from '../../../base/semantic_icons';
import {Operator} from './operations/operation_component';
import {Trace} from '../../../public/trace';
import {MenuItem, PopupMenu} from '../../../widgets/menu';
import {TextInput} from '../../../widgets/text_input';

export interface QueryNodeExplorerAttrs {
  readonly node: QueryNode;
  readonly trace: Trace;
  readonly onQueryAnalyzed: (query: Query) => void;
}

enum SelectedView {
  kModify = 0,
  kSql = 1,
  kProto = 2,
}

export class QueryNodeExplorer
  implements m.ClassComponent<QueryNodeExplorerAttrs>
{
  private readonly tableAsyncLimiter = new AsyncLimiter();

  private selectedView: number = 0;

  private prevSqString?: string;
  private curSqString?: string;

  private currentQuery?: Query | Error;
  view({attrs}: m.CVnode<QueryNodeExplorerAttrs>) {
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
      switch (attrs.node.type) {
        case NodeType.kSimpleSlices:
        case NodeType.kStdlibTable:
          return m(Operator, {
            filter: {
              sourceCols: attrs.node.state.sourceCols,
              filters: attrs.node.state.filters,
            },
            groupby: {
              groupByColumns: attrs.node.state.groupByColumns,
              aggregations: attrs.node.state.aggregations,
            },
          });
        case NodeType.kSqlSource:
          return;
      }
    };

    const getAndRunQuery = (): void => {
      const sq = attrs.node.getStructuredQuery();
      if (sq === undefined) return;

      this.curSqString = JSON.stringify(sq.toJSON(), null, 2);

      if (this.curSqString !== this.prevSqString) {
        this.tableAsyncLimiter.schedule(async () => {
          this.currentQuery = await analyzeNode(attrs.node, attrs.trace.engine);
          if (!isAQuery(this.currentQuery)) {
            return;
          }
          attrs.onQueryAnalyzed(this.currentQuery);
          this.prevSqString = this.curSqString;
        });
      }
    };

    getAndRunQuery();
    const sql: string = isAQuery(this.currentQuery)
      ? queryToRun(this.currentQuery)
      : '';
    const textproto: string = isAQuery(this.currentQuery)
      ? this.currentQuery.textproto
      : '';

    return [
      m(
        '.pf-node-explorer',
        m(
          '.pf-node-explorer__title-row',
          !attrs.node.validate() &&
            m(Icon, {
              icon: Icons.Warning,
              filled: true,
              title: 'Invalid node',
            }),
          m(
            '.title',
            m(TextInput, {
              placeholder: attrs.node.getTitle(),
              oninput: (e: KeyboardEvent) => {
                if (!e.target) return;
                attrs.node.state.customTitle = (
                  e.target as HTMLInputElement
                ).value.trim();
                if (attrs.node.state.customTitle === '') {
                  attrs.node.state.customTitle = undefined;
                }
              },
            }),
          ),
          m('span.spacer'), // Added spacer to push menu to the right
          renderModeMenu(),
        ),
        m(
          'article',
          attrs.node.coreModify(),
          this.selectedView === SelectedView.kSql &&
            m(
              '.code-snippet',
              m(Button, {
                title: 'Copy to clipboard',
                onclick: () => copyToClipboard(sql),
                icon: Icons.Copy,
              }),
              m('code', sql),
            ),
          this.selectedView === SelectedView.kModify && operators(),
          this.selectedView === SelectedView.kProto &&
            m(
              '.code-snippet',
              m(Button, {
                title: 'Copy to clipboard',
                onclick: () => copyToClipboard(textproto),
                icon: Icons.Copy,
              }),
              m('code', textproto),
            ),
        ),
      ),
    ];
  }
}

function getStructuredQueries(
  finalNode: QueryNode,
): protos.PerfettoSqlStructuredQuery[] | undefined {
  if (finalNode.finalCols === undefined) {
    return;
  }
  const revStructuredQueries: protos.PerfettoSqlStructuredQuery[] = [];
  let curNode: QueryNode | undefined = finalNode;
  while (curNode) {
    const curSq = curNode.getStructuredQuery();
    if (curSq === undefined) {
      return;
    }
    revStructuredQueries.push(curSq);
    if (curNode.prevNode && !curNode.prevNode.validate()) {
      return;
    }
    curNode = curNode.prevNode;
  }
  return revStructuredQueries.reverse();
}

export interface Query {
  sql: string;
  textproto: string;
  modules: string[];
  preambles: string[];
}

export function queryToRun(sql?: Query): string {
  if (sql === undefined) return 'N/A';
  const includes = sql.modules.map((c) => `INCLUDE PERFETTO MODULE ${c};\n`);
  return includes + sql.sql;
}

export async function analyzeNode(
  node: QueryNode,
  engine: Engine,
): Promise<Query | undefined | Error> {
  const structuredQueries = getStructuredQueries(node);
  if (structuredQueries === undefined) return;

  const res = await engine.analyzeStructuredQuery(structuredQueries);
  if (res.error) return Error(res.error);
  if (res.results.length === 0) return Error('No structured query results');
  if (res.results.length !== structuredQueries.length) {
    return Error(
      `Wrong structured query results. Asked for ${structuredQueries.length}, received ${res.results.length}`,
    );
  }

  const lastRes = res.results[res.results.length - 1];
  if (lastRes.sql === null || lastRes.sql === undefined) {
    return;
  }
  if (!lastRes.textproto) {
    return Error('No textproto in structured query results');
  }

  const sql: Query = {
    sql: lastRes.sql,
    textproto: lastRes.textproto ?? '',
    modules: lastRes.modules ?? [],
    preambles: lastRes.preambles ?? [],
  };
  return sql;
}

export function isAQuery(
  maybeQuery: Query | undefined | Error,
): maybeQuery is Query {
  return (
    maybeQuery !== undefined &&
    !(maybeQuery instanceof Error) &&
    maybeQuery.sql !== undefined
  );
}
