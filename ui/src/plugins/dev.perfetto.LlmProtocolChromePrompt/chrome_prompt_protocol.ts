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

// A Protocol backed by Chrome's experimental built-in Prompt API (the
// `LanguageModel` global), which runs an on-device Gemma-based model ("Gemini
// Nano") entirely in the browser - no network, no API key, no data leaving the
// machine. Requires a recent Chrome with the feature enabled (chrome://flags ->
// "Prompt API for Gemini Nano", or an origin trial). See
// https://developer.chrome.com/docs/ai/prompt-api.
//
// The API has no native function calling, so tool use is *emulated*: tool
// definitions are injected into the system prompt and the model is asked to
// reply with a JSON object when it wants to call a tool, which we parse back
// out. This is best-effort - small local models are unreliable tool callers -
// but it lets the on-device model participate in the same agent loop as the
// cloud protocols. Plain (tool-free) chat streams normally.

import {z} from 'zod';
import type {
  AvailableModel,
  CredentialField,
  NeutralMessage,
  NeutralRequest,
  NeutralToolCall,
  NeutralToolDef,
  Protocol,
  ProtocolCapabilities,
  StreamEvent,
} from '../dev.perfetto.Llm/protocol';

// --- The Prompt API surface (the subset we use) ------------------------------
// These globals aren't in the TS DOM lib yet, so we declare the shapes we touch.

type Availability =
  | 'unavailable'
  | 'downloadable'
  | 'downloading'
  | 'available';

interface LanguageModelMessage {
  readonly role: 'system' | 'user' | 'assistant';
  readonly content: string;
}

interface LanguageModelParams {
  readonly defaultTopK: number;
  readonly maxTopK: number;
  readonly defaultTemperature: number;
  readonly maxTemperature: number;
}

interface DownloadMonitor {
  addEventListener(
    type: 'downloadprogress',
    cb: (e: {readonly loaded: number}) => void,
  ): void;
}

// What languages we expect to read/write. Chrome warns (and may degrade
// quality / safety attestation) if the output language is left unspecified.
interface ExpectedLanguages {
  readonly type: 'text';
  readonly languages: ReadonlyArray<string>;
}

interface CreateOptions {
  readonly initialPrompts?: ReadonlyArray<LanguageModelMessage>;
  readonly temperature?: number;
  readonly topK?: number;
  readonly signal?: AbortSignal;
  readonly monitor?: (m: DownloadMonitor) => void;
  readonly expectedInputs?: ReadonlyArray<ExpectedLanguages>;
  readonly expectedOutputs?: ReadonlyArray<ExpectedLanguages>;
}

interface LanguageModelSession {
  prompt(input: string, opts?: {signal?: AbortSignal}): Promise<string>;
  promptStreaming(
    input: string,
    opts?: {signal?: AbortSignal},
  ): ReadableStream<string>;
  destroy(): void;
}

interface LanguageModelStatic {
  availability(): Promise<Availability>;
  params(): Promise<LanguageModelParams>;
  create(opts?: CreateOptions): Promise<LanguageModelSession>;
}

function getLanguageModel(): LanguageModelStatic | undefined {
  return (globalThis as {LanguageModel?: LanguageModelStatic}).LanguageModel;
}

const NOT_AVAILABLE_MSG =
  'The Chrome built-in Prompt API (LanguageModel) is not available in this ' +
  'browser. It needs a recent Chrome with on-device AI enabled (see ' +
  'chrome://flags -> "Prompt API for Gemini Nano").';

// --- Tool-call emulation -----------------------------------------------------

// Build the system-prompt addendum that teaches the model our tool protocol.
function buildToolInstructions(tools: ReadonlyArray<NeutralToolDef>): string {
  const lines = ['You can call tools to help answer. The available tools are:'];
  for (const t of tools) {
    // Inline, self-contained JSON Schema (draft 2020-12) so the model sees the
    // exact argument shape. zod 4's native converter; `target` keeps it free of
    // $ref/$defs the model would have to chase.
    const schema = z.toJSONSchema(t.inputSchema, {target: 'draft-2020-12'}) as {
      $schema?: unknown;
    };
    delete schema.$schema;
    lines.push(
      `- ${t.name}: ${t.description}\n  arguments JSON Schema: ` +
        JSON.stringify(schema),
    );
  }
  lines.push(
    'To call a tool, reply with ONLY a single JSON object and nothing else, ' +
      'no prose and no markdown code fences:\n' +
      '{"tool_call": {"name": "<tool name>", "arguments": { <args> }}}\n' +
      'To answer the user directly, reply in plain text and do NOT output ' +
      'any JSON. Call at most one tool per reply.',
  );
  return lines.join('\n\n');
}

// Flatten the neutral history into a single transcript string. The Prompt API
// is multi-turn, but flattening sidesteps its role-alternation constraints
// (which our emulated tool-call/tool-result turns would otherwise trip) and is
// plenty for a small model. The system prompt is passed separately, via
// initialPrompts, not here.
function messagesToTranscript(messages: ReadonlyArray<NeutralMessage>): string {
  const parts: string[] = [];
  for (const msg of messages) {
    switch (msg.role) {
      case 'user':
        parts.push(`User: ${msg.text}`);
        break;
      case 'model':
        parts.push(`Assistant: ${msg.text}`);
        break;
      case 'tool-call':
        for (const c of msg.calls) {
          parts.push(
            `Assistant: ${JSON.stringify({
              tool_call: {name: c.name, arguments: c.args},
            })}`,
          );
        }
        break;
      case 'tool-result':
        for (const r of msg.results) {
          const tag = r.isError ? 'Tool error' : 'Tool result';
          parts.push(`${tag} (${r.name}): ${r.result}`);
        }
        break;
    }
  }
  // Cue the model to produce the next assistant turn.
  parts.push('Assistant:');
  return parts.join('\n\n');
}

// Strip a leading/trailing ```...``` markdown fence, if present.
function stripFences(text: string): string {
  const t = text.trim();
  const fence = /^```(?:json)?\s*\n?([\s\S]*?)\n?```$/i.exec(t);
  return fence ? fence[1].trim() : t;
}

// Return the first brace-balanced `{...}` substring, ignoring braces inside
// strings. Lets us recover a tool call even if the model wrapped it in prose.
function firstJsonObject(text: string): string | undefined {
  const start = text.indexOf('{');
  if (start === -1) return undefined;
  let depth = 0;
  let inStr = false;
  let escaped = false;
  for (let i = start; i < text.length; i++) {
    const ch = text[i];
    if (inStr) {
      if (escaped) escaped = false;
      else if (ch === '\\') escaped = true;
      else if (ch === '"') inStr = false;
      continue;
    }
    if (ch === '"') inStr = true;
    else if (ch === '{') depth++;
    else if (ch === '}' && --depth === 0) return text.slice(start, i + 1);
  }
  return undefined;
}

// Try to read an emulated tool call out of the model's reply. Returns undefined
// if the reply isn't a (valid, known) tool call - in which case it's an answer.
function extractToolCall(
  text: string,
  toolNames: ReadonlySet<string>,
): NeutralToolCall | undefined {
  const cleaned = stripFences(text);
  for (const candidate of [cleaned, firstJsonObject(cleaned)]) {
    if (candidate === undefined) continue;
    let obj: unknown;
    try {
      obj = JSON.parse(candidate);
    } catch {
      continue;
    }
    const tc =
      (obj as {tool_call?: unknown; toolCall?: unknown})?.tool_call ??
      (obj as {toolCall?: unknown})?.toolCall;
    if (tc === undefined || tc === null || typeof tc !== 'object') continue;
    const name = (tc as {name?: unknown}).name;
    if (typeof name !== 'string' || !toolNames.has(name)) continue;
    const rawArgs =
      (tc as {arguments?: unknown}).arguments ?? (tc as {args?: unknown}).args;
    const args =
      typeof rawArgs === 'object' && rawArgs !== null
        ? (rawArgs as Record<string, unknown>)
        : {};
    return {name, args};
  }
  return undefined;
}

// --- Error normalisation -----------------------------------------------------

function errorEvent(e: unknown): StreamEvent {
  // Chrome surfaces these as DOMExceptions with a `name`. Map the ones we know.
  const name = (e as {name?: string})?.name;
  let kind: 'rate-limit' | 'auth' | 'context-length' | 'network' | 'unknown';
  switch (name) {
    case 'QuotaExceededError':
      kind = 'context-length';
      break;
    case 'NotAllowedError':
      kind = 'auth';
      break;
    default:
      kind = 'unknown';
  }
  return {
    type: 'stop',
    reason: 'error',
    error: {kind, message: `Chrome Prompt API error: ${String(e)}`},
  };
}

// --- The protocol ------------------------------------------------------------

const CAPABILITIES: ProtocolCapabilities = {
  // No native function calling - emulated via prompt injection (see header).
  nativeToolCalling: false,
  streaming: true,
  vision: false,
};

// Local on-device model: nothing to configure.
const CREDENTIAL_FIELDS: ReadonlyArray<CredentialField> = [];

export class ChromePromptProtocol implements Protocol {
  readonly id = 'chrome-prompt';
  readonly label = 'Chrome built-in (on-device Gemini Nano)';
  readonly capabilities = CAPABILITIES;
  readonly credentialFields = CREDENTIAL_FIELDS;

  // No models endpoint - the browser picks the device model. Surface a single
  // logical entry, gated on the API actually being usable, so the settings UI
  // can offer it (and reports clearly when it can't).
  async listModels(): Promise<ReadonlyArray<AvailableModel>> {
    const lm = getLanguageModel();
    if (lm === undefined) throw new Error(NOT_AVAILABLE_MSG);
    const availability = await lm.availability();
    if (availability === 'unavailable') {
      throw new Error('The on-device model is unavailable on this device.');
    }
    return [{name: 'gemini-nano'}];
  }

  async *createStream(
    request: NeutralRequest,
    _credentials: Readonly<Record<string, string>>,
    signal?: AbortSignal,
  ): AsyncGenerator<StreamEvent, void, void> {
    const lm = getLanguageModel();
    if (lm === undefined) {
      yield {
        type: 'stop',
        reason: 'error',
        error: {kind: 'unknown', message: NOT_AVAILABLE_MSG},
      };
      return;
    }

    let availability: Availability;
    try {
      availability = await lm.availability();
    } catch (e) {
      yield errorEvent(e);
      return;
    }
    if (availability === 'unavailable') {
      yield {
        type: 'stop',
        reason: 'error',
        error: {
          kind: 'unknown',
          message: 'The on-device model is unavailable on this device.',
        },
      };
      return;
    }

    const hasTools = request.tools.length > 0;
    const systemPrompt = hasTools
      ? `${request.systemPrompt}\n\n${buildToolInstructions(request.tools)}`
      : request.systemPrompt;
    const toolNames = new Set(request.tools.map((t) => t.name));

    const createOpts: {
      initialPrompts: LanguageModelMessage[];
      signal?: AbortSignal;
      temperature?: number;
      topK?: number;
      monitor?: (m: DownloadMonitor) => void;
      expectedInputs?: ReadonlyArray<ExpectedLanguages>;
      expectedOutputs?: ReadonlyArray<ExpectedLanguages>;
    } = {
      initialPrompts: [{role: 'system', content: systemPrompt}],
      signal,
      // Declare English in/out. The model is English-only in practice, and
      // leaving the output language unset makes Chrome warn and may hurt
      // quality / safety attestation.
      expectedInputs: [{type: 'text', languages: ['en']}],
      expectedOutputs: [{type: 'text', languages: ['en']}],
    };

    // The API requires temperature and topK together (or neither). We only have
    // a temperature, so pair it with the device default topK. maxOutputTokens
    // isn't an option the Prompt API accepts, so it's ignored.
    if (request.params.temperature !== undefined) {
      try {
        const params = await lm.params();
        createOpts.temperature = Math.max(
          0,
          Math.min(request.params.temperature, params.maxTemperature),
        );
        createOpts.topK = params.defaultTopK;
      } catch {
        // params() failed - just let the model use its own defaults.
      }
    }

    // First use on a machine downloads the model (hundreds of MB). Give the
    // user a heads-up and log progress; there's no neutral progress event.
    if (availability !== 'available') {
      yield {
        type: 'thought',
        text: 'Downloading the on-device model (first use only); this may take a while…',
      };
      createOpts.monitor = (m) =>
        m.addEventListener('downloadprogress', (e) =>
          console.log(
            `[chrome-prompt] model download ${Math.round(e.loaded * 100)}%`,
          ),
        );
    }

    let session: LanguageModelSession;
    try {
      session = await lm.create(createOpts);
    } catch (e) {
      if (signal?.aborted) return;
      yield errorEvent(e);
      return;
    }

    try {
      const input = messagesToTranscript(request.messages);

      if (!hasTools) {
        // Plain chat: stream tokens straight through.
        yield* this.streamText(session, input, signal);
        if (!signal?.aborted) yield {type: 'stop', reason: 'end'};
        return;
      }

      // Tool mode: stream text, but if the reply opens like a JSON object (or a
      // fenced block) treat it as a (silent) tool call and buffer until we can
      // parse it - so we never leak the call JSON into the chat as text.
      let acc = '';
      let mode: 'undecided' | 'text' | 'json' = 'undecided';
      try {
        for await (const chunk of this.streamChunks(session, input, signal)) {
          acc += chunk;
          if (mode === 'undecided') {
            const lead = acc.replace(/^\s+/, '');
            if (lead === '') continue; // still only whitespace
            if (lead.startsWith('{') || lead.startsWith('```')) {
              mode = 'json';
            } else {
              mode = 'text';
              yield {type: 'text', text: acc};
            }
          } else if (mode === 'text') {
            yield {type: 'text', text: chunk};
          }
          // 'json' mode: keep buffering silently.
        }
      } catch (e) {
        if (signal?.aborted) return;
        yield errorEvent(e);
        return;
      }

      if (signal?.aborted) return;

      if (mode === 'json') {
        const call = extractToolCall(acc, toolNames);
        if (call !== undefined) {
          yield {type: 'tool-call', call};
          yield {type: 'stop', reason: 'tool-calls'};
          return;
        }
        // Looked like JSON but wasn't a known tool call - surface as text.
        yield {type: 'text', text: stripFences(acc)};
      }
      yield {type: 'stop', reason: 'end'};
    } finally {
      session.destroy();
    }
  }

  // Stream a turn as plain `text` events.
  private async *streamText(
    session: LanguageModelSession,
    input: string,
    signal?: AbortSignal,
  ): AsyncGenerator<StreamEvent, void, void> {
    try {
      for await (const chunk of this.streamChunks(session, input, signal)) {
        yield {type: 'text', text: chunk};
      }
    } catch (e) {
      if (signal?.aborted) return;
      yield errorEvent(e);
    }
  }

  // Read the promptStreaming ReadableStream chunk by chunk. Chunks are
  // incremental text deltas.
  private async *streamChunks(
    session: LanguageModelSession,
    input: string,
    signal?: AbortSignal,
  ): AsyncGenerator<string, void, void> {
    const stream = session.promptStreaming(input, {signal});
    const reader = stream.getReader();
    try {
      for (;;) {
        if (signal?.aborted) return;
        const {value, done} = await reader.read();
        if (done) break;
        if (value) yield value;
      }
    } finally {
      reader.releaseLock();
    }
  }
}
