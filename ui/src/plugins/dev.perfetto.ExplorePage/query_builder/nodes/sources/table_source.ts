// Copyright (C) 2025 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';
import {
  SqlTable,
  SqlModules,
} from '../../../../dev.perfetto.SqlModules/sql_modules';
import {
  QueryNode,
  QueryNodeState,
  NodeType,
  nextNodeId,
} from '../../../query_node';
import {StructuredQueryBuilder} from '../../structured_query_builder';
import {
  ColumnInfo,
  columnInfoFromSqlColumn,
  newColumnInfoList,
} from '../../column_info';
import protos from '../../../../../protos';
import {Trace} from '../../../../../public/trace';
import {closeModal, showModal} from '../../../../../widgets/modal';
import {TableList} from '../../table_list';
import {redrawModal} from '../../../../../widgets/modal';
import {setValidationError} from '../../node_issues';
import {TableDescription} from '../../widgets';
import {NodeDetailsAttrs} from '../../node_explorer_types';
import {loadNodeDoc} from '../../node_doc_loader';
import {NodeTitle} from '../../node_styling_widgets';

export interface TableSourceSerializedState {
  sqlTable?: string;
  comment?: string;
}

export interface TableSourceState extends QueryNodeState {
  readonly trace: Trace;
  readonly sqlModules: SqlModules;

  sqlTable?: SqlTable;
  onchange?: () => void;
}

interface TableSelectionResult {
  sqlTable: SqlTable;
  sourceCols: ColumnInfo[];
}

export function modalForTableSelection(
  sqlModules: SqlModules,
): Promise<TableSelectionResult[] | undefined> {
  return new Promise((resolve) => {
    let searchQuery = '';
    const selectedTables = new Set<string>();

    const updateModal = () => {
      showModal({
        key: 'table-selection-modal',
        title:
          selectedTables.size > 0
            ? `Choose tables - ${selectedTables.size} selected`
            : 'Choose a table - Ctrl+click for multiple selection',
        content: () => {
          return m(
            '.pf-exp-node-explorer-help',
            m(TableList, {
              sqlModules,
              onTableClick: handleTableClick,
              searchQuery,
              onSearchQueryChange: (query) => {
                searchQuery = query;
                redrawModal();
              },
              autofocus: true,
              selectedTables,
            }),
          );
        },
        buttons:
          selectedTables.size > 0
            ? [
                {
                  text: `Add ${selectedTables.size} table${selectedTables.size > 1 ? 's' : ''}`,
                  primary: true,
                  action: handleConfirm,
                },
              ]
            : [],
      });
    };

    const handleTableClick = (tableName: string, event: MouseEvent) => {
      if (event.ctrlKey || event.metaKey) {
        // Multi-select mode: toggle selection
        if (selectedTables.has(tableName)) {
          selectedTables.delete(tableName);
        } else {
          selectedTables.add(tableName);
        }
        updateModal();
      } else {
        // Single-select mode: immediately select and close
        const sqlTable = sqlModules.getTable(tableName);
        if (!sqlTable) {
          resolve(undefined);
          return;
        }
        const sourceCols = sqlTable.columns.map((c) =>
          columnInfoFromSqlColumn(c, true),
        );
        resolve([{sqlTable, sourceCols}]);
        closeModal();
      }
    };

    const handleConfirm = () => {
      if (selectedTables.size === 0) {
        resolve(undefined);
        closeModal();
        return;
      }

      const results: TableSelectionResult[] = [];
      for (const tableName of selectedTables) {
        const sqlTable = sqlModules.getTable(tableName);
        if (sqlTable) {
          const sourceCols = sqlTable.columns.map((c) =>
            columnInfoFromSqlColumn(c, true),
          );
          results.push({sqlTable, sourceCols});
        }
      }
      resolve(results);
      closeModal();
    };

    updateModal();
  });
}

export class TableSourceNode implements QueryNode {
  readonly nodeId: string;
  readonly state: TableSourceState;
  readonly finalCols: ColumnInfo[];
  nextNodes: QueryNode[];

  constructor(attrs: TableSourceState) {
    this.nodeId = nextNodeId();
    this.state = attrs;
    this.state.onchange = attrs.onchange;
    this.finalCols = newColumnInfoList(
      this.state.sqlTable?.columns.map((c) =>
        columnInfoFromSqlColumn(c, true),
      ) ?? [],
      true,
    );
    this.nextNodes = [];
  }

  get type() {
    return NodeType.kTable;
  }

  clone(): QueryNode {
    const stateCopy: TableSourceState = {
      trace: this.state.trace,
      sqlModules: this.state.sqlModules,
      sqlTable: this.state.sqlTable,
      onchange: this.state.onchange,
    };
    return new TableSourceNode(stateCopy);
  }

  nodeSpecificModify(): m.Child {
    return undefined;
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.state.sqlTable === undefined) {
      setValidationError(this.state, 'No table selected');
      return false;
    }

    return true;
  }

  getTitle(): string {
    return `${this.state.sqlTable?.name}`;
  }

  nodeDetails(): NodeDetailsAttrs {
    return {
      content: NodeTitle(this.state.sqlTable?.name ?? ''),
    };
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return;
    if (!this.state.sqlTable) return;

    const columnNames = this.finalCols
      .filter((c) => c.checked)
      .map((c) => c.column.name);

    const sq = StructuredQueryBuilder.fromTable(
      this.state.sqlTable.name,
      this.state.sqlTable.includeKey || undefined,
      columnNames,
      this.nodeId,
    );

    StructuredQueryBuilder.applyNodeColumnSelection(sq, this);
    return sq;
  }

  serializeState(): TableSourceSerializedState {
    return {
      sqlTable: this.state.sqlTable?.name,
    };
  }

  nodeInfo(): m.Children {
    // Show general documentation
    const docContent = loadNodeDoc('table_source');

    // If a table is selected, also show table-specific information
    if (this.state.sqlTable != null) {
      return m(
        'div',
        docContent,
        m(
          '.pf-table-source-selected',
          m('h2', 'Selected Table'),
          m(
            '.pf-details-box',
            m(TableDescription, {table: this.state.sqlTable}),
          ),
        ),
      );
    }

    return docContent;
  }

  static deserializeState(
    trace: Trace,
    sqlModules: SqlModules,
    state: TableSourceSerializedState,
  ): TableSourceState {
    const sqlTable = state.sqlTable
      ? sqlModules.getTable(state.sqlTable)
      : undefined;
    return {
      ...state,
      trace,
      sqlModules,
      sqlTable,
    };
  }
}
