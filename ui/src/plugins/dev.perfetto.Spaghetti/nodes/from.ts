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
import type {NodeManifest} from '../node_types';
import type {SqlModules} from '../../dev.perfetto.SqlModules/sql_modules';
import {Combobox} from '../../../widgets/combobox';
import './from.scss';

const FromConfigSchema = z
  .object({
    table: z.string(),
  })
  .describe(
    'Read a trace or stdlib table (stdlib module includes are added automatically).',
  );

export type FromConfig = z.infer<typeof FromConfigSchema>;

function includesForTable(
  config: FromConfig,
  ctx: {readonly sqlModules: SqlModules | undefined},
): string[] | undefined {
  const mod = ctx.sqlModules?.getModuleForTable(config.table);
  return mod && !mod.includeKey.startsWith('prelude')
    ? [mod.includeKey]
    : undefined;
}

export const manifest: NodeManifest<FromConfig> = {
  title: 'From',
  icon: 'table_chart',
  hue: 204,
  schema: FromConfigSchema,
  defaultConfig: () => ({table: 'slice'}),
  render(config, updateConfig, ctx) {
    return m(Combobox, {
      value: config.table,
      suggestions: ctx.tableNames,
      placeholder: 'Pick table...',
      icon: 'table_chart',
      onChange: (value: string) => updateConfig({table: value}),
    });
  },
  getOutputColumns(config, ctx) {
    if (!config.table || !ctx.sqlModules) return undefined;
    const table = ctx.sqlModules.getTable(config.table);
    return table
      ? table.columns.map((c) => ({name: c.name, type: c.type}))
      : undefined;
  },
  generators: {
    // A bare-name `from` renders as a plain reference (no CTE / wrapper).
    sql: (config, ctx) => {
      if (!config.table) return undefined;
      return {from: config.table, includes: includesForTable(config, ctx)};
    },
    psn: (config, ctx) => {
      if (!config.table) return undefined;
      return {
        source: `FROM ${config.table}`,
        includes: includesForTable(config, ctx),
      };
    },
  },
};
