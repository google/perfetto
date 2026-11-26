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
import {TextInput} from '../../../widgets/text_input';
import {DraggableStack} from '../components/draggable_stack';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import {TreeColumnsRow} from '../components/tree_columns_row';
import {subquery, wrapTreeIdStages} from '../generator_utils';
import type {NodeManifest, RenderContext} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const TreeKeyPathsConfigSchema = z
  .object({
    idColumn: z.string(),
    parentIdColumn: z.string(),
    keyColumns: z.array(z.string()),
    keyName: z.string(),
    parentKeyName: z.string(),
  })
  .describe(
    'Assign each tree node an integer key (and parent key) for its root-to-node path of keyColumns.',
  );

export type TreeKeyPathsConfig = z.infer<typeof TreeKeyPathsConfigSchema>;

function TreeKeyPathsContent(): m.Component<{
  config: TreeKeyPathsConfig;
  updateConfig: (updates: Partial<TreeKeyPathsConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, {minWidth: 'lg'}, [
        m(TreeColumnsRow, {
          idColumn: config.idColumn,
          parentIdColumn: config.parentIdColumn,
          columns: ctx.availableColumns,
          onIdChange: (idColumn) => updateConfig({idColumn}),
          onParentIdChange: (parentIdColumn) => updateConfig({parentIdColumn}),
        }),
        m('.pf-spag-section-label', 'Key columns'),
        m(DraggableStack, {
          rows: config.keyColumns.map((col, i) => [
            m(ColumnPicker, {
              value: col,
              columns: ctx.availableColumns,
              placeholder: 'column',
              onSelect: (value: string) =>
                updateConfig({keyColumns: config.keyColumns.with(i, value)}),
            }),
          ]),
          onReorder: (from, to) =>
            updateConfig({keyColumns: moveItem(config.keyColumns, from, to)}),
          onDelete: (i) =>
            updateConfig({keyColumns: config.keyColumns.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Add key column',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () => updateConfig({keyColumns: [...config.keyColumns, '']}),
        }),
        m('.pf-spag-section-label', 'Output key columns'),
        m(Row, [
          m(TextInput, {
            placeholder: 'path_key',
            value: config.keyName,
            onChange: (keyName: string) =>
              updateConfig({keyName: keyName.trim()}),
          }),
          m('span.pf-spag-operator', ','),
          m(TextInput, {
            placeholder: 'parent_path_key',
            value: config.parentKeyName,
            onChange: (parentKeyName: string) =>
              updateConfig({parentKeyName: parentKeyName.trim()}),
          }),
        ]),
      ]);
    },
  };
}

function keyCols(config: TreeKeyPathsConfig): string[] {
  return config.keyColumns.filter((c) => c !== '');
}

function isValid(config: TreeKeyPathsConfig): boolean {
  return (
    config.idColumn !== '' &&
    config.parentIdColumn !== '' &&
    keyCols(config).length > 0 &&
    config.keyName !== '' &&
    config.parentKeyName !== ''
  );
}

// Walks top-down building a collision-free JSON path signature per node and
// assigns contiguous 0-based integer keys to distinct root-to-node paths.
function treeKeyPathsSql(ref: string, config: TreeKeyPathsConfig): string {
  const {idColumn, parentIdColumn, keyName, parentKeyName} = config;
  const cols = keyCols(config);
  const rootVals = cols.map((c) => `r.${c}`).join(', ');
  const childVals = cols.map((c) => `c.${c}`).join(', ');
  return [
    'WITH RECURSIVE',
    `  _walk(_id, _path, _parent_path) AS (\n` +
      `    SELECT\n` +
      `      r.${idColumn},\n` +
      `      json_array(NULL, ${rootVals}),\n` +
      `      CAST(NULL AS TEXT)\n` +
      `    FROM ${ref} AS r\n` +
      `    WHERE r.${parentIdColumn} IS NULL\n` +
      `    UNION ALL\n` +
      `    SELECT\n` +
      `      c.${idColumn},\n` +
      `      json_array(w._path, ${childVals}),\n` +
      `      w._path\n` +
      `    FROM _walk AS w\n` +
      `    JOIN ${ref} AS c ON c.${parentIdColumn} = w._id\n` +
      `  ),`,
    `  _keys AS (\n` +
      `    SELECT\n` +
      `      _path,\n` +
      `      ROW_NUMBER() OVER (ORDER BY _path) - 1 AS _k\n` +
      `    FROM (SELECT DISTINCT _path FROM _walk)\n` +
      `  )`,
    `SELECT\n` +
      `  src.*,\n` +
      `  k._k AS ${keyName},\n` +
      `  pk._k AS ${parentKeyName}\n` +
      `FROM ${ref} AS src\n` +
      `JOIN _walk AS w ON src.${idColumn} = w._id\n` +
      `JOIN _keys AS k ON w._path = k._path\n` +
      `LEFT JOIN _keys AS pk ON w._parent_path = pk._path`,
  ].join('\n');
}

export const manifest: NodeManifest<TreeKeyPathsConfig> = {
  title: 'Tree Key Paths',
  icon: 'conversion_path',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 97,
  schema: TreeKeyPathsConfigSchema,
  expensive: true,
  defaultConfig: () => ({
    idColumn: 'id',
    parentIdColumn: 'parent_id',
    keyColumns: [],
    keyName: 'path_key',
    parentKeyName: 'parent_path_key',
  }),
  render: (config, updateConfig, ctx) =>
    m(TreeKeyPathsContent, {config, updateConfig, ctx}),
  getOutputColumns(config, ctx) {
    const columns = ctx.getInputColumns('input');
    if (columns === undefined) return undefined;
    if (!isValid(config)) return columns;
    return [
      ...columns,
      {name: config.keyName, type: {kind: 'int' as const}},
      {name: config.parentKeyName, type: {kind: 'int' as const}},
    ];
  },
  generators: {
    sql: (config, ctx) => {
      const input = ctx.input('input');
      if (input === undefined || !isValid(config)) return undefined;
      return {from: subquery(treeKeyPathsSql(input.ref, config))};
    },
    psn: (config, ctx) => {
      if (ctx.input('input') === undefined || !isValid(config)) {
        return undefined;
      }
      const cols = keyCols(config);
      return {
        stages: wrapTreeIdStages(config.idColumn, config.parentIdColumn, [
          `TREE KEY PATHS BY ${cols.join(', ')} AS (${config.keyName}, ${config.parentKeyName})`,
        ]),
      };
    },
  },
};
