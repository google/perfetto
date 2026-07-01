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

// A deliberately minimal chat page for exercising the LLM gateway on its own,
// without the Intelletto assistant (or any tool-calling) on top. Plain
// user<->model turns against the default 'conversational' model, streamed live.
// It's the smallest thing that proves the whole stack - provider selection,
// protocol, streaming, error normalisation - works end to end. Registered only
// when the dev-only 'llmTestChatPage' flag is on (see index.ts).

import './test_chat_page.scss';
import m from 'mithril';
import {Button} from '../../widgets/button';
import {Select} from '../../widgets/select';
import {Spinner} from '../../widgets/spinner';
import {TextInput} from '../../widgets/text_input';
import type {LlmGateway, ModelDetails} from '../dev.perfetto.Llm/gateway';
import type {Message} from '../dev.perfetto.Llm/protocol';
import {assertTrue} from '../../base/assert';
import {maybeUndefined} from '../../base/utils';

// A stable key for a (provider, model) pair, used as the <select> value.
function modelKey(rm: ModelDetails): string {
  return `${rm.provider.id}/${rm.model.id}`;
}

const SYSTEM_PROMPT = 'For testing only';

export interface LlmTestChatPageAttrs {
  readonly gateway: LlmGateway;
}

export class LlmTestChatPage implements m.ClassComponent<LlmTestChatPageAttrs> {
  // The committed conversation history (what we resend on every turn).
  private readonly history: Message[] = [];
  // The model the user picked from the dropdown (a modelKey). Undefined means
  // "follow the gateway's default conversational model".
  private selectedModel?: {providerId: string; modelId: string};
  // Draft in the input box.
  private draft = '';
  // The model turn currently being streamed in, if any (not yet in history).
  private streamingText?: string;
  // Live "thinking"/reasoning for the in-flight turn, where the backend exposes
  // it. Shown dimmed while streaming; not committed to history (NeutralMessage
  // doesn't carry it), so it clears when the turn finishes.
  private streamingThought?: string;
  // A normalised error from the last turn, shown inline.
  private error?: string;
  // Lets the user abort an in-flight turn.
  private abort?: AbortController;

  // The model to chat with: the user's dropdown pick if it's still valid,
  // otherwise the gateway's default conversational model (falling back to the
  // first model of any kind so flash-only setups still work).
  private activeModel(gateway: LlmGateway): ModelDetails | undefined {
    const models = gateway.listModels();
    if (this.selectedModel !== undefined) {
      const sm = this.selectedModel;
      const picked = models.find(
        (rm) => rm.model.id === sm.modelId && rm.provider.id === sm.providerId,
      );
      if (picked !== undefined) return picked;
    }

    // If no model is specifically chosen, just return the first one
    const firstModel = maybeUndefined(models[0]);
    return firstModel;
  }

  view({attrs}: m.CVnode<LlmTestChatPageAttrs>): m.Children {
    const {gateway} = attrs;
    const activeModel = this.activeModel(gateway);
    const busy = this.abort !== undefined;

    return m(
      '.pf-llm-test-chat',
      m(
        '.pf-llm-test-chat__header',
        m('h1', 'LLM gateway test chat'),
        this.renderModelPicker(gateway, activeModel, busy),
      ),
      this.renderTranscript(),
      this.renderComposer(gateway, activeModel !== undefined, busy),
    );
  }

  private renderModelPicker(
    gateway: LlmGateway,
    active: ModelDetails | undefined,
    busy: boolean,
  ): m.Children {
    const models = gateway.listModels();
    if (models.length === 0) {
      return m(
        'span.pf-llm-test-chat__model--none',
        'No models available - configure a provider in settings, or enable ' +
          'Chrome’s on-device Prompt API.',
      );
    }

    // A lookup table of models keyed by some string we can use to dump into the
    // select options keys
    const mapOfModels = new Map(
      models.map((model) => [modelKey(model), model]),
    );

    return m(
      'label.pf-llm-test-chat__model',
      'Model:',
      m(
        Select,
        {
          disabled: busy,
          value: active !== undefined ? modelKey(active) : '',
          onchange: (e: Event) => {
            assertTrue(e.target instanceof HTMLSelectElement);
            const key = e.target.value;
            const modelFromMap = mapOfModels.get(key);
            assertTrue(modelFromMap); // Safe to assume this is true as the map is generated every frame
            this.selectedModel = {
              modelId: modelFromMap.model.id,
              providerId: modelFromMap.provider.id,
            };
          },
        },
        Array.from(mapOfModels).map(([key, rm]) => {
          const protocolName = rm.provider.label ?? rm.provider.protocolName;
          const modelName = rm.model.label || rm.model.modelName;
          const label = `${protocolName}/${modelName}`;
          return m('option', {value: key}, label);
        }),
      ),
    );
  }

  private renderTranscript(): m.Children {
    const rows: m.Children[] = this.history.map((msg) =>
      this.renderMessage(msg),
    );
    if (
      this.streamingThought !== undefined &&
      this.streamingThought.length > 0
    ) {
      rows.push(
        m(
          '.pf-llm-test-chat__thought',
          m('.pf-llm-test-chat__role', 'thinking'),
          m('.pf-llm-test-chat__text', this.streamingThought),
        ),
      );
    }
    if (this.streamingText !== undefined) {
      rows.push(
        this.renderBubble('model', this.streamingText, /* streaming */ true),
      );
    }
    if (this.error !== undefined) {
      rows.push(m('.pf-llm-test-chat__error', `Error: ${this.error}`));
    }
    if (rows.length === 0) {
      rows.push(m('.pf-llm-test-chat__empty', 'Send a message to start.'));
    }
    return m('.pf-llm-test-chat__transcript', rows);
  }

  private renderMessage(msg: Message): m.Children {
    // This page never sends tools, so only user/model turns occur; render any
    // unexpected tool turns as JSON so nothing is silently swallowed.
    if (msg.role === 'user' || msg.role === 'model') {
      return this.renderBubble(msg.role, msg.text, false);
    }
    return this.renderBubble('model', JSON.stringify(msg), false);
  }

  private renderBubble(
    role: 'user' | 'model',
    text: string,
    streaming: boolean,
  ): m.Children {
    return m(
      `.pf-llm-test-chat__msg.pf-llm-test-chat__msg--${role}`,
      m('.pf-llm-test-chat__role', role),
      m('.pf-llm-test-chat__text', text),
      streaming && m(Spinner, {className: 'pf-llm-test-chat__spinner'}),
    );
  }

  private renderComposer(
    gateway: LlmGateway,
    haveModel: boolean,
    busy: boolean,
  ): m.Children {
    return m(
      '.pf-llm-test-chat__composer',
      m(TextInput, {
        className: 'pf-llm-test-chat__input',
        placeholder: 'Type a message and press Enter…',
        value: this.draft,
        disabled: busy || !haveModel,
        autofocus: true,
        onInput: (v: string) => (this.draft = v),
        onkeydown: (e: KeyboardEvent) => {
          if (e.key === 'Enter' && !e.shiftKey) {
            e.preventDefault();
            this.send(gateway);
          }
        },
      }),
      busy
        ? m(Button, {
            label: 'Stop',
            onclick: () => this.abort?.abort(),
          })
        : m(Button, {
            label: 'Send',
            disabled: !haveModel || this.draft.trim().length === 0,
            onclick: () => this.send(gateway),
          }),
      m(Button, {
        label: 'Clear',
        disabled: busy || this.history.length === 0,
        onclick: () => {
          this.history.length = 0;
          this.error = undefined;
        },
      }),
    );
  }

  private async send(gateway: LlmGateway): Promise<void> {
    const text = this.draft.trim();
    if (text.length === 0 || this.abort !== undefined) return;

    const activeModel = this.activeModel(gateway);
    if (!activeModel) return; // Do nothing if there is no model selected

    this.history.push({role: 'user', text});
    this.draft = '';
    this.error = undefined;
    this.streamingText = '';
    this.streamingThought = undefined;
    this.abort = new AbortController();

    try {
      const stream = gateway.createStream(
        {
          providerId: activeModel.provider.id,
          modelId: activeModel.model.id,
        },
        {systemPrompt: SYSTEM_PROMPT, messages: this.history, tools: []},
        this.abort.signal,
      );
      for await (const ev of stream) {
        // This is a test/debug page - dump every stream event so the raw
        // protocol output can be inspected in the devtools console.
        console.log('[llm-test-chat] stream event', ev);
        if (ev.type === 'text') {
          this.streamingText = (this.streamingText ?? '') + ev.text;
        } else if (ev.type === 'thought') {
          this.streamingThought = (this.streamingThought ?? '') + ev.text;
        } else if (ev.type === 'stop' && ev.reason === 'error') {
          this.error = ev.error?.message ?? 'Unknown error';
        }
        m.redraw();
      }
    } finally {
      // Commit whatever text we streamed (even a partial turn on abort/error).
      if (this.streamingText !== undefined && this.streamingText.length > 0) {
        this.history.push({role: 'model', text: this.streamingText});
      }
      this.streamingText = undefined;
      this.streamingThought = undefined;
      this.abort = undefined;
      m.redraw();
    }
  }
}
