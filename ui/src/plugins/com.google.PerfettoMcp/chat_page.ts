// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';
import { Chat, FunctionCall, GenerateContentResponse } from '@google/genai';
import { Trace } from '../../public/trace';
import { TextInput } from '../../widgets/text_input';

// Interface for a single message in the chat display
interface ChatMessage {
  role: 'ai' | 'user' | 'error' | 'thought' | 'toolcall' | 'spacer';
  text: string;
}

// Interface for the component's attributes/properties
export interface ChatPageAttrs {
  readonly trace: Trace;
  readonly chat: Chat;
}

export class ChatPage implements m.ClassComponent<ChatPageAttrs> {
  // State variables for the component
  private messages: ChatMessage[];
  private userInput: string;
  private isLoading: boolean;
  private useStream: boolean = true;

  // Services passed in through attributes
  private readonly chat: Chat;

  constructor({ attrs }: m.CVnode<ChatPageAttrs>) {
    this.chat = attrs.chat;

    // Initialize state
    this.userInput = '';
    this.isLoading = false;
    this.messages = [{
      role: 'ai',
      text: 'Hello! I am your friendly AI assistant. How can I help you today?'
    }];
  }

  async processResponse(response: GenerateContentResponse) {
    let toolCalls: FunctionCall[] = [];

    const candidateParts = response.candidates?.[0]?.content?.parts
    if (candidateParts !== undefined) {
      candidateParts.forEach(text => {
        if (text.thought) {
          this.messages.push({ role: 'thought', text: text.text ?? 'unprintable' });
        } else if (text.functionCall) {
          toolCalls.push(text.functionCall);
          this.messages.push({ role: 'toolcall', text: text.functionCall?.name ?? 'unprintable' });
        }
      });
    }

    if (response.text !== undefined) {
      this.messages.push({ role: 'ai', text: response.text });
    }
    
    m.redraw(); // Manually trigger a redraw to show the next part
  }

  // Use async/await for cleaner asynchronous logic
  sendMessage = async () => {
    const trimmedInput = this.userInput.trim();

    // Prevent sending empty messages or sending while a request is in flight
    if (trimmedInput === '' || this.isLoading) return;

    // --- State Update 1: Show user's message immediately ---
    this.messages.push({ role: 'user', text: trimmedInput });
    this.isLoading = true;
    this.userInput = ''; // Clear the input field
    m.redraw(); // Manually trigger a redraw to show the user's message and loading state

    try {
      if (this.useStream) {
        const responseStream = await this.chat.sendMessageStream({
          message: trimmedInput
        });

        for await (const part of responseStream) {
          this.processResponse(part);
        }
      } else {
        const response = await this.chat.sendMessage({
          message: trimmedInput
        });

        this.processResponse(response);
      }

      this.messages.push({ role: 'spacer', text: '' });
      m.redraw(); // Manually trigger a redraw to show the next part
    } catch (error) {
      console.error('AI API call failed:', error);
      // --- State Update 3: Show error message in the UI ---
      this.messages.push({ role: 'error', text: 'Sorry, something went wrong. ' + error });
    } finally {
      // --- Final State Update: Always stop loading ---
      // This is crucial to ensure the user can send another message even if an error occurred.
      this.isLoading = false;
      m.redraw(); // Redraw to update the UI with the final state
    }
  }

  view() {
    return m("section.chat-container",
      m(".conversation",
        {
          overflowX: 'hidden',
          overflowY: 'auto',
        },
        // Map through messages and apply a class based on the role for styling
        this.messages.map(msg => {
          let role = "other";
          switch (msg.role) {
            case "ai":
              role = "AI:";
              break;
            case "error":
              role = "Error:";
              break;
            case "toolcall":
              role = "Tool:";
              break;
            case "thought":
              role = "Thought:";
              break;
            case "error":
              role = "Error:";
              break;
            case "spacer":
              role = "";
              break;
          }
          return m(`.message-wrapper.${msg.role}`,
            m('b.role-label', role),
            m('span.message-text', msg.text)
          );
        })
      ),
      m("footer.chat-input-area",
        m(TextInput, {
          value: this.userInput,
          // Use oninput for real-time updates to the userInput state
          oninput: (e: Event) => {
            this.userInput = (e.target as HTMLTextAreaElement).value;
          },
          // Allow sending with the Enter key
          onkeydown: (e: KeyboardEvent) => {
            if (e.key === 'Enter' && !e.shiftKey) {
              e.preventDefault();
              this.sendMessage();
            }
          },
          placeholder: this.isLoading ? 'Waiting for response...' : 'Type your message...',
          disabled: this.isLoading,
        }),
        // Disable the button while loading to prevent multiple submissions
        m("button", { onclick: this.sendMessage, disabled: this.isLoading }, "Send")
      )
    );
  }
}

