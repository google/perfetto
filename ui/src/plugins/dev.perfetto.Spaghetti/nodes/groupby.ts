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
  ColumnContext,
  LogicalOpContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import type {LogicalOp} from '../logical_ir';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Select} from '../../../widgets/select';
import {ColumnPickerWithAlias} from '../components/column_picker_with_alias';
import {DraggableStack} from '../components/draggable_stack';
import {Stack} from '../components/stack';
import {ColumnPicker} from '../widgets/column_picker';
import type {ColumnDef} from '../graph_utils';

const AggregationSchema = z.object({
  func: z.enum(['COUNT', 'SUM', 'AVG', 'MIN', 'MAX']),
  column: z.string(),
  alias: z.string(),
});

export type Aggregation = z.infer<typeof AggregationSchema>;

const GroupByConfigSchema = z.object({
  groupColumns: z.array(z.string()),
  aggregations: z.array(AggregationSchema),
});

export type GroupByConfig = z.infer<typeof GroupByConfigSchema>;

function aggAlias(a: Aggregation): string {
  if (a.alias) return a.alias;
  const col = a.column === '*' ? 'star' : a.column;
  return `${a.func.toLowerCase()}_${col}`;
}

function GroupByContent(): m.Component<{
  config: GroupByConfig;
  updateConfig: (updates: Partial<GroupByConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, {style: {minWidth: '250px'}}, [
        m('.pf-spag-section-label', 'Group by'),
        m(DraggableStack, {
          rows: config.groupColumns.map((col, i) =>
            m(ColumnPicker, {
              value: col,
              columns: ctx.availableColumns,
              placeholder: 'column',
              onSelect: (value) =>
                updateConfig({
                  groupColumns: config.groupColumns.with(i, value),
                }),
            }),
          ),
          onReorder: (from, to) =>
            updateConfig({
              groupColumns: moveItem(config.groupColumns, from, to),
            }),
          onDelete: (i) =>
            updateConfig({groupColumns: config.groupColumns.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Grouping',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () => {
            updateConfig({groupColumns: [...config.groupColumns, '']});
          },
        }),

        m('.pf-spag-section-label', 'Aggregations'),
        m(DraggableStack, {
          rows: config.aggregations.map((agg, i) => [
            m(
              Select,
              {
                value: agg.func,
                onchange: (e: Event) => {
                  updateConfig({
                    aggregations: config.aggregations.with(i, {
                      ...agg,
                      func: (e.target as HTMLSelectElement)
                        .value as Aggregation['func'],
                    }),
                  });
                },
              },
              [
                m('option', {value: 'COUNT'}, 'COUNT'),
                m('option', {value: 'SUM'}, 'SUM'),
                m('option', {value: 'AVG'}, 'AVG'),
                m('option', {value: 'MIN'}, 'MIN'),
                m('option', {value: 'MAX'}, 'MAX'),
              ],
            ),
            m(ColumnPickerWithAlias, {
              value: agg.column,
              columns: [{name: '*'}, ...ctx.availableColumns],
              onSelect: (column) =>
                updateConfig({
                  aggregations: config.aggregations.with(i, {...agg, column}),
                }),
              alias: agg.alias,
              aliasPlaceholder: aggAlias(agg),
              onAliasChange: (alias) =>
                updateConfig({
                  aggregations: config.aggregations.with(i, {...agg, alias}),
                }),
            }),
          ]),
          onReorder: (from, to) =>
            updateConfig({
              aggregations: moveItem(config.aggregations, from, to),
            }),
          onDelete: (i) =>
            updateConfig({aggregations: config.aggregations.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Aggregation',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () => {
            updateConfig({
              aggregations: [
                ...config.aggregations,
                {func: 'COUNT', column: '*', alias: ''},
              ],
            });
          },
        }),
      ]);
    },
  };
}

function render(
  config: GroupByConfig,
  updateConfig: (updates: Partial<GroupByConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(GroupByContent, {config, updateConfig, ctx});
}

function getOutputColumns(
  config: GroupByConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  const groupCols: ColumnDef[] = config.groupColumns
    .filter((c) => c)
    .map((c) => columns?.find((col) => col.name === c) ?? {name: c});
  const aggAliases: ColumnDef[] = config.aggregations
    .filter((a) => a.column)
    .map((a) => {
      const name = aggAlias(a);
      if (a.func === 'COUNT') {
        return {name, type: {kind: 'int' as const}};
      }
      const orig = columns?.find((c) => c.name === a.column);
      return {name, type: orig?.type};
    });
  const result = [...groupCols, ...aggAliases];
  return result.length > 0 ? result : columns;
}

function isValid(config: GroupByConfig): boolean {
  const hasGroup = config.groupColumns.some((c) => c);
  return hasGroup;
}

function toLogicalOp(
  config: GroupByConfig,
  ctx: LogicalOpContext,
): LogicalOp | undefined {
  const input = ctx.getInputNodeId('input');
  if (!input) return undefined;
  return {
    kind: 'aggregate',
    input,
    groupColumns: config.groupColumns.filter((c) => c !== ''),
    aggregations: config.aggregations
      .filter((a) => a.column !== '')
      .map((a) => ({func: a.func, column: a.column, alias: aggAlias(a)})),
  };
}

export const manifest: NodeManifest<GroupByConfig> = {
  title: 'Group By',
  icon: 'workspaces',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 266,
  schema: GroupByConfigSchema,
  defaultConfig: () => ({groupColumns: [], aggregations: []}),
  render,
  getOutputColumns,
  isValid,
  toLogicalOp,
};
