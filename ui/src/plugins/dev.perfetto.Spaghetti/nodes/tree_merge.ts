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
import {assertUnreachable} from '../../../base/assert';
import {moveItem} from '../../../base/array_utils';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Select} from '../../../widgets/select';
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
  subquery,
  wrapTreeIdStages,
} from '../generator_utils';
import type {ColumnDef} from '../graph_utils';
import type {NodeManifest, RenderContext} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const TreeMergeModeSchema = z.enum([
  'SIBLINGS',
  'CONSECUTIVE_SIBLINGS',
  'INTO_PARENT',
  'INTO_PARENT_INDIRECT',
]);
export type TreeMergeMode = z.infer<typeof TreeMergeModeSchema>;

const TreeMergeConfigSchema = z
  .object({
    mode: TreeMergeModeSchema,
    idColumn: z.string(),
    parentIdColumn: z.string(),
    byColumns: z.array(z.string()),
    aggregations: z.array(PipelineAggEntrySchema),
  })
  .describe(
    'Merge matching sibling nodes or collapse nodes into matching parent/ancestor nodes with aggregations.',
  );

export type TreeMergeConfig = z.infer<typeof TreeMergeConfigSchema>;

function aggAlias(a: PipelineAggEntry): string {
  return pipelineAggAlias(a);
}

function byCols(config: TreeMergeConfig): string[] {
  return config.byColumns.filter((c) => c !== '');
}

function TreeMergeContent(): m.Component<{
  config: TreeMergeConfig;
  updateConfig: (updates: Partial<TreeMergeConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, {minWidth: 'lg'}, [
        m('.pf-spag-section-label', 'Merge mode'),
        m(
          Select,
          {
            value: config.mode,
            onchange: (e: Event) =>
              updateConfig({
                mode: (e.target as HTMLSelectElement).value as TreeMergeMode,
              }),
          },
          [
            m('option', {value: 'SIBLINGS'}, 'Siblings'),
            m(
              'option',
              {value: 'CONSECUTIVE_SIBLINGS'},
              'Consecutive siblings',
            ),
            m('option', {value: 'INTO_PARENT'}, 'Into parent'),
            m(
              'option',
              {value: 'INTO_PARENT_INDIRECT'},
              'Into parent (indirect)',
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
        m('.pf-spag-section-label', 'Match columns (BY)'),
        m(DraggableStack, {
          rows: config.byColumns.map((col, i) => [
            m(ColumnPicker, {
              value: col,
              columns: ctx.availableColumns,
              placeholder: 'column',
              onSelect: (value: string) =>
                updateConfig({byColumns: config.byColumns.with(i, value)}),
            }),
          ]),
          onReorder: (from, to) =>
            updateConfig({byColumns: moveItem(config.byColumns, from, to)}),
          onDelete: (i) =>
            updateConfig({byColumns: config.byColumns.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Add match column',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () => updateConfig({byColumns: [...config.byColumns, '']}),
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

function isValid(config: TreeMergeConfig): boolean {
  return (
    config.idColumn !== '' &&
    config.parentIdColumn !== '' &&
    byCols(config).length > 0 &&
    config.aggregations.length > 0 &&
    config.aggregations.every((a) => a.func === 'COUNT' || a.column !== '')
  );
}

function mergeAggs(config: TreeMergeConfig): AggSpec[] {
  return config.aggregations.map((a) => ({
    func: a.func,
    column: a.func === 'COUNT' ? '*' : a.column,
    alias: aggAlias(a),
  }));
}

function aggSqlExprs(aggs: readonly AggSpec[], alias: string): string[] {
  return aggs.map((a) =>
    a.func === 'COUNT'
      ? `COUNT(*) AS ${a.alias}`
      : `${a.func}(${alias}.${a.column}) AS ${a.alias}`,
  );
}

function treeMergeSql(
  ref: string,
  config: TreeMergeConfig,
  aggs: readonly AggSpec[],
): string {
  const {mode, idColumn, parentIdColumn} = config;
  const cols = byCols(config);
  const aggNames = new Set(aggs.map((a) => a.alias));
  const extraBy = cols.filter(
    (c) => c !== idColumn && c !== parentIdColumn && !aggNames.has(c),
  );
  const aggSelects = aggs.map((a) => `_agg.${a.alias}`);
  const outBy = extraBy.map((c) => `rep.${c}`);

  switch (mode) {
    case 'SIBLINGS': {
      const grpCols = [parentIdColumn, ...cols].join(', ');
      return [
        'WITH',
        `  _reps AS (\n` +
          `    SELECT\n` +
          `      ${idColumn} AS _id,\n` +
          `      MIN(${idColumn}) OVER (PARTITION BY ${grpCols}) AS _rep_id\n` +
          `    FROM ${ref}\n` +
          `  ),`,
        `  _agg AS (\n` +
          `    SELECT\n` +
          `      m._rep_id,\n` +
          `      ${aggSqlExprs(aggs, 'src').join(',\n      ')}\n` +
          `    FROM _reps AS m\n` +
          `    JOIN ${ref} AS src ON src.${idColumn} = m._id\n` +
          `    GROUP BY m._rep_id\n` +
          `  )`,
        `SELECT\n` +
          `  ${[
            `rep.${idColumn}`,
            `pm._rep_id AS ${parentIdColumn}`,
            ...outBy,
            ...aggSelects,
          ].join(',\n  ')}\n` +
          `FROM _agg\n` +
          `JOIN ${ref} AS rep ON rep.${idColumn} = _agg._rep_id\n` +
          `LEFT JOIN _reps AS pm ON pm._id = rep.${parentIdColumn}`,
      ].join('\n');
    }

    case 'CONSECUTIVE_SIBLINGS': {
      const sameAsPrev = cols
        .map(
          (c) =>
            `${c} IS LAG(${c}) OVER (PARTITION BY ${parentIdColumn} ORDER BY ${idColumn})`,
        )
        .join(' AND ');
      return [
        'WITH',
        `  _flagged AS (\n` +
          `    SELECT\n` +
          `      ${idColumn},\n` +
          `      ${parentIdColumn},\n` +
          `      IIF(${sameAsPrev}, 0, 1) AS _new_run\n` +
          `    FROM ${ref}\n` +
          `  ),`,
        `  _runs AS (\n` +
          `    SELECT\n` +
          `      ${idColumn},\n` +
          `      ${parentIdColumn},\n` +
          `      SUM(_new_run) OVER (PARTITION BY ${parentIdColumn} ORDER BY ${idColumn}) AS _run_id\n` +
          `    FROM _flagged\n` +
          `  ),`,
        `  _reps AS (\n` +
          `    SELECT\n` +
          `      ${idColumn} AS _id,\n` +
          `      MIN(${idColumn}) OVER (PARTITION BY ${parentIdColumn}, _run_id) AS _rep_id\n` +
          `    FROM _runs\n` +
          `  ),`,
        `  _agg AS (\n` +
          `    SELECT\n` +
          `      m._rep_id,\n` +
          `      ${aggSqlExprs(aggs, 'src').join(',\n      ')}\n` +
          `    FROM _reps AS m\n` +
          `    JOIN ${ref} AS src ON src.${idColumn} = m._id\n` +
          `    GROUP BY m._rep_id\n` +
          `  )`,
        `SELECT\n` +
          `  ${[
            `rep.${idColumn}`,
            `pm._rep_id AS ${parentIdColumn}`,
            ...outBy,
            ...aggSelects,
          ].join(',\n  ')}\n` +
          `FROM _agg\n` +
          `JOIN ${ref} AS rep ON rep.${idColumn} = _agg._rep_id\n` +
          `LEFT JOIN _reps AS pm ON pm._id = rep.${parentIdColumn}`,
      ].join('\n');
    }

    case 'INTO_PARENT': {
      const sameAsParent = cols.map((c) => `c.${c} IS p.${c}`).join(' AND ');
      return [
        'WITH RECURSIVE',
        `  _reps(_id, _rep_id) AS (\n` +
          `    SELECT ${idColumn}, ${idColumn}\n` +
          `    FROM ${ref}\n` +
          `    WHERE ${parentIdColumn} IS NULL\n` +
          `    UNION ALL\n` +
          `    SELECT\n` +
          `      c.${idColumn},\n` +
          `      IIF(${sameAsParent}, w._rep_id, c.${idColumn})\n` +
          `    FROM _reps AS w\n` +
          `    JOIN ${ref} AS p ON p.${idColumn} = w._id\n` +
          `    JOIN ${ref} AS c ON c.${parentIdColumn} = w._id\n` +
          `  ),`,
        `  _agg AS (\n` +
          `    SELECT\n` +
          `      m._rep_id,\n` +
          `      ${aggSqlExprs(aggs, 'src').join(',\n      ')}\n` +
          `    FROM _reps AS m\n` +
          `    JOIN ${ref} AS src ON src.${idColumn} = m._id\n` +
          `    GROUP BY m._rep_id\n` +
          `  )`,
        `SELECT\n` +
          `  ${[
            `rep.${idColumn}`,
            `pm._rep_id AS ${parentIdColumn}`,
            ...outBy,
            ...aggSelects,
          ].join(',\n  ')}\n` +
          `FROM _agg\n` +
          `JOIN ${ref} AS rep ON rep.${idColumn} = _agg._rep_id\n` +
          `LEFT JOIN _reps AS pm ON pm._id = rep.${parentIdColumn}`,
      ].join('\n');
    }

    case 'INTO_PARENT_INDIRECT': {
      // Every node with any ancestor matching `byColumns` transitively merges
      // into its shallowest matching ancestor (largest `_dist`).
      const sameAsAnc = cols.map((c) => `cur.${c} IS anc.${c}`).join(' AND ');
      return [
        'WITH RECURSIVE',
        `  _anc(_id, _anc_id, _dist) AS (\n` +
          `    SELECT ${idColumn}, ${parentIdColumn}, 1\n` +
          `    FROM ${ref}\n` +
          `    WHERE ${parentIdColumn} IS NOT NULL\n` +
          `    UNION ALL\n` +
          `    SELECT a._id, p.${parentIdColumn}, a._dist + 1\n` +
          `    FROM _anc AS a\n` +
          `    JOIN ${ref} AS p ON p.${idColumn} = a._anc_id\n` +
          `    WHERE p.${parentIdColumn} IS NOT NULL\n` +
          `  ),`,
        `  _matched AS (\n` +
          `    SELECT\n` +
          `      a._id,\n` +
          `      a._anc_id,\n` +
          `      ROW_NUMBER() OVER (PARTITION BY a._id ORDER BY a._dist DESC) AS _rn\n` +
          `    FROM _anc AS a\n` +
          `    JOIN ${ref} AS cur ON cur.${idColumn} = a._id\n` +
          `    JOIN ${ref} AS anc ON anc.${idColumn} = a._anc_id\n` +
          `    WHERE ${sameAsAnc}\n` +
          `  ),`,
        `  _reps AS (\n` +
          `    SELECT\n` +
          `      src.${idColumn} AS _id,\n` +
          `      COALESCE(m._anc_id, src.${idColumn}) AS _rep_id\n` +
          `    FROM ${ref} AS src\n` +
          `    LEFT JOIN _matched AS m ON m._id = src.${idColumn} AND m._rn = 1\n` +
          `  ),`,
        `  _agg AS (\n` +
          `    SELECT\n` +
          `      m._rep_id,\n` +
          `      ${aggSqlExprs(aggs, 'src').join(',\n      ')}\n` +
          `    FROM _reps AS m\n` +
          `    JOIN ${ref} AS src ON src.${idColumn} = m._id\n` +
          `    GROUP BY m._rep_id\n` +
          `  )`,
        `SELECT\n` +
          `  ${[
            `rep.${idColumn}`,
            `pm._rep_id AS ${parentIdColumn}`,
            ...outBy,
            ...aggSelects,
          ].join(',\n  ')}\n` +
          `FROM _agg\n` +
          `JOIN ${ref} AS rep ON rep.${idColumn} = _agg._rep_id\n` +
          `LEFT JOIN _reps AS pm ON pm._id = rep.${parentIdColumn}`,
      ].join('\n');
    }

    default:
      return assertUnreachable(mode);
  }
}

function psnMergeStage(
  config: TreeMergeConfig,
  aggs: readonly AggSpec[],
): string {
  const cols = byCols(config).join(', ');
  const aggStr = formatAggs(aggs);
  switch (config.mode) {
    case 'SIBLINGS':
      return `TREE MERGE SIBLINGS BY ${cols} AGGREGATE ${aggStr}`;
    case 'CONSECUTIVE_SIBLINGS':
      return `TREE MERGE CONSECUTIVE SIBLINGS BY ${cols} AGGREGATE ${aggStr}`;
    case 'INTO_PARENT':
      return `TREE MERGE INTO PARENT BY ${cols} AGGREGATE ${aggStr}`;
    case 'INTO_PARENT_INDIRECT':
      return `TREE MERGE INTO PARENT BY ${cols} INDIRECT AGGREGATE ${aggStr}`;
    default:
      return assertUnreachable(config.mode);
  }
}

export const manifest: NodeManifest<TreeMergeConfig> = {
  title: 'Tree Merge',
  icon: 'call_merge',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 100,
  schema: TreeMergeConfigSchema,
  expensive: true,
  defaultConfig: () => ({
    mode: 'SIBLINGS',
    idColumn: 'id',
    parentIdColumn: 'parent_id',
    byColumns: [],
    aggregations: [{func: 'COUNT', column: '*', alias: ''}],
  }),
  render: (config, updateConfig, ctx) =>
    m(TreeMergeContent, {config, updateConfig, ctx}),
  getOutputColumns(config, ctx) {
    const columns = ctx.getInputColumns('input');
    const colType = (name: string) =>
      columns?.find((c) => c.name === name)?.type;
    const validAggs = config.aggregations.filter(
      (a) => a.func === 'COUNT' || a.column !== '',
    );
    const aggNames = new Set(validAggs.map(aggAlias));
    const keyDefs: ColumnDef[] = byCols(config)
      .filter(
        (c) =>
          c !== config.idColumn &&
          c !== config.parentIdColumn &&
          !aggNames.has(c),
      )
      .map((name) => ({name, type: colType(name)}));
    const aggDefs: ColumnDef[] = validAggs.map((a) => ({
      name: aggAlias(a),
      type: a.func === 'COUNT' ? {kind: 'int' as const} : colType(a.column),
    }));
    return [
      {name: config.idColumn, type: colType(config.idColumn) ?? {kind: 'int'}},
      {
        name: config.parentIdColumn,
        type: colType(config.parentIdColumn) ?? {kind: 'int'},
      },
      ...keyDefs,
      ...aggDefs,
    ];
  },
  generators: {
    sql: (config, ctx) => {
      const input = ctx.input('input');
      if (input === undefined || !isValid(config)) return undefined;
      return {
        from: subquery(treeMergeSql(input.ref, config, mergeAggs(config))),
      };
    },
    psn: (config, ctx) => {
      if (ctx.input('input') === undefined || !isValid(config)) {
        return undefined;
      }
      return {
        stages: wrapTreeIdStages(config.idColumn, config.parentIdColumn, [
          psnMergeStage(config, mergeAggs(config)),
        ]),
      };
    },
  },
};
