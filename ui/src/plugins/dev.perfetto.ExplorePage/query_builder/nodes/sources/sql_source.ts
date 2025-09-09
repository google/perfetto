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
  createSelectColumnsProto,
  QueryNode,
  QueryNodeState,
  NodeType,
} from '../../../query_node';
import {columnInfoFromName} from '../../column_info';
import protos from '../../../../../protos';
import {Editor} from '../../../../../widgets/editor';

import {
  QueryHistoryComponent,
  queryHistoryStorage,
} from '../../../../../components/widgets/query_history';
import {Trace} from '../../../../../public/trace';
import {SourceNode} from '../../source_node';

import {ColumnInfo} from '../../column_info';

export interface SqlSourceState extends QueryNodeState {
  sql?: string;
  trace: Trace;
  sourceCols?: ColumnInfo[];
}

export class SqlSourceNode extends SourceNode {
  readonly state: SqlSourceState;
  prevNodes: QueryNode[] = [];

  constructor(attrs: SqlSourceState) {
    super(attrs);
    this.state = attrs;
    this.state.sourceCols = [];
    this.nextNodes = [];
  }

  get type() {
    return NodeType.kSqlSource;
  }

  get sourceCols() {
    return this.state.sourceCols ?? [];
  }

  setSourceColumns(columns: string[]) {
    this.state.sourceCols = columns.map((c) => columnInfoFromName(c));
    this.finalCols = this.sourceCols;
    m.redraw();
  }

  onQueryExecuted(columns: string[]) {
    this.setSourceColumns(columns);
  }

  clone(): QueryNode {
    const stateCopy: SqlSourceState = {
      sql: this.state.sql,
      filters: [],
      customTitle: this.state.customTitle,
      issues: this.state.issues,
      trace: this.state.trace,
    };
    return new SqlSourceNode(stateCopy);
  }

  validate(): boolean {
    return this.state.sql !== undefined && this.state.sql.trim() !== '';
  }

  getTitle(): string {
    return this.state.customTitle ?? 'Sql source';
  }

  isMaterialised(): boolean {
    return this.state.isExecuted === true && this.meterialisedAs !== undefined;
  }
  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    const sq = new protos.PerfettoSqlStructuredQuery();
    sq.id = this.nodeId;
    const sqlProto = new protos.PerfettoSqlStructuredQuery.Sql();

    if (this.state.sql) sqlProto.sql = this.state.sql;
    sqlProto.columnNames = this.sourceCols.map((c) => c.column.name);

    for (const prevNode of this.prevNodes) {
      const dependency = new protos.PerfettoSqlStructuredQuery.Sql.Dependency();
      dependency.alias = prevNode.nodeId;
      dependency.query = prevNode.getStructuredQuery();
      sqlProto.dependencies.push(dependency);
    }

    sq.sql = sqlProto;

    const selectedColumns = createSelectColumnsProto(this);
    if (selectedColumns) sq.selectColumns = selectedColumns;
    return sq;
  }

  nodeSpecificModify(onExecute: () => void): m.Child {
    const runQuery = (sql: string) => {
      this.state.sql = sql.trim();
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
        m(Editor, {
          text: this.state.sql ?? '',
          onUpdate: (text: string) => {
            this.state.sql = text;
            m.redraw();
          },
          onExecute: (text: string) => {
            queryHistoryStorage.saveQuery(text);
            this.state.sql = text.trim();
            onExecute();
            m.redraw();
          },
          autofocus: true,
        }),
      ),
      m(QueryHistoryComponent, {
        className: '.pf-query-history-container',
        trace: this.state.trace,
        runQuery,
        setQuery: (q: string) => {
          this.state.sql = q;
          m.redraw();
        },
      }),
    );
  }

  findDependencies(): string[] {
    const regex = /\$([A-Za-z0-9_]*)/g;
    let match: RegExpExecArray | null;
    const dependencies: string[] = [];
    const node = this;
    if (node.state.sql) {
      while ((match = regex.exec(node.state.sql)) !== null) {
        dependencies.push(match[1]);
      }
    }
    return dependencies;
  }
}
