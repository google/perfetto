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
import {Checkbox} from '../../../widgets/checkbox';
import type {IntervalIntersectInput} from '../logical_ir';
import type {NodeManifest, RenderContext} from '../node_types';
import {Row} from '../components/row';
import {Stack} from '../components/stack';
import {TextInput} from '../../../widgets/text_input';
import type {ColumnDef} from '../graph_utils';
import type {Port} from '../graph_model';
import {Button, ButtonVariant} from '../../../widgets/button';

const IntervalIntersectConfigSchema = z.object({
  numInputs: z.number(),
  partitionColumns: z.array(z.string()),
  filterNegativeDur: z.boolean(),
});

export type IntervalIntersectConfig = z.infer<
  typeof IntervalIntersectConfigSchema
>;

function IntervalIntersectContent(): m.Component<{
  config: IntervalIntersectConfig;
  updateConfig: (updates: Partial<IntervalIntersectConfig>) => void;
  ctx: RenderContext;
}> {
  return {
    view({attrs: {config, updateConfig}}) {
      const canRemove = config.numInputs > 2;
      return m(Stack, [
        m(Checkbox, {
          label: 'Filter dur >= 0',
          checked: config.filterNegativeDur,
          onchange: () =>
            updateConfig({filterNegativeDur: !config.filterNegativeDur}),
        }),
        m('div', {style: {display: 'flex', gap: '4px'}}, [
          canRemove &&
            m(Button, {
              label: '− Input',
              variant: ButtonVariant.Filled,
              style: {flex: '1'},
              onclick: () =>
                updateConfig({numInputs: Math.max(2, config.numInputs - 1)}),
            }),
          m(Button, {
            label: '+ Input',
            variant: ButtonVariant.Filled,
            style: {flex: '1'},
            onclick: () => updateConfig({numInputs: config.numInputs + 1}),
          }),
        ]),
        m('.pf-spag-section-label', 'Partition by'),
        m(Stack, {compact: true}, [
          ...config.partitionColumns.map((col, i) =>
            m(
              Row,
              {
                key: i,
                draggable: true,
                onReorder: (fromIdx: number, toIdx: number) => {
                  const updated = [...config.partitionColumns];
                  const [moved] = updated.splice(fromIdx, 1);
                  if (moved !== undefined) {
                    updated.splice(toIdx, 0, moved);
                    updateConfig({partitionColumns: updated});
                  }
                },
              },
              [
                m(Row.DragHandle),
                m(TextInput, {
                  value: col,
                  placeholder: 'column',
                  onChange: (value: string) => {
                    const updated = [...config.partitionColumns];
                    updated[i] = value;
                    updateConfig({partitionColumns: updated});
                  },
                }),
                m(Row.DeleteButton, {
                  title: 'Remove',
                  onclick: () => {
                    updateConfig({
                      partitionColumns: config.partitionColumns.filter(
                        (_, j) => j !== i,
                      ),
                    });
                  },
                }),
              ],
            ),
          ),
        ]),
        m(Button, {
          label: 'Column',
          icon: 'add',
          variant: ButtonVariant.Filled,
          onclick: () => {
            updateConfig({
              partitionColumns: [...config.partitionColumns, ''],
            });
          },
        }),
      ]);
    },
  };
}

// Columns intrinsic to _interval_intersect!() output — skip these when
// pulling in source columns to avoid duplicates.
const SKIP_COLS = new Set(['id', 'ts', 'dur']);

interface SourceColumn {
  readonly name: string; // original column name in the source table
  readonly alias: string; // output name (may have suffix for collisions)
  readonly type?: ColumnDef['type'];
}

// Compute extra columns to pull from N inputs, with collision handling.
// Earlier inputs keep original names; later inputs get a _{inputIdx} suffix
// when a name collides with an already-taken name.
function getSourceColumnsForInputs(
  inputCols: (ColumnDef[] | undefined)[],
  numInputs: number,
): SourceColumn[][] {
  const taken = new Set(SKIP_COLS);
  for (let i = 0; i < numInputs; i++) {
    taken.add(`id_${i}`);
  }

  return inputCols.map((cols, inputIdx) => {
    const sourceCols: SourceColumn[] = [];
    for (const col of cols ?? []) {
      if (SKIP_COLS.has(col.name)) continue;
      let alias = col.name;
      if (taken.has(alias)) {
        let suffix = inputIdx;
        alias = `${col.name}_${suffix}`;
        while (taken.has(alias)) {
          suffix++;
          alias = `${col.name}_${suffix}`;
        }
      }
      taken.add(alias);
      sourceCols.push({name: col.name, alias, type: col.type});
    }
    return sourceCols;
  });
}

export const manifest: NodeManifest<IntervalIntersectConfig> = {
  title: 'Interval Intersect',
  icon: 'compare_arrows',
  hue: 340,
  schema: IntervalIntersectConfigSchema,
  getInputs(config: IntervalIntersectConfig): Port[] {
    return Array.from({length: config.numInputs}, (_, i) => ({
      name: `input_${i}`,
      content: `Input ${i + 1}`,
    }));
  },
  defaultConfig: () => ({
    numInputs: 2,
    partitionColumns: [],
    filterNegativeDur: true,
  }),
  isValid: () => true,
  getOutputColumns(_config, ctx) {
    const connectedPorts = ctx.inputPorts.filter(
      (p) => ctx.getInputColumns(p.name) !== undefined,
    );
    if (connectedPorts.length === 0) return undefined;
    const inputCols = connectedPorts.map((p) => ctx.getInputColumns(p.name));
    const sourceCols = getSourceColumnsForInputs(
      inputCols,
      connectedPorts.length,
    );

    const result: ColumnDef[] = [
      {name: 'ts', type: {kind: 'timestamp' as const}},
      {name: 'dur', type: {kind: 'duration' as const}},
    ];
    for (let i = 0; i < connectedPorts.length; i++) {
      const idType = inputCols[i]?.find((c) => c.name === 'id')?.type;
      result.push({name: `id_${i}`, type: idType ?? {kind: 'int' as const}});
    }
    for (const cols of sourceCols) {
      for (const col of cols) {
        result.push({name: col.alias, type: col.type});
      }
    }
    return result;
  },
  toLogicalOp(config, ctx) {
    const connected: Array<{
      readonly portName: string;
      readonly nodeId: string;
    }> = [];
    for (const p of ctx.inputPorts) {
      const nodeId = ctx.getInputNodeId(p.name);
      if (nodeId !== undefined) {
        connected.push({portName: p.name, nodeId});
      }
    }
    if (connected.length === 0) return undefined;
    const inputCols = connected.map((c) => ctx.getInputColumns(c.portName));
    const sourceCols = getSourceColumnsForInputs(inputCols, connected.length);

    const inputs: IntervalIntersectInput[] = connected.map((c, i) => ({
      nodeId: c.nodeId,
      columns: (sourceCols[i] ?? []).map((col) => ({
        name: col.name,
        alias: col.alias,
      })),
    }));

    return {
      kind: 'interval_intersect',
      inputs,
      partitionColumns: config.partitionColumns.filter((c) => c !== ''),
      filterNegativeDur: config.filterNegativeDur,
    };
  },
  render(config, updateConfig, ctx) {
    return m(IntervalIntersectContent, {config, updateConfig, ctx});
  },
};
