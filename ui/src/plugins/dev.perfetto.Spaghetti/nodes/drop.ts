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
import type {
  ColumnContext,
  NodeGenerators,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {DraggableStack} from '../components/draggable_stack';
import {Stack} from '../components/stack';
import {ColumnPicker} from '../widgets/column_picker';
import type {ColumnDef} from '../graph_utils';

const DropConfigSchema = z
  .object({
    // Names of the input columns to drop.
    columns: z.array(z.string()),
  })
  .describe('Drop named columns from the input.');

export type DropConfig = z.infer<typeof DropConfigSchema>;

function DropContent(): m.Component<{
  config: DropConfig;
  updateConfig: (updates: Partial<DropConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, [
        config.columns.length === 0 &&
          m('.pf-spag-passthrough-hint', 'All columns (passthrough)'),
        m(DraggableStack, {
          rows: config.columns.map((column, i) =>
            m(ColumnPicker, {
              value: column,
              columns: ctx.availableColumns,
              placeholder: 'column',
              onSelect: (value) =>
                updateConfig({columns: config.columns.with(i, value)}),
            }),
          ),
          onReorder: (from, to) =>
            updateConfig({columns: moveItem(config.columns, from, to)}),
          onDelete: (i) =>
            updateConfig({columns: config.columns.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Drop column',
          variant: ButtonVariant.Filled,
          icon: 'add',
          onclick: () => {
            updateConfig({columns: [...config.columns, '']});
          },
        }),
      ]);
    },
  };
}

function render(
  config: DropConfig,
  updateConfig: (updates: Partial<DropConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(DropContent, {config, updateConfig, ctx});
}

function getOutputColumns(
  config: DropConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  if (columns === undefined) return undefined;
  const dropped = new Set(config.columns.filter((c) => c));
  return columns.filter((c) => !dropped.has(c.name));
}

const generators: NodeGenerators<DropConfig> = {
  // Standard SQL has no `SELECT * EXCEPT`, so we need the input columns.
  sql: (config, ctx) => {
    if (ctx.input('input') === undefined) return undefined;
    const dropped = new Set(config.columns.filter((c) => c !== ''));
    if (dropped.size === 0) return {};
    const inputColumns = ctx.getInputColumns('input');
    if (inputColumns === undefined) {
      return {error: 'Drop needs known input columns in Standard SQL mode.'};
    }
    return {
      select: inputColumns
        .filter((c) => !dropped.has(c.name))
        .map((c) => c.name),
    };
  },
  psn: (config, ctx) => {
    if (ctx.input('input') === undefined) return undefined;
    const dropped = config.columns.filter((c) => c !== '');
    return {stages: dropped.length > 0 ? [`DROP ${dropped.join(', ')}`] : []};
  },
};

export const manifest: NodeManifest<DropConfig> = {
  title: 'Drop',
  icon: 'remove',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 154,
  schema: DropConfigSchema,
  defaultConfig: () => ({columns: []}),
  render,
  getOutputColumns,
  generators,
};
