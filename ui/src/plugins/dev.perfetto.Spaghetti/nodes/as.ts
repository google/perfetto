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
import {TextInput} from '../../../widgets/text_input';
import {Stack} from '../components/stack';
import type {LogicalOp} from '../logical_ir';
import type {
  LogicalOpContext,
  NodeManifest,
  RenderContext,
} from '../node_types';

const AsConfigSchema = z.object({
  alias: z.string(),
});

export type AsConfig = z.infer<typeof AsConfigSchema>;

const IDENT_RE = /^[a-zA-Z_][a-zA-Z0-9_]*$/;

function isValid(config: AsConfig): boolean {
  return IDENT_RE.test(config.alias.trim());
}

function render(
  config: AsConfig,
  updateConfig: (updates: Partial<AsConfig>) => void,
  _ctx: RenderContext,
): m.Children {
  return m(Stack, [
    m('.pf-spag-section-label', 'Row alias'),
    m(TextInput, {
      value: config.alias,
      placeholder: 'alias',
      oninput: (e: InputEvent) =>
        updateConfig({alias: (e.target as HTMLInputElement).value}),
    }),
  ]);
}

function toLogicalOp(
  config: AsConfig,
  ctx: LogicalOpContext,
): LogicalOp | undefined {
  const input = ctx.getInputNodeId('input');
  if (input === undefined || !isValid(config)) return undefined;
  return {
    kind: 'as',
    input,
    alias: config.alias.trim(),
  };
}

export const manifest: NodeManifest<AsConfig> = {
  title: 'As',
  icon: 'label',
  getInputs: () => [{name: 'input', content: 'Input'}],
  hue: 168,
  schema: AsConfigSchema,
  defaultConfig: () => ({alias: ''}),
  isValid,
  getOutputColumns: (_config, ctx) => ctx.getInputColumns('input'),
  toLogicalOp,
  render,
};
