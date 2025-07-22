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
import {Icon} from '../../../../widgets/icon';
import {Icons} from '../../../../base/semantic_icons';
import {
  createFiltersProto,
  createGroupByProto,
} from '../operations/operation_component';
import {
  QueryHistoryComponent,
  queryHistoryStorage,
} from '../../../../components/widgets/query_history';
import {Trace} from '../../../../public/trace';

export interface SqlSourceState extends QueryNodeState {
  sql?: string;
  onExecute?: (sql: string) => void;
  responseError?: Error;
  trace: Trace;
}

export class SqlSourceNode implements QueryNode {
  readonly type: NodeType = NodeType.kSqlSource;
  readonly prevNode = undefined;
  nextNode?: QueryNode;

  sourceCols: ColumnInfo[];
  finalCols: ColumnInfo[];

  readonly state: SqlSourceState;
  private text: string;

  constructor(attrs: SqlSourceState) {
    this.state = attrs;
    this.sourceCols = attrs.sourceCols ?? [];
    this.finalCols = createFinalColumns(this);
    this.text = this.state.sql ?? '';
  }

  setSourceColumns(columns: string[]) {
    this.state.sourceCols = columns.map((c) => columnInfoFromName(c));
    m.redraw();
  }

  getStateCopy(): QueryNodeState {
    const newState: SqlSourceState = {
      sql: this.state.sql,
      onExecute: this.state.onExecute,
      sourceCols: newColumnInfoList(this.sourceCols),
      groupByColumns: [],
      filters: [],
      aggregations: [],
      customTitle: this.state.customTitle,
      trace: this.state.trace,
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
    sqlProto.columnNames = this.state.sourceCols.map((c) => c.column.name);
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

  nodeSpecificModify(): m.Child {
    const runQuery = (sql: string) => {
      this.state.sql = sql.trim();
      if (this.state.onExecute) {
        this.state.onExecute(this.state.sql);
      }
      m.redraw();
    };

    return m(
      '.sql-source-node',
      m(
        'div',
        {
          style: {
            minHeight: '400px',
            backgroundColor: '#282c34',
            position: 'relative',
          },
        },
        this.state.responseError &&
          m(Icon, {
            icon: Icons.Warning,
            filled: true,
            style: {
              color: 'yellow',
              position: 'absolute',
              top: '10px',
              right: '10px',
              zIndex: 1,
              fontSize: '2rem',
            } as m.Attributes['style'],
            title:
              `NOT A VALID NODE.\nCan't generate proto based on provided query.\n\n` +
              `Response error: ${this.state.responseError.message}`,
          }),
        m(Editor, {
          text: this.text,
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
      m(QueryHistoryComponent, {
        className: '.pf-query-history-container',
        trace: this.state.trace,
        runQuery,
        setQuery: (q: string) => {
          this.text = q;
          m.redraw();
        },
      }),
    );
  }
}
