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
import type {
  LlmGateway,
  ModelDetails,
  ModelPath,
} from '../../../../ui/src/plugins/dev.perfetto.Llm/gateway';
import type {
  Message,
  ToolCall,
  ToolResult,
} from '../../../../ui/src/plugins/dev.perfetto.Llm/protocol';
import {DeclarativeToolRunner} from './tool_runner';
import {
  EvidenceStore,
  type AgentContextSnapshot,
  type HandoffAction,
  type InvestigationNote,
  type ProductAgentAdapter,
} from './types';

const MAX_ITERATIONS = 20;
const MAX_TOOL_OUTPUT_CHARS = 18_000;

export interface PendingApproval {
  readonly title: string;
  readonly command: string;
  readonly resolve: (approved: boolean) => void;
}

export type TranscriptItem =
  | {readonly kind: 'user'; readonly text: string}
  | {readonly kind: 'ai'; text: string}
  | {readonly kind: 'thought'; text: string}
  | {readonly kind: 'error'; readonly text: string}
  | {readonly kind: 'handoff'; readonly action: HandoffAction}
  | {
      readonly kind: 'tool';
      readonly name: string;
      readonly args: Record<string, unknown>;
      result?: string;
      evidenceId?: string;
      isError?: boolean;
      done: boolean;
    };

export function resolveActiveModel(
  preferred: ModelPath | undefined,
  gateway: LlmGateway,
): ModelPath | undefined {
  const all = gateway.listModels();
  if (all.length === 0) return undefined;
  const agentic = all.filter((md) => md.model.roles.includes('agentic'));
  const pool: ReadonlyArray<ModelDetails> = agentic.length > 0 ? agentic : all;
  if (preferred !== undefined) {
    const found = pool.find(
      (md) =>
        md.provider.id === preferred.providerId &&
        md.model.id === preferred.modelId,
    );
    if (found !== undefined) return preferred;
  }
  const first = pool[0]!;
  return {providerId: first.provider.id, modelId: first.model.id};
}

function formatContextHeader(ctx: AgentContextSnapshot): string {
  const parts = [
    `mode=${ctx.mode}`,
    `scope="${ctx.label}"`,
    ctx.subtab ? `tab=${ctx.subtab}` : undefined,
    ctx.deviceName ? `device="${ctx.deviceName}"` : undefined,
    ctx.focusProcess ? `process="${ctx.focusProcess}"` : undefined,
    ctx.window ? `window="${ctx.window.label}"` : undefined,
  ].filter(Boolean);
  let header = `[Active UI context: ${parts.join(', ')}]`;
  if (ctx.extraContext) header += `\n${ctx.extraContext}`;
  return `${header}\n\n`;
}

function serializeOutput(out: unknown): string {
  const raw = typeof out === 'string' ? out : JSON.stringify(out, null, 2);
  if (raw.length <= MAX_TOOL_OUTPUT_CHARS) return raw;
  return (
    raw.slice(0, MAX_TOOL_OUTPUT_CHARS) +
    `\n... [truncated ${raw.length - MAX_TOOL_OUTPUT_CHARS} chars]`
  );
}

export class ProductAgentSession {
  readonly evidence = new EvidenceStore();
  readonly notes: InvestigationNote[] = [];
  readonly items: TranscriptItem[] = [];
  readonly runner: DeclarativeToolRunner;

  input = '';
  loading = false;
  totalTokens?: number;
  selectedModel?: ModelPath;
  activeTab: 'chat' | 'tools' = 'chat';
  pendingApproval?: PendingApproval;

  private history: Message[] = [];
  private abortController?: AbortController;

  constructor(
    readonly gateway: LlmGateway,
    readonly adapter: ProductAgentAdapter,
  ) {
    this.runner = new DeclarativeToolRunner(adapter);
  }

  reset(): void {
    this.cancel();
    this.history = [];
    this.items.length = 0;
    this.evidence.clear();
    this.notes.length = 0;
    this.totalTokens = undefined;
    this.pendingApproval = undefined;
    m.redraw();
  }

  cancel(): void {
    if (this.pendingApproval) {
      this.pendingApproval.resolve(false);
      this.pendingApproval = undefined;
    }
    this.abortController?.abort();
    this.abortController = undefined;
    this.loading = false;
    m.redraw();
  }

  async send(promptOverride?: string): Promise<void> {
    const userText = (promptOverride ?? this.input).trim();
    if (!userText || this.loading) return;
    if (promptOverride === undefined) this.input = '';

    const model = resolveActiveModel(this.selectedModel, this.gateway);
    if (!model) {
      this.items.push({
        kind: 'error',
        text: 'No LLM provider configured yet. Start a local OpenAI-compatible server on http://localhost:8080/v1 or add a Gemini / OpenAI key in Settings → LLM providers.',
      });
      m.redraw();
      return;
    }
    this.selectedModel = model;

    const initialCtx = this.adapter.getContext();
    this.items.push({kind: 'user', text: userText});
    this.history.push({
      role: 'user',
      text: `${formatContextHeader(initialCtx)}${userText}`,
    });

    this.loading = true;
    const ac = new AbortController();
    this.abortController = ac;
    m.redraw();

    try {
      for (let iter = 0; iter < MAX_ITERATIONS; iter++) {
        if (ac.signal.aborted) return;
        const ctx = this.adapter.getContext();
        const tools = this.runner.listLlmToolDefs(ctx);
        const systemPrompt = this.adapter.getSystemPrompt(ctx);

        const calls: ToolCall[] = [];
        let turnText = '';
        let aiItem: {readonly kind: 'ai'; text: string} | undefined;
        let thoughtItem: {readonly kind: 'thought'; text: string} | undefined;
        let backendError: string | undefined;

        for await (const ev of this.gateway.createStream(
          model,
          {systemPrompt, messages: this.history, tools},
          ac.signal,
        )) {
          if (ac.signal.aborted) return;
          switch (ev.type) {
            case 'text':
              turnText += ev.text;
              if (!aiItem) {
                aiItem = {kind: 'ai', text: ev.text};
                this.items.push(aiItem);
              } else {
                aiItem.text += ev.text;
              }
              m.redraw();
              break;
            case 'thought':
              if (!thoughtItem) {
                thoughtItem = {kind: 'thought', text: ev.text};
                this.items.push(thoughtItem);
              } else {
                thoughtItem.text += ev.text;
              }
              m.redraw();
              break;
            case 'tool-call':
              calls.push(ev.call);
              break;
            case 'usage':
              if (ev.usage.totalTokens !== undefined) {
                this.totalTokens = (this.totalTokens ?? 0) + ev.usage.totalTokens;
              }
              break;
            case 'stop':
              if (ev.reason === 'error' && ev.error) {
                backendError = `${ev.error.kind}: ${ev.error.message}`;
              }
              break;
          }
        }

        if (backendError) {
          this.items.push({kind: 'error', text: backendError});
          break;
        }
        if (turnText) {
          this.history.push({role: 'model', text: turnText});
        }
        if (calls.length === 0) break;

        this.history.push({role: 'tool-call', calls});
        const results: ToolResult[] = [];

        for (const call of calls) {
          if (ac.signal.aborted) return;
          const item: Extract<TranscriptItem, {kind: 'tool'}> = {
            kind: 'tool',
            name: call.name,
            args: call.args,
            done: false,
          };
          this.items.push(item);
          m.redraw();

          const liveCtx = this.adapter.getContext();
          const res = await this.runner.execute(
            call.name,
            call.args,
            liveCtx,
            this.evidence,
            this.notes,
            ({title, command}) =>
              new Promise<boolean>((resolve) => {
                this.pendingApproval = {
                  title,
                  command,
                  resolve: (approved) => {
                    this.pendingApproval = undefined;
                    m.redraw();
                    resolve(approved);
                  },
                };
                m.redraw();
              }),
          );
          const serialized = serializeOutput(res.output);
          item.result = serialized;
          item.evidenceId = res.evidenceId;
          item.isError = res.isError;
          item.done = true;
          if (res.handoff) {
            this.items.push({kind: 'handoff', action: res.handoff});
          }
          results.push({
            id: call.id,
            name: call.name,
            result: serialized,
            isError: res.isError,
          });
          m.redraw();
        }

        this.history.push({role: 'tool-result', results});
      }
    } catch (e) {
      if (!ac.signal.aborted) {
        this.items.push({
          kind: 'error',
          text: e instanceof Error ? e.message : String(e),
        });
      }
    } finally {
      this.loading = false;
      this.abortController = undefined;
      m.redraw();
    }
  }
}
