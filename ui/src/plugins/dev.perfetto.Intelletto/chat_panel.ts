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
// assistant. Renders the transcript, the input box, the model picker, a cancel
// control for in-flight turns, queued follow-ups, and a context strip showing
// what the next prompt will carry. All state and side effects are passed in via
// ChatPanelAttrs; this component is a pure view.

import markdownit from 'markdown-it';
import m from 'mithril';
import {classNames} from '../../base/classnames';
import {Icons} from '../../base/semantic_icons';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {EmptyState} from '../../widgets/empty_state';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {PopupPosition} from '../../widgets/popup';
import {Spinner} from '../../widgets/spinner';
import type {ModelDetails, ModelPath} from '../dev.perfetto.Llm/gateway';
import './chat_panel.scss';
import type {ChatLine} from './chat_session';
import type {ContextItem} from './context';
import {renderMarkdown} from './markdown';
import {PromptBox} from './prompt_box';

// Summary of a conversation for the conversation list/switcher.
export interface ConversationSummary {
  readonly id: string;
  readonly title: string;
}

// Attrs for the ChatPanel component.
export interface ChatPanelAttrs {
  // Status
  readonly isAgentRunning: boolean;
  readonly isLoadingSkills: boolean;
  readonly totalTokens?: number;
  readonly onCancel: () => void;
  // Provided when the trailing error line can be retried in place.
  readonly onRetry?: () => void;

  // Conversation state
  readonly conversation: readonly ChatLine[];
  readonly onSendMessage: (msg: string) => void;
  readonly conversations: readonly ConversationSummary[];
  readonly activeConversationId?: string;
  readonly onOpenConversation: (id: string) => void;
  readonly onNewConversation: () => void;
  readonly onRenameConversation?: (name: string) => void;
  readonly onDeleteConversation?: () => void;

  // Pending message control
  readonly pendingMessages: readonly string[];
  readonly onDeletePendingMsg: (index: number) => void;
  readonly onSendPendingMsgNow: (index: number) => void;

  // Context strip
  readonly contextItems: readonly ContextItem[];
  readonly excludedContextIds: ReadonlySet<string>;
  readonly onToggleContext: (id: string) => void;

  // Model selector
  readonly models: readonly ModelDetails[];
  readonly selectedModel: ModelPath | undefined;
  readonly onSelectModel: (model: ModelPath) => void;
  readonly configureModelsHref?: string;
}

/**
 * The Intelletto chat panel: a stateless view over conversation state passed in
 * via ChatPanelAttrs. Renders the transcript, prompt box, queued follow-ups,
 * and context strip.
 */
export class ChatPanel implements m.ClassComponent<ChatPanelAttrs> {
  // Explicitly disable raw HTML and external image rendering so untrusted
  // model output cannot inject markup or trigger outbound image requests.
  private readonly md = markdownit({html: false}).disable('image');
  // Whether the conversation was scrolled to (near) the bottom as of the last
  // render. We only auto-scroll on update when this holds, so new content keeps
  // the view pinned to the bottom but a user who has scrolled up to read is
  // left alone. Sampled before Mithril patches the DOM (onbeforeupdate).
  private stickToBottom = true;

  view({attrs}: m.CVnode<ChatPanelAttrs>): m.Children {
    const lastNonSpacerIdx = findLastNonSpacerIndex(attrs.conversation);

    return m(
      '.pf-intelletto',
      {
        // Clicking anywhere in the panel focuses the text input, unless the
        // click landed on an interactive element or the user is selecting
        // text (e.g. to copy part of the transcript).
        onclick: (e: MouseEvent) => {
          const target = e.target as HTMLElement;
          if (target.closest('input, textarea, select, button, a, summary')) {
            return;
          }
          const selection = window.getSelection();
          if (selection !== null && !selection.isCollapsed) return;
          const root = e.currentTarget as HTMLElement;
          root
            .querySelector<HTMLTextAreaElement>(
              '.pf-intelletto__input textarea',
            )
            ?.focus();
        },
      },
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
        attrs.conversation.length === 0
          ? m(EmptyState, {
              fillHeight: true,
              icon: 'psychology',
              title: 'Ask about your trace',
            })
          : attrs.conversation.map((msg, i) => {
              if (msg.text === '' && msg.role !== 'spacer') return null;
              if (msg.role === 'toolcall') return this.renderToolCall(msg);
              const labels: Record<ChatLine['role'], string> = {
                ai: 'Intelletto',
                user: 'You',
                error: 'Error',
                toolcall: 'Tool',
                thought: 'Thought',
                spacer: '',
              };
              const showRetry =
                attrs.onRetry !== undefined &&
                msg.role === 'error' &&
                i === lastNonSpacerIdx;
              return m(
                `.pf-intelletto__msg.pf-intelletto__msg--${msg.role}`,
                m('b.pf-intelletto__msg-role', labels[msg.role]),
                m(
                  'span.pf-intelletto__msg-text',
                  renderMarkdown(this.md, msg.text),
                ),
                showRetry &&
                  m(
                    '.pf-intelletto__retry',
                    m(Button, {
                      icon: 'refresh',
                      label: 'Retry',
                      compact: true,
                      variant: ButtonVariant.Outlined,
                      intent: Intent.Danger,
                      onclick: () => attrs.onRetry?.(),
                    }),
                  ),
              );
            }),
        (attrs.isAgentRunning || attrs.isLoadingSkills) &&
          m(
            '.pf-intelletto__working',
            m(Spinner, {easing: true}),
            attrs.isAgentRunning ? 'Working...' : 'Loading skills...',
          ),
      ),
      m(
        '.pf-intelletto__footer',
        this.renderQueuedFollowUps(attrs),
        this.renderContextStrip(attrs),
        m(
          'footer.pf-intelletto__input',
          m(PromptBox, {
            isAgentRunning: attrs.isAgentRunning,
            totalTokens: attrs.totalTokens,
            models: attrs.models,
            selectedModel: attrs.selectedModel,
            onSelectModel: attrs.onSelectModel,
            configureModelsHref: attrs.configureModelsHref,
            onSendMessage: attrs.onSendMessage,
            onCancel: attrs.onCancel,
          }),
        ),
      ),
    );
  }

  private renderHeader(attrs: ChatPanelAttrs): m.Children {
    const activeConversation = attrs.conversations.find(
      (c) => c.id === attrs.activeConversationId,
    );

    return m(
      '.pf-intelletto__header',
      attrs.conversations.length > 0 &&
        m(
          PopupMenu,
          {
            position: PopupPosition.BottomStart,
            showArrow: false,
            offset: 4,
            trigger: m(Button, {
              className: 'pf-intelletto__conversations',
              icon: 'history',
              label: activeConversation?.title ?? 'Conversations',
              rightIcon: 'keyboard_arrow_down',
              variant: ButtonVariant.Minimal,
              disabled: attrs.isAgentRunning,
              shrink: true,
            }),
          },
          attrs.conversations.map((conv) => {
            const isActive = conv.id === attrs.activeConversationId;
            return m(MenuItem, {
              label: conv.title,
              rightIcon: isActive ? 'check' : undefined,
              active: isActive,
              onclick: () => attrs.onOpenConversation(conv.id),
            });
          }),
        ),
      m('span.pf-intelletto__header-spacer'),
      m(Button, {
        icon: 'edit',
        title: 'Rename conversation',
        disabled:
          attrs.isAgentRunning || attrs.onRenameConversation === undefined,
        onclick: () => {
          const name = window.prompt(
            'Rename conversation',
            activeConversation?.title ?? '',
          );
          if (name !== null) attrs.onRenameConversation?.(name);
        },
      }),
      m(Button, {
        icon: 'delete',
        title: 'Delete conversation',
        disabled:
          attrs.isAgentRunning || attrs.onDeleteConversation === undefined,
        onclick: () => {
          const title = activeConversation?.title ?? 'this conversation';
          const ok = window.confirm(
            `Delete the conversation "${title}"? This can't be undone.`,
          );
          if (ok) attrs.onDeleteConversation?.();
        },
      }),
      m(Button, {
        icon: 'add_comment',
        title: 'New conversation',
        onclick: () => attrs.onNewConversation(),
      }),
    );
  }

  private renderQueuedFollowUps(attrs: ChatPanelAttrs): m.Children {
    const queued = attrs.pendingMessages;
    if (queued.length === 0) return null;
    return m(
      '.pf-intelletto__queued',
      queued.map((text, idx) =>
        m(
          '.pf-intelletto__queued-item',
          {key: idx},
          m('span.pf-intelletto__queued-label', 'Queued'),
          m('span.pf-intelletto__queued-text', text),
          m(Button, {
            className: 'pf-intelletto__queued-action',
            icon: 'send',
            title: 'Send now',
            rounded: true,
            onclick: () => attrs.onSendPendingMsgNow(idx),
          }),
          m(Button, {
            className: 'pf-intelletto__queued-remove',
            icon: Icons.Close,
            title: 'Remove queued follow-up',
            rounded: true,
            onclick: () => attrs.onDeletePendingMsg(idx),
          }),
        ),
      ),
    );
  }

  private renderContextStrip(attrs: ChatPanelAttrs): m.Children {
    const {contextItems, excludedContextIds, onToggleContext} = attrs;
    if (contextItems.length === 0) return null;
    return m(
      '.pf-intelletto__context',
      contextItems.map((c: ContextItem) => {
        const excluded = excludedContextIds.has(c.id);
        return m(
          'span.pf-intelletto__context-chip',
          {
            className: classNames(
              excluded && 'pf-intelletto__context-chip--off',
            ),
            title: c.payload,
            onclick: () => onToggleContext(c.id),
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
    const badge = pending ? '…' : msg.toolError === true ? '✗' : '✓';
    const argsStr = msg.toolArgs === undefined ? '' : prettyJson(msg.toolArgs);
    const result = msg.toolResult ?? '';

    return m(
      '.pf-intelletto__msg.pf-intelletto__msg--toolcall',
      m(
        'details.pf-intelletto__tool',
        {
          className: classNames(
            msg.toolError === true && 'pf-intelletto__tool--error',
          ),
        },
        m(
          'summary.pf-intelletto__tool-summary',
          m('span.pf-intelletto__tool-badge', badge),
          m('code', msg.text),
        ),
        argsStr !== '' &&
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
}

function prettyJson(value: unknown): string {
  try {
    return JSON.stringify(value, null, 2);
  } catch {
    return String(value);
  }
}

function findLastNonSpacerIndex(lines: readonly ChatLine[]): number {
  for (let i = lines.length - 1; i >= 0; i--) {
    if (lines[i]?.role !== 'spacer') return i;
  }
  return -1;
}

function truncate(s: string, max: number): string {
  return s.length <= max
    ? s
    : `${s.slice(0, max)}\n… (${s.length - max} more chars)`;
}
