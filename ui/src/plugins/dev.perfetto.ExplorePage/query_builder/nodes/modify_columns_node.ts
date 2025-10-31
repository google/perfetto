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
import {
  ColumnInfo,
  columnInfoFromName,
  newColumnInfoList,
} from '../column_info';
import protos from '../../../../protos';
import {
  createFiltersProto,
  FilterOperation,
  UIFilter,
} from '../operations/filter';

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
        Card,
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
      );
    }

    return m(
      Card,
      m(
        '.pf-exp-switch-component',
        m('div', `SWITCH ON ${column.switchOn}`),
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
            m(
              'button',
              {onclick: () => removeCase(i)},
              m(Icon, {icon: 'close'}),
            ),
          ),
        ),
        m(Button, {
          label: 'Add case',
          onclick: addCase,
        }),
      ),
    );
  }

  private updateExpression(col: NewColumn) {
    if (col.type !== 'switch' || !col.switchOn) {
      col.expression = '';
      return;
    }

    const casesStr = (col.cases || [])
      .filter((c) => c.when.trim() !== '' && c.then.trim() !== '')
      .map((c) => `WHEN ${c.when} THEN ${c.then}`)
      .join(' ');

    const defaultStr = col.defaultValue ? `ELSE ${col.defaultValue}` : '';

    if (casesStr === '' && defaultStr === '') {
      col.expression = '';
      return;
    }

    col.expression = `CASE ${col.switchOn} ${casesStr} ${defaultStr} END`;
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
      Card,
      m(
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
            m(
              'button',
              {onclick: () => removeClause(i)},
              m(Icon, {icon: 'close'}),
            ),
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

  // For if columns
  clauses?: IfClause[];
  elseValue?: string;
}

export interface ModifyColumnsSerializedState {
  prevNodeId: string;
  newColumns: NewColumn[];
  selectedColumns: ColumnInfo[];
  filters?: UIFilter[];
  comment?: string;
}

export interface ModifyColumnsState extends QueryNodeState {
  prevNode: QueryNode;
  newColumns: NewColumn[];
  selectedColumns: ColumnInfo[];
  filters?: UIFilter[];
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

    if (this.state.selectedColumns.length === 0) {
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
        finalCols.push(columnInfoFromName(col.name, true));
      });
    return finalCols;
  }

  onPrevNodesUpdated() {
    // This node assumes it has only one previous node.
    const sourceCols = this.state.prevNode.finalCols;

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
    };
  }

  validate(): boolean {
    const colNames = new Set<string>();
    for (const col of this.state.selectedColumns) {
      if (!col.checked) continue;
      const name = col.alias ? col.alias.trim() : col.column.name;
      if (col.alias && name === '') return false; // Disallow empty alias
      if (colNames.has(name)) {
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
          return false;
        }
        if (colNames.has(name)) {
          return false;
        }
        colNames.add(name);
      }
    }

    return true;
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
        for (const c of switchColumns) {
          const cases = (c.cases || [])
            .filter((cas) => cas.when.trim() !== '' && cas.then.trim() !== '')
            .map((cas) =>
              m(
                'div',
                {style: 'padding-left: 16px'},
                `WHEN ${cas.when} THEN ${cas.then}`,
              ),
            );
          if (c.defaultValue) {
            cases.push(
              m('div', {style: 'padding-left: 16px'}, `ELSE ${c.defaultValue}`),
            );
          }
          cards.push(
            m(
              Card,
              {
                className:
                  'pf-exp-node-details-card pf-exp-switch-details-card',
              },
              m(
                'div.pf-exp-switch-expression',
                m('div', `SWITCH ON ${c.switchOn}`),
                ...cases,
              ),
              m('div.pf-exp-switch-alias', `AS ${c.name}`),
            ),
          );
        }
      }
      if (ifColumns.length > 0) {
        for (const c of ifColumns) {
          const clauses = (c.clauses || [])
            .filter((cl) => cl.if.trim() !== '' && cl.then.trim() !== '')
            .map((cl, i) =>
              m(
                'div',
                {style: 'padding-left: 16px'},
                `${i === 0 ? 'if' : 'elif'} (${cl.if}): ${cl.then}`,
              ),
            );
          if (c.elseValue) {
            clauses.push(
              m('div', {style: 'padding-left: 16px'}, `else: ${c.elseValue}`),
            );
          }
          cards.push(
            m(
              Card,
              {className: 'pf-exp-node-details-card pf-exp-if-details-card'},
              m('div.pf-exp-if-expression', ...clauses),
              m('div.pf-exp-if-alias', `AS ${c.name}`),
            ),
          );
        }
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
          m('h2.pf-columns-box-title', 'Selected Columns'),
          m(
            'div.pf-column-list',
            this.state.selectedColumns.map((col, index) =>
              this.renderSelectedColumn(col, index),
            ),
          ),
        ),
        m(
          Card,
          this.state.newColumns.map((col, index) =>
            this.renderNewColumn(col, index),
          ),
          m(
            'div.pf-exp-modify-columns-node-buttons',
            this.renderAddColumnButton(),
            this.renderAddSwitchButton(),
            this.renderAddIfButton(),
          ),
        ),
      ),
      this.renderFilterOperation(),
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
        m(
          'button',
          {
            onclick: () => {
              const newNewColumns = [...this.state.newColumns];
              newNewColumns.splice(index, 1);
              this.state.newColumns = newNewColumns;
              this.state.onchange?.();
            },
          },
          m(Icon, {icon: 'close'}),
        ),
      );
    };

    if (col.type === 'switch') {
      return commonWrapper([
        m(
          '.pf-exp-switch-component-wrapper',
          {style: 'flex-grow: 1'},
          m(SwitchComponent, {
            column: col,
            columns: this.prevNode.finalCols,
            onchange: () => {
              const newNewColumns = [...this.state.newColumns];
              newNewColumns[index] = {...col};
              this.state.newColumns = newNewColumns;
              this.state.onchange?.();
            },
          }),
        ),
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
      ]);
    }

    if (col.type === 'if') {
      return commonWrapper([
        m(
          '.pf-exp-if-component-wrapper',
          {style: 'flex-grow: 1'},
          m(IfComponent, {
            column: col,
            onchange: () => {
              const newNewColumns = [...this.state.newColumns];
              newNewColumns[index] = {...col};
              this.state.newColumns = newNewColumns;
              this.state.onchange?.();
            },
          }),
        ),
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
      ]);
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

  private renderFilterOperation(): m.Child {
    return m(FilterOperation, {
      filters: this.state.filters,
      sourceCols: this.finalCols,
      onFiltersChanged: (newFilters: ReadonlyArray<UIFilter>) => {
        this.state.filters = [...newFilters];
        this.state.onchange?.();
      },
    });
  }

  clone(): QueryNode {
    return new ModifyColumnsNode(this.state);
  }

  getStructuredQuery(): protos.PerfettoSqlStructuredQuery | undefined {
    const selectColumns: protos.PerfettoSqlStructuredQuery.SelectColumn[] = [];
    const referencedModules: string[] = [];

    for (const col of this.state.selectedColumns) {
      if (!col.checked) continue;

      const selectColumn = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectColumn.columnNameOrExpression = col.column.name;
      if (col.alias) {
        selectColumn.alias = col.alias;
      }
      selectColumns.push(selectColumn);
    }

    for (const col of this.state.newColumns) {
      // Only include valid columns (non-empty expression and name)
      if (!this.isNewColumnValid(col)) {
        continue;
      }
      const selectColumn = new protos.PerfettoSqlStructuredQuery.SelectColumn();
      selectColumn.columnNameOrExpression = col.expression;
      selectColumn.alias = col.name;
      selectColumns.push(selectColumn);
      if (col.module) {
        referencedModules.push(col.module);
      }
    }

    // This node assumes it has only one previous node.
    const prevSq = this.prevNode.getStructuredQuery();
    if (!prevSq) return;

    prevSq.selectColumns = selectColumns;
    if (referencedModules.length > 0) {
      prevSq.referencedModules = referencedModules;
    }

    const filtersProto = createFiltersProto(this.state.filters, this.finalCols);

    if (filtersProto) {
      const outerSq = new protos.PerfettoSqlStructuredQuery();
      outerSq.id = this.nodeId;
      outerSq.innerQuery = prevSq;
      outerSq.filters = filtersProto;
      return outerSq;
    }

    return prevSq;
  }

  serializeState(): ModifyColumnsSerializedState {
    if (this.prevNode === undefined) {
      throw new Error('Cannot serialize ModifyColumnsNode without a prevNode');
    }
    return {
      prevNodeId: this.prevNode.nodeId,
      newColumns: this.state.newColumns,
      selectedColumns: this.state.selectedColumns,
      filters: this.state.filters,
      comment: this.state.comment,
    };
  }
}
