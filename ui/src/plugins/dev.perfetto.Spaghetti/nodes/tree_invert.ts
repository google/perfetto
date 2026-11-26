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
import {Stack} from '../components/stack';
import {TreeColumnsRow} from '../components/tree_columns_row';
import {subquery, wrapTreeIdStages} from '../generator_utils';
import type {NodeManifest, RenderContext} from '../node_types';

const TreeInvertConfigSchema = z
  .object({
    idColumn: z.string(),
    parentIdColumn: z.string(),
  })
  .describe(
    'Invert a tree bottom-up so leaves become roots and each leaf-to-root path is reversed.',
  );

export type TreeInvertConfig = z.infer<typeof TreeInvertConfigSchema>;

function TreeInvertContent(): m.Component<{
  config: TreeInvertConfig;
  updateConfig: (updates: Partial<TreeInvertConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, {minWidth: 'md'}, [
        m(TreeColumnsRow, {
          idColumn: config.idColumn,
          parentIdColumn: config.parentIdColumn,
          columns: ctx.availableColumns,
          onIdChange: (idColumn) => updateConfig({idColumn}),
          onParentIdChange: (parentIdColumn) => updateConfig({parentIdColumn}),
        }),
      ]);
    },
  };
}

function isValid(config: TreeInvertConfig): boolean {
  return config.idColumn !== '' && config.parentIdColumn !== '';
}

// Reverses every leaf-to-root path, assigning fresh IDs so each path forms a
// valid tree rooted at its original leaf.
function treeInvertSql(
  ref: string,
  config: TreeInvertConfig,
  inputColumns: readonly string[],
): string {
  const {idColumn, parentIdColumn} = config;
  const selectCols = inputColumns.map((col) => {
    if (col === idColumn) return `n._new_id AS ${idColumn}`;
    if (col === parentIdColumn) return `p._new_id AS ${parentIdColumn}`;
    return `src.${col}`;
  });
  return [
    'WITH RECURSIVE',
    `  _paths(_leaf_id, _orig_id, _depth) AS (\n` +
      `    SELECT l.${idColumn}, l.${idColumn}, 0\n` +
      `    FROM ${ref} AS l\n` +
      `    WHERE l.${idColumn} NOT IN (\n` +
      `      SELECT ${parentIdColumn} FROM ${ref} WHERE ${parentIdColumn} IS NOT NULL\n` +
      `    )\n` +
      `    UNION ALL\n` +
      `    SELECT p._leaf_id, anc.${idColumn}, p._depth + 1\n` +
      `    FROM _paths AS p\n` +
      `    JOIN ${ref} AS cur ON cur.${idColumn} = p._orig_id\n` +
      `    JOIN ${ref} AS anc ON anc.${idColumn} = cur.${parentIdColumn}\n` +
      `  ),`,
    `  _numbered AS (\n` +
      `    SELECT\n` +
      `      _leaf_id,\n` +
      `      _orig_id,\n` +
      `      _depth,\n` +
      `      ROW_NUMBER() OVER (ORDER BY _leaf_id, _depth) - 1 AS _new_id\n` +
      `    FROM _paths\n` +
      `  )`,
    `SELECT\n  ${selectCols.join(',\n  ')}\n` +
      `FROM _numbered AS n\n` +
      `JOIN ${ref} AS src ON src.${idColumn} = n._orig_id\n` +
      `LEFT JOIN _numbered AS p ON p._leaf_id = n._leaf_id AND p._depth = n._depth - 1`,
  ].join('\n');
}

export const manifest: NodeManifest<TreeInvertConfig> = {
  title: 'Tree Invert',
  icon: 'swap_vert',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 104,
  schema: TreeInvertConfigSchema,
  expensive: true,
  defaultConfig: () => ({
    idColumn: 'id',
    parentIdColumn: 'parent_id',
  }),
  render: (config, updateConfig, ctx) =>
    m(TreeInvertContent, {config, updateConfig, ctx}),
  getOutputColumns: (_config, ctx) => ctx.getInputColumns('input'),
  generators: {
    sql: (config, ctx) => {
      const input = ctx.input('input');
      if (input === undefined || !isValid(config)) return undefined;
      const inputCols = ctx.getInputColumns('input')?.map((c) => c.name);
      if (inputCols === undefined) {
        return {
          error: 'Tree Invert needs known input columns in Standard SQL mode.',
        };
      }
      return {from: subquery(treeInvertSql(input.ref, config, inputCols))};
    },
    psn: (config, ctx) => {
      if (ctx.input('input') === undefined || !isValid(config)) {
        return undefined;
      }
      return {
        stages: wrapTreeIdStages(config.idColumn, config.parentIdColumn, [
          'TREE INVERT',
        ]),
      };
    },
  },
};
