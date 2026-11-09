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
import {ColumnPickerWithAlias} from '../components/column_picker_with_alias';
import {DraggableStack} from '../components/draggable_stack';
import {Stack} from '../components/stack';
import type {NodeManifest, RenderContext} from '../node_types';

const RenameEntrySchema = z.object({
  column: z.string(),
  alias: z.string(),
});

export type RenameEntry = z.infer<typeof RenameEntrySchema>;

const RenameConfigSchema = z
  .object({
    entries: z.array(RenameEntrySchema),
  })
  .describe('Rename columns in place, keeping all other columns.');

export type RenameConfig = z.infer<typeof RenameConfigSchema>;

function RenameContent(): m.Component<{
  config: RenameConfig;
  updateConfig: (updates: Partial<RenameConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, {minWidth: 'md'}, [
        config.entries.length === 0 &&
          m('.pf-spag-passthrough-hint', 'All columns (passthrough)'),
        m(DraggableStack, {
          rows: config.entries.map((entry, i) =>
            m(ColumnPickerWithAlias, {
              value: entry.column,
              columns: ctx.availableColumns,
              onSelect: (column) =>
                updateConfig({
                  entries: config.entries.with(i, {...entry, column}),
                }),
              alias: entry.alias,
              aliasPlaceholder: entry.column || 'new name',
              onAliasChange: (alias) =>
                updateConfig({
                  entries: config.entries.with(i, {...entry, alias}),
                }),
            }),
          ),
          onReorder: (from, to) =>
            updateConfig({entries: moveItem(config.entries, from, to)}),
          onDelete: (i) =>
            updateConfig({entries: config.entries.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Rename column',
          variant: ButtonVariant.Filled,
          icon: 'add',
          onclick: () => {
            updateConfig({
              entries: [...config.entries, {column: '', alias: ''}],
            });
          },
        }),
      ]);
    },
  };
}

function isValid(config: RenameConfig): boolean {
  return config.entries.every((e) => e.alias === '' || e.column !== '');
}

function renameMap(config: RenameConfig): Map<string, string> {
  return new Map(
    config.entries
      .filter((e) => e.column !== '' && e.alias !== '' && e.column !== e.alias)
      .map((e) => [e.column, e.alias]),
  );
}

export const manifest: NodeManifest<RenameConfig> = {
  title: 'Rename',
  icon: 'drive_file_rename_outline',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 144,
  schema: RenameConfigSchema,
  defaultConfig: () => ({entries: []}),
  render(config, updateConfig, ctx) {
    return m(RenameContent, {config, updateConfig, ctx});
  },
  getOutputColumns(config, ctx) {
    const columns = ctx.getInputColumns('input');
    if (columns === undefined) return undefined;
    const renameMap = new Map(
      config.entries
        .filter((e) => e.column !== '' && e.alias !== '')
        .map((e) => [e.column, e.alias]),
    );
    if (renameMap.size === 0) return columns;
    return columns.map((c) => ({
      ...c,
      name: renameMap.get(c.name) ?? c.name,
    }));
  },
  generators: {
    // Standard SQL needs the full column list to rename in place.
    sql: (config, ctx) => {
      if (!isValid(config)) return undefined;
      if (ctx.input('input') === undefined) return undefined;
      const renames = renameMap(config);
      if (renames.size === 0) return {};
      const inputColumns = ctx.getInputColumns('input');
      if (inputColumns === undefined) {
        return {
          error: 'Rename needs known input columns in Standard SQL mode.',
        };
      }
      return {
        select: inputColumns.map((c) => {
          const to = renames.get(c.name);
          return to !== undefined ? `${c.name} AS ${to}` : c.name;
        }),
      };
    },
    psn: (config, ctx) => {
      if (!isValid(config)) return undefined;
      if (ctx.input('input') === undefined) return undefined;
      const items = [...renameMap(config)].map(
        ([from, to]) => `${from} AS ${to}`,
      );
      return {stages: items.length > 0 ? [`RENAME ${items.join(', ')}`] : []};
    },
  },
};
