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
import {TextParagraph} from '../../../widgets/text_paragraph';
import {QueryTable} from '../../../components/query_table/query_table';
import {runQuery} from '../../../components/query_table/queries';
import {AsyncLimiter} from '../../../base/async_limiter';
import {QueryResponse} from '../../../components/query_table/queries';
import {SegmentedButtons} from '../../../widgets/segmented_buttons';
import {QueryNode, getLastFinishedNode} from '../query_state';
import {ColumnController, ColumnControllerDiff} from './column_controller';
import {Section} from '../../../widgets/section';
import {Engine} from '../../../trace_processor/engine';
import protos from '../../../protos';

export interface DataSourceAttrs extends PageWithTraceAttrs {
  readonly queryNode: QueryNode;
}

export class DataSourceViewer implements m.ClassComponent<DataSourceAttrs> {
  private readonly tableAsyncLimiter = new AsyncLimiter();

  private queryResult: QueryResponse | undefined;
  private showDataSourceInfoPanel: number = 0;

  private prevSqString?: string;
  private curSqString?: string;

  private currentSql?: string;

  view({attrs}: m.CVnode<DataSourceAttrs>) {
    function renderPickColumns(node: QueryNode): m.Child {
      return (
        node.columns &&
        m(ColumnController, {
          hasValidColumns: true,
          options: node.columns,
          onChange: (diffs: ColumnControllerDiff[]) => {
            diffs.forEach(({id, checked, alias}) => {
              if (node.columns === undefined) {
                return;
              }
              for (const option of node.columns) {
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
          query: this.currentSql,
          resp: this.queryResult,
          fillParent: false,
        })
      );
    };

    const renderButtons = (): m.Child => {
      return m(SegmentedButtons, {
        ...attrs,
        options: [
          {label: 'Show SQL'},
          {label: 'Show columns'},
          {label: 'Show proto'},
        ],
        selectedOption: this.showDataSourceInfoPanel,
        onOptionSelected: (num) => {
          this.showDataSourceInfoPanel = num;
        },
      });
    };
    const lastNode = getLastFinishedNode(attrs.queryNode);
    if (lastNode === undefined) return;

    const sq = lastNode.getStructuredQuery();
    if (sq === undefined) {
      return;
    }
    this.curSqString = JSON.stringify(sq.toJSON());

    if (this.curSqString !== this.prevSqString) {
      this.tableAsyncLimiter.schedule(async () => {
        this.currentSql = await sqlToRun(lastNode, attrs.trace.engine);
        if (this.currentSql === undefined) {
          return;
        }
        this.queryResult = await runQuery(this.currentSql, attrs.trace.engine);
        this.prevSqString = this.curSqString;
      });
    }

    if (this.currentSql === undefined) {
      return;
    }

    const jsonSq = attrs.queryNode.getStructuredQuery()?.toJSON();
    return (
      this.currentSql && [
        m(
          Section,
          {title: lastNode.getTitle()},
          renderButtons(),
          this.showDataSourceInfoPanel === 0 &&
            m(TextParagraph, {
              text: this.currentSql,
              compressSpace: false,
            }),
          this.showDataSourceInfoPanel === 1 && renderPickColumns(lastNode),
          this.showDataSourceInfoPanel === 2 &&
            jsonSq &&
            m(TextParagraph, {
              text: JSON.stringify(jsonSq) || '',
              compressSpace: false,
            }),
        ),
        renderTable(),
      ]
    );
  }
}

function getStructuredQueries(
  finalNode: QueryNode,
): protos.PerfettoSqlStructuredQuery[] | undefined {
  if (!finalNode.finished || finalNode.columns === undefined) {
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

async function sqlToRun(
  node: QueryNode,
  engine: Engine,
): Promise<string | undefined> {
  const structuredQueries = getStructuredQueries(node);
  if (structuredQueries === undefined) return;

  const res = await engine.analyzeStructuredQuery(structuredQueries);
  if (
    res.error ||
    res.sql.length === 0 ||
    res.sql.length !== structuredQueries.length
  ) {
    return;
  }

  return res.sql[res.sql.length - 1];
}
