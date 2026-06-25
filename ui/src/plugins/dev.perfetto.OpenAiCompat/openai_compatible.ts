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

import {maybeUndefined} from '../../base/utils';
import type {
  LlmProvider,
  ModelInfo,
  Protocol,
  ProviderConfig,
  SendMessageResult,
  ToolImpl,
} from '../dev.perfetto.Intelletto/provider';

interface ChatMessage {
  role: 'system' | 'user' | 'assistant' | 'tool';
  content?: string | null;
  tool_calls?: ReadonlyArray<{
    id: string;
    type: 'function';
    function: {name: string; arguments: string};
  }>;
  tool_call_id?: string;
  name?: string;
}

interface ChatChoice {
  message: {
    role: 'assistant';
    content: string | null;
    tool_calls?: ReadonlyArray<{
      id: string;
      type: 'function';
      function: {name: string; arguments: string};
    }>;
  };
  finish_reason: string;
}

interface ChatResponse {
  choices: ReadonlyArray<ChatChoice>;
}

const MAX_TOOL_ROUNDS = 20;

class OpenAiCompletionsProvider implements LlmProvider {
  private readonly apiKey: string;
  private readonly model: string;
  private readonly baseUrl: string;
  private history: ChatMessage[] = [];

  constructor(
    apiKey: string,
    model: string,
    systemPrompt: string,
    baseUrl: string,
  ) {
    this.apiKey = apiKey;
    this.model = model;
    this.baseUrl = baseUrl;
    this.history.push({role: 'system', content: systemPrompt});
  }

  reset(): void {
    const system = this.history[0];
    this.history = system && system.role === 'system' ? [system] : [];
  }

  async sendMessage(opts: {
    userPrompt: string;
    tools: readonly ToolImpl[];
    onText: (text: string) => void;
    onToolUse?: (name: string, input: string) => void;
    onToolResult?: (name: string, error?: string) => void;
    signal?: AbortSignal;
  }): Promise<SendMessageResult> {
    this.history.push({role: 'user', content: opts.userPrompt});
    return this.runToolLoop(opts);
  }

  async continueToolUse(opts: {
    tools: readonly ToolImpl[];
    onText: (text: string) => void;
    onToolUse?: (name: string, input: string) => void;
    onToolResult?: (name: string, error?: string) => void;
    signal?: AbortSignal;
  }): Promise<SendMessageResult> {
    this.history.push({role: 'user', content: 'Please continue.'});
    return this.runToolLoop(opts);
  }

  private async runToolLoop(opts: {
    tools: readonly ToolImpl[];
    onText: (text: string) => void;
    onToolUse?: (name: string, input: string) => void;
    onToolResult?: (name: string, error?: string) => void;
    signal?: AbortSignal;
  }): Promise<SendMessageResult> {
    const {tools, onText, onToolUse, onToolResult, signal} = opts;
    const toolMap = new Map(tools.map((t) => [t.def.name, t]));
    const apiTools = tools.map((t) => ({
      type: 'function' as const,
      function: {
        name: t.def.name,
        description: t.def.description,
        parameters: t.def.input_schema,
      },
    }));

    let turnText = '';
    let hitToolLimit = false;

    for (let round = 0; round < MAX_TOOL_ROUNDS; round++) {
      if (signal?.aborted) break;

      const response = await fetch(`${this.baseUrl}/chat/completions`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          ...(this.apiKey ? {Authorization: `Bearer ${this.apiKey}`} : {}),
        },
        body: JSON.stringify({
          model: this.model,
          messages: this.history,
          ...(apiTools.length > 0 ? {tools: apiTools} : {}),
        }),
        signal,
      });
      if (!response.ok) {
        const text = await response.text();
        throw new Error(`OpenAI-compat API error ${response.status}: ${text}`);
      }
      const json = (await response.json()) as ChatResponse;
      const choice = maybeUndefined(json.choices[0]);
      if (!choice) break;

      const msg = choice.message;
      if (msg.content) {
        turnText += msg.content;
        onText(turnText);
      }
      this.history.push({
        role: 'assistant',
        content: msg.content ?? null,
        tool_calls: msg.tool_calls,
      });

      const toolCalls = msg.tool_calls ?? [];
      if (toolCalls.length === 0) break;

      if (round === MAX_TOOL_ROUNDS - 1) {
        hitToolLimit = true;
        break;
      }

      for (const tc of toolCalls) {
        const name = tc.function.name;
        const args = tc.function.arguments;
        onToolUse?.(name, args);
        const tool = toolMap.get(name);
        let result: string;
        let errorMsg: string | undefined;
        if (!tool) {
          errorMsg = `Unknown tool "${name}"`;
          result = `Error: ${errorMsg}`;
        } else {
          try {
            const parsed = args ? JSON.parse(args) : {};
            result = await tool.handle(parsed);
          } catch (e) {
            errorMsg = String(e);
            result = `Error: ${errorMsg}`;
          }
        }
        onToolResult?.(name, errorMsg);
        this.history.push({
          role: 'tool',
          tool_call_id: tc.id,
          name,
          content: result,
        });
      }
    }

    return {text: turnText, hitToolLimit};
  }
}

export const openAiCompatibleProtocol: Protocol = {
  name: 'openai-compatible',
  createProvider(cfg: ProviderConfig, model: string, systemPrompt: string) {
    if (!cfg.baseUrl) {
      throw new Error('openai-compatible protocol requires a baseUrl');
    }
    return new OpenAiCompletionsProvider(
      cfg.apiKey,
      model,
      systemPrompt,
      cfg.baseUrl,
    );
  },
  async listModels(cfg: ProviderConfig): Promise<ReadonlyArray<ModelInfo>> {
    if (!cfg.baseUrl) {
      throw new Error('openai-compatible protocol requires a baseUrl');
    }
    const response = await fetch(`${cfg.baseUrl}/models`, {
      headers: cfg.apiKey ? {Authorization: `Bearer ${cfg.apiKey}`} : {},
    });
    if (!response.ok) {
      throw new Error(`Model list error ${response.status}`);
    }
    const json = (await response.json()) as {
      data?: ReadonlyArray<{id: string}>;
    };
    return (json.data ?? []).map((m) => ({id: m.id}));
  },
};
