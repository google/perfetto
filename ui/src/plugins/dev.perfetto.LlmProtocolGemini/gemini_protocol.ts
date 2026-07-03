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

import {z} from 'zod';
import type {
  AvailableModel,
  CredentialField,
  Message,
  Protocol,
  ProtocolCapabilities,
  Request,
  StreamEvent,
} from '../dev.perfetto.Llm/protocol';
import {zodToGeminiSchema} from './schema_converter';

// Note: v1beta is required for thinking config
const ENDPOINT = 'https://generativelanguage.googleapis.com/v1beta';

interface GeminiTextPart {
  readonly text: string;
  readonly thought?: boolean;
}
interface GeminiFunctionCallPart {
  readonly functionCall: {
    readonly name: string;
    readonly args: Record<string, unknown>;
  };
  // Encrypted reasoning state Gemini attaches to (the first) functionCall part.
  // The API rejects a follow-up request that echoes a function call back
  // without it ("missing a thought_signature"), so we capture it on the way out
  // and replay it verbatim on the way back.
  readonly thoughtSignature?: string;
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

interface GeminiUsage {
  readonly promptTokenCount?: number;
  readonly candidatesTokenCount?: number;
  readonly totalTokenCount?: number;
}
interface GeminiCandidate {
  readonly content?: GeminiContent;
  readonly finishReason?: string;
}
interface GeminiStreamChunk {
  readonly candidates?: readonly GeminiCandidate[];
  readonly usageMetadata?: GeminiUsage;
}

function messagesToContents(messages: readonly Message[]): GeminiContent[] {
  const contents: GeminiContent[] = [];
  for (const msg of messages) {
    switch (msg.role) {
      case 'user':
        contents.push({role: 'user', parts: [{text: msg.text}]});
        break;
      case 'model':
        contents.push({role: 'model', parts: [{text: msg.text}]});
        break;
      case 'tool-call':
        // Gemini expects tool calls back from the model exactly as it emitted
        // them, attributed to the 'model' role - including the thoughtSignature
        // it stamped on the call (carried opaquely on the neutral call).
        contents.push({
          role: 'model',
          parts: msg.calls.map((c) => ({
            functionCall: {name: c.name, args: c.args},
            ...(c.signature ? {thoughtSignature: c.signature} : {}),
          })),
        });
        break;
      case 'tool-result':
        // Gemini threads results by tool *name* (no id), under the 'user' role.
        contents.push({
          role: 'user',
          parts: msg.results.map((r) => ({
            functionResponse: {
              name: r.name,
              response: {result: r.result},
            },
          })),
        });
        break;
    }
  }
  return contents;
}

function toolsToDeclarations(request: Request) {
  if (request.tools.length === 0) return undefined;
  return [
    {
      functionDeclarations: request.tools.map((t) => ({
        name: t.name,
        description: t.description,
        parameters: zodToGeminiSchema(t.inputSchema),
      })),
    },
  ];
}

function classifyError(status: number, body: string): StreamEvent {
  let kind: 'rate-limit' | 'auth' | 'context-length' | 'network' | 'unknown';
  if (status === 429) kind = 'rate-limit';
  else if (status === 401 || status === 403) kind = 'auth';
  else if (status === 400 && /token|context|too long/i.test(body)) {
    kind = 'context-length';
  } else kind = 'unknown';
  return {
    type: 'stop',
    reason: 'error',
    error: {kind, message: `Gemini API error ${status}: ${body}`},
  };
}

// Response of GET /models (only the fields we consume). Unknown keys are
// dropped by zod, so this tolerates the many other fields Gemini returns.
const LIST_MODELS_RESPONSE = z.object({
  models: z.array(z.object({name: z.string().optional()})).optional(),
});

const CAPABILITIES: ProtocolCapabilities = {
  nativeToolCalling: true,
  streaming: true,
};

const CREDENTIAL_FIELDS: readonly CredentialField[] = [
  {
    key: 'apiKey',
    label: 'API key',
    secret: true,
    required: true,
    placeholder: 'AIza...',
  },
  {
    key: 'endpoint',
    label: 'Endpoint (optional)',
    placeholder: ENDPOINT,
  },
];

export class GeminiProtocol implements Protocol {
  readonly id = 'gemini';
  readonly label = 'Google Gemini';
  readonly capabilities = CAPABILITIES;
  readonly credentialFields = CREDENTIAL_FIELDS;

  // GET {base}/models lists every model the key can reach. The names come back
  // prefixed ('models/gemini-2.5-flash'); strip that to the bare model name the
  // generateContent path expects.
  async listModels(
    credentials: Readonly<Record<string, string>>,
    signal?: AbortSignal,
  ): Promise<readonly AvailableModel[]> {
    const apiKey = credentials.apiKey ?? '';
    const base = credentials.endpoint || ENDPOINT;
    const resp = await fetch(`${base}/models?key=${apiKey}&pageSize=1000`, {
      signal,
    });
    if (!resp.ok) {
      const text = await resp.text().catch(() => '<no body>');
      throw new Error(`Gemini listModels error ${resp.status}: ${text}`);
    }
    const json = LIST_MODELS_RESPONSE.parse(await resp.json());
    return (json.models ?? [])
      .map((m) => m.name ?? '')
      .filter((name) => name !== '')
      .map((name) => ({name: name.replace(/^models\//, '')}));
  }

  async *createStream(
    model: string,
    request: Request,
    credentials: Readonly<Record<string, string>>,
    signal?: AbortSignal,
  ): AsyncGenerator<StreamEvent, void, void> {
    const apiKey = credentials.apiKey ?? '';
    const base = credentials.endpoint || ENDPOINT;
    const url = `${base}/models/${model}:streamGenerateContent?alt=sse&key=${apiKey}`;
    const toolDeclarations = toolsToDeclarations(request);

    const body = {
      contents: messagesToContents(request.messages),
      systemInstruction: {parts: [{text: request.systemPrompt}]},
      ...(toolDeclarations ? {tools: toolDeclarations} : {}),
      generationConfig: {
        thinkingConfig: {includeThoughts: true, thinkingBudget: -1},
      },
    };

    let resp: Response;
    try {
      resp = await fetch(url, {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(body),
        signal,
      });
    } catch (e) {
      // Aborts surface as a thrown DOMException; let the consumer see the
      // cancellation rather than dressing it up as an error event.
      if (signal?.aborted) return;
      yield {
        type: 'stop',
        reason: 'error',
        error: {kind: 'network', message: String(e)},
      };
      return;
    }

    if (!resp.ok || !resp.body) {
      const text = await resp.text().catch(() => '<no body>');
      yield classifyError(resp.status, text);
      return;
    }

    let sawToolCall = false;
    let stopReason: 'end' | 'length' = 'end';
    for await (const chunk of parseSse(resp.body, signal)) {
      const candidate = chunk.candidates?.[0];
      for (const part of candidate?.content?.parts ?? []) {
        if ('text' in part) {
          if (part.thought) {
            yield {type: 'thought', text: part.text};
          } else {
            yield {type: 'text', text: part.text};
          }
        } else if ('functionCall' in part) {
          sawToolCall = true;
          yield {
            type: 'tool-call',
            call: {
              name: part.functionCall.name,
              args: part.functionCall.args ?? {},
              // Carry the signature opaquely so the agent can hand it back on
              // the next request (Gemini requires it echoed verbatim).
              signature: part.thoughtSignature,
            },
          };
        }
      }
      if (candidate?.finishReason === 'MAX_TOKENS') stopReason = 'length';
      if (chunk.usageMetadata) {
        yield {
          type: 'usage',
          usage: {
            inputTokens: chunk.usageMetadata.promptTokenCount,
            outputTokens: chunk.usageMetadata.candidatesTokenCount,
            totalTokens: chunk.usageMetadata.totalTokenCount,
          },
        };
      }
    }

    if (signal?.aborted) return;
    yield {
      type: 'stop',
      reason: sawToolCall ? 'tool-calls' : stopReason,
    };
  }
}

// POSTed body comes back as SSE: blank-line-separated events, each one or more
// `data: <json>` lines carrying a GenerateContentResponse fragment.
async function* parseSse(
  body: ReadableStream<Uint8Array>,
  signal?: AbortSignal,
): AsyncGenerator<GeminiStreamChunk, void, void> {
  const reader = body.getReader();
  const decoder = new TextDecoder();
  let buf = '';
  for (;;) {
    if (signal?.aborted) return;
    const {value, done} = await reader.read();
    if (done) break;
    buf += decoder.decode(value, {stream: true});
    buf = buf.replace(/\r\n/g, '\n');
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
