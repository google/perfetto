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
} from '../../query_node';
import {getSecondaryInput} from '../graph_utils';
import {analyzeNode, isAQuery} from '../query_builder_utils';
import {
  ColumnInfo,
  columnInfoFromName,
  columnInfoFromSqlColumn,
} from '../column_info';
import {
  PerfettoSqlTypes,
  parsePerfettoSqlTypeFromString,
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
import {NodeModifyAttrs, NodeDetailsAttrs} from '../node_explorer_types';
import {NodeDetailsMessage, ColumnName} from '../node_styling_widgets';
import {Spinner} from '../../../../widgets/spinner';
import {STR} from '../../../../trace_processor/query_result';
import {sqliteString} from '../../../../base/string_utils';
import {loadNodeDoc} from '../node_doc_loader';
import {NewColumn, AddColumnsNodeState} from './add_columns_types';
import {SwitchComponent, IfComponent} from './computed_column_components';
import {AddColumnsSuggestionModal} from './add_columns_suggestion_modal';
import {AddColumnsConfigurationModal} from './add_columns_configuration_modal';
import {renderTypeSelector} from './modify_columns_utils';
import {DraggableItem} from '../widgets';
import {Icon} from '../../../../widgets/icon';

// Re-export types for backwards compatibility
export {NewColumn, AddColumnsNodeState} from './add_columns_types';

export class AddColumnsNode implements QueryNode {
  readonly nodeId: string;
  readonly type = NodeType.kAddColumns;
  primaryInput?: QueryNode;
  secondaryInputs: SecondaryInputSpec;
  nextNodes: QueryNode[];
  readonly state: AddColumnsNodeState;

  constructor(state: AddColumnsNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.secondaryInputs = {
      connections: new Map(),
      min: 1,
      max: 1,
      portNames: ['Table'],
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
    this.state.columnTypes = this.state.columnTypes ?? new Map();
    this.state.computedColumns = this.state.computedColumns ?? [];
  }

  // Called when a node is connected/disconnected to inputNodes
  onPrevNodesUpdated(): void {
    // If node is disconnected, reset everything
    if (!this.rightNode) {
      this.state.selectedColumns = [];
      this.state.isGuidedConnection = false;
      this.state.onchange?.();
      return;
    }

    // Check if the join column is still valid
    if (this.state.rightColumn) {
      const rightColExists = this.rightCols.some(
        (c) => c.column.name === this.state.rightColumn,
      );
      if (!rightColExists) {
        // Join column no longer exists - reset selection
        this.state.selectedColumns = [];
        this.state.rightColumn = undefined;
      }
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
      // Add only selected columns (with aliases and types if provided)
      const newCols =
        this.state.selectedColumns?.map((c) => {
          const alias = this.state.columnAliases?.get(c);
          const storedType = this.state.columnTypes?.get(c);

          // Find the column in rightCols to get type information
          const sourceCol = this.rightCols.find((col) => col.name === c);
          if (sourceCol) {
            // Use stored type if available, otherwise use source type
            let finalType = sourceCol.column.type;

            if (storedType) {
              // Parse the stored type string
              const parsedType = parsePerfettoSqlTypeFromString({
                type: storedType,
              });
              if (parsedType.ok) {
                finalType = parsedType.value;
              }
            }

            return columnInfoFromSqlColumn({
              name: alias ?? c,
              type: finalType,
            });
          }
          // Fallback if column not found (shouldn't happen in valid state)
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
            // Parse the stored type string and use it
            const parsedType = parsePerfettoSqlTypeFromString({
              type: col.sqlType,
            });
            if (!parsedType.ok) {
              console.warn(
                `Failed to parse stored type '${col.sqlType}' for column '${col.name}', defaulting to INT`,
              );
            }
            return columnInfoFromSqlColumn({
              name: col.name,
              type: parsedType.ok ? parsedType.value : PerfettoSqlTypes.INT,
            });
          }
          // Try to preserve type information if the expression is a simple column reference
          const sourceCol = this.sourceCols.find(
            (c) => c.column.name === col.expression,
          );
          if (sourceCol && sourceCol.column.type) {
            col.sqlType = sourceCol.type;
            return columnInfoFromSqlColumn({
              name: col.name,
              type: sourceCol.column.type,
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

  // Find all arg_set_id columns in source columns
  getArgSetIdColumns(): ColumnInfo[] {
    return this.sourceCols.filter(
      (col) => col.column.type?.kind === 'arg_set_id',
    );
  }

  // Fetch available arg keys for the given arg_set_id column
  async fetchAvailableArgKeys(argSetIdCol: ColumnInfo): Promise<string[]> {
    const trace = this.state.trace;
    if (!trace) return [];

    // We need to analyze the current node to get the SQL query
    // that includes the arg_set_id column
    const query = await analyzeNode(this, trace.engine);
    if (!isAQuery(query)) return [];

    // Query for distinct arg keys using the current data
    const argColName = argSetIdCol.column.name;
    const sql = `
      SELECT DISTINCT args.flat_key as key
      FROM (${query.sql}) data
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
    } catch {
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
      const selectedTable = this.state.selectedSuggestionTable;
      if (!selectedTable) return true;
      const selectedColumns =
        this.state.suggestionSelections?.get(selectedTable) ?? [];
      if (selectedColumns.length === 0) return true;
      // Also disable if there are duplicate column name errors
      return this.getJoinColumnErrors(selectedColumns, true).length > 0;
    }
    // When rightNode exists, require both join columns to be specified
    if (!this.state.leftColumn || !this.state.rightColumn) {
      return true;
    }
    // Require columns to be selected
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

  nodeDetails(): NodeDetailsAttrs {
    const hasConnectedNode = this.rightNode !== undefined;
    const hasComputedColumns = (this.state.computedColumns?.length ?? 0) > 0;
    const hasSelectedColumns =
      this.state.selectedColumns && this.state.selectedColumns.length > 0;

    if (!hasSelectedColumns && !hasComputedColumns) {
      return {
        content: NodeDetailsMessage('No columns added'),
      };
    }

    const items: m.Child[] = [];

    // Add joined columns
    if (hasConnectedNode && hasSelectedColumns) {
      for (const col of this.state.selectedColumns ?? []) {
        const alias = this.state.columnAliases?.get(col);
        const displayName = alias || col;
        items.push(m('div', [ColumnName(displayName), ': column from input']));
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

      items.push(m('div', [ColumnName(name), `: ${description}`]));
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
    if (isEditing && this.state.computedColumns?.[columnIndex]) {
      const source = this.state.computedColumns[columnIndex];
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
      return col.alias ?? col.column.name;
    };

    showModal({
      title: 'Add Column from Args',
      key: modalKey,
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
                      (col) => col.column.name === value,
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
                      value: col.column.name,
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
                        .replace(/[.\[\]]/g, '_')
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
                          .replace(/[.\[\]]/g, '_')
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

            const argSetIdColName = selectedArgSetIdCol.column.name;
            // Create expression using extract_arg
            const expression = `extract_arg(${argSetIdColName}, ${sqliteString(selectedKey)})`;

            const newColumn: NewColumn = {
              expression,
              name: columnName.trim(),
            };

            this.state.computedColumns = [
              ...(this.state.computedColumns ?? []),
              newColumn,
            ];
            this.state.onchange?.();
            closeModal(modalKey);
          },
        },
      ],
    });
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

    // Show individual joined columns
    if (hasConnectedNode && this.state.selectedColumns) {
      items.push(
        m(
          '.pf-add-columns-joined-list',
          this.state.selectedColumns.map((colName, index) =>
            this.renderJoinedColumn(colName, index),
          ),
        ),
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
        this.renderComputedColumnListItem(col, index, icon, description),
      );
    }

    return m('.pf-added-columns-list', items);
  }

  private renderJoinedColumn(colName: string, index: number): m.Child {
    const alias = this.state.columnAliases?.get(colName);
    const storedType = this.state.columnTypes?.get(colName);
    const sourceCol = this.rightCols.find((col) => col.name === colName);

    // Create ColumnInfo object for the type selector
    const colInfo: ColumnInfo = {
      name: colName,
      column: sourceCol?.column ?? {name: colName},
      type: storedType ?? sourceCol?.type ?? 'UNKNOWN',
      checked: true,
      alias,
    };

    const handleReorder = (from: number, to: number) => {
      if (!this.state.selectedColumns) return;
      const newSelectedColumns = [...this.state.selectedColumns];
      const [removed] = newSelectedColumns.splice(from, 1);
      newSelectedColumns.splice(to, 0, removed);
      this.state.selectedColumns = newSelectedColumns;
      this.state.onchange?.();
    };

    const handleTypeChange = (_index: number, newType: string) => {
      if (!this.state.columnTypes) {
        this.state.columnTypes = new Map();
      }
      this.state.columnTypes.set(colName, newType);
      this.state.onchange?.();
    };

    const handleRemove = () => {
      this.state.selectedColumns = this.state.selectedColumns?.filter(
        (c) => c !== colName,
      );
      this.state.columnAliases?.delete(colName);
      this.state.columnTypes?.delete(colName);
      this.state.onchange?.();
    };

    return m(
      DraggableItem,
      {
        index,
        onReorder: handleReorder,
      },
      m('.pf-column-name', colName),
      m(TextInput, {
        oninput: (e: Event) => {
          const inputValue = (e.target as HTMLInputElement).value;
          if (!this.state.columnAliases) {
            this.state.columnAliases = new Map();
          }
          if (inputValue.trim() === '') {
            this.state.columnAliases.delete(colName);
          } else {
            this.state.columnAliases.set(colName, inputValue);
          }
          this.state.onchange?.();
        },
        placeholder: 'alias',
        value: alias ?? '',
      }),
      renderTypeSelector(colInfo, index, handleTypeChange),
      m(Icon, {
        icon: 'close',
        className: 'pf-clickable',
        onclick: handleRemove,
      }),
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
      type: col.sqlType ?? 'UNKNOWN',
      checked: true,
      column: {name: col.name},
    };

    const handleTypeChange = (_index: number, newType: string) => {
      if (!this.state.computedColumns) return;
      const newComputedColumns = [...this.state.computedColumns];
      newComputedColumns[index] = {
        ...newComputedColumns[index],
        sqlType: newType,
      };
      this.state.computedColumns = newComputedColumns;
      this.state.onchange?.();
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
        renderTypeSelector(colInfo, index, handleTypeChange),
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
            } else {
              this.showExpressionModal(index);
            }
          },
        }),
        m(Button, {
          icon: 'close',
          compact: true,
          onclick: () => {
            this.state.computedColumns?.splice(index, 1);
            this.state.onchange?.();
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
    const selectedTable = this.state.selectedSuggestionTable;
    const selectedColumns = selectedTable
      ? this.state.suggestionSelections?.get(selectedTable) ?? []
      : [];

    return m(AddColumnsSuggestionModal, {
      suggestions,
      sourceCols: this.sourceCols,
      selectedTable,
      selectedColumns,
      suggestionAliases: this.state.suggestionAliases,
      getTable: (tableName: string) => this.getTable(tableName),
      getJoinColumnErrors: (cols: string[]) =>
        this.getJoinColumnErrors(cols, true),
      onTableSelect: (tableName: string | undefined) => {
        this.state.selectedSuggestionTable = tableName;
        m.redraw();
      },
      onColumnToggle: (colName: string, checked: boolean) => {
        if (!selectedTable) return;
        if (!this.state.suggestionSelections) {
          this.state.suggestionSelections = new Map();
        }
        const current =
          this.state.suggestionSelections.get(selectedTable) ?? [];
        let updated = [...current];
        if (checked) {
          if (!updated.includes(colName)) {
            updated.push(colName);
          }
        } else {
          updated = updated.filter((c) => c !== colName);
          this.state.suggestionAliases?.delete(colName);
        }
        this.state.suggestionSelections.set(selectedTable, updated);
        m.redraw();
      },
      onColumnAlias: (colName: string, alias: string) => {
        if (!this.state.suggestionAliases) {
          this.state.suggestionAliases = new Map();
        }
        if (alias.trim() === '') {
          this.state.suggestionAliases.delete(colName);
        } else {
          this.state.suggestionAliases.set(colName, alias);
        }
        m.redraw();
      },
    });
  }

  private renderJoinConfiguration(): m.Child {
    const selectedColumns = this.state.selectedColumns ?? [];

    return m(AddColumnsConfigurationModal, {
      sourceCols: this.sourceCols,
      rightCols: this.rightCols,
      leftColumn: this.state.leftColumn,
      rightColumn: this.state.rightColumn,
      selectedColumns,
      columnAliases: this.state.columnAliases,
      getJoinColumnErrors: (cols: string[]) =>
        this.getJoinColumnErrors(cols, false),
      onLeftColumnChange: (columnName: string) => {
        this.state.leftColumn = columnName;
        this.state.onchange?.();
      },
      onRightColumnChange: (columnName: string) => {
        this.state.rightColumn = columnName;
        this.state.onchange?.();
      },
      onColumnToggle: (colName: string, checked: boolean) => {
        if (!this.state.selectedColumns) {
          this.state.selectedColumns = [];
        }
        if (checked) {
          if (!this.state.selectedColumns.includes(colName)) {
            this.state.selectedColumns.push(colName);
          }
        } else {
          this.state.selectedColumns = this.state.selectedColumns.filter(
            (c) => c !== colName,
          );
          this.state.columnAliases?.delete(colName);
          this.state.columnTypes?.delete(colName);
        }
        this.state.onchange?.();
      },
      onColumnAlias: (colName: string, alias: string) => {
        if (!this.state.columnAliases) {
          this.state.columnAliases = new Map();
        }
        if (alias.trim() === '') {
          this.state.columnAliases.delete(colName);
        } else {
          this.state.columnAliases.set(colName, alias);
        }
        this.state.onchange?.();
      },
    });
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
    );
  }

  nodeInfo(): m.Children {
    return loadNodeDoc('add_columns');
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

    // If there's a rightNode, validate it and the join configuration
    if (this.rightNode) {
      if (!this.rightNode.validate()) {
        setValidationError(
          this.state,
          this.rightNode.state.issues?.queryError?.message ??
            `Lookup table node '${this.rightNode.getTitle()}' is invalid`,
        );
        return false;
      }

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
    const stateCopy: AddColumnsNodeState = {
      selectedColumns: this.state.selectedColumns
        ? [...this.state.selectedColumns]
        : undefined,
      leftColumn: this.state.leftColumn,
      rightColumn: this.state.rightColumn,
      suggestionSelections: this.state.suggestionSelections
        ? new Map(this.state.suggestionSelections)
        : undefined,
      expandedSuggestions: this.state.expandedSuggestions
        ? new Set(this.state.expandedSuggestions)
        : undefined,
      selectedSuggestionTable: this.state.selectedSuggestionTable,
      columnAliases: this.state.columnAliases
        ? new Map(this.state.columnAliases)
        : undefined,
      suggestionAliases: this.state.suggestionAliases
        ? new Map(this.state.suggestionAliases)
        : undefined,
      columnTypes: this.state.columnTypes
        ? new Map(this.state.columnTypes)
        : undefined,
      isGuidedConnection: this.state.isGuidedConnection,
      computedColumns: this.state.computedColumns?.map((col) => ({
        ...col,
        cases: col.cases?.map((c) => ({...c})),
        clauses: col.clauses?.map((c) => ({...c})),
      })),
      filters: this.state.filters?.map((f) => ({...f})),
      filterOperator: this.state.filterOperator,
      onchange: this.state.onchange,
    };
    return new AddColumnsNode(stateCopy);
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
          alias: col.column.name, // Explicitly set alias to avoid protobuf empty string default
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

    // Prepare columns to add from the JOIN
    const joinColumns: ColumnSpec[] = (this.state.selectedColumns ?? []).map(
      (colName) => {
        const explicitAlias = this.state.columnAliases?.get(colName);
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
    for (const col of this.state.computedColumns ?? []) {
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
      this.state.leftColumn !== undefined &&
      this.state.rightColumn !== undefined
    ) {
      condition = {
        type: 'equality',
        leftColumn: this.state.leftColumn,
        rightColumn: this.state.rightColumn,
      };
    } else if (joinColumns.length > 0) {
      // If we have JOIN columns but no condition, this is an invalid state
      // Fall back to just returning the base query
      return this.primaryInput.getStructuredQuery();
    }

    // Collect referenced modules from computed columns
    const referencedModules = this.state.computedColumns
      ?.map((col) => col.module)
      .filter((mod): mod is string => mod !== undefined);

    // Get all base columns from the source (needed when we have JOIN or computed columns)
    const allBaseColumns: ColumnSpec[] =
      joinColumns.length > 0 || computedColumns.length > 0
        ? this.sourceCols.map((col) => ({
            columnNameOrExpression: col.column.name,
            alias: col.column.name, // Explicitly set alias to avoid protobuf empty string default
          }))
        : [];

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
      columnTypes: this.state.columnTypes
        ? Object.fromEntries(this.state.columnTypes)
        : undefined,
      isGuidedConnection: this.state.isGuidedConnection,
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
      columnTypes:
        (serializedState.columnTypes as unknown as Record<string, string>) !==
        undefined
          ? new Map(
              Object.entries(
                serializedState.columnTypes as unknown as Record<
                  string,
                  string
                >,
              ),
            )
          : undefined,
    };
  }
}
