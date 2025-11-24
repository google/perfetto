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
  createFinalColumns,
  MultiSourceNode,
  nextNodeId,
  notifyNextNodes,
} from '../../../query_node';
import {columnInfoFromName} from '../../column_info';
import protos from '../../../../../protos';
import {Editor} from '../../../../../widgets/editor';
import {StructuredQueryBuilder} from '../../structured_query_builder';

import {
  QueryHistoryComponent,
  queryHistoryStorage,
} from '../../../../../components/widgets/query_history';
import {Trace} from '../../../../../public/trace';

import {ColumnInfo} from '../../column_info';
import {setValidationError} from '../../node_issues';

export interface SqlSourceSerializedState {
  sql?: string;
  comment?: string;
}

export interface SqlSourceState extends QueryNodeState {
  sql?: string;
  trace: Trace;
}

export class SqlSourceNode implements MultiSourceNode {
  readonly nodeId: string;
  readonly state: SqlSourceState;
  prevNodes: QueryNode[] = [];
  finalCols: ColumnInfo[];
  nextNodes: QueryNode[];

  constructor(attrs: SqlSourceState) {
    this.nodeId = nextNodeId();
    this.state = {
      ...attrs,
      // SQL source nodes require manual execution since users write SQL
      autoExecute: attrs.autoExecute ?? false,
    };
    this.finalCols = createFinalColumns([]);
    this.nextNodes = [];
    this.prevNodes = attrs.prevNodes ?? [];
  }

  get type() {
    return NodeType.kSqlSource;
  }

  setSourceColumns(columns: string[]) {
    this.finalCols = createFinalColumns(
      columns.map((c) => columnInfoFromName(c)),
    );
    m.redraw();
  }

  onQueryExecuted(columns: string[]) {
    this.setSourceColumns(columns);
    // Notify downstream nodes that our columns have changed, but don't mark
    // this node as having an operation change (which would cause hash to change
    // and trigger re-execution). Column discovery is metadata, not a query change.
    notifyNextNodes(this);
  }

  clone(): QueryNode {
    const stateCopy: SqlSourceState = {
      sql: this.state.sql,
      issues: this.state.issues,
      trace: this.state.trace,
    };
    return new SqlSourceNode(stateCopy);
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.state.sql === undefined || this.state.sql.trim() === '') {
      setValidationError(this.state, 'SQL query is empty');
      return false;
    }

    return true;
  }

  getTitle(): string {
    return 'Sql source';
  }

  serializeState(): SqlSourceSerializedState {
    return {
      sql: this.state.sql,
      comment: this.state.comment,
    };
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    const dependencies = this.prevNodes.map((prevNode) => ({
      alias: prevNode.nodeId,
      query: prevNode.getStructuredQuery(),
    }));

    // Pass empty array for column names - the engine will discover them when analyzing the query
    // Using this.finalCols here would pass stale columns from the previous execution
    const columnNames: string[] = [];

    const sq = StructuredQueryBuilder.fromSql(
      this.state.sql || '',
      dependencies,
      columnNames,
      this.nodeId,
    );

    const selectedColumns = createSelectColumnsProto(this);
    if (selectedColumns) sq.selectColumns = selectedColumns;
    return sq;
  }

  nodeSpecificModify(): m.Child {
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
            // Note: Execution is now handled by the Run button in DataExplorer
            // This callback only saves to query history and updates the SQL text
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

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Write custom queries to access any data in the trace. Use ',
        m('code', '$node_id'),
        ' to reference other nodes in your query.',
      ),
      m(
        'p',
        'Most flexible option for complex logic or operations not available through other nodes.',
      ),
      m(
        'p',
        m('strong', 'Example:'),
        ' Write ',
        m('code', 'SELECT * FROM slice WHERE dur > 1000'),
        ' or reference another node with ',
        m('code', 'SELECT * FROM $other_node WHERE ...'),
      ),
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
