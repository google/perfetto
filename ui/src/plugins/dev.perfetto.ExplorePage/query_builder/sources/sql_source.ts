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
  ColumnControllerRow,
  columnControllerRowFromName,
  newColumnControllerRows,
} from '../column_controller';
import protos from '../../../../protos';
import {TextInput} from '../../../../widgets/text_input';
import {
  createFiltersProto,
  createGroupByProto,
  Operator,
} from '../operations/operation_component';

export interface SqlSourceAttrs extends QueryNodeState {
  sql?: string;
  sqlColumns?: string[];
  preamble?: string;
}

export class SqlSourceNode implements QueryNode {
  readonly type: NodeType = NodeType.kSqlSource;
  readonly prevNode = undefined;
  nextNode?: QueryNode;

  readonly sourceCols: ColumnControllerRow[];
  readonly finalCols: ColumnControllerRow[];

  readonly state: SqlSourceAttrs;

  constructor(attrs: SqlSourceAttrs) {
    this.state = attrs;
    this.sourceCols =
      attrs.sqlColumns?.map((c) => columnControllerRowFromName(c)) ?? [];
    this.finalCols = createFinalColumns(this);
  }

  getStateCopy(): QueryNodeState {
    const newState: SqlSourceAttrs = {
      sql: this.state.sql,
      sqlColumns: this.state.sqlColumns,
      preamble: this.state.preamble,
      sourceCols: newColumnControllerRows(this.sourceCols),
      groupByColumns: newColumnControllerRows(this.state.groupByColumns),
      filters: this.state.filters.map((f) => ({...f})),
      aggregations: this.state.aggregations.map((a) => ({...a})),
    };
    return newState;
  }

  validate(): boolean {
    return (
      this.state.sql !== undefined &&
      this.state.sqlColumns !== undefined &&
      this.state.preamble !== undefined &&
      this.sourceCols.length > 0
    );
  }

  getTitle(): string {
    return `Sql source`;
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = `sql_source`;
    const sqlProto = new protos.PerfettoSqlStructuredQuery.Sql();

    if (this.state.sql) sqlProto.sql = this.state.sql;
    if (this.state.sqlColumns) sqlProto.columnNames = this.state.sqlColumns;
    if (this.state.preamble) sqlProto.preamble = this.state.preamble;
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

  getDetails(): m.Child {
    return m(
      '',
      m(
        '',
        'Preamble',
        m(TextInput, {
          id: 'preamble',
          type: 'string',
          oninput: (e: Event) => {
            if (!e.target) return;
            this.state.preamble = (e.target as HTMLInputElement).value.trim();
          },
        }),
      ),
      m(
        '',
        'Sql ',
        m(TextInput, {
          id: 'sql_source',
          type: 'string',
          oninput: (e: Event) => {
            if (!e.target) return;
            this.state.sql = (e.target as HTMLInputElement).value
              .trim()
              .split(';')[0];
          },
        }),
      ),
      m(
        '',
        'Column names (comma separated strings) ',
        m(TextInput, {
          id: 'columns',
          type: 'string',
          oninput: (e: Event) => {
            if (!e.target) return;
            this.state.sqlColumns = (e.target as HTMLInputElement).value
              .split(',')
              .map((col) => col.trim())
              .filter(Boolean);
            this.state.sourceCols = this.state.sqlColumns.map((c) =>
              columnControllerRowFromName(c, true),
            );
            this.state.groupByColumns = newColumnControllerRows(
              this.state.sourceCols,
              false,
            );
          },
        }),
      ),
    );
  }
}

export class SqlSource implements m.ClassComponent<SqlSourceAttrs> {
  view({attrs}: m.CVnode<SqlSourceAttrs>) {
    return m(
      '',
      m(
        '',
        'Preamble',
        m(TextInput, {
          id: 'preamble',
          type: 'string',
          oninput: (e: Event) => {
            if (!e.target) return;
            attrs.preamble = (e.target as HTMLInputElement).value.trim();
          },
        }),
      ),
      m(
        '',
        'Sql ',
        m(TextInput, {
          id: 'sql_source',
          type: 'string',
          oninput: (e: Event) => {
            if (!e.target) return;
            attrs.sql = (e.target as HTMLInputElement).value
              .trim()
              .split(';')[0];
          },
        }),
      ),
      m(
        '',
        'Column names (comma separated strings) ',
        m(TextInput, {
          id: 'columns',
          type: 'string',
          oninput: (e: Event) => {
            if (!e.target) return;
            attrs.sqlColumns = (e.target as HTMLInputElement).value
              .split(',')
              .map((col) => col.trim())
              .filter(Boolean);
            attrs.sourceCols = attrs.sqlColumns.map((c) =>
              columnControllerRowFromName(c, true),
            );
            attrs.groupByColumns = newColumnControllerRows(
              attrs.sourceCols,
              false,
            );
          },
        }),
      ),
      m(Operator, {
        filter: {sourceCols: attrs.sourceCols, filters: attrs.filters},
        groupby: {
          groupByColumns: attrs.groupByColumns,
          aggregations: attrs.aggregations,
        },
      }),
    );
  }
}
