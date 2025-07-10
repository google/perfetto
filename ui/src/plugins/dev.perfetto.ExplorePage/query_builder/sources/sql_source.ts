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
import {
  createFinalColumns,
  createSelectColumnsProto,
  NodeType,
  QueryNode,
  QueryNodeState,
} from '../../query_node';
import {
  ColumnInfo,
  columnInfoFromName,
  newColumnInfoList,
} from '../column_info';
import protos from '../../../../protos';
import {Editor} from '../../../../widgets/editor';
import {
  createFiltersProto,
  createGroupByProto,
} from '../operations/operation_component';
import {QueryHistoryComponent, queryHistoryStorage} from '../query_history';

export interface SqlSourceAttrs extends QueryNodeState {
  sql?: string;
  sqlColumns?: string[];
  onExecute?: (sql: string) => void;
}

export class SqlSourceNode implements QueryNode {
  readonly type: NodeType = NodeType.kSqlSource;
  readonly prevNode = undefined;
  nextNode?: QueryNode;

  readonly sourceCols: ColumnInfo[];
  readonly finalCols: ColumnInfo[];

  readonly state: SqlSourceAttrs;
  private text: string;
  private generation = 0;

  constructor(attrs: SqlSourceAttrs) {
    this.state = attrs;
    this.sourceCols = attrs.sqlColumns?.map((c) => columnInfoFromName(c)) ?? [];
    this.finalCols = createFinalColumns(this);
    this.text = this.state.sql ?? '';
  }

  getStateCopy(): QueryNodeState {
    const newState: SqlSourceAttrs = {
      sql: this.state.sql,
      sqlColumns: this.state.sqlColumns,
      onExecute: this.state.onExecute,
      sourceCols: newColumnInfoList(this.sourceCols),
      groupByColumns: newColumnInfoList(this.state.groupByColumns),
      filters: this.state.filters.map((f) => ({...f})),
      aggregations: this.state.aggregations.map((a) => ({...a})),
      customTitle: this.state.customTitle,
    };
    return newState;
  }

  validate(): boolean {
    return this.state.sql !== undefined && this.state.sql.trim() !== '';
  }

  getTitle(): string {
    return this.state.customTitle ?? 'Sql source';
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = `sql_source`;
    const sqlProto = new protos.PerfettoSqlStructuredQuery.Sql();

    if (this.state.sql) sqlProto.sql = this.state.sql;
    if (this.state.sqlColumns) sqlProto.columnNames = this.state.sqlColumns;
    sq.sql = sqlProto;

    const filtersProto = createFiltersProto(this.state.filters);
    if (filtersProto) sq.filters = filtersProto;
    const groupByProto = createGroupByProto(
      this.state.groupByColumns,
      this.state.aggregations,
    );
    if (groupByProto) sq.groupBy = groupByProto;

    const selectedColumns = createSelectColumnsProto(this);
    if (selectedColumns) sq.selectColumns = selectedColumns;
    return sq;
  }

  coreModify(): m.Child {
    const runQuery = (sql: string) => {
      this.state.sql = sql.trim();
      if (this.state.onExecute) {
        this.state.onExecute(this.state.sql);
      }
      m.redraw();
    };

    return m(
      '.sql-source-node',
      {
        style: {
          display: 'flex',
          flexDirection: 'column',
        },
      },
      m(
        'div',
        {style: {minHeight: '400px', backgroundColor: '#282c34'}},

        m(Editor, {
          initialText: this.text,
          generation: this.generation,
          onUpdate: (text: string) => {
            this.text = text;
          },
          onExecute: (text: string) => {
            queryHistoryStorage.saveQuery(text);
            runQuery(text);
          },
          autofocus: true,
        }),
      ),
      m(
        '.query-history-container',
        m(QueryHistoryComponent, {
          runQuery,
          setQuery: (q: string) => {
            this.text = q;
            this.generation++;
            m.redraw();
          },
        }),
      ),
    );
  }
}
