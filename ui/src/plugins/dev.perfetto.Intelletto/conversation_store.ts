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

// Persists Intelletto conversations (transcript, agent history and the model
// in use) to localStorage so they survive page reloads and can be picked back
// up from the conversation dropdown in the chat panel.

import {z} from 'zod';

const STORAGE_KEY = 'perfetto.intelletto.conversations';

const MODEL_PATH_SCHEMA = z.object({
  providerId: z.string(),
  modelId: z.string(),
});

const CHAT_LINE_SCHEMA = z.object({
  role: z.enum(['ai', 'user', 'error', 'thought', 'toolcall', 'spacer']),
  text: z.string(),
  toolArgs: z.unknown().optional(),
  toolResult: z.string().optional(),
  toolError: z.boolean().optional(),
});

const TOOL_CALL_SCHEMA = z.object({
  id: z.string().optional(),
  name: z.string(),
  args: z.record(z.string(), z.unknown()),
  signature: z.string().optional(),
});

const TOOL_RESULT_SCHEMA = z.object({
  id: z.string().optional(),
  name: z.string(),
  result: z.string(),
  isError: z.boolean().optional(),
});

const MESSAGE_SCHEMA = z.discriminatedUnion('role', [
  z.object({role: z.literal('user'), text: z.string()}),
  z.object({role: z.literal('model'), text: z.string()}),
  z.object({
    role: z.literal('tool-call'),
    calls: z.array(TOOL_CALL_SCHEMA).readonly(),
  }),
  z.object({
    role: z.literal('tool-result'),
    results: z.array(TOOL_RESULT_SCHEMA).readonly(),
  }),
]);

const STORED_CONVERSATION_SCHEMA = z.object({
  id: z.string(),
  title: z.string(),
  createdAt: z.number(),
  updatedAt: z.number(),
  // The model this conversation is using, if one has been picked.
  model: MODEL_PATH_SCHEMA.optional(),
  // The transcript, as rendered in the chat panel.
  lines: z.array(CHAT_LINE_SCHEMA),
  // The agent's message history, resent to the model on every request.
  history: z.array(MESSAGE_SCHEMA),
  totalTokens: z.number().optional(),
});

export type StoredConversation = z.infer<typeof STORED_CONVERSATION_SCHEMA>;

const STORED_CONVERSATIONS_SCHEMA = z.array(STORED_CONVERSATION_SCHEMA);

/**
 * In-memory view of the stored conversations, backed by localStorage. Loaded
 * once on construction; every save writes through.
 */
export class ConversationStore {
  // Sorted by updatedAt, most recent first.
  private conversations: StoredConversation[];

  constructor() {
    this.conversations = load();
  }

  /** All stored conversations, most recently updated first. */
  list(): readonly StoredConversation[] {
    return this.conversations;
  }

  /** Look up a stored conversation by id. */
  get(id: string): StoredConversation | undefined {
    return this.conversations.find((c) => c.id === id);
  }

  /** Insert or replace a conversation (matched by id) and persist. */
  save(conversation: StoredConversation): void {
    this.conversations = [
      conversation,
      ...this.conversations.filter((c) => c.id !== conversation.id),
    ].sort((a, b) => b.updatedAt - a.updatedAt);
    persist(this.conversations);
  }
}

function load(): StoredConversation[] {
  const stored = localStorage.getItem(STORAGE_KEY);
  if (!stored) return [];
  try {
    const result = STORED_CONVERSATIONS_SCHEMA.safeParse(JSON.parse(stored));
    if (!result.success) return [];
    return result.data.sort((a, b) => b.updatedAt - a.updatedAt);
  } catch {
    return [];
  }
}

function persist(conversations: readonly StoredConversation[]): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(conversations));
  } catch (e) {
    // Most likely the quota was exceeded. The in-memory copy stays intact, so
    // the current page keeps working; it just won't survive a reload.
    console.warn('Intelletto: failed to persist conversations', e);
  }
}
