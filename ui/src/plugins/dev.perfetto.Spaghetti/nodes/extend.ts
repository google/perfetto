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
import type {ColumnDef} from '../graph_utils';
import {DraggableStack} from '../components/draggable_stack';
import {Stack} from '../components/stack';
import {
  sanitizeAlias,
  TextInputWithAlias,
} from '../components/text_input_with_alias';

const ExtendEntrySchema = z.object({
  expression: z.string(),
  alias: z.string(),
});

export type ExtendEntry = z.infer<typeof ExtendEntrySchema>;

const ExtendConfigSchema = z
  .object({
    entries: z.array(ExtendEntrySchema),
  })
  .describe('Keep all input columns and append computed SQL expressions.');

export type ExtendConfig = z.infer<typeof ExtendConfigSchema>;

function entryAlias(e: ExtendEntry): string {
  return e.alias || sanitizeAlias(e.expression);
}

function ExtendContent(): m.Component<{
  config: ExtendConfig;
  updateConfig: (updates: Partial<ExtendConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig}}) {
      return m(Stack, {minWidth: 'lg'}, [
        m(DraggableStack, {
          rows: config.entries.map((entry, i) =>
            m(TextInputWithAlias, {
              placeholder: 'expression',
              value: entry.expression,
              onChange: (expression) =>
                updateConfig({
                  entries: config.entries.with(i, {...entry, expression}),
                }),
              alias: entry.alias,
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
          label: 'Add column',
          variant: ButtonVariant.Filled,
          icon: 'add',
          onclick: () => {
            updateConfig({
              entries: [...config.entries, {expression: '', alias: ''}],
            });
          },
        }),
      ]);
    },
  };
}

function render(
  config: ExtendConfig,
  updateConfig: (updates: Partial<ExtendConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(ExtendContent, {config, updateConfig, ctx});
}

function getOutputColumns(
  config: ExtendConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  const added: ColumnDef[] = config.entries
    .filter((e) => e.expression)
    .map((e) => ({name: entryAlias(e)}));
  if (added.length > 0) {
    return [...(columns ?? []), ...added];
  }
  return columns;
}

function exprParts(config: ExtendConfig): string[] {
  return config.entries
    .filter((e) => e.expression !== '')
    .map((e) => `${e.expression} AS ${entryAlias(e)}`);
}

const generators: NodeGenerators<ExtendConfig> = {
  sql: (config, ctx) => {
    if (ctx.input('input') === undefined) return undefined;
    const exprs = exprParts(config);
    return exprs.length > 0 ? {select: ['*', ...exprs]} : {};
  },
  psn: (config, ctx) => {
    if (ctx.input('input') === undefined) return undefined;
    const exprs = exprParts(config);
    return {stages: exprs.length > 0 ? [`EXTEND ${exprs.join(', ')}`] : []};
  },
};

export const manifest: NodeManifest<ExtendConfig> = {
  title: 'Extend',
  icon: 'add_column_right',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 124,
  schema: ExtendConfigSchema,
  defaultConfig: () => ({entries: []}),
  render,
  getOutputColumns,
  generators,
};
