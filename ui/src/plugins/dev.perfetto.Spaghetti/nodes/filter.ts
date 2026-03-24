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
import {Intent} from '../../../widgets/common';
import {Icon} from '../../../widgets/icon';
import {SegmentedButtons} from '../../../widgets/segmented_buttons';
import {Select} from '../../../widgets/select';
import {TextInput} from '../../../widgets/text_input';
import {ColumnPicker} from '../widgets/column_picker';
import {NodeManifest, RenderContext, SqlStatement} from '../node_types';

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
  readonly filterExpression: string;
  readonly conditions: FilterCondition[];
  readonly conjunction?: FilterConjunction;
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
      return `${c.column} ${c.op} ${c.value}`;
    });
  return parts.join(` ${conjunction} `);
}

function renderFilterNode(
  config: FilterConfig,
  updateConfig: (updates: Partial<FilterConfig>) => void,
  ctx: RenderContext,
): m.Children {
  const availableColumns = ctx.availableColumns;
  const conditions = config.conditions;

  const conjunction = config.conjunction ?? 'AND';

  return m('.pf-qb-stack', [
    m(SegmentedButtons, {
      fillWidth: true,
      className: 'pf-qb-conjunction',
      options: [{label: 'AND'}, {label: 'OR'}],
      selectedOption: conjunction === 'AND' ? 0 : 1,
      onOptionSelected: (i: number) =>
        updateConfig({conjunction: i === 0 ? 'AND' : 'OR'}),
    }),
    m('.pf-qb-filter-list', [
      ...conditions.map((cond, i) =>
        m(
          '.pf-qb-filter-row',
          {
            key: i,
            draggable: conditions.length > 1,
            ondragstart: (e: DragEvent) => {
              e.dataTransfer!.effectAllowed = 'move';
              e.dataTransfer!.setData('text/plain', String(i));
              (e.currentTarget as HTMLElement).classList.add('pf-dragging');
            },
            ondragend: (e: DragEvent) => {
              (e.currentTarget as HTMLElement).classList.remove('pf-dragging');
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
              el.classList.remove('pf-drag-over-top', 'pf-drag-over-bottom');
            },
            ondrop: (e: DragEvent) => {
              e.preventDefault();
              const el = e.currentTarget as HTMLElement;
              const isBottom = el.classList.contains('pf-drag-over-bottom');
              el.classList.remove('pf-drag-over-top', 'pf-drag-over-bottom');
              const fromIdx = parseInt(e.dataTransfer!.getData('text/plain'));
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
            ...(conditions.length > 1 ? [m(Icon, {icon: 'drag_indicator', className: 'pf-qb-drag-handle'})] : []),
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
            m(Button, {
              icon: 'delete',
              variant: ButtonVariant.Filled,
              intent: Intent.Danger,
              className: 'pf-qb-row-delete',
              title: 'Remove condition',
              onclick: () => {
                const newConds = conditions.filter((_, j) => j !== i);
                updateConfig({conditions: newConds});
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
  )
    return false;
  const expr =
    config.conditions.length > 0
      ? conditionsToSql(config.conditions, config.conjunction)
      : config.filterExpression;
  if (expr) {
    stmt.where = stmt.where ? `(${stmt.where}) AND (${expr})` : expr;
  }
  return true;
}

export const manifest: NodeManifest<FilterConfig> = {
  title: 'Filter',
  icon: 'filter_alt',
  inputs: [{name: 'input', content: 'Input', direction: 'left'}],
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockTop: true,
  canDockBottom: true,
  hue: 35,
  defaultConfig: () => ({filterExpression: '', conditions: [], conjunction: 'AND'}),
  render: renderFilterNode,
  isValid,
  getOutputColumns: (_config, ctx) => ctx.getInputColumns('input'),
  tryFold,
};
