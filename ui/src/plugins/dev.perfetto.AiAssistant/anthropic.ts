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

import {LlmProvider, SendMessageResult, ToolDef, ToolImpl} from './provider';
import m from 'mithril';

// Content block types from the Anthropic API.
interface TextBlock {
  readonly type: 'text';
  readonly text: string;
}

interface ToolUseBlock {
  readonly type: 'tool_use';
  readonly id: string;
  readonly name: string;
  readonly input: Record<string, unknown>;
}

interface ToolResultBlock {
  readonly type: 'tool_result';
  readonly tool_use_id: string;
  readonly content: string;
}

type ContentBlock = TextBlock | ToolUseBlock;

interface Message {
  readonly role: 'user' | 'assistant';
  readonly content: string | readonly (ContentBlock | ToolResultBlock)[];
}

interface ApiResponse {
  readonly id: string;
  readonly content: readonly ContentBlock[];
  readonly stop_reason: 'end_turn' | 'tool_use' | 'max_tokens' | null;
}

const MAX_TOOL_ROUNDS = 20;

export class AnthropicProvider implements LlmProvider {
  private readonly apiKey: string;
  private readonly model: string;
  private readonly systemPrompt: string;
  private history: Message[] = [];

  constructor(apiKey: string, model: string, systemPrompt: string) {
    this.apiKey = apiKey;
    this.model = model;
    this.systemPrompt = systemPrompt;
  }

  reset(): void {
    this.history = [];
  }

  async sendMessage(opts: {
    userPrompt: string;
    tools: readonly ToolImpl[];
    onText: (text: string) => void;
    onToolUse?: (name: string, input: string) => void;
    signal?: AbortSignal;
  }): Promise<SendMessageResult> {
    this.history.push({role: 'user', content: opts.userPrompt});
    return this.runToolLoop(opts);
  }

  async continueToolUse(opts: {
    tools: readonly ToolImpl[];
    onText: (text: string) => void;
    onToolUse?: (name: string, input: string) => void;
    signal?: AbortSignal;
  }): Promise<SendMessageResult> {
    this.history.push({role: 'user', content: 'Please continue.'});
    return this.runToolLoop(opts);
  }

  private async runToolLoop(opts: {
    tools: readonly ToolImpl[];
    onText: (text: string) => void;
    onToolUse?: (name: string, input: string) => void;
    signal?: AbortSignal;
  }): Promise<SendMessageResult> {
    const {tools, onText, onToolUse, signal} = opts;

    const toolDefs = tools.map((t) => t.def);
    const toolMap = new Map(tools.map((t) => [t.def.name, t]));

    let turnText = '';
    let hitToolLimit = false;

    for (let round = 0; round < MAX_TOOL_ROUNDS; round++) {
      if (signal?.aborted) break;

      const response = await callApi({
        apiKey: this.apiKey,
        model: this.model,
        system: this.systemPrompt,
        messages: this.history,
        tools: toolDefs,
        signal,
      });

      const assistantContent: ContentBlock[] = [];
      for (const block of response.content) {
        assistantContent.push(block);
        if (block.type === 'text') {
          turnText += block.text;
          onText(turnText);
        }
      }

      this.history.push({role: 'assistant', content: assistantContent});

      if (response.stop_reason !== 'tool_use') {
        break;
      }

      // If this is the last round and we're still doing tool use, flag it.
      if (round === MAX_TOOL_ROUNDS - 1) {
        hitToolLimit = true;
        break;
      }

      const toolResults: ToolResultBlock[] = [];
      for (const block of response.content) {
        if (block.type !== 'tool_use') continue;

        const tool = toolMap.get(block.name);
        if (!tool) {
          toolResults.push({
            type: 'tool_result',
            tool_use_id: block.id,
            content: `Error: unknown tool "${block.name}"`,
          });
          continue;
        }

        onToolUse?.(block.name, JSON.stringify(block.input));

        try {
          const result = await tool.handle(block.input);
          toolResults.push({
            type: 'tool_result',
            tool_use_id: block.id,
            content: result,
          });
        } catch (e) {
          toolResults.push({
            type: 'tool_result',
            tool_use_id: block.id,
            content: `Error: ${e}`,
          });
        }
      }

      this.history.push({role: 'user', content: toolResults});
    }

    return {text: turnText, hitToolLimit};
  }
}

export interface AnthropicModel {
  readonly id: string;
  readonly display_name: string;
}

export async function listAnthropicModels(
  apiKey: string,
): Promise<AnthropicModel[]> {
  const response = await fetch('https://api.anthropic.com/v1/models', {
    headers: {
      'x-api-key': apiKey,
      'anthropic-version': '2023-06-01',
      'anthropic-dangerous-direct-browser-access': 'true',
    },
  });
  if (!response.ok) {
    throw new Error(`Anthropic API error ${response.status}`);
  }
  const json = (await response.json()) as {data: AnthropicModel[]};
  m.redraw();
  return json.data;
}

const MAX_RETRIES = 3;

async function callApi(opts: {
  apiKey: string;
  model: string;
  system: string;
  messages: readonly Message[];
  tools: readonly ToolDef[];
  signal?: AbortSignal;
}): Promise<ApiResponse> {
  const {apiKey, model, system, messages, tools, signal} = opts;

  const body = {
    model,
    max_tokens: 4096,
    system,
    messages,
    ...(tools.length > 0 ? {tools} : {}),
  };

  for (let attempt = 0; attempt <= MAX_RETRIES; attempt++) {
    if (signal?.aborted) throw new Error('AbortError');

    const response = await fetch('https://api.anthropic.com/v1/messages', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'x-api-key': apiKey,
        'anthropic-version': '2023-06-01',
        'anthropic-dangerous-direct-browser-access': 'true',
      },
      body: JSON.stringify(body),
      signal,
    });

    if (response.status === 429 && attempt < MAX_RETRIES) {
      const retryAfter = response.headers.get('retry-after');
      const waitMs = retryAfter
        ? parseInt(retryAfter, 10) * 1000
        : (attempt + 1) * 5000;
      await new Promise((r) => setTimeout(r, waitMs));
      continue;
    }

    if (response.status === 529 && attempt < MAX_RETRIES) {
      // Overloaded — back off and retry.
      await new Promise((r) => setTimeout(r, (attempt + 1) * 10000));
      continue;
    }

    if (!response.ok) {
      const text = await response.text();
      throw new Error(`Anthropic API error ${response.status}: ${text}`);
    }

    return (await response.json()) as ApiResponse;
  }

  throw new Error('Anthropic API: max retries exceeded');
}
