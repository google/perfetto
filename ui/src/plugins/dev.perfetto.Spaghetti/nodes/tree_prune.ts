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
import {idSource, subquery, wrapTreeIdStages} from '../generator_utils';
import type {NodeManifest, RenderContext} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const TreePruneConfigSchema = z
  .object({
    idColumn: z.string(),
    parentIdColumn: z.string(),
    atIdColumn: z.string(),
  })
  .describe('Remove subtrees rooted at node IDs in the Prune At input.');

export type TreePruneConfig = z.infer<typeof TreePruneConfigSchema>;

function TreePruneContent(): m.Component<{
  config: TreePruneConfig;
  updateConfig: (updates: Partial<TreePruneConfig>) => void;
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
        m('.pf-spag-section-label', 'Prune node ID'),
        m(ColumnPicker, {
          value: config.atIdColumn,
          columns: ctx.getInputColumns('at'),
          placeholder: 'id',
          onSelect: (atIdColumn: string) => updateConfig({atIdColumn}),
        }),
      ]);
    },
  };
}

function isValid(config: TreePruneConfig): boolean {
  return (
    config.idColumn !== '' &&
    config.parentIdColumn !== '' &&
    config.atIdColumn !== ''
  );
}

function treePruneSql(
  ref: string,
  atRef: string,
  config: TreePruneConfig,
): string {
  const {idColumn, parentIdColumn, atIdColumn} = config;
  return [
    'WITH RECURSIVE',
    `  _pruned(_id) AS (\n` +
      `    SELECT src.${idColumn}\n` +
      `    FROM ${ref} AS src\n` +
      `    WHERE src.${idColumn} IN (SELECT ${atIdColumn} FROM ${atRef})\n` +
      `    UNION\n` +
      `    SELECT c.${idColumn}\n` +
      `    FROM _pruned AS p\n` +
      `    JOIN ${ref} AS c ON c.${parentIdColumn} = p._id\n` +
      `  )`,
    `SELECT src.*\nFROM ${ref} AS src\nWHERE src.${idColumn} NOT IN (SELECT _id FROM _pruned)`,
  ].join('\n');
}

export const manifest: NodeManifest<TreePruneConfig> = {
  title: 'Tree Prune',
  icon: 'content_cut',
  getInputs: () => [
    {name: 'input', content: 'Input'},
    {name: 'at', content: 'Prune at'},
  ],
  hue: 88,
  schema: TreePruneConfigSchema,
  expensive: true,
  defaultConfig: () => ({
    idColumn: 'id',
    parentIdColumn: 'parent_id',
    atIdColumn: 'id',
  }),
  render: (config, updateConfig, ctx) =>
    m(TreePruneContent, {config, updateConfig, ctx}),
  getOutputColumns: (_config, ctx) => ctx.getInputColumns('input'),
  generators: {
    sql: (config, ctx) => {
      const input = ctx.input('input');
      if (input === undefined || !isValid(config)) return undefined;
      const at = ctx.input('at');
      if (at === undefined) return {};
      return {from: subquery(treePruneSql(input.ref, at.ref, config))};
    },
    psn: (config, ctx) => {
      if (ctx.input('input') === undefined || !isValid(config)) {
        return undefined;
      }
      const at = ctx.input('at');
      if (at === undefined) return {};
      const atSrc = idSource(at, config.atIdColumn);
      return {
        stages: wrapTreeIdStages(config.idColumn, config.parentIdColumn, [
          `TREE PRUNE AT ${atSrc}`,
        ]),
      };
    },
  },
};
