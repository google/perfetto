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
import {moveItem} from '../../../base/array_utils';
import {Button, ButtonVariant} from '../../../widgets/button';
import {RadioGroup} from '../../../widgets/radio_group';
import {Select} from '../../../widgets/select';
import {TextInput} from '../../../widgets/text_input';
import {DraggableStack} from '../components/draggable_stack';
import {Stack} from '../components/stack';
import {ColumnPicker} from '../widgets/column_picker';
import {
  type FilterConjunction,
  type FilterOp,
  isUnaryFilterOp,
} from '../sql_types';
import {formatFilterConditions} from '../generator_utils';
import type {NodeGenerators, NodeManifest, RenderContext} from '../node_types';
import './filter.scss';

export const FILTER_OPS = [
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
] as const satisfies readonly FilterOp[];

export type {FilterConjunction, FilterOp};

const FilterOpSchema = z.enum(FILTER_OPS);

const FilterConditionSchema = z.object({
  column: z.string(),
  op: FilterOpSchema,
  value: z
    .string()
    .describe(
      'Comparison value (numbers stay unquoted, plain strings are auto-quoted, "" for IS NULL / IS NOT NULL)',
    ),
});

export type FilterCondition = z.infer<typeof FilterConditionSchema>;

const FilterConjunctionSchema = z.enum(['AND', 'OR']);

const FilterConfigSchema = z
  .object({
    conditions: z.array(FilterConditionSchema),
    conjunction: FilterConjunctionSchema.optional(),
  })
  .describe('Keep rows matching predicates.');

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

      return m(Stack, {className: 'pf-spag-filter'}, [
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
        m(DraggableStack, {
          rows: conditions.map((cond, i) => [
            m(ColumnPicker, {
              value: cond.column,
              columns: availableColumns,
              placeholder: 'column',
              onSelect: (column) =>
                updateConfig({
                  conditions: conditions.with(i, {...cond, column}),
                }),
            }),
            m(
              Select,
              {
                value: cond.op,
                onchange: (e: Event) => {
                  updateConfig({
                    conditions: conditions.with(i, {
                      ...cond,
                      op: (e.target as HTMLSelectElement).value as FilterOp,
                    }),
                  });
                },
              },
              FILTER_OPS.map((op) => m('option', {value: op}, op)),
            ),
            !isUnaryFilterOp(cond.op) &&
              m(TextInput, {
                placeholder: 'value',
                value: cond.value,
                onChange: (value) =>
                  updateConfig({
                    conditions: conditions.with(i, {...cond, value}),
                  }),
              }),
          ]),
          onReorder: (from, to) =>
            updateConfig({conditions: moveItem(conditions, from, to)}),
          onDelete: (i) =>
            updateConfig({conditions: conditions.toSpliced(i, 1)}),
        }),
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

function whereExpr(config: FilterConfig): string {
  return formatFilterConditions(
    config.conditions.filter((c) => c.column !== ''),
    config.conjunction ?? 'AND',
  );
}

const generators: NodeGenerators<FilterConfig> = {
  sql: (config, ctx) => {
    if (ctx.input('input') === undefined) return undefined;
    const where = whereExpr(config);
    return where !== '' ? {where} : {};
  },
  psn: (config, ctx) => {
    if (ctx.input('input') === undefined) return undefined;
    const where = whereExpr(config);
    return {stages: where !== '' ? [`WHERE ${where}`] : []};
  },
};

export const manifest: NodeManifest<FilterConfig> = {
  title: 'Filter',
  icon: 'filter_alt',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 18,
  schema: FilterConfigSchema,
  defaultConfig: () => ({
    conditions: [],
    conjunction: 'AND',
  }),
  render: renderFilterNode,
  isValid,
  getOutputColumns: (_config, ctx) => ctx.getInputColumns('input'),
  generators,
};
