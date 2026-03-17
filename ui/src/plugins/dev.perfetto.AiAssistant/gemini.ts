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
import {LlmProvider, SendMessageResult, ToolDef, ToolImpl} from './provider';
import m from 'mithril';

// Gemini API types

interface GeminiTextPart {
  readonly text: string;
}

interface GeminiFunctionCallPart {
  readonly functionCall: {
    readonly name: string;
    readonly args: Record<string, unknown>;
  };
}

interface GeminiFunctionResponsePart {
  readonly functionResponse: {
    readonly name: string;
    readonly response: {
      readonly result: unknown;
    };
  };
}

type GeminiPart =
  | GeminiTextPart
  | GeminiFunctionCallPart
  | GeminiFunctionResponsePart;

interface GeminiContent {
  readonly role: 'user' | 'model';
  readonly parts: readonly GeminiPart[];
}

interface GeminiCandidate {
  readonly content: GeminiContent;
  readonly finishReason: string;
}

interface GeminiResponse {
  readonly candidates: readonly GeminiCandidate[];
}

// Convert our tool definitions to Gemini's function declaration format.
function toGeminiTools(tools: readonly ToolDef[]): {
  functionDeclarations: readonly GeminiFunctionDecl[];
} {
  return {
    functionDeclarations: tools.map((t) => ({
      name: t.name,
      description: t.description,
      parameters: t.input_schema,
    })),
  };
}

interface GeminiFunctionDecl {
  readonly name: string;
  readonly description: string;
  readonly parameters: Record<string, unknown>;
}

export interface GeminiModel {
  readonly name: string;
  readonly displayName: string;
  readonly description: string;
}

export async function listGeminiModels(apiKey: string): Promise<GeminiModel[]> {
  const url = `https://generativelanguage.googleapis.com/v1beta/models?key=${apiKey}`;
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`Gemini API error ${response.status}`);
  }
  const json = (await response.json()) as {models: GeminiModel[]};
  m.redraw();
  return json.models;
}

const MAX_TOOL_ROUNDS = 20;

export class GeminiProvider implements LlmProvider {
  private readonly apiKey: string;
  private readonly model: string;
  private readonly systemPrompt: string;
  private history: GeminiContent[] = [];

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
    this.history.push({
      role: 'user',
      parts: [{text: opts.userPrompt}],
    });
    return this.runToolLoop(opts);
  }

  async continueToolUse(opts: {
    tools: readonly ToolImpl[];
    onText: (text: string) => void;
    onToolUse?: (name: string, input: string) => void;
    signal?: AbortSignal;
  }): Promise<SendMessageResult> {
    this.history.push({
      role: 'user',
      parts: [{text: 'Please continue.'}],
    });
    return this.runToolLoop(opts);
  }

  private async runToolLoop(opts: {
    tools: readonly ToolImpl[];
    onText: (text: string) => void;
    onToolUse?: (name: string, input: string) => void;
    signal?: AbortSignal;
  }): Promise<SendMessageResult> {
    const {tools, onText, onToolUse, signal} = opts;

    const toolMap = new Map(tools.map((t) => [t.def.name, t]));
    const geminiTools = toGeminiTools(tools.map((t) => t.def));

    let turnText = '';
    let hitToolLimit = false;

    for (let round = 0; round < MAX_TOOL_ROUNDS; round++) {
      if (signal?.aborted) break;

      const response = await this.callApi(geminiTools, signal);
      const candidate = maybeUndefined(response.candidates[0]);
      if (!candidate) break;

      const modelParts = candidate.content.parts;
      this.history.push({role: 'model', parts: modelParts});

      // Extract text
      for (const part of modelParts) {
        if ('text' in part) {
          turnText += part.text;
          onText(turnText);
        }
      }

      // Check for function calls
      const functionCalls = modelParts.filter(
        (p): p is GeminiFunctionCallPart => 'functionCall' in p,
      );

      if (functionCalls.length === 0) {
        break;
      }

      if (round === MAX_TOOL_ROUNDS - 1) {
        hitToolLimit = true;
        break;
      }

      // Execute function calls and build responses
      const responseParts: GeminiFunctionResponsePart[] = [];
      for (const fc of functionCalls) {
        const {name, args} = fc.functionCall;
        const tool = toolMap.get(name);

        onToolUse?.(name, JSON.stringify(args));

        let result: string;
        if (!tool) {
          result = `Error: unknown tool "${name}"`;
        } else {
          try {
            result = await tool.handle(args);
          } catch (e) {
            result = `Error: ${e}`;
          }
        }

        responseParts.push({
          functionResponse: {
            name,
            response: {result},
          },
        });
      }

      this.history.push({role: 'user', parts: responseParts});
    }

    return {text: turnText, hitToolLimit};
  }

  private async callApi(
    tools: {functionDeclarations: readonly GeminiFunctionDecl[]},
    signal?: AbortSignal,
  ): Promise<GeminiResponse> {
    const url = `https://generativelanguage.googleapis.com/v1beta/models/${this.model}:generateContent?key=${this.apiKey}`;

    const body = {
      contents: this.history,
      systemInstruction: {
        parts: [{text: this.systemPrompt}],
      },
      tools: [tools],
    };

    const response = await fetch(url, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(body),
      signal,
    });

    if (!response.ok) {
      const text = await response.text();
      throw new Error(`Gemini API error ${response.status}: ${text}`);
    }

    return (await response.json()) as GeminiResponse;
  }
}
