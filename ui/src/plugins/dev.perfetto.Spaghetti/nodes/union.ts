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
import {Checkbox} from '../../../widgets/checkbox';
import {InputCountButtons} from '../components/add_button';
import {Stack} from '../components/stack';
import type {NodeManifest} from '../node_types';
import type {Port} from '../graph_model';

export interface UnionConfig {
  readonly distinct: boolean;
  readonly numInputs: number;
}

export const manifest: NodeManifest<UnionConfig> = {
  title: 'Union',
  icon: 'merge',
  hue: 242,
  getInputs(config: UnionConfig): Port[] {
    return Array.from({length: config.numInputs}, (_, i) => ({
      name: `input_${i}`,
      content: `Input ${i + 1}`,
    }));
  },
  defaultConfig: () => ({distinct: false, numInputs: 2}),
  isValid: () => true,
  getOutputColumns(_config, ctx) {
    for (const port of ctx.inputPorts) {
      const cols = ctx.getInputColumns(port.name);
      if (cols) return cols;
    }
    return undefined;
  },
  render(config, updateConfig) {
    const n = config.numInputs;
    return m(Stack, [
      m(InputCountButtons, {
        canRemove: n > 2,
        onAdd: () => updateConfig({numInputs: n + 1}),
        onRemove: () => updateConfig({numInputs: n - 1}),
      }),
      m(Checkbox, {
        label: 'Distinct',
        checked: config.distinct,
        onchange: () => updateConfig({distinct: !config.distinct}),
      }),
    ]);
  },
  emitIr(config, ctx) {
    const refs = ctx.inputPorts
      .map((p) => ctx.getInputRef(p.name))
      .filter((r) => r !== '');
    if (refs.length === 0) return undefined;
    const kw = config.distinct ? 'UNION' : 'UNION ALL';
    return {sql: refs.map((r) => `SELECT *\nFROM ${r}`).join(`\n${kw}\n`)};
  },
};
