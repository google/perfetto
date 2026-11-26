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

const TreeReparentConfigSchema = z
  .object({
    idColumn: z.string(),
    parentIdColumn: z.string(),
    markerIdColumn: z.string(),
  })
  .describe(
    'Reparent each tree node to its nearest ancestor present in the Markers input.',
  );

export type TreeReparentConfig = z.infer<typeof TreeReparentConfigSchema>;

function TreeReparentContent(): m.Component<{
  config: TreeReparentConfig;
  updateConfig: (updates: Partial<TreeReparentConfig>) => void;
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
        m('.pf-spag-section-label', 'Marker node ID'),
        m(ColumnPicker, {
          value: config.markerIdColumn,
          columns: ctx.getInputColumns('markers'),
          placeholder: 'id',
          onSelect: (markerIdColumn: string) => updateConfig({markerIdColumn}),
        }),
      ]);
    },
  };
}

function isValid(config: TreeReparentConfig): boolean {
  return (
    config.idColumn !== '' &&
    config.parentIdColumn !== '' &&
    config.markerIdColumn !== ''
  );
}

// Walks top-down propagating the nearest ancestor present in `_markers`.
function treeReparentSql(
  ref: string,
  markersRef: string,
  config: TreeReparentConfig,
  inputColumns: readonly string[],
): string {
  const {idColumn, parentIdColumn, markerIdColumn} = config;
  const selectCols = inputColumns.map((col) =>
    col === parentIdColumn
      ? `w._new_parent_id AS ${parentIdColumn}`
      : `src.${col}`,
  );
  return [
    'WITH RECURSIVE',
    `  _markers(_id) AS (\n    SELECT DISTINCT ${markerIdColumn} FROM ${markersRef}\n  ),`,
    `  _walk(_id, _new_parent_id, _pass_down) AS (\n` +
      `    SELECT\n` +
      `      r.${idColumn},\n` +
      `      CAST(NULL AS INTEGER),\n` +
      `      IIF(m._id IS NOT NULL, r.${idColumn}, CAST(NULL AS INTEGER))\n` +
      `    FROM ${ref} AS r\n` +
      `    LEFT JOIN _markers AS m ON m._id = r.${idColumn}\n` +
      `    WHERE r.${parentIdColumn} IS NULL\n` +
      `    UNION ALL\n` +
      `    SELECT\n` +
      `      c.${idColumn},\n` +
      `      w._pass_down,\n` +
      `      IIF(m._id IS NOT NULL, c.${idColumn}, w._pass_down)\n` +
      `    FROM _walk AS w\n` +
      `    JOIN ${ref} AS c ON c.${parentIdColumn} = w._id\n` +
      `    LEFT JOIN _markers AS m ON m._id = c.${idColumn}\n` +
      `  )`,
    `SELECT\n  ${selectCols.join(',\n  ')}\nFROM ${ref} AS src\nJOIN _walk AS w ON src.${idColumn} = w._id`,
  ].join('\n');
}

export const manifest: NodeManifest<TreeReparentConfig> = {
  title: 'Tree Reparent',
  icon: 'low_priority',
  getInputs: () => [
    {name: 'input', content: 'Input'},
    {name: 'markers', content: 'Ancestors'},
  ],
  hue: 91,
  schema: TreeReparentConfigSchema,
  expensive: true,
  defaultConfig: () => ({
    idColumn: 'id',
    parentIdColumn: 'parent_id',
    markerIdColumn: 'id',
  }),
  render: (config, updateConfig, ctx) =>
    m(TreeReparentContent, {config, updateConfig, ctx}),
  getOutputColumns: (_config, ctx) => ctx.getInputColumns('input'),
  generators: {
    sql: (config, ctx) => {
      const input = ctx.input('input');
      if (input === undefined || !isValid(config)) return undefined;
      const markers = ctx.input('markers');
      if (markers === undefined) return {};
      const inputCols = ctx.getInputColumns('input')?.map((c) => c.name);
      if (inputCols === undefined) {
        return {
          error:
            'Tree Reparent needs known input columns in Standard SQL mode.',
        };
      }
      return {
        from: subquery(
          treeReparentSql(input.ref, markers.ref, config, inputCols),
        ),
      };
    },
    psn: (config, ctx) => {
      if (ctx.input('input') === undefined || !isValid(config)) {
        return undefined;
      }
      const markers = ctx.input('markers');
      if (markers === undefined) return {};
      const markersSrc = idSource(markers, config.markerIdColumn);
      return {
        stages: wrapTreeIdStages(config.idColumn, config.parentIdColumn, [
          `TREE REPARENT TO NEAREST ANCESTOR IN ${markersSrc}`,
        ]),
      };
    },
  },
};
