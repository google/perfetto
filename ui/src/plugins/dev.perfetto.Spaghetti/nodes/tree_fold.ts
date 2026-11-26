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
import {Stack} from '../components/stack';
import {TreeColumnsRow} from '../components/tree_columns_row';
import {
  type AggSpec,
  formatAggs,
  idSource,
  subquery,
  wrapTreeIdStages,
} from '../generator_utils';
import type {ColumnDef} from '../graph_utils';
import type {NodeManifest, RenderContext} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const TreeFoldKindSchema = z.enum(['CONTRACT', 'ABSORB']);
export type TreeFoldKind = z.infer<typeof TreeFoldKindSchema>;

const TreeFoldConfigSchema = z
  .object({
    kind: TreeFoldKindSchema,
    idColumn: z.string(),
    parentIdColumn: z.string(),
    atIdColumn: z.string(),
    aggregations: z.array(PipelineAggEntrySchema),
  })
  .describe(
    'Fold subtrees at selected node IDs using TREE CONTRACT (collapse into the boundary node) or TREE ABSORB (collapse into its parent).',
  );

export type TreeFoldConfig = z.infer<typeof TreeFoldConfigSchema>;

function aggAlias(a: PipelineAggEntry): string {
  return pipelineAggAlias(a);
}

function TreeFoldContent(): m.Component<{
  config: TreeFoldConfig;
  updateConfig: (updates: Partial<TreeFoldConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, {minWidth: 'lg'}, [
        m(
          RadioGroup,
          {
            fillWidth: true,
            selectedValue: config.kind,
            onValueChange: (value: string) =>
              updateConfig({kind: value as TreeFoldKind}),
          },
          [
            m(
              RadioGroup.Button,
              {
                value: 'CONTRACT',
                title: 'Collapse each marked node’s subtree into itself',
              },
              'Contract',
            ),
            m(
              RadioGroup.Button,
              {
                value: 'ABSORB',
                title: 'Absorb each marked subtree into its parent node',
              },
              'Absorb',
            ),
          ],
        ),
        m(TreeColumnsRow, {
          idColumn: config.idColumn,
          parentIdColumn: config.parentIdColumn,
          columns: ctx.availableColumns,
          onIdChange: (idColumn) => updateConfig({idColumn}),
          onParentIdChange: (parentIdColumn) => updateConfig({parentIdColumn}),
        }),
        m('.pf-spag-section-label', 'Fold at node ID'),
        m(ColumnPicker, {
          value: config.atIdColumn,
          columns: ctx.getInputColumns('at'),
          placeholder: 'id',
          onSelect: (atIdColumn: string) => updateConfig({atIdColumn}),
        }),
        m('.pf-spag-section-label', 'Aggregations'),
        m(DraggableStack, {
          rows: config.aggregations.map((agg, i) =>
            m(PipelineAggRow, {
              value: agg,
              columns: ctx.availableColumns,
              aliasPlaceholder: aggAlias(agg),
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
          label: 'Add aggregation',
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

function isValid(config: TreeFoldConfig): boolean {
  return (
    config.idColumn !== '' &&
    config.parentIdColumn !== '' &&
    config.atIdColumn !== '' &&
    config.aggregations.length > 0 &&
    config.aggregations.every((a) => a.func === 'COUNT' || a.column !== '')
  );
}

function foldAggs(config: TreeFoldConfig): AggSpec[] {
  return config.aggregations.map((a) => ({
    func: a.func,
    column: a.func === 'COUNT' ? '*' : a.column,
    alias: aggAlias(a),
  }));
}

function treeFoldSql(
  ref: string,
  atRef: string,
  config: TreeFoldConfig,
  aggs: readonly AggSpec[],
  inputColumns: readonly string[] | undefined,
): string {
  const {kind, idColumn, parentIdColumn, atIdColumn} = config;
  const aggExprs = aggs.map((a) =>
    a.func === 'COUNT'
      ? `COUNT(*) AS ${a.alias}`
      : `${a.func}(src.${a.column}) AS ${a.alias}`,
  );
  const aggNames = new Set(aggs.map((a) => a.alias));

  // Walk top-down assigning each node its owning surviving node (`_owner`) and
  // whether it is removed (`_folded = 1`).
  const walkCte =
    kind === 'CONTRACT'
      ? `  _walk(_id, _owner, _folded, _pass_owner) AS (\n` +
        `    SELECT\n` +
        `      r.${idColumn},\n` +
        `      r.${idColumn},\n` +
        `      0,\n` +
        `      IIF(m._id IS NOT NULL, r.${idColumn}, CAST(NULL AS INTEGER))\n` +
        `    FROM ${ref} AS r\n` +
        `    LEFT JOIN _marks AS m ON m._id = r.${idColumn}\n` +
        `    WHERE r.${parentIdColumn} IS NULL\n` +
        `    UNION ALL\n` +
        `    SELECT\n` +
        `      c.${idColumn},\n` +
        `      COALESCE(w._pass_owner, c.${idColumn}),\n` +
        `      IIF(w._pass_owner IS NOT NULL, 1, 0),\n` +
        `      COALESCE(w._pass_owner, IIF(m._id IS NOT NULL, c.${idColumn}, CAST(NULL AS INTEGER)))\n` +
        `    FROM _walk AS w\n` +
        `    JOIN ${ref} AS c ON c.${parentIdColumn} = w._id\n` +
        `    LEFT JOIN _marks AS m ON m._id = c.${idColumn}\n` +
        `  ),`
      : `  _walk(_id, _owner, _folded, _pass_owner) AS (\n` +
        `    SELECT\n` +
        `      r.${idColumn},\n` +
        `      IIF(m._id IS NOT NULL, CAST(NULL AS INTEGER), r.${idColumn}),\n` +
        `      IIF(m._id IS NOT NULL, 1, 0),\n` +
        `      IIF(m._id IS NOT NULL, -1, CAST(NULL AS INTEGER))\n` +
        `    FROM ${ref} AS r\n` +
        `    LEFT JOIN _marks AS m ON m._id = r.${idColumn}\n` +
        `    WHERE r.${parentIdColumn} IS NULL\n` +
        `    UNION ALL\n` +
        `    SELECT\n` +
        `      c.${idColumn},\n` +
        `      CASE\n` +
        `        WHEN w._pass_owner = -1 THEN CAST(NULL AS INTEGER)\n` +
        `        WHEN w._pass_owner IS NOT NULL THEN w._pass_owner\n` +
        `        WHEN m._id IS NOT NULL THEN w._owner\n` +
        `        ELSE c.${idColumn}\n` +
        `      END,\n` +
        `      IIF(w._pass_owner IS NOT NULL OR m._id IS NOT NULL, 1, 0),\n` +
        `      COALESCE(w._pass_owner, IIF(m._id IS NOT NULL, w._owner, CAST(NULL AS INTEGER)))\n` +
        `    FROM _walk AS w\n` +
        `    JOIN ${ref} AS c ON c.${parentIdColumn} = w._id\n` +
        `    LEFT JOIN _marks AS m ON m._id = c.${idColumn}\n` +
        `  ),`;

  let selectCols: string[];
  if (inputColumns !== undefined) {
    const replaced = inputColumns.map((col) =>
      aggNames.has(col) ? `_agg.${col}` : `src.${col}`,
    );
    const existing = new Set(inputColumns);
    const added = aggs
      .filter((a) => !existing.has(a.alias))
      .map((a) => `_agg.${a.alias}`);
    selectCols = [...replaced, ...added];
  } else {
    selectCols = ['src.*', ...aggs.map((a) => `_agg.${a.alias}`)];
  }

  return [
    'WITH RECURSIVE',
    `  _marks(_id) AS (\n    SELECT DISTINCT ${atIdColumn} FROM ${atRef}\n  ),`,
    walkCte,
    `  _agg AS (\n` +
      `    SELECT\n` +
      `      w._owner AS _owner_id,\n` +
      `      ${aggExprs.join(',\n      ')}\n` +
      `    FROM _walk AS w\n` +
      `    JOIN ${ref} AS src ON src.${idColumn} = w._id\n` +
      `    WHERE w._owner IS NOT NULL\n` +
      `    GROUP BY w._owner\n` +
      `  )`,
    `SELECT\n  ${selectCols.join(',\n  ')}\n` +
      `FROM ${ref} AS src\n` +
      `JOIN _walk AS w ON src.${idColumn} = w._id AND w._folded = 0\n` +
      `JOIN _agg ON _agg._owner_id = src.${idColumn}`,
  ].join('\n');
}

export const manifest: NodeManifest<TreeFoldConfig> = {
  title: 'Tree Fold',
  icon: 'compress',
  getInputs: () => [
    {name: 'input', content: 'Input'},
    {name: 'at', content: 'Fold at'},
  ],
  hue: 84,
  schema: TreeFoldConfigSchema,
  expensive: true,
  defaultConfig: () => ({
    kind: 'CONTRACT',
    idColumn: 'id',
    parentIdColumn: 'parent_id',
    atIdColumn: 'id',
    aggregations: [{func: 'COUNT', column: '*', alias: ''}],
  }),
  render: (config, updateConfig, ctx) =>
    m(TreeFoldContent, {config, updateConfig, ctx}),
  getOutputColumns(config, ctx) {
    const columns = ctx.getInputColumns('input');
    if (columns === undefined) return undefined;
    const validAggs = config.aggregations.filter(
      (a) => a.func === 'COUNT' || a.column !== '',
    );
    const aggMap = new Map<string, ColumnDef>(
      validAggs.map((a) => {
        const name = aggAlias(a);
        const type =
          a.func === 'COUNT'
            ? {kind: 'int' as const}
            : columns.find((col) => col.name === a.column)?.type;
        return [name, {name, type}];
      }),
    );
    const existingNames = new Set(columns.map((c) => c.name));
    const updated = columns.map((c) => aggMap.get(c.name) ?? c);
    const added = [...aggMap.values()].filter(
      (c) => !existingNames.has(c.name),
    );
    return [...updated, ...added];
  },
  generators: {
    sql: (config, ctx) => {
      const input = ctx.input('input');
      if (input === undefined || !isValid(config)) return undefined;
      const at = ctx.input('at');
      if (at === undefined) return {};
      const aggs = foldAggs(config);
      const inputCols = ctx.getInputColumns('input')?.map((c) => c.name);
      return {
        from: subquery(treeFoldSql(input.ref, at.ref, config, aggs, inputCols)),
      };
    },
    psn: (config, ctx) => {
      if (ctx.input('input') === undefined || !isValid(config)) {
        return undefined;
      }
      const at = ctx.input('at');
      if (at === undefined) return {};
      const aggs = foldAggs(config);
      const atSrc = idSource(at, config.atIdColumn);
      return {
        stages: wrapTreeIdStages(config.idColumn, config.parentIdColumn, [
          `TREE ${config.kind} AT ${atSrc} AGGREGATE ${formatAggs(aggs)}`,
        ]),
      };
    },
  },
};
