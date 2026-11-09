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
import {
  type AggSpec,
  formatAggs,
  partitionBy,
  perClause,
  subquery,
} from '../generator_utils';
import type {
  ColumnContext,
  NodeGenerators,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

export type FlattenAggregation = PipelineAggEntry;

const IntervalFlattenConfigSchema = z
  .object({
    partitionColumns: z.array(z.string()),
    aggregations: z.array(PipelineAggEntrySchema),
    filterNegativeDur: z.boolean(),
  })
  .describe('Flatten overlapping intervals into disjoint slices.');

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

function flattenAggs(config: IntervalFlattenConfig): AggSpec[] {
  return config.aggregations
    .filter((a) => a.func === 'COUNT' || a.column !== '')
    .map((a) => ({
      func: a.func,
      column: a.func === 'COUNT' ? '*' : a.column,
      alias: pipelineAggAlias(a),
    }));
}

// Sweep-line over start/end events: running SUM of +1/-1 deltas gives the
// number of active intervals in each segment. MIN/MAX need the joined form.
function flattenSql(
  ref: string,
  partitionColumns: readonly string[],
  aggs: readonly AggSpec[],
): string {
  const uniquePer = [...new Set(partitionColumns)];
  const perPrefix = uniquePer.length > 0 ? `${uniquePer.join(', ')}, ` : '';
  const partBy = partitionBy(uniquePer);
  const windowSpec = `${partBy}ORDER BY ts ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW`;
  const src = `  _src AS (\n    SELECT * FROM ${ref} WHERE ts IS NOT NULL AND dur > 0\n  ),`;

  if (!aggs.some((a) => a.func === 'MIN' || a.func === 'MAX')) {
    const sums = aggs
      .map((a, idx) => ({a, idx}))
      .filter((x) => x.a.func === 'SUM');
    const startExtras = sums.map(
      ({a, idx}) =>
        `, IIF(${a.column} IS NOT NULL, ${a.column}, 0) AS _s${idx}, IIF(${a.column} IS NOT NULL, 1, 0) AS _c${idx}`,
    );
    const endExtras = sums.map(
      ({a, idx}) =>
        `, -IIF(${a.column} IS NOT NULL, ${a.column}, 0) AS _s${idx}, -IIF(${a.column} IS NOT NULL, 1, 0) AS _c${idx}`,
    );
    const mergedExtras = sums.map(
      ({idx}) => `, SUM(_s${idx}) AS _s${idx}, SUM(_c${idx}) AS _c${idx}`,
    );
    const segExtras = sums.map(
      ({idx}) =>
        `,\n      SUM(_s${idx}) OVER (${windowSpec}) AS _rs${idx},\n      SUM(_c${idx}) OVER (${windowSpec}) AS _rc${idx}`,
    );
    const outCols = ['ts', 'dur', ...partitionColumns];
    for (const [idx, a] of aggs.entries()) {
      outCols.push(
        a.func === 'COUNT'
          ? `_active AS ${a.alias}`
          : `IIF(_rc${idx} > 0, _rs${idx}, NULL) AS ${a.alias}`,
      );
    }
    return [
      'WITH',
      src,
      `  _events AS (\n    SELECT ${perPrefix}ts, 1 AS _delta${startExtras.join('')} FROM _src\n    UNION ALL\n    SELECT ${perPrefix}ts + dur AS ts, -1 AS _delta${endExtras.join('')} FROM _src\n  ),`,
      `  _merged AS (\n    SELECT ${perPrefix}ts, SUM(_delta) AS _delta${mergedExtras.join('')}\n    FROM _events\n    GROUP BY ${perPrefix}ts\n  ),`,
      `  _segs AS (\n    SELECT\n      ts,\n      LEAD(ts) OVER (${partBy}ORDER BY ts) - ts AS dur,\n      ${perPrefix}SUM(_delta) OVER (${windowSpec}) AS _active${segExtras.join('')}\n    FROM _merged\n  )`,
      `SELECT\n  ${outCols.join(',\n  ')}\nFROM _segs\nWHERE _active > 0 AND dur > 0`,
    ].join('\n');
  }

  const joinConds = [
    ...uniquePer.map((p) => `s.${p} IS r.${p}`),
    'r.ts <= s.ts',
    'r.ts + r.dur >= s.ts + s.dur',
  ].join(' AND ');
  const outCols = [
    's.ts',
    's.dur',
    ...partitionColumns.map((p) => `s.${p}`),
    ...aggs.map((a) =>
      a.func === 'COUNT'
        ? `COUNT(*) AS ${a.alias}`
        : `${a.func}(r.${a.column}) AS ${a.alias}`,
    ),
  ];
  return [
    'WITH',
    src,
    `  _events AS (\n    SELECT ${perPrefix}ts, 1 AS _delta FROM _src\n    UNION ALL\n    SELECT ${perPrefix}ts + dur AS ts, -1 AS _delta FROM _src\n  ),`,
    `  _merged AS (\n    SELECT ${perPrefix}ts, SUM(_delta) AS _delta\n    FROM _events\n    GROUP BY ${perPrefix}ts\n  ),`,
    `  _segs AS (\n    SELECT\n      ROW_NUMBER() OVER () AS _seg_id,\n      ts,\n      LEAD(ts) OVER (${partBy}ORDER BY ts) - ts AS dur,\n      ${perPrefix}SUM(_delta) OVER (${windowSpec}) AS _active\n    FROM _merged\n  )`,
    `SELECT\n  ${outCols.join(',\n  ')}\nFROM _segs AS s\nJOIN _src AS r ON ${joinConds}\nWHERE s._active > 0 AND s.dur > 0\nGROUP BY s._seg_id`,
  ].join('\n');
}

const generators: NodeGenerators<IntervalFlattenConfig> = {
  sql: (config, ctx) => {
    if (!isValid(config)) return undefined;
    const input = ctx.input('input');
    if (input === undefined) return undefined;
    const partition = config.partitionColumns.filter((c) => c !== '');
    return {
      from: subquery(flattenSql(input.ref, partition, flattenAggs(config))),
    };
  },
  psn: (config, ctx) => {
    if (!isValid(config)) return undefined;
    if (ctx.input('input') === undefined) return undefined;
    const per = perClause(config.partitionColumns.filter((c) => c !== ''));
    const aggs = flattenAggs(config);
    const negDur = config.filterNegativeDur ? ['WHERE dur >= 0'] : [];
    // INTERVAL FLATTEN requires at least one aggregate.
    const flatten =
      aggs.length > 0
        ? [`INTERVAL FLATTEN${per} AGGREGATE ${formatAggs(aggs)}`]
        : [
            `INTERVAL FLATTEN${per} AGGREGATE COUNT(*) AS _pf_cnt`,
            'DROP _pf_cnt',
          ];
    return {stages: [...negDur, ...flatten]};
  },
};

export const manifest: NodeManifest<IntervalFlattenConfig> = {
  title: 'Interval Flatten',
  icon: 'layers',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 321,
  expensive: true,
  schema: IntervalFlattenConfigSchema,
  defaultConfig: () => ({
    partitionColumns: [],
    aggregations: [{func: 'COUNT', column: '*', alias: 'count'}],
    filterNegativeDur: true,
  }),
  render: (config, updateConfig, ctx) =>
    m(IntervalFlattenContent, {config, updateConfig, ctx}),
  getOutputColumns,
  generators,
};
