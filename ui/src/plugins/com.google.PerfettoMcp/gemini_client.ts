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

// Minimal Gemini REST client. Replaces @google/genai. Speaks the streaming
// generateContent endpoint, drives a function-calling loop against a local
// ToolRegistry, and emits incremental events the chat UI consumes.

import {type ToolRegistry, zodToGeminiSchema} from './tool_registry';

// --- Gemini wire types (subset of what generateContent returns) ---

interface GeminiTextPart {
  readonly text: string;
  readonly thought?: boolean;
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
    readonly response: {readonly result: unknown};
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

export interface GeminiUsage {
  readonly promptTokenCount?: number;
  readonly candidatesTokenCount?: number;
  readonly totalTokenCount?: number;
  readonly thoughtsTokenCount?: number;
}

interface GeminiCandidate {
  readonly content: GeminiContent;
  readonly finishReason?: string;
}

interface GeminiStreamChunk {
  readonly candidates?: readonly GeminiCandidate[];
  readonly usageMetadata?: GeminiUsage;
}

// --- Events surfaced to the UI ---

export type ChatEvent =
  | {readonly type: 'text'; readonly text: string}
  | {readonly type: 'thought'; readonly text: string}
  | {readonly type: 'toolcall'; readonly name: string; readonly args: unknown}
  | {
      readonly type: 'toolresult';
      readonly name: string;
      readonly error?: string;
    }
  | {readonly type: 'usage'; readonly usage: GeminiUsage};

const MAX_TOOL_ROUNDS = 20;
const ENDPOINT = 'https://generativelanguage.googleapis.com/v1beta';

export interface GeminiChatOpts {
  readonly apiKey: string;
  readonly model: string;
  readonly systemPrompt: string;
  readonly tools: ToolRegistry;
}

export class GeminiChat {
  private readonly apiKey: string;
  private readonly model: string;
  private readonly systemPrompt: string;
  private readonly tools: ToolRegistry;
  private history: GeminiContent[] = [];

  constructor(opts: GeminiChatOpts) {
    this.apiKey = opts.apiKey;
    this.model = opts.model;
    this.systemPrompt = opts.systemPrompt;
    this.tools = opts.tools;
  }

  reset(): void {
    this.history = [];
  }

  // Sends one user message and streams events back until the model produces
  // a turn with no function calls (or the tool-round cap is hit).
  async *sendMessage(
    userPrompt: string,
    signal?: AbortSignal,
  ): AsyncGenerator<ChatEvent, void, void> {
    this.history.push({role: 'user', parts: [{text: userPrompt}]});

    const toolDeclarations = this.tools.list().map((t) => ({
      name: t.name,
      description: t.description,
      parameters: zodToGeminiSchema(t.schema),
    }));
    const requestTools =
      toolDeclarations.length > 0
        ? [{functionDeclarations: toolDeclarations}]
        : undefined;

    for (let round = 0; round < MAX_TOOL_ROUNDS; round++) {
      if (signal?.aborted) return;

      const turnParts: GeminiPart[] = [];

      // Drain one streamed turn. Yield events as they arrive.
      for await (const chunk of this.stream(requestTools, signal)) {
        const candidate = chunk.candidates?.[0];
        if (candidate) {
          for (const part of candidate.content.parts) {
            turnParts.push(part);
            if ('text' in part) {
              if (part.thought) {
                yield {type: 'thought', text: part.text};
              } else {
                yield {type: 'text', text: part.text};
              }
            } else if ('functionCall' in part) {
              yield {
                type: 'toolcall',
                name: part.functionCall.name,
                args: part.functionCall.args,
              };
            }
          }
        }
        if (chunk.usageMetadata) {
          yield {type: 'usage', usage: chunk.usageMetadata};
        }
      }

      if (turnParts.length === 0) return;
      this.history.push({role: 'model', parts: turnParts});

      const functionCalls = turnParts.filter(
        (p): p is GeminiFunctionCallPart => 'functionCall' in p,
      );
      if (functionCalls.length === 0) return;

      const responseParts: GeminiFunctionResponsePart[] = [];
      for (const fc of functionCalls) {
        const {name, args} = fc.functionCall;
        let result: string;
        let errorMsg: string | undefined;
        try {
          const r = await this.tools.call(name, args);
          result = r.content.map((c) => c.text).join('\n');
        } catch (e) {
          errorMsg = String(e);
          result = `Error: ${errorMsg}`;
        }
        yield {type: 'toolresult', name, error: errorMsg};
        responseParts.push({
          functionResponse: {name, response: {result}},
        });
      }
      this.history.push({role: 'user', parts: responseParts});
    }
  }

  // POSTs to streamGenerateContent and yields parsed JSON chunks as the SSE
  // stream comes in. Each chunk is one GenerateContentResponse fragment.
  private async *stream(
    tools: ReadonlyArray<{functionDeclarations: unknown}> | undefined,
    signal?: AbortSignal,
  ): AsyncGenerator<GeminiStreamChunk, void, void> {
    const url = `${ENDPOINT}/models/${this.model}:streamGenerateContent?alt=sse&key=${this.apiKey}`;
    const body = {
      contents: this.history,
      systemInstruction: {parts: [{text: this.systemPrompt}]},
      ...(tools ? {tools} : {}),
      generationConfig: {
        thinkingConfig: {
          includeThoughts: true,
          thinkingBudget: -1,
        },
      },
    };

    const resp = await fetch(url, {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(body),
      signal,
    });
    if (!resp.ok || !resp.body) {
      const text = await resp.text().catch(() => '<no body>');
      throw new Error(`Gemini API error ${resp.status}: ${text}`);
    }

    const reader = resp.body.getReader();
    const decoder = new TextDecoder();
    let buf = '';
    for (;;) {
      const {value, done} = await reader.read();
      if (done) break;
      buf += decoder.decode(value, {stream: true});
      // Normalize CRLF so both \n\n and \r\n\r\n separators work.
      buf = buf.replace(/\r\n/g, '\n');
      // SSE messages are separated by blank lines. Each message is one or more
      // "data: <payload>" lines; payload is a JSON GenerateContentResponse.
      let sep: number;
      while ((sep = buf.indexOf('\n\n')) !== -1) {
        const event = buf.slice(0, sep);
        buf = buf.slice(sep + 2);
        const payload = event
          .split('\n')
          .filter((l) => l.startsWith('data:'))
          .map((l) => l.slice(5).trimStart())
          .join('');
        if (!payload || payload === '[DONE]') continue;
        try {
          yield JSON.parse(payload) as GeminiStreamChunk;
        } catch {
          // Ignore unparseable events (keepalives, partial buffers).
        }
      }
    }
  }
}
