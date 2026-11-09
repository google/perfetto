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
import {TextInput} from '../../../widgets/text_input';
import {Stack} from '../components/stack';

const LimitConfigSchema = z
  .object({
    limitCount: z.string().describe('Row limit as digits string, e.g. "100"'),
    offset: z
      .string()
      .optional()
      .describe('Optional offset as digits string or ""'),
  })
  .describe('Cap output row count.');

export type LimitConfig = z.infer<typeof LimitConfigSchema>;

function parseLimit(config: LimitConfig): {count: number; offset: number} {
  const offset =
    config.offset !== undefined && config.offset !== ''
      ? parseInt(config.offset, 10)
      : 0;
  return {count: parseInt(config.limitCount, 10) || 100, offset};
}

function isValid(config: LimitConfig): boolean {
  return (
    config.limitCount !== '' &&
    /^\d+$/.test(config.limitCount) &&
    (config.offset === undefined ||
      config.offset === '' ||
      /^\d+$/.test(config.offset))
  );
}

export const manifest: NodeManifest<LimitConfig> = {
  title: 'Limit',
  icon: 'horizontal_rule',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 57,
  schema: LimitConfigSchema,
  defaultConfig: () => ({limitCount: '100', offset: ''}),
  render(config, updateConfig) {
    return m(Stack, [
      m(TextInput, {
        placeholder: 'Row count...',
        value: config.limitCount,
        onChange: (value: string) => updateConfig({limitCount: value}),
      }),
      m(TextInput, {
        placeholder: 'Offset (optional)...',
        value: config.offset ?? '',
        onChange: (value: string) => updateConfig({offset: value}),
      }),
    ]);
  },
  getOutputColumns: (_config, ctx) => ctx.getInputColumns('input'),
  generators: {
    sql: (config, ctx) => {
      if (ctx.input('input') === undefined) return undefined;
      if (!isValid(config)) return undefined;
      const {count, offset} = parseLimit(config);
      return offset > 0 ? {limit: count, offset} : {limit: count};
    },
    psn: (config, ctx) => {
      if (ctx.input('input') === undefined) return undefined;
      if (!isValid(config)) return undefined;
      const {count, offset} = parseLimit(config);
      return {
        stages: [
          offset > 0 ? `LIMIT ${count} OFFSET ${offset}` : `LIMIT ${count}`,
        ],
      };
    },
  },
};
