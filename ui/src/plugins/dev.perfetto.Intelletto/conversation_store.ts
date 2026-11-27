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
// in use) to IndexedDB so they survive page reloads and can be picked back
// up from the conversation dropdown in the chat panel.

import {z} from 'zod';

const DB_NAME = 'perfetto.intelletto';
const DB_VERSION = 1;
const STORE_NAME = 'conversations';

const MODEL_PATH_SCHEMA = z.object({
  providerId: z.string(),
  modelId: z.string(),
});

const CHAT_LINE_SCHEMA = z.object({
  role: z.enum([
    'ai',
    'user',
    'error',
    'thought',
    'toolcall',
    'compaction',
    'spacer',
  ]),
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
  // A user-chosen name, overriding the title derived from the first message.
  customTitle: z.string().optional(),
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

/**
 * In-memory view of the stored conversations, backed by IndexedDB. Hydrated
 * asynchronously on construction; every save and delete updates the in-memory
 * cache immediately and writes through to IndexedDB.
 */
export class ConversationStore {
  // Sorted by updatedAt, most recent first.
  private conversations: StoredConversation[] = [];
  private db: IDBDatabase | undefined;
  private hydrated = false;
  // Track mutations made before initial IndexedDB hydration completes so
  // hydration doesn't clobber newer saves or resurrect deleted conversations.
  private readonly dirtyIds = new Set<string>();
  private readonly deletedIds = new Set<string>();
  // Serializes writes after initial load so rapid save/delete calls land in
  // IndexedDB in the exact order they were issued.
  private writeQueue: Promise<void>;

  constructor(private readonly onLoad?: () => void) {
    this.writeQueue = this.init();
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
    if (!this.hydrated) {
      this.dirtyIds.add(conversation.id);
      this.deletedIds.delete(conversation.id);
    }
    this.enqueueWrite((db) => putRecord(db, conversation));
  }

  /** Remove a conversation (by id) and persist. No-op if it isn't stored. */
  delete(id: string): void {
    this.conversations = this.conversations.filter((c) => c.id !== id);
    if (!this.hydrated) {
      this.deletedIds.add(id);
      this.dirtyIds.delete(id);
    }
    this.enqueueWrite((db) => deleteRecord(db, id));
  }

  private async init(): Promise<void> {
    try {
      const db = await openDb();
      if (db === undefined) {
        this.hydrated = true;
        return;
      }
      this.db = db;
      const rawRecords = await getAllRecords(db);
      const loaded: StoredConversation[] = [];
      for (const raw of rawRecords) {
        const result = STORED_CONVERSATION_SCHEMA.safeParse(raw);
        if (!result.success) continue;
        const conv = result.data;
        if (this.dirtyIds.has(conv.id) || this.deletedIds.has(conv.id)) {
          continue;
        }
        loaded.push(conv);
      }
      this.conversations = [...this.conversations, ...loaded].sort(
        (a, b) => b.updatedAt - a.updatedAt,
      );
      this.hydrated = true;
      this.dirtyIds.clear();
      this.deletedIds.clear();
      if (loaded.length > 0) {
        this.onLoad?.();
      }
    } catch (e) {
      // IndexedDB can fail in private browsing or when storage is disabled.
      // Keep working in memory; conversations just won't survive a reload.
      this.hydrated = true;
      this.dirtyIds.clear();
      this.deletedIds.clear();
      console.warn('Intelletto: failed to load conversations', e);
    }
  }

  private enqueueWrite(op: (db: IDBDatabase) => Promise<void>): void {
    this.writeQueue = this.writeQueue.then(async () => {
      if (this.db === undefined) return;
      try {
        await op(this.db);
      } catch (e) {
        // Most likely the quota was exceeded. The in-memory copy stays intact,
        // so the current page keeps working; it just won't survive a reload.
        console.warn('Intelletto: failed to persist conversation', e);
      }
    });
  }
}

function openDb(): Promise<IDBDatabase | undefined> {
  if (typeof indexedDB === 'undefined') {
    return Promise.resolve(undefined);
  }
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(STORE_NAME)) {
        const store = db.createObjectStore(STORE_NAME, {keyPath: 'id'});
        store.createIndex('updatedAt', 'updatedAt');
      }
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

function getAllRecords(db: IDBDatabase): Promise<unknown[]> {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_NAME, 'readonly');
    const req = tx.objectStore(STORE_NAME).getAll();
    req.onsuccess = () => resolve(req.result as unknown[]);
    req.onerror = () => reject(req.error);
  });
}

function putRecord(
  db: IDBDatabase,
  conversation: StoredConversation,
): Promise<void> {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_NAME, 'readwrite');
    tx.objectStore(STORE_NAME).put(conversation);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error ?? new Error('Transaction aborted'));
  });
}

function deleteRecord(db: IDBDatabase, id: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const tx = db.transaction(STORE_NAME, 'readwrite');
    tx.objectStore(STORE_NAME).delete(id);
    tx.oncomplete = () => resolve();
    tx.onerror = () => reject(tx.error);
    tx.onabort = () => reject(tx.error ?? new Error('Transaction aborted'));
  });
}
