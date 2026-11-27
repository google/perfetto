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

// Component harness fixtures for PromptBox. Only bundled into the component
// harness (ui/build --enable-component-harness), never shipped.

import m from 'mithril';
import {
  type FixtureArgs,
  type FixtureModule,
  record,
} from '../../component_harness/registry';
import type {ModelDetails, ModelPath} from '../dev.perfetto.Llm/gateway';
import {PromptBox, type PromptBoxAttrs} from './prompt_box';

const FAKE_MODELS: readonly ModelDetails[] = [
  {
    provider: {
      id: 'gemini-prod',
      label: 'Gemini API',
      protocolName: 'Gemini',
    },
    model: {
      id: 'gemini-2.5-pro',
      label: 'Gemini 2.5 Pro',
      modelName: 'gemini-2.5-pro',
      roles: ['agentic'],
    },
  },
  {
    provider: {
      id: 'gemini-prod',
      label: 'Gemini API',
      protocolName: 'Gemini',
    },
    model: {
      id: 'gemini-2.5-flash',
      label: 'Gemini 2.5 Flash',
      modelName: 'gemini-2.5-flash',
      roles: ['agentic'],
    },
  },
  {
    provider: {
      id: 'local-ollama',
      label: 'Local Ollama',
      protocolName: 'OpenAI',
    },
    model: {
      id: 'qwen-32b',
      modelName: 'qwen2.5-coder:32b',
      roles: ['agentic'],
    },
  },
];

const DEFAULT_MODEL: ModelPath = {
  providerId: 'gemini-prod',
  modelId: 'gemini-2.5-pro',
};

export default {
  // Idle prompt box with a selected model.
  idle: (args) => promptBox(args, {}),

  // Prompt box displaying a token count readout.
  with_tokens: (args) =>
    promptBox(args, {
      totalTokens: 14_320,
    }),

  // Agent running: shows stop button and follow-up placeholder.
  running: (args) =>
    promptBox(args, {
      isAgentRunning: true,
      totalTokens: 8_750,
    }),

  // No agentic models configured: disabled textarea and send button,
  // model selector shows "No model selected" with link to settings.
  no_models: (args) =>
    promptBox(args, {
      models: [],
      selectedModel: undefined,
    }),

  // Models available, but none selected yet.
  no_selection: (args) =>
    promptBox(args, {
      selectedModel: undefined,
    }),
} satisfies FixtureModule;

// Renders PromptBox statically inside a sidebar-width container.
//
// Args:
//   width=<css>  container width (default '420px')
function promptBox(
  args: FixtureArgs,
  overrides: Partial<PromptBoxAttrs>,
): m.Component {
  const attrs: PromptBoxAttrs = {
    isAgentRunning: false,
    totalTokens: undefined,
    models: FAKE_MODELS,
    selectedModel: DEFAULT_MODEL,
    onSelectModel: (model) => record({selectModel: model}),
    onSendMessage: (msg) => record({sendMessage: msg}),
    onCancel: () => record('cancel'),
    ...overrides,
  };

  return {
    view: () =>
      m('div', {style: {width: args.width ?? '420px'}}, m(PromptBox, attrs)),
  };
}
