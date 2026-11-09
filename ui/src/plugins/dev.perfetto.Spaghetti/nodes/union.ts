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
import {Checkbox} from '../../../widgets/checkbox';
import {InputPortStepper} from '../components/input_port_stepper';
import {Stack} from '../components/stack';
import type {NodeManifest} from '../node_types';
import type {Port} from '../graph_model';
import {subquery} from '../generator_utils';

const UnionConfigSchema = z
  .object({
    distinct: z.boolean(),
    numInputs: z.number(),
  })
  .describe(
    'Combine rows from >= 2 inputs (UNION ALL when distinct is false, UNION when true).',
  );

export type UnionConfig = z.infer<typeof UnionConfigSchema>;

export const manifest: NodeManifest<UnionConfig> = {
  title: 'Union',
  icon: 'merge',
  hue: 298,
  expensive: true,
  schema: UnionConfigSchema,
  getInputs(config: UnionConfig): Port[] {
    return Array.from({length: config.numInputs}, (_, i) => ({
      name: `input_${i}`,
      content: `Input ${i + 1}`,
    }));
  },
  defaultConfig: () => ({distinct: false, numInputs: 2}),
  getOutputColumns(_config, ctx) {
    for (const port of ctx.inputPorts) {
      const cols = ctx.getInputColumns(port.name);
      if (cols) return cols;
    }
    return undefined;
  },
  render(config, updateConfig) {
    const n = config.numInputs;
    const canRemove = n > 2;
    return m(Stack, [
      m(InputPortStepper, {
        canRemove,
        onRemove: () => updateConfig({numInputs: n - 1}),
        onAdd: () => updateConfig({numInputs: n + 1}),
      }),
      m(Checkbox, {
        label: 'Distinct',
        checked: config.distinct,
        onchange: () => updateConfig({distinct: !config.distinct}),
      }),
    ]);
  },
  generators: {
    sql: (config, ctx) => {
      const inputs = ctx.inputs();
      const first = inputs[0];
      if (first === undefined) return undefined;
      if (inputs.length === 1) return {};
      const kw = config.distinct ? 'UNION' : 'UNION ALL';
      return {
        from: subquery(
          inputs.map((i) => `SELECT * FROM ${i.input.ref}`).join(`\n${kw}\n`),
        ),
      };
    },
    // Pipe-native union syntax (not yet supported by TP):
    //   FROM <a>
    //   |> UNION ALL (<b>)
    psn: (config, ctx) => {
      const inputs = ctx.inputs();
      if (inputs[0] === undefined) return undefined;
      if (inputs.length === 1) return {};
      const kw = config.distinct ? 'UNION' : 'UNION ALL';
      return {
        stages: inputs.slice(1).map((i) => {
          const src = i.input.source;
          return `${kw} ${src.startsWith('(') ? src : `(FROM ${src})`}`;
        }),
      };
    },
  },
};
