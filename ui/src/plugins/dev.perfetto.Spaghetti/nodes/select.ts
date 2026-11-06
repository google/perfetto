// Copyright (C) 2025 The Android Open Source Project
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
import type {LogicalOp} from '../ir';
import {Button, ButtonVariant} from '../../../widgets/button';
import {ColumnPickerWithAlias} from '../components/column_picker_with_alias';
import {DraggableStack} from '../components/draggable_stack';
import {Stack} from '../components/stack';
import {
  sanitizeAlias,
  TextInputWithAlias,
} from '../components/text_input_with_alias';
import type {ColumnDef} from '../graph_utils';

const SelectEntrySchema = z.object({
  column: z.string(),
  alias: z.string(),
});

export type SelectEntry = z.infer<typeof SelectEntrySchema>;

const SelectExpressionSchema = z.object({
  expression: z.string(),
  alias: z.string(),
});

export type SelectExpression = z.infer<typeof SelectExpressionSchema>;

const SelectConfigSchema = z
  .object({
    entries: z.array(SelectEntrySchema).optional(),
    expressions: z.array(SelectExpressionSchema).optional(),
  })
  .describe(
    'Project specific columns and/or computed SQL expressions (empty entries keeps all input columns; alias "" keeps original name).',
  );

export type SelectConfig = z.infer<typeof SelectConfigSchema>;

function exprAlias(e: SelectExpression): string {
  return e.alias || sanitizeAlias(e.expression);
}

function SelectNodeContent(): m.Component<{
  config: SelectConfig;
  updateConfig: (updates: Partial<SelectConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const availableColumns = ctx.availableColumns;
      const entries = config.entries ?? [];
      const expressions = config.expressions ?? [];

      return m(Stack, [
        m('.pf-spag-section-label', 'Columns'),
        entries.length === 0 &&
          m('.pf-spag-passthrough-hint', 'All columns (passthrough)'),
        m(DraggableStack, {
          rows: entries.map((entry, i) =>
            m(ColumnPickerWithAlias, {
              value: entry.column,
              columns: availableColumns,
              onSelect: (column) =>
                updateConfig({entries: entries.with(i, {...entry, column})}),
              alias: entry.alias,
              onAliasChange: (alias) =>
                updateConfig({entries: entries.with(i, {...entry, alias})}),
            }),
          ),
          onReorder: (from, to) =>
            updateConfig({entries: moveItem(entries, from, to)}),
          onDelete: (i) => updateConfig({entries: entries.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Add column',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () => {
            updateConfig({
              entries: [...entries, {column: '', alias: ''}],
            });
          },
        }),

        m('.pf-spag-section-label', 'Expressions'),
        m(DraggableStack, {
          rows: expressions.map((expr, i) =>
            m(TextInputWithAlias, {
              placeholder: 'expression',
              value: expr.expression,
              onChange: (expression) =>
                updateConfig({
                  expressions: expressions.with(i, {...expr, expression}),
                }),
              alias: expr.alias,
              onAliasChange: (alias) =>
                updateConfig({
                  expressions: expressions.with(i, {...expr, alias}),
                }),
            }),
          ),
          onReorder: (from, to) =>
            updateConfig({expressions: moveItem(expressions, from, to)}),
          onDelete: (i) =>
            updateConfig({expressions: expressions.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Add expression',
          variant: ButtonVariant.Filled,
          icon: 'add',
          onclick: () => {
            updateConfig({
              expressions: [...expressions, {expression: '', alias: ''}],
            });
          },
        }),
      ]);
    },
  };
}

function getOutputColumns(
  config: SelectConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  const entries = config.entries ?? [];
  const expressions = config.expressions ?? [];
  const hasEntries = entries.some((e) => e.column);
  const exprAliases: ColumnDef[] = expressions
    .filter((e) => e.expression)
    .map((e) => ({name: exprAlias(e)}));

  // No explicit columns: pass through all input columns (SELECT *)
  // plus any expressions on top.
  if (!hasEntries) {
    return [...(columns ?? []), ...exprAliases];
  }

  const selectedEntries: ColumnDef[] = entries
    .filter((e) => e.column)
    .map((e) => {
      const name = e.alias || e.column;
      const orig = columns?.find((c) => c.name === e.column);
      return {name, type: orig?.type};
    });
  return [...selectedEntries, ...exprAliases];
}

function isValid(config: SelectConfig): boolean {
  const expressions = config.expressions ?? [];
  return expressions.every((e) => e.alias === '' || e.expression !== '');
}

function toLogicalOp(
  config: SelectConfig,
  ctx: LogicalOpContext,
): LogicalOp | undefined {
  const input = ctx.getInputNodeId('input');
  if (!input) return undefined;
  const entries = config.entries ?? [];
  const expressions = config.expressions ?? [];
  return {
    kind: 'project',
    input,
    columns: entries
      .filter((e) => e.column !== '')
      .map((e) => ({
        column: e.column,
        alias: e.alias !== '' ? e.alias : undefined,
      })),
    expressions: expressions
      .filter((e) => e.expression !== '')
      .map((e) => ({expression: e.expression, alias: exprAlias(e)})),
  };
}

export const manifest: NodeManifest<SelectConfig> = {
  title: 'Select',
  icon: 'view_column',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 114,
  schema: SelectConfigSchema,
  defaultConfig: () => ({entries: [], expressions: []}),
  render(config, updateConfig, ctx) {
    return m(SelectNodeContent, {config, updateConfig, ctx});
  },
  getOutputColumns,
  isValid,
  toLogicalOp,
};
