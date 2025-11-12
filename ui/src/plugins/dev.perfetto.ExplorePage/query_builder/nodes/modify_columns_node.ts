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
  notifyNextNodes,
  ModificationNode,
} from '../../query_node';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {Card, CardStack} from '../../../../widgets/card';
import {Checkbox} from '../../../../widgets/checkbox';
import {Icon} from '../../../../widgets/icon';
import {Select} from '../../../../widgets/select';
import {TextInput} from '../../../../widgets/text_input';
import {Switch} from '../../../../widgets/switch';
import {
  ColumnInfo,
  columnInfoFromName,
  newColumnInfoList,
} from '../column_info';
import protos from '../../../../protos';
import {NodeIssues} from '../node_issues';
import {StructuredQueryBuilder, ColumnSpec} from '../structured_query_builder';

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

    // Check if the selected column is a string type
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
  useGlob?: boolean; // Use GLOB instead of = for string matching

  // For if columns
  clauses?: IfClause[];
  elseValue?: string;

  // SQL type for preserving type information across serialization
  sqlType?: string;
}

export interface ModifyColumnsSerializedState {
  prevNodeId?: string;
  newColumns: NewColumn[];
  selectedColumns: {
    name: string;
    type: string;
    checked: boolean;
    alias?: string;
  }[];
  comment?: string;
}

export interface ModifyColumnsState extends QueryNodeState {
  prevNode: QueryNode;
  newColumns: NewColumn[];
  selectedColumns: ColumnInfo[];
}

export class ModifyColumnsNode implements ModificationNode {
  readonly nodeId: string;
  readonly type = NodeType.kModifyColumns;
  readonly prevNode: QueryNode;
  nextNodes: QueryNode[];
  readonly state: ModifyColumnsState;

  constructor(state: ModifyColumnsState) {
    this.nodeId = nextNodeId();
    this.prevNode = state.prevNode;
    this.nextNodes = [];

    this.state = {
      ...state,
      newColumns: state.newColumns ?? [],
      selectedColumns: state.selectedColumns ?? [],
    };

    if (
      this.state.selectedColumns.length === 0 &&
      this.prevNode !== undefined
    ) {
      this.state.selectedColumns = newColumnInfoList(this.prevNode.finalCols);
    }

    const userOnChange = this.state.onchange;
    this.state.onchange = () => {
      notifyNextNodes(this);
      userOnChange?.();
    };
  }

  get finalCols(): ColumnInfo[] {
    return this.computeFinalCols();
  }

  private computeFinalCols(): ColumnInfo[] {
    const finalCols = newColumnInfoList(
      this.state.selectedColumns.filter((col) => col.checked),
    );
    this.state.newColumns
      .filter((c) => this.isNewColumnValid(c))
      .forEach((col) => {
        // Use stored sqlType if available (from deserialization)
        if (col.sqlType) {
          finalCols.push({
            name: col.name,
            type: col.sqlType,
            checked: true,
            column: {name: col.name},
          });
          return;
        }

        // Try to preserve type information if the expression is a simple column reference
        const sourceCol = this.state.prevNode?.finalCols?.find(
          (c) => c.column.name === col.expression,
        );
        if (sourceCol) {
          // If the expression is a simple column reference, preserve the type
          // Also store it in sqlType for future serialization
          col.sqlType = sourceCol.type;
          finalCols.push({
            name: col.name,
            type: sourceCol.type,
            checked: true,
            column: {...sourceCol.column, name: col.name},
          });
        } else {
          // For complex expressions, use 'NA' as type
          finalCols.push(columnInfoFromName(col.name, true));
        }
      });
    return finalCols;
  }

  onPrevNodesUpdated() {
    // This node assumes it has only one previous node.
    if (this.prevNode === undefined) {
      return;
    }

    const sourceCols = this.prevNode.finalCols;

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
  }

  static deserializeState(
    serializedState: ModifyColumnsSerializedState,
  ): ModifyColumnsState {
    return {
      ...serializedState,
      prevNode: undefined as unknown as QueryNode,
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
    // Recover full column information from prevNode
    if (this.prevNode === undefined) {
      return;
    }

    const sourceCols = this.prevNode.finalCols ?? [];
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

    const colNames = new Set<string>();
    for (const col of this.state.selectedColumns) {
      if (!col.checked) continue;
      const name = col.alias ? col.alias.trim() : col.column.name;
      if (col.alias && name === '') {
        this.setValidationError('Empty alias not allowed');
        return false;
      }
      if (colNames.has(name)) {
        this.setValidationError('Duplicate column names');
        return false;
      }
      colNames.add(name);
    }

    for (const col of this.state.newColumns) {
      const name = col.name.trim();
      const expression = col.expression.trim();

      // If a column has an expression, it must have a name and be unique.
      if (expression !== '') {
        if (name === '') {
          this.setValidationError('New column must have a name');
          return false;
        }
        if (colNames.has(name)) {
          this.setValidationError('Duplicate column names');
          return false;
        }
        colNames.add(name);
      }
    }

    // Check if there are no columns selected and no valid new columns
    if (colNames.size === 0) {
      this.setValidationError(
        'No columns selected. Select at least one column or add a new column.',
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

  nodeDetails(): m.Child {
    // Determine the state of modifications.
    const hasUnselected = this.state.selectedColumns.some((c) => !c.checked);
    const hasAlias = this.state.selectedColumns.some((c) => c.alias);
    const newValidColumns = this.state.newColumns.filter((c) =>
      this.isNewColumnValid(c),
    );

    // If there are no modifications, show a default message.
    if (!hasUnselected && !hasAlias && newValidColumns.length === 0) {
      return m('.pf-exp-node-details-message', 'Select all');
    }

    const cards: m.Child[] = [];

    // If columns have been unselected or aliased, list the selected ones.
    if (hasUnselected || hasAlias) {
      const selectedCols = this.state.selectedColumns.filter((c) => c.checked);
      if (selectedCols.length > 0) {
        // If there are too many selected columns and some are unselected, show a summary.
        const maxColumnsToShow = 5;
        const shouldShowSummary =
          hasUnselected && selectedCols.length > maxColumnsToShow;

        if (shouldShowSummary) {
          const renamedCols = selectedCols.filter((c) => c.alias);
          const totalCols = this.state.selectedColumns.length;
          const summaryText = `${selectedCols.length} of ${totalCols} columns selected`;

          // Show up to 3 renamed columns explicitly even in summary mode
          if (renamedCols.length > 0 && renamedCols.length <= 3) {
            const renamedItems = renamedCols.map((c) =>
              m('div', `${c.column.name} AS ${c.alias}`),
            );
            cards.push(
              m(
                Card,
                {className: 'pf-exp-node-details-card'},
                m('div', summaryText),
                m('div', {style: 'height: 8px'}), // spacing
                ...renamedItems,
              ),
            );
          } else {
            cards.push(
              m(
                Card,
                {className: 'pf-exp-node-details-card'},
                m('div', summaryText),
              ),
            );
          }
        } else {
          const selectedItems = selectedCols.map((c) => {
            if (c.alias) {
              return m('div', `${c.column.name} AS ${c.alias}`);
            } else {
              return m('div', c.column.name);
            }
          });
          cards.push(
            m(Card, {className: 'pf-exp-node-details-card'}, ...selectedItems),
          );
        }
      }
    }

    // If new columns have been added, list them.
    if (newValidColumns.length > 0) {
      if (!hasUnselected && !hasAlias) {
        cards.push(m('span', '+'));
      }
      const switchColumns = newValidColumns.filter((c) => c.type === 'switch');
      const ifColumns = newValidColumns.filter((c) => c.type === 'if');
      const otherNewColumns = newValidColumns.filter(
        (c) => c.type !== 'switch' && c.type !== 'if',
      );

      if (otherNewColumns.length > 0) {
        const newItems = otherNewColumns.map((c) => {
          const expression = c.expression.replace(' END', '');
          return m('.', `${expression} AS ${c.name}`);
        });
        cards.push(
          m(Card, {className: 'pf-exp-node-details-card'}, ...newItems),
        );
      }

      if (switchColumns.length > 0) {
        const switchItems = switchColumns.map((c) =>
          m(
            'div.pf-exp-switch-summary',
            m('span.pf-exp-switch-keyword', 'SWITCH'),
            ' on ',
            m('span.pf-exp-column-name', c.switchOn),
            ' ',
            m('span.pf-exp-as-keyword', 'AS'),
            ' ',
            m('span.pf-exp-alias-name', c.name),
          ),
        );
        cards.push(
          m(Card, {className: 'pf-exp-node-details-card'}, ...switchItems),
        );
      }
      if (ifColumns.length > 0) {
        const ifItems = ifColumns.map((c) =>
          m(
            'div.pf-exp-if-summary',
            m('span.pf-exp-if-keyword', 'IF'),
            ' ',
            m('span.pf-exp-as-keyword', 'AS'),
            ' ',
            m('span.pf-exp-alias-name', c.name),
          ),
        );
        cards.push(
          m(Card, {className: 'pf-exp-node-details-card'}, ...ifItems),
        );
      }
    }

    // If all columns have been deselected, show a specific message.
    if (cards.length === 0) {
      return m('.pf-exp-node-details-message', 'All columns deselected');
    }

    return m(CardStack, cards);
  }

  nodeSpecificModify(): m.Child {
    return m(
      'div.pf-modify-columns-node',
      m(
        CardStack,
        m(
          Card,
          m(
            'div',
            {
              style:
                'display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px',
            },
            m(
              'h2.pf-columns-box-title',
              {style: 'margin: 0'},
              'Selected Columns',
            ),
            m(Button, {
              label: 'Deselect All',
              variant: ButtonVariant.Outlined,
              onclick: () => {
                this.state.selectedColumns = this.state.selectedColumns.map(
                  (col) => ({...col, checked: false}),
                );
                this.state.onchange?.();
              },
            }),
          ),
          m(
            'div.pf-column-list',
            this.state.selectedColumns.map((col, index) =>
              this.renderSelectedColumn(col, index),
            ),
          ),
        ),
        this.state.newColumns.length > 0 &&
          m(
            Card,
            this.state.newColumns.map((col, index) =>
              this.renderNewColumn(col, index),
            ),
          ),
        m(
          Card,
          m(
            'div.pf-exp-modify-columns-node-buttons',
            this.renderAddColumnButton(),
            this.renderAddSwitchButton(),
            this.renderAddIfButton(),
          ),
        ),
      ),
    );
  }

  private renderSelectedColumn(col: ColumnInfo, index: number): m.Child {
    return m(
      '.pf-column',
      {
        ondragover: (e: DragEvent) => {
          e.preventDefault();
        },
        ondrop: (e: DragEvent) => {
          e.preventDefault();
          const from = parseInt(e.dataTransfer!.getData('text/plain'), 10);
          const to = index;

          const newSelectedColumns = [...this.state.selectedColumns];
          const [removed] = newSelectedColumns.splice(from, 1);
          newSelectedColumns.splice(to, 0, removed);
          this.state.selectedColumns = newSelectedColumns;
          this.state.onchange?.();
        },
      },
      m(
        'span.pf-drag-handle',
        {
          draggable: true,
          ondragstart: (e: DragEvent) => {
            e.dataTransfer!.setData('text/plain', index.toString());
          },
        },
        '☰',
      ),
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
          newSelectedColumns[index] = {
            ...newSelectedColumns[index],
            alias: (e.target as HTMLInputElement).value,
          };
          this.state.selectedColumns = newSelectedColumns;
          this.state.onchange?.();
        },
        placeholder: 'alias',
        value: col.alias ? col.alias : '',
      }),
    );
  }

  private isNewColumnValid(col: NewColumn): boolean {
    return col.expression.trim() !== '' && col.name.trim() !== '';
  }

  private renderNewColumn(col: NewColumn, index: number): m.Child {
    const commonWrapper = (children: m.Child[]) => {
      return m(
        '.pf-column',
        {
          ondragover: (e: DragEvent) => {
            e.preventDefault();
          },
          ondrop: (e: DragEvent) => {
            e.preventDefault();
            const from = parseInt(e.dataTransfer!.getData('text/plain'), 10);
            const to = this.state.selectedColumns.length + index;

            const newSelectedColumns = [...this.state.selectedColumns];
            const newNewColumns = [...this.state.newColumns];

            if (from < this.state.selectedColumns.length) {
              const [removed] = newSelectedColumns.splice(from, 1);
              newNewColumns.splice(to - this.state.selectedColumns.length, 0, {
                expression: removed.column.name,
                name: removed.alias || '',
              });
            } else {
              const [removed] = newNewColumns.splice(
                from - this.state.selectedColumns.length,
                1,
              );
              newNewColumns.splice(
                to - this.state.selectedColumns.length,
                0,
                removed,
              );
            }
            this.state.selectedColumns = newSelectedColumns;
            this.state.newColumns = newNewColumns;
            this.state.onchange?.();
          },
        },
        m(
          'span.pf-drag-handle',
          {
            draggable: true,
            ondragstart: (e: DragEvent) => {
              e.dataTransfer!.setData(
                'text/plain',
                (this.state.selectedColumns.length + index).toString(),
              );
            },
          },
          '☰',
        ),
        ...children,
        m(Button, {
          icon: 'close',
          compact: true,
          onclick: () => {
            const newNewColumns = [...this.state.newColumns];
            newNewColumns.splice(index, 1);
            this.state.newColumns = newNewColumns;
            this.state.onchange?.();
          },
        }),
      );
    };

    if (col.type === 'switch') {
      return m(
        '.pf-exp-switch-wrapper',
        m(
          '.pf-exp-switch-name-row',
          m('label', 'New switch column name'),
          m(TextInput, {
            oninput: (e: Event) => {
              const newNewColumns = [...this.state.newColumns];
              newNewColumns[index] = {
                ...newNewColumns[index],
                name: (e.target as HTMLInputElement).value,
              };
              this.state.newColumns = newNewColumns;
              this.state.onchange?.();
            },
            placeholder: 'name',
            value: col.name,
          }),
          !this.isNewColumnValid(col) && m(Icon, {icon: 'warning'}),
          m(Button, {
            icon: 'close',
            compact: true,
            onclick: () => {
              const newNewColumns = [...this.state.newColumns];
              newNewColumns.splice(index, 1);
              this.state.newColumns = newNewColumns;
              this.state.onchange?.();
            },
          }),
        ),
        m(SwitchComponent, {
          column: col,
          columns: this.prevNode?.finalCols ?? [],
          onchange: () => {
            const newNewColumns = [...this.state.newColumns];
            newNewColumns[index] = {...col};
            this.state.newColumns = newNewColumns;
            this.state.onchange?.();
          },
        }),
      );
    }

    if (col.type === 'if') {
      return m(
        '.pf-exp-if-wrapper',
        m(
          '.pf-exp-if-name-row',
          m('label', 'New if column name'),
          m(TextInput, {
            oninput: (e: Event) => {
              const newNewColumns = [...this.state.newColumns];
              newNewColumns[index] = {
                ...newNewColumns[index],
                name: (e.target as HTMLInputElement).value,
              };
              this.state.newColumns = newNewColumns;
              this.state.onchange?.();
            },
            placeholder: 'name',
            value: col.name,
          }),
          !this.isNewColumnValid(col) && m(Icon, {icon: 'warning'}),
          m(Button, {
            icon: 'close',
            compact: true,
            onclick: () => {
              const newNewColumns = [...this.state.newColumns];
              newNewColumns.splice(index, 1);
              this.state.newColumns = newNewColumns;
              this.state.onchange?.();
            },
          }),
        ),
        m(IfComponent, {
          column: col,
          onchange: () => {
            const newNewColumns = [...this.state.newColumns];
            newNewColumns[index] = {...col};
            this.state.newColumns = newNewColumns;
            this.state.onchange?.();
          },
        }),
      );
    }

    const isValid = this.isNewColumnValid(col);

    return commonWrapper([
      m(TextInput, {
        oninput: (e: Event) => {
          const newNewColumns = [...this.state.newColumns];
          newNewColumns[index] = {
            ...newNewColumns[index],
            expression: (e.target as HTMLInputElement).value,
          };
          this.state.newColumns = newNewColumns;
          this.state.onchange?.();
        },
        placeholder: 'expression',
        value: col.expression,
      }),
      m(TextInput, {
        oninput: (e: Event) => {
          const newNewColumns = [...this.state.newColumns];
          newNewColumns[index] = {
            ...newNewColumns[index],
            name: (e.target as HTMLInputElement).value,
          };
          this.state.newColumns = newNewColumns;
          this.state.onchange?.();
        },
        placeholder: 'name',
        value: col.name,
      }),
      !isValid && m(Icon, {icon: 'warning'}),
    ]);
  }

  private renderAddColumnButton(): m.Child {
    return m(Button, {
      label: 'Add column',
      variant: ButtonVariant.Outlined,
      onclick: () => {
        this.state.newColumns = [
          ...this.state.newColumns,
          {
            expression: '',
            name: '',
          },
        ];
        this.state.onchange?.();
      },
    });
  }

  private renderAddSwitchButton(): m.Child {
    return m(Button, {
      label: 'Add SWITCH',
      variant: ButtonVariant.Outlined,
      onclick: () => {
        this.state.newColumns = [
          ...this.state.newColumns,
          {
            type: 'switch',
            expression: '',
            name: '',
          },
        ];
        this.state.onchange?.();
      },
    });
  }

  private renderAddIfButton(): m.Child {
    return m(Button, {
      label: 'Add IF',
      variant: ButtonVariant.Outlined,
      onclick: () => {
        this.state.newColumns = [
          ...this.state.newColumns,
          {
            type: 'if',
            expression: '',
            name: '',
            clauses: [{if: '', then: ''}],
          },
        ];
        this.state.onchange?.();
      },
    });
  }

  nodeInfo(): m.Children {
    return m(
      'div',
      m(
        'p',
        'Select which columns to include, rename columns, and create new computed columns using expressions.',
      ),
      m(
        'p',
        'Use expressions like ',
        m('code', 'dur / 1000000'),
        ' to convert nanoseconds to milliseconds, or ',
        m('code', 'CASE WHEN ... THEN ... END'),
        ' for conditional logic.',
      ),
      m(
        'p',
        m('strong', 'Example:'),
        ' Create a new column ',
        m('code', 'dur_ms'),
        ' by computing ',
        m('code', 'dur / 1000000'),
        ', or rename ',
        m('code', 'ts'),
        ' to ',
        m('code', 'timestamp'),
        '.',
      ),
    );
  }

  clone(): QueryNode {
    return new ModifyColumnsNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    if (this.prevNode === undefined) return undefined;

    // Build column specifications
    const columns: ColumnSpec[] = [];

    for (const col of this.state.selectedColumns) {
      if (!col.checked) continue;
      columns.push({
        columnNameOrExpression: col.column.name,
        alias: col.alias,
      });
    }

    for (const col of this.state.newColumns) {
      if (!this.isNewColumnValid(col)) continue;
      columns.push({
        columnNameOrExpression: col.expression,
        alias: col.name,
        referencedModule: col.module,
      });
    }

    // Collect referenced modules
    const referencedModules = this.state.newColumns
      .filter((col) => col.module)
      .map((col) => col.module!);

    // Apply column selection
    return StructuredQueryBuilder.withSelectColumns(
      this.prevNode,
      columns,
      referencedModules.length > 0 ? referencedModules : undefined,
      this.nodeId,
    );
  }

  serializeState(): ModifyColumnsSerializedState {
    return {
      prevNodeId: this.prevNode?.nodeId,
      newColumns: this.state.newColumns.map((c) => ({
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
        sqlType: c.sqlType, // Preserve SQL type across serialization
      })),
      selectedColumns: this.state.selectedColumns.map((c) => ({
        name: c.name,
        type: c.type,
        checked: c.checked,
        alias: c.alias,
      })),
      comment: this.state.comment,
    };
  }
}
