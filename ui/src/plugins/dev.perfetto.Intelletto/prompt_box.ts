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

// Prompt composer for the Intelletto chat panel. Owns the uncommitted draft
// text locally and renders the auto-resizing textarea, token readout, model
// picker, and send/cancel button.

import m from 'mithril';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {MenuDivider, MenuItem, MenuTitle, PopupMenu} from '../../widgets/menu';
import {PopupPosition} from '../../widgets/popup';
import type {ModelDetails, ModelPath} from '../dev.perfetto.Llm/gateway';
import './prompt_box.scss';

export interface PromptBoxAttrs {
  readonly isAgentRunning: boolean;
  readonly totalTokens?: number;
  readonly models: readonly ModelDetails[];
  readonly selectedModel: ModelPath | undefined;
  readonly onSelectModel: (model: ModelPath) => void;
  readonly configureModelsHref?: string;
  readonly onSendMessage: (msg: string) => void;
  readonly onCancel: () => void;
}

// Resize a textarea's height to fit its content.
function autoResize(el: HTMLTextAreaElement) {
  el.style.height = 'auto';
  el.style.height = `${el.scrollHeight}px`;
}

export class PromptBox implements m.ClassComponent<PromptBoxAttrs> {
  // Uncommitted draft text lives locally in the prompt box.
  private text = '';

  view({attrs}: m.CVnode<PromptBoxAttrs>): m.Children {
    const hasModels = attrs.models.length > 0;
    const canSend =
      hasModels &&
      attrs.selectedModel !== undefined &&
      this.text.trim().length > 0;
    const sendMessage = () => {
      const trimmed = this.text.trim();
      if (!hasModels || trimmed === '' || attrs.selectedModel === undefined) {
        return;
      }
      this.text = '';
      attrs.onSendMessage(trimmed);
    };

    return m(
      '.pf-prompt-box',
      m('textarea.pf-prompt-box__textarea', {
        rows: 2,
        value: this.text,
        disabled: !hasModels,
        // Grow the textarea to fit its content (capped by max-height in CSS).
        // Done on create/update too so it shrinks back after send.
        oncreate: (vnode: m.VnodeDOM) =>
          autoResize(vnode.dom as HTMLTextAreaElement),
        onupdate: (vnode: m.VnodeDOM) =>
          autoResize(vnode.dom as HTMLTextAreaElement),
        oninput: (e: Event) => {
          const el = e.target as HTMLTextAreaElement;
          this.text = el.value;
          autoResize(el);
        },
        onkeydown: (e: KeyboardEvent) => {
          if (e.key === 'Enter' && !e.shiftKey) {
            e.preventDefault();
            sendMessage();
          }
        },
        placeholder: attrs.isAgentRunning
          ? 'Queue a follow-up...'
          : 'Ask a question...',
      }),
      m(
        '.pf-prompt-box__toolbar',
        attrs.totalTokens !== undefined
          ? this.renderTokens(
              attrs.totalTokens,
              getContextWindow(attrs.selectedModel, attrs.models),
            )
          : null,
        this.renderModelPicker(attrs),
        attrs.isAgentRunning && this.text.trim() === ''
          ? m(Button, {
              className: 'pf-prompt-box__action',
              icon: 'stop',
              title: 'Cancel',
              onclick: () => attrs.onCancel(),
              variant: ButtonVariant.Filled,
              rounded: true,
              intent: Intent.Danger,
            })
          : m(Button, {
              className: 'pf-prompt-box__action',
              icon: 'send',
              title: 'Send',
              disabled: !canSend,
              onclick: sendMessage,
              variant: ButtonVariant.Filled,
              rounded: true,
              intent: Intent.Primary,
            }),
      ),
    );
  }

  private renderTokenSize(totalTokens: number): m.Children {
    if (totalTokens < 1000) {
      return `${totalTokens} tokens`;
    } else if (totalTokens < 1000000) {
      return `${(totalTokens / 1000).toFixed(1)}k`;
    } else {
      return `${(totalTokens / 1000000).toFixed(1)}M`;
    }
  }

  // Render the token readout: a percentage of the model's context window when
  // its size is known, else the raw token count.
  private renderTokens(
    totalTokens: number,
    contextWindow: number | undefined,
  ): m.Children {
    const ktok = `${(totalTokens / 1000).toFixed(1)} ktok`;
    if (contextWindow === undefined || contextWindow <= 0) {
      return m('.pf-prompt-box__tokens', ktok);
    }
    const pct = (totalTokens / contextWindow) * 100;
    return m(
      '.pf-prompt-box__tokens',
      {title: `${ktok} of ${(contextWindow / 1000).toFixed(1)} ktok`},
      `${pct.toFixed(1)}% of `,
      this.renderTokenSize(contextWindow),
    );
  }

  private renderModelPicker(attrs: PromptBoxAttrs): m.Children {
    const {models, selectedModel: currentModel, onSelectModel} = attrs;

    const currentDetails =
      currentModel === undefined
        ? undefined
        : models.find(
            (md) =>
              md.provider.id === currentModel.providerId &&
              md.model.id === currentModel.modelId,
          );
    const label =
      models.length === 0
        ? 'No model selected'
        : currentDetails !== undefined
          ? shortModelName(currentDetails)
          : currentModel !== undefined
            ? currentModel.modelId
            : 'Select model';
    const tooltip =
      models.length === 0
        ? 'No agentic models configured'
        : currentDetails !== undefined
          ? modelName(currentDetails)
          : currentModel !== undefined
            ? `${currentModel.providerId}/${currentModel.modelId}`
            : 'Select a model';

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
        className: 'pf-popup-menu pf-prompt-box__model-menu',
        position: PopupPosition.TopEnd,
        showArrow: false,
        offset: 4,
        trigger: m(Button, {
          className: 'pf-prompt-box__model',
          label,
          title: tooltip,
          rightIcon: 'keyboard_arrow_down',
          variant: ButtonVariant.Minimal,
          shrink: true,
        }),
      },
      groups.length === 0
        ? m(MenuTitle, {label: 'No agentic models configured'})
        : groups.flatMap((group, idx) => [
            idx > 0 && m(MenuDivider),
            m(MenuTitle, {label: group.providerLabel}),
            ...group.models.map((md) => {
              const isSelected =
                currentModel !== undefined &&
                md.provider.id === currentModel.providerId &&
                md.model.id === currentModel.modelId;
              return m(MenuItem, {
                label: shortModelName(md),
                rightIcon: isSelected ? 'check' : undefined,
                active: isSelected,
                onclick: () => {
                  onSelectModel({
                    providerId: md.provider.id,
                    modelId: md.model.id,
                  });
                },
              });
            }),
          ]),
      attrs.configureModelsHref !== undefined && [
        groups.length > 0 && m(MenuDivider),
        m(MenuItem, {
          icon: 'settings',
          label: 'Configure models...',
          href: attrs.configureModelsHref,
        }),
      ],
    );
  }
}

function getContextWindow(
  model: ModelPath | undefined,
  models: readonly ModelDetails[],
): number | undefined {
  if (model === undefined) return undefined;
  return models.find(
    (m) => m.model.id === model.modelId && m.provider.id === model.providerId,
  )?.model.contextWindow;
}

function shortModelName(model: ModelDetails): string {
  return model.model.label || model.model.modelName;
}

function modelName(model: ModelDetails): string {
  const protocolName = model.provider.label || model.provider.protocolName;
  return `${protocolName}/${shortModelName(model)}`;
}
