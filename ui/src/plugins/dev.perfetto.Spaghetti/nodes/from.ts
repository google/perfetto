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
import type {NodeManifest, RenderContext} from '../node_types';
import {Combobox} from '../../../widgets/combobox';
import './from.scss';

export interface FromConfig {
  readonly table: string;
}

export const manifest: NodeManifest<FromConfig> = {
  title: 'From',
  icon: 'table_chart',
  hue: 210,
  defaultConfig: () => ({table: 'slice'}),
  isValid: (config) => config.table !== '',
  emitIr: (config) => ({sql: `SELECT *\nFROM ${config.table}`}),
  getOutputColumns(config, ctx) {
    if (!config.table || !ctx.sqlModules) return undefined;
    const table = ctx.sqlModules.getTable(config.table);
    return table
      ? table.columns.map((c) => ({name: c.name, type: c.type}))
      : undefined;
  },
  render(
    config: FromConfig,
    updateConfig: (updates: Partial<FromConfig>) => void,
    ctx: RenderContext,
  ): m.Children {
    return m(Combobox, {
      value: config.table,
      suggestions: ctx.tableNames,
      placeholder: 'Pick table...',
      icon: 'table_chart',
      onChange: (value: string) => updateConfig({table: value}),
    });
  },
};
