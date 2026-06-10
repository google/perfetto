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

// The Protocol layer: the code-behind that knows how to talk to a *kind* of
// LLM API (gemini, anthropic, openai-compatible, ...). A protocol is provided
// by a plugin and registered with the gateway; one protocol backs many
// providers (see provider.ts). The rest of the gateway, and consumers like the
// Intelletto assistant, only ever deal in the neutral request/response shapes
// defined here - they never see a backend's native wire format.

import type {z} from 'zod';

// --- Neutral conversation shapes ---------------------------------------------

// A neutral tool definition, as the model sees it. The Protocol is responsible
// for down-converting `inputSchema` into whatever subset of JSON Schema the
// backend accepts.
export interface NeutralToolDef {
  readonly name: string;
  readonly description: string;
  // Authored in zod; the protocol converts to the backend's native schema.
  readonly inputSchema: z.ZodType;
}

// One model->user message asking to invoke a tool.
export interface NeutralToolCall {
  // Backend-specific id used to thread the result back to this call. Some
  // backends (Gemini) key on the tool name instead of an id; protocols that
  // don't need an id leave it undefined.
  readonly id?: string;
  readonly name: string;
  readonly args: Record<string, unknown>;
  // Opaque, protocol-private blob the consumer must carry through history and
  // hand back unchanged when re-sending this call. Used by backends that
  // require state to be echoed verbatim on the next request - e.g. Gemini's
  // `thoughtSignature`, which the API rejects requests for if dropped. Other
  // protocols leave it undefined and ignore it.
  readonly signature?: string;
}

// The user->model result of a previously-requested tool call.
export interface NeutralToolResult {
  readonly id?: string;
  readonly name: string;
  // The serialised tool output (or an error string the model can recover from).
  readonly result: string;
  readonly isError?: boolean;
}

// One entry in the conversation history. The consumer owns the history and
// resends it on every request (LLM endpoints are stateless).
export type NeutralMessage =
  | {readonly role: 'user'; readonly text: string}
  | {readonly role: 'model'; readonly text: string}
  | {readonly role: 'tool-call'; readonly calls: ReadonlyArray<NeutralToolCall>}
  | {
      readonly role: 'tool-result';
      readonly results: ReadonlyArray<NeutralToolResult>;
    };

// Model parameters that travel with a request (provider/model -derived).
export interface NeutralModelParams {
  // The backend's own model identifier, e.g. 'gemini-2.5-flash'.
  readonly modelName: string;
  readonly temperature?: number;
  readonly maxOutputTokens?: number;
}

// A complete request handed to the protocol. Everything the backend needs to
// produce the next turn.
export interface NeutralRequest {
  readonly systemPrompt: string;
  readonly messages: ReadonlyArray<NeutralMessage>;
  readonly tools: ReadonlyArray<NeutralToolDef>;
  readonly params: NeutralModelParams;
}

// --- Streamed response events ------------------------------------------------

// Token usage, normalised across backends. Any field a backend doesn't report
// is left undefined.
export interface TokenUsage {
  readonly inputTokens?: number;
  readonly outputTokens?: number;
  readonly totalTokens?: number;
}

// Why a turn stopped. Backends report a zoo of finish reasons; we collapse them
// into this set.
export type StopReason =
  | 'end' // The model finished its turn normally.
  | 'tool-calls' // The model wants to call tools and is waiting on results.
  | 'length' // Hit the output-token limit.
  | 'error'; // Aborted by a backend error (see NeutralError).

// Normalised backend error categories. Surfaced to the user in the chat.
export type ErrorKind =
  | 'rate-limit'
  | 'auth'
  | 'context-length'
  | 'network'
  | 'unknown';

export interface NeutralError {
  readonly kind: ErrorKind;
  readonly message: string;
}

// The incremental events a protocol emits while streaming one turn. A turn is a
// sequence of these ending in exactly one `stop`.
export type StreamEvent =
  // Incremental assistant text.
  | {readonly type: 'text'; readonly text: string}
  // Incremental "thinking" text, where the backend exposes it. Kept separate so
  // consumers can show or hide it independently of the answer.
  | {readonly type: 'thought'; readonly text: string}
  // A fully-formed tool call the model wants executed.
  | {readonly type: 'tool-call'; readonly call: NeutralToolCall}
  // Token accounting for the turn (may arrive incrementally; last one wins).
  | {readonly type: 'usage'; readonly usage: TokenUsage}
  // Terminal event. `error` is set iff reason === 'error'.
  | {
      readonly type: 'stop';
      readonly reason: StopReason;
      readonly error?: NeutralError;
    };

// --- What a protocol declares about itself -----------------------------------

export interface ProtocolCapabilities {
  // Whether the backend natively supports tool/function calling. If false the
  // gateway/consumer would have to emulate it via prompt injection (not done in
  // Phase 1 - all Phase 1 protocols are native).
  readonly nativeToolCalling: boolean;
  // Whether the backend streams responses incrementally.
  readonly streaming: boolean;
  // Whether the backend accepts image input. Phase 1 is text-only, so this is
  // informational for now.
  readonly vision: boolean;
}

// Describes the credential/connection form a provider for this protocol needs.
// The settings UI renders a field per entry; the protocol declares its shape so
// the gateway never has to know about any specific backend's login form.
export interface CredentialField {
  readonly key: string;
  readonly label: string;
  // `secret` fields are masked in the UI and flagged for any future
  // settings-export stripping (see settings.ts `.meta({secret: true})`).
  readonly secret?: boolean;
  readonly required?: boolean;
  readonly placeholder?: string;
}

// One model the backend reports it can serve, as returned by listModels(). Just
// the backend's own model identifier (e.g. 'gemini-2.5-flash') for now - the
// settings UI uses these to populate the model-name combobox.
export interface AvailableModel {
  readonly name: string;
}

// The contract a protocol implementation must satisfy. Implemented by protocol
// plugins (e.g. dev.perfetto.LlmProtocolGemini) and registered with the gateway
// via registerProtocol().
export interface Protocol {
  // Stable id referenced by providers, e.g. 'gemini'.
  readonly id: string;
  // Human-readable name for the settings UI, e.g. 'Google Gemini'.
  readonly label: string;
  readonly capabilities: ProtocolCapabilities;
  // The credential fields a provider using this protocol must supply.
  readonly credentialFields: ReadonlyArray<CredentialField>;

  // Ask the backend which models it can serve, using the provider's
  // credentials. Optional: a protocol that has no models endpoint (or doesn't
  // implement it) simply omits this, and the settings UI falls back to a
  // free-text model name. Throws on a network/auth error so the caller can
  // tell "couldn't reach the backend" from "backend served an empty list".
  listModels?(
    credentials: Readonly<Record<string, string>>,
    signal?: AbortSignal,
  ): Promise<ReadonlyArray<AvailableModel>>;

  // Take a neutral request and stream back a normalised response. The protocol
  // owns translating tool defs/calls/results to and from the backend's native
  // format and normalising errors into a terminal `stop` event (it must not
  // throw for backend errors - it emits {type:'stop', reason:'error'} so the
  // consumer can render it in the chat). `credentials` is the provider's
  // configured credential bag (keyed by CredentialField.key).
  createStream(
    request: NeutralRequest,
    credentials: Readonly<Record<string, string>>,
    signal?: AbortSignal,
  ): AsyncGenerator<StreamEvent, void, void>;
}
