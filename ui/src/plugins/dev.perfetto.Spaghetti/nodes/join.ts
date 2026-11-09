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
import type {NodeManifest, RenderContext} from '../node_types';
import type {ColumnDef} from '../graph_utils';
import {Button, ButtonVariant} from '../../../widgets/button';
import {ColumnPickerWithAlias} from '../components/column_picker_with_alias';
import {DraggableStack} from '../components/draggable_stack';
import {Stack} from '../components/stack';
import './join.scss';
import {ColumnPicker} from '../widgets/column_picker';
import {RadioGroup} from '../../../widgets/radio_group';

const JoinColumnSchema = z.object({
  column: z.string(),
  alias: z.string(),
});

export type JoinColumn = z.infer<typeof JoinColumnSchema>;

const JoinConfigSchema = z
  .object({
    leftColumn: z.string(),
    rightColumn: z.string(),
    columns: z
      .array(JoinColumnSchema)
      .optional()
      .describe(
        'Right-side columns to append (all left columns are kept automatically)',
      ),
    joinType: z.enum(['LEFT', 'INNER']),
  })
  .describe('Join two inputs on leftColumn = rightColumn.');

export type JoinConfig = z.infer<typeof JoinConfigSchema>;

// Compute the resolved aliases for all extend columns.
export function getExtendColumnAliases(
  config: JoinConfig,
  leftAvail: string[],
): JoinColumn[] {
  const leftSet = new Set(leftAvail);
  const columns = config.columns ?? [];
  return columns.map((c) => ({
    column: c.column,
    alias: c.alias || (leftSet.has(c.column) ? `right_${c.column}` : c.column),
  }));
}

function JoinNodeContent(): m.Component<{
  config: JoinConfig;
  updateConfig: (updates: Partial<JoinConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const leftColumns = ctx.getInputColumns('left');
      const rightColumns = ctx.getInputColumns('right');
      const columns = config.columns ?? [];
      const leftSet = new Set(leftColumns.map((c) => c.name));

      const joinType = config.joinType;
      return m(Stack, {minWidth: 'sm'}, [
        m(
          RadioGroup,
          {
            fillWidth: true,
            selectedValue: joinType,
            onValueChange: (value: string) =>
              updateConfig({joinType: value as 'LEFT' | 'INNER'}),
          },
          [
            m(RadioGroup.Button, {value: 'LEFT'}, 'Left'),
            m(RadioGroup.Button, {value: 'INNER'}, 'Inner'),
          ],
        ),
        m('.pf-spag-section-label', 'Join on'),
        m('.pf-spag-group-grid', [
          m(ColumnPicker, {
            value: config.leftColumn,
            columns: leftColumns,
            placeholder: 'left col',
            className: 'pf-spag-join-col',
            onSelect: (value: string) => {
              updateConfig({leftColumn: value});
            },
          }),
          m('span.pf-spag-operator', '='),
          m(ColumnPicker, {
            value: config.rightColumn,
            columns: rightColumns,
            placeholder: 'right col',
            className: 'pf-spag-join-col',
            onSelect: (value: string) => {
              updateConfig({rightColumn: value});
            },
          }),
        ]),

        m('.pf-spag-section-label', 'Columns to add'),
        m(DraggableStack, {
          rows: columns.map((entry, i) => {
            const defaultAlias = leftSet.has(entry.column)
              ? `right_${entry.column}`
              : entry.column;
            return m(ColumnPickerWithAlias, {
              value: entry.column,
              columns: rightColumns,
              onSelect: (column) =>
                updateConfig({columns: columns.with(i, {...entry, column})}),
              alias: entry.alias,
              aliasPlaceholder: defaultAlias,
              onAliasChange: (alias) =>
                updateConfig({columns: columns.with(i, {...entry, alias})}),
            });
          }),
          onReorder: (from, to) =>
            updateConfig({columns: moveItem(columns, from, to)}),
          onDelete: (i) => updateConfig({columns: columns.toSpliced(i, 1)}),
        }),
        m(Button, {
          label: 'Column',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () => {
            updateConfig({
              columns: [...columns, {column: '', alias: ''}],
            });
          },
        }),
      ]);
    },
  };
}

function render(
  config: JoinConfig,
  updateConfig: (updates: Partial<JoinConfig>) => void,
  ctx: RenderContext,
): m.Children {
  return m(JoinNodeContent, {config, updateConfig, ctx});
}

function isValid(config: JoinConfig): boolean {
  return config.leftColumn !== '' && config.rightColumn !== '';
}

/** `l.*` plus the picked right-hand columns (aliased on name clashes). */
function joinSelect(
  config: JoinConfig,
  ctx: {getInputColumns(port: string): ColumnDef[] | undefined},
): string[] {
  const leftCols = (ctx.getInputColumns('left') ?? []).map((c) => c.name);
  const picked = getExtendColumnAliases(config, leftCols)
    .filter((c) => c.column !== '')
    .map((c) =>
      c.alias !== c.column ? `r.${c.column} AS ${c.alias}` : `r.${c.column}`,
    );
  return ['l.*', ...picked];
}

export const manifest: NodeManifest<JoinConfig> = {
  title: 'Join',
  icon: 'join_left',
  resolveIcon: (config) =>
    config.joinType === 'LEFT' ? 'join_left' : 'join_inner',
  getInputs: () => [
    {name: 'left', content: 'Left'},
    {name: 'right', content: 'Right'},
  ],
  hue: 282,
  expensive: true,
  schema: JoinConfigSchema,
  getOutputColumns(config, ctx) {
    // Output columns are all left columns, plus select right columns.
    const leftCols = ctx.getInputColumns('left') ?? [];
    const rightCols = ctx.getInputColumns('right') ?? [];
    const leftSet = new Set(leftCols.map((c) => c.name));
    const outputCols = [...leftCols];
    const columns = config.columns ?? [];

    for (const entry of columns) {
      const colName = entry.column;
      if (!colName) continue;
      const rightCol = rightCols.find((c) => c.name === colName);
      if (rightCol) {
        const alias =
          entry.alias || (leftSet.has(colName) ? `right_${colName}` : colName);
        outputCols.push({name: alias, type: rightCol.type});
      }
    }

    return outputCols;
  },
  defaultConfig: () => ({
    joinType: 'LEFT',
    leftColumn: '',
    rightColumn: '',
    columns: [],
  }),
  render,
  generators: {
    // No right input -> passthrough of the left input.
    sql: (config, ctx) => {
      if (!isValid(config)) return undefined;
      const left = ctx.input('left');
      const right = ctx.input('right');
      if (left === undefined) return undefined;
      if (right === undefined) return {};
      const kw = config.joinType === 'INNER' ? 'JOIN' : 'LEFT JOIN';
      return {
        from:
          `${left.ref} AS l\n` +
          `${kw} ${right.ref} AS r ON l.${config.leftColumn} = r.${config.rightColumn}`,
        select: joinSelect(config, ctx),
      };
    },
    psn: (config, ctx) => {
      if (!isValid(config)) return undefined;
      const left = ctx.input('left');
      const right = ctx.input('right');
      if (left === undefined) return undefined;
      if (right === undefined) return {};
      const kw = config.joinType === 'INNER' ? 'JOIN' : 'LEFT JOIN';
      return {
        stages: [
          'AS l',
          `${kw} ${right.source} AS r ON l.${config.leftColumn} = r.${config.rightColumn}`,
          `SELECT ${joinSelect(config, ctx).join(', ')}`,
        ],
      };
    },
  },
};
