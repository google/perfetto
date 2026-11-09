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
import type {NodeManifest} from '../node_types';
import {Stack} from '../components/stack';

const DistinctConfigSchema = z
  .object({})
  .describe('Deduplicate identical rows.');

export type DistinctConfig = z.infer<typeof DistinctConfigSchema>;

export const manifest: NodeManifest<DistinctConfig> = {
  title: 'Distinct',
  icon: 'done_all',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 31,
  schema: DistinctConfigSchema,
  defaultConfig: () => ({}),
  getOutputColumns: (_config, ctx) => ctx.getInputColumns('input'),
  generators: {
    sql: (_config, ctx) => {
      if (ctx.input('input') === undefined) return undefined;
      return {distinct: true};
    },
    psn: (_config, ctx) => {
      if (ctx.input('input') === undefined) return undefined;
      return {stages: ['DISTINCT']};
    },
  },
  render() {
    return m(
      Stack,
      m('.pf-spag-passthrough-hint', 'Deduplicate identical rows'),
    );
  },
};
