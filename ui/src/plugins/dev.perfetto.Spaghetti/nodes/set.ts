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
import {TextInput} from '../../../widgets/text_input';
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

const SetEntrySchema = z.object({
  column: z.string(),
  expression: z.string(),
});

export type SetEntry = z.infer<typeof SetEntrySchema>;

const SetConfigSchema = z.object({
  entries: z.array(SetEntrySchema),
});

export type SetConfig = z.infer<typeof SetConfigSchema>;

function SetContent(): m.Component<{
  config: SetConfig;
  updateConfig: (updates: Partial<SetConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, {style: {minWidth: '250px'}}, [
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
              m('span', {style: {opacity: 0.5}}, '='),
              m(TextInput, {
                placeholder: 'expression',
                value: entry.expression,
                onChange: (value: string) => {
                  const updated = [...config.entries];
                  updated[i] = {...entry, expression: value};
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
  hue: 38,
  schema: SetConfigSchema,
  defaultConfig: () => ({entries: []}),
  render,
  getOutputColumns,
  isValid,
  toLogicalOp,
};
