// Copyright (C) 2025 The Android Open Source Project
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
import type {Trace} from '../../public/trace';
import {TextInput} from '../../widgets/text_input';
import markdownit from 'markdown-it';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import type {Setting} from '../../public/settings';
import type {GeminiChat, GeminiUsage} from './gemini_client';

interface ChatMessage {
  role: 'ai' | 'user' | 'error' | 'thought' | 'toolcall' | 'spacer';
  text: string;
}

export interface ChatPageAttrs {
  readonly trace: Trace;
  readonly chat: GeminiChat;
  readonly showThoughts: Setting<boolean>;
  readonly showTokens: Setting<boolean>;
}

export class ChatPage implements m.ClassComponent<ChatPageAttrs> {
  private messages: ChatMessage[];
  private userInput: string;
  private isLoading: boolean;
  private showThoughts: Setting<boolean>;
  private showTokens: Setting<boolean>;
  private readonly chat: GeminiChat;
  private md: markdownit;
  private usage?: GeminiUsage;

  constructor({attrs}: m.CVnode<ChatPageAttrs>) {
    this.chat = attrs.chat;
    this.showThoughts = attrs.showThoughts;
    this.showTokens = attrs.showTokens;
    this.md = markdownit();
    this.userInput = '';
    this.isLoading = false;
    this.messages = [
      {
        role: 'ai',
        text: 'Hello! I am your friendly AI assistant. How can I help you today?',
      },
    ];
  }

  private appendAiText(text: string) {
    const last = this.messages[this.messages.length - 1];
    if (last !== undefined && last.role === 'ai') {
      last.text += text;
    } else {
      this.messages.push({role: 'ai', text});
    }
  }

  sendMessage = async () => {
    const trimmedInput = this.userInput.trim();
    if (trimmedInput === '' || this.isLoading) return;

    this.messages.push({role: 'user', text: trimmedInput});
    this.isLoading = true;
    this.userInput = '';
    m.redraw();

    try {
      for await (const evt of this.chat.sendMessage(trimmedInput)) {
        switch (evt.type) {
          case 'text':
            this.appendAiText(evt.text);
            break;
          case 'thought':
            if (this.showThoughts.get()) {
              this.messages.push({role: 'thought', text: evt.text});
            }
            break;
          case 'toolcall':
            if (this.showThoughts.get()) {
              this.messages.push({role: 'toolcall', text: evt.name});
            }
            // After a tool call, the next AI text belongs to a new turn.
            this.messages.push({role: 'spacer', text: ''});
            break;
          case 'toolresult':
            // No-op for now; could surface errors here.
            break;
          case 'usage':
            this.usage = evt.usage;
            break;
        }
        m.redraw();
      }
      this.messages.push({role: 'spacer', text: ''});
      m.redraw();
    } catch (error) {
      console.error('AI API call failed:', error);
      this.messages.push({
        role: 'error',
        text: 'Sorry, something went wrong. ' + error,
      });
    } finally {
      this.isLoading = false;
      m.redraw();
    }
  };

  view() {
    return m(
      '.pf-ai-chat-panel',
      m(
        '.pf-ai-chat-panel__conversation',
        {
          onupdate: (vnode: m.VnodeDOM) => {
            const element = vnode.dom as HTMLElement;
            element.scrollTop = element.scrollHeight;
          },
        },
        this.messages.map((msg) => {
          if (!msg.text && msg.role !== 'spacer') return null;
          let role = 'other';
          switch (msg.role) {
            case 'ai':
              role = 'AI';
              break;
            case 'user':
              role = 'You';
              break;
            case 'error':
              role = 'Error';
              break;
            case 'toolcall':
              role = 'Tool';
              break;
            case 'thought':
              role = 'Thought';
              break;
            case 'spacer':
              role = '';
              break;
          }
          return m(
            `.pf-ai-chat-message.pf-ai-chat-message--${msg.role}`,
            m('b.pf-ai-chat-message--role-label', role),
            m(
              'span.pf-ai-chat-message--role-text',
              m.trust(this.md.render(msg.text)),
            ),
          );
        }),
      ),
      m(
        'footer.pf-ai-chat-panel__input-area',
        m(TextInput, {
          value: this.userInput,
          oninput: (e: Event) => {
            this.userInput = (e.target as HTMLTextAreaElement).value;
          },
          onkeydown: (e: KeyboardEvent) => {
            if (e.key === 'Enter' && !e.shiftKey) {
              e.preventDefault();
              this.sendMessage();
            }
          },
          placeholder: this.isLoading
            ? 'Waiting for response...'
            : 'Ask me about your trace...',
          disabled: this.isLoading,
        }),

        this.showTokens.get()
          ? [
              m('.pf-ai-chat-panel__tokens', [
                m('div.pf-ai-chat-panel__tokens__label', 'Tokens'),
                m(
                  'div.pf-ai-chat-panel__tokens__count',
                  this.usage?.totalTokenCount ?? '--',
                ),
              ]),
            ]
          : [],

        m(Button, {
          icon: 'arrow_forward',
          title: 'Send',
          onclick: () => this.sendMessage(),
          loading: this.isLoading,
          variant: ButtonVariant.Filled,
          intent: Intent.Primary,
        }),
      ),
    );
  }
}
