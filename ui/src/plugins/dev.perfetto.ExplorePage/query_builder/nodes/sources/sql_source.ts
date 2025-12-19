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
  QueryNode,
  QueryNodeState,
  NodeType,
  nextNodeId,
} from '../../../query_node';
import {notifyNextNodes} from '../../graph_utils';
import {columnInfoFromName, newColumnInfoList} from '../../column_info';
import protos from '../../../../../protos';
import {Editor} from '../../../../../widgets/editor';
import {StructuredQueryBuilder} from '../../structured_query_builder';
import {Trace} from '../../../../../public/trace';

import {ColumnInfo} from '../../column_info';
import {setValidationError} from '../../node_issues';
import {NodeDetailsAttrs} from '../../node_explorer_types';
import {findRef, toHTMLElement} from '../../../../../base/dom_utils';
import {assertExists} from '../../../../../base/logging';
import {ResizeHandle} from '../../../../../widgets/resize_handle';
import {loadNodeDoc} from '../../node_doc_loader';
import {NodeTitle} from '../../node_styling_widgets';

export interface SqlSourceSerializedState {
  sql?: string;
  comment?: string;
}

export interface SqlSourceState extends QueryNodeState {
  sql?: string;
  trace: Trace;
}

interface SqlEditorAttrs {
  sql: string;
  onUpdate: (text: string) => void;
  onExecute: (text: string) => void;
}

class SqlEditor implements m.ClassComponent<SqlEditorAttrs> {
  private editorHeight: number = 0;
  private editorElement?: HTMLElement;

  oncreate({dom}: m.VnodeDOM<SqlEditorAttrs>) {
    this.editorElement = toHTMLElement(assertExists(findRef(dom, 'editor')));
    this.editorElement.style.height = '400px';
  }

  view({attrs}: m.CVnode<SqlEditorAttrs>) {
    return [
      m(Editor, {
        ref: 'editor',
        text: attrs.sql,
        onUpdate: attrs.onUpdate,
        onExecute: attrs.onExecute,
        autofocus: true,
      }),
      m(ResizeHandle, {
        onResize: (deltaPx: number) => {
          this.editorHeight += deltaPx;
          this.editorElement!.style.height = `${this.editorHeight}px`;
        },
        onResizeStart: () => {
          this.editorHeight = this.editorElement!.clientHeight;
        },
      }),
    ];
  }
}

export class SqlSourceNode implements QueryNode {
  readonly nodeId: string;
  readonly state: SqlSourceState;
  finalCols: ColumnInfo[];
  nextNodes: QueryNode[];

  constructor(attrs: SqlSourceState) {
    this.nodeId = nextNodeId();
    this.state = {
      ...attrs,
      // SQL source nodes require manual execution since users write SQL
      autoExecute: attrs.autoExecute ?? false,
    };
    this.finalCols = [];
    this.nextNodes = [];
  }

  get type() {
    return NodeType.kSqlSource;
  }

  setSourceColumns(columns: string[]) {
    this.finalCols = newColumnInfoList(
      columns.map((c) => columnInfoFromName(c)),
      true,
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

  nodeDetails(): NodeDetailsAttrs {
    return {
      content: NodeTitle(this.getTitle()),
    };
  }

  serializeState(): SqlSourceSerializedState {
    return {
      sql: this.state.sql,
    };
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    // Source nodes don't have dependencies
    const dependencies: Array<{
      alias: string;
      query: protos.PerfettoSqlStructuredQuery | undefined;
    }> = [];

    // Use columns from the last successful execution. These are populated
    // by onQueryExecuted() and are cleared when SQL changes (to prevent
    // stale columns from being used with a different query).
    const columnNames: string[] = this.finalCols.map((c) => c.column.name);

    const sq = StructuredQueryBuilder.fromSql(
      this.state.sql || '',
      dependencies,
      columnNames,
      this.nodeId,
    );

    StructuredQueryBuilder.applyNodeColumnSelection(sq, this);
    return sq;
  }

  nodeSpecificModify(): m.Child {
    return m(
      '.sql-source-node',
      m(SqlEditor, {
        sql: this.state.sql ?? '',
        onUpdate: (text: string) => {
          this.state.sql = text;
          // Clear columns when SQL changes to prevent stale column usage
          this.finalCols = [];
          m.redraw();
        },
        onExecute: (text: string) => {
          this.state.sql = text.trim();
          // Clear columns when SQL changes to prevent stale column usage
          this.finalCols = [];
          m.redraw();
        },
      }),
    );
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('sql_source');
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
