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
import {Stack} from '../components/stack';
import type {ColumnDef} from '../graph_utils';
import type {LogicalOp} from '../ir';
import type {
  ColumnContext,
  LogicalOpContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const SetEntrySchema = z.object({
  column: z.string(),
  expression: z.string(),
});

export type SetEntry = z.infer<typeof SetEntrySchema>;

const SetConfigSchema = z
  .object({
    entries: z.array(SetEntrySchema),
  })
  .describe('Overwrite existing columns in place with SQL expressions.');

export type SetConfig = z.infer<typeof SetConfigSchema>;

function SetContent(): m.Component<{
  config: SetConfig;
  updateConfig: (updates: Partial<SetConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, {minWidth: 'lg'}, [
        config.entries.length === 0 &&
          m('.pf-spag-passthrough-hint', 'All columns (passthrough)'),
        m(DraggableStack, {
          rows: config.entries.map((entry, i) => [
            m(ColumnPicker, {
              value: entry.column,
              columns: ctx.availableColumns,
              placeholder: 'column',
              onSelect: (column) =>
                updateConfig({
                  entries: config.entries.with(i, {...entry, column}),
                }),
            }),
            m('span.pf-spag-operator', '='),
            m(TextInput, {
              placeholder: 'expression',
              value: entry.expression,
              onChange: (expression) =>
                updateConfig({
                  entries: config.entries.with(i, {...entry, expression}),
                }),
            }),
          ]),
          onReorder: (from, to) =>
            updateConfig({entries: moveItem(config.entries, from, to)}),
          onDelete: (i) =>
            updateConfig({entries: config.entries.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Set column',
          variant: ButtonVariant.Filled,
          icon: 'add',
          onclick: () => {
            updateConfig({
              entries: [...config.entries, {column: '', expression: ''}],
            });
          },
        }),
      ]);
    },
  };
}

function render(
  config: SetConfig,
  updateConfig: (updates: Partial<SetConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(SetContent, {config, updateConfig, ctx});
}

function getOutputColumns(
  config: SetConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  if (columns === undefined) return undefined;
  const overwritten = new Set(
    config.entries
      .filter((e) => e.column !== '' && e.expression !== '')
      .map((e) => e.column),
  );
  if (overwritten.size === 0) return columns;
  return columns.map((c) => (overwritten.has(c.name) ? {name: c.name} : c));
}

function isValid(config: SetConfig): boolean {
  return config.entries.every(
    (e) =>
      (e.column === '' && e.expression === '') ||
      (e.column !== '' && e.expression !== ''),
  );
}

function toLogicalOp(
  config: SetConfig,
  ctx: LogicalOpContext,
): LogicalOp | undefined {
  const input = ctx.getInputNodeId('input');
  if (!input) return undefined;
  const assignments = config.entries
    .filter((e) => e.column !== '' && e.expression !== '')
    .map((e) => ({column: e.column, expression: e.expression}));
  const inputColumns = ctx.getInputColumns('input')?.map((c) => c.name);
  return {
    kind: 'set',
    input,
    assignments,
    inputColumns,
  };
}

export const manifest: NodeManifest<SetConfig> = {
  title: 'Set',
  icon: 'edit_note',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 134,
  schema: SetConfigSchema,
  defaultConfig: () => ({entries: []}),
  render,
  getOutputColumns,
  isValid,
  toLogicalOp,
};
