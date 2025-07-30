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
import { Chat } from '@google/genai';
import { Trace } from '../../public/trace';
import { TextInput } from '../../widgets/text_input';

// Interface for a single message in the chat display
interface ChatMessage {
  role: 'ai' | 'user' | 'error';
  text: string;
}

// Interface for the component's attributes/properties
export interface ChatPageAttrs {
  readonly trace: Trace;
  readonly chat: Chat,
}

export class ChatPage implements m.ClassComponent<ChatPageAttrs> {
  // State variables for the component
  private messages: ChatMessage[];
  private userInput: string;
  private isLoading: boolean;

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
      const response = await this.chat.sendMessage({
        message: trimmedInput
      });

      const responseText = response.text;

      if (responseText) {
        // --- State Update 2: Show AI's response ---
        this.messages.push({ role: 'ai', text: responseText });
        if (functionCalls !== undefined && functionCalls.length != 0) {
          this.messages.push({ role: 'error', text: 'The response contains function calls which are unsupported.' });
        }
      } else {
        // Handle cases where the response might be empty
        this.messages.push({ role: 'error', text: 'Received an empty text response from the AI.' });
        var functionCalls = response.functionCalls
        if (functionCalls !== undefined && functionCalls.length != 0) {
          this.messages.push({ role: 'error', text: 'The response contains function calls which are unsupported.' });
        }
      }

    } catch (error) {
      console.error('AI API call failed:', error);
      // --- State Update 3: Show error message in the UI ---
      this.messages.push({ role: 'error', text: 'Sorry, something went wrong. Please try again.' });
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
        // Map through messages and apply a class based on the role for styling
        this.messages.map(msg => {
          return m(`.message-wrapper.${msg.role}`,
            m('b.role-label', msg.role === 'ai' ? 'AI:' : msg.role === 'error' ? 'Error:' : 'You:'),
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
