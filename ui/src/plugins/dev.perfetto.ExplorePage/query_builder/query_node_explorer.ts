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

  private currentSql?: Query;

  view({attrs}: m.CVnode<QueryNodeExplorerAttrs>) {
    const renderTable = () => {
      if (this.queryResult === undefined) {
        return;
      }
      if (this.queryResult.error !== undefined) {
        return m(TextParagraph, {text: `Error: ${this.queryResult.error}`});
      }
      return (
        this.currentSql &&
        m(QueryTable, {
          trace: attrs.trace,
          query: queryToRun(this.currentSql),
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

    const sq = attrs.node.getStructuredQuery();
    if (sq === undefined) return;

    this.curSqString = JSON.stringify(sq.toJSON(), null, 2);

    if (this.curSqString !== this.prevSqString) {
      this.tableAsyncLimiter.schedule(async () => {
        this.currentSql = await analyzeNode(attrs.node, attrs.trace.engine);
        if (this.currentSql === undefined) {
          return;
        }
        this.queryResult = await runQueryForQueryTable(
          queryToRun(this.currentSql),
          attrs.trace.engine,
        );
        this.prevSqString = this.curSqString;
      });
    }

    if (this.currentSql === undefined) return;
    const sql = queryToRun(this.currentSql);
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
              onclick: () => copyToClipboard(sql ?? ''),
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
              onclick: () => copyToClipboard(this.currentSql?.textproto ?? ''),
              icon: Icons.Copy,
            }),
            m('code', this.currentSql.textproto),
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

export function queryToRun(sql: Query): string {
  const includes = sql.modules.map((c) => `INCLUDE PERFETTO MODULE ${c};\n`);
  return includes + sql.sql;
}

export async function analyzeNode(
  node: QueryNode,
  engine: Engine,
): Promise<Query | undefined> {
  const structuredQueries = getStructuredQueries(node);
  if (structuredQueries === undefined) return;

  const res = await engine.analyzeStructuredQuery(structuredQueries);

  if (res.error) throw Error(res.error);
  if (res.results.length === 0) throw Error('No structured query results');
  if (res.results.length !== structuredQueries.length) {
    throw Error(
      `Wrong structured query results. Asked for ${structuredQueries.length}, received ${res.results.length}`,
    );
  }

  const lastRes = res.results[res.results.length - 1];
  if (lastRes.sql === null || lastRes.sql === undefined) {
    return;
  }
  if (!lastRes.textproto) {
    throw Error('No textproto in structured query results');
  }

  const sql: Query = {
    sql: lastRes.sql,
    textproto: lastRes.textproto ?? '',
    modules: lastRes.modules ?? [],
    preambles: lastRes.preambles ?? [],
  };
  return sql;
}
