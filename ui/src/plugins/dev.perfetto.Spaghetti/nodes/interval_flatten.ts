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
import {Button, ButtonVariant} from '../../../widgets/button';
import {Checkbox} from '../../../widgets/checkbox';
import {Select} from '../../../widgets/select';
import {AliasTag} from '../components/alias_tag';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import type {ColumnDef} from '../graph_utils';
import type {LogicalOp, PipelineAggFunc} from '../logical_ir';
import type {
  ColumnContext,
  LogicalOpContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const FlattenAggSchema = z.object({
  func: z.enum(['COUNT', 'SUM', 'MIN', 'MAX']),
  column: z.string(),
  alias: z.string(),
});

export type FlattenAggregation = z.infer<typeof FlattenAggSchema>;

const IntervalFlattenConfigSchema = z.object({
  partitionColumns: z.array(z.string()),
  aggregations: z.array(FlattenAggSchema),
  filterNegativeDur: z.boolean(),
});

export type IntervalFlattenConfig = z.infer<typeof IntervalFlattenConfigSchema>;

function aggAlias(a: FlattenAggregation): string {
  if (a.alias !== '') return a.alias;
  if (a.func === 'COUNT') return 'count';
  return `${a.func.toLowerCase()}_${a.column}`;
}

function IntervalFlattenContent(): m.Component<{
  config: IntervalFlattenConfig;
  updateConfig: (updates: Partial<IntervalFlattenConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, {style: {minWidth: '250px'}}, [
        m(Checkbox, {
          label: 'Filter dur >= 0',
          checked: config.filterNegativeDur,
          onchange: () =>
            updateConfig({filterNegativeDur: !config.filterNegativeDur}),
        }),
        m('.pf-spag-section-label', 'Partition by'),
        m(Stack, {compact: true}, [
          ...config.partitionColumns.map((col, i) =>
            m(Row, {key: i}, [
              m(ColumnPicker, {
                value: col,
                columns: ctx.availableColumns,
                placeholder: 'column',
                onSelect: (value: string) => {
                  const updated = [...config.partitionColumns];
                  updated[i] = value;
                  updateConfig({partitionColumns: updated});
                },
              }),
              m(Row.DeleteButton, {
                onclick: () =>
                  updateConfig({
                    partitionColumns: config.partitionColumns.filter(
                      (_, j) => j !== i,
                    ),
                  }),
              }),
            ]),
          ),
        ]),
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
        m(Stack, {compact: true}, [
          ...config.aggregations.map((agg, i) =>
            m(Row, {key: i}, [
              m(
                Select,
                {
                  value: agg.func,
                  onchange: (e: Event) => {
                    const nextFunc = (e.target as HTMLSelectElement)
                      .value as PipelineAggFunc;
                    const updated = [...config.aggregations];
                    updated[i] = {
                      ...agg,
                      func: nextFunc,
                      column:
                        nextFunc === 'COUNT'
                          ? '*'
                          : agg.column === '*'
                            ? ''
                            : agg.column,
                    };
                    updateConfig({aggregations: updated});
                  },
                },
                [
                  m('option', {value: 'COUNT'}, 'COUNT'),
                  m('option', {value: 'SUM'}, 'SUM'),
                  m('option', {value: 'MIN'}, 'MIN'),
                  m('option', {value: 'MAX'}, 'MAX'),
                ],
              ),
              agg.func !== 'COUNT' &&
                m(ColumnPicker, {
                  value: agg.column,
                  columns: ctx.availableColumns,
                  placeholder: 'column',
                  onSelect: (value: string) => {
                    const updated = [...config.aggregations];
                    updated[i] = {...agg, column: value};
                    updateConfig({aggregations: updated});
                  },
                }),
              m(AliasTag, {
                alias: agg.alias,
                placeholder: aggAlias(agg),
                onChange: (value: string) => {
                  const updated = [...config.aggregations];
                  updated[i] = {...agg, alias: value};
                  updateConfig({aggregations: updated});
                },
              }),
              m(Row.DeleteButton, {
                onclick: () =>
                  updateConfig({
                    aggregations: config.aggregations.filter((_, j) => j !== i),
                  }),
              }),
            ]),
          ),
        ]),
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
    .map((a) => ({name: aggAlias(a), type: {kind: 'int' as const}}));
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
        alias: aggAlias(a),
      })),
    filterNegativeDur: config.filterNegativeDur,
  };
}

export const manifest: NodeManifest<IntervalFlattenConfig> = {
  title: 'Interval Flatten',
  icon: 'layers',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 350,
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
