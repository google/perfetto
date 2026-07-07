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

import m from 'mithril';
import type {LlmGateway} from '../dev.perfetto.Llm/gateway';
import {Agent} from './agent';
import type {ContextRegistry} from './context';
import type {ToolRegistry} from './tools';
import {SYSTEM_PROMPT} from './system_prompt';

/** A single line in the conversation transcript. */
export interface ChatLine {
  /** Which kind of line this is; drives how it renders. */
  role: 'ai' | 'user' | 'error' | 'thought' | 'toolcall' | 'spacer';
  /** The line's text content (e.g. message body, or tool name for toolcalls). */
  text: string;
  /**
   * For 'toolcall' lines: the call args and (once it arrives) the result, so
   * the line can show a collapsible summary of what the tool did and returned.
   */
  toolArgs?: unknown;
  toolResult?: string;
  toolError?: boolean;
}

/**
 * The state and behaviour of one chat conversation with the agent: owns the
 * transcript, the in-flight turn, the follow-up queue, and the Agent instance.
 * The UI renders from this and calls its methods; there is no other state.
 */
export class ChatSession {
  /**
   * The assembled system prompt in use for this conversation - kept so the
   * "show system prompt" inspector displays exactly what the agent sends.
   */
  readonly systemPrompt: string;

  /** The conversation transcript, in display order. */
  lines: ChatLine[] = [];
  /** The current (uncommitted) text in the input box. */
  input = '';
  /** True while a turn is in flight. */
  loading = false;
  /** Total tokens reported by the most recent turn, if any. */
  totalTokens?: number;
  /** Context items the user has toggled off for the next prompt. */
  readonly excludedContext = new Set<string>();

  private readonly agent: Agent;
  private abort?: AbortController;
  // Messages typed while a turn was in flight. The agent polls this at the top
  // of each tool-use round; anything still queued when the turn ends starts a
  // fresh turn. Cancelling drops the queue.
  private pendingFollowUps: string[] = [];

  /**
   * @param gateway - The LLM gateway used to drive the agent.
   * @param tools - The registry of tools available to the agent.
   * @param context - The registry supplying per-turn context items and, at
   *     construction, the payload-format descriptions folded into the system
   *     prompt.
   */
  constructor(
    gateway: LlmGateway,
    tools: ToolRegistry,
    private readonly context: ContextRegistry,
  ) {
    // The context providers' invariant payload-format descriptions are folded
    // into the system prompt here, once per conversation - per-turn payloads
    // stay data-only.
    this.systemPrompt = assembleSystemPrompt(context);
    this.agent = new Agent(gateway, tools, this.systemPrompt);
    this.lines = [];
  }

  /**
   * Submit the current input. If a turn is already running the text is queued
   * as a follow-up; otherwise it starts a new turn (and drains any follow-ups
   * queued while it runs). No-op on empty input.
   */
  async send(): Promise<void> {
    const text = this.input.trim();
    if (text === '') return;

    this.input = '';
    this.lines.push({role: 'user', text});

    if (this.loading) {
      // Typed mid-turn: queue it as a follow-up. The running turn picks it up
      // at its next tool-use round, or the loop below starts a new turn with
      // it once the current one finishes.
      this.pendingFollowUps.push(text);
      return;
    }

    let next: string | undefined = text;
    while (next !== undefined) {
      await this.runTurn(next);
      next = this.pendingFollowUps.shift();
    }
  }

  /**
   * Abort the in-flight turn and drop any queued follow-ups - "stop" means
   * stop. Leaves the transcript in place with a "(turn cancelled)" marker.
   */
  cancel(): void {
    this.abort?.abort();
    this.pendingFollowUps = [];
    this.lines.push({role: 'error', text: '(turn cancelled)'});
  }

  /**
   * Reset to a fresh conversation: abort any in-flight turn, clear the agent's
   * history and the token count, and replace the transcript with a greeting.
   */
  newConversation(): void {
    this.abort?.abort();
    this.pendingFollowUps = [];
    this.agent.reset();
    this.totalTokens = undefined;
    this.lines = [
      {role: 'ai', text: 'New conversation. What would you like to know?'},
    ];
  }

  private appendAi(text: string) {
    const last = this.lines[this.lines.length - 1];
    if (last?.role === 'ai') {
      last.text += text;
    } else {
      this.lines.push({role: 'ai', text});
    }
  }

  private async runTurn(text: string): Promise<void> {
    // Fold in any non-excluded context as a preamble the model can see.
    const context = this.context
      .buildContextItems()
      .filter((c) => !this.excludedContext.has(c.id));
    const prompt =
      context.length === 0
        ? text
        : `Context for this question:\n${context
            .map((c) => `- ${c.label}: ${c.payload}`)
            .join('\n')}\n\nQuestion: ${text}`;

    this.loading = true;
    this.abort = new AbortController();
    m.redraw();

    try {
      for await (const evt of this.agent.sendMessage(
        prompt,
        this.abort.signal,
        () => this.pendingFollowUps.shift(),
      )) {
        switch (evt.type) {
          case 'text':
            this.appendAi(evt.text);
            break;
          case 'thought':
            this.lines.push({role: 'thought', text: evt.text});
            break;
          case 'toolcall':
            this.lines.push({
              role: 'toolcall',
              text: evt.name,
              toolArgs: evt.args,
            });
            this.lines.push({role: 'spacer', text: ''});
            break;
          case 'toolresult':
            // Attach the result to the most recent matching tool-call line that
            // doesn't have one yet (tool calls within a round execute in order).
            for (let i = this.lines.length - 1; i >= 0; i--) {
              const line = this.lines[i];
              if (
                line.role === 'toolcall' &&
                line.text === evt.name &&
                line.toolResult === undefined
              ) {
                line.toolResult = evt.result;
                line.toolError = evt.isError;
                break;
              }
            }
            break;
          case 'usage':
            if (evt.totalTokens !== undefined) {
              this.totalTokens = evt.totalTokens;
            }
            break;
          case 'error':
            this.lines.push({role: 'error', text: evt.message});
            break;
        }
        m.redraw();
      }
      this.lines.push({role: 'spacer', text: ''});
    } catch (e) {
      this.lines.push({role: 'error', text: `Something went wrong: ${e}`});
    } finally {
      this.loading = false;
      this.abort = undefined;
      m.redraw();
    }
  }
}

// The assembled system prompt: the application brief plus the context
// providers' payload-format descriptions (already sorted by provider id, so
// the result is byte-identical across turns and the cached prefix survives).
// Sampled once per conversation; providers registered mid-conversation take
// effect on the next one.
function assembleSystemPrompt(context: ContextRegistry): string {
  const descriptions = context.descriptions();
  if (descriptions.length === 0) return SYSTEM_PROMPT;
  return [
    SYSTEM_PROMPT,
    'Context payload formats (for the context preamble on user messages):',
    ...descriptions,
  ].join('\n\n');
}
