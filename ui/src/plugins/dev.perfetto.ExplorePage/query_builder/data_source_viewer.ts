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
import {NodeType, QueryNode} from '../query_node';
import {ColumnController, ColumnControllerDiff} from './column_controller';
import {Section} from '../../../widgets/section';
import {Engine} from '../../../trace_processor/engine';
import protos from '../../../protos';
import {copyToClipboard} from '../../../base/clipboard';
import {Button} from '../../../widgets/button';
import {Icons} from '../../../base/semantic_icons';
import {Trace} from '../../../public/trace';

export interface DataSourceAttrs {
  readonly trace: Trace;
  readonly queryNode: QueryNode;
}

enum SelectedView {
  COLUMNS = 0,
  SQL = 1,
  PROTO = 2,
}

export class DataSourceViewer implements m.ClassComponent<DataSourceAttrs> {
  private readonly tableAsyncLimiter = new AsyncLimiter();

  private queryResult: QueryResponse | undefined;
  private showDataSourceInfoPanel: number = 0;

  private prevSqString?: string;
  private curSqString?: string;

  private currentSql?: Query;

  view({attrs}: m.CVnode<DataSourceAttrs>) {
    function renderPickColumns(node: QueryNode): m.Child {
      return m(ColumnController, {
        options: node.finalCols,
        onChange: (diffs: ColumnControllerDiff[]) => {
          diffs.forEach(({id, checked, alias}) => {
            if (node.finalCols === undefined) {
              return;
            }
            for (const option of node.finalCols) {
              if (option.id === id) {
                option.checked = checked;
                option.alias = alias;
              }
            }
          });
        },
      });
    }

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

    const renderButtons = (): m.Child => {
      return m(SegmentedButtons, {
        ...attrs,
        options: [
          {label: 'Show columns'},
          {label: 'Show SQL'},
          {label: 'Show proto'},
        ],
        selectedOption: this.showDataSourceInfoPanel,
        onOptionSelected: (num) => {
          this.showDataSourceInfoPanel = num;
        },
      });
    };

    const sq = attrs.queryNode.getStructuredQuery();
    if (sq === undefined) return;

    this.curSqString = JSON.stringify(sq.toJSON(), null, 2);

    if (this.curSqString !== this.prevSqString) {
      this.tableAsyncLimiter.schedule(async () => {
        this.currentSql = await analyzeNode(
          attrs.queryNode,
          attrs.trace.engine,
        );
        if (this.currentSql === undefined) {
          return;
        }
        this.queryResult = await runQueryForQueryTable(
          attrs.queryNode.type === NodeType.kSqlSource
            ? queryToRun(this.currentSql)
            : `${queryToRun(this.currentSql)} LIMIT 50`,
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
        {title: attrs.queryNode.getTitle()},
        attrs.queryNode.getDetails(),
        renderButtons(),
        this.showDataSourceInfoPanel === SelectedView.SQL &&
          m(
            '.code-snippet',
            m(Button, {
              title: 'Copy to clipboard',
              onclick: () => copyToClipboard(sql ?? ''),
              icon: Icons.Copy,
            }),
            m('code', sql),
          ),
        this.showDataSourceInfoPanel === SelectedView.COLUMNS &&
          renderPickColumns(attrs.queryNode),
        this.showDataSourceInfoPanel === SelectedView.PROTO &&
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
