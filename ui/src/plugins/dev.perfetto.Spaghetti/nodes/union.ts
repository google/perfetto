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
import {Button, ButtonVariant} from '../../../widgets/button';
import {NodeManifest} from '../node_types';
import {ManifestPort} from '../graph_model';

export interface UnionConfig {
  readonly distinct: boolean;
}

export const manifest: NodeManifest<UnionConfig> = {
  title: 'Union',
  icon: 'merge',
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockTop: true,
  canDockBottom: true,
  hue: 242,
  defaultInputs(): ManifestPort[] {
    return [
      {name: 'input_1', content: 'Input 1', direction: 'left'},
      {name: 'input_2', content: 'Input 2', direction: 'left'},
    ];
  },
  defaultConfig: () => ({distinct: false}),
  isValid: () => true,
  getOutputColumns(_config, ctx) {
    for (const port of ctx.inputPorts) {
      const cols = ctx.getInputColumns(port.name);
      if (cols) return cols;
    }
    return undefined;
  },
  render(config, updateConfig, ctx) {
    const n = ctx.inputPorts.length;
    const canRemove = ctx.removeLastInput !== undefined && n > 2;
    return m('.pf-qb-stack', [
      m('div', {style: {display: 'flex', gap: '4px'}}, [
        canRemove &&
          m(Button, {
            label: '- Input',
            variant: ButtonVariant.Filled,
            style: {flex: '1'},
            onclick: ctx.removeLastInput,
          }),
        ctx.addInput &&
          m(Button, {
            label: '+ Input',
            variant: ButtonVariant.Filled,
            style: {flex: '1'},
            onclick: () =>
              ctx.addInput!({
                name: `input_${n + 1}`,
                content: `Input ${n + 1}`,
                direction: 'left',
              }),
          }),
      ]),
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
