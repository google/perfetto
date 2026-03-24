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
import {NodeManifest} from '../node_types';

export interface UnionConfig {
  readonly distinct: boolean;
}

export const manifest: NodeManifest<UnionConfig> = {
  title: 'Union',
  icon: 'merge',
  inputs: [
    {name: 'input_1', content: 'Input 1', direction: 'left'},
    {name: 'input_2', content: 'Input 2', direction: 'left'},
  ],
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockTop: true,
  canDockBottom: true,
  hue: 242,
  defaultConfig: () => ({distinct: false}),
  isValid: () => true,
  getOutputColumns: (_config, ctx) =>
    ctx.getInputColumns('input_1') ?? ctx.getInputColumns('input_2'),
  render(config, updateConfig) {
    return m(Checkbox, {
      label: 'Distinct',
      checked: config.distinct,
      onchange: () => updateConfig({distinct: !config.distinct}),
    });
  },
  emitIr(config, ctx) {
    const leftRef = ctx.getInputRef('input_1');
    const rightRef = ctx.getInputRef('input_2');
    const kw = config.distinct ? 'UNION' : 'UNION ALL';
    return {
      sql: `SELECT *\nFROM ${leftRef}\n${kw}\nSELECT *\nFROM ${rightRef}`,
    };
  },
};
