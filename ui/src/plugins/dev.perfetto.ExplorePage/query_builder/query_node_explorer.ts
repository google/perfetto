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
import {SegmentedButtons} from '../../../widgets/segmented_buttons';
import {QueryNode} from '../query_node';
import {Section} from '../../../widgets/section';
import {Engine} from '../../../trace_processor/engine';
import protos from '../../../protos';
import {copyToClipboard} from '../../../base/clipboard';
import {Button} from '../../../widgets/button';
import {Icons} from '../../../base/semantic_icons';
import {Operator} from './operations/operation_component';
import {Trace} from 'src/public/trace';

export interface QueryNodeExplorerAttrs {
  readonly node: QueryNode;
  readonly trace: Trace;
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

  private queryResult: QueryResponse | undefined;
  private selectedView: number = 0;

  private prevSqString?: string;
  private curSqString?: string;

  private currentQuery?: Query | Error;

  private getAndRunQuery(node: QueryNode, engine: Engine): undefined {
    console.log('getAndRunQuery', node);
    const sq = node.getStructuredQuery();
    if (sq === undefined) return;
    console.log('sq', sq);

    this.curSqString = JSON.stringify(sq.toJSON(), null, 2);

    if (this.curSqString !== this.prevSqString) {
      this.tableAsyncLimiter.schedule(async () => {
        this.currentQuery = await analyzeNode(node, engine);
        if (!isQueryValid(this.currentQuery)) {
          return;
        }
        this.queryResult = await runQueryForQueryTable(
          queryToRun(this.currentQuery),
          engine,
        );
        this.prevSqString = this.curSqString;
      });
    }
  }

  view({attrs}: m.CVnode<QueryNodeExplorerAttrs>) {
    const renderTable = () => {
      if (this.currentQuery === undefined) {
        return m(TextParagraph, {text: `No data to display}`});
      }
      if (this.currentQuery instanceof Error) {
        return m(TextParagraph, {text: `Error: ${this.currentQuery.message}`});
      }
      if (this.queryResult === undefined) {
        this.getAndRunQuery(attrs.node, attrs.trace.engine);
        return m(TextParagraph, {text: `No data to display`});
      }
      if (this.queryResult.error !== undefined) {
        return m(TextParagraph, {text: `Error: ${this.queryResult.error}`});
      }
      return (
        this.currentQuery &&
        m(QueryTable, {
          trace: attrs.trace,
          query: queryToRun(this.currentQuery),
          resp: this.queryResult,
          fillParent: false,
        })
      );
    };

    const renderSelectedViewButtons = (): m.Child => {
      return m(SegmentedButtons, {
        ...attrs,
        options: [
          {label: 'Modify'},
          {label: 'Show SQL'},
          {label: 'Show proto'},
        ],
        selectedOption: this.selectedView,
        onOptionSelected: (num) => {
          this.selectedView = num;
        },
      });
    };

    this.getAndRunQuery(attrs.node, attrs.trace.engine);
    const sql: string = isQueryValid(this.currentQuery)
      ? queryToRun(this.currentQuery)
      : '';
    const textproto: string = isQueryValid(this.currentQuery)
      ? this.currentQuery.textproto
      : '';

    return [
      m(
        Section,
        {title: attrs.node.getTitle()},
        attrs.node.getDetails(),
        renderSelectedViewButtons(),
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
        this.selectedView === SelectedView.kModify &&
          m(Operator, {
            filter: {
              sourceCols: attrs.node.state.sourceCols,
              filters: attrs.node.state.filters,
            },
            groupby: {
              groupByColumns: attrs.node.state.groupByColumns,
              aggregations: attrs.node.state.aggregations,
            },
          }),
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
      m(Section, {title: 'Sample data'}, renderTable()),
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

export function isQueryValid(
  maybeQuery: Query | undefined | Error,
): maybeQuery is Query {
  return (
    maybeQuery !== undefined &&
    !(maybeQuery instanceof Error) &&
    maybeQuery.sql !== undefined
  );
}
