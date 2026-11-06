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
import type {
  LogicalOpContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import type {LogicalOp} from '../ir';
import {Button, ButtonVariant} from '../../../widgets/button';
import {DraggableStack} from '../components/draggable_stack';
import {Stack} from '../components/stack';
import {ColumnPicker} from '../widgets/column_picker';
import {RadioGroup} from '../../../widgets/radio_group';

const SortOrderSchema = z.enum(['ASC', 'DESC']);

const SortConditionSchema = z.object({
  column: z.string(),
  order: SortOrderSchema,
});

export type SortCondition = z.infer<typeof SortConditionSchema>;

const SortConfigSchema = z
  .object({
    sortColumn: z.string(),
    sortOrder: SortOrderSchema,
    conditions: z.array(SortConditionSchema).optional(),
  })
  .describe(
    'Order rows; set sortColumn and sortOrder to match the primary sort key.',
  );

export type SortConfig = z.infer<typeof SortConfigSchema>;

// Get the effective sort conditions, migrating legacy single-column data.
export function getSortConditions(config: SortConfig): SortCondition[] {
  if (config.conditions && config.conditions.length > 0) {
    return config.conditions;
  }
  // Migrate legacy single-column format.
  if (config.sortColumn) {
    return [{column: config.sortColumn, order: config.sortOrder}];
  }
  return [];
}

// Convert sort conditions to SQL ORDER BY clause.
export function sortConditionsToSql(conditions: SortCondition[]): string {
  return conditions
    .filter((c) => c.column)
    .map((c) => `${c.column} ${c.order}`)
    .join(', ');
}

function SortNodeContent(): m.Component<{
  config: SortConfig;
  updateConfig: (updates: Partial<SortConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const availableColumns = ctx.availableColumns;
      const conditions = getSortConditions(config);

      const updateConditions = (newConditions: SortCondition[]) => {
        const first = newConditions[0];
        updateConfig({
          conditions: newConditions,
          sortColumn: first ? first.column : '',
          sortOrder: first ? first.order : 'ASC',
        });
      };

      return m(Stack, [
        m(DraggableStack, {
          rows: conditions.map((cond, i) => [
            m(ColumnPicker, {
              value: cond.column,
              columns: availableColumns,
              placeholder: 'column',
              onSelect: (column) =>
                updateConditions(conditions.with(i, {...cond, column})),
            }),
            m(
              RadioGroup,
              {
                selectedValue: cond.order,
                onValueChange: (value: string) =>
                  updateConditions(
                    conditions.with(i, {
                      ...cond,
                      order: value as 'ASC' | 'DESC',
                    }),
                  ),
              },
              m(RadioGroup.Button, {value: 'ASC'}, 'ASC'),
              m(RadioGroup.Button, {value: 'DESC'}, 'DESC'),
            ),
          ]),
          onReorder: (from, to) =>
            updateConditions(moveItem(conditions, from, to)),
          onDelete: (i) => updateConditions(conditions.toSpliced(i, 1)),
        }),
        m(Button, {
          label: 'Add sort by',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () => {
            updateConditions([...conditions, {column: '', order: 'ASC'}]);
          },
        }),
      ]);
    },
  };
}

function renderSortNode(
  config: SortConfig,
  updateConfig: (updates: Partial<SortConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(SortNodeContent, {config, updateConfig, ctx});
}

function isValid(config: SortConfig): boolean {
  return (
    (config.conditions ?? []).some((c) => c.column !== '') ||
    config.sortColumn !== ''
  );
}

function toLogicalOp(
  config: SortConfig,
  ctx: LogicalOpContext,
): LogicalOp | undefined {
  const input = ctx.getInputNodeId('input');
  if (!input) return undefined;
  return {
    kind: 'sort',
    input,
    keys: getSortConditions(config).filter((c) => c.column !== ''),
  };
}

export const manifest: NodeManifest<SortConfig> = {
  title: 'Sort',
  icon: 'sort',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 44,
  schema: SortConfigSchema,
  defaultConfig: () => ({sortColumn: '', sortOrder: 'ASC', conditions: []}),
  render: renderSortNode,
  isValid,
  getOutputColumns: (_config, ctx) => ctx.getInputColumns('input'),
  toLogicalOp,
};
