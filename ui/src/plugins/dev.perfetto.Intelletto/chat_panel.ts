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
import {classNames} from '../../base/classnames';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {EmptyState} from '../../widgets/empty_state';
import {MenuDivider, MenuItem, MenuTitle, PopupMenu} from '../../widgets/menu';
import {Popup, PopupPosition} from '../../widgets/popup';
import type {
  LlmGateway,
  ModelDetails,
  ModelPath,
} from '../dev.perfetto.Llm/gateway';
import './chat_panel.scss';
import type {ChatLine, ChatSession} from './chat_session';
import type {ContextItem, ContextRegistry} from './context';
import type {StoredConversation} from './conversation_store';
import {renderMarkdown} from './markdown';
import {maybeUndefined} from '../../base/utils';
import type {Setting} from '../../public/settings';
import {assertExists} from '../../base/assert';
import {Spinner} from '../../widgets/spinner';

// Attrs for the ChatPanel component.
export interface ChatPanelAttrs {
  readonly gateway: LlmGateway;
  // The trace-scoped conversation state, owned by IntellettoPlugin.
  readonly session: ChatSession;
  // The shared context-provider registry, sampled live for the strip.
  readonly context: ContextRegistry;
  // The setting that controls the active model. Updated directly when
  // the user picks a different model from the select box. If the selected
  // model is unavailable or undefined, we fall back to the first agentic
  // model. If no agentic models are registered, messages cannot be sent.
  readonly selectedModelSetting: Setting<ModelPath | undefined>;
  // Past conversations available to switch to, most recent first.
  readonly conversations: readonly StoredConversation[];
  // Callback for when the user requests a new session.
  readonly onNewSession: () => void;
  // Callback for when the user picks a past conversation from the dropdown.
  readonly onSelectConversation: (id: string) => void;
  // Callback for when the user deletes the current conversation.
  readonly onDeleteConversation: (id: string) => void;
}

// Given a currently selected model path and a gateway to fetch the list of all
// registered models, return either the currently selected model after
// valididating it actually exists, OR the first agentic model otherwise.
function getValidModel(
  model: ModelPath | undefined,
  gateway: LlmGateway,
): ModelPath | undefined {
  const agenticModels = gateway
    .listModels()
    .filter((m) => m.model.roles.includes('agentic'));

  if (model) {
    // Model is defined, see if it's a real model in the list of registered
    // agentic models.
    const foundModel = agenticModels.find(
      (m) => m.model.id === model.modelId && m.provider.id === model.providerId,
    );
    if (foundModel) {
      return model;
    }
  }

  const firstModel = maybeUndefined(agenticModels[0]);
  if (firstModel) {
    return {providerId: firstModel.provider.id, modelId: firstModel.model.id};
  }

  return undefined;
}

// Resize a textarea's height to fit its content.
function autoResize(el: HTMLTextAreaElement) {
  el.style.height = 'auto';
  el.style.height = `${el.scrollHeight}px`;
}

/**
 * The Intelletto chat panel: a pure view over a ChatSession. Renders the
 * transcript, input box, model header, and context strip.
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
  // Whether the conversation picker popup is open.
  private conversationsOpen = false;

  view({attrs}: m.CVnode<ChatPanelAttrs>): m.Children {
    const {session, selectedModelSetting, gateway} = attrs;
    const agenticModels = gateway
      .listModels()
      .filter((rm) => rm.model.roles.includes('agentic'));
    // The conversation's own model takes precedence; fresh conversations fall
    // back to the last-selected model.
    const currentModel = getValidModel(
      session.model ?? selectedModelSetting.get(),
      gateway,
    );
    const onModelChange = (model: ModelPath) => {
      session.model = model;
      selectedModelSetting.set(model);
      session.save();
    };

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
      this.renderConversationPicker(attrs),
      agenticModels.length === 0 &&
        m(
          '.pf-intelletto__header',
          m('span.pf-intelletto__no-model', 'No agentic model configured'),
        ),
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
                  renderMarkdown(this.md, msg.text),
                ),
              );
            }),
        session.loading &&
          m(
            '.pf-intelletto__working',
            m(Spinner, {easing: true}),
            'Working...',
          ),
      ),
      m(
        '.pf-intelletto__footer',
        this.renderQueuedFollowUps(session),
        this.renderContextStrip(attrs),
        currentModel &&
          m(
            'footer.pf-intelletto__input',
            m(
              '.pf-intelletto__prompt-box',
              m('textarea.pf-intelletto__textarea', {
                rows: 2,
                value: session.input,
                // Grow the textarea to fit its content (capped by max-height in
                // CSS). Done on create/update too so it shrinks back after send.
                oncreate: (vnode: m.VnodeDOM) =>
                  autoResize(vnode.dom as HTMLTextAreaElement),
                onupdate: (vnode: m.VnodeDOM) =>
                  autoResize(vnode.dom as HTMLTextAreaElement),
                oninput: (e: Event) => {
                  const el = e.target as HTMLTextAreaElement;
                  session.input = el.value;
                  autoResize(el);
                },
                onkeydown: (e: KeyboardEvent) => {
                  if (e.key === 'Enter' && !e.shiftKey) {
                    e.preventDefault();
                    session.send(currentModel);
                  }
                },
                placeholder: session.loading
                  ? 'Queue a follow-up...'
                  : 'Ask about your trace...',
              }),
              m(
                '.pf-intelletto__prompt-toolbar',
                session.totalTokens !== undefined
                  ? m(
                      '.pf-intelletto__tokens',
                      `${(session.totalTokens / 1000).toFixed(1)} ktok`,
                    )
                  : null,
                this.renderModelPicker(
                  agenticModels,
                  currentModel,
                  onModelChange,
                ),
                session.loading
                  ? m(Button, {
                      icon: 'stop',
                      title: 'Cancel',
                      onclick: () => session.cancel(),
                      variant: ButtonVariant.Filled,
                      rounded: true,
                      intent: Intent.Danger,
                    })
                  : m(Button, {
                      icon: 'send',
                      title: 'Send',
                      disabled: session.input.trim().length === 0,
                      onclick: () => session.send(currentModel),
                      variant: ButtonVariant.Filled,
                      rounded: true,
                      intent: Intent.Primary,
                    }),
              ),
            ),
          ),
      ),
    );
  }

  // The dropdown popup of past conversations (with inline delete buttons),
  // plus rename and new-conversation buttons. The current conversation is
  // always listed, even before it has been persisted (i.e. while still empty).
  // Disabled while a turn is in flight so the stream isn't orphaned.
  private renderConversationPicker(attrs: ChatPanelAttrs): m.Children {
    const {
      session,
      conversations,
      onNewSession,
      onSelectConversation,
      onDeleteConversation,
    } = attrs;
    const isStored = conversations.some((c) => c.id === session.id);
    const options: ReadonlyArray<{
      readonly id: string;
      readonly title: string;
      readonly updatedAt?: number;
      readonly stored: boolean;
    }> = [
      ...(isStored
        ? []
        : [{id: session.id, title: session.title, stored: false}]),
      ...conversations.map((c) => ({
        id: c.id,
        title: c.title,
        updatedAt: c.updatedAt,
        stored: true,
      })),
    ];

    return m(
      '.pf-intelletto__conversations',
      m(
        Popup,
        {
          position: PopupPosition.BottomStart,
          showArrow: false,
          matchWidth: true,
          isOpen: this.conversationsOpen && !session.loading,
          onChange: (open) => {
            this.conversationsOpen = open;
          },
          trigger: m(Button, {
            className: 'pf-intelletto__conversation-trigger',
            label: session.title,
            rightIcon: 'arrow_drop_down',
            shrink: true,
            active: this.conversationsOpen && !session.loading,
            disabled: session.loading,
            onclick: () => {
              this.conversationsOpen = !this.conversationsOpen;
            },
          }),
        },
        m(
          '.pf-intelletto__conversation-list',
          options.map((c) =>
            m(
              '.pf-intelletto__conversation-item',
              {
                key: c.id,
                className: classNames(
                  c.id === session.id &&
                    'pf-intelletto__conversation-item--active',
                ),
                onclick: () => {
                  if (c.id !== session.id) onSelectConversation(c.id);
                  this.conversationsOpen = false;
                },
              },
              m(
                '.pf-intelletto__conversation-item-body',
                m('.pf-intelletto__conversation-item-title', c.title),
                c.updatedAt !== undefined &&
                  m(
                    '.pf-intelletto__conversation-item-time',
                    formatTimestamp(c.updatedAt),
                  ),
              ),
              c.stored &&
                m(Button, {
                  className: 'pf-intelletto__conversation-delete',
                  icon: 'delete',
                  title: 'Delete conversation',
                  onclick: (e: MouseEvent) => {
                    e.stopPropagation();
                    const ok = window.confirm(
                      `Delete the conversation "${c.title}"? This can't be undone.`,
                    );
                    if (ok) {
                      onDeleteConversation(c.id);
                      if (c.id === session.id) {
                        this.conversationsOpen = false;
                      }
                    }
                  },
                }),
            ),
          ),
        ),
      ),
      m(Button, {
        icon: 'edit',
        title: 'Rename conversation',
        disabled: session.loading || !isStored,
        onclick: () => {
          const name = window.prompt('Rename conversation', session.title);
          if (name !== null) session.rename(name);
        },
      }),
      m(Button, {
        icon: 'add_comment',
        title: 'New conversation',
        onclick: () => {
          this.conversationsOpen = false;
          onNewSession();
        },
      }),
    );
  }

  private renderModelPicker(
    models: readonly ModelDetails[],
    currentModel: ModelPath | undefined,
    onModelChange: (m: ModelPath) => void,
  ): m.Children {
    if (models.length === 0) {
      // No point showing a picker with 0 options.
      return m('span.pf-intelletto__model', 'No models available');
    }

    // If we get here current model should be defined
    assertExists(currentModel);

    const currentDetails = models.find(
      (md) =>
        md.provider.id === currentModel.providerId &&
        md.model.id === currentModel.modelId,
    );
    const label =
      currentDetails !== undefined
        ? shortModelName(currentDetails)
        : currentModel.modelId;
    const tooltip =
      currentDetails !== undefined
        ? modelName(currentDetails)
        : `${currentModel.providerId}/${currentModel.modelId}`;

    // Group models by provider while preserving registration order.
    const groups: Array<{
      readonly providerId: string;
      readonly providerLabel: string;
      readonly models: ModelDetails[];
    }> = [];
    const groupByProvider = new Map<string, (typeof groups)[number]>();
    for (const md of models) {
      let group = groupByProvider.get(md.provider.id);
      if (group === undefined) {
        group = {
          providerId: md.provider.id,
          providerLabel: md.provider.label || md.provider.protocolName,
          models: [],
        };
        groupByProvider.set(md.provider.id, group);
        groups.push(group);
      }
      group.models.push(md);
    }

    return m(
      PopupMenu,
      {
        className: 'pf-popup-menu pf-intelletto__model-menu',
        position: PopupPosition.TopEnd,
        showArrow: false,
        offset: 4,
        trigger: m(Button, {
          className: 'pf-intelletto__model',
          label,
          title: tooltip,
          rightIcon: 'keyboard_arrow_down',
          variant: ButtonVariant.Minimal,
          shrink: true,
        }),
      },
      groups.flatMap((group, idx) => [
        idx > 0 && m(MenuDivider),
        m(MenuTitle, {label: group.providerLabel}),
        ...group.models.map((md) => {
          const isSelected =
            md.provider.id === currentModel.providerId &&
            md.model.id === currentModel.modelId;
          return m(MenuItem, {
            label: shortModelName(md),
            rightIcon: isSelected ? 'check' : undefined,
            active: isSelected,
            onclick: () => {
              onModelChange({
                providerId: md.provider.id,
                modelId: md.model.id,
              });
            },
          });
        }),
      ]),
    );
  }

  private renderQueuedFollowUps(session: ChatSession): m.Children {
    const queued = session.queuedFollowUps;
    if (queued.length === 0) return null;
    return m(
      '.pf-intelletto__queued',
      queued.map((text, idx) =>
        m(
          '.pf-intelletto__queued-item',
          {key: idx},
          m('span.pf-intelletto__queued-label', 'Queued'),
          m('span.pf-intelletto__queued-text', text),
        ),
      ),
    );
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
}

function shortModelName(model: ModelDetails): string {
  return model.model.label || model.model.modelName;
}

function modelName(model: ModelDetails): string {
  const protocolName = model.provider.label || model.provider.protocolName;
  return `${protocolName}/${shortModelName(model)}`;
}

function formatTimestamp(ms: number): string {
  return new Date(ms).toLocaleString(undefined, {
    dateStyle: 'short',
    timeStyle: 'short',
  });
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
