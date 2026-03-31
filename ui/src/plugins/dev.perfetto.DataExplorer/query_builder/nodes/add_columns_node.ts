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
  nextNodeId,
  NodeType,
  SecondaryInputSpec,
  NodeContext,
} from '../../query_node';
import {getSecondaryInput} from '../graph_utils';
import {
  ColumnInfo,
  columnInfoFromSqlColumn,
  legacyDeserializeType,
} from '../column_info';
import {
  PerfettoSqlType,
  PerfettoSqlTypes,
} from '../../../../trace_processor/perfetto_sql_type';
import protos from '../../../../protos';
import m from 'mithril';
import {Select} from '../../../../widgets/select';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {TextInput} from '../../../../widgets/text_input';
import {showModal, redrawModal, closeModal} from '../../../../widgets/modal';
import {
  StructuredQueryBuilder,
  ColumnSpec,
  JoinCondition,
} from '../structured_query_builder';
import {setValidationError} from '../node_issues';
import {EmptyState} from '../../../../widgets/empty_state';
import {Callout} from '../../../../widgets/callout';
import {Form, FormSection} from '../../../../widgets/form';
import {NodeModifyAttrs, NodeDetailsAttrs} from '../../node_types';
import {NodeDetailsMessage, ColumnName} from '../node_styling_widgets';
import {Spinner} from '../../../../widgets/spinner';
import {STR} from '../../../../trace_processor/query_result';
import {sqliteString} from '../../../../base/string_utils';
import {loadNodeDoc} from '../node_doc_loader';
import {NewColumn, AddColumnsNodeAttrs} from './add_columns_types';
import {SwitchComponent, IfComponent} from './computed_column_components';
import {AddColumnsSuggestionModal} from './add_columns_suggestion_modal';
import {AddColumnsConfigurationModal} from './add_columns_configuration_modal';
import {renderTypeSelector} from './modify_columns_utils';
import {FunctionWithModule} from '../function_list';
import {Icon} from '../../../../widgets/icon';
import {
  FunctionModalState,
  createFunctionModalState,
  isFunctionModalValid,
  createFunctionColumn,
  FunctionSelectStep,
  FunctionConfigureStep,
} from './add_columns_function_modal';

// Re-export types for backwards compatibility
export {NewColumn, AddColumnsNodeAttrs} from './add_columns_types';

export class AddColumnsNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kAddColumns;
  primaryInput?: QueryNode;
  secondaryInputs: SecondaryInputSpec;
  nextNodes: QueryNode[];
  readonly attrs: AddColumnsNodeAttrs;
  readonly context: NodeContext;

  constructor(attrs: AddColumnsNodeAttrs, context: NodeContext) {
    this.nodeId = nextNodeId();
    this.attrs = {
      ...attrs,
      selectedColumns: attrs.selectedColumns ?? [],
      leftColumn: attrs.leftColumn ?? 'id',
      rightColumn: attrs.rightColumn ?? 'id',
      suggestionSelections: attrs.suggestionSelections ?? {},
      expandedSuggestions: attrs.expandedSuggestions ?? [],
      columnAliases: attrs.columnAliases ?? {},
      suggestionAliases: attrs.suggestionAliases ?? {},
      columnTypes: attrs.columnTypes ?? {},
      computedColumns: attrs.computedColumns ?? [],
    };
    this.context = {...context, autoExecute: context.autoExecute ?? true};
    this.secondaryInputs = {
      connections: new Map(),
      min: 1,
      max: 1,
      portNames: ['Table'],
    };
    this.nextNodes = [];
  }

  // Called when a node is connected/disconnected to inputNodes
  onPrevNodesUpdated(): void {
    // If node is disconnected, reset everything
    if (!this.rightNode) {
      this.attrs.selectedColumns = [];
      this.attrs.isGuidedConnection = false;
      this.context.onchange?.();
      return;
    }

    // Check if the join column is still valid
    if (this.attrs.rightColumn) {
      const rightColExists = this.rightCols.some(
        (c) => c.name === this.attrs.rightColumn,
      );
      if (!rightColExists) {
        // Join column no longer exists - reset selection
        this.attrs.selectedColumns = [];
        this.attrs.rightColumn = undefined;
      }
    }

    this.context.onchange?.();
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
      // Add only selected columns (with aliases and types if provided)
      const newCols =
        this.attrs.selectedColumns?.map((c) => {
          const alias = this.attrs.columnAliases?.[c];
          const storedType = this.attrs.columnTypes?.[c];

          // Find the column in rightCols to get type information
          const sourceCol = this.rightCols.find((col) => col.name === c);
          if (sourceCol) {
            // Use stored type if available, otherwise use source type
            const finalType = storedType ?? sourceCol.type;

            return columnInfoFromSqlColumn({
              name: alias ?? c,
              type: finalType,
            });
          }
          // Fallback if column not found (shouldn't happen in valid state)
          return {name: alias ?? c, checked: false};
        }) ?? [];
      cols = [...cols, ...newCols];
    }

    // Add computed columns (expressions, SWITCH, IF)
    const computedCols =
      this.attrs.computedColumns
        ?.filter((c) => this.isComputedColumnValid(c))
        .map((col) => {
          // Use stored sqlType if available (from deserialization or user change)
          if (col.sqlType) {
            return columnInfoFromSqlColumn({
              name: col.name,
              type: col.sqlType,
            });
          }
          // Try to preserve type information if the expression is a simple column reference
          const sourceCol = this.sourceCols.find(
            (c) => c.name === col.expression,
          );
          if (sourceCol?.type) {
            col.sqlType = sourceCol.type;
            return columnInfoFromSqlColumn({
              name: col.name,
              type: sourceCol.type,
            });
          }
          // For complex expressions, we can't infer the type, use INT as default
          return columnInfoFromSqlColumn({
            name: col.name,
            type: PerfettoSqlTypes.INT,
          });
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

    // Reject names that aren't valid SQL identifiers (alphanumeric + underscore)
    if (/[^a-zA-Z0-9_]/.test(trimmedName)) {
      return 'Column name can only contain letters, numbers, and underscores';
    }

    // Check against source columns (use alias if present, otherwise column name)
    for (const c of this.sourceCols) {
      const effectiveName = c.alias ?? c.name;
      if (effectiveName === trimmedName) {
        return `Column "${trimmedName}" already exists in the source data`;
      }
    }

    // Check against selected columns from joined source (with aliases)
    if (this.attrs.selectedColumns) {
      for (const colName of this.attrs.selectedColumns) {
        const alias = this.attrs.columnAliases?.[colName];
        const effectiveName = alias ?? colName;
        if (effectiveName === trimmedName) {
          return `Column "${trimmedName}" already exists in joined columns`;
        }
      }
    }

    // Check against other computed columns
    for (let i = 0; i < (this.attrs.computedColumns?.length ?? 0); i++) {
      if (i === excludeIndex) continue; // Skip the column being edited
      const col = this.attrs.computedColumns![i];
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
      const colType = col.type;

      // Check if this column has a JOINID type with explicit source information
      if (colType && colType.kind === 'joinid') {
        suggestions.push({
          colName: col.name,
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
    if (!this.context.sqlModules) return undefined;

    return this.context.sqlModules
      .listTables()
      .find((t) => t.name === tableName);
  }

  // Find all arg_set_id columns in source columns
  getArgSetIdColumns(): ColumnInfo[] {
    return this.sourceCols.filter((col) => col.type?.kind === 'arg_set_id');
  }

  // Fetch available arg keys for the given arg_set_id column.
  // Uses the primary input's materialized table from the execution service
  // to avoid creating a competing summarizer (which would conflict with
  // the main QueryExecutionService's materialized tables).
  async fetchAvailableArgKeys(argSetIdCol: ColumnInfo): Promise<string[]> {
    const trace = this.context.trace;
    if (!trace) return [];

    const primaryInput = this.primaryInput;
    if (!primaryInput) return [];

    // Get the materialized table name for the primary input node
    const tableName = await this.context.getTableNameForNode?.(
      primaryInput.nodeId,
    );
    if (!tableName) return [];

    const argColName = argSetIdCol.name;
    const sql = `
      SELECT DISTINCT args.flat_key as key
      FROM ${tableName} data
      JOIN args ON args.arg_set_id = data.${argColName}
      ORDER BY key
    `;

    try {
      const result = await trace.engine.query(sql);
      const keys: string[] = [];
      const it = result.iter({key: STR});
      for (; it.valid(); it.next()) {
        keys.push(it.key);
      }
      return keys;
    } catch (e) {
      console.warn('fetchAvailableArgKeys: query failed', e);
      return [];
    }
  }

  getTitle(): string {
    return 'Add Columns';
  }

  // Check if the Apply button should be disabled in the join modal
  isApplyDisabled(): boolean {
    // When no rightNode exists, require table and columns selection
    if (!this.rightNode) {
      const selectedTable = this.attrs.selectedSuggestionTable;
      if (!selectedTable) return true;
      const selectedColumns =
        this.attrs.suggestionSelections?.[selectedTable] ?? [];
      if (selectedColumns.length === 0) return true;
      // Also disable if there are duplicate column name errors
      return this.getJoinColumnErrors(selectedColumns, true).length > 0;
    }
    // When rightNode exists, require both join columns to be specified
    if (!this.attrs.leftColumn || !this.attrs.rightColumn) {
      return true;
    }
    // Require columns to be selected
    if (
      !this.attrs.selectedColumns ||
      this.attrs.selectedColumns.length === 0
    ) {
      return true;
    }
    // Also disable if there are duplicate column name errors
    return (
      this.getJoinColumnErrors(this.attrs.selectedColumns, false).length > 0
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
      ? this.attrs.suggestionAliases
      : this.attrs.columnAliases;

    // Get effective names (alias or original name) for all selected columns
    const effectiveNames = new Map<string, string>();
    for (const col of selectedColumns) {
      const alias = aliasMap?.[col];
      effectiveNames.set(col, alias !== undefined ? alias : col);
    }

    // Check each column against source columns
    const sourceColNames = new Set(
      this.sourceCols.map((c) => c.alias ?? c.name),
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

  nodeDetails(): NodeDetailsAttrs {
    const hasConnectedNode = this.rightNode !== undefined;
    const hasComputedColumns = (this.attrs.computedColumns?.length ?? 0) > 0;
    const hasSelectedColumns =
      this.attrs.selectedColumns && this.attrs.selectedColumns.length > 0;

    if (!hasSelectedColumns && !hasComputedColumns) {
      return {
        content: NodeDetailsMessage('No columns added'),
      };
    }

    const items: m.Child[] = [];

    // Add joined columns
    if (hasConnectedNode && hasSelectedColumns) {
      items.push(m('div', 'Add columns from input node:'));
      const columns = [];
      for (const col of this.attrs.selectedColumns ?? []) {
        const alias = this.attrs.columnAliases?.[col];
        const displayName = alias || col;
        columns.push(displayName);
      }
      items.push(
        m('div', [
          ...columns.flatMap((c, i) =>
            i === 0 ? [ColumnName(c)] : [', ', ColumnName(c)],
          ),
        ]),
      );
    }

    // Add computed columns
    if (hasComputedColumns) {
      if (hasConnectedNode && hasSelectedColumns) {
        items.push(m('.pf-section-spacer'));
      }
      items.push(m('div', 'Add computed columns:'));
      for (const col of this.attrs.computedColumns ?? []) {
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

        items.push(m('div', [ColumnName(name), `: ${description}`]));
      }
    }

    return {
      content: m('div', items),
    };
  }

  nodeSpecificModify(): NodeModifyAttrs {
    const hasConnectedNode = this.rightNode !== undefined;
    const argSetIdCols = this.getArgSetIdColumns();
    const hasArgSetId = argSetIdCols.length > 0;

    // Build sections
    const sections: NodeModifyAttrs['sections'] = [
      {
        content: m(
          '.pf-exp-action-buttons',
          m(Button, {
            label: 'From another source',
            icon: 'table_chart',
            onclick: () => this.showJoinModal(),
            variant: hasConnectedNode
              ? ButtonVariant.Filled
              : ButtonVariant.Outlined,
          }),
          m(Button, {
            label: 'Expression',
            icon: 'functions',
            onclick: () => this.showExpressionModal(),
            variant: ButtonVariant.Outlined,
          }),
          m(Button, {
            label: 'Switch',
            icon: 'alt_route',
            onclick: () => this.showSwitchModal(),
            variant: ButtonVariant.Outlined,
          }),
          m(Button, {
            label: 'If',
            icon: 'rule',
            onclick: () => this.showIfModal(),
            variant: ButtonVariant.Outlined,
          }),
          m(Button, {
            label: 'From args',
            icon: 'list',
            onclick: () => this.showArgsModal(),
            variant: ButtonVariant.Outlined,
            disabled: !hasArgSetId,
            title: hasArgSetId
              ? 'Add a column from args'
              : 'Source must have an arg_set_id column',
          }),
          m(Button, {
            label: 'Apply function',
            icon: 'function',
            onclick: () => this.showFunctionModal(),
            variant: ButtonVariant.Outlined,
            title: 'Apply a stdlib function to create a new column',
          }),
        ),
      },
      {
        content: this.renderAddedColumnsList(),
      },
    ];

    return {
      info: 'Add new columns to your query using expressions, joins, conditional logic, or by extracting values from args. Use the buttons above to select how you want to add columns.',
      sections,
    };
  }

  private showJoinModal() {
    const modalKey = 'add-join-modal';

    showModal({
      title: this.rightNode
        ? 'Configure Joined Columns'
        : 'Add Columns from Another Source',
      key: modalKey,
      vAlign: 'TOP',
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
            if (!this.rightNode && this.attrs.selectedSuggestionTable) {
              const suggestions = this.getJoinSuggestions();
              const selectedSuggestion = suggestions.find(
                (s) => s.suggestedTable === this.attrs.selectedSuggestionTable,
              );
              const selectedColumns =
                this.attrs.suggestionSelections?.[
                  this.attrs.selectedSuggestionTable
                ] ?? [];

              if (selectedSuggestion && selectedColumns.length > 0) {
                if (this.context.actions?.onAddAndConnectTable) {
                  this.attrs.isGuidedConnection = true;
                  this.context.actions.onAddAndConnectTable(
                    selectedSuggestion.suggestedTable,
                    0,
                  );
                  this.attrs.leftColumn = selectedSuggestion.colName;
                  this.attrs.rightColumn = selectedSuggestion.targetColumn;
                  this.attrs.selectedColumns = [...selectedColumns];
                  // Copy suggestion aliases to column aliases
                  if (this.attrs.suggestionAliases) {
                    if (!this.attrs.columnAliases) {
                      this.attrs.columnAliases = {};
                    }
                    for (const col of selectedColumns) {
                      const alias = this.attrs.suggestionAliases[col];
                      if (alias) {
                        this.attrs.columnAliases[col] = alias;
                      }
                    }
                  }
                }
              }
            }
            this.context.onchange?.();
          },
        },
      ],
    });
  }

  private showComputedColumnModal(
    type: 'expression' | 'switch' | 'if',
    columnIndex?: number,
  ) {
    const isEditing = columnIndex !== undefined;
    const typeConfig = {
      expression: {
        key: 'add-expression-modal',
        title: isEditing ? 'Edit Expression Column' : 'Add Expression Column',
        defaultColumn: {expression: '', name: ''},
      },
      switch: {
        key: 'add-switch-modal',
        title: isEditing ? 'Edit Switch Column' : 'Add Switch Column',
        defaultColumn: {
          type: 'switch' as const,
          expression: '',
          name: '',
          cases: [],
        },
      },
      if: {
        key: 'add-if-modal',
        title: isEditing ? 'Edit If Column' : 'Add If Column',
        defaultColumn: {
          type: 'if' as const,
          expression: '',
          name: '',
          clauses: [{if: '', then: ''}],
        },
      },
    }[type];

    // Create a temporary copy to work with in the modal
    let tempColumn: NewColumn;
    if (isEditing && this.attrs.computedColumns?.[columnIndex]) {
      const source = this.attrs.computedColumns[columnIndex];
      tempColumn = {
        ...source,
        cases: source.cases?.map((c) => ({...c})),
        clauses: source.clauses?.map((c) => ({...c})),
      };
    } else {
      tempColumn = typeConfig.defaultColumn;
    }

    showModal({
      title: typeConfig.title,
      key: typeConfig.key,
      vAlign: 'TOP',
      className:
        type === 'switch' || type === 'if'
          ? 'pf-computed-column-modal-wide'
          : undefined,
      content: () => {
        const nameError = this.getColumnNameError(tempColumn.name, columnIndex);
        return this.renderComputedColumn(tempColumn, nameError);
      },
      buttons: [
        {
          text: 'Cancel',
          action: () => {},
        },
        {
          text: isEditing ? 'Save' : 'Add',
          primary: true,
          disabled: () =>
            !this.isComputedColumnValid(tempColumn) ||
            this.getColumnNameError(tempColumn.name, columnIndex) !== undefined,
          action: () => {
            if (isEditing && columnIndex !== undefined) {
              const newComputedColumns = [
                ...(this.attrs.computedColumns ?? []),
              ];
              newComputedColumns[columnIndex] = tempColumn;
              this.attrs.computedColumns = newComputedColumns;
            } else {
              this.attrs.computedColumns = [
                ...(this.attrs.computedColumns ?? []),
                tempColumn,
              ];
            }
            this.context.onchange?.();
          },
        },
      ],
    });
  }

  private showExpressionModal(columnIndex?: number) {
    this.showComputedColumnModal('expression', columnIndex);
  }

  private showSwitchModal(columnIndex?: number) {
    this.showComputedColumnModal('switch', columnIndex);
  }

  private showIfModal(columnIndex?: number) {
    this.showComputedColumnModal('if', columnIndex);
  }

  private showArgsModal() {
    const modalKey = 'add-args-modal';
    const argSetIdCols = this.getArgSetIdColumns();

    if (argSetIdCols.length === 0) {
      console.warn(
        'Cannot show args modal: no arg_set_id columns found in input',
      );
      return;
    }

    // State for the modal
    let isLoading = false;
    let availableKeys: string[] = [];
    let selectedKey: string | undefined;
    let columnName = '';
    // For multiple arg_set_id columns, let user select which one to use
    let selectedArgSetIdCol: ColumnInfo | undefined =
      argSetIdCols.length === 1 ? argSetIdCols[0] : undefined;

    const fetchKeysForColumn = (col: ColumnInfo) => {
      isLoading = true;
      availableKeys = [];
      selectedKey = undefined;
      columnName = '';
      redrawModal();

      this.fetchAvailableArgKeys(col).then((keys) => {
        isLoading = false;
        availableKeys = keys;
        redrawModal();
      });
    };

    // If only one column, fetch keys immediately
    if (selectedArgSetIdCol) {
      fetchKeysForColumn(selectedArgSetIdCol);
    }

    const getColumnNameError = (): string | undefined => {
      if (!columnName.trim()) return undefined;
      return this.getColumnNameError(columnName.trim());
    };

    const isValid = (): boolean => {
      return (
        selectedArgSetIdCol !== undefined &&
        selectedKey !== undefined &&
        columnName.trim() !== '' &&
        getColumnNameError() === undefined
      );
    };

    const getArgSetIdColDisplayName = (col: ColumnInfo): string => {
      return col.alias ?? col.name;
    };

    showModal({
      title: 'Add Column from Args',
      key: modalKey,
      vAlign: 'TOP',
      content: () => {
        const nameError = getColumnNameError();
        const hasMultipleArgSetIdCols = argSetIdCols.length > 1;

        // Show column selector if there are multiple arg_set_id columns
        const columnSelector = hasMultipleArgSetIdCols
          ? m(FormSection, {label: 'Arg Set ID Column'}, [
              m(
                Select,
                {
                  onchange: (e: Event) => {
                    const value = (e.target as HTMLSelectElement).value;
                    selectedArgSetIdCol = argSetIdCols.find(
                      (col) => col.name === value,
                    );
                    if (selectedArgSetIdCol) {
                      fetchKeysForColumn(selectedArgSetIdCol);
                    } else {
                      availableKeys = [];
                      selectedKey = undefined;
                      columnName = '';
                      redrawModal();
                    }
                  },
                },
                m(
                  'option',
                  {value: '', selected: !selectedArgSetIdCol},
                  'Select a column',
                ),
                argSetIdCols.map((col) =>
                  m(
                    'option',
                    {
                      value: col.name,
                      selected: col === selectedArgSetIdCol,
                    },
                    getArgSetIdColDisplayName(col),
                  ),
                ),
              ),
            ])
          : null;

        // Show loading state
        if (isLoading) {
          return m(
            Form,
            columnSelector,
            m(
              '.pf-args-loading',
              m(Spinner),
              m('span', 'Loading available args...'),
            ),
          );
        }

        // If no column selected yet (multiple columns case)
        if (!selectedArgSetIdCol) {
          return m(
            Form,
            columnSelector,
            m(
              'p',
              'Select which arg_set_id column to use for extracting args.',
            ),
          );
        }

        // No args found - but still allow manual entry
        const noArgsFound = availableKeys.length === 0;

        return m(
          Form,
          nameError && m(Callout, {icon: 'error'}, nameError),
          noArgsFound &&
            m(
              Callout,
              {icon: 'info'},
              'No args found for the current data. You can still manually enter an arg key to fetch.',
            ),
          !noArgsFound &&
            m(
              'p',
              'Select an arg key to add as a column. The column will contain the value of that arg for each row.',
            ),
          columnSelector,
          m(FormSection, {label: 'Arg Key'}, [
            noArgsFound
              ? m(TextInput, {
                  placeholder: 'Enter arg key (e.g., display_frame_token)',
                  value: selectedKey || '',
                  oninput: (e: Event) => {
                    const value = (e.target as HTMLInputElement).value;
                    selectedKey = value || undefined;
                    // Auto-generate column name from key (replace special chars)
                    if (selectedKey && !columnName) {
                      columnName = selectedKey
                        .replace(/[^a-zA-Z0-9_]/g, '_')
                        .replace(/_+/g, '_')
                        .replace(/^_|_$/g, '');
                    }
                    redrawModal();
                  },
                })
              : m(
                  Select,
                  {
                    onchange: (e: Event) => {
                      const value = (e.target as HTMLSelectElement).value;
                      selectedKey = value || undefined;
                      // Auto-generate column name from key (replace special chars)
                      if (selectedKey && !columnName) {
                        columnName = selectedKey
                          .replace(/[^a-zA-Z0-9_]/g, '_')
                          .replace(/_+/g, '_')
                          .replace(/^_|_$/g, '');
                      }
                      redrawModal();
                    },
                  },
                  m(
                    'option',
                    {value: '', selected: !selectedKey},
                    'Select an arg key',
                  ),
                  availableKeys.map((key) =>
                    m(
                      'option',
                      {value: key, selected: key === selectedKey},
                      key,
                    ),
                  ),
                ),
          ]),
          m(FormSection, {label: 'Column Name'}, [
            m(TextInput, {
              placeholder: 'Enter column name',
              value: columnName,
              oninput: (e: Event) => {
                columnName = (e.target as HTMLInputElement).value;
                redrawModal();
              },
            }),
          ]),
        );
      },
      buttons: [
        {
          text: 'Cancel',
          action: () => {},
        },
        {
          text: 'Add',
          primary: true,
          disabled: () => !isValid(),
          action: () => {
            if (!isValid() || !selectedKey || !selectedArgSetIdCol) return;

            const argSetIdColName = selectedArgSetIdCol.name;
            // Create expression using extract_arg
            const expression = `extract_arg(${argSetIdColName}, ${sqliteString(selectedKey)})`;

            const newColumn: NewColumn = {
              expression,
              name: columnName.trim(),
            };

            this.attrs.computedColumns = [
              ...(this.attrs.computedColumns ?? []),
              newColumn,
            ];
            this.context.onchange?.();
            closeModal(modalKey);
          },
        },
      ],
    });
  }

  private showFunctionModal(columnIndex?: number) {
    const modalKey = 'add-function-modal';
    const isEditing = columnIndex !== undefined;
    const existingColumn = isEditing
      ? this.attrs.computedColumns?.[columnIndex]
      : undefined;

    // Create modal state using the helper
    const modalState: FunctionModalState = createFunctionModalState(
      isEditing,
      existingColumn,
      this.context.sqlModules,
    );

    // Generate a unique column name by appending a number suffix if needed
    const generateUniqueColumnName = (baseName: string): string => {
      let candidate = baseName;
      let suffix = 2;
      while (this.getColumnNameError(candidate, columnIndex) !== undefined) {
        candidate = `${baseName}_${suffix}`;
        suffix++;
      }
      return candidate;
    };

    const getColumnNameError = (name: string): string | undefined => {
      if (!name.trim()) return undefined;
      return this.getColumnNameError(name.trim(), columnIndex);
    };

    const handleFunctionSelect = (fnWithModule: FunctionWithModule) => {
      modalState.selectedFunctionWithModule = fnWithModule;
      // Initialize arg bindings
      modalState.argBindings = fnWithModule.fn.args.map((arg) => ({
        argName: arg.name,
        value: '',
        isCustomExpression: false,
      }));
      // Auto-generate unique column name
      modalState.columnName = generateUniqueColumnName(
        fnWithModule.fn.name.toLowerCase(),
      );
      modalState.step = 'configure';
      // Re-show modal to update buttons (redrawModal only updates content)
      showModalForStep();
    };

    const showModalForStep = () => {
      showModal({
        title:
          modalState.step === 'select'
            ? 'Select a Function'
            : isEditing
              ? 'Edit Function Column'
              : `Configure: ${modalState.selectedFunctionWithModule?.fn.name}`,
        key: modalKey,
        vAlign: 'TOP',
        className: 'pf-function-selection-modal',
        content: () => {
          if (modalState.step === 'select') {
            return m(FunctionSelectStep, {
              sqlModules: this.context.sqlModules!,
              searchQuery: modalState.searchQuery,
              selectedFunction: modalState.selectedFunctionWithModule?.fn.name,
              onSearchQueryChange: (query) => {
                modalState.searchQuery = query;
                redrawModal();
              },
              onFunctionSelect: handleFunctionSelect,
            });
          }

          // Configure step
          return m(FunctionConfigureStep, {
            selectedFunctionWithModule: modalState.selectedFunctionWithModule!,
            argBindings: modalState.argBindings,
            columnName: modalState.columnName,
            columnNameError: getColumnNameError(modalState.columnName),
            sourceCols: this.sourceCols,
            onArgBindingChange: (argIndex, binding) => {
              modalState.argBindings[argIndex] = binding;
              redrawModal();
            },
            onColumnNameChange: (name) => {
              modalState.columnName = name;
              redrawModal();
            },
          });
        },
        buttons: [
          // Back button only in configure step
          ...(modalState.step === 'configure'
            ? [
                {
                  text: 'Back',
                  action: () => {
                    modalState.step = 'select';
                    showModalForStep();
                  },
                },
              ]
            : []),
          {
            text: 'Cancel',
            action: () => {},
          },
          {
            text: isEditing ? 'Save' : 'Add',
            primary: true,
            disabled: () =>
              !isFunctionModalValid(modalState, getColumnNameError),
            action: () => {
              const newColumn = createFunctionColumn(modalState);
              if (!newColumn) return;

              if (isEditing && columnIndex !== undefined) {
                const newComputedColumns = [
                  ...(this.attrs.computedColumns ?? []),
                ];
                newComputedColumns[columnIndex] = newColumn;
                this.attrs.computedColumns = newComputedColumns;
              } else {
                this.attrs.computedColumns = [
                  ...(this.attrs.computedColumns ?? []),
                  newColumn,
                ];
              }
              this.context.onchange?.();
              closeModal(modalKey);
            },
          },
        ],
      });
    };

    showModalForStep();
  }

  private renderAddedColumnsList(): m.Child {
    const hasConnectedNode = this.rightNode !== undefined;
    const hasComputedColumns = (this.attrs.computedColumns?.length ?? 0) > 0;
    const hasSelectedColumns =
      this.attrs.selectedColumns && this.attrs.selectedColumns.length > 0;

    if (!hasSelectedColumns && !hasComputedColumns) {
      return m(EmptyState, {
        title: 'No columns added yet. Use the buttons above to add columns.',
      });
    }

    const items: m.Child[] = [];

    // Show joined columns as a single row
    if (hasConnectedNode && hasSelectedColumns) {
      items.push(this.renderJoinedColumnsRow());
    }

    // Show computed columns
    for (const [index, col] of (this.attrs.computedColumns ?? []).entries()) {
      const icon =
        col.type === 'switch'
          ? 'alt_route'
          : col.type === 'if'
            ? 'help_outline'
            : col.type === 'function'
              ? 'function'
              : 'functions';
      const typeName =
        col.type === 'switch'
          ? 'Switch'
          : col.type === 'if'
            ? 'If'
            : col.type === 'function'
              ? 'Function'
              : 'Expression';

      // Show the expression/preview for the column
      const description =
        col.type === 'switch' || col.type === 'if'
          ? typeName
          : col.type === 'function'
            ? `${typeName}: ${col.functionName ?? col.expression}`
            : col.expression
              ? `${typeName}: ${col.expression}`
              : `${typeName} (empty)`;

      items.push(
        this.renderComputedColumnListItem(col, index, icon, description),
      );
    }

    return m('.pf-added-columns-list', items);
  }

  private renderJoinedColumnsRow(): m.Child {
    const count = this.attrs.selectedColumns?.length ?? 0;
    const columnNames = (this.attrs.selectedColumns ?? [])
      .map((colName) => {
        const alias = this.attrs.columnAliases?.[colName];
        return alias || colName;
      })
      .join(', ');

    return m(
      '.pf-exp-list-item',
      {
        tabindex: 0,
        role: 'listitem',
      },
      m(Icon, {icon: 'table_chart'}),
      m(
        '.pf-exp-list-item-info',
        m(
          '.pf-exp-list-item-name',
          `${count} column${count === 1 ? '' : 's'} from source`,
        ),
        m('.pf-exp-list-item-description', columnNames),
      ),
      m(
        '.pf-exp-list-item-actions',
        m(Button, {
          label: 'Edit',
          icon: 'edit',
          variant: ButtonVariant.Outlined,
          compact: true,
          onclick: () => this.showJoinModal(),
        }),
        m(Button, {
          icon: 'close',
          compact: true,
          onclick: () => {
            this.attrs.selectedColumns = [];
            this.attrs.columnAliases = {};
            this.attrs.columnTypes = {};
            this.context.onchange?.();
          },
          title: 'Remove all joined columns',
        }),
      ),
    );
  }

  private renderComputedColumnListItem(
    col: NewColumn,
    index: number,
    icon: string,
    description: string,
  ): m.Child {
    // Create a ColumnInfo-like object for renderTypeSelector
    const colInfo: ColumnInfo = {
      name: col.name || '(unnamed)',
      checked: true,
      type: col.sqlType,
    };

    const handleTypeChange = (_index: number, newType: PerfettoSqlType) => {
      if (!this.attrs.computedColumns) return;
      const newComputedColumns = [...this.attrs.computedColumns];
      newComputedColumns[index] = {
        ...newComputedColumns[index],
        sqlType: newType,
      };
      this.attrs.computedColumns = newComputedColumns;
      this.context.onchange?.();
    };

    return m(
      '.pf-exp-list-item',
      {
        tabindex: 0,
        role: 'listitem',
      },
      m(Icon, {icon}),
      m(
        '.pf-exp-list-item-info',
        m('.pf-exp-list-item-name', col.name || '(unnamed)'),
        m('.pf-exp-list-item-description', description),
      ),
      m(
        '.pf-exp-list-item-actions',
        renderTypeSelector(
          colInfo,
          index,
          this.context.sqlModules,
          handleTypeChange,
        ),
        m(Button, {
          label: 'Edit',
          icon: 'edit',
          variant: ButtonVariant.Outlined,
          compact: true,
          onclick: () => {
            if (col.type === 'switch') {
              this.showSwitchModal(index);
            } else if (col.type === 'if') {
              this.showIfModal(index);
            } else if (col.type === 'function') {
              this.showFunctionModal(index);
            } else {
              this.showExpressionModal(index);
            }
          },
        }),
        m(Button, {
          icon: 'close',
          compact: true,
          onclick: () => {
            this.attrs.computedColumns?.splice(index, 1);
            this.context.onchange?.();
          },
          title: 'Remove item',
        }),
      ),
    );
  }

  private renderGuidedMode(): m.Child {
    return !this.rightNode
      ? this.renderSuggestionMode()
      : this.renderJoinConfiguration();
  }

  private renderSuggestionMode(): m.Child {
    const suggestions = this.getJoinSuggestions();
    const selectedTable = this.attrs.selectedSuggestionTable;
    const selectedColumns = selectedTable
      ? this.attrs.suggestionSelections?.[selectedTable] ?? []
      : [];

    return m(AddColumnsSuggestionModal, {
      suggestions,
      sourceCols: this.sourceCols,
      selectedTable,
      selectedColumns,
      suggestionAliases: this.attrs.suggestionAliases,
      getTable: (tableName: string) => this.getTable(tableName),
      getJoinColumnErrors: (cols: string[]) =>
        this.getJoinColumnErrors(cols, true),
      onTableSelect: (tableName: string | undefined) => {
        this.attrs.selectedSuggestionTable = tableName;
        m.redraw();
      },
      onColumnToggle: (colName: string, checked: boolean) => {
        if (!selectedTable) return;
        if (!this.attrs.suggestionSelections) {
          this.attrs.suggestionSelections = {};
        }
        const current = this.attrs.suggestionSelections[selectedTable] ?? [];
        let updated = [...current];
        if (checked) {
          if (!updated.includes(colName)) {
            updated.push(colName);
          }
        } else {
          updated = updated.filter((c) => c !== colName);
          if (this.attrs.suggestionAliases) {
            delete this.attrs.suggestionAliases[colName];
          }
        }
        this.attrs.suggestionSelections[selectedTable] = updated;
        redrawModal();
      },
      onColumnAlias: (colName: string, alias: string) => {
        if (!this.attrs.suggestionAliases) {
          this.attrs.suggestionAliases = {};
        }
        if (alias.trim() === '') {
          delete this.attrs.suggestionAliases[colName];
        } else {
          this.attrs.suggestionAliases[colName] = alias;
        }
        redrawModal();
      },
    });
  }

  private renderJoinConfiguration(): m.Child {
    const selectedColumns = this.attrs.selectedColumns ?? [];

    return m(AddColumnsConfigurationModal, {
      sourceCols: this.sourceCols,
      rightCols: this.rightCols,
      leftColumn: this.attrs.leftColumn,
      rightColumn: this.attrs.rightColumn,
      selectedColumns,
      columnAliases: this.attrs.columnAliases,
      getJoinColumnErrors: (cols: string[]) =>
        this.getJoinColumnErrors(cols, false),
      onLeftColumnChange: (columnName: string) => {
        this.attrs.leftColumn = columnName;
        redrawModal();
      },
      onRightColumnChange: (columnName: string) => {
        this.attrs.rightColumn = columnName;
        redrawModal();
      },
      onColumnToggle: (colName: string, checked: boolean) => {
        if (!this.attrs.selectedColumns) {
          this.attrs.selectedColumns = [];
        }
        if (checked) {
          if (!this.attrs.selectedColumns.includes(colName)) {
            this.attrs.selectedColumns.push(colName);
          }
        } else {
          this.attrs.selectedColumns = this.attrs.selectedColumns.filter(
            (c) => c !== colName,
          );
          if (this.attrs.columnAliases) {
            delete this.attrs.columnAliases[colName];
          }
          if (this.attrs.columnTypes) delete this.attrs.columnTypes[colName];
        }
        redrawModal();
      },
      onColumnAlias: (colName: string, alias: string) => {
        if (!this.attrs.columnAliases) {
          this.attrs.columnAliases = {};
        }
        if (alias.trim() === '') {
          delete this.attrs.columnAliases[colName];
        } else {
          this.attrs.columnAliases[colName] = alias;
        }
        redrawModal();
      },
    });
  }

  private renderComputedColumn(col: NewColumn, nameError?: string): m.Child {
    if (col.type === 'switch') {
      return m(
        Form,
        m(FormSection, {label: 'Column Name'}, [
          m(TextInput, {
            placeholder: 'Enter column name',
            value: col.name,
            oninput: (e: Event) => {
              col.name = (e.target as HTMLInputElement).value;
              redrawModal();
            },
          }),
          nameError && m(Callout, {icon: 'error'}, nameError),
        ]),
        m(SwitchComponent, {
          column: col,
          columns: this.sourceCols,
          onchange: () => {
            // No-op in modal mode - changes are already in col
          },
        }),
      );
    }

    if (col.type === 'if') {
      return m(
        Form,
        m(FormSection, {label: 'Column Name'}, [
          m(TextInput, {
            placeholder: 'Enter column name',
            value: col.name,
            oninput: (e: Event) => {
              col.name = (e.target as HTMLInputElement).value;
              redrawModal();
            },
          }),
          nameError && m(Callout, {icon: 'error'}, nameError),
        ]),
        m(IfComponent, {
          column: col,
          onchange: () => {
            // No-op in modal mode - changes are already in col
          },
        }),
      );
    }

    return m(
      Form,
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
        nameError && m(Callout, {icon: 'error'}, nameError),
      ]),
    );
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('add_columns');
  }

  validate(): boolean {
    // Clear any previous errors at the start of validation
    if (this.context.issues) {
      this.context.issues.clear();
    }

    if (this.primaryInput === undefined) {
      setValidationError(this.context, 'No input node connected');
      return false;
    }

    if (!this.primaryInput.validate()) {
      setValidationError(this.context, 'Previous node is invalid');
      return false;
    }

    // If there's a rightNode, validate it and the join configuration
    if (this.rightNode) {
      if (!this.rightNode.validate()) {
        setValidationError(
          this.context,
          this.rightNode.context.issues?.queryError?.message ??
            `Lookup table node '${this.rightNode.getTitle()}' is invalid`,
        );
        return false;
      }

      // We need valid join columns
      if (!this.attrs.leftColumn || !this.attrs.rightColumn) {
        setValidationError(
          this.context,
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
    const attrsCopy: AddColumnsNodeAttrs = {
      selectedColumns: this.attrs.selectedColumns
        ? [...this.attrs.selectedColumns]
        : undefined,
      leftColumn: this.attrs.leftColumn,
      rightColumn: this.attrs.rightColumn,
      suggestionSelections: this.attrs.suggestionSelections
        ? {...this.attrs.suggestionSelections}
        : undefined,
      expandedSuggestions: this.attrs.expandedSuggestions
        ? [...this.attrs.expandedSuggestions]
        : undefined,
      selectedSuggestionTable: this.attrs.selectedSuggestionTable,
      columnAliases: this.attrs.columnAliases
        ? {...this.attrs.columnAliases}
        : undefined,
      suggestionAliases: this.attrs.suggestionAliases
        ? {...this.attrs.suggestionAliases}
        : undefined,
      columnTypes: this.attrs.columnTypes
        ? {...this.attrs.columnTypes}
        : undefined,
      isGuidedConnection: this.attrs.isGuidedConnection,
      computedColumns: this.attrs.computedColumns?.map((col) => ({
        ...col,
        cases: col.cases?.map((c) => ({...c})),
        clauses: col.clauses?.map((c) => ({...c})),
      })),
    };
    return new AddColumnsNode(attrsCopy, {
      onchange: this.context.onchange,
      sqlModules: this.context.sqlModules,
    });
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return undefined;
    if (this.primaryInput === undefined) return undefined;

    // If there's no rightNode, we only add computed columns (no JOIN)
    if (!this.rightNode) {
      const computedColumns: ColumnSpec[] = [];
      for (const col of this.attrs.computedColumns ?? []) {
        if (!this.isComputedColumnValid(col)) continue;
        computedColumns.push({
          columnNameOrExpression: col.expression,
          alias: col.name,
          referencedModule: col.module,
        });
      }

      // If there are no computed columns, return passthrough to maintain reference chain
      if (computedColumns.length === 0) {
        return StructuredQueryBuilder.passthrough(
          this.primaryInput,
          this.nodeId,
        );
      }

      // Build column specifications including existing columns and computed columns
      const allColumns: ColumnSpec[] = [
        ...this.sourceCols.map((col) => ({
          columnNameOrExpression: col.name,
          alias: col.name, // Explicitly set alias to avoid protobuf empty string default
        })),
        ...computedColumns,
      ];

      // Collect referenced modules
      const referencedModules = this.attrs.computedColumns
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

    // Prepare columns to add from the JOIN
    const joinColumns: ColumnSpec[] = (this.attrs.selectedColumns ?? []).map(
      (colName) => {
        const explicitAlias = this.attrs.columnAliases?.[colName];
        // Use explicit alias if provided, otherwise default to the column name
        const alias =
          explicitAlias && explicitAlias.trim() !== ''
            ? explicitAlias.trim()
            : colName;
        return {
          columnNameOrExpression: colName,
          alias: alias,
        };
      },
    );

    // Prepare computed columns (expressions)
    const computedColumns: ColumnSpec[] = [];
    for (const col of this.attrs.computedColumns ?? []) {
      if (!this.isComputedColumnValid(col)) continue;
      computedColumns.push({
        columnNameOrExpression: col.expression,
        alias: col.name,
        referencedModule: col.module,
      });
    }

    // Prepare join condition (if we have columns to join)
    let condition: JoinCondition | undefined;
    if (
      joinColumns.length > 0 &&
      this.attrs.leftColumn !== undefined &&
      this.attrs.rightColumn !== undefined
    ) {
      condition = {
        type: 'equality',
        leftColumn: this.attrs.leftColumn,
        rightColumn: this.attrs.rightColumn,
      };
    } else if (joinColumns.length > 0) {
      // If we have JOIN columns but no condition, this is an invalid state
      // Return passthrough to maintain reference chain
      return StructuredQueryBuilder.passthrough(this.primaryInput, this.nodeId);
    }

    // Collect referenced modules from computed columns
    const referencedModules = this.attrs.computedColumns
      ?.map((col) => col.module)
      .filter((mod): mod is string => mod !== undefined);

    // If no columns to add (neither JOIN columns nor computed columns), return passthrough
    if (joinColumns.length === 0 && computedColumns.length === 0) {
      return StructuredQueryBuilder.passthrough(this.primaryInput, this.nodeId);
    }

    // Get all base columns from the source (needed when we have JOIN or computed columns)
    const allBaseColumns: ColumnSpec[] = this.sourceCols.map((col) => ({
      columnNameOrExpression: col.name,
      alias: col.name, // Explicitly set alias to avoid protobuf empty string default
    }));

    // Use the builder to handle the complexity of composing JOIN + computed columns
    return StructuredQueryBuilder.withAddColumnsAndExpressions(
      this.primaryInput,
      this.rightNode,
      joinColumns,
      condition,
      computedColumns,
      allBaseColumns,
      referencedModules && referencedModules.length > 0
        ? referencedModules
        : undefined,
      this.nodeId,
    );
  }

  static deserializeState(
    serializedState: AddColumnsNodeAttrs,
  ): AddColumnsNodeAttrs {
    // Migrate columnTypes: apply legacyDeserializeType to each value.
    const columnTypes = serializedState.columnTypes
      ? Object.fromEntries(
          Object.entries(serializedState.columnTypes)
            .map(([k, v]) => [k, legacyDeserializeType(v)] as const)
            .filter((e): e is [string, PerfettoSqlType] => e[1] !== undefined),
        )
      : undefined;
    return {...serializedState, columnTypes};
  }
}
