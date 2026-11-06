// Copyright (C) 2026 The Android Open Source Project
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
import {Checkbox} from '../../../widgets/checkbox';
import {DraggableStack} from '../components/draggable_stack';
import {
  pipelineAggAlias,
  type PipelineAggEntry,
  PipelineAggEntrySchema,
  PipelineAggRow,
} from '../components/pipeline_agg_row';
import {Stack} from '../components/stack';
import type {ColumnDef} from '../graph_utils';
import type {LogicalOp} from '../logical_ir';
import type {
  ColumnContext,
  LogicalOpContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

export type FlattenAggregation = PipelineAggEntry;

const IntervalFlattenConfigSchema = z.object({
  partitionColumns: z.array(z.string()),
  aggregations: z.array(PipelineAggEntrySchema),
  filterNegativeDur: z.boolean(),
});

export type IntervalFlattenConfig = z.infer<typeof IntervalFlattenConfigSchema>;

function IntervalFlattenContent(): m.Component<{
  config: IntervalFlattenConfig;
  updateConfig: (updates: Partial<IntervalFlattenConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, {minWidth: 'lg'}, [
        m(Checkbox, {
          label: 'Filter dur >= 0',
          checked: config.filterNegativeDur,
          onchange: () =>
            updateConfig({filterNegativeDur: !config.filterNegativeDur}),
        }),
        m('.pf-spag-section-label', 'Partition by'),
        m(DraggableStack, {
          rows: config.partitionColumns.map((col, i) =>
            m(ColumnPicker, {
              value: col,
              columns: ctx.availableColumns,
              placeholder: 'column',
              onSelect: (value) =>
                updateConfig({
                  partitionColumns: config.partitionColumns.with(i, value),
                }),
            }),
          ),
          onReorder: (from, to) =>
            updateConfig({
              partitionColumns: moveItem(config.partitionColumns, from, to),
            }),
          onDelete: (i) =>
            updateConfig({
              partitionColumns: config.partitionColumns.toSpliced(i, 1),
            }),
        }),
        m(Button, {
          label: 'Partition',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () =>
            updateConfig({
              partitionColumns: [...config.partitionColumns, ''],
            }),
        }),
        m('.pf-spag-section-label', 'Aggregations'),
        m(DraggableStack, {
          rows: config.aggregations.map((agg, i) =>
            m(PipelineAggRow, {
              value: agg,
              columns: ctx.availableColumns,
              onChange: (updated) =>
                updateConfig({
                  aggregations: config.aggregations.with(i, updated),
                }),
            }),
          ),
          onReorder: (from, to) =>
            updateConfig({
              aggregations: moveItem(config.aggregations, from, to),
            }),
          onDelete: (i) =>
            updateConfig({
              aggregations: config.aggregations.toSpliced(i, 1),
            }),
        }),
        m(Button, {
          label: 'Aggregation',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () =>
            updateConfig({
              aggregations: [
                ...config.aggregations,
                {func: 'COUNT', column: '*', alias: ''},
              ],
            }),
        }),
      ]);
    },
  };
}

function getOutputColumns(
  config: IntervalFlattenConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  if (columns === undefined) return undefined;
  const partCols: ColumnDef[] = config.partitionColumns
    .filter((c) => c !== '')
    .map((c) => columns.find((col) => col.name === c) ?? {name: c});
  const aggCols: ColumnDef[] = config.aggregations
    .filter((a) => a.func === 'COUNT' || a.column !== '')
    .map((a) => ({name: pipelineAggAlias(a), type: {kind: 'int' as const}}));
  return [
    {name: 'ts', type: {kind: 'timestamp' as const}},
    {name: 'dur', type: {kind: 'duration' as const}},
    ...partCols,
    ...aggCols,
  ];
}

function isValid(config: IntervalFlattenConfig): boolean {
  return config.aggregations.every(
    (a) => a.func === 'COUNT' || a.column !== '',
  );
}

function toLogicalOp(
  config: IntervalFlattenConfig,
  ctx: LogicalOpContext,
): LogicalOp | undefined {
  const input = ctx.getInputNodeId('input');
  if (!input) return undefined;
  return {
    kind: 'interval_flatten',
    input,
    partitionColumns: config.partitionColumns.filter((c) => c !== ''),
    aggregations: config.aggregations
      .filter((a) => a.func === 'COUNT' || a.column !== '')
      .map((a) => ({
        func: a.func,
        column: a.func === 'COUNT' ? '*' : a.column,
        alias: pipelineAggAlias(a),
      })),
    filterNegativeDur: config.filterNegativeDur,
  };
}

export const manifest: NodeManifest<IntervalFlattenConfig> = {
  title: 'Interval Flatten',
  icon: 'layers',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 321,
  schema: IntervalFlattenConfigSchema,
  defaultConfig: () => ({
    partitionColumns: [],
    aggregations: [{func: 'COUNT', column: '*', alias: 'count'}],
    filterNegativeDur: true,
  }),
  render: (config, updateConfig, ctx) =>
    m(IntervalFlattenContent, {config, updateConfig, ctx}),
  getOutputColumns,
  isValid,
  toLogicalOp,
};
