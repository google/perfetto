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

// Provider-agnostic tool definition.
export interface ToolDef {
  readonly name: string;
  readonly description: string;
  readonly input_schema: Record<string, unknown>;
}

// A tool implementation that handles invocations.
export interface ToolImpl {
  readonly def: ToolDef;
  handle(input: Record<string, unknown>): Promise<string>;
}

// Callback for streaming text updates.
export type OnTextUpdate = (text: string) => void;

// Callback for tool use notifications.
export type OnToolUse = (name: string, input: string) => void;

// Result of a sendMessage call.
export interface SendMessageResult {
  readonly text: string;
  readonly hitToolLimit: boolean;
}

// Provider-agnostic interface for sending messages to an LLM.
// Each provider maintains its own internal conversation history format.
export interface LlmProvider {
  // Send a user message and get the assistant's response.
  // Handles tool call loops internally.
  sendMessage(opts: {
    userPrompt: string;
    tools: readonly ToolImpl[];
    onText: OnTextUpdate;
    onToolUse?: OnToolUse;
    signal?: AbortSignal;
  }): Promise<SendMessageResult>;

  // Continue the conversation after hitting the tool limit.
  // Sends a "please continue" message to resume tool execution.
  continueToolUse(opts: {
    tools: readonly ToolImpl[];
    onText: OnTextUpdate;
    onToolUse?: OnToolUse;
    signal?: AbortSignal;
  }): Promise<SendMessageResult>;

  // Reset conversation history.
  reset(): void;
}
