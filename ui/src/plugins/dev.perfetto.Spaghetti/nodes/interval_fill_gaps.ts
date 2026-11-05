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

const IntervalFillGapsConfigSchema = z.object({
  partitionColumns: z.array(z.string()),
});

export type IntervalFillGapsConfig = z.infer<
  typeof IntervalFillGapsConfigSchema
>;

function IntervalFillGapsContent(): m.Component<{
  config: IntervalFillGapsConfig;
  updateConfig: (updates: Partial<IntervalFillGapsConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const inputColumns = ctx.getInputColumns('input');
      return m(Stack, {style: {minWidth: '200px'}}, [
        m('.pf-spag-section-label', 'Partition by'),
        m(Stack, {compact: true}, [
          ...config.partitionColumns.map((col, i) =>
            m(Row, {key: i}, [
              m(ColumnPicker, {
                value: col,
                columns: inputColumns,
                placeholder: 'column',
                onSelect: (value: string) => {
                  const updated = [...config.partitionColumns];
                  updated[i] = value;
                  updateConfig({partitionColumns: updated});
                },
              }),
              m(Row.DeleteButton, {
                onclick: () =>
                  updateConfig({
                    partitionColumns: config.partitionColumns.filter(
                      (_, j) => j !== i,
                    ),
                  }),
              }),
            ]),
          ),
        ]),
        m(Button, {
          label: 'Partition',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () =>
            updateConfig({
              partitionColumns: [...config.partitionColumns, ''],
            }),
        }),
      ]);
    },
  };
}

function getOutputColumns(
  _config: IntervalFillGapsConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  return ctx.getInputColumns('input');
}

function toLogicalOp(
  config: IntervalFillGapsConfig,
  ctx: LogicalOpContext,
): LogicalOp | undefined {
  const input = ctx.getInputNodeId('input');
  if (!input) return undefined;
  const background = ctx.getInputNodeId('background');
  if (!background) {
    return {kind: 'passthrough', input};
  }
  return {
    kind: 'interval_fill_gaps',
    input,
    background,
    partitionColumns: config.partitionColumns.filter((c) => c !== ''),
    inputColumns: ctx.getInputColumns('input')?.map((c) => c.name),
    backgroundColumns: ctx.getInputColumns('background')?.map((c) => c.name),
  };
}

export const manifest: NodeManifest<IntervalFillGapsConfig> = {
  title: 'Fill Gaps',
  icon: 'space_bar',
  getInputs: () => [
    {name: 'input', content: 'Input'},
    {name: 'background', content: 'Background'},
  ],
  hue: 325,
  schema: IntervalFillGapsConfigSchema,
  defaultConfig: () => ({partitionColumns: []}),
  isValid: () => true,
  getOutputColumns,
  toLogicalOp,
  render: (config, updateConfig, ctx) =>
    m(IntervalFillGapsContent, {config, updateConfig, ctx}),
};
