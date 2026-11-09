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
import {DraggableStack} from '../components/draggable_stack';
import {Stack} from '../components/stack';
import type {ColumnDef} from '../graph_utils';
import {partitionBy, perClause, subquery} from '../generator_utils';
import type {
  ColumnContext,
  NodeGenerators,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const IntervalFillGapsConfigSchema = z
  .object({
    partitionColumns: z.array(z.string()),
  })
  .describe('Fill gaps in input intervals within background intervals.');

export type IntervalFillGapsConfig = z.infer<
  typeof IntervalFillGapsConfigSchema
>;

function IntervalFillGapsContent(): m.Component<{
  config: IntervalFillGapsConfig;
  updateConfig: (updates: Partial<IntervalFillGapsConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const inputColumns = ctx.getInputColumns('input');
      return m(Stack, {minWidth: 'sm'}, [
        m('.pf-spag-section-label', 'Partition by'),
        m(DraggableStack, {
          rows: config.partitionColumns.map((col, i) =>
            m(ColumnPicker, {
              value: col,
              columns: inputColumns,
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
      ]);
    },
  };
}

function getOutputColumns(
  _config: IntervalFillGapsConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  return ctx.getInputColumns('input');
}

// Islands of foreground coverage per lane, the gaps between them, and the
// background clipped to those gaps, UNIONed back onto the foreground rows.
function fillGapsSql(
  fgRef: string,
  bgRef: string,
  partitionColumns: readonly string[],
  inputColumns: readonly string[] | undefined,
  backgroundColumns: readonly string[] | undefined,
): string {
  const uniquePer = [...new Set(partitionColumns)];
  const perPrefix = uniquePer.length > 0 ? `${uniquePer.join(', ')}, ` : '';
  const partBy = partitionBy(uniquePer);
  const partOnly = partBy.trimEnd();
  const bgCols = backgroundColumns ?? [];
  const bgHasPer =
    uniquePer.length === 0 ||
    (backgroundColumns !== undefined &&
      uniquePer.every((p) => bgCols.includes(p)));

  const ctes: string[] = [];
  if (bgHasPer) {
    ctes.push(
      `  _bg AS (\n    SELECT * FROM ${bgRef} WHERE ts IS NOT NULL AND dur > 0\n  )`,
    );
  } else {
    // Background has no lane columns: replicate it across every lane.
    const bgExtra = bgCols.filter(
      (c) => c !== 'ts' && c !== 'dur' && !uniquePer.includes(c),
    );
    const bgProj = [
      ...uniquePer.map((p) => `l.${p}`),
      'b.ts',
      'b.dur',
      ...bgExtra.map((c) => `b.${c}`),
    ].join(', ');
    ctes.push(
      `  _lanes AS (\n    SELECT DISTINCT ${uniquePer.join(', ')} FROM ${fgRef}\n  )`,
      `  _bg AS (\n    SELECT ${bgProj}\n    FROM _lanes AS l\n    CROSS JOIN (SELECT * FROM ${bgRef} WHERE ts IS NOT NULL AND dur > 0) AS b\n  )`,
    );
  }

  const maxInt64 = '9223372036854775807';
  const notExistsWhere =
    uniquePer.length > 0
      ? ` WHERE ${uniquePer.map((p) => `i.${p} IS b.${p}`).join(' AND ')}`
      : '';
  ctes.push(
    `  _fg_valid AS (\n    SELECT\n      ${perPrefix}ts AS _s,\n      IIF(dur = -1, ${maxInt64}, ts + dur) AS _e\n    FROM ${fgRef}\n    WHERE ts IS NOT NULL AND (dur > 0 OR dur = -1)\n  )`,
    `  _fg_max AS (\n    SELECT\n      *,\n      MAX(_e) OVER (${partBy}ORDER BY _s ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) AS _prev_max\n    FROM _fg_valid\n  )`,
    `  _fg_grp AS (\n    SELECT\n      *,\n      SUM(IIF(_prev_max IS NULL OR _s > _prev_max, 1, 0)) OVER (${partBy}ORDER BY _s ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS _isl\n    FROM _fg_max\n  )`,
    `  _islands AS (\n    SELECT\n      ${perPrefix}MIN(_s) AS _s,\n      MAX(_e) AS _e,\n      ROW_NUMBER() OVER (${partBy}ORDER BY MIN(_s)) AS _rn,\n      COUNT(*) OVER (${partOnly}) AS _cnt\n    FROM _fg_grp\n    GROUP BY ${perPrefix}_isl\n  )`,
    `  _isl_prev AS (\n    SELECT\n      *,\n      LAG(_e) OVER (${partBy}ORDER BY _s) AS _prev_e\n    FROM _islands\n  )`,
    [
      '  _gaps AS (',
      `    SELECT ${perPrefix}COALESCE(_prev_e, 0) AS _gs, _s AS _ge FROM _isl_prev WHERE _s > COALESCE(_prev_e, 0)`,
      '    UNION ALL',
      `    SELECT ${perPrefix}_e AS _gs, ${maxInt64} AS _ge FROM _isl_prev WHERE _rn = _cnt AND _e < ${maxInt64}`,
      '    UNION ALL',
      `    SELECT DISTINCT ${uniquePer.map((p) => `b.${p}, `).join('')}0 AS _gs, ${maxInt64} AS _ge FROM _bg AS b WHERE NOT EXISTS (SELECT 1 FROM _islands AS i${notExistsWhere})`,
      '  )',
    ].join('\n'),
  );

  const outSchema = inputColumns ?? ['ts', 'dur', ...uniquePer];
  const availableOnBg = new Set([...bgCols, ...uniquePer]);
  const fillerProj = outSchema.map((col) => {
    if (col === 'ts') return 'MAX(g._gs, b.ts) AS ts';
    if (col === 'dur') {
      return 'MIN(g._ge, b.ts + b.dur) - MAX(g._gs, b.ts) AS dur';
    }
    if (availableOnBg.has(col)) return `b.${col} AS ${col}`;
    return `NULL AS ${col}`;
  });
  const joinCond =
    uniquePer.length > 0
      ? `ON ${uniquePer.map((p) => `b.${p} IS g.${p}`).join(' AND ')}\n`
      : '';

  return [
    'WITH',
    ctes.join(',\n'),
    `SELECT *\nFROM ${fgRef}`,
    'UNION ALL',
    `SELECT\n  ${fillerProj.join(',\n  ')}\nFROM _bg AS b\nJOIN _gaps AS g ${joinCond}WHERE MIN(g._ge, b.ts + b.dur) > MAX(g._gs, b.ts)`,
  ].join('\n');
}

const generators: NodeGenerators<IntervalFillGapsConfig> = {
  // No background input -> passthrough of the foreground.
  sql: (config, ctx) => {
    const input = ctx.input('input');
    if (input === undefined) return undefined;
    const background = ctx.input('background');
    if (background === undefined) return {};
    return {
      from: subquery(
        fillGapsSql(
          input.ref,
          background.ref,
          config.partitionColumns.filter((c) => c !== ''),
          ctx.getInputColumns('input')?.map((c) => c.name),
          ctx.getInputColumns('background')?.map((c) => c.name),
        ),
      ),
    };
  },
  psn: (config, ctx) => {
    if (ctx.input('input') === undefined) return undefined;
    const background = ctx.input('background');
    if (background === undefined) return {};
    const partition = config.partitionColumns.filter((c) => c !== '');
    // Narrow the background to columns the foreground also has, so the
    // filled rows line up with the input schema.
    const inputCols = ctx.getInputColumns('input')?.map((c) => c.name);
    const bgCols = ctx.getInputColumns('background')?.map((c) => c.name);
    let bgSource = background.source;
    if (inputCols !== undefined && bgCols !== undefined) {
      const inputSet = new Set(inputCols);
      const bgHasAllPer =
        partition.length > 0 && partition.every((p) => bgCols.includes(p));
      const kept = bgCols.filter(
        (c) =>
          inputSet.has(c) &&
          (partition.length === 0 || bgHasAllPer || !partition.includes(c)),
      );
      if (kept.length > 0 && kept.length < bgCols.length) {
        bgSource = background.withStages([`SELECT ${kept.join(', ')}`]);
      }
    }
    return {
      stages: [`INTERVAL FILL GAPS WITH ${bgSource}${perClause(partition)}`],
    };
  },
};

export const manifest: NodeManifest<IntervalFillGapsConfig> = {
  title: 'Fill Gaps',
  icon: 'space_bar',
  getInputs: () => [
    {name: 'input', content: 'Input'},
    {name: 'background', content: 'Background'},
  ],
  hue: 339,
  expensive: true,
  schema: IntervalFillGapsConfigSchema,
  defaultConfig: () => ({partitionColumns: []}),
  getOutputColumns,
  generators,
  render: (config, updateConfig, ctx) =>
    m(IntervalFillGapsContent, {config, updateConfig, ctx}),
};
