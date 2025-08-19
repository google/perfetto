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
import {
  Chat,
  GenerateContentResponse,
  GenerateContentResponseUsageMetadata,
} from '@google/genai';
import {Trace} from '../../public/trace';
import {TextInput} from '../../widgets/text_input';
import markdownit from 'markdown-it';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {Setting} from '../../public/settings';

// Interface for a single message in the chat display

interface ChatMessage {
  role: 'ai' | 'user' | 'error' | 'thought' | 'toolcall' | 'spacer';
  text: string;
}

// Interface for the component's attributes/properties
export interface ChatPageAttrs {
  readonly trace: Trace;
  readonly chat: Chat;
  readonly showThoughts: Setting<boolean>;
  readonly showTokens: Setting<boolean>;
}

export class ChatPage implements m.ClassComponent<ChatPageAttrs> {
  private messages: ChatMessage[];
  private userInput: string;
  private isLoading: boolean;
  private showThoughts: Setting<boolean>;
  private showTokens: Setting<boolean>;
  private readonly chat: Chat;
  private md: markdownit;
  private usage?: GenerateContentResponseUsageMetadata;

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

  async processResponse(response: GenerateContentResponse) {
    if (this.showThoughts.get()) {
      const candidateParts = response.candidates?.[0]?.content?.parts;
      if (candidateParts !== undefined) {
        candidateParts.forEach((part) => {
          if (part.thought) {
            this.messages.push({
              role: 'thought',
              text: part.text ?? 'unprintable',
            });
          } else if (part.functionCall) {
            this.messages.push({
              role: 'toolcall',
              text: part.functionCall?.name ?? 'unprintable',
            });
          }
        });
      }
    }

    if (response.text !== undefined) {
      this.updateAiResponse(response.text);
    }

    if (response.usageMetadata) {
      this.usage = response.usageMetadata;
    }

    m.redraw();
  }

  updateAiResponse(text: string) {
    const lastResponse = this.messages[this.messages.length - 1];
    if (lastResponse.role == 'ai') {
      lastResponse.text += text;
      this.messages[this.messages.length - 1] = lastResponse;
    } else {
      this.messages.push({role: 'ai', text: text});
    }
  }

  sendMessage = async () => {
    const trimmedInput = this.userInput.trim();

    // Prevent sending empty messages or sending while a request is in flight
    if (trimmedInput === '' || this.isLoading) return;

    this.messages.push({role: 'user', text: trimmedInput});
    this.isLoading = true;
    this.userInput = '';
    m.redraw();

    try {
      const responseStream = await this.chat.sendMessageStream({
        message: trimmedInput,
      });

      for await (const part of responseStream) {
        this.processResponse(part);
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
      // Stop loading whether the request succeeded or failed
      this.isLoading = false;
      m.redraw();
    }
  };

  view() {
    // We return a fragment (an array) containing the style and the chat container.
    return m(
      '.pf-ai-chat-panel',
      m(
        '.pf-ai-chat-panel__conversation',
        {
          // This onupdate hook automatically scrolls to the bottom
          // whenever the messages are updated.
          onupdate: (vnode: m.VnodeDOM) => {
            const element = vnode.dom as HTMLElement;
            element.scrollTop = element.scrollHeight;
          },
        },
        // Map through messages and apply a class based on the role for styling
        this.messages.map((msg) => {
          if (!msg.text && msg.role !== 'spacer') return null; // Don't render empty messages
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
