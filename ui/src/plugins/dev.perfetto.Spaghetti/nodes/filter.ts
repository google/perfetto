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
import {z} from 'zod';
import {moveArrayItem} from '../../../base/array_utils';
import {Button, ButtonVariant} from '../../../widgets/button';
import {RadioGroup} from '../../../widgets/radio_group';
import {Select} from '../../../widgets/select';
import {TextInput} from '../../../widgets/text_input';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import {ColumnPicker} from '../widgets/column_picker';
import {
  FILTER_OPS,
  type FilterConjunction,
  type FilterOp,
  isUnaryFilterOp,
  type LogicalOp,
} from '../logical_ir';
import type {
  LogicalOpContext,
  NodeManifest,
  RenderContext,
} from '../node_types';

export {FILTER_OPS};
export type {FilterConjunction, FilterOp};

const FilterOpSchema = z.enum(FILTER_OPS);

const FilterConditionSchema = z.object({
  column: z.string(),
  op: FilterOpSchema,
  value: z.string(),
});

export type FilterCondition = z.infer<typeof FilterConditionSchema>;

const FilterConjunctionSchema = z.enum(['AND', 'OR']);

const FilterConfigSchema = z.object({
  conditions: z.array(FilterConditionSchema),
  conjunction: FilterConjunctionSchema.optional(),
});

export type FilterConfig = z.infer<typeof FilterConfigSchema>;

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
                  const toIdx = isBottom ? i + 1 : i;
                  if (fromIdx !== toIdx && fromIdx + 1 !== toIdx) {
                    const newConds = [...conditions];
                    moveArrayItem(newConds, fromIdx, toIdx);
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
                ...(!isUnaryFilterOp(cond.op)
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

function isValid(config: FilterConfig): boolean {
  if (config.conditions.length === 0) return true;
  return config.conditions.every((c) => {
    if (!c.column) return true;
    if (isUnaryFilterOp(c.op)) return true;
    return c.value !== '';
  });
}

function toLogicalOp(
  config: FilterConfig,
  ctx: LogicalOpContext,
): LogicalOp | undefined {
  const input = ctx.getInputNodeId('input');
  if (!input) return undefined;
  return {
    kind: 'filter',
    input,
    conditions: config.conditions.filter((c) => c.column !== ''),
    conjunction: config.conjunction ?? 'AND',
  };
}

export const manifest: NodeManifest<FilterConfig> = {
  title: 'Filter',
  icon: 'filter_alt',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 35,
  schema: FilterConfigSchema,
  defaultConfig: () => ({
    conditions: [],
    conjunction: 'AND',
  }),
  render: renderFilterNode,
  isValid,
  getOutputColumns: (_config, ctx) => ctx.getInputColumns('input'),
  toLogicalOp,
};
