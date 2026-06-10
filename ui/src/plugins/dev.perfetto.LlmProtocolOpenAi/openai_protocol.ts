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

// An OpenAI-compatible Protocol: speaks the /v1/chat/completions wire format
// with SSE streaming and OpenAI-style function calling. This one protocol backs
// any server that implements that API - OpenAI itself, llama.cpp's
// llama-server (run with --jinja for tool calling), vLLM, LM Studio, Ollama's
// OpenAI endpoint, etc. - the only difference between them is the base URL and
// whether an API key is required (local servers usually run without one).

import type {
  AvailableModel,
  CredentialField,
  NeutralMessage,
  NeutralRequest,
  NeutralToolCall,
  Protocol,
  ProtocolCapabilities,
  StreamEvent,
} from '../dev.perfetto.Llm/protocol';
import {zodToJsonSchema} from './json_schema';

const DEFAULT_BASE_URL = 'http://localhost:8080/v1';

// Temporary: dump raw SSE payloads and assembled tool calls to the console to
// diagnose how a given OpenAI-compatible server (e.g. llama-server) shapes its
// tool-call stream. Flip off once the wire format is understood.
const DEBUG_SSE = true;

// --- OpenAI chat-completions wire types (the subset we use) ------------------

interface OpenAiToolCall {
  readonly id?: string;
  readonly type?: 'function';
  readonly function: {readonly name?: string; readonly arguments?: string};
}

interface OpenAiMessage {
  readonly role: 'system' | 'user' | 'assistant' | 'tool';
  readonly content?: string | null;
  readonly tool_calls?: ReadonlyArray<OpenAiToolCall>;
  readonly tool_call_id?: string;
}

// A tool-call fragment as it appears in a streamed delta. Per the OpenAI spec
// these arrive piecemeal keyed by `index`, with `arguments` a JSON *string*
// accumulated across chunks. But OpenAI-compatible servers vary:
//   - llama.cpp / some builds omit `index` (single call, sent whole),
//   - some emit `arguments` as an already-parsed object rather than a string.
// The assembler tolerates all of these.
interface ToolCallFragment {
  readonly index?: number;
  readonly id?: string;
  readonly function?: {
    readonly name?: string;
    readonly arguments?: string | Record<string, unknown>;
  };
}

interface StreamDelta {
  readonly role?: string;
  readonly content?: string | null;
  readonly tool_calls?: ReadonlyArray<ToolCallFragment>;
}

interface StreamChunk {
  readonly choices?: ReadonlyArray<{
    readonly delta?: StreamDelta;
    // Non-streaming-style servers may return the whole turn under `message`
    // instead of incremental `delta`s; accept that too.
    readonly message?: StreamDelta;
    readonly finish_reason?: string | null;
  }>;
  readonly usage?: {
    readonly prompt_tokens?: number;
    readonly completion_tokens?: number;
    readonly total_tokens?: number;
  } | null;
}

// --- Translation: neutral -> OpenAI ------------------------------------------

function messagesToOpenAi(
  systemPrompt: string,
  messages: ReadonlyArray<NeutralMessage>,
): OpenAiMessage[] {
  const out: OpenAiMessage[] = [{role: 'system', content: systemPrompt}];
  for (const msg of messages) {
    switch (msg.role) {
      case 'user':
        out.push({role: 'user', content: msg.text});
        break;
      case 'model':
        out.push({role: 'assistant', content: msg.text});
        break;
      case 'tool-call':
        // The assistant turn that requested the tools. OpenAI threads results
        // back by tool_call_id, so we must surface an id even though Gemini
        // doesn't need one - synthesise a stable one from name+index if the
        // backend didn't give us one when it streamed the call.
        out.push({
          role: 'assistant',
          content: null,
          tool_calls: msg.calls.map((c, i) => ({
            id: c.id ?? `call_${i}_${c.name}`,
            type: 'function',
            function: {name: c.name, arguments: JSON.stringify(c.args)},
          })),
        });
        break;
      case 'tool-result':
        // One `tool` message per result, keyed to its originating call id.
        for (const [i, r] of msg.results.entries()) {
          out.push({
            role: 'tool',
            tool_call_id: r.id ?? `call_${i}_${r.name}`,
            content: r.result,
          });
        }
        break;
    }
  }
  return out;
}

function toolsToOpenAi(request: NeutralRequest) {
  if (request.tools.length === 0) return undefined;
  return request.tools.map((t) => ({
    type: 'function' as const,
    function: {
      name: t.name,
      description: t.description,
      parameters: zodToJsonSchema(t.inputSchema),
    },
  }));
}

// --- Error normalisation -----------------------------------------------------

function classifyError(status: number, body: string): StreamEvent {
  let kind: 'rate-limit' | 'auth' | 'context-length' | 'network' | 'unknown';
  if (status === 429) kind = 'rate-limit';
  else if (status === 401 || status === 403) kind = 'auth';
  else if (status === 400 && /context|maximum.*token|too long/i.test(body))
    {kind = 'context-length';}
  else kind = 'unknown';
  return {
    type: 'stop',
    reason: 'error',
    error: {kind, message: `OpenAI-compatible API error ${status}: ${body}`},
  };
}

// --- The protocol ------------------------------------------------------------

const CAPABILITIES: ProtocolCapabilities = {
  nativeToolCalling: true,
  streaming: true,
  vision: false,
};

const CREDENTIAL_FIELDS: ReadonlyArray<CredentialField> = [
  {
    key: 'endpoint',
    label: 'Base URL',
    required: true,
    placeholder: DEFAULT_BASE_URL,
  },
  {
    // Optional: local servers (llama-server, LM Studio, Ollama) usually run
    // without auth. Sent as a bearer token when present.
    key: 'apiKey',
    label: 'API key (optional for local servers)',
    secret: true,
  },
];

// Accumulates streamed tool-call fragments into whole calls. Fragments are
// grouped by `index` where present; servers that omit it (sending a call whole)
// fall back to a positional key so multiple distinct calls don't collide. The
// `arguments` field is concatenated as a string, but if a server hands us an
// already-parsed object we stash it directly rather than string-concatenating
// (which would otherwise produce "[object Object]" and lose all the args).
class ToolCallAssembler {
  private readonly byKey = new Map<
    string,
    {
      id?: string;
      name: string;
      argsStr: string;
      argsObj?: Record<string, unknown>;
    }
  >();
  private fallbackSeq = 0;

  add(fragments: ReadonlyArray<ToolCallFragment>): void {
    for (const tc of fragments) {
      // Key by index when the server provides one (delta-style streaming),
      // otherwise treat each fragment with an id - or each id-less fragment - as
      // its own whole call.
      const key =
        tc.index !== undefined
          ? `i${tc.index}`
          : tc.id ?? `seq${this.fallbackSeq++}`;
      const cur = this.byKey.get(key) ?? {name: '', argsStr: ''};

      const rawArgs = tc.function?.arguments;
      if (typeof rawArgs === 'object' && rawArgs !== null) {
        cur.argsObj = rawArgs;
      } else if (typeof rawArgs === 'string') {
        cur.argsStr += rawArgs;
      }

      this.byKey.set(key, {
        id: tc.id ?? cur.id,
        name: cur.name + (tc.function?.name ?? ''),
        argsStr: cur.argsStr,
        argsObj: cur.argsObj,
      });
    }
  }

  finish(): NeutralToolCall[] {
    const calls: NeutralToolCall[] = [];
    for (const {id, name, argsStr, argsObj} of this.byKey.values()) {
      if (name === '') continue;
      let parsed: Record<string, unknown> = argsObj ?? {};
      if (argsObj === undefined && argsStr.trim() !== '') {
        try {
          parsed = JSON.parse(argsStr);
        } catch {
          // Leave args empty on malformed JSON; the registry will validate and
          // the model can self-correct from the resulting tool error.
        }
      }
      calls.push({id, name, args: parsed});
    }
    return calls;
  }
}

export class OpenAiProtocol implements Protocol {
  readonly id = 'openai-compatible';
  readonly label = 'OpenAI-compatible (OpenAI, llama-server, vLLM, ...)';
  readonly capabilities = CAPABILITIES;
  readonly credentialFields = CREDENTIAL_FIELDS;

  // GET {base}/models is part of the OpenAI spec and implemented by virtually
  // every compatible server (OpenAI, llama-server, vLLM, LM Studio, Ollama).
  // Returns {data: [{id, ...}]}.
  async listModels(
    credentials: Readonly<Record<string, string>>,
    signal?: AbortSignal,
  ): Promise<ReadonlyArray<AvailableModel>> {
    const base = (credentials.endpoint || DEFAULT_BASE_URL).replace(/\/$/, '');
    const apiKey = credentials.apiKey ?? '';
    const resp = await fetch(`${base}/models`, {
      headers: apiKey ? {Authorization: `Bearer ${apiKey}`} : {},
      signal,
    });
    if (!resp.ok) {
      const text = await resp.text().catch(() => '<no body>');
      throw new Error(
        `OpenAI-compatible listModels error ${resp.status}: ${text}`,
      );
    }
    const json = (await resp.json()) as {
      readonly data?: ReadonlyArray<{readonly id?: string}>;
    };
    return (json.data ?? [])
      .map((m) => m.id ?? '')
      .filter((id) => id !== '')
      .map((id) => ({name: id}));
  }

  async *createStream(
    request: NeutralRequest,
    credentials: Readonly<Record<string, string>>,
    signal?: AbortSignal,
  ): AsyncGenerator<StreamEvent, void, void> {
    const base = (credentials.endpoint || DEFAULT_BASE_URL).replace(/\/$/, '');
    const apiKey = credentials.apiKey ?? '';
    const url = `${base}/chat/completions`;

    const body = {
      model: request.params.modelName,
      messages: messagesToOpenAi(request.systemPrompt, request.messages),
      ...(toolsToOpenAi(request) ? {tools: toolsToOpenAi(request)} : {}),
      stream: true,
      // Ask for usage in the final chunk (supported by OpenAI and most clones).
      stream_options: {include_usage: true},
      ...(request.params.temperature !== undefined
        ? {temperature: request.params.temperature}
        : {}),
      ...(request.params.maxOutputTokens !== undefined
        ? {max_tokens: request.params.maxOutputTokens}
        : {}),
    };


    DEBUG_SSE &&
      console.log(
        '[openai-sse] request tools:',
        JSON.stringify((body as {tools?: unknown}).tools, null, 2),
      );

    let resp: Response;
    try {
      resp = await fetch(url, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          ...(apiKey ? {Authorization: `Bearer ${apiKey}`} : {}),
        },
        body: JSON.stringify(body),
        signal,
      });
    } catch (e) {
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

    const assembler = new ToolCallAssembler();
    let finishReason: string | null = null;
    for await (const chunk of parseSse(resp.body, signal)) {
      const choice = chunk.choices?.[0];
      // Prefer incremental `delta`, but accept `message` for servers that
      // return the whole turn in one chunk.
      const delta = choice?.delta ?? choice?.message;
      if (delta?.content) {
        yield {type: 'text', text: delta.content};
      }
      if (delta?.tool_calls) {
        DEBUG_SSE &&
          console.log(
            '[openai-sse] tool_calls delta:',
            JSON.stringify(delta.tool_calls),
          );
        assembler.add(delta.tool_calls);
      }
      if (choice?.finish_reason) {
        finishReason = choice.finish_reason;
      }
      if (chunk.usage) {
        yield {
          type: 'usage',
          usage: {
            inputTokens: chunk.usage.prompt_tokens,
            outputTokens: chunk.usage.completion_tokens,
            totalTokens: chunk.usage.total_tokens,
          },
        };
      }
    }

    if (signal?.aborted) return;

    // Emit assembled tool calls (if any) before the terminal stop event.
    const calls = assembler.finish();

    DEBUG_SSE &&
      console.log('[openai-sse] assembled calls:', JSON.stringify(calls));
    for (const call of calls) {
      yield {type: 'tool-call', call};
    }

    if (calls.length > 0 || finishReason === 'tool_calls') {
      yield {type: 'stop', reason: 'tool-calls'};
    } else if (finishReason === 'length') {
      yield {type: 'stop', reason: 'length'};
    } else {
      yield {type: 'stop', reason: 'end'};
    }
  }
}

// SSE: blank-line-separated events, each a `data: <json>` line. The terminal
// event is the literal `data: [DONE]`.
async function* parseSse(
  body: ReadableStream<Uint8Array>,
  signal?: AbortSignal,
): AsyncGenerator<StreamChunk, void, void> {
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

      DEBUG_SSE && console.log('[openai-sse] raw:', payload);
      try {
        yield JSON.parse(payload) as StreamChunk;
      } catch {
        // Ignore unparseable events (keepalives, partial buffers).
      }
    }
  }
}
