// Copyright (C) 2022 The Android Open Source Project
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

import {AsyncLazy} from '../../../../base/async_lazy';
import {okResult, type Result} from '../../../../base/result';
import {AdbKey} from './adb_key';

const DB_NAME = 'perfetto-adb';
const DB_VERSION = 1;
const STORE_NAME = 'keys';
const KEY_ID = 'webusb-adb-key';

export interface AdbKeyStore {
  load(): Promise<string | undefined>;
  save(serializedKey: string): Promise<void>;
}

const INDEXED_DB_KEY_STORE: AdbKeyStore = {
  load: loadKeyFromIndexedDb,
  save: storeKeyInIndexedDb,
};

export class AdbKeyManager {
  private static shared?: AdbKeyManager;
  private readonly key = new AsyncLazy<AdbKey>();

  constructor(private readonly store: AdbKeyStore = INDEXED_DB_KEY_STORE) {}

  // Returns a process-wide AdbKeyManager so the cached in-memory key survives
  // page unmounts/remounts and is shared across plugins.
  static getShared(): AdbKeyManager {
    return (AdbKeyManager.shared ??= new AdbKeyManager());
  }

  // Finds a key, by priority:
  // 1. In memory (this.key).
  // 2. Persisted in IndexedDB (survives reloads and new tabs on this origin).
  // 3. Generated fresh and persisted to IndexedDB.
  async getOrCreateKey(): Promise<Result<AdbKey>> {
    return this.key.getOrCreate(async () => {
      try {
        const stored = await this.store.load();
        if (stored !== undefined) {
          const parsed = AdbKey.tryDeserialize(stored);
          if (parsed !== undefined) {
            return okResult(parsed);
          }
        }
      } catch (err) {
        console.warn('Failed to load WebUSB ADB key from IndexedDB:', err);
      }

      const newKey = await AdbKey.generateNewKeyPair();
      try {
        await this.store.save(newKey.serialize());
      } catch (err) {
        console.warn('Failed to store WebUSB ADB key in IndexedDB:', err);
      }
      return okResult(newKey);
    });
  }
}

function openKeyDb(): Promise<IDBDatabase> {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open(DB_NAME, DB_VERSION);
    req.onupgradeneeded = () => {
      const db = req.result;
      if (!db.objectStoreNames.contains(STORE_NAME)) {
        db.createObjectStore(STORE_NAME);
      }
    };
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error ?? new Error('indexedDB.open failed'));
  });
}

async function loadKeyFromIndexedDb(): Promise<string | undefined> {
  if (typeof indexedDB === 'undefined') return undefined;
  const db = await openKeyDb();
  try {
    return await new Promise<string | undefined>((resolve, reject) => {
      const tx = db.transaction(STORE_NAME, 'readonly');
      const req = tx.objectStore(STORE_NAME).get(KEY_ID);
      req.onsuccess = () => {
        resolve(typeof req.result === 'string' ? req.result : undefined);
      };
      req.onerror = () =>
        reject(req.error ?? new Error('indexedDB get failed'));
    });
  } finally {
    db.close();
  }
}

async function storeKeyInIndexedDb(serializedKey: string): Promise<void> {
  if (typeof indexedDB === 'undefined') return;
  const db = await openKeyDb();
  try {
    await new Promise<void>((resolve, reject) => {
      const tx = db.transaction(STORE_NAME, 'readwrite');
      tx.objectStore(STORE_NAME).put(serializedKey, KEY_ID);
      tx.oncomplete = () => resolve();
      tx.onerror = () =>
        reject(tx.error ?? new Error('indexedDB transaction failed'));
      tx.onabort = () =>
        reject(tx.error ?? new Error('indexedDB transaction aborted'));
    });
  } finally {
    db.close();
  }
}
