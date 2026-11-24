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

import {cacheTrace, tryGetTrace} from './cache_manager';
import type {TraceArrayBufferSource} from './trace_source';

// Minimal in-memory stand-in for the browser's Cache storage, keyed by path.
class FakeCache {
  private readonly entries = new Map<string, Response>();

  async put(key: Request | string, value: Response) {
    // Buffer the body so that the entry can be read more than once.
    const body = await value.arrayBuffer();
    this.entries.set(pathOf(key), new Response(body, {headers: value.headers}));
  }

  async match(key: Request | string) {
    return this.entries.get(pathOf(key))?.clone();
  }

  async keys() {
    return [...this.entries.keys()].map((k) => new Request(`http://x${k}`));
  }

  async delete(key: Request | string) {
    return this.entries.delete(pathOf(key));
  }
}

function pathOf(key: Request | string): string {
  return typeof key === 'string' ? key : new URL(key.url).pathname;
}

beforeAll(() => {
  const cache = new FakeCache();
  vi.stubGlobal('caches', {open: async () => cache});
});

afterAll(() => {
  vi.unstubAllGlobals();
});

function bufferSource(
  extra: Partial<TraceArrayBufferSource> = {},
): TraceArrayBufferSource {
  return {
    type: 'ARRAY_BUFFER',
    buffer: new Uint8Array([1, 2, 3]).buffer,
    title: 'trace',
    ...extra,
  };
}

test('caches buffer traces by default', async () => {
  expect(await cacheTrace(bufferSource(), 'uuid-default')).toBe(true);

  const cached = await tryGetTrace('uuid-default');
  expect(cached?.title).toBe('trace');
  expect(cached?.buffer.byteLength).toBe(3);
});

test('does not cache buffer traces with cacheable=false', async () => {
  const src = bufferSource({cacheable: false});
  expect(await cacheTrace(src, 'uuid-uncacheable')).toBe(false);
  expect(await tryGetTrace('uuid-uncacheable')).toBeUndefined();
});
