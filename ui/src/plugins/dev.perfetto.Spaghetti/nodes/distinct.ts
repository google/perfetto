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
import type {LogicalOp} from '../logical_ir';
import type {LogicalOpContext, NodeManifest} from '../node_types';
import {Stack} from '../components/stack';

const DistinctConfigSchema = z.object({});

export type DistinctConfig = z.infer<typeof DistinctConfigSchema>;

export const manifest: NodeManifest<DistinctConfig> = {
  title: 'Distinct',
  icon: 'done_all',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 195,
  schema: DistinctConfigSchema,
  defaultConfig: () => ({}),
  isValid: () => true,
  getOutputColumns: (_config, ctx) => ctx.getInputColumns('input'),
  toLogicalOp(
    _config: DistinctConfig,
    ctx: LogicalOpContext,
  ): LogicalOp | undefined {
    const input = ctx.getInputNodeId('input');
    if (!input) return undefined;
    return {
      kind: 'distinct',
      input,
    };
  },
  render() {
    return m(
      Stack,
      m(
        '.pf-spag-passthrough-hint',
        {
          style: {
            opacity: 0.5,
            fontStyle: 'italic',
            fontSize: '11px',
            padding: '2px 4px',
          },
        },
        'Deduplicate identical rows',
      ),
    );
  },
};
