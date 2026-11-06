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
import {RadioGroup} from '../../../widgets/radio_group';
import {TextInput} from '../../../widgets/text_input';
import {DraggableStack} from '../components/draggable_stack';
import {Stack} from '../components/stack';
import type {ColumnDef} from '../graph_utils';
import type {EventsLastMode, LogicalOp} from '../ir';
import type {
  ColumnContext,
  LogicalOpContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {ColumnPicker} from '../widgets/column_picker';

const IntervalsFromEventsConfigSchema = z.object({
  partitionColumns: z.array(z.string()),
  lastMode: z.enum(['CLOSE_AT', 'KEEP_OPEN']),
  closeAtExpr: z.string(),
});

export type IntervalsFromEventsConfig = z.infer<
  typeof IntervalsFromEventsConfigSchema
>;

function IntervalsFromEventsContent(): m.Component<{
  readonly config: IntervalsFromEventsConfig;
  readonly updateConfig: (updates: Partial<IntervalsFromEventsConfig>) => void;
  readonly ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig, ctx}}) {
      const inputColumns = ctx.getInputColumns('input');
      return m(Stack, [
        m('.pf-spag-section-label', 'Partition by'),
        m(DraggableStack, {
          rows: config.partitionColumns.map((col, i) =>
            m(ColumnPicker, {
              value: col,
              columns: inputColumns,
              placeholder: 'column',
              onSelect: (value) =>
                updateConfig({
                  partitionColumns: config.partitionColumns.with(i, value),
                }),
            }),
          ),
          onReorder: (from, to) =>
            updateConfig({
              partitionColumns: moveItem(config.partitionColumns, from, to),
            }),
          onDelete: (i) =>
            updateConfig({
              partitionColumns: config.partitionColumns.toSpliced(i, 1),
            }),
        }),
        m(Button, {
          label: 'Partition',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () =>
            updateConfig({
              partitionColumns: [...config.partitionColumns, ''],
            }),
        }),
        m('.pf-spag-section-label', 'Last event'),
        m(
          RadioGroup,
          {
            fillWidth: true,
            selectedValue: config.lastMode,
            onValueChange: (value: string) =>
              updateConfig({lastMode: value as EventsLastMode}),
          },
          [
            m(RadioGroup.Button, {value: 'CLOSE_AT'}, 'Close at'),
            m(RadioGroup.Button, {value: 'KEEP_OPEN'}, 'Keep open'),
          ],
        ),
        config.lastMode === 'CLOSE_AT' &&
          m(TextInput, {
            value: config.closeAtExpr,
            placeholder: 'trace_end()',
            oninput: (e: InputEvent) =>
              updateConfig({
                closeAtExpr: (e.target as HTMLInputElement).value,
              }),
          }),
      ]);
    },
  };
}

function getOutputColumns(
  _config: IntervalsFromEventsConfig,
  ctx: ColumnContext,
): ColumnDef[] | undefined {
  const columns = ctx.getInputColumns('input');
  if (columns === undefined) return undefined;
  const hasDur = columns.some((c) => c.name === 'dur');
  const out: ColumnDef[] = [];
  let insertedDur = false;
  for (const col of columns) {
    if (col.name === 'ts') {
      out.push({name: 'ts', type: {kind: 'timestamp' as const}});
      if (!hasDur && !insertedDur) {
        out.push({name: 'dur', type: {kind: 'duration' as const}});
        insertedDur = true;
      }
    } else if (col.name === 'dur') {
      out.push({name: 'dur', type: {kind: 'duration' as const}});
      insertedDur = true;
    } else {
      out.push(col);
    }
  }
  if (!insertedDur) {
    out.push({name: 'dur', type: {kind: 'duration' as const}});
  }
  return out;
}

function isValid(config: IntervalsFromEventsConfig): boolean {
  return config.lastMode === 'KEEP_OPEN' || config.closeAtExpr.trim() !== '';
}

function toLogicalOp(
  config: IntervalsFromEventsConfig,
  ctx: LogicalOpContext,
): LogicalOp | undefined {
  const input = ctx.getInputNodeId('input');
  if (input === undefined || !isValid(config)) return undefined;
  const stop = ctx.getInputNodeId('stop');
  return {
    kind: 'intervals_from_events',
    input,
    stop,
    partitionColumns: config.partitionColumns.filter((c) => c !== ''),
    lastMode: config.lastMode,
    closeAtExpr: config.closeAtExpr.trim(),
    inputColumns: ctx.getInputColumns('input')?.map((c) => c.name),
  };
}

export const manifest: NodeManifest<IntervalsFromEventsConfig> = {
  title: 'Intervals From Events',
  icon: 'timeline',
  getInputs: () => [
    {name: 'input', content: 'Input'},
    {name: 'stop', content: 'Stop'},
  ],
  hue: 348,
  schema: IntervalsFromEventsConfigSchema,
  defaultConfig: () => ({
    partitionColumns: [],
    lastMode: 'CLOSE_AT',
    closeAtExpr: 'trace_end()',
  }),
  render: (config, updateConfig, ctx) =>
    m(IntervalsFromEventsContent, {config, updateConfig, ctx}),
  getOutputColumns,
  isValid,
  toLogicalOp,
};
