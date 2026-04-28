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
  NodeType,
  nextNodeId,
  SecondaryInputSpec,
  NodeContext,
} from '../../../query_node';
import {notifyNextNodes} from '../../graph_utils';
import protos from '../../../../../protos';
import {Editor} from '../../../../../widgets/editor';
import {
  StructuredQueryBuilder,
  SqlDependency,
} from '../../structured_query_builder';
import {ColumnInfo} from '../../column_info';
import {setValidationError} from '../../node_issues';
import {NodeDetailsAttrs} from '../../../node_types';
import {loadNodeDoc} from '../../node_doc_loader';
import {NodeTitle} from '../../node_styling_widgets';

// Serializable node configuration.
export interface SqlSourceNodeAttrs {
  sql?: string;
}

interface SqlEditorAttrs {
  sql: string;
  onUpdate: (text: string) => void;
  onExecute: (text: string) => void;
}

class SqlEditor implements m.ClassComponent<SqlEditorAttrs> {
  view({attrs}: m.CVnode<SqlEditorAttrs>) {
    return m(
      '.pf-sql-editor-container',
      {
        onkeydown: (e: KeyboardEvent) => {
          // When ESC is pressed, blur the editor and focus the canvas
          // so that delete key can work on the graph
          if (e.key === 'Escape') {
            const target = e.target as HTMLElement;
            target.blur();

            // Find the graph canvas (it's a div, not a canvas element) and focus it
            const canvas = document.querySelector('.pf-canvas') as HTMLElement;
            if (canvas !== null) {
              canvas.focus();
            }
          }
          // Stop propagation for all keyboard events to prevent them from
          // reaching the graph (e.g., Delete/Backspace would delete the node)
          e.stopPropagation();
        },
      },
      m(Editor, {
        fillHeight: true,
        text: attrs.sql,
        onUpdate: attrs.onUpdate,
        onExecute: attrs.onExecute,
        autofocus: true,
        language: 'perfetto-sql',
      }),
    );
  }
}

/**
 * Removes comments and string literals from SQL to allow safe keyword detection.
 */
function stripCommentsAndStrings(sql: string): string {
  let result = sql;
  // Remove single-line comments (-- ...)
  result = result.replace(/--[^\n]*$/gm, '');
  // Remove multi-line comments (/* ... */)
  result = result.replace(/\/\*[\s\S]*?\*\//g, '');
  // Remove string literals ('...' and "...")
  result = result.replace(/'(?:[^'\\]|\\.)*'/g, '');
  result = result.replace(/"(?:[^"\\]|\\.)*"/g, '');
  return result;
}

/**
 * Validates SQL statement structure. Returns an error message if invalid,
 * or undefined if valid.
 *
 * Valid structure: zero or more INCLUDE PERFETTO MODULE statements,
 * followed by exactly one SELECT statement.
 */
function validateStatementStructure(sql: string): string | undefined {
  const cleaned = stripCommentsAndStrings(sql);

  // Split by semicolons and filter out empty statements
  const statements = cleaned
    .split(';')
    .map((s) => s.trim())
    .filter((s) => s.length > 0);

  if (statements.length === 0) {
    return 'SQL query is empty';
  }

  // Check that all statements except the last are INCLUDE PERFETTO MODULE
  for (let i = 0; i < statements.length - 1; i++) {
    if (!/^INCLUDE\s+PERFETTO\s+MODULE\b/i.test(statements[i])) {
      return 'Only INCLUDE PERFETTO MODULE statements are allowed before the SELECT query.';
    }
  }

  // Check that the last statement starts with SELECT (or WITH for CTEs)
  const lastStatement = statements[statements.length - 1];
  if (!/^(?:SELECT|WITH)\b/i.test(lastStatement)) {
    return 'The query must end with a SELECT statement.';
  }

  return undefined;
}

export class SqlSourceNode implements QueryNode {
  readonly nodeId: string;
  readonly attrs: SqlSourceNodeAttrs;
  readonly context: NodeContext;
  finalCols: ColumnInfo[];
  nextNodes: QueryNode[];
  secondaryInputs: SecondaryInputSpec;

  constructor(attrs: SqlSourceNodeAttrs, context: NodeContext) {
    this.nodeId = nextNodeId();
    this.attrs = attrs;
    this.context = {
      ...context,
      // SQL source nodes require manual execution since users write SQL
      autoExecute: context.autoExecute ?? false,
    };
    this.finalCols = [];
    this.nextNodes = [];
    // Support unbounded number of input nodes that can be referenced as $input_0, $input_1, etc.
    this.secondaryInputs = {
      connections: new Map(),
      min: 0,
      max: 'unbounded',
      portNames: (portIndex: number) => `$input_${portIndex}`,
    };
  }

  get type() {
    return NodeType.kSqlSource;
  }

  setSourceColumns(columns: string[]) {
    this.finalCols = columns.map((c) => ({name: c, checked: true}));
    m.redraw();
  }

  onQueryExecuted(columns: string[]) {
    this.setSourceColumns(columns);
    // Notify downstream nodes that our columns have changed, but don't mark
    // this node as having an operation change (which would cause hash to change
    // and trigger re-execution). Column discovery is metadata, not a query change.
    notifyNextNodes(this);
  }

  /**
   * Returns the list of connected input nodes sorted by port index.
   */
  get inputNodesList(): QueryNode[] {
    return [...this.secondaryInputs.connections.entries()]
      .sort(([a], [b]) => a - b)
      .map(([, node]) => node);
  }

  clone(): QueryNode {
    return new SqlSourceNode({sql: this.attrs.sql}, this.context);
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.context.issues) {
      this.context.issues.clear();
    }

    if (this.attrs.sql === undefined || this.attrs.sql.trim() === '') {
      setValidationError(this.context, 'SQL query is empty');
      return false;
    }

    const structureError = validateStatementStructure(this.attrs.sql);
    if (structureError !== undefined) {
      setValidationError(this.context, structureError);
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

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    // Build dependencies from connected input nodes
    // Each input can be referenced in SQL as $input_0, $input_1, etc.
    const dependencies: SqlDependency[] = [];

    for (const [portIndex, inputNode] of this.secondaryInputs.connections) {
      const inputQuery = inputNode.getStructuredQuery();
      if (inputQuery === undefined) {
        // If any input is invalid, the query cannot be built
        return undefined;
      }
      dependencies.push({
        alias: `input_${portIndex}`,
        query: inputQuery,
      });
    }

    // Use columns from the last successful execution. These are populated
    // by onQueryExecuted() and are cleared when SQL changes (to prevent
    // stale columns from being used with a different query).
    const columnNames: string[] = this.finalCols.map((c) => c.name);

    const sq = StructuredQueryBuilder.fromSql(
      this.attrs.sql || '',
      dependencies,
      columnNames,
      this.nodeId,
    );

    StructuredQueryBuilder.applyNodeColumnSelection(sq, this);
    return sq;
  }

  nodeSpecificModify(): m.Child {
    return m(SqlEditor, {
      sql: this.attrs.sql ?? '',
      onUpdate: (text: string) => {
        if (this.attrs.sql === text) {
          return;
        }
        this.attrs.sql = text;
        // Clear columns when SQL changes to prevent stale column usage
        this.finalCols = [];
        // Notify that the query has changed so stale results are cleared
        this.context.onchange?.();
        m.redraw();
      },
      onExecute: (text: string) => {
        this.attrs.sql = text.trim();
        // Clear columns when SQL changes to prevent stale column usage
        this.finalCols = [];
        // Notify that the query has changed so stale results are cleared
        this.context.onchange?.();
        m.redraw();
      },
    });
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('sql_source');
  }

  findDependencies(): string[] {
    const regex = /\$([A-Za-z0-9_]*)/g;
    let match: RegExpExecArray | null;
    const dependencies: string[] = [];
    if (this.attrs.sql) {
      while ((match = regex.exec(this.attrs.sql)) !== null) {
        dependencies.push(match[1]);
      }
    }
    return dependencies;
  }
}
