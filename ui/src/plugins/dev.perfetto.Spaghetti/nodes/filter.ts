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
import {Button, ButtonVariant} from '../../../widgets/button';
import {RadioGroup} from '../../../widgets/radio_group';
import {Select} from '../../../widgets/select';
import {TextInput} from '../../../widgets/text_input';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import {ColumnPicker} from '../widgets/column_picker';
import type {NodeManifest, RenderContext, SqlStatement} from '../node_types';

export type FilterOp =
  | '='
  | '!='
  | '>'
  | '>='
  | '<'
  | '<='
  | 'LIKE'
  | 'NOT LIKE'
  | 'GLOB'
  | 'IS NULL'
  | 'IS NOT NULL';

export const FILTER_OPS: FilterOp[] = [
  '=',
  '!=',
  '>',
  '>=',
  '<',
  '<=',
  'LIKE',
  'NOT LIKE',
  'GLOB',
  'IS NULL',
  'IS NOT NULL',
];

const UNARY_OPS: Set<FilterOp> = new Set(['IS NULL', 'IS NOT NULL']);

export interface FilterCondition {
  readonly column: string;
  readonly op: FilterOp;
  readonly value: string;
}

export type FilterConjunction = 'AND' | 'OR';

export interface FilterConfig {
  readonly conditions: FilterCondition[];
  readonly conjunction?: FilterConjunction;
}

// Returns true if the value should be emitted as-is (numeric literal, SQL
// NULL keyword, or already quoted/parenthesised by the user).
function isRawValue(value: string): boolean {
  if (value === '') return true;
  if (/^-?\d+(\.\d+)?([eE][+-]?\d+)?$/.test(value)) return true; // number
  if (/^null$/i.test(value)) return true; // NULL
  if (value.startsWith("'") || value.startsWith('"')) return true; // already quoted
  if (value.startsWith('(')) return true; // e.g. (1,2,3)
  return false;
}

function quoteValue(value: string): string {
  if (isRawValue(value)) return value;
  // Escape any single quotes inside the value then wrap in single quotes.
  return `'${value.replace(/'/g, "''")}'`;
}

export function conditionsToSql(
  conditions: FilterCondition[],
  conjunction: FilterConjunction = 'AND',
): string {
  const parts = conditions
    .filter((c) => c.column)
    .map((c) => {
      if (UNARY_OPS.has(c.op)) {
        return `${c.column} ${c.op}`;
      }
      return `${c.column} ${c.op} ${quoteValue(c.value)}`;
    });
  return parts.join(` ${conjunction} `);
}

function FilterNodeContent(): m.Component<{
  config: FilterConfig;
  updateConfig: (updates: Partial<FilterConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const availableColumns = ctx.availableColumns;
      const conditions = config.conditions;
      const conjunction = config.conjunction ?? 'AND';

      return m(Stack, [
        m(
          RadioGroup,
          {
            fillWidth: true,
            className: 'pf-spag-conjunction',
            selectedValue: conjunction,
            onValueChange: (value: string) =>
              updateConfig({conjunction: value as 'AND' | 'OR'}),
          },
          [
            m(RadioGroup.Button, {value: 'AND'}, 'AND'),
            m(RadioGroup.Button, {value: 'OR'}, 'OR'),
          ],
        ),
        m(Stack, [
          ...conditions.map((cond, i) =>
            m(
              Row,
              {
                key: i,
                draggable: true,
                ondragstart: (e: DragEvent) => {
                  e.dataTransfer!.effectAllowed = 'move';
                  e.dataTransfer!.setData('text/plain', String(i));
                  (e.currentTarget as HTMLElement).classList.add('pf-dragging');
                },
                ondragend: (e: DragEvent) => {
                  (e.currentTarget as HTMLElement).classList.remove(
                    'pf-dragging',
                  );
                },
                ondragover: (e: DragEvent) => {
                  e.preventDefault();
                  e.dataTransfer!.dropEffect = 'move';
                  const el = e.currentTarget as HTMLElement;
                  const rect = el.getBoundingClientRect();
                  const isBottom = e.clientY > rect.top + rect.height / 2;
                  el.classList.toggle('pf-drag-over-top', !isBottom);
                  el.classList.toggle('pf-drag-over-bottom', isBottom);
                },
                ondragleave: (e: DragEvent) => {
                  const el = e.currentTarget as HTMLElement;
                  el.classList.remove(
                    'pf-drag-over-top',
                    'pf-drag-over-bottom',
                  );
                },
                ondrop: (e: DragEvent) => {
                  e.preventDefault();
                  const el = e.currentTarget as HTMLElement;
                  const isBottom = el.classList.contains('pf-drag-over-bottom');
                  el.classList.remove(
                    'pf-drag-over-top',
                    'pf-drag-over-bottom',
                  );
                  const fromIdx = parseInt(
                    e.dataTransfer!.getData('text/plain'),
                  );
                  let toIdx = isBottom ? i + 1 : i;
                  if (fromIdx !== toIdx && fromIdx + 1 !== toIdx) {
                    const newConds = [...conditions];
                    const [moved] = newConds.splice(fromIdx, 1);
                    if (fromIdx < toIdx) toIdx--;
                    newConds.splice(toIdx, 0, moved);
                    updateConfig({conditions: newConds});
                  }
                },
              },
              [
                m(Row.DragHandle),
                m(ColumnPicker, {
                  value: cond.column,
                  columns: availableColumns,
                  placeholder: 'column',
                  onSelect: (value: string) => {
                    const newConds = [...conditions];
                    newConds[i] = {...cond, column: value};
                    updateConfig({conditions: newConds});
                  },
                }),
                m(
                  Select,
                  {
                    value: cond.op,
                    onchange: (e: Event) => {
                      const newConds = [...conditions];
                      newConds[i] = {
                        ...cond,
                        op: (e.target as HTMLSelectElement).value as FilterOp,
                      };
                      updateConfig({conditions: newConds});
                    },
                  },
                  FILTER_OPS.map((op) => m('option', {value: op}, op)),
                ),
                ...(!UNARY_OPS.has(cond.op)
                  ? [
                      m(TextInput, {
                        placeholder: 'value',
                        value: cond.value,
                        onChange: (value: string) => {
                          const newConds = [...conditions];
                          newConds[i] = {...cond, value};
                          updateConfig({conditions: newConds});
                        },
                      }),
                    ]
                  : []),
                m(Row.DeleteButton, {
                  onclick: () => {
                    updateConfig({
                      conditions: conditions.filter((_, j) => j !== i),
                    });
                  },
                }),
              ],
            ),
          ),
        ]),
        m(Button, {
          label: 'Add condition',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () => {
            updateConfig({
              conditions: [...conditions, {column: '', op: '=', value: ''}],
            });
          },
        }),
      ]);
    },
  };
}

function renderFilterNode(
  config: FilterConfig,
  updateConfig: (updates: Partial<FilterConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(FilterNodeContent, {config, updateConfig, ctx});
}

const UNARY_FILTER_OPS: Set<FilterOp> = new Set(['IS NULL', 'IS NOT NULL']);

function isValid(config: FilterConfig): boolean {
  if (config.conditions.length === 0) return true;
  return config.conditions.every((c) => {
    if (!c.column) return true;
    if (UNARY_FILTER_OPS.has(c.op)) return true;
    return c.value !== '';
  });
}

function tryFold(stmt: SqlStatement, config: FilterConfig): boolean {
  if (
    stmt.groupBy !== undefined ||
    stmt.orderBy !== undefined ||
    stmt.limit !== undefined
  ) {
    return false;
  }
  const expr = conditionsToSql(config.conditions, config.conjunction);
  if (expr) {
    stmt.where = stmt.where ? `(${stmt.where}) AND (${expr})` : expr;
  }
  return true;
}

export const manifest: NodeManifest<FilterConfig> = {
  title: 'Filter',
  icon: 'filter_alt',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 35,
  defaultConfig: () => ({
    conditions: [],
    conjunction: 'AND',
  }),
  render: renderFilterNode,
  isValid,
  getOutputColumns: (_config, ctx) => ctx.getInputColumns('input'),
  tryFold,
};
