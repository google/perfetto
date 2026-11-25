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

import './agent_sidebar.scss';
import m from 'mithril';
import {classNames} from '../../../../ui/src/base/classnames';
import type {App} from '../../../../ui/src/public/app';
import {Button, ButtonVariant} from '../../../../ui/src/widgets/button';
import {Intent} from '../../../../ui/src/widgets/common';
import {Icon} from '../../../../ui/src/widgets/icon';
import {Spinner} from '../../../../ui/src/widgets/spinner';
import {
  ProductAgentSession,
  resolveActiveModel,
  type TranscriptItem,
} from './agent_loop';
import type {EvidenceRecord, ProductToolSpec} from './types';

function renderMarkdownInline(
  text: string,
  onEvidenceClick: (id: string) => void,
): m.Children {
  // Highlight [E1] evidence tags and `code` spans and **bold**.
  const parts = text.split(/(\[E\d+\]|`[^`]+`|\*\*[^*]+\*\*)/g);
  return parts.map((part) => {
    if (/^\[E\d+\]$/.test(part)) {
      const id = part.slice(1, -1);
      return m(
        'button.pf-agent__ev-chip',
        {onclick: () => onEvidenceClick(id), title: `Inspect ${id}`},
        id,
      );
    }
    if (part.startsWith('`') && part.endsWith('`') && part.length >= 2) {
      return m('code.pf-agent__code', part.slice(1, -1));
    }
    if (part.startsWith('**') && part.endsWith('**') && part.length >= 4) {
      return m('strong', part.slice(2, -2));
    }
    return part;
  });
}

function renderMarkdownBlocks(
  text: string,
  onEvidenceClick: (id: string) => void,
): m.Children {
  const lines = text.split('\n');
  const out: m.Children[] = [];
  let listItems: string[] = [];

  const flushList = () => {
    if (listItems.length === 0) return;
    out.push(
      m(
        'ul.pf-agent__list',
        listItems.map((li) => m('li', renderMarkdownInline(li, onEvidenceClick))),
      ),
    );
    listItems = [];
  };

  for (const raw of lines) {
    const trimmed = raw.trim();
    if (trimmed === '') {
      flushList();
      continue;
    }
    if (/^[-*•]\s+/.test(trimmed)) {
      listItems.push(trimmed.replace(/^[-*•]\s+/, ''));
      continue;
    }
    flushList();
    if (trimmed.startsWith('### ')) {
      out.push(
        m('h4.pf-agent__h', renderMarkdownInline(trimmed.slice(4), onEvidenceClick)),
      );
    } else if (trimmed.startsWith('## ')) {
      out.push(
        m('h3.pf-agent__h', renderMarkdownInline(trimmed.slice(3), onEvidenceClick)),
      );
    } else {
      out.push(
        m('p.pf-agent__p', renderMarkdownInline(trimmed, onEvidenceClick)),
      );
    }
  }
  flushList();
  return out;
}

export interface AgentSidebarAttrs {
  readonly app: App;
  readonly session: ProductAgentSession;
}

export function AgentSidebar(): m.Component<AgentSidebarAttrs> {
  const expandedTools = new Set<number>();
  let inspectedEvidence: EvidenceRecord | undefined;
  let toolRunOutput: {name: string; text: string; isError?: boolean} | undefined;
  let runningToolName: string | undefined;

  return {
    view({attrs: {app, session}}) {
      const ctx = session.adapter.getContext();
      const models = session.gateway.listModels();
      const activeModel = resolveActiveModel(
        session.selectedModel,
        session.gateway,
      );
      const specs = session.runner.listSpecs(ctx);
      const suggestions = session.adapter.getSuggestions(ctx);

      const onEvidenceClick = (id: string) => {
        const rec = session.evidence.get(id);
        if (rec) {
          inspectedEvidence =
            inspectedEvidence?.id === id ? undefined : rec;
          m.redraw();
        }
      };

      const runSpecDirectly = async (spec: ProductToolSpec) => {
        runningToolName = spec.name;
        toolRunOutput = undefined;
        m.redraw();
        const res = await session.runner.execute(
          spec.name,
          {},
          ctx,
          session.evidence,
          session.notes,
        );
        runningToolName = undefined;
        toolRunOutput = {
          name: spec.name,
          text: JSON.stringify(res.output, null, 2),
          isError: res.isError,
        };
        m.redraw();
      };

      return m('.pf-agent', [
        // Top strip: context pill + Chat/Tools switch + reset
        m('.pf-agent__top', [
          m('.pf-agent__ctx', [
            m('span.pf-agent__mode-dot', {
              className: classNames(
                ctx.mode === 'live' && 'pf-agent__mode-dot--live',
                ctx.mode === 'none' && 'pf-agent__mode-dot--idle',
              ),
            }),
            m('span.pf-agent__ctx-label', ctx.label),
            ctx.subtab && m('span.pf-agent__ctx-sub', ctx.subtab),
          ]),
          m('.pf-agent__top-actions', [
            m(
              '.pf-agent__seg',
              [
                m(
                  'button.pf-agent__seg-btn',
                  {
                    className: classNames(
                      session.activeTab === 'chat' && 'pf-agent__seg-btn--active',
                    ),
                    onclick: () => {
                      session.activeTab = 'chat';
                    },
                  },
                  'Chat',
                ),
                m(
                  'button.pf-agent__seg-btn',
                  {
                    className: classNames(
                      session.activeTab === 'tools' && 'pf-agent__seg-btn--active',
                    ),
                    onclick: () => {
                      session.activeTab = 'tools';
                    },
                  },
                  `Tools (${specs.length})`,
                ),
              ],
            ),
            m(Button, {
              icon: 'refresh',
              title: 'New conversation',
              variant: ButtonVariant.Minimal,
              compact: true,
              onclick: () => {
                inspectedEvidence = undefined;
                session.reset();
              },
            }),
          ]),
        ]),

        // Model selector strip
        m('.pf-agent__model-bar', [
          models.length > 0
            ? m(
                'select.pf-agent__model-select',
                {
                  value: activeModel
                    ? `${activeModel.providerId}::${activeModel.modelId}`
                    : '',
                  onchange: (e: Event) => {
                    const val = (e.target as HTMLSelectElement).value;
                    const [providerId, modelId] = val.split('::');
                    if (providerId && modelId) {
                      session.selectedModel = {providerId, modelId};
                    }
                  },
                },
                models.map((md) =>
                  m(
                    'option',
                    {value: `${md.provider.id}::${md.model.id}`},
                    `${md.model.label ?? md.model.modelName} (${md.provider.label ?? md.provider.protocolName})`,
                  ),
                ),
              )
            : m('.pf-agent__no-model', [
                m('span', 'No LLM configured'),
                m(Button, {
                  label: 'Settings',
                  icon: 'settings',
                  variant: ButtonVariant.Minimal,
                  compact: true,
                  onclick: () => app.navigate('#!/settings'),
                }),
              ]),
          session.totalTokens !== undefined &&
            m(
              'span.pf-agent__tokens',
              `${(session.totalTokens / 1000).toFixed(1)}k tok`,
            ),
        ]),

        // Evidence inspector popover banner when clicking an [E1] chip
        inspectedEvidence &&
          m('.pf-agent__ev-drawer', [
            m('.pf-agent__ev-drawer-head', [
              m('strong', `${inspectedEvidence.id} · ${inspectedEvidence.tool}`),
              m(Button, {
                icon: 'close',
                variant: ButtonVariant.Minimal,
                compact: true,
                onclick: () => {
                  inspectedEvidence = undefined;
                },
              }),
            ]),
            inspectedEvidence.sql &&
              m('pre.pf-agent__ev-sql', inspectedEvidence.sql),
            m(
              'pre.pf-agent__ev-json',
              JSON.stringify(inspectedEvidence.data, null, 2).slice(0, 3000),
            ),
          ]),

        // Body: either Chat or Declarative Tools Inspector
        session.activeTab === 'tools'
          ? m('.pf-agent__tools-panel', [
              m(
                '.pf-agent__tools-intro',
                'Available analysis tools for this view. Run any tool directly to inspect its output.',
              ),
              toolRunOutput &&
                m('.pf-agent__tool-preview', [
                  m('.pf-agent__ev-drawer-head', [
                    m('strong', `Result: ${toolRunOutput.name}`),
                    m(Button, {
                      icon: 'close',
                      variant: ButtonVariant.Minimal,
                      compact: true,
                      onclick: () => {
                        toolRunOutput = undefined;
                      },
                    }),
                  ]),
                  m('pre.pf-agent__ev-json', toolRunOutput.text.slice(0, 4000)),
                ]),
              specs.map((spec) =>
                m('.pf-agent__tool-card', [
                  m('.pf-agent__tool-card-head', [
                    m('code.pf-agent__tool-name', spec.name),
                    m('span.pf-agent__tool-kind', spec.kind),
                    (spec.kind === 'sql' ||
                      spec.kind === 'sql_multi' ||
                      spec.name === 'device_status') &&
                      m(Button, {
                        label: runningToolName === spec.name ? 'Running…' : 'Run',
                        icon: 'play_arrow',
                        variant: ButtonVariant.Outlined,
                        compact: true,
                        disabled: runningToolName !== undefined || (!ctx.engine && spec.kind !== 'device'),
                        onclick: () => void runSpecDirectly(spec),
                      }),
                  ]),
                  m('.pf-agent__tool-desc', spec.description),
                ]),
              ),
            ])
          : m('.pf-agent__chat', [
              m('.pf-agent__messages', [
                session.items.length === 0
                  ? m('.pf-agent__empty', [
                      m('.pf-agent__empty-title', [
                        m(Icon, {icon: 'auto_awesome'}),
                        ` ${session.adapter.productName} Assistant`,
                      ]),
                      m(
                        '.pf-agent__empty-sub',
                        ctx.mode === 'none'
                          ? 'Connect a device or open a trace, then ask about bottlenecks, memory, CPU, or storage.'
                          : `Ask about ${ctx.label}.`,
                      ),
                      m(
                        '.pf-agent__suggestions',
                        suggestions.map((s) =>
                          m(
                            'button.pf-agent__suggestion',
                            {
                              disabled: session.loading,
                              onclick: () => void session.send(s.prompt),
                            },
                            [
                              s.icon && m(Icon, {icon: s.icon}),
                              m('span', s.label),
                            ],
                          ),
                        ),
                      ),
                    ])
                  : session.items.map((item, idx) =>
                      renderTranscriptItem(
                        item,
                        idx,
                        expandedTools,
                        onEvidenceClick,
                      ),
                    ),
                session.pendingApproval &&
                  m('.pf-agent__approval', [
                    m('.pf-agent__approval-title', session.pendingApproval.title),
                    m('code.pf-agent__approval-cmd', session.pendingApproval.command),
                    m('.pf-agent__approval-btns', [
                      m(Button, {
                        label: 'Approve & run',
                        intent: Intent.Primary,
                        variant: ButtonVariant.Filled,
                        compact: true,
                        onclick: () => session.pendingApproval?.resolve(true),
                      }),
                      m(Button, {
                        label: 'Cancel',
                        variant: ButtonVariant.Outlined,
                        compact: true,
                        onclick: () => session.pendingApproval?.resolve(false),
                      }),
                    ]),
                  ]),
                session.loading &&
                  m('.pf-agent__thinking', [
                    m(Spinner, {easing: true}),
                    m('span', 'Querying & analyzing…'),
                  ]),
              ]),

              // Composer
              m('.pf-agent__composer', [
                m('textarea.pf-agent__input', {
                  rows: 2,
                  placeholder:
                    ctx.mode === 'live'
                      ? 'Ask what is happening, or run a workload & snapshot…'
                      : `Ask about ${ctx.label}…`,
                  value: session.input,
                  oninput: (e: InputEvent) => {
                    session.input = (e.target as HTMLTextAreaElement).value;
                  },
                  onkeydown: (e: KeyboardEvent) => {
                    if (e.key === 'Enter' && !e.shiftKey) {
                      e.preventDefault();
                      void session.send();
                    }
                  },
                }),
                m('.pf-agent__composer-actions', [
                  session.loading
                    ? m(Button, {
                        label: 'Stop',
                        icon: 'stop',
                        intent: Intent.Danger,
                        variant: ButtonVariant.Outlined,
                        compact: true,
                        onclick: () => session.cancel(),
                      })
                    : m(Button, {
                        label: 'Send',
                        icon: 'send',
                        intent: Intent.Primary,
                        variant: ButtonVariant.Filled,
                        compact: true,
                        disabled: session.input.trim() === '',
                        onclick: () => void session.send(),
                      }),
                ]),
              ]),
            ]),
      ]);
    },
  };
}

function renderTranscriptItem(
  item: TranscriptItem,
  idx: number,
  expandedTools: Set<number>,
  onEvidenceClick: (id: string) => void,
): m.Child {
  switch (item.kind) {
    case 'user':
      return m('.pf-agent__msg.pf-agent__msg--user', item.text);
    case 'ai':
      return m(
        '.pf-agent__msg.pf-agent__msg--ai',
        renderMarkdownBlocks(item.text, onEvidenceClick),
      );
    case 'thought':
      return m('details.pf-agent__thought', [
        m('summary', 'Reasoning'),
        m('div', item.text),
      ]);
    case 'error':
      return m('.pf-agent__msg.pf-agent__msg--error', [
        m(Icon, {icon: 'error'}),
        ' ',
        item.text,
      ]);
    case 'handoff':
      return m('.pf-agent__handoff', [
        m('.pf-agent__handoff-title', item.action.label),
        m('.pf-agent__handoff-desc', item.action.description),
        m(Button, {
          label: `Open in ${item.action.product}`,
          rightIcon: 'open_in_new',
          intent: Intent.Primary,
          variant: ButtonVariant.Filled,
          compact: true,
          onclick: () => {
            if (item.action.onOpen) {
              item.action.onOpen();
            } else {
              window.open(item.action.url, '_blank', 'noopener');
            }
          },
        }),
      ]);
    case 'tool': {
      const open = expandedTools.has(idx);
      return m(
        '.pf-agent__tool-pill',
        {
          className: classNames(
            item.isError && 'pf-agent__tool-pill--err',
            open && 'pf-agent__tool-pill--open',
          ),
        },
        [
          m(
            '.pf-agent__tool-pill-head',
            {
              onclick: () => {
                if (open) expandedTools.delete(idx);
                else expandedTools.add(idx);
              },
            },
            [
              m(Icon, {
                icon: !item.done
                  ? 'hourglass_top'
                  : item.isError
                    ? 'error_outline'
                    : 'check_circle',
              }),
              m('code', item.name),
              item.evidenceId &&
                m(
                  'button.pf-agent__ev-chip',
                  {
                    onclick: (e: MouseEvent) => {
                      e.stopPropagation();
                      onEvidenceClick(item.evidenceId!);
                    },
                  },
                  item.evidenceId,
                ),
              m(Icon, {icon: open ? 'expand_less' : 'expand_more'}),
            ],
          ),
          open &&
            m('.pf-agent__tool-pill-body', [
              Object.keys(item.args).length > 0 &&
                m('pre', `args: ${JSON.stringify(item.args, null, 2)}`),
              item.result && m('pre', item.result.slice(0, 2500)),
            ]),
        ],
      );
    }
  }
}
