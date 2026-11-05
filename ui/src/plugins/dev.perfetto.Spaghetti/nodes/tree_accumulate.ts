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
import {RadioGroup} from '../../../widgets/radio_group';
import {Select} from '../../../widgets/select';
import {AliasTag} from '../components/alias_tag';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import type {ColumnDef} from '../graph_utils';
import type {LogicalOp, PipelineAggFunc} from '../logical_ir';
import type {
  ColumnContext,
  LogicalOpContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const TreeAggSchema = z.object({
  func: z.enum(['COUNT', 'SUM', 'MIN', 'MAX']),
  column: z.string(),
  alias: z.string(),
});

export type TreeAggregation = z.infer<typeof TreeAggSchema>;

const TreeAccumulateConfigSchema = z.object({
  direction: z.enum(['UP', 'DOWN']),
  idColumn: z.string(),
  parentIdColumn: z.string(),
  aggregations: z.array(TreeAggSchema),
});

export type TreeAccumulateConfig = z.infer<typeof TreeAccumulateConfigSchema>;

function aggAlias(a: TreeAggregation, direction: 'UP' | 'DOWN'): string {
  if (a.alias !== '') return a.alias;
  const prefix = direction === 'UP' ? 'subtree' : 'path';
  if (a.func === 'COUNT') return `${prefix}_count`;
  return `${prefix}_${a.func.toLowerCase()}_${a.column}`;
}

function TreeAccumulateContent(): m.Component<{
  config: TreeAccumulateConfig;
  updateConfig: (updates: Partial<TreeAccumulateConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      return m(Stack, {style: {minWidth: '250px'}}, [
        m(
          RadioGroup,
          {
            fillWidth: true,
            selectedValue: config.direction,
            onValueChange: (value: string) =>
              updateConfig({direction: value as 'UP' | 'DOWN'}),
          },
          [
            m(RadioGroup.Button, {value: 'UP'}, 'Up (Subtree)'),
            m(RadioGroup.Button, {value: 'DOWN'}, 'Down (Ancestors)'),
          ],
        ),
        m('.pf-spag-section-label', 'Tree columns'),
        m(Row, [
          m(ColumnPicker, {
            value: config.idColumn,
            columns: ctx.availableColumns,
            placeholder: 'id',
            onSelect: (value: string) => updateConfig({idColumn: value}),
          }),
          m('span', {style: {opacity: 0.5}}, '→'),
          m(ColumnPicker, {
            value: config.parentIdColumn,
            columns: ctx.availableColumns,
            placeholder: 'parent_id',
            onSelect: (value: string) => updateConfig({parentIdColumn: value}),
          }),
        ]),
        m('.pf-spag-section-label', 'Aggregations'),
        m(Stack, {compact: true}, [
          ...config.aggregations.map((agg, i) =>
            m(Row, {key: i}, [
              m(
                Select,
                {
                  value: agg.func,
                  onchange: (e: Event) => {
                    const nextFunc = (e.target as HTMLSelectElement)
                      .value as PipelineAggFunc;
                    const updated = [...config.aggregations];
                    updated[i] = {
                      ...agg,
                      func: nextFunc,
                      column:
                        nextFunc === 'COUNT'
                          ? '*'
                          : agg.column === '*'
                            ? ''
                            : agg.column,
                    };
                    updateConfig({aggregations: updated});
                  },
                },
                [
                  m('option', {value: 'COUNT'}, 'COUNT'),
                  m('option', {value: 'SUM'}, 'SUM'),
                  m('option', {value: 'MIN'}, 'MIN'),
                  m('option', {value: 'MAX'}, 'MAX'),
                ],
              ),
              agg.func !== 'COUNT' &&
                m(ColumnPicker, {
                  value: agg.column,
                  columns: ctx.availableColumns,
                  placeholder: 'column',
                  onSelect: (value: string) => {
                    const updated = [...config.aggregations];
                    updated[i] = {...agg, column: value};
                    updateConfig({aggregations: updated});
                  },
                }),
              m(AliasTag, {
                alias: agg.alias,
                placeholder: aggAlias(agg, config.direction),
                onChange: (value: string) => {
                  const updated = [...config.aggregations];
                  updated[i] = {...agg, alias: value};
                  updateConfig({aggregations: updated});
                },
              }),
              m(Row.DeleteButton, {
                onclick: () =>
                  updateConfig({
                    aggregations: config.aggregations.filter((_, j) => j !== i),
                  }),
              }),
            ]),
          ),
        ]),
        m(Button, {
          label: 'Aggregation',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () =>
            updateConfig({
              aggregations: [
                ...config.aggregations,
                {func: 'COUNT', column: '*', alias: ''},
              ],
            }),
        }),
      ]);
    },
  };
}

function getOutputColumns(
  config: TreeAccumulateConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  if (columns === undefined) return undefined;
  const added: ColumnDef[] = config.aggregations
    .filter((a) => a.func === 'COUNT' || a.column !== '')
    .map((a) => ({
      name: aggAlias(a, config.direction),
      type: {kind: 'int' as const},
    }));
  return [...columns, ...added];
}

function isValid(config: TreeAccumulateConfig): boolean {
  return (
    config.idColumn !== '' &&
    config.parentIdColumn !== '' &&
    config.aggregations.some((a) => a.func === 'COUNT' || a.column !== '') &&
    config.aggregations.every((a) => a.func === 'COUNT' || a.column !== '')
  );
}

function toLogicalOp(
  config: TreeAccumulateConfig,
  ctx: LogicalOpContext,
): LogicalOp | undefined {
  const input = ctx.getInputNodeId('input');
  if (!input || !isValid(config)) return undefined;
  return {
    kind: 'tree_accumulate',
    input,
    direction: config.direction,
    idColumn: config.idColumn,
    parentIdColumn: config.parentIdColumn,
    aggregations: config.aggregations.map((a) => ({
      func: a.func,
      column: a.func === 'COUNT' ? '*' : a.column,
      alias: aggAlias(a, config.direction),
    })),
  };
}

export const manifest: NodeManifest<TreeAccumulateConfig> = {
  title: 'Tree Accumulate',
  icon: 'mediation',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 290,
  schema: TreeAccumulateConfigSchema,
  defaultConfig: () => ({
    direction: 'UP',
    idColumn: 'id',
    parentIdColumn: 'parent_id',
    aggregations: [{func: 'COUNT', column: '*', alias: ''}],
  }),
  render: (config, updateConfig, ctx) =>
    m(TreeAccumulateContent, {config, updateConfig, ctx}),
  getOutputColumns,
  isValid,
  toLogicalOp,
};
