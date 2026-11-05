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
import {Button, ButtonVariant} from '../../../widgets/button';
import {AliasTag} from '../components/alias_tag';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import type {ColumnDef} from '../graph_utils';
import type {LogicalOp} from '../logical_ir';
import type {
  ColumnContext,
  LogicalOpContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const RenameEntrySchema = z.object({
  column: z.string(),
  alias: z.string(),
});

export type RenameEntry = z.infer<typeof RenameEntrySchema>;

const RenameConfigSchema = z.object({
  entries: z.array(RenameEntrySchema),
});

export type RenameConfig = z.infer<typeof RenameConfigSchema>;

function RenameContent(): m.Component<{
  config: RenameConfig;
  updateConfig: (updates: Partial<RenameConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, {style: {minWidth: '220px'}}, [
        config.entries.length === 0 &&
          m(
            '.pf-spag-passthrough-hint',
            {
              style: {
                opacity: 0.5,
                fontStyle: 'italic',
                fontSize: '11px',
                padding: '2px 4px',
              },
            },
            'All columns (passthrough)',
          ),
        m(Stack, {compact: true}, [
          ...config.entries.map((entry, i) =>
            m(Row, {key: i}, [
              m(ColumnPicker, {
                value: entry.column,
                columns: ctx.availableColumns,
                placeholder: 'column',
                onSelect: (value: string) => {
                  const updated = [...config.entries];
                  updated[i] = {...entry, column: value};
                  updateConfig({entries: updated});
                },
              }),
              m(AliasTag, {
                alias: entry.alias,
                placeholder: entry.column || 'new name',
                onChange: (value: string) => {
                  const updated = [...config.entries];
                  updated[i] = {...entry, alias: value};
                  updateConfig({entries: updated});
                },
              }),
              m(Row.DeleteButton, {
                onclick: () =>
                  updateConfig({
                    entries: config.entries.filter((_, j) => j !== i),
                  }),
              }),
            ]),
          ),
        ]),
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

function render(
  config: RenameConfig,
  updateConfig: (updates: Partial<RenameConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(RenameContent, {config, updateConfig, ctx});
}

function getOutputColumns(
  config: RenameConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
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
}

function isValid(config: RenameConfig): boolean {
  return config.entries.every((e) => e.alias === '' || e.column !== '');
}

function toLogicalOp(
  config: RenameConfig,
  ctx: LogicalOpContext,
): LogicalOp | undefined {
  const input = ctx.getInputNodeId('input');
  if (!input) return undefined;
  const renames = config.entries
    .filter((e) => e.column !== '' && e.alias !== '' && e.column !== e.alias)
    .map((e) => ({from: e.column, to: e.alias}));
  const inputColumns = ctx.getInputColumns('input')?.map((c) => c.name);
  return {
    kind: 'rename',
    input,
    renames,
    inputColumns,
  };
}

export const manifest: NodeManifest<RenameConfig> = {
  title: 'Rename',
  icon: 'drive_file_rename_outline',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 25,
  schema: RenameConfigSchema,
  defaultConfig: () => ({entries: []}),
  render,
  getOutputColumns,
  isValid,
  toLogicalOp,
};
