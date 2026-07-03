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

// The Intelletto chat panel - the sidebar surface for the conversational
// assistant. Renders the transcript, the input box, the active-model indicator
// (header), a cancel control for in-flight turns, and a context strip showing
// what the next prompt will carry. All conversation state lives in the
// ChatSession (owned by IntellettoPlugin, so it survives the panel unmounting);
// this component is a pure view over it.

import markdownit from 'markdown-it';
import m from 'mithril';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {EmptyState} from '../../widgets/empty_state';
import {showModal} from '../../widgets/modal';
import {TextInput} from '../../widgets/text_input';
import type {LlmGateway, ModelDetails} from '../dev.perfetto.Llm/gateway';
import './chat_panel.scss';
import type {ChatLine, ChatSession} from './chat_session';
import type {ContextItem, ContextRegistry} from './context';

export interface ChatPanelAttrs {
  readonly gateway: LlmGateway;
  // The trace-scoped conversation state, owned by IntellettoPlugin.
  readonly session: ChatSession;
  // The shared, trace-scoped context-provider registry (core providers + those
  // contributed by other plugins). Sampled live for the context strip.
  readonly context: ContextRegistry;
}

export class ChatPanel implements m.ClassComponent<ChatPanelAttrs> {
  private readonly md = markdownit();
  // Whether the conversation was scrolled to (near) the bottom as of the last
  // render. We only auto-scroll on update when this holds, so new content keeps
  // the view pinned to the bottom but a user who has scrolled up to read is
  // left alone. Sampled before Mithril patches the DOM (onbeforeupdate).
  private stickToBottom = true;

  private renderHeader(attrs: ChatPanelAttrs): m.Children {
    const {gateway, session} = attrs;
    // The model the agent will drive: the first configured model advertising
    // the 'agentic' role (matches Agent.sendMessage).
    const active = gateway
      .listModels()
      .find((rm) => rm.model.roles.includes('agentic'));

    return m(
      '.pf-intelletto__header',
      active === undefined
        ? m('span.pf-intelletto__no-model', 'No agentic model configured')
        : m('span.pf-intelletto__model', modelName(active)),
      m(Button, {
        icon: 'manage_search',
        title: 'Show system prompt',
        onclick: () => this.showSystemPrompt(session),
      }),
      m(Button, {
        icon: 'add_comment',
        title: 'New conversation',
        onclick: session.newConversation,
      }),
    );
  }

  // Transparency hatch: show the exact system prompt the agent sends, in a
  // modal. Like the context strip's expand-to-raw-payload, no hidden context.
  private showSystemPrompt(session: ChatSession) {
    showModal({
      title: 'System prompt',
      content: m('pre.pf-intelletto__system-prompt', session.systemPrompt),
      buttons: [{text: 'Close', primary: true}],
    });
  }

  private renderContextStrip(attrs: ChatPanelAttrs): m.Children {
    const {context, session} = attrs;
    const items = context.buildContextItems();
    return m(
      '.pf-intelletto__context',
      items.map((c: ContextItem) => {
        const excluded = session.excludedContext.has(c.id);
        return m(
          'span.pf-intelletto__context-chip',
          {
            class: excluded ? 'pf-intelletto__context-chip--off' : '',
            title: c.payload,
            onclick: () => {
              if (excluded) session.excludedContext.delete(c.id);
              else session.excludedContext.add(c.id);
            },
          },
          `${excluded ? '☐' : '☑'} ${c.label}`,
        );
      }),
    );
  }

  // A tool call rendered as a collapsible summary: a one-line header (tool
  // name + ok/error badge) that expands to show the args sent and the result
  // returned. Defaults to collapsed so the transcript stays readable, but the
  // full result is always one click away.
  private renderToolCall(msg: ChatLine): m.Children {
    const pending = msg.toolResult === undefined;
    const badge = pending ? '…' : msg.toolError ? '✗' : '✓';
    const argsStr = msg.toolArgs === undefined ? '' : prettyJson(msg.toolArgs);
    const result = msg.toolResult ?? '';

    return m(
      '.pf-intelletto__msg.pf-intelletto__msg--toolcall',
      m(
        'details.pf-intelletto__tool',
        {class: msg.toolError ? 'pf-intelletto__tool--error' : ''},
        m(
          'summary.pf-intelletto__tool-summary',
          m('span.pf-intelletto__tool-badge', badge),
          m('code', msg.text),
        ),
        argsStr &&
          m('.pf-intelletto__tool-section', [
            m('.pf-intelletto__tool-label', 'args'),
            m('pre.pf-intelletto__tool-body', argsStr),
          ]),
        !pending &&
          m('.pf-intelletto__tool-section', [
            m('.pf-intelletto__tool-label', 'result'),
            m('pre.pf-intelletto__tool-body', truncate(result, 4000)),
          ]),
      ),
    );
  }

  view({attrs}: m.CVnode<ChatPanelAttrs>): m.Children {
    const {session} = attrs;
    return m(
      '.pf-intelletto',
      this.renderHeader(attrs),
      m(
        '.pf-intelletto__conversation',
        {
          oncreate: (vnode: m.VnodeDOM) => {
            const el = vnode.dom as HTMLElement;
            el.scrollTop = el.scrollHeight;
          },
          // Sample the scroll position *before* the DOM is patched: if the user
          // is at the bottom we keep them pinned, otherwise we leave their
          // scroll position untouched so they can read/select scrollback.
          // NB: read `old.dom`, not `vnode.dom` - Mithril only copies the DOM
          // ref onto the new vnode *after* onbeforeupdate runs, so `vnode.dom`
          // is undefined here (and measuring it would leave stickToBottom stuck
          // at its initial true, auto-scrolling every update).
          onbeforeupdate: (_vnode: m.VnodeDOM, old: m.VnodeDOM) => {
            const el = old.dom as HTMLElement | undefined;
            if (el !== undefined) {
              const distFromBottom =
                el.scrollHeight - el.scrollTop - el.clientHeight;
              this.stickToBottom = distFromBottom < 8;
            }
            return true;
          },
          onupdate: (vnode: m.VnodeDOM) => {
            if (!this.stickToBottom) return;
            const el = vnode.dom as HTMLElement;
            el.scrollTop = el.scrollHeight;
          },
        },
        session.lines.length === 0
          ? m(EmptyState, {
              fillHeight: true,
              icon: 'psychology',
              title: 'Ask about your trace',
            })
          : session.lines.map((msg) => {
              if (!msg.text && msg.role !== 'spacer') return null;
              if (msg.role === 'toolcall') return this.renderToolCall(msg);
              const labels: Record<ChatLine['role'], string> = {
                ai: 'Intelletto',
                user: 'You',
                error: 'Error',
                toolcall: 'Tool',
                thought: 'Thought',
                spacer: '',
              };
              return m(
                `.pf-intelletto__msg.pf-intelletto__msg--${msg.role}`,
                m('b.pf-intelletto__msg-role', labels[msg.role]),
                m(
                  'span.pf-intelletto__msg-text',
                  m.trust(this.md.render(msg.text)),
                ),
              );
            }),
      ),
      this.renderContextStrip(attrs),
      m(
        'footer.pf-intelletto__input',
        m(TextInput, {
          value: session.input,
          oninput: (e: Event) => {
            session.input = (e.target as HTMLInputElement).value;
          },
          onkeydown: (e: KeyboardEvent) => {
            if (e.key === 'Enter' && !e.shiftKey) {
              e.preventDefault();
              session.send();
            }
          },
          placeholder: session.loading
            ? 'Queue a follow-up...'
            : 'Ask about your trace...',
        }),
        session.totalTokens !== undefined
          ? m(
              '.pf-intelletto__tokens',
              `${(session.totalTokens / 1000).toFixed(1)} ktok`,
            )
          : null,
        session.loading
          ? m(Button, {
              icon: 'stop',
              title: 'Cancel',
              onclick: session.cancel,
              intent: Intent.Danger,
            })
          : m(Button, {
              icon: 'arrow_forward',
              title: 'Send',
              onclick: () => session.send(),
              variant: ButtonVariant.Filled,
              intent: Intent.Primary,
            }),
      ),
    );
  }
}

function modelName(model: ModelDetails) {
  const protocolName = model.provider.label || model.provider.protocolName;
  const modelName = model.model.label || model.model.modelName;
  return `${protocolName}/${modelName}`;
}

function prettyJson(value: unknown): string {
  try {
    return JSON.stringify(value, null, 2);
  } catch {
    return String(value);
  }
}

function truncate(s: string, max: number): string {
  return s.length <= max
    ? s
    : `${s.slice(0, max)}\n… (${s.length - max} more chars)`;
}
