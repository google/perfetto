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
import {RadioGroup} from '../../../widgets/radio_group';
import {Select} from '../../../widgets/select';
import {AliasTag} from '../components/alias_tag';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import type {ColumnDef} from '../graph_utils';
import type {
  IntervalMergeKind,
  LogicalOp,
  PipelineAggFunc,
} from '../logical_ir';
import type {
  ColumnContext,
  LogicalOpContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const MergeAggSchema = z.object({
  func: z.enum(['COUNT', 'SUM', 'MIN', 'MAX']),
  column: z.string(),
  alias: z.string(),
});

export type MergeAggregation = z.infer<typeof MergeAggSchema>;

const IntervalMergeConfigSchema = z.object({
  mergeKind: z.enum(['OVERLAPPING', 'CONSECUTIVE']),
  keyColumns: z.array(z.string()),
  partitionColumns: z.array(z.string()),
  aggregations: z.array(MergeAggSchema),
  filterNegativeDur: z.boolean(),
});

export type IntervalMergeConfig = z.infer<typeof IntervalMergeConfigSchema>;

function aggAlias(a: MergeAggregation): string {
  if (a.alias !== '') return a.alias;
  if (a.func === 'COUNT') return 'count';
  return `${a.func.toLowerCase()}_${a.column}`;
}

function IntervalMergeContent(): m.Component<{
  readonly config: IntervalMergeConfig;
  readonly updateConfig: (updates: Partial<IntervalMergeConfig>) => void;
  readonly ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, [
        m(
          RadioGroup,
          {
            fillWidth: true,
            selectedValue: config.mergeKind,
            onValueChange: (value: string) => {
              const nextKind = value as IntervalMergeKind;
              if (
                nextKind === 'CONSECUTIVE' &&
                config.keyColumns.length === 0
              ) {
                updateConfig({mergeKind: nextKind, keyColumns: ['']});
              } else {
                updateConfig({mergeKind: nextKind});
              }
            },
          },
          [
            m(RadioGroup.Button, {value: 'OVERLAPPING'}, 'Overlapping'),
            m(RadioGroup.Button, {value: 'CONSECUTIVE'}, 'Consecutive'),
          ],
        ),
        m(Checkbox, {
          label: 'Filter dur >= 0',
          checked: config.filterNegativeDur,
          onchange: () =>
            updateConfig({filterNegativeDur: !config.filterNegativeDur}),
        }),
        config.mergeKind === 'CONSECUTIVE' && [
          m('.pf-spag-section-label', 'Merge by'),
          m(Stack, {compact: true}, [
            ...config.keyColumns.map((col, i) =>
              m(Row, {key: i}, [
                m(ColumnPicker, {
                  value: col,
                  columns: ctx.availableColumns,
                  placeholder: 'key column',
                  onSelect: (value: string) => {
                    const updated = [...config.keyColumns];
                    updated[i] = value;
                    updateConfig({keyColumns: updated});
                  },
                }),
                m(Row.DeleteButton, {
                  onclick: () =>
                    updateConfig({
                      keyColumns: config.keyColumns.filter((_, j) => j !== i),
                    }),
                }),
              ]),
            ),
          ]),
          m(Button, {
            label: 'Key',
            icon: 'add',
            variant: ButtonVariant.Filled,
            onclick: () =>
              updateConfig({
                keyColumns: [...config.keyColumns, ''],
              }),
          }),
        ],
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
  config: IntervalMergeConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  if (columns === undefined) return undefined;
  const nonEmptyPer = config.partitionColumns.filter((c) => c !== '');
  const partCols: ColumnDef[] = nonEmptyPer.map(
    (c) => columns.find((col) => col.name === c) ?? {name: c},
  );
  const keyCols: ColumnDef[] =
    config.mergeKind === 'CONSECUTIVE'
      ? config.keyColumns
          .filter((c) => c !== '' && !nonEmptyPer.includes(c))
          .map((c) => columns.find((col) => col.name === c) ?? {name: c})
      : [];
  const aggCols: ColumnDef[] = config.aggregations
    .filter((a) => a.func === 'COUNT' || a.column !== '')
    .map((a) => ({name: aggAlias(a), type: {kind: 'int' as const}}));
  return [
    {name: 'ts', type: {kind: 'timestamp' as const}},
    {name: 'dur', type: {kind: 'duration' as const}},
    ...partCols,
    ...keyCols,
    ...aggCols,
  ];
}

function isValid(config: IntervalMergeConfig): boolean {
  const aggsValid = config.aggregations.every(
    (a) => a.func === 'COUNT' || a.column !== '',
  );
  if (!aggsValid) return false;
  if (config.mergeKind === 'CONSECUTIVE') {
    return (
      config.keyColumns.some((k) => k !== '') &&
      config.keyColumns.every((k) => k !== '')
    );
  }
  return true;
}

function toLogicalOp(
  config: IntervalMergeConfig,
  ctx: LogicalOpContext,
): LogicalOp | undefined {
  const input = ctx.getInputNodeId('input');
  if (input === undefined || !isValid(config)) return undefined;
  return {
    kind: 'interval_merge',
    input,
    mergeKind: config.mergeKind,
    keyColumns:
      config.mergeKind === 'CONSECUTIVE'
        ? config.keyColumns.filter((c) => c !== '')
        : [],
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

export const manifest: NodeManifest<IntervalMergeConfig> = {
  title: 'Interval Merge',
  icon: 'compress',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 330,
  schema: IntervalMergeConfigSchema,
  defaultConfig: () => ({
    mergeKind: 'OVERLAPPING',
    keyColumns: [],
    partitionColumns: [],
    aggregations: [{func: 'COUNT', column: '*', alias: 'count'}],
    filterNegativeDur: true,
  }),
  render: (config, updateConfig, ctx) =>
    m(IntervalMergeContent, {config, updateConfig, ctx}),
  getOutputColumns,
  isValid,
  toLogicalOp,
};
