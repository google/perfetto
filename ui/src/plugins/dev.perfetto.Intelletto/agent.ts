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
import type {LlmGateway, ModelPath} from '../dev.perfetto.Llm/gateway';
import type {
  Message,
  ToolCall,
  ToolDef,
  ToolResult,
} from '../dev.perfetto.Llm/protocol';
import type {ToolRegistry} from './tools';

// Hard bound on tool-use rounds per user turn. Stops a misbehaving model from
// looping forever (and caps token spend). Hitting it surfaces to the user
// rather than failing silently.
const MAX_ITERATIONS = 50;

// What the UI consumes as a turn streams.
export type AgentEvent =
  | {readonly type: 'text'; readonly text: string}
  | {readonly type: 'thought'; readonly text: string}
  | {readonly type: 'toolcall'; readonly name: string; readonly args: unknown}
  | {
      readonly type: 'toolresult';
      readonly name: string;
      readonly isError: boolean;
      // The (possibly truncated) tool result string, so the UI can show a
      // summary of what the tool returned. This is the same string fed back to
      // the model.
      readonly result: string;
    }
  | {
      readonly type: 'usage';
      readonly inputTokens?: number;
      readonly outputTokens?: number;
      readonly totalTokens?: number;
    }
  // A user-facing error (backend failure, iteration cap, no model configured).
  // Distinct from a tool error, which is fed back to the model, not surfaced.
  | {readonly type: 'error'; readonly message: string};

export class Agent {
  // The full conversation history (system prompt aside). Owned here because LLM
  // endpoints are stateless and we resend it every request.
  private history: Message[] = [];

  /**
   * @param gateway - The LLM gateway used to drive the model.
   * @param tools - The registry of tools available to the model.
   * @param systemPrompt - The system prompt sent on every request.
   * @param eagerTools - When true (the default), every tool definition is sent
   *     on every request and the list_tools/more_tools meta-tools are omitted.
   *     The lazy scheme (discover via list_tools, provision via more_tools) is
   *     an optimisation for *large* tool sets, but the indirection reliably
   *     trips up smaller / local models - they loop calling more_tools instead
   *     of the real tool. We only have a handful of core tools, so eager is
   *     both simpler and more robust. Flip this off once the tool count makes
   *     eager loading expensive.
   */
  constructor(
    private readonly gateway: LlmGateway,
    private readonly tools: ToolRegistry,
    private readonly systemPrompt: string,
    private readonly eagerTools: boolean = true,
  ) {}

  reset(): void {
    this.history = [];
  }

  /**
   * Run one user turn to completion, yielding events as they stream.
   *
   * @param userText - The user's message for this turn.
   * @param signal - Optional cancellation signal. An aborted turn stops
   *     cleanly and what completed stays in the history (the transcript stays
   *     truthful).
   * @param dequeueFollowUp - Implements queued follow-ups: it is polled once at
   *     the top of each tool-use round, and any message it returns is appended
   *     to the history as a user message, so something the user typed mid-loop
   *     ("oh wait, also check X") reaches the model at the next round without
   *     any interruption machinery.
   * @yields Events as the turn streams.
   */
  async *sendMessage(
    userText: string,
    signal?: AbortSignal,
    dequeueFollowUp?: () => string | undefined,
  ): AsyncGenerator<AgentEvent, void, void> {
    this.history.push({role: 'user', text: userText});

    // The model driving this turn: the first configured model advertising the
    // 'agentic' role. Looked up per turn so adding/removing a provider between
    // turns routes the next turn accordingly (history carries over).
    const modelDetails = this.gateway
      .listModels()
      .find((m) => m.model.roles.includes('agentic'));
    if (modelDetails === undefined) {
      yield {
        type: 'error',
        message:
          'No agentic model configured. Add a provider and a model with the ' +
          'agentic role in the LLM settings.',
      };
      return;
    }
    const modelPath: ModelPath = {
      providerId: modelDetails.provider.id,
      modelId: modelDetails.model.id,
    };

    // createStream requires a signal; use a never-aborting fallback when the
    // caller didn't supply one.
    const abortSignal = signal ?? new AbortController().signal;

    // The set of tool names exposed to the model this turn. In eager mode it's
    // every tool up front; in lazy mode it starts empty and grows as the model
    // pulls tools in via more_tools.
    const loaded = new Set<string>(
      this.eagerTools ? this.tools.list().map((t) => t.name) : [],
    );

    for (let iter = 0; iter < MAX_ITERATIONS; iter++) {
      if (signal?.aborted) return;

      // Pick up any follow-up the user queued while the loop was running.
      const followUp = dequeueFollowUp?.();
      if (followUp !== undefined) {
        this.history.push({role: 'user', text: followUp});
      }

      const toolDefs = this.buildToolDefs(loaded);
      const calls: ToolCall[] = [];
      let turnText = '';
      let backendError: string | undefined;

      for await (const evt of this.gateway.createStream(
        modelPath,
        {
          systemPrompt: this.systemPrompt,
          messages: this.history,
          tools: toolDefs,
        },
        abortSignal,
      )) {
        switch (evt.type) {
          case 'text':
            turnText += evt.text;
            yield {type: 'text', text: evt.text};
            break;
          case 'thought':
            yield {type: 'thought', text: evt.text};
            break;
          case 'tool-call':
            calls.push(evt.call);
            yield {
              type: 'toolcall',
              name: evt.call.name,
              args: evt.call.args,
            };
            break;
          case 'usage':
            yield {type: 'usage', ...evt.usage};
            break;
          case 'stop':
            if (evt.reason === 'error') {
              backendError = evt.error?.message ?? 'Unknown backend error';
            }
            break;
        }
      }

      if (signal?.aborted) return;

      if (backendError !== undefined) {
        yield {type: 'error', message: backendError};
        return;
      }

      // Record the model's turn (text and/or tool calls) in the history.
      if (turnText.length > 0) {
        this.history.push({role: 'model', text: turnText});
      }
      if (calls.length === 0) {
        // No tool calls -> the model is done with this turn.
        return;
      }
      this.history.push({role: 'tool-call', calls});

      // Execute every requested tool, threading results back. Tool errors
      // (bad SQL, invalid args) become tool results the model can self-correct
      // from - they are NOT surfaced to the user as errors.
      const results: ToolResult[] = [];
      for (const call of calls) {
        if (signal?.aborted) return;
        const {result, isError} = await this.runTool(call, loaded);
        yield {type: 'toolresult', name: call.name, isError, result};
        results.push({name: call.name, result, isError});
      }
      this.history.push({role: 'tool-result', results});
    }

    yield {
      type: 'error',
      message:
        `Stopped after ${MAX_ITERATIONS} tool-use rounds without a ` +
        `final answer. Try narrowing the question.`,
    };
  }

  // Execute one tool call. The meta-tools (list_tools / more_tools) are handled
  // here because they mutate the loop's loaded-tool set; everything else goes
  // through the registry (which validates args first).
  private async runTool(
    call: ToolCall,
    loaded: Set<string>,
  ): Promise<{result: string; isError: boolean}> {
    try {
      if (call.name === 'list_tools') {
        const list = this.tools.list().map((t) => ({
          name: t.name,
          description: firstLine(t.description),
        }));
        return {result: JSON.stringify(list), isError: false};
      }
      if (call.name === 'more_tools') {
        const names = z
          .object({names: z.array(z.string())})
          .parse(call.args).names;
        const unknown = names.filter((n) => this.tools.get(n) === undefined);
        names.forEach((n) => this.tools.get(n) && loaded.add(n));
        const msg =
          unknown.length > 0
            ? `Loaded. Unknown (ignored): ${unknown.join(', ')}.`
            : 'Loaded. The requested tools are now available.';
        return {result: msg, isError: unknown.length > 0};
      }
      const result = await this.tools.call(call.name, call.args);
      return {result, isError: false};
    } catch (e) {
      return {result: `Error: ${String(e)}`, isError: true};
    }
  }

  // The tool list sent on a request. In lazy mode this is the bootstrap
  // meta-tools (list_tools/more_tools) plus every tool the model has loaded so
  // far; in eager mode the meta-tools are omitted and `loaded` already holds
  // every tool. Order is stable and the set only grows within a turn,
  // preserving the cached prompt prefix.
  private buildToolDefs(loaded: Set<string>): ToolDef[] {
    const defs: ToolDef[] = [];
    if (!this.eagerTools) {
      defs.push(
        {
          name: 'list_tools',
          description:
            'List the names and one-line descriptions of every tool ' +
            'available. Call this first to discover what you can do, then ' +
            'more_tools to load the ones you need before calling them.',
          inputSchema: z.object({}),
        },
        {
          name: 'more_tools',
          description:
            'Load tool definitions by name so you can call them. You can only ' +
            'call a tool after loading it here. Discover names with list_tools.',
          inputSchema: z.object({
            names: z
              .array(z.string())
              .describe('Tool names to load, from list_tools.'),
          }),
        },
      );
    }
    for (const tool of this.tools.list()) {
      if (loaded.has(tool.name)) {
        defs.push({
          name: tool.name,
          description: tool.description,
          inputSchema: tool.inputSchema,
        });
      }
    }
    return defs;
  }
}

function firstLine(s: string): string {
  const trimmed = s.trim();
  const nl = trimmed.indexOf('\n');
  return nl === -1 ? trimmed : trimmed.slice(0, nl);
}
