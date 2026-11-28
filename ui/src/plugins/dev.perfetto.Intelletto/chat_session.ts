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
import {uuidv4} from '../../base/uuid';
import type {LlmGateway, ModelPath} from '../dev.perfetto.Llm/gateway';
import {Agent, type AgentEvent} from './agent';
import type {ContextRegistry} from './context';
import type {ConversationStore, StoredConversation} from './conversation_store';
import type {ToolRegistry} from './tools';

const CANCELLED_TEXT = '(turn cancelled)';

/** A single line in the conversation transcript. */
export interface ChatLine {
  /** Which kind of line this is; drives how it renders. */
  role:
    'ai' | 'user' | 'error' | 'thought' | 'toolcall' | 'compaction' | 'spacer';
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
  /** Stable id of this conversation, used as its key in the store. */
  readonly id: string;
  /** When this conversation was started (ms since epoch). */
  readonly createdAt: number;
  /** The model this conversation is using, if one has been picked. */
  model?: ModelPath;
  /** The conversation transcript, in display order. */
  lines: ChatLine[] = [];
  /** True while a turn is in flight. */
  loading = false;
  /** True while older conversation history is being compacted. */
  compacting = false;
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
  // A user-chosen name, overriding the title derived from the first message.
  private customTitle?: string;

  /** Follow-up messages queued while a turn is in flight, in FIFO order. */
  get queuedFollowUps(): readonly string[] {
    return this.pendingFollowUps;
  }

  /** Remove a queued follow-up at the given index. */
  removeQueuedFollowUp(index: number): void {
    if (index < 0 || index >= this.pendingFollowUps.length) return;
    this.pendingFollowUps.splice(index, 1);
  }

  /**
   * Promote a queued follow-up to the front of the queue and interrupt the
   * in-flight turn so it runs immediately.
   */
  sendQueuedFollowUpNow(index: number, model: ModelPath): void {
    const [text] = this.pendingFollowUps.splice(index, 1);
    if (text === undefined) return;
    if (this.loading) {
      this.pendingFollowUps.unshift(text);
      this.abort?.abort();
    } else {
      void this.send(text, model);
    }
  }

  /** Toggle whether a context item is excluded from the next prompt. */
  toggleContext(id: string): void {
    if (this.excludedContext.has(id)) {
      this.excludedContext.delete(id);
    } else {
      this.excludedContext.add(id);
    }
  }

  /**
   * @param gateway - The LLM gateway used to drive the agent.
   * @param tools - The registry (or getter for the active registry) of tools
   *     available to the agent.
   * @param context - The registry (or getter for the active registry) supplying
   *     per-turn context items.
   * @param systemPrompt - The assembled system prompt (or getter) sent on every
   *     request.
   * @param store - Where the conversation is persisted after each change.
   * @param init - Either a previously stored conversation to restore, or the
   *     initial model for a fresh one.
   */
  constructor(
    gateway: LlmGateway,
    tools: ToolRegistry | (() => ToolRegistry),
    private readonly context: ContextRegistry | (() => ContextRegistry),
    systemPrompt: string | (() => string),
    private readonly store: ConversationStore,
    init: {readonly stored: StoredConversation} | {readonly model?: ModelPath},
    maxCompactionTokens?: number | (() => number),
  ) {
    this.agent = new Agent(
      gateway,
      tools,
      systemPrompt,
      true,
      maxCompactionTokens,
    );
    if ('stored' in init) {
      const {stored} = init;
      this.id = stored.id;
      this.createdAt = stored.createdAt;
      this.model = stored.model;
      this.customTitle = stored.customTitle;
      this.lines = stored.lines.map((l) => ({...l}));
      this.totalTokens = stored.totalTokens;
      this.agent.setHistory(stored.history, stored.totalTokens);
    } else {
      this.id = uuidv4();
      this.createdAt = Date.now();
      this.model = init.model;
      this.lines = [];
    }
  }

  /**
   * A short human-readable title for the conversation: the user-chosen name if
   * set, else the first user message, truncated.
   */
  get title(): string {
    if (this.customTitle !== undefined) return this.customTitle;
    const first = this.lines.find((l) => l.role === 'user')?.text;
    if (first === undefined) return 'New conversation';
    const oneLine = first.replace(/\s+/g, ' ').trim();
    return oneLine.length > 60 ? `${oneLine.slice(0, 60)}…` : oneLine;
  }

  /**
   * Rename the conversation and persist. An empty name reverts to the title
   * derived from the first message.
   */
  rename(name: string): void {
    const trimmed = name.trim();
    this.customTitle = trimmed === '' ? undefined : trimmed;
    this.save();
  }

  /**
   * Persist the conversation to the store. No-op until the conversation has
   * some content, so empty conversations don't clutter the history.
   */
  save(): void {
    if (this.lines.length === 0) return;
    this.store.save({
      id: this.id,
      title: this.title,
      customTitle: this.customTitle,
      createdAt: this.createdAt,
      updatedAt: Date.now(),
      model: this.model,
      lines: this.lines.map((l) => ({...l})),
      history: [...this.agent.getHistory()],
      totalTokens: this.totalTokens,
    });
  }

  /**
   * Submit a user message. If a turn is already running the text is queued as a
   * follow-up; otherwise it starts a new turn (and drains any follow-ups queued
   * while it runs). No-op on empty input.
   */
  async send(message: string, model: ModelPath): Promise<void> {
    const text = message.trim();
    if (text === '') return;

    this.model = model;
    if (this.loading) {
      // Typed mid-turn: hold it in the follow-up queue until the running turn
      // picks it up at its next tool-use round, or until the loop below starts
      // a new turn with it once the current one finishes.
      this.pendingFollowUps.push(text);
      return;
    }

    await this.drainTurns(text, model);
  }

  /**
   * True when the conversation has history worth manually compacting and no
   * turn is currently in flight.
   */
  get canCompact(): boolean {
    return !this.loading && this.agent.canCompact();
  }

  /**
   * Manually summarize the conversation history using `model` so subsequent
   * turns use less of the context window. No-op while a turn is running or
   * when there is nothing to compact.
   */
  async compact(model: ModelPath): Promise<void> {
    if (!this.canCompact) return;

    this.model = model;
    const abort = new AbortController();
    this.abort = abort;
    this.loading = true;
    this.compacting = true;
    m.redraw();

    try {
      for await (const evt of this.agent.compact(model, abort.signal)) {
        switch (evt.type) {
          case 'compacting':
            this.compacting = true;
            break;
          case 'compacted':
            this.compacting = false;
            this.totalTokens = undefined;
            this.lines.push({role: 'compaction', text: evt.summary});
            this.save();
            break;
          case 'error':
            this.compacting = false;
            this.lines.push({
              role: 'error',
              text: evt.message || 'Unknown error',
            });
            break;
        }
        m.redraw();
      }
    } catch (e) {
      if (!abort.signal.aborted) {
        this.lines.push({role: 'error', text: `Something went wrong: ${e}`});
      }
    } finally {
      this.loading = false;
      this.compacting = false;
      this.abort = undefined;
      this.save();
      m.redraw();
    }

    const next = this.pendingFollowUps.shift();
    if (next !== undefined) {
      await this.drainTurns(next, model);
    }
  }

  /**
   * True when the most recent turn ended in an outright request failure (not
   * a user cancellation) and the agent history can re-send that request.
   */
  get canRetry(): boolean {
    if (this.loading || !this.agent.canRetry()) return false;
    for (let i = this.lines.length - 1; i >= 0; i--) {
      const line = this.lines[i];
      if (line === undefined || line.role === 'spacer') continue;
      return line.role === 'error' && line.text !== CANCELLED_TEXT;
    }
    return false;
  }

  /**
   * Retry the latest failed turn without re-adding the user message. Drops the
   * trailing error banner before re-sending. No-op if nothing failed or a turn
   * is already running.
   */
  async retry(model: ModelPath): Promise<void> {
    if (!this.canRetry) return;

    while (this.lines[this.lines.length - 1]?.role === 'spacer') {
      this.lines.pop();
    }
    if (this.lines[this.lines.length - 1]?.role === 'error') {
      this.lines.pop();
    }
    this.model = model;
    this.save();

    await this.drainTurns(undefined, model);
  }

  private async drainTurns(
    firstPrompt: string | undefined,
    model: ModelPath,
  ): Promise<void> {
    await this.runTurn(firstPrompt, model);
    let next = this.pendingFollowUps.shift();
    while (next !== undefined) {
      await this.runTurn(next, model);
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
    this.lines.push({role: 'error', text: CANCELLED_TEXT});
  }

  private getContext(): ContextRegistry {
    return typeof this.context === 'function' ? this.context() : this.context;
  }

  private appendAi(text: string) {
    const last = this.lines[this.lines.length - 1];
    if (last?.role === 'ai') {
      last.text += text;
    } else {
      this.lines.push({role: 'ai', text});
    }
  }

  private async runTurn(
    text: string | undefined,
    model: ModelPath,
  ): Promise<void> {
    const dequeueFollowUp = (): string | undefined => {
      const followUp = this.pendingFollowUps.shift();
      if (followUp !== undefined) {
        this.lines.push({role: 'user', text: followUp});
        this.save();
        m.redraw();
      }
      return followUp;
    };

    let prompt: string | undefined;
    if (text !== undefined) {
      this.lines.push({role: 'user', text});
      this.save();

      // Fold in any non-excluded context as a preamble the model can see.
      const context = this.getContext()
        .buildContextItems()
        .filter((c) => !this.excludedContext.has(c.id));
      prompt =
        context.length === 0
          ? text
          : `Context for this question:\n${context
              .map((c) => `- ${c.label}: ${c.payload}`)
              .join('\n')}\n\nQuestion: ${text}`;
    }

    const abort = new AbortController();
    this.abort = abort;
    this.loading = true;
    m.redraw();

    try {
      const stream: AsyncGenerator<AgentEvent, void, void> =
        prompt === undefined
          ? this.agent.retry(model, abort.signal, dequeueFollowUp)
          : this.agent.sendMessage(
              prompt,
              model,
              abort.signal,
              dequeueFollowUp,
            );
      for await (const evt of stream) {
        if (evt.type !== 'compacting') {
          this.compacting = false;
        }
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
              const line = this.lines[i]!;
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
          case 'compacting':
            this.compacting = true;
            break;
          case 'compacted':
            this.totalTokens = undefined;
            this.lines.push({role: 'compaction', text: evt.summary});
            this.save();
            break;
          case 'error':
            this.lines.push({
              role: 'error',
              text: evt.message || 'Unknown error',
            });
            break;
        }
        m.redraw();
      }
      this.lines.push({role: 'spacer', text: ''});
    } catch (e) {
      if (!abort.signal.aborted) {
        this.lines.push({role: 'error', text: `Something went wrong: ${e}`});
      }
    } finally {
      this.loading = false;
      this.compacting = false;
      this.abort = undefined;
      this.save();
      m.redraw();
    }
  }
}
