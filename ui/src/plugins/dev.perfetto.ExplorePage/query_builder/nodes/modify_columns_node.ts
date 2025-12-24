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
  nextNodeId,
  NodeType,
} from '../../query_node';
import {Checkbox} from '../../../../widgets/checkbox';
import {TextInput} from '../../../../widgets/text_input';
import {ColumnInfo, newColumnInfoList} from '../column_info';
import {SIMPLE_TYPE_KINDS} from '../../../../trace_processor/perfetto_sql_type';
import protos from '../../../../protos';
import {NodeIssues} from '../node_issues';
import {StructuredQueryBuilder, ColumnSpec} from '../structured_query_builder';
import {DraggableItem, SelectDeselectAllButtons} from '../widgets';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../node_explorer_types';
import {
  NodeDetailsMessage,
  NodeDetailsSpacer,
  ColumnName,
} from '../node_styling_widgets';
import {loadNodeDoc} from '../node_doc_loader';
import {renderTypeSelector} from './modify_columns_utils';

export interface ModifyColumnsSerializedState {
  primaryInputId?: string;
  selectedColumns: {
    name: string;
    type: string;
    checked: boolean;
    alias?: string;
  }[];
  comment?: string;
}

export interface ModifyColumnsState extends QueryNodeState {
  selectedColumns: ColumnInfo[];
}

export class ModifyColumnsNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kModifyColumns;
  primaryInput?: QueryNode;
  nextNodes: QueryNode[];
  readonly state: ModifyColumnsState;

  constructor(state: ModifyColumnsState) {
    this.nodeId = nextNodeId();
    this.nextNodes = [];

    this.state = {
      ...state,
      selectedColumns: state.selectedColumns ?? [],
    };
  }

  get finalCols(): ColumnInfo[] {
    return this.computeFinalCols();
  }

  private computeFinalCols(): ColumnInfo[] {
    const finalCols = newColumnInfoList(
      this.state.selectedColumns.filter((col) => col.checked),
    );
    return finalCols;
  }

  onPrevNodesUpdated() {
    // This node assumes it has only one previous node.
    if (this.primaryInput === undefined) {
      return;
    }

    const sourceCols = this.primaryInput.finalCols;

    const newSelectedColumns = newColumnInfoList(sourceCols);

    // Preserve checked status and aliases for columns that still exist.
    for (const oldCol of this.state.selectedColumns) {
      const newCol = newSelectedColumns.find(
        (c) => c.column.name === oldCol.column.name,
      );
      if (newCol) {
        newCol.checked = oldCol.checked;
        newCol.alias = oldCol.alias;
      }
    }

    this.state.selectedColumns = newSelectedColumns;

    // Trigger downstream update (handled by builder's onchange callback)
    this.state.onchange?.();
  }

  static deserializeState(
    serializedState: ModifyColumnsSerializedState,
  ): ModifyColumnsState {
    return {
      ...serializedState,
      selectedColumns: serializedState.selectedColumns.map((c) => ({
        name: c.name,
        type: c.type,
        checked: c.checked,
        column: {name: c.name},
        alias: c.alias,
      })),
    };
  }

  resolveColumns() {
    // Recover full column information from primaryInput
    if (this.primaryInput === undefined) {
      return;
    }

    const sourceCols = this.primaryInput.finalCols ?? [];
    this.state.selectedColumns.forEach((c) => {
      const sourceCol = sourceCols.find((s) => s.name === c.name);
      if (sourceCol) {
        c.column = sourceCol.column;
        c.type = sourceCol.type;
      }
    });
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    // Check if primary input exists and is valid
    if (this.primaryInput === undefined) {
      this.setValidationError('No input node connected');
      return false;
    }

    if (!this.primaryInput.validate()) {
      return false;
    }

    const colNames = new Set<string>();
    for (const col of this.state.selectedColumns) {
      if (!col.checked) continue;
      // Empty aliases are allowed - they just mean use the original column name
      const name = col.alias ? col.alias.trim() : col.column.name;
      if (colNames.has(name)) {
        this.setValidationError('Duplicate column names');
        return false;
      }
      colNames.add(name);
    }

    // Check if there are no columns selected
    if (colNames.size === 0) {
      this.setValidationError(
        'No columns selected. Select at least one column.',
      );
      return false;
    }

    return true;
  }

  private setValidationError(message: string): void {
    if (!this.state.issues) {
      this.state.issues = new NodeIssues();
    }
    this.state.issues.queryError = new Error(message);
  }

  getTitle(): string {
    return 'Modify Columns';
  }

  nodeDetails(): NodeDetailsAttrs {
    const selectedCols = this.state.selectedColumns.filter((c) => c.checked);
    const totalCols = this.state.selectedColumns.length;

    // If all columns have been deselected, show a specific message.
    if (selectedCols.length === 0) {
      return {
        content: NodeDetailsMessage('All columns deselected'),
      };
    }

    // Determine the state of modifications.
    const hasUnselected = this.state.selectedColumns.some((c) => !c.checked);
    const hasAlias = this.state.selectedColumns.some((c) => c.alias);
    if (!hasUnselected && !hasAlias) {
      return {
        content: NodeDetailsMessage('Select all'),
      };
    }

    // If there are too many selected columns, show a summary.
    const maxColumnsToShow = 5;
    if (selectedCols.length > maxColumnsToShow) {
      const renamedCols = selectedCols.filter((c) => c.alias);
      const allSelected = selectedCols.length === totalCols;

      // Show up to 3 renamed columns explicitly even in summary mode.
      if (renamedCols.length > 0 && renamedCols.length <= 3) {
        const renamedItems = renamedCols.map((c) =>
          m('div', ColumnName(c.column.name), ' AS ', ColumnName(c.alias!)),
        );
        // Only show the count if not all columns are selected
        if (allSelected) {
          return {
            content: m('div', ...renamedItems),
          };
        }
        const summaryText = `${selectedCols.length} of ${totalCols} columns selected`;
        return {
          content: m(
            'div',
            m('div', summaryText),
            NodeDetailsSpacer(),
            ...renamedItems,
          ),
        };
      } else {
        // If all columns are selected, don't show the redundant "X of X" message
        if (allSelected) {
          return {
            content: NodeDetailsMessage('Select all'),
          };
        }
        const summaryText = `${selectedCols.length} of ${totalCols} columns selected`;
        return {
          content: m('div', summaryText),
        };
      }
    }

    // Otherwise, list all selected columns.
    const selectedItems = selectedCols.map((c) => {
      if (c.alias) {
        return m('div', ColumnName(c.column.name), ' AS ', ColumnName(c.alias));
      } else {
        return m('div', ColumnName(c.column.name));
      }
    });
    return {
      content: m('div', ...selectedItems),
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    const selectedCount = this.state.selectedColumns.filter(
      (col) => col.checked,
    ).length;
    const totalCount = this.state.selectedColumns.length;

    // Build sections
    const sections: NodeModifyAttrs['sections'] = [
      {
        title: `Select and Rename Columns (${selectedCount} / ${totalCount} selected)`,
        content: m(
          '.pf-modify-columns-content',
          m(SelectDeselectAllButtons, {
            onSelectAll: () => {
              this.state.selectedColumns = this.state.selectedColumns.map(
                (col) => ({
                  ...col,
                  checked: true,
                }),
              );
              this.state.onchange?.();
            },
            onDeselectAll: () => {
              this.state.selectedColumns = this.state.selectedColumns.map(
                (col) => ({
                  ...col,
                  checked: false,
                }),
              );
              this.state.onchange?.();
            },
          }),
          this.renderColumnList(),
        ),
      },
    ];

    return {
      info: 'Select which columns to include in the output and optionally rename them using aliases. Check columns to include, add aliases to rename, and drag to reorder.',
      sections,
    };
  }

  private renderColumnList(): m.Child {
    return m(
      '.pf-modify-columns-node',
      m(
        '.pf-column-list-container',
        m(
          '.pf-column-list-help',
          'Check columns to include, add aliases to rename, and drag to reorder',
        ),
        m(
          '.pf-column-list',
          this.state.selectedColumns.map((col, index) =>
            this.renderSelectedColumn(col, index),
          ),
        ),
      ),
    );
  }

  private renderSelectedColumn(col: ColumnInfo, index: number): m.Child {
    const handleReorder = (from: number, to: number) => {
      const newSelectedColumns = [...this.state.selectedColumns];
      const [removed] = newSelectedColumns.splice(from, 1);
      newSelectedColumns.splice(to, 0, removed);
      this.state.selectedColumns = newSelectedColumns;
      this.state.onchange?.();
    };

    const handleTypeChange = (index: number, newType: string) => {
      const newSelectedColumns = [...this.state.selectedColumns];
      const lowerType = newType.toLowerCase();

      // Check if it's a simple type
      const isSimple = SIMPLE_TYPE_KINDS.includes(
        lowerType as (typeof SIMPLE_TYPE_KINDS)[number],
      );

      const originalType = col.column.type;
      newSelectedColumns[index] = {
        ...newSelectedColumns[index],
        type: newType,
        column: {
          ...newSelectedColumns[index].column,
          type: isSimple
            ? {kind: lowerType as (typeof SIMPLE_TYPE_KINDS)[number]}
            : originalType, // Keep original if it's an ID type
        },
      };
      this.state.selectedColumns = newSelectedColumns;
      this.state.onchange?.();
    };

    return m(
      DraggableItem,
      {
        index,
        onReorder: handleReorder,
      },
      m(Checkbox, {
        checked: col.checked,
        label: col.column.name,
        onchange: (e) => {
          const newSelectedColumns = [...this.state.selectedColumns];
          newSelectedColumns[index] = {
            ...newSelectedColumns[index],
            checked: (e.target as HTMLInputElement).checked,
          };
          this.state.selectedColumns = newSelectedColumns;
          this.state.onchange?.();
        },
      }),
      m(TextInput, {
        oninput: (e: Event) => {
          const newSelectedColumns = [...this.state.selectedColumns];
          const inputValue = (e.target as HTMLInputElement).value;
          newSelectedColumns[index] = {
            ...newSelectedColumns[index],
            // Normalize empty strings to undefined (no alias)
            alias: inputValue.trim() === '' ? undefined : inputValue,
          };
          this.state.selectedColumns = newSelectedColumns;
          this.state.onchange?.();
        },
        placeholder: 'alias',
        value: col.alias ? col.alias : '',
      }),
      renderTypeSelector(col, index, handleTypeChange),
    );
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('modify_columns');
  }

  clone(): QueryNode {
    const stateCopy: ModifyColumnsState = {
      selectedColumns: newColumnInfoList(this.state.selectedColumns),
      filters: this.state.filters?.map((f) => ({...f})),
      filterOperator: this.state.filterOperator,
      onchange: this.state.onchange,
    };
    return new ModifyColumnsNode(stateCopy);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.primaryInput === undefined) return undefined;

    // Build column specifications
    const columns: ColumnSpec[] = [];

    for (const col of this.state.selectedColumns) {
      if (!col.checked) continue;
      columns.push({
        columnNameOrExpression: col.column.name,
        alias: col.alias,
      });
    }

    // Apply column selection
    return StructuredQueryBuilder.withSelectColumns(
      this.primaryInput,
      columns,
      undefined,
      this.nodeId,
    );
  }

  serializeState(): ModifyColumnsSerializedState {
    return {
      primaryInputId: this.primaryInput?.nodeId,
      selectedColumns: this.state.selectedColumns.map((c) => ({
        name: c.name,
        type: c.type,
        checked: c.checked,
        alias: c.alias,
      })),
    };
  }
}
