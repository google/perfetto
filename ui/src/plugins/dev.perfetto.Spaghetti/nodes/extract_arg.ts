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
  LogicalOpContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import type {LogicalOp} from '../logical_ir';
import {Button, ButtonVariant} from '../../../widgets/button';
import {DraggableStack} from '../components/draggable_stack';
import {Stack} from '../components/stack';
import {
  sanitizeAlias,
  TextInputWithAlias,
} from '../components/text_input_with_alias';
import {ColumnPicker} from '../widgets/column_picker';
import type {ColumnDef} from '../graph_utils';

const ExtractArgEntrySchema = z.object({
  argName: z.string(),
  alias: z.string(),
});

export type ExtractArgEntry = z.infer<typeof ExtractArgEntrySchema>;

const ExtractArgConfigSchema = z.object({
  argSetIdCol: z.string(),
  extractions: z.array(ExtractArgEntrySchema),
});

export type ExtractArgConfig = z.infer<typeof ExtractArgConfigSchema>;

function extractArgAlias(e: ExtractArgEntry): string {
  return e.alias || sanitizeAlias(e.argName);
}

function ExtractArgContent(): m.Component<{
  config: ExtractArgConfig;
  updateConfig: (updates: Partial<ExtractArgConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, [
        m('.pf-spag-section-label', 'Join Key'),
        m(ColumnPicker, {
          value: config.argSetIdCol,
          columns: ctx.availableColumns,
          placeholder: 'column',
          onSelect: (value: string) => {
            updateConfig({argSetIdCol: value});
          },
        }),
        m('.pf-spag-section-label', 'Args'),
        m(DraggableStack, {
          rows: config.extractions.map((entry, i) =>
            m(TextInputWithAlias, {
              placeholder: 'arg name',
              value: entry.argName,
              onChange: (argName) =>
                updateConfig({
                  extractions: config.extractions.with(i, {...entry, argName}),
                }),
              alias: entry.alias,
              onAliasChange: (alias) =>
                updateConfig({
                  extractions: config.extractions.with(i, {...entry, alias}),
                }),
            }),
          ),
          onReorder: (from, to) =>
            updateConfig({
              extractions: moveItem(config.extractions, from, to),
            }),
          onDelete: (i) =>
            updateConfig({extractions: config.extractions.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Add arg',
          variant: ButtonVariant.Filled,
          icon: 'add',
          onclick: () => {
            updateConfig({
              extractions: [...config.extractions, {argName: '', alias: ''}],
            });
          },
        }),
      ]);
    },
  };
}

function render(
  config: ExtractArgConfig,
  updateConfig: (updates: Partial<ExtractArgConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(ExtractArgContent, {config, updateConfig, ctx});
}

function getOutputColumns(
  config: ExtractArgConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  const extractAliases: ColumnDef[] = config.extractions
    .filter((e) => e.argName)
    .map((e) => ({name: extractArgAlias(e)}));
  if (extractAliases.length > 0) {
    return [...(columns ?? []), ...extractAliases];
  }
  return columns;
}

function isValid(config: ExtractArgConfig): boolean {
  return config.extractions.every((e) => e.alias === '' || e.argName !== '');
}

function toLogicalOp(
  config: ExtractArgConfig,
  ctx: LogicalOpContext,
): LogicalOp | undefined {
  const input = ctx.getInputNodeId('input');
  if (!input) return undefined;
  return {
    kind: 'extract_arg',
    input,
    argSetIdCol: config.argSetIdCol,
    extractions: config.extractions
      .filter((e) => e.argName !== '')
      .map((e) => ({argName: e.argName, alias: extractArgAlias(e)})),
  };
}

export const manifest: NodeManifest<ExtractArgConfig> = {
  title: 'Extract Args',
  icon: 'data_object',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 174,
  schema: ExtractArgConfigSchema,
  defaultConfig: () => ({argSetIdCol: '', extractions: []}),
  render,
  getOutputColumns,
  isValid,
  toLogicalOp,
};
