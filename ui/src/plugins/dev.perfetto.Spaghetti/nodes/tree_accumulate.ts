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
import {RadioGroup} from '../../../widgets/radio_group';
import {DraggableStack} from '../components/draggable_stack';
import {
  pipelineAggAlias,
  type PipelineAggEntry,
  PipelineAggEntrySchema,
  PipelineAggRow,
} from '../components/pipeline_agg_row';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import type {ColumnDef} from '../graph_utils';
import {type AggSpec, formatAggs, subquery} from '../generator_utils';
import type {
  ColumnContext,
  NodeGenerators,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

export type TreeAggregation = PipelineAggEntry;

const TreeAccumulateConfigSchema = z
  .object({
    direction: z.enum(['UP', 'DOWN']),
    idColumn: z.string(),
    parentIdColumn: z.string(),
    aggregations: z.array(PipelineAggEntrySchema),
  })
  .describe(
    'Accumulate aggregations UP (subtree) or DOWN (ancestors) over (idColumn, parentIdColumn).',
  );

export type TreeAccumulateConfig = z.infer<typeof TreeAccumulateConfigSchema>;

function aggAlias(a: TreeAggregation, direction: 'UP' | 'DOWN'): string {
  return pipelineAggAlias(a, direction === 'UP' ? 'subtree' : 'path');
}

function TreeAccumulateContent(): m.Component<{
  config: TreeAccumulateConfig;
  updateConfig: (updates: Partial<TreeAccumulateConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, {minWidth: 'lg'}, [
        m(
          RadioGroup,
          {
            fillWidth: true,
            selectedValue: config.direction,
            onValueChange: (value: string) =>
              updateConfig({direction: value as 'UP' | 'DOWN'}),
          },
          [
            m(RadioGroup.Button, {value: 'UP'}, 'Up (Subtree)'),
            m(RadioGroup.Button, {value: 'DOWN'}, 'Down (Ancestors)'),
          ],
        ),
        m('.pf-spag-section-label', 'Tree columns'),
        m(Row, [
          m(ColumnPicker, {
            value: config.idColumn,
            columns: ctx.availableColumns,
            placeholder: 'id',
            onSelect: (value: string) => updateConfig({idColumn: value}),
          }),
          m('span.pf-spag-operator', '→'),
          m(ColumnPicker, {
            value: config.parentIdColumn,
            columns: ctx.availableColumns,
            placeholder: 'parent_id',
            onSelect: (value: string) => updateConfig({parentIdColumn: value}),
          }),
        ]),
        m('.pf-spag-section-label', 'Aggregations'),
        m(DraggableStack, {
          rows: config.aggregations.map((agg, i) =>
            m(PipelineAggRow, {
              value: agg,
              columns: ctx.availableColumns,
              aliasPlaceholder: aggAlias(agg, config.direction),
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
  config: TreeAccumulateConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  if (columns === undefined) return undefined;
  const added: ColumnDef[] = config.aggregations
    .filter((a) => a.func === 'COUNT' || a.column !== '')
    .map((a) => ({
      name: aggAlias(a, config.direction),
      type: {kind: 'int' as const},
    }));
  return [...columns, ...added];
}

function isValid(config: TreeAccumulateConfig): boolean {
  return (
    config.idColumn !== '' &&
    config.parentIdColumn !== '' &&
    config.aggregations.some((a) => a.func === 'COUNT' || a.column !== '') &&
    config.aggregations.every((a) => a.func === 'COUNT' || a.column !== '')
  );
}

function treeAggs(config: TreeAccumulateConfig): AggSpec[] {
  return config.aggregations.map((a) => ({
    func: a.func,
    column: a.func === 'COUNT' ? '*' : a.column,
    alias: aggAlias(a, config.direction),
  }));
}

// Recursive closure of (anchor, member) pairs, aggregated per anchor (UP:
// over each node's subtree) or per member (DOWN: over each node's ancestors).
function treeAccumulateSql(
  ref: string,
  config: TreeAccumulateConfig,
  aggs: readonly AggSpec[],
): string {
  const {idColumn, parentIdColumn, direction} = config;
  const keyExpr = direction === 'UP' ? 'c._anchor' : 'c._member';
  const valJoinKey = direction === 'UP' ? 'c._member' : 'c._anchor';
  const aggExprs = aggs.map((a) =>
    a.func === 'COUNT'
      ? `COUNT(*) AS ${a.alias}`
      : `${a.func}(v.${a.column}) AS ${a.alias}`,
  );
  const outerCols = aggs.map((a) => `_agg.${a.alias}`);
  return [
    'WITH RECURSIVE',
    `  _closure(_anchor, _member) AS (\n    SELECT ${idColumn}, ${idColumn} FROM ${ref}\n    UNION ALL\n    SELECT c._anchor, child.${idColumn}\n    FROM _closure AS c\n    JOIN ${ref} AS child ON child.${parentIdColumn} = c._member\n  ),`,
    `  _agg AS (\n    SELECT\n      ${keyExpr} AS _key_id,\n      ${aggExprs.join(',\n      ')}\n    FROM _closure AS c\n    JOIN ${ref} AS v ON v.${idColumn} = ${valJoinKey}\n    GROUP BY ${keyExpr}\n  )`,
    `SELECT\n  src.*,\n  ${outerCols.join(',\n  ')}\nFROM ${ref} AS src\nLEFT JOIN _agg ON src.${idColumn} = _agg._key_id`,
  ].join('\n');
}

const generators: NodeGenerators<TreeAccumulateConfig> = {
  sql: (config, ctx) => {
    const input = ctx.input('input');
    if (input === undefined || !isValid(config)) return undefined;
    const aggs = treeAggs(config);
    if (aggs.length === 0) return {};
    return {from: subquery(treeAccumulateSql(input.ref, config, aggs))};
  },
  // TREE ACCUMULATE expects `id` / `parent_id`: rename in and back out.
  psn: (config, ctx) => {
    if (ctx.input('input') === undefined || !isValid(config)) return undefined;
    const aggs = treeAggs(config);
    if (aggs.length === 0) return {};
    const renamesIn: string[] = [];
    const renamesOut: string[] = [];
    if (config.idColumn !== 'id') {
      renamesIn.push(`${config.idColumn} AS id`);
      renamesOut.push(`id AS ${config.idColumn}`);
    }
    if (config.parentIdColumn !== 'parent_id') {
      renamesIn.push(`${config.parentIdColumn} AS parent_id`);
      renamesOut.push(`parent_id AS ${config.parentIdColumn}`);
    }
    return {
      stages: [
        ...(renamesIn.length > 0 ? [`RENAME ${renamesIn.join(', ')}`] : []),
        `TREE ACCUMULATE ${config.direction} ${formatAggs(aggs)}`,
        ...(renamesOut.length > 0 ? [`RENAME ${renamesOut.join(', ')}`] : []),
      ],
    };
  },
};

export const manifest: NodeManifest<TreeAccumulateConfig> = {
  title: 'Tree Accumulate',
  icon: 'mediation',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 94,
  expensive: true,
  schema: TreeAccumulateConfigSchema,
  defaultConfig: () => ({
    direction: 'UP',
    idColumn: 'id',
    parentIdColumn: 'parent_id',
    aggregations: [{func: 'COUNT', column: '*', alias: ''}],
  }),
  render: (config, updateConfig, ctx) =>
    m(TreeAccumulateContent, {config, updateConfig, ctx}),
  getOutputColumns,
  isValid,
  generators,
};
