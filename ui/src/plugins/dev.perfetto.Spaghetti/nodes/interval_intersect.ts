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
import {Checkbox} from '../../../widgets/checkbox';
import type {
  GeneratorContext,
  NodeManifest,
  RenderContext,
} from '../node_types';
import {indentTail, perClause} from '../generator_utils';
import {DraggableStack} from '../components/draggable_stack';
import {InputPortStepper} from '../components/input_port_stepper';
import {Stack} from '../components/stack';
import {TextInput} from '../../../widgets/text_input';
import type {ColumnDef} from '../graph_utils';
import type {Port} from '../graph_model';
import {Button, ButtonVariant} from '../../../widgets/button';

const IntervalIntersectConfigSchema = z
  .object({
    numInputs: z.number(),
    partitionColumns: z.array(z.string()),
    filterNegativeDur: z.boolean(),
  })
  .describe(
    'Intersect overlapping (id, ts, dur) intervals across >= 2 inputs.',
  );

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
        m(InputPortStepper, {
          canRemove,
          onRemove: () =>
            updateConfig({numInputs: Math.max(2, config.numInputs - 1)}),
          onAdd: () => updateConfig({numInputs: config.numInputs + 1}),
        }),
        m('.pf-spag-section-label', 'Partition by'),
        m(DraggableStack, {
          rows: config.partitionColumns.map((col, i) =>
            m(TextInput, {
              value: col,
              placeholder: 'column',
              onChange: (value) =>
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

/** Non-interval columns picked from each operand `tN`, de-duplicated. */
function pickedColumns<I>(ctx: GeneratorContext<I>): string[] {
  const inputs = ctx.inputs();
  const sourceCols = getSourceColumnsForInputs(
    inputs.map((i) => ctx.getInputColumns(i.port)),
    inputs.length,
  );
  return sourceCols.flatMap((cols, i) =>
    cols.map((col) =>
      col.alias !== col.name
        ? `t${i + 1}.${col.name} AS ${col.alias}`
        : `t${i + 1}.${col.name}`,
    ),
  );
}

export const manifest: NodeManifest<IntervalIntersectConfig> = {
  title: 'Interval Intersect',
  icon: 'compare_arrows',
  hue: 312,
  expensive: true,
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
  generators: {
    // `_interval_intersect!` yields (ts, dur, id_0..id_n); picked input
    // columns are joined back on from each operand by id.
    sql: (config, ctx) => {
      const inputs = ctx.inputs();
      if (inputs.length === 0) return undefined;
      const negDur = config.filterNegativeDur;
      if (inputs.length === 1) return negDur ? {where: 'dur >= 0'} : {};

      const operands = inputs.map((i) =>
        negDur ? `(SELECT * FROM ${i.input.ref} WHERE dur >= 0)` : i.input.ref,
      );
      const partition = config.partitionColumns.filter((c) => c !== '');
      const macro = `_interval_intersect!(\n  (${operands.join(',\n   ')}),\n  (${partition.join(', ')})\n)`;
      const picked = pickedColumns(ctx);
      if (picked.length === 0) {
        return {from: macro, includes: ['intervals.intersect']};
      }
      return {
        includes: ['intervals.intersect'],
        from: [
          `${macro} ii`,
          ...inputs.map(
            (inp, i) =>
              `JOIN ${inp.input.ref} t${i + 1} ON ii.id_${i} = t${i + 1}.id`,
          ),
        ].join('\n'),
        select: [
          'ii.ts',
          'ii.dur',
          ...inputs.map((_, i) => `ii.id_${i}`),
          ...picked,
        ],
      };
    },
    // Native INTERVAL INTERSECTION OF; the SELECT stage dedupes the operands'
    // id / ts / dur columns into id_0..id_n and applies picked columns.
    psn: (config, ctx) => {
      const inputs = ctx.inputs();
      if (inputs.length === 0) return undefined;
      const negDur = config.filterNegativeDur ? ['WHERE dur >= 0'] : [];
      if (inputs.length === 1) return {stages: negDur};

      const operands = inputs.map(
        (i, idx) =>
          `${indentTail(i.input.withStages(negDur), '  ')} AS t${idx + 1}`,
      );
      const partition = config.partitionColumns.filter((c) => c !== '');
      return {
        source: `INTERVAL INTERSECTION OF (\n  ${operands.join(',\n  ')}\n)${perClause(partition)}`,
        stages: [
          `SELECT ${[
            'ts',
            'dur',
            ...inputs.map((_, i) => `t${i + 1}.id AS id_${i}`),
            ...pickedColumns(ctx),
          ].join(', ')}`,
        ],
      };
    },
  },
  render(config, updateConfig, ctx) {
    return m(IntervalIntersectContent, {config, updateConfig, ctx});
  },
};
