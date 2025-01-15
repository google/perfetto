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
import SqlModulesPlugin from '../../dev.perfetto.SqlModules';

import {PageWithTraceAttrs} from '../../../public/page';
import {Button} from '../../../widgets/button';
import {TextParagraph} from '../../../widgets/text_paragraph';
import {QueryTable} from '../../../components/query_table/query_table';
import {runQuery} from '../../../components/query_table/queries';
import {AsyncLimiter} from '../../../base/async_limiter';
import {QueryResponse} from '../../../components/query_table/queries';
import {Trace} from '../../../public/trace';
import {SegmentedButtons} from '../../../widgets/segmented_buttons';
import {QueryNode, getLastFinishedNode, getFirstNode} from '../query_state';
import {ColumnControllerRows} from './column_controller';

export interface DataSourceAttrs extends PageWithTraceAttrs {
  readonly plugin: SqlModulesPlugin;
  readonly queryNode: QueryNode;
}

export class DataSourceViewer implements m.ClassComponent<DataSourceAttrs> {
  private readonly tableAsyncLimiter = new AsyncLimiter();
  private queryResult: QueryResponse | undefined;
  private showSql: number = 0;
  private currentSql?: string;

  private renderRunButton(sql: string, trace: Trace): m.Child {
    return m(Button, {
      label: 'Run',
      onclick: () => {
        this.tableAsyncLimiter.schedule(async () => {
          this.queryResult = await runQuery(sql, trace.engine);
        });
      },
    });
  }

  view({attrs}: m.CVnode<DataSourceAttrs>) {
    this.currentSql = sqlToRun(attrs.queryNode);
    if (this.currentSql === undefined) {
      return;
    }

    const renderTable = (queryResp: QueryResponse | undefined) => {
      if (queryResp === undefined) {
        return;
      }
      if (queryResp.error !== undefined) {
        return m(TextParagraph, {text: `Error: ${queryResp.error}`});
      }

      return [
        this.currentSql &&
          m(QueryTable, {
            trace: attrs.trace,
            query: this.currentSql,
            resp: queryResp,
            fillParent: false,
          }),
      ];
    };

    const renderButtons = (): m.Child => {
      return m(SegmentedButtons, {
        ...attrs,
        options: [{label: 'Show SQL'}, {label: 'Show columns'}],
        selectedOption: this.showSql,
        onOptionSelected: (num) => {
          this.showSql = num;
        },
      });
    };

    return (
      this.currentSql &&
      m(
        '.explore-page__rowish',
        m(
          '.explore-page__columnar',
          this.renderRunButton(this.currentSql, attrs.trace),
          renderTable(this.queryResult),
        ),
        m(
          '.explore-page__columnar',
          renderButtons(),
          this.showSql === 0
            ? m(TextParagraph, {
                text: this.currentSql,
                compressSpace: false,
              })
            : m(TextParagraph, {
                text: 'FUTURE COLUMNS',
              }),
        ),
      )
    );
  }
}

function getImports(node: QueryNode): string[] | undefined {
  if (!node.finished) {
    return;
  }

  const imports = new Set<string>();
  while (node.nextNode) {
    if (!node.nextNode.finished) {
      node.imports?.forEach((i) => imports.add(i));
    }
    node = node.nextNode;
  }
  return Array.from(imports);
}

function getSource(node: QueryNode): string | undefined {
  let currentNode = getFirstNode(node);
  if (currentNode === undefined) {
    return;
  }

  const ret: string[] = [];
  while (currentNode.finished) {
    const curSql = currentNode.getSourceSql();
    if (curSql === undefined) {
      return;
    }
    ret.push(curSql);
    if (currentNode.nextNode === undefined) {
      break;
    }
    currentNode = currentNode.nextNode;
  }
  return ret.join('\n');
}

function sqlToRun(node: QueryNode): string | undefined {
  const currentNode = getLastFinishedNode(node);
  if (currentNode === undefined || currentNode.columns === undefined) {
    return;
  }

  const imports = getImports(currentNode);
  if (imports === undefined) {
    return;
  }
  const importsStr = imports
    .map((i) => `INCLUDE PERFETTO MODULE ${i};`)
    .join('\n');

  const colsStr: string = currentNode.columns
    .filter((c) => c.checked)
    .map((c) => getColStr(c))
    .join(',\n  ');

  const sourceStr = getSource(node);
  if (sourceStr === undefined) {
    return;
  }

  return `${importsStr}\n\nSELECT\n  ${colsStr}\nFROM ${sourceStr};`.trim();
}

function getColStr(col: ColumnControllerRows) {
  const colWithSource = col.source
    ? `${col.source}.${col.column.name}`
    : col.column.name;
  return col.alias ? `${colWithSource} AS ${col.alias}` : colWithSource;
}
