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
import {NodeManifest, SqlStatement} from '../node_types';
import {TextInput} from '../../../widgets/text_input';

export interface LimitConfig {
  readonly limitCount: string;
}

export const manifest: NodeManifest<LimitConfig> = {
  title: 'Limit',
  icon: 'horizontal_rule',
  inputs: [{name: 'input', content: 'Input', direction: 'left'}],
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockTop: true,
  canDockBottom: true,
  hue: 60,
  defaultConfig: () => ({limitCount: '100'}),
  isValid: (config) =>
    config.limitCount !== '' && /^\d+$/.test(config.limitCount),
  getOutputColumns: (_config, ctx) => ctx.getInputColumns('input'),
  tryFold(stmt: SqlStatement, config: LimitConfig) {
    if (stmt.limit !== undefined) return false;
    stmt.limit = parseInt(config.limitCount) || 100;
    return true;
  },
  render(config, updateConfig) {
    return m(
      '.pf-qb-stack',
      m(TextInput, {
        placeholder: 'Row count...',
        value: config.limitCount,
        onChange: (value: string) => updateConfig({limitCount: value}),
      }),
    );
  },
};
