// Copyright (C) 2023 The Android Open Source Project
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
import {
  QueryResponse,
  runQueryForQueryTable,
} from '../../components/query_table/queries';
import {DataSource} from '../../components/widgets/datagrid/data_source';
import {InMemoryDataSource} from '../../components/widgets/datagrid/in_memory_data_source';
import {Trace} from '../../public/trace';
import {Tab} from '../../public/tab';
import {DetailsShell} from '../../widgets/details_shell';
import {ResultsData, ResultsTable} from './results_table';

interface QueryResultTabConfig {
  readonly query: string;
  readonly title: string;
}

export class QueryResultsTab implements Tab {
  private queryResponse?: QueryResponse;
  private dataSource?: DataSource;

  constructor(
    private readonly trace: Trace,
    private readonly args: QueryResultTabConfig,
  ) {
    // Run the query and load data when the tab is created
    this.loadData();
  }

  private async loadData() {
    const result = await runQueryForQueryTable(
      this.args.query,
      this.trace.engine,
    );
    this.queryResponse = result;

    if (result.error === undefined) {
      this.dataSource = new InMemoryDataSource(this.queryResponse.rows);
    }
  }

  getTitle(): string {
    const suffix = this.queryResponse
      ? ` (${this.queryResponse.rows.length})`
      : '';
    return `${this.args.title}${suffix}`;
  }

  render(): m.Children {
    const resp = this.queryResponse;

    return m(
      DetailsShell,
      {
        title: this.args.title,
        description: resp ? this.args.query : 'Loading...',
      },
      resp && this.renderResponse(resp),
    );
  }

  private renderResponse(resp: QueryResponse): m.Children {
    const data: ResultsData = resp.error
      ? {kind: 'error', errorMessage: resp.error}
      : {
          kind: 'success',
          columns: resp.columns,
          rows: resp.rows,
          dataSource: this.dataSource!,
          rowCount: resp.totalRowCount,
          queryTimeMs: resp.durationMs,
          query: this.args.query,
          lastStatementSql: resp.lastStatementSql,
          statementCount: resp.statementCount,
          statementWithOutputCount: resp.statementWithOutputCount,
        };

    return m(ResultsTable, {
      data,
      fillHeight: true,
      trace: this.trace,
      onIdClick: (sqlTable, id, doubleClick) => {
        this.trace.selection.selectSqlEvent(sqlTable, id, {
          switchToCurrentSelectionTab: doubleClick,
          scrollToSelection: true,
        });
      },
    });
  }

  isLoading() {
    return this.queryResponse === undefined;
  }
}
