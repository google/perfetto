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
  ModificationNode,
} from '../../query_node';
import {ColumnInfo, columnInfoFromName} from '../column_info';
import protos from '../../../../protos';
import m from 'mithril';
import {Card, CardStack} from '../../../../widgets/card';
import {MultiselectInput} from '../../../../widgets/multiselect_input';
import {Select} from '../../../../widgets/select';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {TextInput} from '../../../../widgets/text_input';
import {showModal} from '../../../../widgets/modal';
import {Switch} from '../../../../widgets/switch';
import {Icon} from '../../../../widgets/icon';
import {
  StructuredQueryBuilder,
  ColumnSpec,
  JoinCondition,
} from '../structured_query_builder';
import {setValidationError} from '../node_issues';
import {ColumnNameRow} from '../widgets';

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
          {style: {marginTop: '8px', marginBottom: '8px'}},
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
  prevNode: QueryNode;
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

  // Map from column name to its alias (for renaming added columns)
  columnAliases?: Map<string, string>;

  // Track if connection was made through guided suggestion
  isGuidedConnection?: boolean;

  // Computed columns (expressions, SWITCH, IF)
  computedColumns?: NewColumn[];
}

export class AddColumnsNode implements ModificationNode {
  readonly nodeId: string;
  readonly type = NodeType.kAddColumns;
  readonly prevNode: QueryNode;
  inputNodes?: (QueryNode | undefined)[];
  nextNodes: QueryNode[];
  readonly state: AddColumnsNodeState;

  constructor(state: AddColumnsNodeState) {
    this.nodeId = nextNodeId();
    this.state = state;
    this.prevNode = state.prevNode;
    this.inputNodes = [];
    this.nextNodes = [];
    this.state.selectedColumns = this.state.selectedColumns ?? [];
    this.state.leftColumn = this.state.leftColumn ?? 'id';
    this.state.rightColumn = this.state.rightColumn ?? 'id';
    this.state.autoExecute = this.state.autoExecute ?? false;
    this.state.suggestionSelections =
      this.state.suggestionSelections ?? new Map();
    this.state.expandedSuggestions =
      this.state.expandedSuggestions ?? new Set();
    this.state.columnAliases = this.state.columnAliases ?? new Map();
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
    return this.prevNode?.finalCols ?? [];
  }

  // Get the node connected to the left-side input port (for adding columns from)
  get rightNode(): QueryNode | undefined {
    return this.inputNodes?.[0];
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
    if (!this.state.sqlModules) return [];

    const table = this.state.sqlModules
      .listTables()
      .find((t) => t.name === tableName);
    if (!table) return [];

    return table.columns.map((c) => c.name);
  }

  getTitle(): string {
    return 'Add Columns';
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
        items.push(m('div', `${displayName}: column from input`));
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

      items.push(m('div', `${name}: ${description}`));
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
          action: () => {
            // If there's no rightNode, connect the first suggestion with selections
            if (!this.rightNode && this.state.suggestionSelections) {
              const suggestions = this.getJoinSuggestions();
              for (const s of suggestions) {
                const selectedColumns =
                  this.state.suggestionSelections.get(s.suggestedTable) ?? [];
                if (selectedColumns.length > 0) {
                  // Found a suggestion with selections - connect it
                  if (this.state.actions?.onAddAndConnectTable) {
                    this.state.isGuidedConnection = true;
                    this.state.actions.onAddAndConnectTable(
                      s.suggestedTable,
                      0,
                    );
                    this.state.leftColumn = s.colName;
                    this.state.rightColumn = s.targetColumn;
                    this.state.selectedColumns = [...selectedColumns];
                  }
                  break;
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
        return this.renderComputedColumn(tempColumn);
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
        return this.renderComputedColumn(tempColumn);
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
        return this.renderComputedColumn(tempColumn);
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
      m(
        '.pf-add-columns-actions-buttons',
        m(Button, {
          label: hasConnectedNode
            ? 'From another source ✓'
            : 'From another source',
          icon: 'table_chart',
          variant: ButtonVariant.Outlined,
          onclick: () => {
            this.showJoinModal();
          },
        }),
        m(Button, {
          label: 'Expression',
          icon: 'functions',
          variant: ButtonVariant.Outlined,
          onclick: () => {
            this.showExpressionModal();
          },
        }),
        m(Button, {
          label: 'Switch',
          icon: 'alt_route',
          variant: ButtonVariant.Outlined,
          onclick: () => {
            this.showSwitchModal();
          },
        }),
        m(Button, {
          label: 'If',
          icon: 'help_outline',
          variant: ButtonVariant.Outlined,
          onclick: () => {
            this.showIfModal();
          },
        }),
      ),
    );
  }

  private renderAddedColumnsList(): m.Child {
    const hasConnectedNode = this.rightNode !== undefined;
    const hasComputedColumns = (this.state.computedColumns?.length ?? 0) > 0;

    if (!hasConnectedNode && !hasComputedColumns) {
      return m(
        '.pf-added-columns-empty',
        'No columns added yet. Use the buttons above to add columns.',
      );
    }

    const items: m.Child[] = [];

    // Show joined columns
    if (hasConnectedNode) {
      items.push(
        m(
          '.pf-added-column-item.pf-joined-source',
          m(Icon, {icon: 'table_chart'}),
          m(
            '.pf-added-column-info',
            m('.pf-added-column-name', 'Joined Source'),
            m(
              '.pf-added-column-description',
              `${this.state.selectedColumns?.length ?? 0} selected columns`,
            ),
          ),
          m(Button, {
            label: 'Configure',
            icon: 'settings',
            variant: ButtonVariant.Outlined,
            compact: true,
            onclick: () => {
              this.showJoinModal();
            },
          }),
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
        m(
          '.pf-added-column-item',
          m(Icon, {icon}),
          m(
            '.pf-added-column-info',
            m('.pf-added-column-name', col.name || '(unnamed)'),
            m('.pf-added-column-description', description),
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
              } else {
                this.showExpressionModal(index);
              }
            },
          }),
        ),
      );
    }

    return m('.pf-added-columns-list', items);
  }

  private renderGuidedMode(): m.Child {
    if (!this.rightNode) {
      const suggestions = this.getJoinSuggestions();

      return m(
        'div',
        m(
          Card,
          m('h3', 'Join Suggestions'),
          suggestions.length > 0
            ? m(
                'div',
                {style: {display: 'flex', flexDirection: 'column', gap: '8px'}},
                [
                  m(
                    'p',
                    {style: {marginBottom: '8px', color: '#888'}},
                    'Based on your JOINID columns, you could join with:',
                  ),
                  suggestions.map((s) => {
                    const availableColumns = this.getTableColumns(
                      s.suggestedTable,
                    );
                    const selectedColumns =
                      this.state.suggestionSelections?.get(s.suggestedTable) ??
                      [];
                    const isExpanded =
                      this.state.expandedSuggestions?.has(s.suggestedTable) ??
                      false;

                    return m(
                      'div',
                      {
                        style: {
                          padding: '8px',
                          backgroundColor: 'rgba(255, 255, 255, 0.05)',
                          borderRadius: '4px',
                          display: 'flex',
                          flexDirection: 'column',
                          gap: '8px',
                        },
                      },
                      [
                        // Header row with table name and expand/collapse
                        m(
                          'div',
                          {
                            style: {
                              display: 'flex',
                              justifyContent: 'space-between',
                              alignItems: 'center',
                              gap: '8px',
                              cursor: 'pointer',
                              userSelect: 'none',
                            },
                            onclick: (e: MouseEvent) => {
                              // Don't toggle if clicking on the button
                              if (
                                (e.target as HTMLElement).closest('button') ||
                                (e.target as HTMLElement).tagName === 'BUTTON'
                              ) {
                                return;
                              }

                              if (!this.state.expandedSuggestions) {
                                this.state.expandedSuggestions = new Set();
                              }
                              if (isExpanded) {
                                this.state.expandedSuggestions.delete(
                                  s.suggestedTable,
                                );
                              } else {
                                this.state.expandedSuggestions.add(
                                  s.suggestedTable,
                                );
                              }
                              m.redraw();
                            },
                          },
                          [
                            m(
                              'span',
                              {
                                style: {
                                  fontFamily: 'monospace',
                                  fontSize: '12px',
                                  display: 'flex',
                                  alignItems: 'center',
                                  gap: '4px',
                                },
                              },
                              [
                                m(
                                  'span',
                                  {
                                    style: {
                                      fontSize: '16px',
                                      lineHeight: '1',
                                    },
                                  },
                                  isExpanded ? '▼' : '▶',
                                ),
                                m('strong', s.suggestedTable),
                                ' table (using ',
                                m('code', s.colName),
                                ' = ',
                                m('code', s.targetColumn),
                                ')',
                                selectedColumns.length > 0 &&
                                  m(
                                    'span',
                                    {
                                      style: {
                                        marginLeft: '8px',
                                        color: '#888',
                                        fontSize: '11px',
                                      },
                                    },
                                    `${selectedColumns.length} selected`,
                                  ),
                              ],
                            ),
                          ],
                        ),
                        // Column selection (only when expanded)
                        isExpanded &&
                          m(
                            'div',
                            {
                              style: {
                                marginTop: '4px',
                                paddingTop: '8px',
                                borderTop: '1px solid rgba(255, 255, 255, 0.1)',
                              },
                            },
                            [
                              m(
                                'div',
                                {
                                  style: {
                                    marginBottom: '8px',
                                    fontSize: '11px',
                                    color: '#888',
                                  },
                                },
                                `Select columns from ${s.suggestedTable} (${availableColumns.length} available):`,
                              ),
                              m(MultiselectInput, {
                                options: availableColumns.map((col) => ({
                                  key: col,
                                  label: col,
                                })),
                                selectedOptions: selectedColumns,
                                onOptionAdd: (key: string) => {
                                  if (!this.state.suggestionSelections) {
                                    this.state.suggestionSelections = new Map();
                                  }
                                  const current =
                                    this.state.suggestionSelections.get(
                                      s.suggestedTable,
                                    ) ?? [];
                                  this.state.suggestionSelections.set(
                                    s.suggestedTable,
                                    [...current, key],
                                  );
                                  m.redraw();
                                },
                                onOptionRemove: (key: string) => {
                                  if (this.state.suggestionSelections) {
                                    const current =
                                      this.state.suggestionSelections.get(
                                        s.suggestedTable,
                                      ) ?? [];
                                    this.state.suggestionSelections.set(
                                      s.suggestedTable,
                                      current.filter((c) => c !== key),
                                    );
                                    m.redraw();
                                  }
                                },
                              }),
                            ],
                          ),
                      ],
                    );
                  }),
                  m(
                    'p',
                    {
                      style: {
                        marginTop: '8px',
                        color: '#888',
                        fontSize: '12px',
                      },
                    },
                    'Connect a table node to the left port to add columns.',
                  ),
                ],
              )
            : m(
                'p',
                {style: {color: '#888'}},
                'No JOINID columns found in your data. You can still connect any node to the left port.',
              ),
        ),
      );
    }

    const leftCols = this.sourceCols;
    const rightCols = this.rightCols;

    return m('div', [
      m(
        CardStack,
        m(
          Card,
          m('h3', 'Select Columns to Add'),
          m(MultiselectInput, {
            options: rightCols.map((c) => ({
              key: c.column.name,
              label: c.column.name,
            })),
            selectedOptions: this.state.selectedColumns ?? [],
            onOptionAdd: (key: string) => {
              if (!this.state.selectedColumns) {
                this.state.selectedColumns = [];
              }
              this.state.selectedColumns.push(key);
              this.state.onchange?.();
              m.redraw();
            },
            onOptionRemove: (key: string) => {
              if (this.state.selectedColumns) {
                this.state.selectedColumns = this.state.selectedColumns.filter(
                  (c) => c !== key,
                );
                // Also remove the alias for this column
                this.state.columnAliases?.delete(key);
                this.state.onchange?.();
                m.redraw();
              }
            },
          }),
          // Show alias inputs for selected columns
          this.state.selectedColumns && this.state.selectedColumns.length > 0
            ? m(
                'div',
                {
                  style: {
                    paddingTop: '5px',
                    borderTop: '1px solid rgba(255, 255, 255, 0.1)',
                  },
                },
                [
                  m(
                    'h4',
                    {style: {marginBottom: '8px'}},
                    'Column Aliases (optional)',
                  ),
                  m(
                    'div',
                    {
                      style: {
                        fontSize: '11px',
                        color: '#888',
                        marginBottom: '8px',
                      },
                    },
                    'Rename columns by providing an alias:',
                  ),
                  this.state.selectedColumns.map((colName) =>
                    m(
                      '.pf-form-row',
                      {
                        style: {
                          display: 'flex',
                          alignItems: 'center',
                          gap: '8px',
                          marginBottom: '8px',
                        },
                      },
                      [
                        m(
                          'code',
                          {style: {minWidth: '120px', fontSize: '12px'}},
                          colName,
                        ),
                        m('span', '→'),
                        m(TextInput, {
                          placeholder: 'alias (optional)',
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
                      ],
                    ),
                  ),
                ],
              )
            : null,
        ),
        m(
          Card,
          m('h3', 'Join Condition'),
          m(
            '.pf-form-row',
            m('label', 'Base Column:'),
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
          ),
          m(
            '.pf-form-row',
            m('label', 'Connected Node Column:'),
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
          ),
        ),
      ),
    ]);
  }

  private renderComputedColumn(col: NewColumn): m.Child {
    if (col.type === 'switch') {
      return m(
        '.pf-exp-switch-wrapper',
        m(ColumnNameRow, {
          label: 'New switch column name',
          name: col.name,
          isValid: this.isComputedColumnValid(col),
          onNameChange: (name) => {
            col.name = name;
          },
          onRemove: () => {
            // No-op in modal mode
          },
        }),
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
        '.pf-exp-if-wrapper',
        m(ColumnNameRow, {
          label: 'New if column name',
          name: col.name,
          isValid: this.isComputedColumnValid(col),
          onNameChange: (name) => {
            col.name = name;
          },
          onRemove: () => {
            // No-op in modal mode
          },
        }),
        m(IfComponent, {
          column: col,
          onchange: () => {
            // No-op in modal mode - changes are already in col
          },
        }),
      );
    }

    const isValid = this.isComputedColumnValid(col);

    return m(
      'div',
      {style: {display: 'flex', flexDirection: 'column', gap: '16px'}},
      // Help text
      m(
        'div',
        {
          style: {
            padding: '12px',
            background: 'var(--background-color)',
            borderRadius: '4px',
            fontSize: '13px',
            color: 'var(--pf-text-color-secondary)',
          },
        },
        m(
          'div',
          {style: {marginBottom: '8px'}},
          'Create a computed column using any SQL expression.',
        ),
        m(
          'div',
          {style: {fontStyle: 'italic'}},
          'Example: ',
          m('code', 'dur / 1e6'),
          ' to convert duration to milliseconds',
        ),
      ),
      // Expression input
      m(
        'div',
        {style: {display: 'flex', flexDirection: 'column', gap: '8px'}},
        m(
          'label',
          {
            style: {
              fontSize: '14px',
              fontWeight: 600,
              color: 'var(--pf-text-color-primary)',
            },
          },
          'SQL Expression',
        ),
        m(TextInput, {
          oninput: (e: Event) => {
            col.expression = (e.target as HTMLInputElement).value;
          },
          placeholder:
            'Enter SQL expression (e.g., dur / 1e6, name || "_suffix")',
          value: col.expression,
        }),
      ),
      // Column name input
      m(
        'div',
        {style: {display: 'flex', flexDirection: 'column', gap: '8px'}},
        m(
          'label',
          {
            style: {
              fontSize: '14px',
              fontWeight: 600,
              color: 'var(--pf-text-color-primary)',
            },
          },
          'Column Name',
        ),
        m(TextInput, {
          oninput: (e: Event) => {
            col.name = (e.target as HTMLInputElement).value;
          },
          placeholder: 'Enter column name (e.g., dur_ms)',
          value: col.name,
        }),
      ),
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

    if (this.prevNode === undefined) {
      setValidationError(this.state, 'No input node connected');
      return false;
    }

    if (!this.prevNode.validate()) {
      setValidationError(this.state, 'Previous node is invalid');
      return false;
    }

    // Check if there are any valid computed columns
    const hasValidComputedColumns = this.state.computedColumns?.some((col) =>
      this.isComputedColumnValid(col),
    );

    // Require either a rightNode or valid computed columns
    if (!this.rightNode && !hasValidComputedColumns) {
      setValidationError(
        this.state,
        'No node connected to add columns from and no valid computed columns',
      );
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

    return true;
  }

  clone(): QueryNode {
    return new AddColumnsNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (!this.validate()) return undefined;

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
        return this.prevNode.getStructuredQuery();
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
        this.prevNode,
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
      return this.prevNode.getStructuredQuery();
    }

    // Prepare join condition
    const condition: JoinCondition = {
      type: 'equality',
      leftColumn: this.state.leftColumn!,
      rightColumn: this.state.rightColumn!,
    };

    return StructuredQueryBuilder.withAddColumns(
      this.prevNode,
      this.rightNode,
      inputColumns,
      condition,
      this.nodeId,
    );
  }

  serializeState(): object {
    return {
      selectedColumns: this.state.selectedColumns,
      leftColumn: this.state.leftColumn,
      rightColumn: this.state.rightColumn,
      suggestionSelections: this.state.suggestionSelections
        ? Object.fromEntries(this.state.suggestionSelections)
        : undefined,
      expandedSuggestions: this.state.expandedSuggestions
        ? Array.from(this.state.expandedSuggestions)
        : undefined,
      columnAliases: this.state.columnAliases
        ? Object.fromEntries(this.state.columnAliases)
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
      prevNode: undefined as unknown as QueryNode,
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
    };
  }
}
