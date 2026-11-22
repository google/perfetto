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

import {afterEach, describe, expect, test, vi} from 'vitest';

// jsbn-rsa calls the Netscape-4-era window.crypto.random() under the test
// runner's navigator/window shim; mock it since this test does not run RSA math.
vi.mock('jsbn-rsa', () => ({
  BigInteger: {ONE: {shiftLeft: () => ({})}},
  RSAKey: class {},
}));

import {AdbKey} from './adb_key';
import {AdbKeyManager, type AdbKeyStore} from './adb_key_manager';

function makeMemoryStore(initial?: string): AdbKeyStore & {
  saved?: string;
  loadCalls: number;
  saveCalls: number;
} {
  const store = {
    saved: initial,
    loadCalls: 0,
    saveCalls: 0,
    async load() {
      store.loadCalls++;
      return store.saved;
    },
    async save(serializedKey: string) {
      store.saveCalls++;
      store.saved = serializedKey;
    },
  };
  return store;
}

describe('AdbKeyManager', () => {
  afterEach(() => {
    vi.restoreAllMocks();
  });

  test('persists a generated key and reloads it across manager instances', async () => {
    const store = makeMemoryStore();
    const mgr1 = new AdbKeyManager(store);

    const first = await mgr1.getOrCreateKey();
    expect(first.ok).toBe(true);
    if (!first.ok) return;
    expect(store.loadCalls).toBe(1);
    expect(store.saveCalls).toBe(1);
    expect(store.saved).toBe(first.value.serialize());

    // Subsequent call on the same instance uses the in-memory cache.
    const cached = await mgr1.getOrCreateKey();
    expect(cached.ok).toBe(true);
    if (!cached.ok) return;
    expect(cached.value).toBe(first.value);
    expect(store.loadCalls).toBe(1);

    // New instance (simulating page reload or new tab) loads from the store.
    const mgr2 = new AdbKeyManager(store);
    const reloaded = await mgr2.getOrCreateKey();
    expect(reloaded.ok).toBe(true);
    if (!reloaded.ok) return;
    expect(reloaded.value.serialize()).toBe(first.value.serialize());
    expect(store.loadCalls).toBe(2);
    expect(store.saveCalls).toBe(1);
  });

  test('regenerates and overwrites corrupt stored key data', async () => {
    const store = makeMemoryStore('not-valid-json');
    const mgr = new AdbKeyManager(store);

    const res = await mgr.getOrCreateKey();
    expect(res.ok).toBe(true);
    if (!res.ok) return;
    expect(store.saveCalls).toBe(1);
    expect(AdbKey.tryDeserialize(store.saved ?? '')).toBeDefined();
  });

  test('falls back to in-memory key when storage throws', async () => {
    const warnSpy = vi.spyOn(console, 'warn').mockImplementation(() => {});
    const brokenStore: AdbKeyStore = {
      load: async () => {
        throw new Error('IDB blocked');
      },
      save: async () => {
        throw new Error('IDB blocked');
      },
    };
    const mgr = new AdbKeyManager(brokenStore);

    const first = await mgr.getOrCreateKey();
    const second = await mgr.getOrCreateKey();
    expect(first.ok).toBe(true);
    expect(second.ok).toBe(true);
    if (first.ok && second.ok) {
      expect(second.value).toBe(first.value);
    }
    expect(warnSpy).toHaveBeenCalledTimes(2);
  });

  test('AdbKey.tryDeserialize rejects partial or malformed payloads', () => {
    expect(AdbKey.tryDeserialize('')).toBeUndefined();
    expect(AdbKey.tryDeserialize('{')).toBeUndefined();
    expect(AdbKey.tryDeserialize('{"n":"abc"}')).toBeUndefined();
    expect(AdbKey.tryDeserialize('null')).toBeUndefined();
  });
});
