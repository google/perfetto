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
import {RadioGroup} from '../../../widgets/radio_group';
import {DraggableStack} from '../components/draggable_stack';
import {
  pipelineAggAlias,
  type PipelineAggEntry,
  PipelineAggEntrySchema,
  PipelineAggRow,
} from '../components/pipeline_agg_row';
import {Stack} from '../components/stack';
import type {ColumnDef} from '../graph_utils';
import type {IntervalMergeKind} from '../sql_types';
import {
  type AggSpec,
  formatAggs,
  partitionBy,
  perClause,
  subquery,
} from '../generator_utils';
import type {NodeManifest, RenderContext} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

export type MergeAggregation = PipelineAggEntry;

const IntervalMergeConfigSchema = z
  .object({
    mergeKind: z.enum(['OVERLAPPING', 'CONSECUTIVE']),
    keyColumns: z.array(z.string()),
    partitionColumns: z.array(z.string()),
    aggregations: z.array(PipelineAggEntrySchema),
    filterNegativeDur: z.boolean(),
  })
  .describe('Merge overlapping or consecutive intervals.');

export type IntervalMergeConfig = z.infer<typeof IntervalMergeConfigSchema>;

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
          m(DraggableStack, {
            rows: config.keyColumns.map((col, i) =>
              m(ColumnPicker, {
                value: col,
                columns: ctx.availableColumns,
                placeholder: 'key column',
                onSelect: (value) =>
                  updateConfig({keyColumns: config.keyColumns.with(i, value)}),
              }),
            ),
            onReorder: (from, to) =>
              updateConfig({keyColumns: moveItem(config.keyColumns, from, to)}),
            onDelete: (i) =>
              updateConfig({keyColumns: config.keyColumns.toSpliced(i, 1)}),
          }),
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

function mergeAggs(config: IntervalMergeConfig): AggSpec[] {
  return config.aggregations
    .filter((a) => a.func === 'COUNT' || a.column !== '')
    .map((a) => ({
      func: a.func,
      column: a.func === 'COUNT' ? '*' : a.column,
      alias: pipelineAggAlias(a),
    }));
}

function mergeKeys(config: IntervalMergeConfig): string[] {
  return config.mergeKind === 'CONSECUTIVE'
    ? config.keyColumns.filter((c) => c !== '')
    : [];
}

// Gaps-and-islands: a running group id starts a new group whenever the
// interval doesn't overlap (OVERLAPPING) / abut with equal keys (CONSECUTIVE)
// the previous one; each group collapses to a single interval.
function mergeSql(ref: string, config: IntervalMergeConfig): string {
  const partition = config.partitionColumns.filter((c) => c !== '');
  const uniquePer = [...new Set(partition)];
  const partBy = partitionBy(uniquePer);
  const where = config.filterNegativeDur
    ? 'WHERE ts IS NOT NULL AND dur >= 0'
    : 'WHERE ts IS NOT NULL';
  const aggExprs = mergeAggs(config).map((a) =>
    a.func === 'COUNT'
      ? `COUNT(*) AS ${a.alias}`
      : `${a.func}(${a.column}) AS ${a.alias}`,
  );
  const running = `${partBy}ORDER BY ts, dur ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW`;

  if (config.mergeKind === 'OVERLAPPING') {
    const outCols = [
      'MIN(ts) AS ts',
      'MAX(ts + dur) - MIN(ts) AS dur',
      ...partition,
      ...aggExprs,
    ];
    return [
      'WITH',
      `  _bounds AS (\n    SELECT\n      *,\n      MAX(ts + dur) OVER (${partBy}ORDER BY ts, dur ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) AS _max_end\n    FROM ${ref}\n    ${where}\n  ),`,
      `  _groups AS (\n    SELECT\n      *,\n      SUM(IIF(_max_end IS NULL OR ts > _max_end, 1, 0)) OVER (${running}) AS _grp\n    FROM _bounds\n  )`,
      `SELECT\n  ${outCols.join(',\n  ')}\nFROM _groups\nGROUP BY ${[...uniquePer, '_grp'].join(', ')}`,
    ].join('\n');
  }

  const keys = mergeKeys(config);
  const uniqueKeys = [...new Set(keys)];
  const prevKeys = uniqueKeys.map(
    (k, idx) =>
      `,\n      LAG(${k}) OVER (${partBy}ORDER BY ts, dur) AS _prev_k${idx}`,
  );
  const keySame = uniqueKeys
    .map((k, idx) => `${k} IS _prev_k${idx}`)
    .join(' AND ');
  const newGrp = `_prev_end IS NULL OR ts != _prev_end${keySame !== '' ? ` OR NOT (${keySame})` : ''}`;
  const groupCols = [
    ...uniquePer,
    '_grp',
    ...uniqueKeys.filter((k) => !uniquePer.includes(k)),
  ];
  const outCols = [
    'MIN(ts) AS ts',
    'MAX(ts + dur) - MIN(ts) AS dur',
    ...partition,
    ...keys.filter((k) => !partition.includes(k)),
    ...aggExprs,
  ];
  return [
    'WITH',
    `  _prev AS (\n    SELECT\n      *,\n      LAG(ts + dur) OVER (${partBy}ORDER BY ts, dur) AS _prev_end${prevKeys.join('')}\n    FROM ${ref}\n    ${where}\n  ),`,
    `  _groups AS (\n    SELECT\n      *,\n      SUM(IIF(${newGrp}, 1, 0)) OVER (${running}) AS _grp\n    FROM _prev\n  )`,
    `SELECT\n  ${outCols.join(',\n  ')}\nFROM _groups\nGROUP BY ${groupCols.join(', ')}`,
  ].join('\n');
}

export const manifest: NodeManifest<IntervalMergeConfig> = {
  title: 'Interval Merge',
  icon: 'compress',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 330,
  schema: IntervalMergeConfigSchema,
  expensive: true,
  defaultConfig: () => ({
    mergeKind: 'OVERLAPPING',
    keyColumns: [],
    partitionColumns: [],
    aggregations: [{func: 'COUNT', column: '*', alias: 'count'}],
    filterNegativeDur: true,
  }),
  render: (config, updateConfig, ctx) =>
    m(IntervalMergeContent, {config, updateConfig, ctx}),
  getOutputColumns(config, ctx) {
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
      .map((a) => ({name: pipelineAggAlias(a), type: {kind: 'int' as const}}));
    return [
      {name: 'ts', type: {kind: 'timestamp' as const}},
      {name: 'dur', type: {kind: 'duration' as const}},
      ...partCols,
      ...keyCols,
      ...aggCols,
    ];
  },
  generators: {
    sql: (config, ctx) => {
      const input = ctx.input('input');
      if (input === undefined || !isValid(config)) return undefined;
      return {from: subquery(mergeSql(input.ref, config))};
    },
    psn: (config, ctx) => {
      if (ctx.input('input') === undefined || !isValid(config)) {
        return undefined;
      }
      const keys = mergeKeys(config);
      const by = keys.length > 0 ? ` BY ${keys.join(', ')}` : '';
      const per = perClause(config.partitionColumns.filter((c) => c !== ''));
      const head = `INTERVAL MERGE ${config.mergeKind}${by}${per}`;
      const aggs = mergeAggs(config);
      const negDur = config.filterNegativeDur ? ['WHERE dur >= 0'] : [];
      // INTERVAL MERGE requires at least one aggregate.
      const merge =
        aggs.length > 0
          ? [`${head} AGGREGATE ${formatAggs(aggs)}`]
          : [`${head} AGGREGATE COUNT(*) AS _pf_cnt`, 'DROP _pf_cnt'];
      return {stages: [...negDur, ...merge]};
    },
  },
};
