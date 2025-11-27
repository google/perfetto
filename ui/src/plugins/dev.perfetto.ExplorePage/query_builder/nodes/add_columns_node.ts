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

import {
  QueryNode,
  QueryNodeState,
  nextNodeId,
  NodeType,
  getSecondaryInput,
} from '../../query_node';
import {ColumnInfo, columnInfoFromName} from '../column_info';
import protos from '../../../../protos';
import m from 'mithril';
import {
  PopupMultiSelect,
  MultiSelectDiff,
} from '../../../../widgets/multiselect';
import {Select} from '../../../../widgets/select';
import {Button} from '../../../../widgets/button';
import {TextInput} from '../../../../widgets/text_input';
import {showModal, redrawModal} from '../../../../widgets/modal';
import {Switch} from '../../../../widgets/switch';
import {Icon} from '../../../../widgets/icon';
import {
  StructuredQueryBuilder,
  ColumnSpec,
  JoinCondition,
} from '../structured_query_builder';
import {setValidationError} from '../node_issues';
import {
  ListItem,
  ActionButtons,
  LabeledControl,
  TableDescription,
  IssueList,
} from '../widgets';
import {EmptyState} from '../../../../widgets/empty_state';
import {Callout} from '../../../../widgets/callout';
import {Form, FormLabel, FormSection} from '../../../../widgets/form';

// Helper components for computed columns (SWITCH and IF)
class SwitchComponent
  implements
    m.ClassComponent<{
      column: NewColumn;
      columns: ColumnInfo[];
      onchange: () => void;
    }>
{
  view({
    attrs,
  }: m.Vnode<{
    column: NewColumn;
    columns: ColumnInfo[];
    onchange: () => void;
  }>) {
    const {column, columns, onchange} = attrs;

    if (column.type !== 'switch') {
      return m('');
    }

    const setSwitchOn = (newSwitchOn: string) => {
      column.switchOn = newSwitchOn;
      this.updateExpression(column);
      onchange();
    };

    const setDefaultValue = (newDefaultValue: string) => {
      column.defaultValue = newDefaultValue;
      this.updateExpression(column);
      onchange();
    };

    const setCaseWhen = (index: number, newWhen: string) => {
      if (!column.cases) return;
      column.cases[index].when = newWhen;
      this.updateExpression(column);
      onchange();
    };

    const setCaseThen = (index: number, newThen: string) => {
      if (!column.cases) return;
      column.cases[index].then = newThen;
      this.updateExpression(column);
      onchange();
    };

    const addCase = () => {
      if (!column.cases) {
        column.cases = [];
      }
      column.cases.push({when: '', then: ''});
      this.updateExpression(column);
      onchange();
    };

    const removeCase = (index: number) => {
      if (!column.cases) return;
      column.cases.splice(index, 1);
      this.updateExpression(column);
      onchange();
    };

    if (column.switchOn === undefined || column.switchOn === '') {
      const columnNames = columns.map((c) => c.column.name);
      return m(
        '.pf-exp-switch-component',
        m(
          '.pf-exp-switch-header',
          'SWITCH ON ',
          m(
            Select,
            {
              onchange: (e: Event) => {
                setSwitchOn((e.target as HTMLSelectElement).value);
              },
            },
            m('option', {value: ''}, 'Select column'),
            ...columnNames.map((name) => m('option', {value: name}, name)),
          ),
        ),
      );
    }

    const columnNames = columns.map((c) => c.column.name);

    const selectedColumn = columns.find(
      (c) => c.column.name === column.switchOn,
    );
    const isStringColumn = selectedColumn?.type === 'STRING';

    return m(
      '.pf-exp-switch-component',
      m(
        '.pf-exp-switch-header',
        'SWITCH ON ',
        m(
          Select,
          {
            value: column.switchOn,
            onchange: (e: Event) => {
              setSwitchOn((e.target as HTMLSelectElement).value);
            },
          },
          ...columnNames.map((name) => m('option', {value: name}, name)),
        ),
      ),
      isStringColumn &&
        m(
          '.pf-exp-switch-glob-toggle',
          m(Switch, {
            label: 'Use glob matching',
            checked: column.useGlob ?? false,
            onchange: (e: Event) => {
              column.useGlob = (e.target as HTMLInputElement).checked;
              this.updateExpression(column);
              onchange();
            },
          }),
        ),
      m(
        '.pf-exp-switch-default-row',
        'Default ',
        m(TextInput, {
          placeholder: 'default value',
          value: column.defaultValue || '',
          oninput: (e: Event) => {
            setDefaultValue((e.target as HTMLInputElement).value);
          },
        }),
      ),
      ...(column.cases || []).map((c, i) =>
        m(
          '.pf-exp-switch-case',
          'WHEN ',
          m(TextInput, {
            placeholder: 'is equal to',
            value: c.when,
            oninput: (e: Event) => {
              setCaseWhen(i, (e.target as HTMLInputElement).value);
            },
          }),
          ' THEN ',
          m(TextInput, {
            placeholder: 'then value',
            value: c.then,
            oninput: (e: Event) => {
              setCaseThen(i, (e.target as HTMLInputElement).value);
            },
          }),
          m(Button, {
            icon: 'close',
            compact: true,
            onclick: () => removeCase(i),
          }),
        ),
      ),
      m(Button, {
        label: 'Add case',
        onclick: addCase,
      }),
    );
  }

  private updateExpression(col: NewColumn) {
    if (col.type !== 'switch' || !col.switchOn) {
      col.expression = '';
      return;
    }

    const operator = col.useGlob ? 'GLOB' : '=';
    const casesStr = (col.cases || [])
      .filter((c) => c.when.trim() !== '' && c.then.trim() !== '')
      .map((c) => `WHEN ${col.switchOn} ${operator} ${c.when} THEN ${c.then}`)
      .join(' ');

    const defaultStr = col.defaultValue ? `ELSE ${col.defaultValue}` : '';

    if (casesStr === '' && defaultStr === '') {
      col.expression = '';
      return;
    }

    col.expression = `CASE ${casesStr} ${defaultStr} END`;
  }
}

class IfComponent
  implements
    m.ClassComponent<{
      column: NewColumn;
      onchange: () => void;
    }>
{
  view({
    attrs,
  }: m.Vnode<{
    column: NewColumn;
    onchange: () => void;
  }>) {
    const {column, onchange} = attrs;

    if (column.type !== 'if') {
      return m('');
    }

    const setIfCondition = (index: number, newIf: string) => {
      if (!column.clauses) return;
      column.clauses[index].if = newIf;
      this.updateExpression(column);
      onchange();
    };

    const setThenValue = (index: number, newThen: string) => {
      if (!column.clauses) return;
      column.clauses[index].then = newThen;
      this.updateExpression(column);
      onchange();
    };

    const setElseValue = (newElse: string) => {
      column.elseValue = newElse;
      this.updateExpression(column);
      onchange();
    };

    const addElseIf = () => {
      if (!column.clauses) {
        column.clauses = [];
      }
      column.clauses.push({if: '', then: ''});
      this.updateExpression(column);
      onchange();
    };

    const removeClause = (index: number) => {
      if (!column.clauses) return;
      column.clauses.splice(index, 1);
      this.updateExpression(column);
      onchange();
    };

    const hasElse = column.elseValue !== undefined;

    return m(
      '.pf-exp-if-component',
      (column.clauses || []).map((c, i) =>
        m(
          '.pf-exp-if-clause',
          i === 0 ? 'IF ' : 'ELSE IF',
          m(TextInput, {
            placeholder: 'condition',
            value: c.if,
            oninput: (e: Event) => {
              setIfCondition(i, (e.target as HTMLInputElement).value);
            },
          }),
          ' THEN ',
          m(TextInput, {
            placeholder: 'value',
            value: c.then,
            oninput: (e: Event) => {
              setThenValue(i, (e.target as HTMLInputElement).value);
            },
          }),
          m(Button, {
            icon: 'close',
            compact: true,
            onclick: () => removeClause(i),
          }),
        ),
      ),

      hasElse &&
        m(
          '.pf-exp-else-clause',
          'ELSE ',
          m(TextInput, {
            placeholder: 'value',
            value: column.elseValue || '',
            oninput: (e: Event) => {
              setElseValue((e.target as HTMLInputElement).value);
            },
          }),
        ),

      m(
        '.pf-exp-if-buttons',
        !hasElse &&
          m(Button, {
            label: 'Add ELSE IF',
            onclick: addElseIf,
          }),
        !hasElse &&
          m(Button, {
            label: 'Add ELSE',
            onclick: () => {
              column.elseValue = '';
              this.updateExpression(column);
              onchange();
            },
          }),
      ),
    );
  }

  private updateExpression(col: NewColumn) {
    if (col.type !== 'if') {
      col.expression = '';
      return;
    }

    const clausesStr = (col.clauses || [])
      .filter((c) => c.if.trim() !== '' && c.then.trim() !== '')
      .map((c) => `WHEN ${c.if} THEN ${c.then}`)
      .join(' ');

    const elseStr =
      col.elseValue !== undefined ? `ELSE ${col.elseValue.trim()}` : '';

    if (clausesStr === '' && elseStr === '') {
      col.expression = '';
      return;
    }

    col.expression = `CASE ${clausesStr} ${elseStr} END`;
  }
}

interface IfClause {
  if: string;
  then: string;
}

interface NewColumn {
  expression: string;
  name: string;
  module?: string;

  // For switch columns
  type?: 'switch' | 'if';
  switchOn?: string;
  cases?: {when: string; then: string}[];
  defaultValue?: string;
  useGlob?: boolean;

  // For if columns
  clauses?: IfClause[];
  elseValue?: string;

  // SQL type for preserving type information across serialization
  sqlType?: string;
}

export interface AddColumnsNodeState extends QueryNodeState {
  selectedColumns?: string[];
  leftColumn?: string;
  rightColumn?: string;
  // Note: sqlTable is no longer used - we get columns from the connected node

  // Note: onAddAndConnectTable callback is now provided through
  // QueryNodeState.actions.onAddAndConnectTable

  // Pre-selected columns for each suggested table (before connecting)
  suggestionSelections?: Map<string, string[]>;

  // Track which suggestions are expanded to show column selection
  expandedSuggestions?: Set<string>;

  // Currently selected suggestion table (for single-selection UI)
  selectedSuggestionTable?: string;

  // Map from column name to its alias (for renaming added columns)
  columnAliases?: Map<string, string>;

  // Map from column name to its alias for suggestion mode (before applying)
  suggestionAliases?: Map<string, string>;

  // Track if connection was made through guided suggestion
  isGuidedConnection?: boolean;

  // Computed columns (expressions, SWITCH, IF)
  computedColumns?: NewColumn[];
}

export class AddColumnsNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kAddColumns;
  primaryInput?: QueryNode;
  secondaryInputs: {
    connections: Map<number, QueryNode>;
    min: 1;
    max: 1;
  };
  nextNodes: QueryNode[];
  readonly state: AddColumnsNodeState;

  constructor(state: AddColumnsNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.secondaryInputs = {
      connections: new Map(),
      min: 1,
      max: 1,
    };
    this.nextNodes = [];
    this.state.selectedColumns = this.state.selectedColumns ?? [];
    this.state.leftColumn = this.state.leftColumn ?? 'id';
    this.state.rightColumn = this.state.rightColumn ?? 'id';
    this.state.autoExecute = this.state.autoExecute ?? true;
    this.state.suggestionSelections =
      this.state.suggestionSelections ?? new Map();
    this.state.expandedSuggestions =
      this.state.expandedSuggestions ?? new Set();
    this.state.columnAliases = this.state.columnAliases ?? new Map();
    this.state.suggestionAliases = this.state.suggestionAliases ?? new Map();
    this.state.computedColumns = this.state.computedColumns ?? [];
  }

  // Called when a node is connected/disconnected to inputNodes
  onPrevNodesUpdated(): void {
    // Reset column selection when the right node changes
    this.state.selectedColumns = [];

    // If node is disconnected, reset the guided connection flag
    if (!this.rightNode) {
      this.state.isGuidedConnection = false;
    }

    this.state.onchange?.();
  }

  get sourceCols(): ColumnInfo[] {
    return this.primaryInput?.finalCols ?? [];
  }

  // Get the node connected to the left-side input port (for adding columns from)
  get rightNode(): QueryNode | undefined {
    return getSecondaryInput(this, 0);
  }

  get rightCols(): ColumnInfo[] {
    return this.rightNode?.finalCols ?? [];
  }

  get finalCols(): ColumnInfo[] {
    let cols = [...this.sourceCols];

    // Add columns from connected node (JOIN)
    if (this.rightNode) {
      // Add only selected columns (with aliases if provided)
      const newCols =
        this.state.selectedColumns?.map((c) => {
          const alias = this.state.columnAliases?.get(c);
          // If an alias is provided, use it as the column name
          return columnInfoFromName(alias ?? c);
        }) ?? [];
      cols = [...cols, ...newCols];
    }

    // Add computed columns (expressions, SWITCH, IF)
    const computedCols =
      this.state.computedColumns
        ?.filter((c) => this.isComputedColumnValid(c))
        .map((col) => {
          // Use stored sqlType if available (from deserialization)
          if (col.sqlType) {
            return columnInfoFromName(col.name);
          }
          // Try to preserve type information if the expression is a simple column reference
          const sourceCol = this.sourceCols.find(
            (c) => c.column.name === col.expression,
          );
          if (sourceCol) {
            col.sqlType = sourceCol.type;
            return columnInfoFromName(col.name);
          }
          // For complex expressions, use 'NA' as type
          return columnInfoFromName(col.name, true);
        }) ?? [];

    return [...cols, ...computedCols];
  }

  private isComputedColumnValid(col: NewColumn): boolean {
    return col.expression.trim() !== '' && col.name.trim() !== '';
  }

  // Check if a column name already exists (for duplicate detection)
  // excludeIndex: if editing an existing column, exclude it from the check
  private getColumnNameError(
    name: string,
    excludeIndex?: number,
  ): string | undefined {
    const trimmedName = name.trim();
    if (trimmedName === '') {
      return undefined; // Empty names are handled by isComputedColumnValid
    }

    // Check against source columns (use alias if present, otherwise column name)
    for (const c of this.sourceCols) {
      const effectiveName = c.alias ?? c.column.name;
      if (effectiveName === trimmedName) {
        return `Column "${trimmedName}" already exists in the source data`;
      }
    }

    // Check against selected columns from joined source (with aliases)
    if (this.state.selectedColumns) {
      for (const colName of this.state.selectedColumns) {
        const alias = this.state.columnAliases?.get(colName);
        const effectiveName = alias ?? colName;
        if (effectiveName === trimmedName) {
          return `Column "${trimmedName}" already exists in joined columns`;
        }
      }
    }

    // Check against other computed columns
    for (let i = 0; i < (this.state.computedColumns?.length ?? 0); i++) {
      if (i === excludeIndex) continue; // Skip the column being edited
      const col = this.state.computedColumns![i];
      if (col.name.trim() === trimmedName) {
        return `Column "${trimmedName}" already exists in computed columns`;
      }
    }

    return undefined;
  }

  // Suggest joinable tables based on JOINID column types
  getJoinSuggestions(): Array<{
    colName: string;
    suggestedTable: string;
    targetColumn: string;
  }> {
    const suggestions: Array<{
      colName: string;
      suggestedTable: string;
      targetColumn: string;
    }> = [];

    for (const col of this.sourceCols) {
      const colType = col.column.type;

      // Check if this column has a JOINID type with explicit source information
      if (colType && colType.kind === 'joinid') {
        suggestions.push({
          colName: col.column.name,
          suggestedTable: colType.source.table,
          targetColumn: colType.source.column,
        });
      }
    }

    return suggestions;
  }

  // Get available columns for a suggested table
  getTableColumns(tableName: string): string[] {
    const table = this.getTable(tableName);
    if (!table) return [];

    return table.columns.map((c) => c.name);
  }

  // Get full table info for a suggested table
  private getTable(tableName: string) {
    if (!this.state.sqlModules) return undefined;

    return this.state.sqlModules.listTables().find((t) => t.name === tableName);
  }

  getTitle(): string {
    return 'Add Columns';
  }

  // Check if the Apply button should be disabled in the join modal
  private isApplyDisabled(): boolean {
    // When no rightNode exists, require table and columns selection
    if (!this.rightNode) {
      const selectedTable = this.state.selectedSuggestionTable;
      if (!selectedTable) return true;
      const selectedColumns =
        this.state.suggestionSelections?.get(selectedTable) ?? [];
      if (selectedColumns.length === 0) return true;
      // Also disable if there are duplicate column name errors
      return this.getJoinColumnErrors(selectedColumns, true).length > 0;
    }
    // When rightNode exists, require columns to be selected
    if (
      !this.state.selectedColumns ||
      this.state.selectedColumns.length === 0
    ) {
      return true;
    }
    // Also disable if there are duplicate column name errors
    return (
      this.getJoinColumnErrors(this.state.selectedColumns, false).length > 0
    );
  }

  // Get errors for join columns (duplicates with source or between selected)
  // Returns array of error messages for columns that have conflicts
  private getJoinColumnErrors(
    selectedColumns: string[],
    useSuggestionAliases: boolean,
  ): Array<{column: string; error: string}> {
    const errors: Array<{column: string; error: string}> = [];
    const aliasMap = useSuggestionAliases
      ? this.state.suggestionAliases
      : this.state.columnAliases;

    // Get effective names (alias or original name) for all selected columns
    const effectiveNames = new Map<string, string>();
    for (const col of selectedColumns) {
      const alias = aliasMap?.get(col);
      effectiveNames.set(col, alias || col);
    }

    // Check each column against source columns
    const sourceColNames = new Set(
      this.sourceCols.map((c) => c.alias ?? c.column.name),
    );
    for (const col of selectedColumns) {
      const effectiveName = effectiveNames.get(col) ?? col;
      if (sourceColNames.has(effectiveName)) {
        errors.push({
          column: col,
          error: `"${effectiveName}" already exists in source data`,
        });
      }
    }

    // Check for duplicates among selected columns
    const seenNames = new Map<string, string>(); // effectiveName -> original column
    for (const col of selectedColumns) {
      const effectiveName = effectiveNames.get(col) ?? col;
      const existingCol = seenNames.get(effectiveName);
      if (existingCol && existingCol !== col) {
        errors.push({
          column: col,
          error: `"${effectiveName}" conflicts with another selected column`,
        });
      } else {
        seenNames.set(effectiveName, col);
      }
    }

    return errors;
  }

  // Get error for a specific column (for UI display)
  private getJoinColumnError(
    colName: string,
    selectedColumns: string[],
    useSuggestionAliases: boolean,
  ): string | undefined {
    const errors = this.getJoinColumnErrors(
      selectedColumns,
      useSuggestionAliases,
    );
    const error = errors.find((e) => e.column === colName);
    return error?.error;
  }

  nodeDetails(): m.Child {
    const hasConnectedNode = this.rightNode !== undefined;
    const hasComputedColumns = (this.state.computedColumns?.length ?? 0) > 0;
    const hasSelectedColumns =
      this.state.selectedColumns && this.state.selectedColumns.length > 0;

    if (!hasSelectedColumns && !hasComputedColumns) {
      return m('.pf-exp-node-details-message', 'No columns added');
    }

    const items: m.Child[] = [];

    // Add joined columns
    if (hasConnectedNode && hasSelectedColumns) {
      for (const col of this.state.selectedColumns ?? []) {
        const alias = this.state.columnAliases?.get(col);
        const displayName = alias || col;
        items.push(
          m('div', [
            m('span.pf-exp-column-name', displayName),
            ': column from input',
          ]),
        );
      }
    }

    // Add computed columns
    for (const col of this.state.computedColumns ?? []) {
      const name = col.name || '(unnamed)';
      let description = '';

      if (col.type === 'switch') {
        description = `SWITCH ON ${col.switchOn || '(not set)'}`;
      } else if (col.type === 'if') {
        const firstCondition = col.clauses?.[0]?.if || '(empty)';
        description = `if ${firstCondition}`;
      } else {
        description = col.expression || '(empty)';
      }

      items.push(
        m('div', [m('span.pf-exp-column-name', name), `: ${description}`]),
      );
    }

    return m('div', items);
  }

  nodeSpecificModify(): m.Child {
    return m('div', [
      this.renderAddColumnsButtons(),
      this.renderAddedColumnsList(),
    ]);
  }

  private showJoinModal() {
    const modalKey = 'add-join-modal';

    showModal({
      title: 'Add Columns from Another Source',
      key: modalKey,
      className: 'pf-join-modal-wide',
      content: () => {
        return m('div', this.renderGuidedMode());
      },
      buttons: [
        {
          text: 'Cancel',
          action: () => {
            // Just close
          },
        },
        {
          text: 'Apply',
          primary: true,
          disabled: () => this.isApplyDisabled(),
          action: () => {
            // If there's no rightNode, connect the selected suggestion table
            if (!this.rightNode && this.state.selectedSuggestionTable) {
              const suggestions = this.getJoinSuggestions();
              const selectedSuggestion = suggestions.find(
                (s) => s.suggestedTable === this.state.selectedSuggestionTable,
              );
              const selectedColumns =
                this.state.suggestionSelections?.get(
                  this.state.selectedSuggestionTable,
                ) ?? [];

              if (selectedSuggestion && selectedColumns.length > 0) {
                if (this.state.actions?.onAddAndConnectTable) {
                  this.state.isGuidedConnection = true;
                  this.state.actions.onAddAndConnectTable(
                    selectedSuggestion.suggestedTable,
                    0,
                  );
                  this.state.leftColumn = selectedSuggestion.colName;
                  this.state.rightColumn = selectedSuggestion.targetColumn;
                  this.state.selectedColumns = [...selectedColumns];
                  // Copy suggestion aliases to column aliases
                  if (this.state.suggestionAliases) {
                    if (!this.state.columnAliases) {
                      this.state.columnAliases = new Map();
                    }
                    for (const col of selectedColumns) {
                      const alias = this.state.suggestionAliases.get(col);
                      if (alias) {
                        this.state.columnAliases.set(col, alias);
                      }
                    }
                  }
                }
              }
            }
            this.state.onchange?.();
          },
        },
      ],
    });
  }

  private showExpressionModal(columnIndex?: number) {
    const modalKey = 'add-expression-modal';
    const isEditing = columnIndex !== undefined;

    // Create a temporary copy to work with in the modal
    let tempColumn: NewColumn;
    if (isEditing && this.state.computedColumns?.[columnIndex]) {
      tempColumn = {...this.state.computedColumns[columnIndex]};
    } else {
      tempColumn = {expression: '', name: ''};
    }

    showModal({
      title: isEditing ? 'Edit Expression Column' : 'Add Expression Column',
      key: modalKey,
      content: () => {
        const nameError = this.getColumnNameError(tempColumn.name, columnIndex);
        return this.renderComputedColumn(tempColumn, nameError);
      },
      buttons: [
        {
          text: 'Cancel',
          action: () => {
            // Do nothing - changes are not applied
          },
        },
        {
          text: isEditing ? 'Save' : 'Add',
          primary: true,
          disabled: () =>
            !this.isComputedColumnValid(tempColumn) ||
            this.getColumnNameError(tempColumn.name, columnIndex) !== undefined,
          action: () => {
            // Apply the temporary changes to the actual state
            if (isEditing && columnIndex !== undefined) {
              const newComputedColumns = [
                ...(this.state.computedColumns ?? []),
              ];
              newComputedColumns[columnIndex] = tempColumn;
              this.state.computedColumns = newComputedColumns;
            } else {
              this.state.computedColumns = [
                ...(this.state.computedColumns ?? []),
                tempColumn,
              ];
            }
            this.state.onchange?.();
          },
        },
      ],
    });
  }

  private showSwitchModal(columnIndex?: number) {
    const modalKey = 'add-switch-modal';
    const isEditing = columnIndex !== undefined;

    // Create a temporary copy to work with in the modal
    let tempColumn: NewColumn;
    if (isEditing && this.state.computedColumns?.[columnIndex]) {
      tempColumn = {
        ...this.state.computedColumns[columnIndex],
        cases: this.state.computedColumns[columnIndex].cases?.map((c) => ({
          ...c,
        })),
      };
    } else {
      tempColumn = {
        type: 'switch' as const,
        expression: '',
        name: '',
        cases: [],
      };
    }

    showModal({
      title: isEditing ? 'Edit Switch Column' : 'Add Switch Column',
      key: modalKey,
      content: () => {
        const nameError = this.getColumnNameError(tempColumn.name, columnIndex);
        return this.renderComputedColumn(tempColumn, nameError);
      },
      buttons: [
        {
          text: 'Cancel',
          action: () => {
            // Do nothing - changes are not applied
          },
        },
        {
          text: isEditing ? 'Save' : 'Add',
          primary: true,
          disabled: () =>
            !this.isComputedColumnValid(tempColumn) ||
            this.getColumnNameError(tempColumn.name, columnIndex) !== undefined,
          action: () => {
            // Apply the temporary changes to the actual state
            if (isEditing && columnIndex !== undefined) {
              const newComputedColumns = [
                ...(this.state.computedColumns ?? []),
              ];
              newComputedColumns[columnIndex] = tempColumn;
              this.state.computedColumns = newComputedColumns;
            } else {
              this.state.computedColumns = [
                ...(this.state.computedColumns ?? []),
                tempColumn,
              ];
            }
            this.state.onchange?.();
          },
        },
      ],
    });
  }

  private showIfModal(columnIndex?: number) {
    const modalKey = 'add-if-modal';
    const isEditing = columnIndex !== undefined;

    // Create a temporary copy to work with in the modal
    let tempColumn: NewColumn;
    if (isEditing && this.state.computedColumns?.[columnIndex]) {
      tempColumn = {
        ...this.state.computedColumns[columnIndex],
        clauses: this.state.computedColumns[columnIndex].clauses?.map((c) => ({
          ...c,
        })),
      };
    } else {
      tempColumn = {
        type: 'if' as const,
        expression: '',
        name: '',
        clauses: [{if: '', then: ''}],
      };
    }

    showModal({
      title: isEditing ? 'Edit If Column' : 'Add If Column',
      key: modalKey,
      content: () => {
        const nameError = this.getColumnNameError(tempColumn.name, columnIndex);
        return this.renderComputedColumn(tempColumn, nameError);
      },
      buttons: [
        {
          text: 'Cancel',
          action: () => {
            // Do nothing - changes are not applied
          },
        },
        {
          text: isEditing ? 'Save' : 'Add',
          primary: true,
          disabled: () =>
            !this.isComputedColumnValid(tempColumn) ||
            this.getColumnNameError(tempColumn.name, columnIndex) !== undefined,
          action: () => {
            // Apply the temporary changes to the actual state
            if (isEditing && columnIndex !== undefined) {
              const newComputedColumns = [
                ...(this.state.computedColumns ?? []),
              ];
              newComputedColumns[columnIndex] = tempColumn;
              this.state.computedColumns = newComputedColumns;
            } else {
              this.state.computedColumns = [
                ...(this.state.computedColumns ?? []),
                tempColumn,
              ];
            }
            this.state.onchange?.();
          },
        },
      ],
    });
  }

  private renderAddColumnsButtons(): m.Child {
    const hasConnectedNode = this.rightNode !== undefined;

    return m(
      '.pf-add-columns-actions-section',
      m(ActionButtons, {
        buttons: [
          {
            label: 'From another source',
            icon: 'table_chart',
            active: hasConnectedNode,
            onclick: () => this.showJoinModal(),
          },
          {
            label: 'Expression',
            icon: 'functions',
            onclick: () => this.showExpressionModal(),
          },
          {
            label: 'Switch',
            icon: 'alt_route',
            onclick: () => this.showSwitchModal(),
          },
          {
            label: 'If',
            icon: 'help_outline',
            onclick: () => this.showIfModal(),
          },
        ],
      }),
    );
  }

  private renderAddedColumnsList(): m.Child {
    const hasConnectedNode = this.rightNode !== undefined;
    const hasComputedColumns = (this.state.computedColumns?.length ?? 0) > 0;

    if (!hasConnectedNode && !hasComputedColumns) {
      return m(EmptyState, {
        title: 'No columns added yet. Use the buttons above to add columns.',
      });
    }

    const items: m.Child[] = [];

    // Show joined columns
    if (hasConnectedNode) {
      items.push(
        m(ListItem, {
          icon: 'table_chart',
          name: 'Joined Source',
          description: `${this.state.selectedColumns?.length ?? 0} selected columns`,
          actionLabel: 'Configure',
          actionIcon: 'settings',
          onAction: () => this.showJoinModal(),
          className: 'pf-joined-source',
        }),
      );
    }

    // Show computed columns
    for (const [index, col] of (this.state.computedColumns ?? []).entries()) {
      const icon =
        col.type === 'switch'
          ? 'alt_route'
          : col.type === 'if'
            ? 'help_outline'
            : 'functions';
      const typeName =
        col.type === 'switch'
          ? 'Switch'
          : col.type === 'if'
            ? 'If'
            : 'Expression';

      // Show the expression/preview for the column
      const description =
        col.type === 'switch' || col.type === 'if'
          ? typeName
          : col.expression
            ? `${typeName}: ${col.expression}`
            : `${typeName} (empty)`;

      items.push(
        m(ListItem, {
          icon,
          name: col.name || '(unnamed)',
          description,
          actionLabel: 'Edit',
          actionIcon: 'edit',
          onAction: () => {
            if (col.type === 'switch') {
              this.showSwitchModal(index);
            } else if (col.type === 'if') {
              this.showIfModal(index);
            } else {
              this.showExpressionModal(index);
            }
          },
          onRemove: () => {
            this.state.computedColumns?.splice(index, 1);
            this.state.onchange?.();
          },
        }),
      );
    }

    return m('.pf-added-columns-list', items);
  }

  private renderGuidedMode(): m.Child {
    if (!this.rightNode) {
      const suggestions = this.getJoinSuggestions();

      if (suggestions.length === 0) {
        return m(
          Form,
          m(
            'p',
            'No JOINID columns found in your data. You can still connect any node to the left port.',
          ),
        );
      }

      // Find the currently selected suggestion (if any)
      const selectedTable = this.state.selectedSuggestionTable;
      const selectedSuggestion = suggestions.find(
        (s) => s.suggestedTable === selectedTable,
      );
      const tableInfo = selectedTable
        ? this.getTable(selectedTable)
        : undefined;
      const availableColumns = selectedTable
        ? this.getTableColumns(selectedTable)
        : [];
      const selectedColumns = selectedTable
        ? this.state.suggestionSelections?.get(selectedTable) ?? []
        : [];

      return m(
        '.pf-join-modal-layout',
        // Left column: Form controls
        m(
          '.pf-join-modal-controls',
          m(
            Form,
            // Step 1: Select which table to join
            m(FormSection, {label: 'Select Table to Join'}, [
              m(
                Select,
                {
                  onchange: (e: Event) => {
                    const value = (e.target as HTMLSelectElement).value;
                    this.state.selectedSuggestionTable = value || undefined;
                    m.redraw();
                  },
                },
                m(
                  'option',
                  {value: '', selected: !selectedTable},
                  'Choose a table',
                ),
                suggestions.map((s) =>
                  m(
                    'option',
                    {
                      value: s.suggestedTable,
                      selected: s.suggestedTable === selectedTable,
                    },
                    s.suggestedTable,
                  ),
                ),
              ),
            ]),

            // Step 2: Show join condition (only when table is selected)
            selectedSuggestion &&
              m(
                LabeledControl,
                {label: 'Join on:'},
                m(
                  'span',
                  m('code', selectedSuggestion.colName),
                  ' = ',
                  m('code', selectedSuggestion.targetColumn),
                ),
              ),

            // Step 3: Select columns (only when table is selected)
            selectedSuggestion &&
              m(
                LabeledControl,
                {label: 'Columns:'},
                m(PopupMultiSelect, {
                  label:
                    selectedColumns.length > 0
                      ? selectedColumns.join(', ')
                      : 'Select columns to add',
                  showNumSelected: false,
                  compact: true,
                  options: availableColumns.map((col) => ({
                    id: col,
                    name: col,
                    checked: selectedColumns.includes(col),
                  })),
                  onChange: (diffs: MultiSelectDiff[]) => {
                    if (!this.state.suggestionSelections) {
                      this.state.suggestionSelections = new Map();
                    }
                    const current =
                      this.state.suggestionSelections.get(selectedTable!) ?? [];
                    let updated = [...current];
                    for (const diff of diffs) {
                      if (diff.checked) {
                        if (!updated.includes(diff.id)) {
                          updated.push(diff.id);
                        }
                      } else {
                        updated = updated.filter((c) => c !== diff.id);
                      }
                    }
                    this.state.suggestionSelections.set(
                      selectedTable!,
                      updated,
                    );
                    m.redraw();
                  },
                }),
              ),

            // Show hint when table is selected but no columns are selected
            selectedSuggestion &&
              selectedColumns.length === 0 &&
              m(
                Callout,
                {icon: 'info'},
                'Select at least one column to add from the joined table.',
              ),

            // Show alias inputs for selected columns (suggestion mode)
            selectedSuggestion &&
              selectedColumns.length > 0 &&
              m(FormSection, {label: 'Column Aliases (optional)'}, [
                m(FormLabel, 'Rename columns to avoid conflicts:'),
                selectedColumns.map((colName) => {
                  const error = this.getJoinColumnError(
                    colName,
                    selectedColumns,
                    true,
                  );
                  return m(
                    LabeledControl,
                    {label: `${colName} →`},
                    m(TextInput, {
                      placeholder: error
                        ? 'alias required'
                        : 'alias (optional)',
                      value: this.state.suggestionAliases?.get(colName) ?? '',
                      oninput: (e: InputEvent) => {
                        const target = e.target as HTMLInputElement;
                        const alias = target.value.trim();
                        if (!this.state.suggestionAliases) {
                          this.state.suggestionAliases = new Map();
                        }
                        if (alias) {
                          this.state.suggestionAliases.set(colName, alias);
                        } else {
                          this.state.suggestionAliases.delete(colName);
                        }
                        m.redraw();
                      },
                    }),
                    error && m(Icon, {icon: 'error'}),
                  );
                }),
              ]),

            // Show error summary if there are conflicts
            selectedSuggestion &&
              selectedColumns.length > 0 &&
              m(IssueList, {
                icon: 'error',
                title: 'Column name conflicts:',
                items: this.getJoinColumnErrors(selectedColumns, true).map(
                  (err) => err.error,
                ),
              }),
          ),
        ),

        // Right column: Table info panel (only when table is selected)
        tableInfo &&
          m('.pf-join-modal-info', m(TableDescription, {table: tableInfo})),
      );
    }

    const leftCols = this.sourceCols;
    const rightCols = this.rightCols;

    const selectedColumns = this.state.selectedColumns ?? [];
    const noColumnsSelected = selectedColumns.length === 0;
    const selectedColumnsLabel = noColumnsSelected
      ? 'Select columns to add'
      : selectedColumns.join(', ');

    return m(
      Form,
      m(
        LabeledControl,
        {label: 'Columns:'},
        m(PopupMultiSelect, {
          label: selectedColumnsLabel,
          showNumSelected: false,
          compact: true,
          options: rightCols.map((c) => ({
            id: c.column.name,
            name: c.column.name,
            checked:
              this.state.selectedColumns?.includes(c.column.name) ?? false,
          })),
          onChange: (diffs: MultiSelectDiff[]) => {
            if (!this.state.selectedColumns) {
              this.state.selectedColumns = [];
            }
            for (const diff of diffs) {
              if (diff.checked) {
                if (!this.state.selectedColumns.includes(diff.id)) {
                  this.state.selectedColumns.push(diff.id);
                }
              } else {
                this.state.selectedColumns = this.state.selectedColumns.filter(
                  (c) => c !== diff.id,
                );
                // Also remove the alias for this column
                this.state.columnAliases?.delete(diff.id);
              }
            }
            this.state.onchange?.();
          },
        }),
      ),
      // Show hint when no columns are selected
      noColumnsSelected &&
        m(
          Callout,
          {icon: 'info'},
          'Select at least one column to add from the joined source.',
        ),
      // Show alias inputs for selected columns
      this.state.selectedColumns && this.state.selectedColumns.length > 0
        ? m(FormSection, {label: 'Column Aliases (optional)'}, [
            m(FormLabel, 'Rename columns to avoid conflicts:'),
            this.state.selectedColumns.map((colName) => {
              const error = this.getJoinColumnError(
                colName,
                this.state.selectedColumns!,
                false,
              );
              return m(
                LabeledControl,
                {label: `${colName} →`},
                m(TextInput, {
                  placeholder: error ? 'alias required' : 'alias (optional)',
                  value: this.state.columnAliases?.get(colName) ?? '',
                  oninput: (e: InputEvent) => {
                    const target = e.target as HTMLInputElement;
                    const alias = target.value.trim();
                    if (!this.state.columnAliases) {
                      this.state.columnAliases = new Map();
                    }
                    if (alias) {
                      this.state.columnAliases.set(colName, alias);
                    } else {
                      this.state.columnAliases.delete(colName);
                    }
                    this.state.onchange?.();
                  },
                }),
                error && m(Icon, {icon: 'error'}),
              );
            }),
          ])
        : null,
      // Show error summary if there are conflicts
      this.state.selectedColumns &&
        this.state.selectedColumns.length > 0 &&
        m(IssueList, {
          icon: 'error',
          title: 'Column name conflicts:',
          items: this.getJoinColumnErrors(
            this.state.selectedColumns,
            false,
          ).map((err) => err.error),
        }),
      m(FormSection, {label: 'Join Condition'}, [
        m(FormLabel, 'Base Column'),
        m(
          Select,
          {
            onchange: (e: Event) => {
              const target = e.target as HTMLSelectElement;
              this.state.leftColumn = target.value;
              this.state.onchange?.();
            },
          },
          m(
            'option',
            {disabled: true, selected: !this.state.leftColumn},
            'Select column',
          ),
          leftCols.map((col) =>
            m(
              'option',
              {
                value: col.column.name,
                selected: col.column.name === this.state.leftColumn,
              },
              col.column.name,
            ),
          ),
        ),
        m(FormLabel, 'Connected Node Column'),
        m(
          Select,
          {
            onchange: (e: Event) => {
              const target = e.target as HTMLSelectElement;
              this.state.rightColumn = target.value;
              this.state.onchange?.();
            },
          },
          m(
            'option',
            {disabled: true, selected: !this.state.rightColumn},
            'Select column',
          ),
          rightCols.map((col) =>
            m(
              'option',
              {
                value: col.column.name,
                selected: col.column.name === this.state.rightColumn,
              },
              col.column.name,
            ),
          ),
        ),
      ]),
    );
  }

  private renderComputedColumn(col: NewColumn, nameError?: string): m.Child {
    if (col.type === 'switch') {
      return m(
        Form,
        nameError && m(Callout, {icon: 'error'}, nameError),
        m(FormSection, {label: 'Column Name'}, [
          m(TextInput, {
            placeholder: 'Enter column name',
            value: col.name,
            oninput: (e: Event) => {
              col.name = (e.target as HTMLInputElement).value;
              redrawModal();
            },
          }),
        ]),
        m(FormSection, {label: 'Switch Configuration'}, [
          m(SwitchComponent, {
            column: col,
            columns: this.sourceCols,
            onchange: () => {
              // No-op in modal mode - changes are already in col
            },
          }),
        ]),
      );
    }

    if (col.type === 'if') {
      return m(
        Form,
        nameError && m(Callout, {icon: 'error'}, nameError),
        m(FormSection, {label: 'Column Name'}, [
          m(TextInput, {
            placeholder: 'Enter column name',
            value: col.name,
            oninput: (e: Event) => {
              col.name = (e.target as HTMLInputElement).value;
              redrawModal();
            },
          }),
        ]),
        m(FormSection, {label: 'If Configuration'}, [
          m(IfComponent, {
            column: col,
            onchange: () => {
              // No-op in modal mode - changes are already in col
            },
          }),
        ]),
      );
    }

    const isValid = this.isComputedColumnValid(col) && !nameError;

    return m(
      Form,
      nameError && m(Callout, {icon: 'error'}, nameError),
      m(
        'p',
        'Create a computed column using any SQL expression. Example: ',
        m('code', 'dur / 1e6'),
        ' to convert duration to milliseconds.',
      ),
      m(FormSection, {label: 'SQL Expression'}, [
        m(TextInput, {
          oninput: (e: Event) => {
            col.expression = (e.target as HTMLInputElement).value;
          },
          placeholder:
            'Enter SQL expression (e.g., dur / 1e6, name || "_suffix")',
          value: col.expression,
        }),
      ]),
      m(FormSection, {label: 'Column Name'}, [
        m(TextInput, {
          oninput: (e: Event) => {
            col.name = (e.target as HTMLInputElement).value;
            redrawModal();
          },
          placeholder: 'Enter column name (e.g., dur_ms)',
          value: col.name,
        }),
      ]),
      !isValid && m(Icon, {icon: 'warning'}),
    );
  }

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Enrich your data by adding columns from another table or query. Connect the additional source to the left port.',
      ),
      m(
        'p',
        'Specify which columns to match (join key) and which columns to add. In Guided mode, get suggestions based on JOINID columns.',
      ),
      m(
        'p',
        m('strong', 'Example:'),
        ' Add process details to slices by joining ',
        m('code', 'upid'),
        ' with the process table.',
      ),
    );
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.state.issues) {
      this.state.issues.clear();
    }

    if (this.primaryInput === undefined) {
      setValidationError(this.state, 'No input node connected');
      return false;
    }

    if (!this.primaryInput.validate()) {
      setValidationError(this.state, 'Previous node is invalid');
      return false;
    }

    // If there's a rightNode, validate the join configuration
    if (this.rightNode) {
      // We need valid join columns
      if (!this.state.leftColumn || !this.state.rightColumn) {
        setValidationError(
          this.state,
          'Join requires both left and right join columns to be selected',
        );
        return false;
      }
    }

    // If no columns are being added (no rightNode and no computed columns),
    // this is valid - it's just a passthrough node
    return true;
  }

  clone(): QueryNode {
    return new AddColumnsNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return undefined;
    if (this.primaryInput === undefined) return undefined;

    // If there's no rightNode, we only add computed columns (no JOIN)
    if (!this.rightNode) {
      const computedColumns: ColumnSpec[] = [];
      for (const col of this.state.computedColumns ?? []) {
        if (!this.isComputedColumnValid(col)) continue;
        computedColumns.push({
          columnNameOrExpression: col.expression,
          alias: col.name,
          referencedModule: col.module,
        });
      }

      // If there are no computed columns, just pass through
      if (computedColumns.length === 0) {
        return this.primaryInput.getStructuredQuery();
      }

      // Build column specifications including existing columns and computed columns
      const allColumns: ColumnSpec[] = [
        ...this.sourceCols.map((col) => ({
          columnNameOrExpression: col.column.name,
        })),
        ...computedColumns,
      ];

      // Collect referenced modules
      const referencedModules = this.state.computedColumns
        ?.filter((col) => col.module)
        .map((col) => col.module!);

      // Use withSelectColumns to add computed columns without a JOIN
      return StructuredQueryBuilder.withSelectColumns(
        this.primaryInput,
        allColumns,
        referencedModules && referencedModules.length > 0
          ? referencedModules
          : undefined,
        this.nodeId,
      );
    }

    // Prepare input columns (for JOIN)
    const inputColumns: ColumnSpec[] = (this.state.selectedColumns ?? []).map(
      (colName) => {
        const alias = this.state.columnAliases?.get(colName);
        return {
          columnNameOrExpression: colName,
          alias: alias && alias.trim() !== '' ? alias.trim() : undefined,
        };
      },
    );

    // Add computed columns to the JOIN
    for (const col of this.state.computedColumns ?? []) {
      if (!this.isComputedColumnValid(col)) continue;
      inputColumns.push({
        columnNameOrExpression: col.expression,
        alias: col.name,
        referencedModule: col.module,
      });
    }

    // If no columns are selected from the JOIN and no computed columns, just pass through
    if (inputColumns.length === 0) {
      return this.primaryInput.getStructuredQuery();
    }

    // Prepare join condition
    const condition: JoinCondition = {
      type: 'equality',
      leftColumn: this.state.leftColumn!,
      rightColumn: this.state.rightColumn!,
    };

    return StructuredQueryBuilder.withAddColumns(
      this.primaryInput,
      this.rightNode,
      inputColumns,
      condition,
      this.nodeId,
    );
  }

  serializeState(): object {
    // Get the secondary input node ID (the node connected to port 0)
    const secondaryInputNodeId =
      this.secondaryInputs.connections.get(0)?.nodeId;

    return {
      primaryInputId: this.primaryInput?.nodeId,
      secondaryInputNodeId,
      selectedColumns: this.state.selectedColumns,
      leftColumn: this.state.leftColumn,
      rightColumn: this.state.rightColumn,
      suggestionSelections: this.state.suggestionSelections
        ? Object.fromEntries(this.state.suggestionSelections)
        : undefined,
      expandedSuggestions: this.state.expandedSuggestions
        ? Array.from(this.state.expandedSuggestions)
        : undefined,
      selectedSuggestionTable: this.state.selectedSuggestionTable,
      columnAliases: this.state.columnAliases
        ? Object.fromEntries(this.state.columnAliases)
        : undefined,
      suggestionAliases: this.state.suggestionAliases
        ? Object.fromEntries(this.state.suggestionAliases)
        : undefined,
      isGuidedConnection: this.state.isGuidedConnection,
      comment: this.state.comment,
      autoExecute: this.state.autoExecute,
      computedColumns: this.state.computedColumns?.map((c) => ({
        expression: c.expression,
        name: c.name,
        module: c.module,
        type: c.type,
        switchOn: c.switchOn,
        cases: c.cases
          ? c.cases.map((cs) => ({when: cs.when, then: cs.then}))
          : undefined,
        defaultValue: c.defaultValue,
        useGlob: c.useGlob,
        clauses: c.clauses
          ? c.clauses.map((cl) => ({if: cl.if, then: cl.then}))
          : undefined,
        elseValue: c.elseValue,
        sqlType: c.sqlType,
      })),
    };
  }

  static deserializeState(
    serializedState: AddColumnsNodeState,
  ): AddColumnsNodeState {
    return {
      ...serializedState,
      suggestionSelections:
        (serializedState.suggestionSelections as unknown as Record<
          string,
          string[]
        >) !== undefined
          ? new Map(
              Object.entries(
                serializedState.suggestionSelections as unknown as Record<
                  string,
                  string[]
                >,
              ),
            )
          : undefined,
      expandedSuggestions:
        (serializedState.expandedSuggestions as unknown as string[]) !==
        undefined
          ? new Set(serializedState.expandedSuggestions as unknown as string[])
          : undefined,
      columnAliases:
        (serializedState.columnAliases as unknown as Record<string, string>) !==
        undefined
          ? new Map(
              Object.entries(
                serializedState.columnAliases as unknown as Record<
                  string,
                  string
                >,
              ),
            )
          : undefined,
      suggestionAliases:
        (serializedState.suggestionAliases as unknown as Record<
          string,
          string
        >) !== undefined
          ? new Map(
              Object.entries(
                serializedState.suggestionAliases as unknown as Record<
                  string,
                  string
                >,
              ),
            )
          : undefined,
    };
  }
}
